#pragma once

// Shared by the runtime and the independently built paraLLEl-GS stream tool.
// Keep this header C++14 compatible; all packets retain their existing format.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <stdexcept>
#include <string>
#include <system_error>

namespace takedown { namespace platform { namespace windows_gs_pipe {

inline void validate(const wchar_t* path) {
    const std::wstring name(path);
    if (name.compare(0, 9, L"\\\\.\\pipe\\") != 0 || name.size() <= 9) {
        throw std::invalid_argument("live paraLLEl-GS requires a local \\\\.\\pipe\\ name");
    }
}

inline std::wstring widen(const char* path) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (count == 0) throw std::invalid_argument("invalid UTF-8 pipe name");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &result[0], count);
    result.pop_back();
    return result;
}

inline std::system_error error(const char* operation) {
    return std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

inline HANDLE open_reader(const wchar_t* path) {
    validate(path);
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        HANDLE pipe = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD code = GetLastError();
        if ((code != ERROR_FILE_NOT_FOUND && code != ERROR_PIPE_BUSY) || GetTickCount64() >= deadline) {
            SetLastError(code);
            throw error("could not connect paraLLEl-GS pipe reader");
        }
        Sleep(10);
    }
}

// Polling clients never read more than PeekNamedPipe says is available. Only
// this thread reads this handle, so an idle renderer cannot stall emulation.
// -1 denotes disconnect, zero denotes no data (including before connection).
inline int read_available(HANDLE pipe, void* bytes, std::size_t capacity) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED)
            return -1;
        throw error("could not inspect paraLLEl-GS pipe");
    }
    if (available == 0) return 0;
    DWORD count = 0;
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(available, capacity));
    if (!ReadFile(pipe, bytes, requested, &count, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return -1;
        throw error("could not read paraLLEl-GS pipe");
    }
    return static_cast<int>(count);
}

inline HANDLE create_writer(const wchar_t* path, bool polling) {
    validate(path);
    HANDLE pipe = CreateNamedPipeW(path, PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        (polling ? PIPE_TYPE_MESSAGE | PIPE_NOWAIT : PIPE_TYPE_BYTE | PIPE_WAIT) |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 256 * 1024, 0, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw error("could not create paraLLEl-GS pipe");
    return pipe;
}

inline std::FILE* open_stream_writer(const wchar_t* path) {
    HANDLE pipe = create_writer(path, false);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        const auto failure = error("could not connect paraLLEl-GS stream");
        CloseHandle(pipe);
        throw failure;
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(pipe), _O_WRONLY | _O_BINARY);
    if (descriptor < 0) { CloseHandle(pipe); throw std::runtime_error("could not wrap GS pipe"); }
    std::FILE* file = _fdopen(descriptor, "wb");
    if (!file) { _close(descriptor); throw std::runtime_error("could not wrap GS pipe stream"); }
    return file;
}

// Small return-channel records use nonblocking message writes (all-or-none).
// Read handles use byte mode, preserving the existing incremental decoders.
class Output {
public:
    explicit Output(const char* path) : handle(create_writer(widen(path).c_str(), true)) {}
    ~Output() { CloseHandle(handle); }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    int write(const void* bytes, std::size_t size) noexcept {
        if (!connected) {
            if (ConnectNamedPipe(handle, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
                connected = true;
            else if (GetLastError() == ERROR_PIPE_LISTENING) return 0;
            else return -1;
        }
        DWORD count = 0;
        if (!WriteFile(handle, bytes, static_cast<DWORD>(size), &count, nullptr)) return -1;
        return static_cast<int>(count);
    }
private:
    HANDLE handle;
    bool connected = false;
};

}}} // namespace takedown::platform::windows_gs_pipe
#endif
