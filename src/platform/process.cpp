#include "takedown/platform/process.hpp"

#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

namespace takedown::platform {

HostCpuSnapshot capture_host_cpu_snapshot() noexcept {
    HostCpuSnapshot snapshot;

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};

    const auto ticks = [](const FILETIME& time) noexcept {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };

    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0) {
        const std::uint64_t kernel_ticks = ticks(kernel);
        snapshot.thread_nanoseconds = (ticks(user) + kernel_ticks) * 100;
    }
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
        const std::uint64_t kernel_ticks = ticks(kernel);
        snapshot.process_nanoseconds = (ticks(user) + kernel_ticks) * 100;
    }

    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0) {
        snapshot.thread_cycles = cycles;
    }
    return snapshot;
}

bool ProcessResult::exited_normally() const noexcept {
    return termination_signal == 0 && exit_code >= 0;
}

bool ProcessResult::succeeded() const noexcept {
    return exited_normally() && exit_code == 0;
}

namespace {

// Quotes each argument using the MSVCRT rules so the child parses it back exactly.
[[nodiscard]] std::string build_windows_command_line(std::span<const std::string> arguments) {
    std::string command_line;
    for (const std::string& argument : arguments) {
        if (!command_line.empty()) {
            command_line.push_back(' ');
        }

        const bool needs_quotes = argument.empty() || argument.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needs_quotes) {
            command_line += argument;
            continue;
        }

        command_line.push_back('"');
        std::size_t backslashes = 0;
        for (const char character : argument) {
            if (character == '\\') {
                ++backslashes;
                continue;
            }

            // Backslashes before a quote are doubled, and the quote itself is escaped.
            if (character == '"') {
                command_line.append(backslashes * 2 + 1, '\\');
                command_line.push_back(character);
            } else {
                command_line.append(backslashes, '\\');
                command_line.push_back(character);
            }
            backslashes = 0;
        }
        command_line.append(backslashes * 2, '\\');
        command_line.push_back('"');
    }
    return command_line;
}

[[nodiscard]] std::system_error last_error(const std::string& what) {
    return std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

// Closes a process or thread handle once and clears it so later cleanup is a no-op.
void close_handle(HANDLE& handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

} // namespace

ProcessResult run_process(std::span<const std::string> arguments) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::invalid_argument("run_process requires a non-empty executable name");
    }

    const std::string command_line = build_windows_command_line(arguments);
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (CreateProcessA(nullptr, mutable_command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup_info, &process_info) == 0) {
        throw last_error("could not execute " + arguments.front());
    }

    close_handle(process_info.hThread);
    if (WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0) {
        const std::system_error error = last_error("could not wait for " + arguments.front());
        close_handle(process_info.hProcess);
        throw error;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_info.hProcess, &exit_code) == 0) {
        const std::system_error error = last_error("could not read exit status of " + arguments.front());
        close_handle(process_info.hProcess);
        throw error;
    }
    close_handle(process_info.hProcess);

    ProcessResult result;
    const auto signed_code = static_cast<int>(exit_code);
    if (signed_code >= 0) {
        result.exit_code = signed_code;
    } else {
        result.termination_signal = signed_code;
    }
    return result;
}

CapturedProcessResult run_process_and_capture(std::span<const std::string> arguments,
                                              std::size_t maximum_output_bytes) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::invalid_argument("run_process_and_capture requires a non-empty executable name");
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0) == 0) {
        throw last_error("could not create output pipe");
    }
    if (SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0) == 0) {
        const std::system_error error = last_error("could not configure output pipe");
        close_handle(read_pipe);
        close_handle(write_pipe);
        throw error;
    }

    const std::string command_line = build_windows_command_line(arguments);
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = write_pipe;
    startup_info.hStdError = write_pipe;
    PROCESS_INFORMATION process_info{};
    if (CreateProcessA(nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup_info, &process_info) == 0) {
        const std::system_error error = last_error("could not execute " + arguments.front());
        close_handle(read_pipe);
        close_handle(write_pipe);
        throw error;
    }

    close_handle(process_info.hThread);
    close_handle(write_pipe);

    CapturedProcessResult captured;
    std::array<char, 4096> buffer{};
    DWORD read_count = 0;
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_count, nullptr) != 0) {
        if (read_count == 0) {
            break;
        }

        const auto received = static_cast<std::size_t>(read_count);
        const std::size_t kept = captured.output.size();
        const std::size_t remaining = maximum_output_bytes > kept ? maximum_output_bytes - kept : 0;
        const std::size_t copied = std::min<std::size_t>(received, remaining);
        captured.output.append(buffer.data(), copied);
        captured.output_truncated = captured.output_truncated || received != copied;
    }
    close_handle(read_pipe);

    if (WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0) {
        const std::system_error error = last_error("could not wait for " + arguments.front());
        close_handle(process_info.hProcess);
        throw error;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_info.hProcess, &exit_code) == 0) {
        const std::system_error error = last_error("could not read exit status of " + arguments.front());
        close_handle(process_info.hProcess);
        throw error;
    }
    close_handle(process_info.hProcess);

    const auto signed_code = static_cast<int>(exit_code);
    ProcessResult& result = captured.process;
    if (signed_code >= 0) {
        result.exit_code = signed_code;
    } else {
        result.termination_signal = signed_code;
    }
    return captured;
}

std::string describe_process_result(const ProcessResult& result) {
    if (result.exited_normally()) {
        return "exit code " + std::to_string(result.exit_code);
    }
    if (result.termination_signal != 0) {
        return "signal " + std::to_string(result.termination_signal);
    }
    return "unknown process status";
}

} // namespace takedown::platform
