#pragma once
// Renderer side of the input FIFO (takedown/platform/parallel_gs_input.hpp
// and raw_input_protocol.hpp). The runtime reads it with
// src/platform/parallel_gs_input.cpp; this writer must produce exactly the
// packets that reader understands. No SDL here: the application translates
// SDL events into these calls, so this class is testable anywhere.
#include <cstdint>
#include <memory>
#include <string>

namespace TakedownRenderer
{
class InputWriter
{
public:
	// Windows: `path` is "\\.\pipe\...", created with windows_gs_pipe::Output
	// (nonblocking message pipe; the runtime connects as reader). POSIX:
	// an existing FIFO opened O_WRONLY | O_NONBLOCK (retry open while no
	// reader: ENXIO). Throws std::runtime_error on failure to create.
	explicit InputWriter(const std::string &path);
	~InputWriter();
	InputWriter(const InputWriter &) = delete;
	InputWriter &operator=(const InputWriter &) = delete;

	// SDL scancode (USB HID usage, 0..511) went down/up.
	void key(std::uint32_t scancode, bool down);
	// Mouse button 1 left, 2 middle, 3 right, 4/5 side.
	void mouse_button(std::uint32_t button, bool down);
	// Pointer position as a fraction of the shown game picture (0..1 across
	// it), and whether the pointer is inside the window.
	void pointer(float x, float y, bool inside);
	// Wheel steps, positive away from the player.
	void wheel(int steps);
	// Text input (UTF-8) and editing keys, see raw_input_protocol.hpp.
	void text(const std::string &utf8);
	void text_backspace();
	void text_submit();
	void text_cancel();
	// The window was asked to close (B3IN Quit edge).
	void request_quit();
	// Window focus: on focus loss every held key/button is released.
	void focus(bool focused);

	// Sends every packet whose content changed since the last flush (and a
	// periodic full-state refresh if the reader expects one). Nonblocking.
	void flush();

	std::uint64_t packets_written() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
}
