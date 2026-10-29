#pragma once
// Opens the graphics stream the runtime writes (parallel_gs_stream.hpp).
// On Windows a "\\.\pipe\..." name connects to the runtime's named pipe
// (windows_gs_pipe::open_reader, which retries for 30 s); any other path is
// opened as a regular file, so recorded captures replay the same way.
#include <cstdio>
#include <string>

namespace TakedownRenderer
{
// Returns nullptr and fills `error` on failure. The caller owns the FILE.
// `announce_native_cmd`: create the NS-3 capability event for a live pipe (native_cmd_protocol.hpp, "Handshake") before opening it,
// so the game may replace VU1 strips by tokens; only for a renderer that consumes them (tools/ns3_live.hpp).
std::FILE *open_graphics_stream(const std::string &path, std::string &error, bool announce_native_cmd = false);
// True for a live pipe name (restart/looping is meaningless there).
bool is_live_pipe_name(const std::string &path);
}
