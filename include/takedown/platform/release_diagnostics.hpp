#pragma once

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace takedown::platform {

[[nodiscard]] inline bool diagnostic_environment_name(std::string_view name) {
    std::string upper{name};
    for (char& character : upper) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    name = upper;
    if (!name.starts_with("TAKEDOWN_")) return false;
    if (name.starts_with("TAKEDOWN_DEV_")) return true;
    constexpr std::array markers{"CAPTURE", "PROFILE", "TRACE", "DUMP", "DIAG",
                                  "CENSUS", "JOURNAL", "_CASE", "_PROBE", "_WATCH"};
    for (const auto marker : markers) if (name.find(marker) != name.npos) return true;
    return name == "TAKEDOWN_EE_AOT_MISSES" || name == "TAKEDOWN_EXIT_PEEK" ||
           name == "TAKEDOWN_GS_STATS" || name == "TAKEDOWN_M3_LAUNCH_LOG" ||
           name == "TAKEDOWN_INPUT_SCRIPT" || name == "TAKEDOWN_SCALAR_BURST_AUDIT" ||
           name == "TAKEDOWN_NATIVE_CMD_ECHO" || name == "TAKEDOWN_TAA_LOG" ||
           name == "TAKEDOWN_TAA_NO_OBJECT_MOTION";
}

[[nodiscard]] inline bool diagnostic_argument(std::string_view argument) {
    if (!argument.starts_with("--")) return false;
    constexpr std::array markers{"capture", "profile", "trace", "dump", "diagnostic",
                                  "scanout-ppm", "guest-input-record", "frame-time-script"};
    for (const auto marker : markers) if (argument.find(marker) != argument.npos) return true;
    return false;
}

// Strip opt-in developer outputs even when a release is launched from a shell
// containing old profiling settings. Collect first: CRT mutation invalidates
// pointers in the original environment block.
inline bool disable_release_diagnostics() {
    std::vector<std::string> names;
#ifdef _WIN32
    auto* const environment = _environ;
#else
    auto* const environment = ::environ;
#endif
    if (environment != nullptr) {
        for (auto** entry = environment; *entry != nullptr; ++entry) {
            const std::string_view value{*entry};
            const auto name = value.substr(0, value.find('='));
            if (diagnostic_environment_name(name)) names.emplace_back(name);
        }
    }
    for (const auto& name : names) {
#ifdef _WIN32
        if (_putenv_s(name.c_str(), "") != 0) return false;
#else
        if (unsetenv(name.c_str()) != 0) return false;
#endif
    }
    return true;
}

} // namespace takedown::platform
