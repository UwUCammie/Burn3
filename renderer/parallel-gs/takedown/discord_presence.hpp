#pragma once
// Discord Rich Presence over the Discord client's local IPC pipe
// (\\.\pipe\discord-ipc-N): a handshake with the application ID, then
// SET_ACTIVITY frames. No Discord SDK. A worker thread connects (and
// reconnects when Discord starts later or restarts), so the renderer never
// waits on it. Without an application ID, or off Windows, it does nothing.
//
// The application (its name, "Burnout 3 PC - Recomp", and the art asset
// kLargeImage) is set up on discord.com/developers; its ID comes from the
// environment variable TAKEDOWN_DISCORD_APPLICATION_ID, else
// discord_application_id.txt beside the executable, else the build's
// TAKEDOWN_DISCORD_APPLICATION_ID. TAKEDOWN_DISCORD=0 turns presence off.
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace TakedownRenderer
{
class DiscordPresence
{
public:
	static constexpr const char *kLargeImage = "b3";
	static constexpr const char *kLargeText = "Burnout 3 PC - Recomp";

	DiscordPresence();
	~DiscordPresence();
	DiscordPresence(const DiscordPresence &) = delete;
	DiscordPresence &operator=(const DiscordPresence &) = delete;

	bool enabled() const { return !application_id_.empty(); }
	// What the player is doing ("Racing") and a second line (may be empty).
	// Cheap: only a change is sent.
	void set(const std::string &details, const std::string &state);

	// The JSON of a SET_ACTIVITY command (exposed for tests).
	static std::string activity_json(uint32_t pid, const std::string &details, const std::string &state,
	                                 int64_t start_unix_seconds, uint64_t nonce);

private:
	void run();

	std::string application_id_;
	int64_t started_ = 0;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::string details_, state_;
	uint64_t generation_ = 0;
	bool stop_ = false;
	std::thread worker_;
};
}
