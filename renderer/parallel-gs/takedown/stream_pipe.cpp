#include "stream_pipe.hpp"
#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"
#include "gs_shm_ring.h"
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
	return open_graphics_stream(path, error, announce_native_cmd, nullptr);
}

std::FILE *open_graphics_stream(const std::string &path, std::string &error, bool announce_native_cmd, GsShm **ring)
{
	if (ring)
		*ring = nullptr;
#if defined(_WIN32)
	if (is_live_pipe_name(path))
	{
		try
		{
			namespace pipe = takedown::platform::windows_gs_pipe;
			const std::wstring wide = pipe::widen(path.c_str());
			const char *shm_env = std::getenv("TAKEDOWN_GS_STREAM_SHM");
			const bool want_ring = ring && stream_dump_path().empty() && !(shm_env && shm_env[0] == '0');
			char base[40] = {};
			if (want_ring)
			{
				// Announce shared-memory support before connecting; kept for the life of the process.
				gs_shm_cap_name(wide.c_str(), base, sizeof(base));
				static HANDLE announce = gs_shm_cap_create(base);
				(void)announce;
			}
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
			HANDLE handle = pipe::open_reader(wide.c_str());
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
				return nullptr;
			}
			file = maybe_tee(file);
			if (!want_ring || !file)
				return file;

			// The first byte tells the carrier: 0xFE opens a hello record, anything else is the stream
			// itself (no valid record starts with 0xFE) and is pushed back.
			const int first = std::fgetc(file);
			if (first == EOF)
				return file;
			if (first != 0xFE)
			{
				std::ungetc(first, file);
				return file;
			}
			unsigned char hello[GS_SHM_HELLO_BYTES];
			hello[0] = 0xFE;
			uint32_t ring_bytes = 0;
			if (std::fread(hello + 1, 1, sizeof(hello) - 1, file) != sizeof(hello) - 1 || !gs_shm_hello_decode(hello, &ring_bytes))
			{
				std::fclose(file);
				error = "malformed shared-memory hello from the game";
				return nullptr;
			}
			ULONG server_pid = 0;
			HANDLE peer = nullptr;
			if (GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))), &server_pid))
				peer = OpenProcess(SYNCHRONIZE, FALSE, server_pid);
			uint32_t spin_us = 0;
			if (const char *spin = std::getenv("TAKEDOWN_GS_STREAM_SHM_SPIN_US"); spin && *spin)
				spin_us = uint32_t(std::strtoul(spin, nullptr, 10));
			auto *opened = new GsShm;
			if (gs_shm_consumer_open(opened, base, spin_us, peer) != 0)
			{
				delete opened;
				if (peer)
					CloseHandle(peer);
				std::fclose(file);
				error = "could not open the game's shared-memory ring";
				return nullptr;
			}
			std::fprintf(stderr, "[INFO]: Graphics stream: shared-memory ring %u KB.\n", ring_bytes >> 10);
			*ring = opened;
			return file;
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
