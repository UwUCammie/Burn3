#include "stream_pipe.hpp"
#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"
#endif
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#include <fcntl.h>
#endif

namespace TakedownRenderer
{
// Developer aid: TAKEDOWN_DEV_STREAM_DUMP=<file> copies every byte read from
// the graphics stream into <file>, so a live session can be replayed later.
// A thread pumps the source into an anonymous pipe and the file.
// Returns as soon as some bytes are available (fread would wait for a full buffer).
static size_t read_some(std::FILE *source, char *buffer, size_t size)
{
#if defined(_WIN32)
	const int count = _read(_fileno(source), buffer, unsigned(size));
#else
	const ssize_t count = read(fileno(source), buffer, size);
#endif
	return count > 0 ? size_t(count) : 0;
}

static std::FILE *tee_stream(std::FILE *source, const char *dump_path)
{
	std::FILE *dump = dump_path ? std::fopen(dump_path, "wb") : nullptr;
	if (dump_path && !dump)
		std::fprintf(stderr, "[ERROR]: Stream dump: could not open %s.\n", dump_path);
	int fds[2];
#if defined(_WIN32)
	if (_pipe(fds, 16 << 20, _O_BINARY) != 0)
#else
	if (pipe(fds) != 0)
#endif
	{
		if (dump)
			std::fclose(dump);
		return source;
	}
	std::thread([source, dump, write_fd = fds[1]]() {
		static char buffer[1 << 16];
		size_t count;
		while ((count = read_some(source, buffer, sizeof(buffer))) > 0)
		{
			if (dump)
				std::fwrite(buffer, 1, count, dump);
			const char *at = buffer;
			while (count > 0)
			{
#if defined(_WIN32)
				const int written = _write(write_fd, at, unsigned(count));
#else
				const ssize_t written = write(write_fd, at, count);
#endif
				if (written <= 0)
					break;
				at += written;
				count -= size_t(written);
			}
		}
		if (dump)
			std::fclose(dump);
		std::fclose(source);
#if defined(_WIN32)
		_close(write_fd);
#else
		close(write_fd);
#endif
	}).detach();
#if defined(_WIN32)
	return _fdopen(fds[0], "rb");
#else
	return fdopen(fds[0], "rb");
#endif
}

// The dump path: TAKEDOWN_DEV_STREAM_DUMP, else the first line of
// stream-dump.txt beside the executable (for launches that do not pass the
// environment through).
static std::string stream_dump_path()
{
	if (const char *dump = std::getenv("TAKEDOWN_DEV_STREAM_DUMP"); dump && *dump)
		return dump;
#if defined(_WIN32)
	char module[MAX_PATH];
	const DWORD length = GetModuleFileNameA(nullptr, module, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return {};
	std::string config(module, length);
	const size_t slash = config.find_last_of("\\/");
	config = (slash == std::string::npos ? std::string() : config.substr(0, slash + 1)) + "stream-dump.txt";
	std::FILE *file = std::fopen(config.c_str(), "rb");
	if (!file)
		return {};
	char line[1024] = {};
	const bool read = std::fgets(line, sizeof(line), file) != nullptr;
	std::fclose(file);
	if (!read)
		return {};
	std::string path(line);
	while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
		path.pop_back();
	return path;
#else
	return {};
#endif
}

static std::FILE *maybe_tee(std::FILE *file)
{
	if (!file)
		return file;
	const std::string dump = stream_dump_path();
	if (!dump.empty())
	{
		std::fprintf(stderr, "[INFO]: Stream dump: writing the graphics stream to %s.\n", dump.c_str());
		return tee_stream(file, dump.c_str());
	}
	// Read the live pipe directly with a large buffer: small packet headers
	// come from the buffer and large payloads are read in few calls.
	std::setvbuf(file, nullptr, _IOFBF, 4u << 20);
	return file;
}

bool is_live_pipe_name(const std::string &path)
{
	return path.compare(0, 9, "\\\\.\\pipe\\") == 0;
}

std::FILE *open_graphics_stream(const std::string &path, std::string &error, bool announce_native_cmd)
{
#if defined(_WIN32)
	if (is_live_pipe_name(path))
	{
		try
		{
			namespace pipe = takedown::platform::windows_gs_pipe;
			if (announce_native_cmd)
			{
				// The game looks for this event when it connects; it stays open (manual reset, never set) for the life of the process.
				const std::string name = takedown::native_cmd::capability_event_name(takedown::native_cmd::capability_base(path.c_str()), takedown::native_cmd::kVersion);
				if (CreateEventA(nullptr, TRUE, FALSE, name.c_str()) == nullptr)
					std::fprintf(stderr, "[WARNING]: Could not create the native command capability event %s (error %lu); the game will send legacy strips.\n", name.c_str(),
					             static_cast<unsigned long>(GetLastError()));
				else
					std::fprintf(stderr, "[INFO]: Native command stream v%u announced (%s).\n", takedown::native_cmd::kVersion, name.c_str());
			}
			HANDLE handle = pipe::open_reader(pipe::widen(path.c_str()).c_str());
			const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
			if (descriptor < 0)
			{
				CloseHandle(handle);
				error = "could not wrap GS pipe";
				return nullptr;
			}
			std::FILE *file = _fdopen(descriptor, "rb");
			if (!file)
			{
				_close(descriptor);
				error = "could not wrap GS pipe stream";
			}
			return maybe_tee(file);
		}
		catch (const std::exception &e)
		{
			error = e.what();
			return nullptr;
		}
	}
#endif
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (!file)
		error = "could not open " + path;
	return file;
}
}
