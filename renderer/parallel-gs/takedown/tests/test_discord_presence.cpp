// Tests of DiscordPresence::activity_json: the SET_ACTIVITY command Discord receives.
#include "discord_presence.hpp"

#include <cstdio>
#include <string>

using TakedownRenderer::DiscordPresence;

int main()
{
	int failures = 0;
	const auto check = [&](bool condition, const char *what) {
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", what);
			failures++;
		}
	};
	const std::string json = DiscordPresence::activity_json(42, "Racing", "Version v0.0.25", 1700000000, 7);
	check(json == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"details\":\"Racing\",\"state\":"
	              "\"Version v0.0.25\",\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":\"b3\","
	              "\"large_text\":\"Burnout 3 PC - Recomp\"}}},\"nonce\":\"7\"}",
	      "activity JSON");
	const std::string quoted = DiscordPresence::activity_json(1, "say \"hi\"\n", "", 0, 1);
	check(quoted.find("\"details\":\"say \\\"hi\\\" \"") != std::string::npos && quoted.find("\"state\"") == std::string::npos,
	      "quotes escaped, control characters dropped, empty state left out");
	if (failures == 0)
		std::printf("discord presence tests passed\n");
	return failures == 0 ? 0 : 1;
}
