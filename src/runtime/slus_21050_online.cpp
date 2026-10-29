#include "takedown/runtime/slus_21050_online.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/net/peer_transport.hpp"
#include "takedown/platform/raw_input_protocol.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_misc.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>

namespace takedown::runtime {
namespace {

using Address = guest::GuestAddress;

bool main_ram_range(const Address pointer, const std::size_t bytes) {
    const auto physical = pointer & 0x1FFFFFFFU;
    return (pointer != 0U) & ((pointer & 0x1E000000U) == 0U) &&
           bytes <= std::size_t{0x2000000U} - physical;
}

bool ram(const guest::EeMemory& memory, Address pointer, std::size_t bytes) {
    return main_ram_range(pointer, bytes) || memory.is_native_heap_range(pointer, bytes);
}

std::string tag_text(std::uint32_t n) {
    std::string text(4U, ' ');
    for (unsigned i = 0U; i != 4U; ++i) {
        text[i] = static_cast<char>(n >> ((3U - i) * 8U));
    }
    if (!std::all_of(text.begin(), text.end(), [](const char c) {
            return static_cast<unsigned char>(c) >= 32U && static_cast<unsigned char>(c) <= 126U;
        })) {
        return "<event>";
    }
    return text;
}

std::string read_text(const guest::EeMemory& memory, Address p, std::size_t maximum) {
    std::string result;
    for (std::size_t i = 0U; i < maximum; ++i) {
        if (!ram(memory, p, i + 1U)) {
            throw std::runtime_error("invalid online text pointer");
        }
        const auto byte = memory.read_u8(p + static_cast<std::uint32_t>(i));
        if (byte == 0U) {
            return result;
        }
        result.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("online text exceeds bound");
}

void write_text(guest::EeMemory& memory, Address p, std::string_view text) {
    if (!ram(memory, p, text.size() + 1U)) {
        throw std::runtime_error("invalid online output buffer");
    }
    for (std::size_t i = 0U; i < text.size(); ++i) {
        memory.write_u8(p + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    }
    memory.write_u8(p + static_cast<std::uint32_t>(text.size()), 0U);
}

std::string encode_tag_string(std::string_view value) {
    const char hex[] = "0123456789ABCDEF";
    std::string result = "\"";
    for (const unsigned char ch : value) {
        if (ch <= 32U || ch == '"' || ch == '%' || ch > 126U) {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 15]);
        } else {
            result.push_back(static_cast<char>(ch));
        }
    }
    return result + '"';
}

std::uint32_t integer(const std::map<std::string, std::string>& values, const std::string& key,
                      const std::uint32_t fallback = 0U) {
    const auto item = values.find(key);
    if (item == values.end()) {
        return fallback;
    }
    std::uint32_t value = 0U;
    const auto result = std::from_chars(item->second.data(), item->second.data() + item->second.size(), value);
    if (result.ec != std::errc{} || result.ptr != item->second.data() + item->second.size()) {
        throw std::runtime_error("invalid native online integer: " + key);
    }
    return value;
}

std::uint32_t number(const content::JsonValue& object, std::string_view key) {
    const auto* const value = object.find(key);
    if (value == nullptr || !value->is_number() || !std::isfinite(value->as_number()) ||
        value->as_number() < 0.0 || value->as_number() > 4294967295.0 ||
        std::floor(value->as_number()) != value->as_number()) {
        throw std::runtime_error("invalid native room integer");
    }
    return static_cast<std::uint32_t>(value->as_number());
}

std::string text(const content::JsonValue& object, std::string_view key) {
    const auto* const value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error("invalid native room text");
    }
    return value->as_string();
}

std::string virtual_address_text(std::uint32_t player) {
    return "10.254." + std::to_string((player >> 8U) & 0xFFU) + "." +
           std::to_string(player & 0xFFU);
}

Address localized(const guest::EeMemory& m, unsigned id) {
    const Address table = m.read_u32_le(0x0051BA44U);
    if (!main_ram_range(table, 16U)) {
        throw std::runtime_error("native online string table unavailable");
    }
    const Address strings = m.read_u32_le(table + 12U);
    if (!main_ram_range(strings, (id + 1U) * 4U)) {
        throw std::runtime_error("native online strings unavailable");
    }
    return m.read_u32_le(strings + id * 4U);
}

bool pad(const guest::EeMemory& m, unsigned slot) {
    const auto active = m.read_u8(0x004F6061U);
    if (active > 1U) {
        return false;
    }
    return m.read_u16_le(0x004F5E08U + active * 300U + slot) != 0U;
}


constexpr std::uint32_t tag(const char (&name)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(name[3]));
}

std::uint32_t packed_tag(const std::string_view value) {
    std::uint32_t result = 0U;
    for (const unsigned char c : value) {
        result = (result << 8U) | c;
    }
    return result;
}

void write_wide(guest::EeMemory& memory, Address address, std::string_view text) {
    if (text.size() > 511U) {
        throw std::runtime_error("native online message too long");
    }
    Address p = address;
    for (const unsigned char c : text) {
        memory.write_u16_le(p, c);
        p += 2U;
    }
    memory.write_u16_le(p, 0U);
}

std::string utf8_from_guest_utf16(const guest::EeMemory& memory, Address pointer) {
    std::string encoded;
    for (std::size_t i = 0U; i <= 511U; ++i) {
        const Address at = pointer + static_cast<std::uint32_t>(i) * 2U;
        if (!ram(memory, at, 2U)) {
            break;
        }
        std::uint32_t value = memory.read_u16_le(at);
        if (value == 0U) {
            break;
        }
        if (value - 0xD800U <= 1023U) {
            const Address next = pointer + static_cast<std::uint32_t>(i + 1U) * 2U;
            if (ram(memory, next, 2U)) {
                const std::uint32_t low = memory.read_u16_le(next);
                if (low - 0xDC00U <= 1023U) {
                    value = ((value - 0xD800U) << 10U) + low + 9216U;
                    ++i;
                } else {
                    value = 0xFFFDU;
                }
            }
        } else if (value - 0xDC00U <= 1023U) {
            value = 0xFFFDU;
        }
        if (value <= 127U) {
            encoded.push_back(static_cast<char>(value));
        } else if (value <= 2047U) {
            encoded.push_back(static_cast<char>(192U | (value >> 6U)));
            encoded.push_back(static_cast<char>(128U | (value & 63U)));
        } else if (value <= 65535U) {
            encoded.push_back(static_cast<char>(224U | (value >> 12U)));
            encoded.push_back(static_cast<char>(128U | ((value >> 6U) & 63U)));
            encoded.push_back(static_cast<char>(128U | (value & 63U)));
        } else {
            encoded.push_back(static_cast<char>(240U | (value >> 18U)));
            encoded.push_back(static_cast<char>(128U | ((value >> 12U) & 63U)));
            encoded.push_back(static_cast<char>(128U | ((value >> 6U) & 63U)));
            encoded.push_back(static_cast<char>(128U | (value & 63U)));
        }
    }
    return encoded;
}

std::string percent_decode(std::string_view value) {
    std::string decoded;
    for (std::size_t i = 0U; value.size() > i; ++i) {
        if (value[i] == '%' && value.size() > i + 2U) {
            unsigned byte = 0U;
            const auto parsed =
                std::from_chars(value.data() + i + 1U, value.data() + i + 3U, byte, 16);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + i + 3U) {
                decoded.push_back(static_cast<char>(byte));
                i += 2U;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::map<std::string, std::string> parse_fields(std::string_view text) {
    std::map<std::string, std::string> fields;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0U, end);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        const auto equals = line.find('=');
        if (equals != std::string_view::npos) {
            auto value = line.substr(equals + 1U);
            if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
                value = value.substr(1U, value.size() - 2U);
            }
            fields.emplace(std::string(line.substr(0U, equals)), percent_decode(value));
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1U);
    }
    return fields;
}

struct Signature {
    Address begin;
    Address end;
    std::uint64_t hash;
};

constexpr std::array<Signature, 62> signatures{{
    {0x002348A0U, 0x00234994U, 0x9DFB1308A834816FULL},
    {0x0033BD80U, 0x0033BDF0U, 0xBA409B3F4C65C5D8ULL},
    {0x00339C50U, 0x00339E70U, 0x630182DE21C68608ULL},
    {0x004196E8U, 0x00419728U, 0x641B156D45536794ULL},
    {0x00313230U, 0x00313450U, 0x45A99DBD16E48019ULL},
    {0x00397280U, 0x003974E0U, 0x494A7C15C028CC26ULL},
    {0x0022F220U, 0x0022F320U, 0x84B073A3D97367E9ULL},
    {0x0035F100U, 0x0035F240U, 0xE5678190FB657DBEULL},
    {0x0035F240U, 0x0035F490U, 0xEA1B58913560B833ULL},
    {0x00335F60U, 0x00335FB4U, 0x6C62784F1EE57D7AULL},
    {0x00335FC0U, 0x00335FC8U, 0x0CF51AC412356254ULL},
    {0x00335FD0U, 0x00336144U, 0x95B8CAEC670D069EULL},
    {0x00336150U, 0x003361B4U, 0xFAA67AE380E98AB1ULL},
    {0x00338D60U, 0x00338DE8U, 0x8D2623421ACC776CULL},
    {0x0023B380U, 0x0023B4FCU, 0xB2A75334C941B80BULL},
    {0x00379910U, 0x00379980U, 0x7149AF42CDF1ECD7ULL},
    {0x0033C390U, 0x0033C620U, 0xD85EF1A1AE29E7FCULL},
    {0x0035F4D0U, 0x00360AC0U, 0xAF8DAE4666711FEFULL},
    {0x00323A10U, 0x00323C58U, 0x6CD0B3014B2D81A7ULL},
    {0x00323E30U, 0x003242E8U, 0xBBF9B7851BBC59D9ULL},
    {0x0022EB40U, 0x0022EBF8U, 0xA18E85B428E9575FULL},
    {0x0022EC00U, 0x0022EC60U, 0xEB72E1383B7AC336ULL},
    {0x0022EC60U, 0x0022EDD4U, 0xCB4B6E6B0594BDD0ULL},
    {0x0022EDE0U, 0x0022F000U, 0x4A60032D2B797169ULL},
    {0x00360800U, 0x003608ECU, 0x1900E52CA8F23FB1ULL},
    {0x003406C0U, 0x003418E4U, 0x016B7CA88DBF9B47ULL},
    {0x0034BC30U, 0x0034BEA0U, 0x38738C870F67BE37ULL},
    {0x0034CD50U, 0x0034CEC8U, 0xD67A04D46521AA25ULL},
    {0x002C2600U, 0x002C26BCU, 0x372B4F29D2C42111ULL},
    {0x002C26D0U, 0x002C2830U, 0x07FA1B7CCC3C9539ULL},
    {0x0026D2A0U, 0x0026D968U, 0x96372F42A6CBEF61ULL},
    {0x00230600U, 0x00230C50U, 0xFA4D8F5841D599A5ULL},
    {0x00347440U, 0x003476B0U, 0xE045B11FDC800F95ULL},
    {0x003530F0U, 0x00353518U, 0x362802944DF7738BULL},
    {0x0022F000U, 0x0022F020U, 0xF69877FA57AAB82FULL},
    {0x0022F020U, 0x0022F214U, 0x90045F1D9B8A13CCULL},
    {0x0022F320U, 0x0022F504U, 0xE5C9548C67D7A0C1ULL},
    {0x003464B0U, 0x00346944U, 0x41AD25519F519FA2ULL},
    {0x0034A430U, 0x0034A4D8U, 0xDF2A36E84667EFE3ULL},
    {0x00349E00U, 0x0034A424U, 0x45C743A7C7C4D842ULL},
    {0x00349DF0U, 0x00349DFCU, 0x8EA89A9083CC5C0AULL},
    {0x003181E0U, 0x0031833CU, 0x87EF4A50827803AEULL},
    {0x004020F8U, 0x004021D8U, 0xB363A2559F193BCEULL},
    {0x004021D8U, 0x00402260U, 0xA7993F1088B12B50ULL},
    {0x00402260U, 0x004022E8U, 0x8C556072151F4E68ULL},
    {0x004022E8U, 0x00402664U, 0x52B7F09A74C3010DULL},
    {0x00402668U, 0x004026C8U, 0x9BA9B00AC963C3A3ULL},
    {0x004026C8U, 0x0040276CU, 0x536FB1EA28006780ULL},
    {0x00402AC8U, 0x00402C04U, 0xA2A7A5EED8B7219AULL},
    {0x00402C08U, 0x00402C88U, 0xD4E90242E4044118ULL},
    {0x00402C88U, 0x00402DD0U, 0x84A2A76FE6649036ULL},
    {0x00402DD0U, 0x00402E5CU, 0x51C359215C537E03ULL},
    {0x00402E60U, 0x00402F64U, 0xC2D5F2F82064D525ULL},
    {0x00402F68U, 0x00402FF4U, 0x086B5E7B0F09C81CULL},
    {0x00402FF8U, 0x00403184U, 0xB79852E9DA468179ULL},
    {0x00403188U, 0x00403190U, 0x589026CFBEC22F44ULL},
    {0x00403190U, 0x004032D4U, 0xD3ACCB0B951C6419ULL},
    {0x004032F8U, 0x004034C0U, 0xEC678A4AC1C3163CULL},
    {0x004034C0U, 0x0040360CU, 0xFE6AD6AA47F3D3A4ULL},
    {0x00403610U, 0x00403690U, 0x5CBF882DCB863221ULL},
    {0x00403690U, 0x0040371CU, 0x1396119D74975106ULL},
    {0x00403720U, 0x0040373CU, 0x84D77AD28E0C3838ULL},
}};

std::uint64_t fnv1a(const guest::EeMemory& memory, Address begin, Address end) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned p = begin; p < end; ++p) {
        hash = (hash ^ memory.read_u8(p)) * 1099511628211ULL;
    }
    return hash;
}

bool signature_matches(const guest::EeMemory& memory, const Signature& signature) {
    if (!main_ram_range(signature.begin, static_cast<std::size_t>(signature.end - signature.begin))) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned p = signature.begin; p < signature.end; ++p) {
        hash = (hash ^ memory.read_u8(p)) * 1099511628211ULL;
    }
    return hash == signature.hash;
}

Address hook_address(const Address entry) {
    if (entry == 0x00313230U) {
        return 0x0031336CU;
    }
    if (entry == 0x00397280U) {
        return 0x003973F8U;
    }
    if (entry == 0x00338D60U) {
        return 0x00338DCCU;
    }
    if (entry == 0x0033C390U) {
        return 0x0033C5A4U;
    }
    if (entry == 0x0035F4D0U) {
        return 0x00360950U;
    }
    if (entry == 0x00323A10U) {
        return 0x00323A50U;
    }
    if (entry == 0x00323E30U) {
        return 0x00323E34U;
    }
    if (entry == 0x003406C0U) {
        return 0x0034115CU;
    }
    if (entry == 0x0034BC30U) {
        return 0x0034BD60U;
    }
    if (entry == 0x0034CD50U) {
        return 0x0034CD68U;
    }
    if (entry == 0x00347440U) {
        return 0x003474ACU;
    }
    if (entry == 0x003530F0U) {
        return 0x00353114U;
    }
    if (entry == 0x003181E0U) {
        return 0x0031826CU;
    }
    if (entry == 0x0034A430U) {
        return 0x0034A478U;
    }
    return entry;
}

} // namespace

struct Slus21050Online::Impl {
    enum class State { Idle, Connecting, Online, Failed };
    enum class RejoinStage { None, Connecting, Listing, Failed, Joining, Admitted, Refused };

    struct Reply {
        std::uint32_t kind{};
        std::uint32_t code{};
        std::string text;
    };
    struct Pending {
        std::size_t ref{};
        std::uint32_t command{};
        std::chrono::steady_clock::time_point deadline;
    };
    struct BuddyRequest {
        Address callback{};
        Address context{};
        std::chrono::steady_clock::time_point deadline;
    };
    struct BuddyCallback {
        Address target{};
        Address context{};
        bool success{};
    };
    struct LobbyUser {
        std::string name;
        std::uint32_t game{};
        bool operator==(const LobbyUser&) const = default;
    };
    struct Ref {
        bool used{};
        bool connected{};
        std::deque<Reply> replies;
        std::uint32_t persona{};
        std::map<std::uint32_t, LobbyUser> visible_users;
    };
    struct GuestJoin {
        std::array<std::uint32_t, 10> words{};
        std::string name;
        std::string password;
        std::string digest;
        std::string host_name;
        std::uint32_t screen{};
        std::uint32_t page{};
        std::uint32_t lobby{};
        std::uint32_t frontend_return{};
        std::array<std::uint32_t, 12> browser_selection{};
        std::uint32_t room{};
        std::uint32_t host{};
        std::size_t ref{};
    };

    net::MatchmakingConfig config;
    ModSyncSelectionResolver mod_sync_selection_resolver;
    net::MatchmakingClient client;
    std::unique_ptr<net::PeerTransport> peers;
    State state{State::Idle};
    bool open{};
    bool presence_requested{};
    std::uint32_t player{};
    std::string server_address;
    std::string applied_mod_identity;
    bool revision_registered{};
    std::deque<std::uint32_t> text_keys;
    bool configuration_open{};
    bool configuration_message_sent{};
    bool busy_box{};
    bool error_shown{};
    std::uint32_t ui_return{};
    std::uint32_t page_message{};
    std::uint32_t news_request{};
    std::optional<std::string> news_body;
    bool news_failed{};
    std::chrono::steady_clock::time_point news_deadline;
    std::array<Ref, 4> refs;
    std::map<std::uint32_t, LobbyUser> lobby_users;
    std::optional<GuestJoin> last_guest_join;
    std::optional<GuestJoin> blocked_join;
    RejoinStage rejoin_stage{RejoinStage::None};
    bool rejoin_prepare_only{};
    bool rejoin_prepared{};
    bool rejoin_prepared_announced{};
    bool mod_mismatch_pending{};
    std::uint32_t rejoin_list_request{};
    std::uint32_t rejoin_join_request{};
    std::uint32_t retired_rejoin_request{};
    std::chrono::steady_clock::time_point rejoin_deadline;
    std::optional<std::string> rejoin_error;
    bool discarded_session{};
    std::map<std::uint32_t, Pending> pending;
    std::uint32_t next_request{1U};
    std::optional<content::JsonValue> room;
    std::optional<content::JsonValue> lobby;
    std::map<std::string, std::uint32_t> discovered_rooms;
    std::map<std::uint32_t, content::JsonValue> discovered_room_details;
    std::uint16_t game_port{};
    bool packet_diagnostic{std::getenv("TAKEDOWN_ONLINE_PACKET_DIAGNOSTIC") != nullptr};
    std::uint32_t send_diagnostics{};
    std::uint32_t receive_diagnostics{};
    std::uint32_t socket_diagnostics{};
    std::vector<std::uint32_t> readiness_roster;
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> readiness_reports;
    std::chrono::steady_clock::time_point readiness_advertisement;
    bool advertised_ready{};
    std::deque<net::PeerMessage> datagrams;
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    content::JsonValue::Array buddies;
    std::map<std::uint32_t, BuddyRequest> buddy_requests;
    std::deque<BuddyCallback> buddy_callbacks;
    std::chrono::steady_clock::time_point buddy_refresh;
    bool buddies_changed{};
    bool buddy_initialized{};


    void clear_buddies() {
        buddies.clear();
        buddy_requests.clear();
        buddy_callbacks.clear();
        buddy_refresh = {};
        buddies_changed = true;
    }

    void publish_presence(guest::EeMemory& memory) {
        if (!buddy_initialized && !presence_requested) {
            return;
        }
        memory.write_u8(31708336U, presence_requested && state == State::Online);
        memory.write_u8(31708252U, presence_requested && state == State::Connecting);
        memory.write_u8(31708337U, buddy_initialized);
        if (buddies_changed) {
            memory.write_u8(31708251U, 1U);
            buddies_changed = false;
        }
    }

    const content::JsonValue* buddy(std::string_view name) const {
        const auto found = std::find_if(buddies.begin(), buddies.end(), [&name](const content::JsonValue& item) {
            const auto candidate = text(item, "name");
            return candidate == name || candidate.substr(0U, 15U) == name;
        });
        return found == buddies.end() ? nullptr : &*found;
    }

    bool request_buddies(std::string action, std::string name, Address callback, Address context) {
        if (state != State::Online || buddy_requests.size() > 7U || next_request == 0U) {
            return false;
        }
        net::MatchRequest request;
        request.kind = net::MatchRequest::Kind::Service;
        request.operation = "buddies";
        request.fields = {{"action", std::move(action)}, {"name", std::move(name)}};
        request.request_id = next_request++;
        if (!client.request(request)) {
            return false;
        }
        buddy_requests.emplace(request.request_id,
                               BuddyRequest{callback, context,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(10)});
        buddy_refresh = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return true;
    }

    void connect() {
        if (state == State::Connecting || state == State::Online) {
            return;
        }
        std::clog << "Native online: connecting to online services\n";
        clear_buddies();
        lobby_users.clear();
        presence_requested = false;
        state = State::Connecting;
        discarded_session = false;
        try {
            client.connect(config);
        } catch (const std::exception& error) {
            state = State::Failed;
            std::clog << "Native online: connection failed: " << error.what() << '\n';
        }
    }

    void ui_call(NativeFunctionContext& c, Address target, Address ret, std::uint32_t a,
                 std::uint32_t b, std::uint32_t d, std::uint32_t e) {
        for (unsigned reg = 4U; reg != 11U; ++reg) {
            c.cpu.write_gpr_u64(reg, 0U);
        }
        c.cpu.write_gpr_u64(4U, a);
        c.cpu.write_gpr_u64(5U, b);
        c.cpu.write_gpr_u64(6U, d);
        c.cpu.write_gpr_u64(7U, e);
        c.cpu.write_gpr_u64(31U, ret);
        c.cpu.set_pc(target);
    }

    void fail_rejoin(std::string error) {
        if (rejoin_join_request != 0U) {
            pending.erase(rejoin_join_request);
        }
        client.disconnect();
        discarded_session = true;
        peers.reset();
        room.reset();
        datagrams.clear();
        readiness_roster.clear();
        readiness_reports.clear();
        state = State::Failed;
        for (auto& ref : refs) {
            ref.connected = false;
            ref.replies.clear();
        }
        rejoin_stage = RejoinStage::None;
        rejoin_prepared = false;
        rejoin_join_request = 0U;
        rejoin_list_request = 0U;
        rejoin_error = std::move(error);
    }

    bool rejoin_dispatch_context_valid(const guest::EeMemory& memory) const {
        if (blocked_page_active(memory) || !blocked_join.has_value()) {
            return true;
        }
        const Address object = memory.read_u32_le(31706744U);
        const bool valid =
            blocked_reference_live() && memory.read_u32_le(32123252U) == 32124440U &&
            memory.read_u32_le(32048176U) == 32081040U &&
            memory.read_u32_le(32081040U) == 5104592U && memory.read_u32_le(5104600U) == 3239552U &&
            memory.read_u32_le(32048164U) == 5U &&
            memory.read_u32_le(31706696U) == (*blocked_join).lobby &&
            ram(memory, (*blocked_join).lobby, 4U) &&
            memory.read_u32_le((*blocked_join).lobby) == 729088U + (*blocked_join).ref * 8960U &&
            memory.read_u8(31706720U) == 1U && memory.read_u8(31706725U) == 1U &&
            memory.read_u32_le(31706736U) == (*blocked_join).room && ram(memory, object, 4U) &&
            memory.read_u32_le(object) == (*blocked_join).room;
        if (!valid) {
            std::clog << "Native online: mod-sync join context changed screen=" << std::hex
                      << memory.read_u32_le(32123252U) << " page=" << memory.read_u32_le(32048176U)
                      << " phase=" << memory.read_u32_le(32048164U)
                      << " room=" << memory.read_u32_le(31706736U) << " object=" << object
                      << " lobby=" << memory.read_u32_le(31706696U) << std::dec
                      << " joined=" << static_cast<unsigned>(memory.read_u8(31706720U))
                      << " complete=" << static_cast<unsigned>(memory.read_u8(31706725U))
                      << '\n';
        }
        return valid;
    }

    bool admitted_rejoin_matches(std::size_t ref) const {
        return rejoin_stage == RejoinStage::Admitted && blocked_join.has_value() &&
               room.has_value() && blocked_join->ref == ref &&
               number(*room, "id") == blocked_join->room &&
               number(*room, "host") == blocked_join->host &&
               text(*room, "mods") == blocked_join->digest &&
               text(*room, "name") == blocked_join->name;
    }

    void complete_admitted_rejoin() {
        const auto item = pending.find(rejoin_join_request);
        if (item != pending.end() && blocked_join.has_value() &&
            item->second.ref == blocked_join->ref && item->second.command == 0x676A6F69U) {
            reply(refs[item->second.ref], item->second.command, room_body(*room), 0U);
            pending.erase(item);
        }
        retired_rejoin_request = rejoin_join_request;
        rejoin_stage = RejoinStage::None;
        rejoin_join_request = 0U;
        blocked_join.reset();
        mod_mismatch_pending = false;
        std::clog << "Native online: mod-sync completed through admitted-room join reply\n";
    }

    bool update_lobby_users(const content::JsonValue& roster) {
        if (!roster.is_array() || roster.as_array().size() > 256U) {
            throw std::runtime_error("invalid native public lobby roster");
        }
        std::map<std::uint32_t, LobbyUser> replacement;
        for (const auto& member : roster.as_array()) {
            const auto id = number(member, "id");
            const auto name = text(member, "name");
            if (id == 0U || name.empty() || name.size() > 32U || replacement.contains(id) ||
                std::any_of(name.begin(), name.end(), [](const unsigned char ch) {
                    return ch < 32U || ch == 127U;
                })) {
                throw std::runtime_error("invalid native public lobby member");
            }
            replacement.emplace(id, LobbyUser{name, number(member, "game")});
        }
        if (replacement == lobby_users) {
            return false;
        }
        lobby_users = std::move(replacement);
        return true;
    }

    bool text_entry_focused(const guest::EeMemory& memory) const {
        if (!revision_registered || state != State::Online || memory.read_u8(32020938U) == 0U) {
            return false;
        }
        const Address text = memory.read_u32_le(32033428U);
        const std::uint32_t length = memory.read_u32_le(32033740U);
        return length - 2U <= 510U && ram(memory, text, length * 2U) &&
               memory.read_u32_le(5199132U) == 32020928U;
    }

    bool blocked_reference_live() const {
        return blocked_join.has_value() && (*blocked_join).ref <= 3U && refs[(*blocked_join).ref].used &&
               refs[(*blocked_join).ref].connected && refs[(*blocked_join).ref].persona != 0U;
    }

    bool blocked_page_active(const guest::EeMemory& memory) const {
        return blocked_reference_live() && memory.read_u32_le(32123252U) == (*blocked_join).screen &&
               ((*blocked_join).page == 0U || memory.read_u32_le(32048176U) == (*blocked_join).page) &&
               memory.read_u32_le(31706696U) == (*blocked_join).lobby &&
               ram(memory, (*blocked_join).lobby, 4U) &&
               memory.read_u32_le((*blocked_join).lobby) == 729088U + (*blocked_join).ref * 8960U;
    }

    bool frontend(Address address, NativeFunctionContext& c) {
        if (address == 2312352U) {
            const auto a0 = c.cpu.read_gpr_u32(4U);
            bool live_sdk = false;
            const Address lobby_object = c.memory.read_u32_le(31706696U);
            if (ram(c.memory, lobby_object, 4U)) {
                const auto marker = c.memory.read_u32_le(lobby_object);
                if (marker >= 729088U) {
                    const auto offset = marker - 729088U;
                    if (offset % 8960U == 0U && offset / 8960U < refs.size()) {
                        const auto& ref = refs[offset / 8960U];
                        live_sdk = ref.used && ref.connected && ref.persona != 0U;
                    }
                }
            }
            if (a0 == 31666112U && state == State::Online && player != 0U && peers != nullptr &&
                live_sdk && c.memory.read_u32_le(31667428U) == 31704608U &&
                c.memory.read_u32_le(31667524U) == 9U && c.memory.read_u32_le(31667536U) == 3658U) {
                const auto address_text = virtual_address_text(player);
                const std::uint32_t local = (player & 65535U) + 184418304U;
                if (c.memory.read_u32_le(31667500U) != local ||
                    c.memory.read_u32_le(31667516U) != local ||
                    read_text(c.memory, 31704636U, 16U) != address_text ||
                    read_text(c.memory, 31667484U, 16U) != address_text) {
                    write_text(c.memory, 31704636U, address_text);
                    write_text(c.memory, 31667484U, address_text);
                    c.memory.write_u32_le(31667500U, local);
                    c.memory.write_u32_le(31667516U, local);
                    std::clog << "Native online: refreshed local endpoint " << address_text << '\n';
                }
                if (read_text(c.memory, 31707836U, 16U) != address_text) {
                    write_text(c.memory, 31707836U, address_text);
                }
            }
            c.cpu.write_gpr_sign_extended_32(3U, c.memory.read_u32_le(a0 + 1404U));
            c.cpu.set_pc(2312356U);
            return true;
        }
        if (address == 3390848U) {
            if (c.cpu.read_gpr_u32(4U) == 31706436U) {
                c.memory.ensure_native_heap(c.cpu.read_gpr_u32(6U));
                c.cpu.write_gpr_u64(5U, 100663296U);
            }
            c.cpu.write_gpr_u64(29U, c.cpu.read_gpr_u32(29U) - 64U);
            c.cpu.set_pc(3390852U);
            return true;
        }
        if (address == 3382352U) {
            last_guest_join.reset();
            const auto a1 = c.cpu.read_gpr_u32(5U);
            if (c.cpu.read_gpr_u32(4U) == 31706436U && ram(c.memory, a1, 40U)) {
                GuestJoin saved;
                for (unsigned i = 0U; i != 10U; ++i) {
                    saved.words[i] = c.memory.read_u32_le(a1 + i * 4U);
                }
                saved.name = read_text(c.memory, saved.words[1], 33U);
                saved.password = read_text(c.memory, saved.words[2], 33U);
                if (blocked_join.has_value() && admitted_rejoin_matches(blocked_join->ref) &&
                    saved.words[0] == blocked_join->room && saved.name == blocked_join->name &&
                    saved.password == blocked_join->password &&
                    rejoin_dispatch_context_valid(c.memory)) {
                    complete_admitted_rejoin();
                }
                saved.screen = c.memory.read_u32_le(32123252U);
                saved.page = c.memory.read_u32_le(32048176U);
                saved.frontend_return = c.cpu.read_gpr_u32(31U);
                if (saved.page == 32086432U ||
                    (saved.page == 32104528U && saved.frontend_return == 3650248U)) {
                    for (unsigned i = 0U; i != 12U; ++i) {
                        saved.browser_selection[i] = c.memory.read_u32_le(saved.page + i * 4U + 8U);
                    }
                }
                saved.lobby = c.memory.read_u32_le(31706696U);
                last_guest_join = saved;
            }
            c.cpu.write_gpr_u64(29U, c.cpu.read_gpr_u32(29U) - 144U);
            c.cpu.set_pc(3382356U);
            return true;
        }
        if (address == 3224428U || address == 3765240U) {
            const Address target = c.cpu.read_gpr_u32(29U) + 32U;
            const std::string encoded =
                utf8_from_guest_utf16(c.memory, c.memory.read_u32_le(32033428U));
            write_text(c.memory, target, encoded);
            c.cpu.set_pc(address == 3224428U ? 3224444U : 3765256U);
            return true;
        }
        if (address == 3643664U) {
            if (c.cpu.read_gpr_u32(7U) == 991U) {
                write_wide(c.memory, 727808U, "Retrieving news...");
                c.cpu.write_gpr_u64(7U, 727808U);
                c.cpu.set_pc(3643024U);
            } else {
                c.cpu.write_gpr_u64(29U, c.cpu.read_gpr_u32(29U) - 80U);
                c.cpu.set_pc(3643668U);
            }
            return true;
        }
        if (address == 3392932U) {
            const Address source = c.cpu.read_gpr_u32(29U) + 416U;
            const Address destination = c.memory.read_u32_le(c.cpu.read_gpr_u32(28U) - 22724U);
            write_text(c.memory, destination, read_text(c.memory, source, 257U));
            c.cpu.write_gpr_u64(3U, c.memory.read_u32_le(c.cpu.read_gpr_u32(16U) + 1540U));
            c.cpu.set_pc(3392936U);
            return true;
        }
        if (address == 3541328U) {
            if (rejoin_stage == RejoinStage::Admitted && !rejoin_dispatch_context_valid(c.memory)) {
                fail_rejoin("The original online page changed. Choose the lobby again.");
            }
            update();
            publish_presence(c.memory);
            if (presence_requested && state == State::Online && buddy_requests.empty() &&
                buddy_refresh <= std::chrono::steady_clock::now()) {
                request_buddies("list", std::string{}, 0U, 0U);
            }
            const auto ret = c.cpu.read_gpr_u32(31U);
            if (rejoin_stage == RejoinStage::Failed && state == State::Online) {
                if (mod_mismatch_pending && c.memory.read_u8(31706710U) != 0U) {
                    if (!rejoin_prepared_announced) {
                        if (c.memory.read_u32_le(31706712U) != 0U) {
                            fail_rejoin("The online session reported another error. Choose the lobby again.");
                        } else {
                            mod_mismatch_pending = false;
                            std::clog << "Native online: acknowledging owned mod-sync join refusal\n";
                            ui_call(c, 3378816U, ret, 31706436U, 0U, 0U, 0U);
                            return true;
                        }
                    }
                } else if (c.memory.read_u8(31706708U) == 0U && c.memory.read_u8(31706710U) == 0U) {
                    mod_mismatch_pending = false;
                    rejoin_prepared = true;
                    rejoin_prepared_announced = true;
                    std::clog << "Native online: mod-sync prepared for original browser return\n";
                }
            }
            if (rejoin_stage == RejoinStage::Joining && blocked_join.has_value()) {
                if (!blocked_page_active(c.memory)) {
                    fail_rejoin("The original online page changed. Choose the lobby again.");
                } else if (c.memory.read_u8(31706710U) != 0U) {
                    fail_rejoin("The online session reported another error. Choose the lobby again.");
                } else if (c.memory.read_u8(31706708U) == 0U) {
                    std::optional<ModSyncSelection> selection;
                    if (mod_sync_selection_resolver) {
                        const std::uint64_t vehicle_key =
                            static_cast<std::uint64_t>(blocked_join->words[3]) |
                            (static_cast<std::uint64_t>(blocked_join->words[4]) << 32U);
                        selection = mod_sync_selection_resolver(c.memory, vehicle_key,
                                                                blocked_join->words[5]);
                    }
                    if (!selection) {
                        fail_rejoin("No available car can join this lobby. Choose an available car and select the lobby again.");
                    } else {
                        for (std::uint32_t i = 0U; i != 10U; ++i) {
                            c.memory.write_u32_le((i + 191488U) * 4U, blocked_join->words[i]);
                        }
                        c.memory.write_u32_le(765952U, blocked_join->room);
                        c.memory.write_u32_le(765956U, 766208U);
                        c.memory.write_u32_le(765960U, 766336U);
                        c.memory.write_u64_le(765964U, selection->vehicle_key);
                        c.memory.write_u32_le(765972U, selection->paint);
                        write_text(c.memory, 766208U, blocked_join->name);
                        write_text(c.memory, 766336U, blocked_join->password);
                        const bool hosted =
                            blocked_join->page == 32086432U &&
                            (blocked_join->frontend_return == 3314448U ||
                             blocked_join->frontend_return == 3312592U);
                        const bool browsed = blocked_join->page == 32104528U &&
                                             blocked_join->frontend_return == 3650248U;
                        if (hosted || browsed) {
                            for (std::uint32_t i = 0U; i != 12U; ++i) {
                                c.memory.write_u32_le(blocked_join->page + i * 4U + 8U,
                                                      blocked_join->browser_selection[i]);
                            }
                            if (hosted) {
                                c.memory.write_u32_le(blocked_join->page + 6928U, blocked_join->room);
                                c.memory.write_u8(blocked_join->page + 6933U, 1U);
                            } else {
                                c.memory.write_u8(blocked_join->page + 6466U, 5U);
                            }
                        }
                        rejoin_stage = RejoinStage::Admitted;
                        ui_call(c, 3382352U, ret, 31706436U, 765952U, 0U, 0U);
                        return true;
                    }
                }
            }
            if (text_entry_focused(c.memory)) {
                if (!text_keys.empty()) {
                    const auto key = text_keys.front();
                    text_keys.pop_front();
                    if (key == 1114113U) {
                        text_keys.clear();
                        ui_call(c, 3206512U, ret, 32123248U, 3U, 0U, 0U);
                        return true;
                    }
                    if (key == 1114112U) {
                        text_keys.clear();
                    }
                    ui_call(c, 3463440U, ret, 32020928U, key == 1114112U ? 10U : key, 0U, 0U);
                    return true;
                }
            } else {
                text_keys.clear();
            }
            if (!buddy_callbacks.empty()) {
                const auto callback = buddy_callbacks.front();
                buddy_callbacks.pop_front();
                ui_call(c, callback.target, ret, callback.success ? 1U : 0U, callback.context, 0U, 0U);
                return true;
            }
            c.cpu.set_pc(ret);
            return true;
        }
        if (address == 3291728U) {
            c.cpu.write_gpr_u64(2U, 0U);
            c.cpu.set_pc(3291732U);
            return true;
        }
        if (address == 3291960U) {
            const auto table = c.cpu.read_gpr_u32(16U);
            const std::uint32_t count = c.memory.read_u16_le(table + 6U);
            if (count - 5U <= 1U) {
                for (std::uint32_t row = 2U; row < count; ++row) {
                    const auto base = table + row * 8U;
                    c.memory.write_u32_le(base + 8U, c.memory.read_u32_le(base + 16U));
                    c.memory.write_u32_le(base + 12U, c.memory.read_u32_le(base + 20U));
                }
                c.memory.write_u16_le(table + 6U, static_cast<std::uint16_t>(count - 1U));
            }
            c.cpu.write_gpr_u64(2U, 1142947840U);
            c.cpu.set_pc(3291964U);
            return true;
        }
        if (address == 3292724U) {
            if (c.cpu.read_gpr_u32(5U) == 36U) {
                c.cpu.write_gpr_u64(29U, c.cpu.read_gpr_u32(29U) + 32U);
                c.cpu.set_pc(c.cpu.read_gpr_u32(31U));
            } else {
                c.cpu.write_gpr_u64(3U, 36U);
                c.cpu.set_pc(3292728U);
            }
            return true;
        }
        if (address == 3292780U) {
            const auto v1 = c.cpu.read_gpr_u32(3U);
            if (v1 - 2U <= 2U) {
                c.cpu.write_gpr_u64(3U, v1 + 1U);
            }
            c.cpu.set_pc(3292784U);
            c.cpu.write_gpr_u64(1U, c.cpu.read_gpr_u32(3U) <= 5U ? 1U : 0U);
            return true;
        }
        if (address == 3412316U) {
            update();
            c.cpu.set_pc(state == State::Online && presence_requested ? 3412492U
                         : state == State::Failed                       ? 3412348U
                                                                        : 3414220U);
            return true;
        }
        if (address == 3456352U) {
            const auto a2 = c.cpu.read_gpr_u32(6U);
            c.cpu.set_pc(state == State::Online || c.memory.read_u8(a2 + 172U) != 0U ? 3456364U
                                                                                     : 3456404U);
            return true;
        }
        if (address == 3460456U) {
            if (state == State::Online) {
                write_text(c.memory, 727296U, config.player_name);
                write_text(c.memory, 727424U, "");
                c.cpu.write_gpr_u64(18U, 727296U);
                c.cpu.write_gpr_u64(6U, 727424U);
            } else {
                c.cpu.write_gpr_u64(18U, c.cpu.read_gpr_u32(5U));
            }
            c.cpu.set_pc(3460460U);
            return true;
        }
        if (address == 3437740U) {
            c.cpu.set_pc(state == State::Online && c.memory.read_u32_le(31706696U) != 0U ? 3437760U
                                                                                         : 3437912U);
            return true;
        }
        if (address == 3485972U) {
            update();
            if (state == State::Online) {
                const auto s0 = c.cpu.read_gpr_u32(16U);
                c.cpu.set_pc(c.memory.read_u8(s0 + 8U) != 0U ? 3486276U : 3486420U);
            } else {
                c.cpu.set_pc(3486984U);
            }
            return true;
        }
        if (address == 3486436U) {
            c.cpu.set_pc(c.cpu.read_gpr_u32(2U) != 0U ? 3486668U : 3486984U);
            return true;
        }
        if (address == 3433648U) {
            update();
            const auto ret = c.cpu.read_gpr_u32(31U);
            if (c.memory.read_u32_le(32048164U) == 5U && state == State::Online) {
                c.memory.write_u32_le(32093880U, 1U);
                ui_call(c, 3200352U, ret, 32047680U, 1U, 0U, 0U);
            } else {
                c.cpu.set_pc(ret);
            }
            return true;
        }
        if (address == 3449976U) {
            configuration_open = true;
            configuration_message_sent = false;
            error_shown = false;
            open = true;
            connect();
            write_wide(c.memory, 724992U,
                       config.browser_signin ? "SIGN IN USING YOUR WEB BROWSER..."
                                             : "CONNECTING TO ONLINE SERVICES...");
            busy_box = true;
            ui_call(c, 3329024U, 3450004U, 32020712U, 1U, 724992U, localized(c.memory, 15U));
            return true;
        }
        if (address == 3450020U) {
            c.cpu.write_gpr_u64(5U, 724992U);
            c.cpu.set_pc(3450024U);
            return true;
        }
        if (address == 3448304U) {
            configuration_open = false;
            if (busy_box) {
                busy_box = false;
                ui_call(c, 3323184U, c.cpu.read_gpr_u32(31U), 32020712U, 0U, 0U, 0U);
            } else {
                c.cpu.set_pc(c.cpu.read_gpr_u32(31U));
            }
            return true;
        }
        if (address == 727040U) {
            busy_box = false;
            configuration_message_sent = true;
            ui_call(c, 3200352U, ui_return, 32047680U, page_message, 0U, 0U);
            return true;
        }
        if (address == 727056U) {
            busy_box = true;
            ui_call(c, 3329024U, ui_return, 32020712U, 2U, 726016U, localized(c.memory, 446U));
            return true;
        }
        if (address != 3448320U) {
            return false;
        }
        update();
        const auto ret = c.cpu.read_gpr_u32(31U);
        if (configuration_open && !configuration_message_sent &&
            c.memory.read_u32_le(32048164U) == 5U && !pad(c.memory, 92U)) {
            if (error_shown || !pad(c.memory, 68U)) {
                if (state == State::Online) {
                    ui_return = ret;
                    page_message = 1U;
                } else {
                    client.disconnect();
                    peers.reset();
                    state = State::Idle;
                    presence_requested = false;
                    ui_return = ret;
                    page_message = 3U;
                }
                ui_call(c, 3323184U, 727040U, 32020712U, 0U, 0U, 0U);
                return true;
            }
        }
        if (state == State::Failed && !error_shown) {
            error_shown = true;
            ui_return = ret;
            write_wide(c.memory, 726016U,
                       "Unable to connect to online services.\nCheck your internet connection and try again.");
            ui_call(c, 3323184U, 727056U, 32020712U, 0U, 0U, 0U);
            return true;
        }
        c.cpu.set_pc(ret);
        return true;
    }

    std::string room_body(const content::JsonValue& value) const {
        const auto id = number(value, "id");
        const auto* const players = value.find("players");
        const auto count = number(value, "count");
        if (count > 8U || number(value, "capacity") > 8U) {
            throw std::runtime_error("native room exceeds original roster capacity");
        }
        const auto system_flags =
            (number(value, "system_flags") & 0xFFF7FFFFU) | (value.find("started")->as_bool() ? 0x80000U : 0U);
        std::string body =
            "IDENT=" + std::to_string(id) + "\nNAME=" + text(value, "name") + "\nPARAMS=" +
            text(value, "parameters") + "\nROOM=1\nMINSIZE=2\nMAXSIZE=" +
            std::to_string(number(value, "capacity")) + "\nCOUNT=" + std::to_string(count) +
            "\nSYSFLAGS=" + std::to_string(system_flags) + "\nCUSTFLAGS=0\nNUMPART=0\nSEED=" +
            std::to_string(id) + "\nWHEN=" + (value.find("started")->as_bool() ? "1" : "0") + "\nSELF=" +
            config.player_name.substr(0U, 15U) + "\nHOST=" +
            text(value, "host_name").substr(0U, 15U) + "\n";
        if (const auto* const key = value.find("key"); key != nullptr) {
            body += "AUTH=" + key->as_string() + "\nSESS=" + key->as_string() + "\n";
        }
        if (players != nullptr) {
            if (!players->is_array() || players->as_array().size() != count) {
                throw std::runtime_error("invalid native room roster");
            }
            unsigned i = 0U;
            for (const auto& player : players->as_array()) {
                const auto player_id = number(player, "id");
                const auto name = text(player, "name").substr(0U, 15U);
                const auto suffix = std::to_string(i++);
                const auto address = virtual_address_text(player_id);
                body += "OPID" + suffix + "=" + std::to_string(player_id) + "\nOPPO" + suffix + "=" +
                        name + "\nADDR" + suffix + "=" + address + "\nLADDR" + suffix + "=" +
                        address + "\nOPPARAM" + suffix + "=" + text(player, "parameters") +
                        "\nOPPART" + suffix + "=0\nOPFLAG" + suffix + "=0\n";
            }
        }
        return body;
    }

    void request(Ref& ref, std::uint32_t command, net::MatchRequest request) {
        if (pending.size() > 31U || next_request == 0U) {
            reply(ref, command, "ERROR=Too many pending requests\n", 0x62757379U);
            return;
        }
        request.request_id = next_request++;
        if (!client.request(request)) {
            reply(ref, command, "ERROR=Native service unavailable\n", 0x6661696CU);
            return;
        }
        pending.emplace(request.request_id,
                        Pending{static_cast<std::size_t>(&ref - refs.data()), command,
                                std::chrono::steady_clock::now() + std::chrono::seconds(10)});
    }
    void receive_readiness(const net::PeerMessage& message) {
        const auto& bytes = message.bytes;
        if (bytes.size() != readiness_roster.size() * 4U + 6U || bytes.size() <= 9U ||
            !std::equal(bytes.begin(), bytes.begin() + 4, "B3RD") || bytes[4] > 1U ||
            bytes[5] != readiness_roster.size() ||
            !std::binary_search(readiness_roster.begin(), readiness_roster.end(),
                                message.sender)) {
            return;
        }
        for (std::size_t i = 0U; i < readiness_roster.size(); ++i) {
            const auto p = 6U + i * 4U;
            const unsigned id = (static_cast<std::uint32_t>(bytes[p]) << 24U) |
                                     (static_cast<std::uint32_t>(bytes[p + 1U]) << 16U) |
                                     (static_cast<std::uint32_t>(bytes[p + 2U]) << 8U) |
                                     static_cast<std::uint32_t>(bytes[p + 3U]);
            if (id != readiness_roster[i]) {
                return;
            }
        }
        if (bytes[4] != 0U) {
            readiness_reports[message.sender] = std::chrono::steady_clock::now();
        } else {
            readiness_reports.erase(message.sender);
        }
    }

    void update() {
        for (const auto& event : client.poll()) {
            if (discarded_session) {
                break;
            }
            if (retired_rejoin_request != 0U &&
                number(event.payload, "request_id") == retired_rejoin_request) {
                retired_rejoin_request = 0U;
                continue;
            }
            switch (event.kind) {
            case net::MatchEvent::Kind::BrowserSignIn: {
                const auto url = text(event.payload, "url");
                if (!std::string_view(url).starts_with("https://uwucammie.com/burn3/account/?device=")) {
                    state = State::Failed;
                    client.disconnect();
                    continue;
                }
                auto* const owned = new std::string(url);
                if (!SDL_RunOnMainThread(
                        [](void* data) {
                            std::unique_ptr<std::string> target(static_cast<std::string*>(data));
                            if (!SDL_OpenURL(target->c_str())) {
                                std::clog << "Native online: browser could not be opened\n";
                            }
                        },
                        owned, false)) {
                    delete owned;
                    state = State::Failed;
                    client.disconnect();
                }
                break;
            }
            case net::MatchEvent::Kind::Connected: {
                if (const auto* const name = event.payload.find("name");
                    name != nullptr && name->is_string()) {
                    config.player_name = name->as_string();
                }
                player = static_cast<std::uint32_t>(event.payload.find("id")->as_number());
                peers = std::make_unique<net::PeerTransport>();
                const auto admission = net::decode_peer_admission(event.payload);
                server_address = admission.rendezvous.address;
                peers->admit(admission);
                state = State::Online;
                if (const auto* const applied = event.payload.find("applied_mods"); applied != nullptr) {
                    applied_mod_identity = applied->as_string();
                }
                if (const auto* const value = event.payload.find("lobby"); value != nullptr) {
                    lobby = *value;
                }
                if (lobby.has_value()) {
                    if (const auto* const players = lobby->find("players"); players != nullptr) {
                        update_lobby_users(*players);
                    }
                }
                if (rejoin_stage == RejoinStage::Connecting && blocked_join.has_value()) {
                    auto& ref = refs[blocked_join->ref];
                    if (ref.used && ref.connected && ref.persona != 0U) {
                        publish_self(ref, 0U);
                        publish_lobby(ref);
                    }
                }
                std::clog << "Native online: admitted player " << player << '\n';
                break;
            }
            case net::MatchEvent::Kind::Disconnected: {
                state = State::Failed;
                presence_requested = false;
                peers.reset();
                room.reset();
                lobby.reset();
                lobby_users.clear();
                datagrams.clear();
                readiness_roster.clear();
                readiness_reports.clear();
                text_keys.clear();
                if (rejoin_stage != RejoinStage::None) {
                    fail_rejoin("Online reconnection failed. Choose the lobby again.");
                }
                const auto interrupted = std::move(buddy_requests);
                clear_buddies();
                for (const auto& [request_id, operation] : interrupted) {
                    if (operation.callback != 0U) {
                        buddy_callbacks.push_back(BuddyCallback{operation.callback, operation.context, false});
                    }
                }
                if (news_request != 0U) {
                    news_request = 0U;
                    news_failed = true;
                }
                std::clog << "Native online: connection failed: " << event.detail << '\n';
                break;
            }
            case net::MatchEvent::Kind::Heartbeat: {
                const auto* const players = event.payload.find("lobby_players");
                const bool changed = players != nullptr && update_lobby_users(*players);
                for (auto& ref : refs) {
                    if (ref.used && ref.connected) {
                        reply(ref, 0x40746963U, std::string{}, 0U);
                        if (changed && ref.persona != 0U) {
                            publish_self(ref, room.has_value() ? number(*room, "id") : 0U);
                        }
                    }
                }
                break;
            }
            case net::MatchEvent::Kind::Chat: {
                const auto body = "N=" + encode_tag_string(text(event.payload, "name")) + "\nT=" +
                                  encode_tag_string(text(event.payload, "text")) + "\nF=" +
                                  text(event.payload, "attributes") + "\nU=\n";
                for (auto& ref : refs) {
                    if (ref.used && ref.connected && ref.persona != 0U) {
                        reply(ref, 0x2B6D7367U, body, 0U);
                    }
                }
                break;
            }
            case net::MatchEvent::Kind::RoomChanged: {
                const auto* const value = event.payload.find("room");
                if (value == nullptr) {
                    throw std::runtime_error("missing native room");
                }
                if (rejoin_stage == RejoinStage::Admitted && blocked_join.has_value() &&
                    (number(*value, "id") != blocked_join->room ||
                     number(*value, "host") != blocked_join->host ||
                     text(*value, "mods") != blocked_join->digest ||
                     text(*value, "name") != blocked_join->name)) {
                    fail_rejoin("The host changed during admission. Choose the lobby again.");
                    break;
                }
                peers->set_room(net::decode_peer_room(*value));
                std::vector<std::uint32_t> members;
                for (const auto& member : value->find("players")->as_array()) {
                    members.push_back(number(member, "id"));
                }
                std::sort(members.begin(), members.end());
                if (members != readiness_roster) {
                    readiness_roster = std::move(members);
                    readiness_reports.clear();
                    readiness_advertisement = {};
                }
                const bool was_started = room.has_value() && number(*room, "id") == number(*value, "id") &&
                                         room->find("started")->as_bool();
                room = *value;
                const auto body = room_body(*room);
                for (auto& ref : refs) {
                    if (ref.used && ref.connected && ref.persona != 0U) {
                        publish_self(ref, number(*room, "id"));
                        reply(ref, 0x2B61676DU, body, 0U);
                        reply(ref, 0x2B6D676DU, body, 0U);
                        if (room->find("started")->as_bool() && !was_started) {
                            reply(ref, 0x2B736573U, body, 0U);
                        }
                    }
                }
                break;
            }
            case net::MatchEvent::Kind::RoomClosed:
            case net::MatchEvent::Kind::Left: {
                if (room.has_value()) {
                    for (auto& ref : refs) {
                        if (ref.used && ref.connected && ref.persona != 0U) {
                            reply(ref, 0x2B61676DU,
                                  "IDENT=" + std::to_string(number(*room, "id")) + "\n", 0U);
                            publish_self(ref, 0U);
                        }
                    }
                }
                room.reset();
                readiness_roster.clear();
                readiness_reports.clear();
                datagrams.clear();
                if (peers) {
                    peers->leave();
                }
                break;
            }
            default:
                break;
            }
        if (event.payload.find("request_id") == nullptr) {
            continue;
        }
        const auto request_id = number(event.payload, "request_id");
        if (rejoin_list_request == request_id && rejoin_stage == RejoinStage::Listing &&
            blocked_join.has_value()) {
            rejoin_list_request = 0U;
            const auto* const rooms = event.payload.find("rooms");
            const content::JsonValue* target = nullptr;
            if (event.kind == net::MatchEvent::Kind::Rooms && rooms != nullptr &&
                rooms->is_array() && rooms->as_array().size() <= 12U) {
                for (const auto& item : rooms->as_array()) {
                    if (number(item, "id") == blocked_join->room) {
                        target = &item;
                    }
                }
            }
            if (target == nullptr) {
                fail_rejoin("The host lobby is no longer available. Choose a lobby again.");
                continue;
            }
            if (text(*target, "mods") != blocked_join->digest ||
                applied_mod_identity != blocked_join->digest ||
                number(*target, "host") != blocked_join->host ||
                text(*target, "host_name") != blocked_join->host_name ||
                text(*target, "name") != blocked_join->name) {
                fail_rejoin("The host or its mods changed. Choose the lobby again to review them.");
                continue;
            }
            if (target->find("started") == nullptr || !target->find("started")->is_bool() ||
                target->find("started")->as_bool() ||
                number(*target, "count") >= number(*target, "capacity")) {
                fail_rejoin("The lobby is full or its race has started. Choose a lobby again.");
                continue;
            }
            discovered_rooms[blocked_join->name] = blocked_join->room;
            discovered_room_details[blocked_join->room] = *target;
            rejoin_stage = rejoin_prepare_only ? RejoinStage::Failed : RejoinStage::Joining;
            continue;
        }
        if (const auto found = buddy_requests.find(request_id); found != buddy_requests.end()) {
            const auto operation = found->second;
            buddy_requests.erase(found);
            const auto* const friends = event.payload.find("friends");
            const bool succeeded = event.kind == net::MatchEvent::Kind::Service && friends != nullptr &&
                                   friends->is_array() && friends->as_array().size() <= 100U &&
                                   ((buddies = friends->as_array()), (buddies_changed = true));
            if (operation.callback != 0U) {
                buddy_callbacks.push_back(BuddyCallback{operation.callback, operation.context, succeeded});
            }
            continue;
        }
        if (news_request == request_id) {
            news_request = 0U;
            news_failed = event.kind != net::MatchEvent::Kind::Service;
            if (!news_failed) {
                news_body = text(event.payload, "body").substr(0U, 7000U);
            }
            continue;
        }
        const auto found = pending.find(request_id);
        if (found == pending.end()) {
            continue;
        }
        const auto operation = found->second;
        pending.erase(found);
        auto& ref = refs[operation.ref];
        if (!ref.used || !ref.connected) {
            continue;
        }
        if (event.kind == net::MatchEvent::Kind::Rejected) {
            reply(ref, operation.command, "ERROR=" + text(event.payload, "error") + "\n", 0x6661696CU);
            if (rejoin_join_request == request_id) {
                fail_rejoin("The lobby could not be rejoined: " + text(event.payload, "error"));
            }
        } else if (event.kind == net::MatchEvent::Kind::Service) {
            reply(ref, operation.command, text(event.payload, "body"), number(event.payload, "code"));
            if (const auto* const packets = event.payload.find("packets"); packets != nullptr) {
                if (!packets->is_array() || packets->as_array().size() > 100U ||
                    ref.replies.size() + packets->as_array().size() > 512U) {
                    throw std::runtime_error("invalid native service packet count");
                }
                for (const auto& packet : packets->as_array()) {
                    const auto kind = text(packet, "kind");
                    if (kind != "+snp" || operation.command != 0x736E6170U) {
                        throw std::runtime_error("invalid native service packet kind");
                    }
                    reply(ref, packed_tag(kind), text(packet, "body"), number(packet, "code"));
                }
            }
        } else if (event.kind == net::MatchEvent::Kind::Rooms) {
            const auto* const rooms = event.payload.find("rooms");
            if (rooms == nullptr || !rooms->is_array() || rooms->as_array().size() > 12U) {
                throw std::runtime_error("invalid native search results");
            }
            discovered_rooms.clear();
            discovered_room_details.clear();
            unsigned count = 0U;
            for (const auto& item : rooms->as_array()) {
                if (text(item, "mode") != "original") {
                    continue;
                }
                discovered_rooms.emplace(text(item, "name"), number(item, "id"));
                discovered_room_details.emplace(number(item, "id"), item);
                reply(ref, 0x2B67616DU, room_body(item), 0U);
                ++count;
            }
            reply(ref, operation.command, "COUNT=" + std::to_string(count) + "\n", 0U);
        } else if (operation.command == 0x67637265U || operation.command == 0x676A6F69U) {
            if (!room.has_value()) {
                throw std::runtime_error("native room admission is missing");
            }
            reply(ref, operation.command, room_body(*room), 0U);
            if (rejoin_join_request == request_id) {
                rejoin_stage = RejoinStage::None;
                rejoin_join_request = 0U;
                blocked_join.reset();
                mod_mismatch_pending = false;
            }
            } else {
                reply(ref, operation.command, std::string{}, 0U);
            }
        }
        if (peers) {
            peers->update();
        }
        if (peers) {
            for (auto& message : peers->poll()) {
                if (message.channel == net::PeerChannel::Reliable) {
                    receive_readiness(message);
                    continue;
                }
                if (message.channel != net::PeerChannel::Datagram || message.bytes.size() <= 4U ||
                    message.bytes.size() > 1028U || game_port == 0U) {
                    continue;
                }
                const unsigned port = (static_cast<unsigned>(message.bytes[2]) << 8U) | message.bytes[3];
                if (port != game_port) {
                    continue;
                }
                if (datagrams.size() < 128U) {
                    datagrams.push_back(std::move(message));
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (rejoin_stage != RejoinStage::None && now >= rejoin_deadline) {
            fail_rejoin("Rejoining the lobby timed out. Choose the lobby again.");
        }
        if (rejoin_stage == RejoinStage::Connecting && state == State::Online && blocked_join.has_value()) {
            if (applied_mod_identity != blocked_join->digest) {
                fail_rejoin("Applied mods no longer match the host. Refresh Mods and choose the lobby again.");
            } else {
                net::MatchRequest search;
                search.mode = net::OnlineMode::Original;
                search.request_id = next_request++;
                rejoin_list_request = search.request_id;
                rejoin_stage = RejoinStage::Listing;
                if (!client.request(search)) {
                    fail_rejoin("The online lobby list could not be refreshed.");
                }
            }
        }
        std::erase_if(buddy_requests, [this, &now](const auto& item) {
            if (state != State::Failed && now < item.second.deadline) {
                return false;
            }
            if (item.second.callback != 0U) {
                buddy_callbacks.push_back(BuddyCallback{item.second.callback, item.second.context, false});
            }
            return true;
        });
        bool rejoin_timed_out = false;
        std::erase_if(pending, [this, &now, &rejoin_timed_out](const auto& item) {
            if (state != State::Failed && now < item.second.deadline) {
                return false;
            }
            rejoin_timed_out = rejoin_timed_out || rejoin_join_request == item.first;
            auto& ref = refs[item.second.ref];
            if (ref.used && ref.connected) {
                reply(ref, item.second.command, "ERROR=Native service timed out\n", 0x74696D65U);
            }
            return true;
        });
        if (rejoin_timed_out) {
            fail_rejoin("Rejoining the lobby timed out. Choose the lobby again.");
        }
    }

    void reply(Ref& ref, std::uint32_t kind, std::string text, std::uint32_t code) {
        if (text.size() > 8191U || ref.replies.size() > 511U) {
            throw std::runtime_error("native online reply queue overflow");
        }
        ref.replies.push_back({kind, code, std::move(text)});
    }

    void publish_lobby(Ref& ref) {
        if (!lobby.has_value()) {
            return;
        }
        reply(ref, 0x2B726F6DU,
              "I=" + std::to_string(number(*lobby, "id")) + "\nN=" + text(*lobby, "name") + "\nD=" +
                  text(*lobby, "name") + "\nF=C\nC=0\nL=" + std::to_string(number(*lobby, "limit")) +
                  "\n",
              0U);
    }

    std::uint32_t self_address() const {
        return player;
    }

    void publish_self(Ref& ref, std::uint32_t game) {
        reply(ref, 0x2B77686FU,
              "I=" + std::to_string(player) + "\nN=" + encode_tag_string(config.player_name) + "\nA=" +
                  virtual_address_text(self_address()) + "\nF=\nG=" + std::to_string(game) + "\nRI=" +
                  std::to_string(lobby.has_value() ? number(*lobby, "id") : 0U) + "\nRM=" +
                  encode_tag_string(lobby.has_value() ? text(*lobby, "name") : std::string("")) +
                  "\nRT=" + std::to_string(lobby_users.size()) + "\nRF=C\n",
              0U);
        if (game != 0U && room.has_value() && game == number(*room, "id")) {
            if (const auto* const players = room->find("players"); players != nullptr && players->is_array()) {
                for (const auto& member : players->as_array()) {
                    const auto id = number(member, "id");
                    const auto name = text(member, "name");
                    std::erase_if(lobby_users, [&id, &name](const auto& item) {
                        return item.first != id && item.second.name == name;
                    });
                    lobby_users[id] = LobbyUser{name, game};
                }
            }
        }
        auto users = lobby_users;
        users[player] = LobbyUser{config.player_name, game};
        for (const auto& [id, user] : ref.visible_users) {
            if (users.find(id) == users.end()) {
                reply(ref, 0x2B757372U, "I=" + std::to_string(id) + "\n", 0U);
            }
        }
        for (const auto& [id, user] : users) {
            if (!ref.visible_users.contains(id) || ref.visible_users.at(id) != user) {
                reply(ref, 0x2B757372U,
                      "I=" + std::to_string(id) + "\nN=" + encode_tag_string(user.name) + "\nA=" +
                          virtual_address_text(id) + "\nF=\nG=" + std::to_string(user.game) + "\n",
                      0U);
            }
        }
        ref.visible_users = std::move(users);
    }

    void cancel(Ref& ref) {
        std::erase_if(pending, [this, &ref](const auto& item) { return &refs[item.second.ref] == &ref; });
    }

    void send(Ref& ref, std::uint32_t kind, std::string_view body) {
        const auto data = parse_fields(body);
        if (packet_diagnostic) {
            std::clog << "Native online: request " << tag_text(kind) << " fields=";
            for (const auto& item : data) {
                std::clog << ' ' << item.first;
            }
            std::clog << '\n';
        }
        if (kind == tag("DISC")) {
            cancel(ref);
            ref.persona = false;
            ref.replies.clear();
            ref.connected = false;
            return;
        }
        if (kind == tag("@tic")) {
            reply(ref, kind, std::string{}, 0U);
            return;
        }
        if (kind == tag("@dir")) {
            reply(ref, kind,
                  "ADDR=" + server_address + "\nPORT=" + std::to_string(config.port) + "\nSESS=" +
                      std::to_string(player) + "\nSKEY=native-session\n",
                  0U);
            return;
        }
        if (kind == tag("skey")) {
            reply(ref, kind, "SKEY=native-session\n", 0U);
            return;
        }
        if (kind == tag("news") && data.contains("NAME") && !data.at("NAME").empty() &&
            data.at("NAME").find_first_not_of("0123456789") != std::string::npos) {
            reply(ref, kind, "NEWS_URL=https://uwucammie.com/burn3/news/\nCOUNT=1\n", 0U);
            return;
        }
        if (kind == tag("news") || kind == tag("sviw") || kind == tag("sdta") || kind == tag("cate") ||
            kind == tag("snap") || kind == tag("rank")) {
            net::MatchRequest service;
            service.kind = net::MatchRequest::Kind::Service;
            service.operation = tag_text(kind);
            service.fields = data;
            request(ref, kind, std::move(service));
            return;
        }
        if (kind == tag("mesg")) {
            net::MatchRequest chat;
            chat.kind = net::MatchRequest::Kind::Chat;
            chat.message = data.contains("TEXT") ? data.at("TEXT") : "";
            chat.target = data.contains("PRIV") ? data.at("PRIV") : "";
            chat.attributes = data.contains("ATTR") ? data.at("ATTR") : "";
            request(ref, kind, std::move(chat));
            return;
        }
        if (kind == tag("sele") || kind == tag("addr") || kind == tag("ping")) {
            reply(ref, kind, std::string{}, 0U);
            return;
        }
        if (kind == tag("auth") || kind == tag("acct")) {
            reply(ref, kind,
                  "NAME=" + config.player_name + "\nPERSONAS=" + config.player_name +
                      "\nLOC=enUS\nSPAM=NN\nTOS=1\n",
                  0U);
            return;
        }
        if (kind == tag("pers")) {
            ref.persona = true;
            ref.visible_users.clear();
            reply(ref, kind,
                  "PERS=" + config.player_name + "\nLKEY=native-session\nLOC=enUS\nIDLE=100000\n", 0U);
            publish_self(ref, room.has_value() ? number(*room, "id") : 0U);
            publish_lobby(ref);
            return;
        }
        if (kind == tag("gsea")) {
            net::MatchRequest search;
            search.kind = net::MatchRequest::Kind::List;
            search.mode = net::OnlineMode::Original;
            request(ref, kind, std::move(search));
            return;
        }
        if (kind == tag("gcre")) {
            net::MatchRequest create;
            create.kind = net::MatchRequest::Kind::Create;
            create.mode = net::OnlineMode::Original;
            create.name = data.at("NAME");
            create.password = data.contains("PASS") ? data.at("PASS") : "";
            create.parameters = data.at("PARAMS");
            create.capacity = integer(data, "MAXSIZE");
            create.system_flags = integer(data, "SYSFLAGS");
            request(ref, kind, std::move(create));
            return;
        }
        if (kind == tag("gjoi")) {
            if (room.has_value() && data.contains("NAME") && data.at("NAME") == text(*room, "name") &&
                admitted_rejoin_matches(static_cast<std::size_t>(&ref - refs.data()))) {
                complete_admitted_rejoin();
                reply(ref, kind, room_body(*room), 0U);
                return;
            }
            net::MatchRequest join;
            join.kind = net::MatchRequest::Kind::Join;
            join.mode = net::OnlineMode::Original;
            join.room = integer(data, "IDENT");
            if (join.room == 0U && data.contains("NAME") &&
                discovered_rooms.contains(data.at("NAME"))) {
                join.room = discovered_rooms.at(data.at("NAME"));
            }
            const auto details = discovered_room_details.find(join.room);
            if (details != discovered_room_details.end()) {
                const auto* const mods = details->second.find("mods");
                if (mods != nullptr && mods->is_string() && mods->as_string() != applied_mod_identity) {
                    blocked_join.reset();
                    mod_mismatch_pending = false;
                    if (last_guest_join.has_value() && data.contains("NAME") &&
                        last_guest_join->name == data.at("NAME") &&
                        (data.contains("PASS") ? last_guest_join->password == data.at("PASS")
                                               : last_guest_join->password == "")) {
                        blocked_join = last_guest_join;
                        blocked_join->room = join.room;
                        blocked_join->digest = mods->as_string();
                        blocked_join->host = number(details->second, "host");
                        blocked_join->host_name = text(details->second, "host_name");
                        blocked_join->ref = static_cast<std::size_t>(&ref - refs.data());
                        mod_mismatch_pending = true;
                        if (config.mod_sync_required) {
                            config.mod_sync_required(details->second);
                        }
                        reply(ref, kind,
                              "ERROR=Host mods differ. Open Mods and Refresh to match this host before joining.\n",
                              0x6661696CU);
                        return;
                    }
                }
            }
            join.password = data.contains("PASS") ? data.at("PASS") : "";
            if (data.contains("USERPARAMS")) {
                join.user_parameters = data.at("USERPARAMS");
            }
            rejoin_join_request = join.room;
            request(ref, kind, std::move(join));
            return;
        }
        if (kind == tag("glea") || kind == tag("gdel")) {
            net::MatchRequest leave;
            leave.kind = net::MatchRequest::Kind::Leave;
            request(ref, kind, std::move(leave));
            return;
        }
        if (kind == tag("gset") && data.contains("KICK") && room.has_value() &&
            player == number(*room, "host") && data.contains("NAME") &&
            data.at("NAME") == text(*room, "name") && !data.at("KICK").empty() &&
            data.at("KICK").size() <= 15U) {
            unsigned matches = 0U;
            std::uint32_t target = 0U;
            for (const auto& member : room->find("players")->as_array()) {
                if (text(member, "name") == data.at("KICK")) {
                    target = number(member, "id");
                    ++matches;
                }
            }
            if (matches != 1U || target == 0U || player == target) {
                reply(ref, kind, "ERROR=The selected player is no longer in this host lobby\n",
                      0x6661696CU);
                return;
            }
            net::MatchRequest kick;
            kick.kind = net::MatchRequest::Kind::Update;
            kick.operation = "kick";
            kick.room = target;
            request(ref, kind, std::move(kick));
            return;
        }
        if (kind == tag("gset")) {
            net::MatchRequest change;
            change.kind = net::MatchRequest::Kind::Update;
            if (data.contains("PARAMS")) {
                change.parameters = data.at("PARAMS");
            }
            if (data.contains("USERPARAMS")) {
                change.user_parameters = data.at("USERPARAMS");
            }
            if (data.contains("SYSFLAGS")) {
                change.updated_system_flags = integer(data, "SYSFLAGS");
            }
            request(ref, kind, std::move(change));
            return;
        }
        if (kind == tag("gsta") || kind == tag("gfin")) {
            net::MatchRequest start;
            start.kind = kind == tag("gfin") ? net::MatchRequest::Kind::Finish
                                              : net::MatchRequest::Kind::Start;
            request(ref, kind, std::move(start));
            return;
        }
        if (kind == tag("gget") && room.has_value()) {
            reply(ref, kind, room_body(*room), 0U);
            return;
        }
        if (kind == tag("room")) {
            reply(ref, kind, room.has_value() ? "COUNT=1\n" : "COUNT=0\n", 0U);
            publish_lobby(ref);
            return;
        }
        if (kind == tag("move") && lobby.has_value()) {
            reply(ref, kind,
                  "IDENT=" + std::to_string(number(*lobby, "id")) + "\nNAME=" + text(*lobby, "name") +
                      "\nCOUNT=0\nFLAGS=C\nLIMIT=" + std::to_string(number(*lobby, "limit")) + "\n",
                  0U);
            return;
        }
        reply(ref, kind, "ERROR=Unsupported native online operation\n", 0x756E696DU);
    }
    bool peers_ready(guest::EeMemory& m) {
        bool ready = false;
        if (!room.has_value() || !peers || game_port == 0U || readiness_roster.empty()) {
            return false;
        }
        const auto count = m.read_u32_le(31664904U);
        std::vector<std::uint32_t> initialized{player};
        ready = m.read_u32_le(31664984U) == player && count == readiness_roster.size() && count <= 8U;
        if (ready) {
            for (std::uint32_t i = 0U; i < count; ++i) {
                const Address slot = m.read_u32_le(31664872U + i * 4U);
                if (slot == 0U) {
                    continue;
                }
                if (!ram(m, slot, 1268U) || m.read_u32_le(slot + 1264U) != 2U) {
                    ready = false;
                    break;
                }
                const auto id = m.read_u32_le(slot + 8U);
                initialized.push_back(id);
                ready = ready && peers->peer_connected(id);
            }
        }
        std::sort(initialized.begin(), initialized.end());
        ready = ready && initialized == readiness_roster;
        const auto now = std::chrono::steady_clock::now();
        if (advertised_ready != ready || readiness_advertisement <= now) {
            std::vector<std::uint8_t> bytes{'B', '3', 'R', 'D', static_cast<std::uint8_t>(ready),
                                            static_cast<std::uint8_t>(readiness_roster.size())};
            for (const auto id : readiness_roster) {
                for (const unsigned shift : {24U, 16U, 8U, 0U}) {
                    bytes.push_back(static_cast<std::uint8_t>(id >> shift));
                }
            }
            for (const auto id : readiness_roster) {
                if (id != player) {
                    (void)peers->send(id, bytes, net::PeerChannel::Reliable);
                }
            }
            advertised_ready = ready;
            readiness_advertisement = now + std::chrono::seconds(1);
        }
        return ready && std::all_of(readiness_roster.begin(), readiness_roster.end(),
                                    [this, &now](const std::uint32_t id) {
                                        const auto report = readiness_reports.find(id);
                                        return id == player ||
                                               (report != readiness_reports.end() &&
                                                now - report->second < std::chrono::seconds(5) &&
                                                peers->peer_connected(id));
                                    });
    }

    std::int64_t call(Address address, NativeFunctionContext& c) {
        if (rejoin_stage == RejoinStage::Admitted && !rejoin_dispatch_context_valid(c.memory)) {
            fail_rejoin("The original online page changed. Choose the lobby again.");
        }
        update();
        publish_presence(c.memory);
        const bool ready = peers_ready(c.memory);
        auto& m = c.memory;
        const auto a0 = c.cpu.read_gpr_u32(4U);
        const auto a1 = c.cpu.read_gpr_u32(5U);
        const auto a2 = c.cpu.read_gpr_u32(6U);
        const auto a3 = c.cpu.read_gpr_u32(7U);
        const auto f = c.cpu.read_gpr_u32(8U);
        if (address == 3366752U || address == 3366848U || address == 3366864U || address == 3367248U) {
            if (a0 != 31706436U) {
                return -1;
            }
            if (address == 3366752U) {
                news_request = 0U;
                news_body.reset();
                news_failed = false;
                return 0;
            }
            if (address == 3367248U) {
                news_request = 0U;
                news_body.reset();
                news_failed = true;
                if (state == State::Online && next_request != 0U) {
                    net::MatchRequest request;
                    request.kind = net::MatchRequest::Kind::Service;
                    request.operation = "news";
                    request.fields = {{"NAME", "0"}};
                    request.request_id = next_request++;
                    if (client.request(request)) {
                        news_request = request.request_id;
                        news_failed = false;
                        news_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    }
                }
                return 0;
            }
            if (address == 3366848U) {
                if (news_body.has_value()) {
                    write_text(m, 778240U, *news_body);
                    return 778240;
                }
                return 0;
            }
            if (news_request != 0U && std::chrono::steady_clock::now() >= news_deadline) {
                news_request = 0U;
                news_failed = true;
            }
            if (news_failed) {
                return -1;
            }
            if (news_body.has_value()) {
                return static_cast<std::int64_t>(news_body->size() + 1U);
            }
            return 0;
        }
        if (address == 2339712U) {
            if (!ram(m, a0, 1268U) || !room.has_value() || !peers) {
                return 0;
            }
            const auto id = m.read_u32_le(a0 + 8U);
            return ready && player != id &&
                   std::binary_search(readiness_roster.begin(), readiness_roster.end(), id);
        }
        if (address == 3541408U) {
            if (!buddy_initialized) {
                for (std::uint32_t offset = 0U; offset != 280U; ++offset) {
                    m.write_u8(a0 + offset, 0U);
                }
                m.write_u32_le(a0 + 280U, 0U);
                m.write_u8(a0 + 290U, static_cast<std::uint8_t>(c.cpu.read_gpr_u32(9U)));
                buddy_initialized = true;
                publish_presence(m);
            }
            return 1;
        }
        if (address == 3541232U || address == 3540928U) {
            presence_requested = false;
            if (address == 3541232U) {
                buddy_initialized = false;
                clear_buddies();
            }
            publish_presence(m);
            return 0;
        }
        if (address == 3540704U) {
            if (a1 > 6U) {
                return 5;
            }
            m.write_u32_le(a0 + 208U, a1);
            return state == State::Online ? 0 : 5;
        }
        if (address == 3535104U) {
            for (std::uint32_t offset = 0U; offset != 280U; ++offset) {
                m.write_u8(a0 + offset, 0U);
            }
            return 1;
        }
        if (address == 3535248U || address == 3535328U) {
            const auto* const entry = buddy(read_text(m, a1, 33U));
            if (address == 3535248U) {
                m.write_u32_le(a2, 0U);
            } else {
                m.write_u8(a2, static_cast<std::uint8_t>(entry != nullptr &&
                                                         (number(*entry, "flags") & 8192U) != 0U));
            }
            return entry != nullptr ? 0 : 5;
        }
        if (address == 3535424U) {
            const auto left = read_text(m, a2, 33U);
            const auto right = read_text(m, a3, 33U);
            const auto rank = [this](std::string_view name) -> unsigned {
                const auto* const entry = buddy(name);
                if (entry == nullptr) {
                    return 6U;
                }
                const auto flags = number(*entry, "flags");
                if ((flags & 512U) != 0U) {
                    return 1U;
                }
                if ((flags & 262148U) != 0U) {
                    return 5U;
                }
                if ((flags & 524288U) != 0U) {
                    return 2U;
                }
                if (number(*entry, "status") == 0U) {
                    return 3U;
                }
                return 4U;
            };
            if (rank(left) == rank(right)) {
                return left.compare(right);
            }
            return static_cast<std::int64_t>(rank(left)) - static_cast<std::int64_t>(rank(right));
        }
        if (address == 3540688U) {
            return static_cast<std::int64_t>(buddies.size());
        }
        if (address == 3536640U || address == 4298472U) {
            return 0;
        }
        if (address == 3536496U) {
            const unsigned mask = 262144U;
            return std::any_of(buddies.begin(), buddies.end(), [mask](const content::JsonValue& item) {
                return (number(item, "flags") & mask) != 0U;
            });
        }
        if (address == 3540512U) {
            write_text(m, a2, config.player_name.substr(0U, 15U));
            return 0;
        }
        if (address == 3540592U) {
            if (a1 >= buddies.size()) {
                return 5;
            }
            write_text(m, a2, text(buddies[a1], "name").substr(0U, 15U));
            return 0;
        }
        if (address == 3540320U || address == 3540064U || address == 3540160U || address == 3537152U ||
            address == 3537264U || address == 3537376U || address == 3537472U || address == 3537568U ||
            address == 3537664U || address == 3537760U) {
            const auto* const entry = buddy(read_text(m, a1, 33U));
            if (entry == nullptr) {
                if (address == 3540320U) {
                    m.write_u32_le(a2, 0U);
                } else {
                    m.write_u8(a2, 0U);
                }
                return 5;
            }
            const auto flags = number(*entry, "flags");
            if (address == 3540320U) {
                m.write_u32_le(a2, number(*entry, "status"));
                return 0;
            }
            if (address == 3540160U) {
                write_text(m, a2, text(*entry, "name"));
                return 0;
            }
            const unsigned mask = address == 3540064U   ? 512U
                                  : address == 3537152U ? 131072U
                                  : address == 3537264U ? 4194304U
                                  : address == 3537376U ? 33554432U
                                  : address == 3537472U ? 524288U
                                  : address == 3537568U ? 262144U
                                  : address == 3537664U ? 4U
                                                        : 786948U;
            m.write_u8(a2, static_cast<std::uint8_t>((mask & flags) != 0U));
            return 0;
        }
        if (address == 3538976U || address == 3539152U || address == 3536400U || address == 3538672U ||
            address == 3538304U || address == 3536080U || address == 3536160U || address == 3536240U ||
            address == 3536320U) {
            auto name = read_text(m, a1, 33U);
            if (const auto* const entry = buddy(name); entry != nullptr) {
                name = text(*entry, "name");
            }
            const char* const action = address == 3539152U   ? "add"
                                       : address == 3536320U ? "accept"
                                       : address == 3536240U ? "decline"
                                       : address == 3536080U ? "block"
                                       : address == 3538672U ? "unblock"
                                                             : "remove";
            return request_buddies(action, name, a2, a3) ? 0 : 5;
        }
        if (address == 2288448U || address == 2288640U || address == 2288736U || address == 2289120U) {
            if (packet_diagnostic && socket_diagnostics++ < 16U) {
                std::clog << "Native online: game socket call=" << address << " object=" << a0
                          << " args=" << a1 << ',' << a2 << ',' << a3 << ',' << f << '\n';
            }
            if (a0 != 31704608U) {
                return 0;
            }
            if (address == 2288448U) {
                if (a1 - 1U > 65534U || !peers) {
                    return 0;
                }
                game_port = static_cast<std::uint16_t>(a1);
                if (packet_diagnostic) {
                    std::clog << "Native online: game socket port=" << a1 << '\n';
                }
                datagrams.clear();
                m.write_u32_le(31706404U, a1);
                return 0;
            }
            if (address == 2288640U) {
                if (packet_diagnostic) {
                    std::clog << "Native online: game socket closed caller=" << c.cpu.read_gpr_u32(31U)
                              << " tick="
                              << static_cast<std::uint32_t>(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - epoch)
                                         .count())
                              << '\n';
                }
                game_port = 0U;
                datagrams.clear();
                return 0;
            }
            if (address == 2289120U) {
                if (packet_diagnostic && send_diagnostics++ < 128U) {
                    std::clog << "Native online: datagram send bytes=" << a2 << " address=" << a3
                              << " port=" << f << " bound=" << static_cast<unsigned long>(game_port)
                              << " type="
                              << (a2 > 3U && ram(m, a1, a2) ? static_cast<unsigned>(m.read_u8(a1 + 3U)) : 0U)
                              << '\n';
                }
                if (game_port == 0U || !peers || !room.has_value() || a2 - 1U > 1023U || f == 0U ||
                    f > 65535U || !ram(m, a1, a2)) {
                    return 0;
                }
                for (const auto& member : room->find("players")->as_array()) {
                    const auto id = number(member, "id");
                    if (id == player || (id & 65535U) + 184418304U != a3) {
                        continue;
                    }
                    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(game_port >> 8U),
                                                    static_cast<std::uint8_t>(game_port),
                                                    static_cast<std::uint8_t>(f >> 8U),
                                                    static_cast<std::uint8_t>(f)};
                    for (std::uint32_t i = 0U; i < a2; ++i) {
                        bytes.push_back(m.read_u8(a1 + i));
                    }
                    return peers->send(id, bytes, net::PeerChannel::Datagram) ? 1 : 0;
                }
                return 0;
            }
            if (game_port == 0U || datagrams.empty() || !ram(m, a1, 4U) || !ram(m, a2, 4U) ||
                !ram(m, a3, 4U)) {
                return 0;
            }
            auto message = std::move(datagrams.front());
            datagrams.pop_front();
            if (packet_diagnostic && receive_diagnostics++ < 128U) {
                std::clog << "Native online: datagram receive bytes=" << message.bytes.size() - 4U
                          << " sender=" << message.sender << " type="
                          << (message.bytes.size() > 7U ? static_cast<unsigned>(message.bytes[7]) : 0U)
                          << '\n';
            }
            for (std::size_t i = 4U; i < message.bytes.size(); ++i) {
                m.write_u8(static_cast<Address>(i) + 774140U, message.bytes[i]);
            }
            m.write_u32_le(a1, 774144U);
            m.write_u32_le(a2, (message.sender & 65535U) + 184418304U);
            m.write_u32_le(a3, (static_cast<unsigned>(message.bytes[0]) << 8U) | message.bytes[1]);
            return static_cast<std::int64_t>(message.bytes.size() - 4U);
        }
        if (address == 3540992U) {
            if (a0 != 31708048U) {
                return -1;
            }
            connect();
            presence_requested = true;
            buddy_initialized = true;
            publish_presence(m);
            return state == State::Online ? 0 : -1;
        }
        if (address == 2893312U || address == 2893520U) {
            return 0;
        }
        if (address == 2544288U) {
            if (a0 == 31664864U && state == State::Online && peers && peers->rendezvous_registered()) {
                return 9;
            }
            return 0;
        }
        if (address == 2295296U) {
            if (a0 != 31666112U || state != State::Online || !peers || !peers->rendezvous_registered()) {
                return 0;
            }
            game_port = 3658U;
            m.write_u32_le(31667536U, 3658U);
            m.write_u32_le(31706404U, 3658U);
            m.write_u32_le(31667524U, 9U);
            return 1;
        }
        if (address == 2290208U) {
            if (a0 != 31704608U) {
                return 0;
            }
            game_port = 0U;
            datagrams.clear();
            m.write_u32_le(31706404U, 4294967295U);
            m.write_u32_le(31704780U, 4294967295U);
            m.write_u32_le(31704768U, 4294967295U);
            m.write_u32_le(31704628U, 0U);
            m.write_u32_le(31704632U, 0U);
            m.write_u32_le(31704680U, 1U);
            m.write_u8(31704684U, 1U);
            m.write_u8(31704672U, 0U);
            m.write_u8(31704687U, 0U);
            return 0;
        }
        if (address == 2289664U || address == 2289696U || address == 2290464U) {
            if (a0 != 31704608U) {
                return 3;
            }
            if (address == 2289664U) {
                connect();
                if (state == State::Online) {
                    write_text(m, 31704636U, virtual_address_text(player));
                }
            }
            if (static_cast<unsigned>(state) - 1U <= 2U) {
                return static_cast<std::int64_t>(state);
            }
            return 0;
        }
        if (address == 4202744U) {
            open = true;
            return 0;
        }
        if (address == 4202968U) {
            if (a1 != 0U && a2 != 0U) {
                if (a2 > 256U || !ram(m, a1, 256U)) {
                    return -1;
                }
                for (std::size_t i = 0U; i != 256U; ++i) {
                    m.write_u8(a1 + static_cast<Address>(i), 0U);
                }
                write_text(m, a1, "Native PC");
            }
            return 1;
        }
        if (address == 4203104U) {
            open = true;
            connect();
            return 0;
        }
        if (address == 4204136U || address == 4204232U) {
            if (packet_diagnostic) {
                std::clog << "Native online: network lifecycle close=" << address
                          << " caller=" << c.cpu.read_gpr_u32(31U) << " tick="
                          << static_cast<std::uint32_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - epoch)
                                     .count())
                          << '\n';
            }
            client.disconnect();
            peers.reset();
            room.reset();
            lobby.reset();
            discovered_rooms.clear();
            discovered_room_details.clear();
            clear_buddies();
            pending.clear();
            news_request = 0U;
            news_body.reset();
            for (auto& ref : refs) {
                ref.connected = false;
                ref.replies.clear();
            }
            datagrams.clear();
            game_port = 0U;
            state = State::Idle;
            presence_requested = false;
            publish_presence(m);
            if (address == 4204232U) {
                open = false;
            }
            return 0;
        }
        if (address == 4203240U) {
            if (a0 == 1869636974U) {
                return open;
            }
            if (a0 == 1869507694U) {
                return state == State::Online;
            }
            if (a0 == 1668247150U) {
                if (state != State::Online || state == State::Failed) {
                    return state == State::Failed ? -1 : 0;
                }
                return 0;
            }
            if (a0 == 1886156135U || a0 == 1650617956U) {
                return 0;
            }
            if (a0 == 1937010544U) {
                return state == State::Idle;
            }
            if ((a0 == 1752134516U || a0 == 1633969266U) && state == State::Online) {
                return (player & 65535U) + 184418304U;
            }
            return 0;
        }
        if (address == 4208416U) {
            return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - epoch)
                                                  .count());
        }
        if (address == 4205256U) {
            for (std::size_t i = 0U; i != refs.size(); ++i) {
                if (refs[i].used) {
                    continue;
                }
                refs[i] = Ref{};
                refs[i].used = true;
                const Address p = 729088U + static_cast<Address>(i) * 8960U;
                for (Address j = 0U; j != 256U; ++j) {
                    m.write_u8(p + j, 0U);
                }
                m.write_u32_le(p + 20U, 8192U);
                m.write_u32_le(p + 24U, p + 256U);
                return p;
            }
            return 0;
        }
        if (a0 <= 729087U) {
            return -1;
        }
        const auto offset = a0 - 729088U;
        if (offset % 8960U != 0U || offset / 8960U >= refs.size()) {
            return -1;
        }
        auto& ref = refs[offset / 8960U];
        if (!ref.used) {
            return -1;
        }
        if (address == 4205576U) {
            cancel(ref);
            ref.persona = 0U;
            ref = Ref{};
            return 0;
        }
        if (address == 4205704U) {
            cancel(ref);
            ref.persona = 0U;
            connect();
            ref.connected = true;
            ref.replies.clear();
            reply(ref, 4294967295U, std::string{}, 4294967295U);
            return 0;
        }
        if (address == 4206032U || address == 4206440U) {
            cancel(ref);
            ref.persona = 0U;
            ref.connected = false;
            ref.replies.clear();
            return 0;
        }
        if (address == 4206584U) {
            if (a1 == 1668446576U || a1 == 1936024437U) {
                return 1;
            }
            if (a1 == 1937006964U) {
                if (ref.connected && state == State::Online) {
                    return 2;
                }
                if (state == State::Failed) {
                    return -1;
                }
                return 0;
            }
            if (a1 == 1768060262U || a1 == 1868723558U) {
                return 8192;
            }
            if (a1 == 1818322034U || a1 == 1633969266U) {
                return (player & 65535U) + 184418304U;
            }
            if (a1 == 1819308660U || a1 == 1886351988U) {
                return 3658;
            }
            return 0;
        }
        if (address == 4206984U || address == 4208144U || address == 4208272U) {
            return 0;
        }
        if (address == 4206992U) {
            const auto length = static_cast<int>(f);
            if (state != State::Online || length > 8192) {
                return -1;
            }
            const std::string body =
                a2 != 0U ? read_text(m, a2, length >= 0 ? std::max<std::size_t>(f, 1U) : 8192U) : std::string{};
            send(ref, a1, body);
            return static_cast<std::int64_t>(body.size() + 1U);
        }
        if (address == 4207352U || address == 4207808U) {
            if ((a1 != 0U && !ram(m, a1, 4U)) || (a2 != 0U && !ram(m, a2, 4U)) || ref.replies.empty()) {
                return 0;
            }
            const bool copy_text = address == 4207808U;
            if ((address == 4207352U && a3 != 0U && !ram(m, a3, 4U)) ||
                (copy_text && a3 != 0U && f != 0U && !ram(m, a3, f))) {
                return 0;
            }
            const auto& item = ref.replies.front();
            write_text(m, a0 + 256U, item.text);
            if (a1 != 0U) {
                m.write_u32_le(a1, item.kind);
            }
            if (a2 != 0U) {
                m.write_u32_le(a2, item.code);
            }
            if (address == 4207352U) {
                if (a3 != 0U) {
                    m.write_u32_le(a3, a0 + 256U);
                }
            } else if (a3 != 0U) {
                for (std::size_t i = 0U; i < f; ++i) {
                    m.write_u8(a3 + static_cast<Address>(i), m.read_u8(a0 + 256U + static_cast<Address>(i)));
                }
            }
            const auto length = item.text.size() + 1U;
            ref.replies.pop_front();
            return static_cast<std::int64_t>(length);
        }
        return 0;
    }
    explicit Impl(net::MatchmakingConfig value) : config(std::move(value)) {}
};

Slus21050Online::Slus21050Online(net::MatchmakingConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}

Slus21050Online::~Slus21050Online() = default;

void Slus21050Online::set_mod_sync_selection_resolver(ModSyncSelectionResolver resolver) {
    impl_->mod_sync_selection_resolver = std::move(resolver);
}

std::optional<Slus21050Online::ModSyncFrontend> Slus21050Online::mod_sync_frontend() const {
    auto& join = impl_->blocked_join;
    if (!join.has_value() || join->screen == 0U || join->page == 0U) {
        return std::nullopt;
    }
    return ModSyncFrontend{join->screen, join->page};
}

void Slus21050Online::set_applied_mods(std::shared_ptr<const content::ModManager> mods) {
    impl_->config.applied_mods = std::move(mods);
    impl_->client.disconnect();
    impl_->peers.reset();
    impl_->room.reset();
    impl_->lobby.reset();
    impl_->pending.clear();
    impl_->discovered_rooms.clear();
    impl_->discovered_room_details.clear();
    impl_->clear_buddies();
    impl_->readiness_roster.clear();
    impl_->readiness_reports.clear();
    impl_->datagrams.clear();
    impl_->game_port = 0U;
    impl_->presence_requested = false;
    impl_->state = Impl::State::Idle;
    impl_->applied_mod_identity.clear();
    impl_->text_keys.clear();
    impl_->rejoin_stage = Impl::RejoinStage::None;
    impl_->rejoin_prepared = false;
    impl_->rejoin_join_request = 0U;
    impl_->rejoin_list_request = 0U;
    for (auto& ref : impl_->refs) {
        ref.connected = false;
        ref.replies.clear();
    }
}

std::optional<std::string> Slus21050Online::request_mod_sync_rejoin(std::uint32_t room,
                                                                   std::string_view digest) {
        if (!impl_->revision_registered || !impl_->blocked_join.has_value() || impl_->blocked_join->room != room ||
        impl_->blocked_join->digest != digest) {
        return "The original lobby request is no longer available. Choose the lobby again.";
    }
    if (impl_->rejoin_stage != Impl::RejoinStage::None && impl_->rejoin_stage != Impl::RejoinStage::Refused) {
        return "A lobby rejoin is already in progress.";
    }
    if (!impl_->mod_sync_selection_resolver) {
        return "The current car selection cannot be validated. Choose the lobby again.";
    }
    const auto ref = impl_->blocked_join->ref;
    if (!impl_->refs[ref].used || impl_->refs[ref].persona == 0U) {
        return "The original online session closed. Sign in and choose the lobby again.";
    }
    if (impl_->room.has_value()) {
        return "Leave the current lobby before rejoining this host.";
    }
    impl_->rejoin_error.reset();
    impl_->rejoin_prepare_only = false;
    impl_->rejoin_prepared = false;
    impl_->rejoin_prepared_announced = false;
    impl_->rejoin_stage = Impl::RejoinStage::Connecting;
    impl_->rejoin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    impl_->refs[ref].connected = true;
    impl_->connect();
    impl_->presence_requested = true;
    if (impl_->state == Impl::State::Failed) {
        impl_->fail_rejoin("Online reconnection failed. Choose the lobby again.");
    }
    return consume_mod_sync_error();
}

std::optional<std::string> Slus21050Online::prepare_mod_sync_rejoin(std::uint32_t room,
                                                                   std::string_view digest) {
    auto error = request_mod_sync_rejoin(room, digest);
    if (!error.has_value()) {
        impl_->rejoin_prepare_only = true;
    }
    return error;
}

bool Slus21050Online::consume_mod_sync_prepared() {
    return std::exchange(impl_->rejoin_prepared, false);
}

std::optional<std::string> Slus21050Online::consume_mod_sync_error() {
    return std::exchange(impl_->rejoin_error, std::nullopt);
}

bool Slus21050Online::text_entry_focused(const guest::EeMemory& memory) const {
    return impl_->text_entry_focused(memory);
}

void Slus21050Online::handle_text_input(const platform::TextInputEvent& event,
                                        const guest::EeMemory& memory) {
    auto& d = *impl_;
    if (!d.text_entry_focused(memory)) {
        d.text_keys.clear();
        return;
    }
    auto& keys = d.text_keys;
    if (keys.size() > 511U) {
        return;
    }
    if (event.kind != platform::TextInputKind::Text) {
        keys.push_back(event.kind == platform::TextInputKind::Backspace
                           ? 8U
                           : event.kind == platform::TextInputKind::Submit ? 1114112U : 1114113U);
        return;
    }
    std::vector<std::uint32_t> decoded;
    for (std::size_t i = 0U; i < event.text.size(); ++i) {
        const auto first = static_cast<unsigned char>(event.text[i]);
        std::uint32_t value = 0U;
        unsigned count = 0U;
        std::uint32_t minimum = 0U;
        if (first < 0x80U) {
            value = first;
        } else if (first - 0xC2U <= 29U) {
            value = first & 31U;
            count = 1U;
            minimum = 128U;
        } else if (first - 0xE0U <= 15U) {
            value = first & 15U;
            count = 2U;
            minimum = 2048U;
        } else if (first - 0xF0U <= 4U) {
            value = first & 7U;
            count = 3U;
            minimum = 65536U;
        } else {
            return;
        }
        if (count + i >= event.text.size() && count != 0U) {
            return;
        }
        while (count != 0U) {
            ++i;
            const auto next = static_cast<unsigned char>(event.text[i]);
            if ((next & 0xC0U) != 0x80U) {
                return;
            }
            value = (value << 6U) | (next & 63U);
            --count;
        }
        if (value < minimum || value > 1114111U || value - 0xD800U <= 2047U) {
            return;
        }
        if (value <= 31U || value == 127U) {
            continue;
        }
        if (value > 65535U) {
            decoded.push_back(((value - 0x10000U) >> 10U) | 0xD800U);
            decoded.push_back((value & 1023U) | 0xDC00U);
        } else {
            decoded.push_back(value);
        }
    }
    if (keys.size() + decoded.size() <= 512U) {
        keys.insert(keys.end(), decoded.begin(), decoded.end());
    }
}

bool Slus21050Online::register_functions(NativeFunctionDispatcher& dispatcher,
                                         const guest::EeMemory& memory) {
    if (fnv1a(memory, 3378816U, 3378848U) != 13144635540051018216ULL) {
        return false;
    }
    if (fnv1a(memory, 3650192U, 3650260U) != 10234772374946450023ULL) {
        return false;
    }
    for (const auto& signature : signatures) {
        if (!signature_matches(memory, signature)) {
            return false;
        }
        if (dispatcher.lookup(hook_address(signature.begin)) != nullptr) {
            return false;
        }
    }
    std::vector<Address> addresses;
    for (const auto& signature : signatures) {
        addresses.push_back(hook_address(signature.begin));
    }
    for (const auto address : {0x0034A4A4U, 0x003532E4U, 0x000B1800U, 0x000B1810U, 0x00323B38U, 0x00323E6CU, 0x0035F4D0U, 0x0035F520U, 0x0035F570U, 0x0035F5C0U, 0x0035F610U, 0x0035F670U, 0x0035F700U, 0x0035FD80U, 0x0035FEF0U, 0x0035F900U, 0x0035F970U, 0x0035F9E0U, 0x0035FA40U, 0x0035FAA0U, 0x0035FB00U, 0x0035FB60U, 0x00360020U, 0x003600D0U, 0x00360460U, 0x003604C0U, 0x00360560U, 0x00360620U, 0x00360670U, 0x003606D0U, 0x003606E0U, 0x003607C0U, 0x003608F0U, 0x003609A0U, 0x0035F190U, 0x0035F1E0U}) {
        if (dispatcher.lookup(address) != nullptr) {
            return false;
        }
        addresses.push_back(address);
    }
    for (const auto address : addresses) {
        if (!dispatcher.register_function(
            address, "NativeOnlineApi", NativeFunctionLimits{1U, 1U},
            NativeFunctionTraits{true, true, false},
            [impl = impl_, address](NativeFunctionContext& c) -> NativeFunctionExecution {
                if (address == 3378636U) {
                    c.cpu.write_gpr_u64(5U, 1734764910U);
                    c.cpu.set_pc(3378640U);
                    return {1U, 1U};
                }
                if (address == 3244652U) {
                    c.cpu.set_pc(3244720U);
                    return {1U, 1U};
                }
                std::int64_t result = -1;
                try {
                    if (impl->frontend(address, c)) {
                        return {1U, 1U};
                    }
                    result = impl->call(address, c);
                    if (address == 2289120U && result == 1) {
                        const auto a0 = c.cpu.read_gpr_u32(4U);
                        const auto a1 = c.cpu.read_gpr_u32(5U);
                        const Address callback = c.memory.read_u32_le(a0 + 60U);
                        if (callback != 0U && main_ram_range(callback, 4U)) {
                            c.cpu.write_gpr_u64(4U, a1);
                            c.cpu.set_pc(callback);
                            return {1U, 1U};
                        }
                        result = 0;
                    }
                } catch (const std::exception& error) {
                    std::clog << "Native online: " << error.what() << '\n';
                }
                c.cpu.write_gpr_u64(2U, static_cast<std::uint64_t>(result));
                c.cpu.set_pc(c.cpu.read_gpr_u32(31U));
                return {1U, 1U};
            }).registered()) {
            return false;
        }
    }
    impl_->revision_registered = true;
    return true;
}
} // namespace takedown::runtime
