#include "discord_presence.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef TAKEDOWN_DISCORD_APPLICATION_ID
#define TAKEDOWN_DISCORD_APPLICATION_ID ""
#endif

namespace TakedownRenderer
{
namespace
{
std::string json_string(const std::string &text)
{
	std::string out = "\"";
	for (char c : text)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += c;
		}
		else if (static_cast<unsigned char>(c) < 0x20)
			out += ' ';
		else
			out += c;
	}
	return out + "\"";
}

bool digits_only(const std::string &text)
{
	if (text.empty() || text.size() > 24)
		return false;
	for (char c : text)
		if (c < '0' || c > '9')
			return false;
	return true;
}

#ifdef _WIN32
// Discord's frame: opcode and length (little endian), then JSON.
enum Opcode : uint32_t
{
	Handshake = 0,
	Frame = 1,
	Close = 2,
};

bool write_frame(HANDLE pipe, uint32_t opcode, const std::string &json)
{
	std::vector<char> data(8 + json.size());
	const uint32_t length = uint32_t(json.size());
	std::memcpy(data.data(), &opcode, 4);
	std::memcpy(data.data() + 4, &length, 4);
	std::memcpy(data.data() + 8, json.data(), json.size());
	DWORD written = 0;
	return WriteFile(pipe, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
}

// Reads whatever Discord sent (replies to our commands), so the pipe never
// fills. False when the pipe is gone or Discord closed it.
bool drain(HANDLE pipe)
{
	for (;;)
	{
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
			return false;
		if (available < 8)
			return true;
		uint32_t header[2] = {};
		DWORD got = 0;
		if (!ReadFile(pipe, header, 8, &got, nullptr) || got != 8)
			return false;
		std::vector<char> body(header[1]);
		if (header[1] && (!ReadFile(pipe, body.data(), header[1], &got, nullptr) || got != header[1]))
			return false;
		if (header[0] == Close)
			return false;
	}
}

HANDLE open_pipe()
{
	for (int i = 0; i < 10; i++)
	{
		const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
		HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			return pipe;
	}
	return INVALID_HANDLE_VALUE;
}
#endif
}

std::string DiscordPresence::activity_json(uint32_t pid, const std::string &details, const std::string &state,
                                           int64_t start_unix_seconds, uint64_t nonce)
{
	std::string activity = "{\"details\":" + json_string(details);
	if (!state.empty())
		activity += ",\"state\":" + json_string(state);
	activity += ",\"timestamps\":{\"start\":" + std::to_string(start_unix_seconds) + "}";
	activity += ",\"assets\":{\"large_image\":" + json_string(kLargeImage) + ",\"large_text\":" +
	            json_string(kLargeText) + "}}";
	return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":" + activity +
	       "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

DiscordPresence::DiscordPresence()
{
	const char *off = std::getenv("TAKEDOWN_DISCORD");
	if (off && std::strcmp(off, "0") == 0)
		return;
	const char *id = std::getenv("TAKEDOWN_DISCORD_APPLICATION_ID");
	application_id_ = id && *id ? id : TAKEDOWN_DISCORD_APPLICATION_ID;
#ifdef _WIN32
	// Or discord_application_id.txt beside the executable (set by a release
	// without rebuilding the renderer).
	if (!(id && *id))
	{
		char path[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string file(path, length);
		const auto slash = file.find_last_of("\\/");
		file = (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + "discord_application_id.txt";
		if (FILE *stream = std::fopen(file.c_str(), "rb"))
		{
			char text[64] = {};
			const size_t got = std::fread(text, 1, sizeof(text) - 1, stream);
			std::fclose(stream);
			std::string value(text, got);
			while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
				value.pop_back();
			if (!value.empty())
				application_id_ = value;
		}
	}
#endif
	if (!digits_only(application_id_))
		application_id_.clear();
#ifdef _WIN32
	if (enabled())
	{
		started_ = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		worker_ = std::thread([this]() { run(); });
	}
#else
	application_id_.clear();
#endif
}

DiscordPresence::~DiscordPresence()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

void DiscordPresence::set(const std::string &details, const std::string &state)
{
	if (!enabled())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (details == details_ && state == state_)
			return;
		details_ = details;
		state_ = state;
		generation_++;
	}
	wake_.notify_all();
}

void DiscordPresence::run()
{
#ifdef _WIN32
	uint64_t nonce = 0;
	while (true)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stop_)
				return;
		}
		HANDLE pipe = open_pipe();
		bool connected = pipe != INVALID_HANDLE_VALUE &&
		                 write_frame(pipe, Handshake, "{\"v\":1,\"client_id\":\"" + application_id_ + "\"}");
		uint64_t sent = 0;
		while (connected)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			// Wake for a change, to drain replies, or to stop.
			wake_.wait_for(lock, std::chrono::seconds(2), [&]() { return stop_ || generation_ != sent; });
			if (stop_)
				break;
			const uint64_t generation = generation_;
			const std::string details = details_, state = state_;
			lock.unlock();
			connected = drain(pipe);
			if (connected && generation != sent && !details.empty())
			{
				connected = write_frame(pipe, Frame,
				                        activity_json(uint32_t(GetCurrentProcessId()), details, state, started_, ++nonce));
				sent = generation;
			}
		}
		if (pipe != INVALID_HANDLE_VALUE)
			CloseHandle(pipe);
		// Discord not running (or it closed): try again later.
		std::unique_lock<std::mutex> lock(mutex_);
		if (wake_.wait_for(lock, std::chrono::seconds(15), [&]() { return stop_; }))
			return;
	}
#endif
}
}
