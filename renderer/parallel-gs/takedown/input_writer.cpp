// Renderer side of the runtime's input FIFO. Packets must match the reader in
// src/platform/parallel_gs_input.cpp and the headers in
// include/takedown/platform/parallel_gs_input.hpp and raw_input_protocol.hpp.
#include "input_writer.hpp"

#include "takedown/platform/raw_input_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#else
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace TakedownRenderer
{
namespace
{
namespace tp = takedown::platform;

// B3IN and B3RI hotkey bits, as ParallelGsInputButton in
// takedown/platform/parallel_gs_input.hpp. That header pulls in window.hpp,
// which needs C++20 (std::span), so the values are repeated here; the reader
// keeps them in B3RI bits 16..31 and turns rising edges into requests.
constexpr std::uint32_t kFrameStepToggleBit = 1U << 16U; // F6
constexpr std::uint32_t kFrameAdvanceBit = 1U << 17U;    // F7
constexpr std::uint32_t kCaptureBit = 1U << 18U;         // F8
constexpr std::uint32_t kQuitBit = 1U << 19U;            // request_quit()

// SDL/USB HID scancodes of F6, F7, F8: the same keys the runtime's own window
// maps to frame step, frame advance and capture (src/platform/window.cpp).
constexpr std::uint32_t kScancodeF6 = 63U;
constexpr std::uint32_t kScancodeF7 = 64U;
constexpr std::uint32_t kScancodeF8 = 65U;

constexpr std::uint32_t kKeyCount = 512U;
constexpr std::uint32_t kMouseButtonCount = 8U;
// B3IN is the 8-byte button packet; the reader only looks at its mask word.
constexpr std::size_t kButtonPacketBytes = 8U;
// Text is framed with at most 68 bytes per packet (raw_input_protocol.hpp).
constexpr std::size_t kMaxTextBytes = 68U;
// Upper bound on packets waiting for the pipe. Beyond it state and text stay in
// their latest-value or queue form and are packetised later, so flush() never
// grows without limit while the reader is absent or slow.
constexpr std::size_t kMaxQueuedPackets = 512U;
// A full raw and pointer snapshot is repeated this often, so a reader that
// connects after the last change still converges. Repeats cause no edges.
constexpr std::chrono::milliseconds kRefreshInterval{250};

std::uint32_t hotkey_for_scancode(const std::uint32_t scancode) noexcept
{
	switch (scancode)
	{
	case kScancodeF6:
		return kFrameStepToggleBit;
	case kScancodeF7:
		return kFrameAdvanceBit;
	case kScancodeF8:
		return kCaptureBit;
	default:
		return 0U;
	}
}

struct OutgoingPacket
{
	tp::RawInputPacket bytes{};
	std::size_t size{tp::kRawInputPacketBytes};
	std::size_t sent{0U};
};

OutgoingPacket outgoing(const tp::RawInputPacket &packet)
{
	OutgoingPacket out;
	out.bytes = packet;
	return out;
}

OutgoingPacket outgoing_button_word(const std::uint32_t mask)
{
	OutgoingPacket out;
	out.bytes[0] = 'B';
	out.bytes[1] = '3';
	out.bytes[2] = 'I';
	out.bytes[3] = 'N';
	for (unsigned i = 0; i < 4U; ++i)
		out.bytes[4U + i] = static_cast<std::uint8_t>(mask >> (8U * i));
	out.size = kButtonPacketBytes;
	return out;
}

#if !defined(_WIN32)
// write(2) to a FIFO whose reader has gone raises SIGPIPE, which would end the
// renderer. SIGPIPE is blocked for the call and the signal it leaves pending is
// cleared, so a vanished reader only shows up as EPIPE.
std::ptrdiff_t write_fifo(const int fd, const std::uint8_t *data, const std::size_t size) noexcept
{
	sigset_t pipe_signal{};
	sigset_t previous{};
	sigemptyset(&pipe_signal);
	sigaddset(&pipe_signal, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);
	ssize_t written = 0;
	do
	{
		written = ::write(fd, data, size);
	} while (written < 0 && errno == EINTR);
	const int error = errno;
	if (written < 0 && error == EPIPE)
	{
		const timespec immediate{0, 0};
		while (sigtimedwait(&pipe_signal, nullptr, &immediate) > 0)
		{
		}
	}
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	errno = error;
	return static_cast<std::ptrdiff_t>(written);
}
#endif

} // namespace

// Host state is kept as latest values (keys, mouse buttons, hotkeys, pointer)
// plus queues for events that must not merge (text, wheel steps). flush()
// turns changes into whole packets in an outbox, which is written in order.
// A packet is only counted as written after every one of its bytes went out.
struct InputWriter::Impl
{
	using Clock = std::chrono::steady_clock;

	explicit Impl(const std::string &fifo_path) : path(fifo_path)
	{
#if defined(_WIN32)
		pipe = std::make_unique<takedown::platform::windows_gs_pipe::Output>(path.c_str());
#else
		struct stat info{};
		if (::stat(path.c_str(), &info) != 0)
		{
			throw std::system_error(errno, std::generic_category(), "could not stat paraLLEl-GS input FIFO");
		}
		if (!S_ISFIFO(info.st_mode))
		{
			throw std::runtime_error("paraLLEl-GS input path is not a FIFO: " + path);
		}
		// ENXIO means the runtime's reader is not open yet; flush() retries.
		const int error = open_fifo();
		if (error != 0 && error != ENXIO)
		{
			throw std::system_error(error, std::generic_category(), "could not open paraLLEl-GS input FIFO");
		}
#endif
	}

	~Impl()
	{
#if !defined(_WIN32)
		if (fd >= 0)
		{
			::close(fd);
		}
#endif
	}

	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;

	void set_key(const std::uint32_t scancode, const bool down)
	{
		if (scancode == 0U || scancode >= kKeyCount || input.keys[scancode] == down)
		{
			return;
		}
		input.keys[scancode] = down;
		const std::uint32_t hotkey = hotkey_for_scancode(scancode);
		if (hotkey == 0U)
		{
			return;
		}
		if (down)
		{
			// The press is latched as a pulse so that a press and release inside
			// one flush interval still produce one rising edge in the reader.
			hotkeys_held |= hotkey;
			hotkeys_pulse |= hotkey;
		}
		else
		{
			hotkeys_held &= ~hotkey;
		}
	}

	void set_mouse_button(const std::uint32_t button, const bool down)
	{
		if (button < 1U || button > kMouseButtonCount)
		{
			return;
		}
		const auto mask = static_cast<std::uint8_t>(1U << (button - 1U));
		input.mouse_buttons = down ? static_cast<std::uint8_t>(input.mouse_buttons | mask)
		                           : static_cast<std::uint8_t>(input.mouse_buttons & ~mask);
	}

	void set_pointer(const float x, const float y, const bool inside)
	{
		if (!std::isfinite(x) || !std::isfinite(y))
		{
			return;
		}
		if (pointer_set && pointer.x == x && pointer.y == y && pointer.inside == inside)
		{
			return;
		}
		pointer.x = x;
		pointer.y = y;
		pointer.inside = inside;
		pointer.wheel = 0;
		pointer_set = true;
		pointer_dirty = true;
	}

	void focus(const bool focused)
	{
		if (!focused)
		{
			// Losing focus releases every held key, button and hotkey. Pending
			// press pulses are kept, so the press still reaches the runtime.
			input.keys.reset();
			input.mouse_buttons = 0U;
			hotkeys_held = 0U;
		}
	}

	void queue_text(const tp::TextInputKind kind, std::string text)
	{
		if (kind == tp::TextInputKind::Text && text.empty())
		{
			return;
		}
		text_queue.push_back(tp::TextInputEvent{kind, std::move(text)});
	}

	bool refresh_due(const Clock::time_point now) const noexcept
	{
		// The original renderer sends one full snapshot at connection and then
		// only changes; repeating snapshots disturbs the runtime (the intro
		// movies stall), so there is no periodic refresh.
		(void)now;
		return !refreshed;
	}

	void flush()
	{
		drain();
		const Clock::time_point now = Clock::now();
		if (outbox.size() < kMaxQueuedPackets)
		{
			queue_text_events();
			const bool refresh = refresh_due(now);
			queue_raw_snapshot(refresh);
			queue_pointer(refresh);
			if (refresh)
			{
				refreshed = true;
				last_refresh = now;
			}
		}
		if (quit_requested && !quit_queued)
		{
			outbox.push_back(outgoing_button_word(kQuitBit));
			quit_queued = true;
		}
		drain();
	}

	// Writes outbox packets in order until the pipe stops accepting bytes.
	void drain()
	{
		while (!outbox.empty())
		{
			OutgoingPacket &packet = outbox.front();
			const std::ptrdiff_t written = write_some(packet.bytes.data() + packet.sent, packet.size - packet.sent);
			if (written <= 0)
			{
				return;
			}
			packet.sent += static_cast<std::size_t>(written);
			if (packet.sent < packet.size)
			{
				return;
			}
			outbox.pop_front();
			++packets_written;
		}
	}

	std::ptrdiff_t write_some(const std::uint8_t *data, const std::size_t size)
	{
#if defined(_WIN32)
		if (!pipe)
		{
			return 0;
		}
		// Output::write returns 0 while no reader is connected and -1 on error.
		// Both leave the packet queued for the next flush.
		const int count = pipe->write(data, size);
		return count > 0 ? count : 0;
#else
		if (fd < 0 && open_fifo() != 0)
		{
			return 0;
		}
		const std::ptrdiff_t count = write_fifo(fd, data, size);
		return count > 0 ? count : 0;
#endif
	}

#if !defined(_WIN32)
	int open_fifo() noexcept
	{
		const int handle = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (handle < 0)
		{
			return errno;
		}
		fd = handle;
		return 0;
	}
#endif

	void queue_text_events()
	{
		while (!text_queue.empty() && outbox.size() < kMaxQueuedPackets)
		{
			const tp::TextInputEvent &event = text_queue.front();
			if (event.kind == tp::TextInputKind::Text)
			{
				queue_text_chunks(event.text);
			}
			else
			{
				outbox.push_back(outgoing(tp::encode_text_input(event.kind)));
			}
			text_queue.pop_front();
		}
	}

	// Splits text into packets of at most kMaxTextBytes, never inside a UTF-8
	// code point, so the reader sees every byte in order.
	void queue_text_chunks(std::string_view rest)
	{
		while (!rest.empty())
		{
			std::size_t length = std::min(rest.size(), kMaxTextBytes);
			while (length > 0U && length < rest.size() &&
			       (static_cast<unsigned char>(rest[length]) & 0xC0U) == 0x80U)
			{
				--length;
			}
			if (length == 0U)
			{
				length = std::min(rest.size(), kMaxTextBytes);
			}
			outbox.push_back(outgoing(tp::encode_text_input(tp::TextInputKind::Text, rest.substr(0, length))));
			rest.remove_prefix(length);
		}
	}

	void queue_raw_snapshot(const bool refresh)
	{
		// A press while the reader still holds the bit from an earlier press
		// (released and pressed again before a flush) is no rising edge on the
		// wire. Send the bit low first so the press becomes one.
		const std::uint32_t retrigger = hotkeys_pulse & generated_hotkeys;
		if (retrigger != 0U)
		{
			generated_hotkeys &= ~retrigger;
			outbox.push_back(outgoing(tp::encode_raw_input(generated_input, generated_hotkeys)));
		}
		const std::uint32_t hotkeys = hotkeys_held | hotkeys_pulse;
		const bool changed = input.keys != generated_input.keys ||
		                     input.mouse_buttons != generated_input.mouse_buttons ||
		                     hotkeys != generated_hotkeys;
		if (!changed && !refresh)
		{
			return;
		}
		outbox.push_back(outgoing(tp::encode_raw_input(input, hotkeys)));
		generated_input = input;
		generated_hotkeys = hotkeys;
		hotkeys_pulse = 0U;
	}

	// Each wheel step is carried by exactly one B3MP packet; the packets repeat
	// the latest pointer position. Pointer repeats carry no wheel steps.
	void queue_pointer(const bool refresh)
	{
		const bool emit = pointer_set && (pointer_dirty || refresh);
		if (!emit && wheel_pending == 0)
		{
			return;
		}
		bool first = true;
		while (first || (wheel_pending != 0 && outbox.size() < kMaxQueuedPackets))
		{
			first = false;
			const std::int64_t step = std::clamp<std::int64_t>(wheel_pending, -32768, 32767);
			wheel_pending -= step;
			tp::PointerSnapshot packet = pointer;
			packet.wheel = static_cast<std::int16_t>(step);
			outbox.push_back(outgoing(tp::encode_pointer(packet)));
		}
		pointer_dirty = false;
	}

	std::string path;
#if defined(_WIN32)
	std::unique_ptr<takedown::platform::windows_gs_pipe::Output> pipe;
#else
	int fd = -1;
#endif

	tp::KeyboardMouseSnapshot input{};
	std::uint32_t hotkeys_held = 0U;
	std::uint32_t hotkeys_pulse = 0U;

	tp::PointerSnapshot pointer{};
	bool pointer_set = false;
	bool pointer_dirty = false;
	std::int64_t wheel_pending = 0;

	std::deque<tp::TextInputEvent> text_queue;
	bool quit_requested = false;
	bool quit_queued = false;

	tp::KeyboardMouseSnapshot generated_input{};
	std::uint32_t generated_hotkeys = 0U;
	bool refreshed = false;
	Clock::time_point last_refresh{};

	std::deque<OutgoingPacket> outbox;
	std::uint64_t packets_written = 0U;
};

InputWriter::InputWriter(const std::string &path) : impl_(std::make_unique<Impl>(path)) {}

InputWriter::~InputWriter() = default;

void InputWriter::key(const std::uint32_t scancode, const bool down)
{
	impl_->set_key(scancode, down);
}

void InputWriter::mouse_button(const std::uint32_t button, const bool down)
{
	impl_->set_mouse_button(button, down);
}

void InputWriter::pointer(const float x, const float y, const bool inside)
{
	impl_->set_pointer(x, y, inside);
}

void InputWriter::wheel(const int steps)
{
	impl_->wheel_pending += steps;
}

void InputWriter::text(const std::string &utf8)
{
	impl_->queue_text(tp::TextInputKind::Text, utf8);
}

void InputWriter::text_backspace()
{
	impl_->queue_text(tp::TextInputKind::Backspace, {});
}

void InputWriter::text_submit()
{
	impl_->queue_text(tp::TextInputKind::Submit, {});
}

void InputWriter::text_cancel()
{
	impl_->queue_text(tp::TextInputKind::Cancel, {});
}

void InputWriter::request_quit()
{
	impl_->quit_requested = true;
}

void InputWriter::focus(const bool focused)
{
	impl_->focus(focused);
}

void InputWriter::flush()
{
	impl_->flush();
}

std::uint64_t InputWriter::packets_written() const
{
	return impl_->packets_written;
}

} // namespace TakedownRenderer
