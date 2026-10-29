// Release credential: the proof matches the server's (tests/test_release_gate.py checks the same vector),
// the hello members have the expected shape, and the key is found through TAKEDOWN_RELEASE_KEY_FILE.
#include "takedown/net/release_credential.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void set_environment(const char* const name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

} // namespace

int main() {
    using namespace takedown::net;
    const std::string key(64U, 'x');
    const std::string vector_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr const char* kVector = "b5603a8a37224cb19235ad334ae277078ee30e6fce5201c41606425d1656ef49";

    expect(release_proof(vector_key, 1700000000U, "0.0.26", 1U, "mods-abc", "Driver") == kVector,
           "proof differs from the server's vector");
    expect(release_proof(key, 1U, "0.0.26", 1U, "m", "n") != release_proof(key, 2U, "0.0.26", 1U, "m", "n"),
           "timestamp is part of the proof");
    expect(release_proof(key, 1U, "0.0.26", 1U, "m", "n") != release_proof(key, 1U, "0.0.26", 1U, "m", "other"),
           "name is part of the proof");

    expect(release_hello_members("", 1700000000U, "0.0.26", 1U, "mods-abc", "Driver").empty(),
           "no key, no members");
    expect(release_hello_members(vector_key, 1700000000U, "0.0.26", 1U, "mods-abc", "Driver") ==
               std::string(",\"release_ts\":1700000000,\"release_proof\":\"") + kVector + "\"",
           "hello members");

    const auto path = std::filesystem::temp_directory_path() / "takedown-release-credential-test.key";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "  " << vector_key << "\r\n";
    }
    set_environment("TAKEDOWN_RELEASE_KEY_FILE", path.string());
    expect(load_release_key() == vector_key, "key is read and trimmed from TAKEDOWN_RELEASE_KEY_FILE");
    std::filesystem::remove(path);

    if (failures == 0) {
        std::cout << "release credential tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
