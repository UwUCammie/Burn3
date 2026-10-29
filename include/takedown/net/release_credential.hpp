#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace takedown::net {

// Official release builds ship a secret (release.key) that the official matchmaking service checks, so builds
// made from source cannot use it (server/release_gate.py, docs/RELEASING.md). Without the file nothing is sent.
//
// The key is looked for, in order, in the file named by TAKEDOWN_RELEASE_KEY_FILE, release.key beside the
// executable, release.key in the working directory, and release.key in the launcher's active runtime bin
// folder (<install>/.launcher/<runtime>/bin, found through <install>/.launcher/runtime.json).
// Empty when there is none. Read once per process.
[[nodiscard]] std::string load_release_key();

// HMAC-SHA256 keyed by `key` over the message layout of game_login_proof, with the ticket set to
// "release:<timestamp>". Hex.
[[nodiscard]] std::string release_proof(std::string_view key, std::uint64_t timestamp, std::string_view version,
                                        unsigned wire, std::string_view mods, std::string_view name);

// The JSON members to append to a hello message, with a leading comma: release_ts and release_proof.
// Empty when `key` is empty.
[[nodiscard]] std::string release_hello_members(std::string_view key, std::uint64_t timestamp,
                                                std::string_view version, unsigned wire, std::string_view mods,
                                                std::string_view name);

// The same with the key from load_release_key() and the current time.
[[nodiscard]] std::string release_hello_members(std::string_view version, unsigned wire, std::string_view mods,
                                                std::string_view name);

} // namespace takedown::net
