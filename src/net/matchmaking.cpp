#include "takedown/net/matchmaking.hpp"

#include "takedown/content/mod_identity.hpp"
#include "takedown/net/browser_auth.hpp"
#include "takedown/net/release_credential.hpp"
#include "takedown/net/socket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace takedown::net {
namespace {

constexpr std::string_view kClientTag = "v0.0.24";

// JSON string literal for the line protocol: only quotes, backslashes and
// control characters need escaping, everything else is passed through.
// RECOVERY: not identical to the original GIMPLE; escape appends are emitted in a different order than the original, and the "\"" literal is not kept in rodata.
std::string quote(std::string_view text) {
    const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c <= 31U) {
            out += "\\u00";
            out += hex[c >> 4U];
            out += hex[c & 15U];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

std::string text_member(const content::JsonValue& object, std::string_view name) {
    const auto* member = object.find(name);
    if (member == nullptr || !member->is_string()) {
        throw std::runtime_error("invalid matchmaking field: " + std::string(name));
    }
    return member->as_string();
}

// RECOVERY: not identical to the original GIMPLE; original has one throw site reached from a combined check; this version nests the checks.
std::uint32_t number_member(const content::JsonValue& object, std::string_view name) {
    const auto* member = object.find(name);
    if (member != nullptr && member->is_number()) {
        const double value = member->as_number();
        if (std::isfinite(value) && value >= 0.0 && value <= 4294967295.0 && std::floor(value) == value) {
            return static_cast<std::uint32_t>(value);
        }
    }
    throw std::runtime_error("invalid matchmaking field: " + std::string(name));
}

// The applied mods as a JSON array for the hello message. The server checks
// every entry again; the revision comes from the launcher's catalog receipt
// and the content digest is always recomputed from the files on disk.
// RECOVERY: not identical to the original GIMPLE; hunks remain in the catalog revision lookup and the per-mod JSON build.
std::string mod_manifest(const content::ModManager& manager, std::stop_token stop) {
    std::optional<content::JsonValue> catalog;
    {
        std::ifstream metadata(manager.directory() / ".catalog.json", std::ios::binary);
        if (metadata) {
            std::string bytes(1048577U, '\0');
            metadata.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(metadata.gcount()));
            content::JsonError error;
            if (bytes.size() <= 1048576U) {
                catalog = content::parse_json(bytes, error);
            }
        }
    }
    std::string out = "[";
    bool first = true;
    for (const std::size_t index : manager.application_order()) {
        const auto& mod = manager.mods().at(index);
        std::string revision;
        for (const char* const field : {"revisions", "mods"}) {
            const auto* entries = catalog ? catalog->find(field) : nullptr;
            if (entries == nullptr || !entries->is_array()) {
                continue;
            }
            for (const auto& entry : entries->as_array()) {
                const auto* folder = entry.find("folder");
                const auto* id = entry.find("id");
                const auto* recorded = entry.find("revision");
                if (folder != nullptr && id != nullptr && recorded != nullptr && folder->is_string() &&
                    id->is_string() && recorded->is_string() && folder->as_string() == mod.folder &&
                    id->as_string() == mod.manifest.id) {
                    revision = recorded->as_string();
                }
            }
        }
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"id\":" + quote(mod.manifest.id) + ",\"name\":" + quote(mod.display_name) +
               ",\"folder\":" + quote(mod.folder) + ",\"revision\":" + quote(revision) +
               ",\"enabled\":true,\"content_sha256\":" +
               quote(content::mod_content_identity(manager.directory() / mod.folder, stop)) + "}";
    }
    out += ']';
    return out;
}

// RECOVERY: not identical to the original GIMPLE; string operator+ forms in the list, create and update cases differ from the original.
std::string encode_command(const MatchRequest& request) {
    const std::string mode = quote(online_mode_name(request.mode));
    switch (request.kind) {
    case MatchRequest::Kind::List: {
        const char* original = request.mode == OnlineMode::Original ? ",\"mode\":\"original\"" : "";
        return "{\"type\":\"list\",\"after\":" + std::to_string(request.after) + original + "}";
    }
    case MatchRequest::Kind::Create: {
        const std::string original =
            request.mode == OnlineMode::Original
                ? ",\"capacity\":" + std::to_string(request.capacity) +
                      ",\"parameters\":" + quote(request.parameters) +
                      ",\"system_flags\":" + std::to_string(request.system_flags) +
                      ",\"password\":" + quote(request.password)
                : std::string();
        return "{\"type\":\"create\",\"mode\":" + mode + ",\"name\":" + quote(request.name) +
               ",\"track\":" + quote(request.track) + original + "}";
    }
    case MatchRequest::Kind::Join: {
        const std::string user = request.user_parameters
                                     ? ",\"user_parameters\":" + quote(*request.user_parameters)
                                     : std::string();
        return "{\"type\":\"join\",\"room\":" + std::to_string(request.room) +
               ",\"password\":" + quote(request.password) + user + "}";
    }
    case MatchRequest::Kind::QuickMatch:
        return "{\"type\":\"quick_match\",\"mode\":" + mode + "}";
    case MatchRequest::Kind::Leave:
        return "{\"type\":\"leave\"}";
    case MatchRequest::Kind::Ready:
        return std::string("{\"type\":\"ready\",\"ready\":") + (request.ready ? "true}" : "false}");
    case MatchRequest::Kind::Start:
        return "{\"type\":\"start\"}";
    case MatchRequest::Kind::Finish:
        return "{\"type\":\"finish\"}";
    case MatchRequest::Kind::Chat:
        return "{\"type\":\"chat\",\"text\":" + quote(request.message) +
               ",\"target\":" + quote(request.target) +
               ",\"attributes\":" + quote(request.attributes) + "}";
    case MatchRequest::Kind::Service: {
        std::string out = "{\"type\":\"service\",\"operation\":" + quote(request.operation) + ",\"fields\":{";
        bool first = true;
        for (const auto& [key, value] : request.fields) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out += quote(key) + ':' + quote(value);
        }
        return out + "}}";
    }
    case MatchRequest::Kind::Update: {
        if (request.operation == "kick") {
            return "{\"type\":\"kick\",\"member\":" + std::to_string(request.room) + "}";
        }
        const std::string parameters =
            request.parameters.empty() ? std::string() : ",\"parameters\":" + quote(request.parameters);
        const std::string flags =
            request.updated_system_flags
                ? ",\"system_flags\":" + std::to_string(*request.updated_system_flags)
                : std::string();
        const std::string user = request.user_parameters
                                     ? ",\"user_parameters\":" + quote(*request.user_parameters)
                                     : std::string();
        return "{\"type\":\"update\"" + user + flags + parameters + "}";
    }
    }
    throw std::invalid_argument("invalid matchmaking request");
}

} // namespace

const char* online_mode_name(OnlineMode mode) noexcept {
    switch (mode) {
    case OnlineMode::Race:
        return "race";
    case OnlineMode::RoadRage:
        return "road_rage";
    case OnlineMode::TeamCrush:
        return "team_crush";
    case OnlineMode::DoubleImpact:
        return "double_impact";
    case OnlineMode::PartyCrash:
        return "party_crash";
    case OnlineMode::Original:
        return "original";
    }
    return "unsupported";
}

unsigned online_mode_capacity(OnlineMode mode) noexcept {
    switch (mode) {
    case OnlineMode::Race:
    case OnlineMode::RoadRage:
        return 6U;
    case OnlineMode::TeamCrush:
    case OnlineMode::DoubleImpact:
        return 2U;
    case OnlineMode::PartyCrash:
    case OnlineMode::Original:
        return 8U;
    }
    return 0U;
}

struct MatchmakingClient::Impl {
    std::jthread worker;
    std::atomic<bool> connected{};
    std::mutex mutex;
    std::deque<std::string> outgoing;
    std::vector<MatchEvent> events;

    void event(MatchEvent::Kind kind, content::JsonValue payload, std::string detail);
    void run(std::stop_token stop, MatchmakingConfig config);
};

// RECOVERY: not identical to the original GIMPLE; original compares the raw queue byte span and copies the payload with an outlined variant visitor.
void MatchmakingClient::Impl::event(MatchEvent::Kind kind, content::JsonValue payload,
                                    std::string detail) {
    const std::lock_guard lock(mutex);
    if (kind == MatchEvent::Kind::Heartbeat) {
        // Keepalive replies replace each other instead of filling the queue.
        const auto existing = std::find_if(events.begin(), events.end(), [](const MatchEvent& event) {
            return event.kind == MatchEvent::Kind::Heartbeat;
        });
        if (existing != events.end()) {
            existing->payload = std::move(payload);
            return;
        }
    }
    if (events.size() > 127U) {
        throw std::runtime_error("matchmaking event queue overflow");
    }
    events.push_back(MatchEvent{kind, payload, std::move(detail)});
}

// RECOVERY: not identical to the original GIMPLE; worker loop not reconstructed to the original control flow (48 hunks).
void MatchmakingClient::Impl::run(std::stop_token stop, MatchmakingConfig config) {
    std::optional<GameLogin> login;
    try {
        if (config.browser_signin) {
            if (stop.stop_requested()) {
                return;
            }
            {
                const auto remembered = load_game_session(config.session_cache);
                if (remembered) {
                    login = resume_game_session(*remembered);
                    if (!login) {
                        forget_game_session(config.session_cache);
                    }
                }
            }
            if (!login) {
                const BrowserSignIn request = begin_browser_signin();
                event(MatchEvent::Kind::BrowserSignIn,
                      content::JsonValue(content::JsonValue::Object{
                          {"url", content::JsonValue(request.url)},
                          {"code", content::JsonValue(request.user_code)}}),
                      {});
                const auto expires = std::chrono::steady_clock::now() + std::chrono::minutes(10);
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < expires) {
                    login = poll_browser_signin(request);
                    if (login) {
                        break;
                    }
                    for (unsigned i = 0U; i < 20U && !stop.stop_requested(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
                if (stop.stop_requested()) {
                    return;
                }
                if (!login) {
                    throw std::runtime_error("Browser sign-in expired. Try Online again.");
                }
            }
            config.player_name = login->name;
            if (!login->session_id.empty()) {
                save_game_session(GameSession{login->session_id, login->session_secret},
                                  config.session_cache);
            }
        }

        content::ModManager mods(config.mods_directory, config.game_root);
        if (config.applied_mods) {
            mods = *config.applied_mods;
        } else {
            mods.rescan();
        }
        if (!config.mods_enabled) {
            for (std::size_t i = 0U; i < mods.mods().size(); ++i) {
                static_cast<void>(mods.set_enabled(i, false));
            }
        }
        const std::string mod_identity = content::installed_mod_identity(mods, stop);
        const std::string manifest = mod_manifest(mods, stop);

        Socket socket(Socket::Kind::Stream);
        socket.connect(config.host, config.port, std::chrono::milliseconds(2000));
        const Endpoint server = socket.remote_endpoint();

        const std::string account =
            login ? ",\"login_ticket\":" + quote(login->ticket) +
                        ",\"login_proof\":" +
                        quote(game_login_proof(login->secret, login->ticket, kClientTag.substr(1),
                                               kOnlineWireVersion, mod_identity, config.player_name))
                  : std::string();
        std::string sending = "{\"type\":\"hello\",\"wire\":" + std::to_string(kOnlineWireVersion) +
                              ",\"version\":" + quote(kClientTag.substr(1)) +
                              ",\"name\":" + quote(config.player_name) + ",\"mods\":" + quote(mod_identity) +
                              ",\"mod_manifest\":" + manifest + account +
                              release_hello_members(kClientTag.substr(1), kOnlineWireVersion, mod_identity,
                                                    config.player_name) +
                              "}\n";
        login.reset();

        std::string incoming;
        std::size_t sent = 0U;
        const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto ping_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto received_at = std::chrono::steady_clock::now();
        std::array<std::uint8_t, 4096U> bytes{};
        while (!stop.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            if (!connected.load() && now > handshake_deadline) {
                throw std::runtime_error("matchmaking version handshake timed out");
            }
            if (now - received_at > std::chrono::seconds(30)) {
                throw std::runtime_error("matchmaking server timed out");
            }
            if (sending.empty()) {
                const std::lock_guard lock(mutex);
                if (connected.load() && !outgoing.empty()) {
                    sending = std::move(outgoing.front()) + '\n';
                    outgoing.pop_front();
                    sent = 0U;
                }
            }
            if (sending.empty() && connected.load() && now >= ping_at) {
                sending = "{\"type\":\"ping\"}\n";
                sent = 0U;
                ping_at = now + std::chrono::seconds(10);
            }
            if (!sending.empty()) {
                sent += socket.send(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(sending.data()) + sent, sending.size() - sent));
                if (sent == sending.size()) {
                    sending.clear();
                }
            }
            if (!socket.readable(std::chrono::milliseconds(20))) {
                continue;
            }
            const std::size_t received = socket.receive(bytes);
            if (received == 0U) {
                continue;
            }
            received_at = std::chrono::steady_clock::now();
            incoming.append(reinterpret_cast<const char*>(bytes.data()), received);
            for (std::size_t newline = incoming.find('\n'); newline != std::string::npos;
                 newline = incoming.find('\n')) {
                if (newline > 65536U) {
                    throw std::runtime_error("matchmaking message too large");
                }
                content::JsonError error;
                const auto value = content::parse_json(std::string_view(incoming.data(), newline), error);
                if (!value || !value->is_object()) {
                    throw std::runtime_error("invalid matchmaking response");
                }
                incoming.erase(0U, newline + 1U);
                const std::string type = text_member(*value, "type");
                if (type == "error") {
                    const std::string reason = text_member(*value, "error");
                    if (reason == "session_revoked" || reason == "account_signed_in_elsewhere") {
                        if (config.browser_signin) {
                            forget_game_session(config.session_cache);
                        }
                        throw std::runtime_error(reason);
                    }
                }
                if (!connected.load()) {
                    if (type == "error") {
                        throw std::runtime_error(text_member(*value, "error"));
                    }
                    if (type != "welcome" || text_member(*value, "version") != kClientTag.substr(1) ||
                        number_member(*value, "wire") != kOnlineWireVersion) {
                        throw std::runtime_error("matchmaking version mismatch");
                    }
                    const std::uint32_t udp_port = number_member(*value, "udp_port");
                    const std::uint32_t id = number_member(*value, "id");
                    const std::string token = text_member(*value, "token");
                    bool token_valid = token.size() == 32U;
                    for (const char c : token) {
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                            token_valid = false;
                            break;
                        }
                    }
                    if (id == 0U || udp_port == 0U || udp_port > 65535U || !token_valid) {
                        throw std::runtime_error("invalid matchmaking admission");
                    }
                    content::JsonValue::Object payload = value->as_object();
                    payload.emplace_back("server_address", content::JsonValue(server.address));
                    payload.emplace_back("applied_mods", content::JsonValue(mod_identity));
                    connected.store(true);
                    event(MatchEvent::Kind::Connected, content::JsonValue(std::move(payload)), {});
                } else if (type == "rooms") {
                    event(MatchEvent::Kind::Rooms, *value, {});
                } else if (type == "room" || type == "joined") {
                    event(MatchEvent::Kind::RoomChanged, *value, {});
                } else if (type == "room_closed") {
                    event(MatchEvent::Kind::RoomClosed, *value, {});
                } else if (type == "started") {
                    event(MatchEvent::Kind::Started, *value, {});
                } else if (type == "left") {
                    event(MatchEvent::Kind::Left, *value, {});
                } else if (type == "error") {
                    event(MatchEvent::Kind::Rejected, *value, {});
                } else if (type == "ok") {
                    event(MatchEvent::Kind::Updated, *value, {});
                } else if (type == "chat") {
                    event(MatchEvent::Kind::Chat, *value, {});
                } else if (type == "service") {
                    event(MatchEvent::Kind::Service, *value, {});
                } else if (type == "pong") {
                    event(MatchEvent::Kind::Heartbeat, *value, {});
                } else {
                    throw std::runtime_error("unknown matchmaking response");
                }
            }
            if (incoming.size() > 65536U) {
                throw std::runtime_error("matchmaking message too large");
            }
        }
    } catch (const std::exception& error) {
        if (!stop.stop_requested()) {
            const std::lock_guard lock(mutex);
            if (events.size() >= 128U) {
                events.erase(events.begin());
            }
            events.push_back(MatchEvent{MatchEvent::Kind::Disconnected, content::JsonValue{}, error.what()});
        }
    }
    connected.store(false);
}

MatchmakingClient::MatchmakingClient() : impl_(std::make_unique<Impl>()) {}

MatchmakingClient::~MatchmakingClient() {
    disconnect();
}

void MatchmakingClient::connect(MatchmakingConfig config) {
    if (config.host.empty() || config.port == 0U || config.player_name.empty() ||
        config.player_name.size() > 32U) {
        throw std::invalid_argument("invalid native matchmaking configuration");
    }
    disconnect();
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->events.clear();
        impl_->outgoing.clear();
    }
    impl_->worker = std::jthread([this, config = std::move(config)](std::stop_token stop) mutable {
        impl_->run(stop, std::move(config));
    });
}

void MatchmakingClient::disconnect() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->connected.store(false);
    const std::lock_guard lock(impl_->mutex);
    impl_->events.clear();
    impl_->outgoing.clear();
}

bool MatchmakingClient::connected() const noexcept {
    return impl_->connected.load();
}

// RECOVERY: not identical to the original GIMPLE; original keeps the command text address-taken; ours does not.
bool MatchmakingClient::request(const MatchRequest& request) {
    std::string text = encode_command(request);
    if (request.request_id != 0U) {
        text.pop_back();
        text += ",\"request_id\":" + std::to_string(request.request_id) + "}";
    }
    if (text.size() > 65535U) {
        return false;
    }
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->connected.load() || impl_->outgoing.size() > 63U) {
        return false;
    }
    impl_->outgoing.push_back(std::move(text));
    return true;
}

// RECOVERY: not identical to the original GIMPLE; original calls the outlined variant reset visitor where this version inlines it.
std::vector<MatchEvent> MatchmakingClient::poll() {
    const std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->events, {});
}

} // namespace takedown::net
