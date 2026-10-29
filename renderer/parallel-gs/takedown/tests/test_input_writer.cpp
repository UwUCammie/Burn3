// Tests for the renderer-side input FIFO writer (input_writer.cpp).
//
// The writer is driven over a real FIFO and every packet is decoded with the
// runtime's header helpers (raw_input_protocol.hpp).
//
//   C++17 (writer only):
//     g++ -std=c++17 ... input_writer.cpp tests/test_input_writer.cpp
//   C++20 (also cross-checks against the real runtime reader):
//     g++ -std=c++20 ... input_writer.cpp tests/test_input_writer.cpp
//         <repo>/src/platform/parallel_gs_input.cpp
//
// parallel_gs_input.hpp includes window.hpp, which needs C++20 (std::span), so
// the reader cross-check is only compiled when __cplusplus >= 202002L.
#include "input_writer.hpp"

#include "takedown/platform/raw_input_protocol.hpp"

#if __cplusplus >= 202002L
#include "takedown/platform/parallel_gs_input.hpp"
#define TAKEDOWN_TEST_HAS_READER 1
#else
#define TAKEDOWN_TEST_HAS_READER 0
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tp = takedown::platform;
using TakedownRenderer::InputWriter;
using Bytes = std::vector<std::uint8_t>;

namespace
{
int g_failures = 0;

void check(const bool ok, const std::string &what)
{
	if (!ok)
	{
		++g_failures;
		std::cerr << "FAIL: " << what << '\n';
	}
}

// SDL scancodes (USB HID usages).
constexpr std::uint32_t kKeyA = 4U;
constexpr std::uint32_t kKeyF6 = 63U;
constexpr std::uint32_t kKeyF7 = 64U;
constexpr std::uint32_t kKeyF8 = 65U;

// B3IN/B3RI hotkey bits.
constexpr std::uint32_t kStepBit = 1U << 16U;
constexpr std::uint32_t kAdvanceBit = 1U << 17U;
constexpr std::uint32_t kCaptureBit = 1U << 18U;
constexpr std::uint32_t kQuitBit = 1U << 19U;

const std::string kScratchDir = (std::filesystem::temp_directory_path() / "takedown-input-writer-test").string();

// Creates a FIFO and removes it again when the test ends.
class Fifo
{
public:
	explicit Fifo(const std::string &name)
	    : path_(kScratchDir + "/" + name + "-" + std::to_string(::getpid()) + ".fifo")
	{
		std::filesystem::create_directories(kScratchDir);
		::unlink(path_.c_str());
		if (::mkfifo(path_.c_str(), 0600) != 0)
		{
			throw std::runtime_error("mkfifo failed: " + path_);
		}
	}
	~Fifo()
	{
		::unlink(path_.c_str());
	}
	Fifo(const Fifo &) = delete;
	Fifo &operator=(const Fifo &) = delete;
	const std::string &path() const
	{
		return path_;
	}

private:
	std::string path_;
};

// The read end, opened non-blocking the way the runtime opens its reader.
class ReadEnd
{
public:
	explicit ReadEnd(const std::string &path) : fd_(::open(path.c_str(), O_RDONLY | O_NONBLOCK))
	{
		if (fd_ < 0)
		{
			throw std::runtime_error("could not open read end: " + path);
		}
	}
	~ReadEnd()
	{
		::close(fd_);
	}
	ReadEnd(const ReadEnd &) = delete;
	ReadEnd &operator=(const ReadEnd &) = delete;
	int fd() const
	{
		return fd_;
	}

private:
	int fd_;
};

std::uint32_t word_at(const Bytes &packet, const std::size_t at)
{
	return std::uint32_t(packet[at]) | (std::uint32_t(packet[at + 1]) << 8U) |
	       (std::uint32_t(packet[at + 2]) << 16U) | (std::uint32_t(packet[at + 3]) << 24U);
}

std::string magic_of(const Bytes &packet)
{
	return std::string(packet.begin(), packet.begin() + 4);
}

std::vector<Bytes> of_kind(const std::vector<Bytes> &packets, const std::string &magic)
{
	std::vector<Bytes> out;
	for (const Bytes &packet : packets)
	{
		if (magic_of(packet) == magic)
		{
			out.push_back(packet);
		}
	}
	return out;
}

struct Snapshot
{
	tp::KeyboardMouseSnapshot input;
	std::uint32_t hotkeys;
};

Snapshot decode_snapshot(const Bytes &packet)
{
	return Snapshot{tp::decode_raw_input(packet.data()), word_at(packet, 4)};
}

struct Text
{
	tp::TextInputKind kind;
	std::string text;
};

Text decode_text(const Bytes &packet)
{
	return Text{static_cast<tp::TextInputKind>(packet[4]),
	            std::string(reinterpret_cast<const char *>(packet.data() + 8), packet[5])};
}

// True when the piece is a sequence of complete UTF-8 code points.
bool whole_utf8(const std::string &piece)
{
	std::size_t at = 0U;
	while (at < piece.size())
	{
		const auto lead = static_cast<unsigned char>(piece[at]);
		std::size_t length = 0U;
		if (lead < 0x80U)
		{
			length = 1U;
		}
		else if ((lead >> 5U) == 0x6U)
		{
			length = 2U;
		}
		else if ((lead >> 4U) == 0xEU)
		{
			length = 3U;
		}
		else if ((lead >> 3U) == 0x1EU)
		{
			length = 4U;
		}
		if (length == 0U || at + length > piece.size())
		{
			return false;
		}
		at += length;
	}
	return true;
}

int wheel_sum(const std::vector<Bytes> &pointers)
{
	int sum = 0;
	for (const Bytes &packet : pointers)
	{
		sum += tp::decode_pointer(packet.data()).wheel;
	}
	return sum;
}

// Splits a stream read from the FIFO into whole packets.
std::vector<Bytes> split_packets(const Bytes &stream)
{
	std::vector<Bytes> packets;
	std::size_t at = 0U;
	while (at < stream.size())
	{
		const std::string magic(stream.begin() + static_cast<std::ptrdiff_t>(at),
		                        stream.begin() + static_cast<std::ptrdiff_t>(std::min(at + 4U, stream.size())));
		std::size_t size = 0U;
		if (magic == "B3IN")
		{
			size = 8U;
		}
		else if (magic == "B3RI" || magic == "B3TX" || magic == "B3MP")
		{
			size = 76U;
		}
		else
		{
			check(false, "stream holds only known packet kinds");
			return packets;
		}
		if (stream.size() - at < size)
		{
			check(false, "stream holds only whole packets");
			return packets;
		}
		packets.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(at),
		                     stream.begin() + static_cast<std::ptrdiff_t>(at + size));
		at += size;
	}
	return packets;
}

// Reads whatever the writer has put into the pipe so far.
class Capture
{
public:
	explicit Capture(const int fd) : fd_(fd) {}

	// Flushes the writer, then returns the packets that arrived.
	std::vector<Bytes> pump(InputWriter &writer)
	{
		writer.flush();
		return take();
	}

	std::vector<Bytes> take()
	{
		Bytes stream;
		std::uint8_t buffer[4096];
		for (;;)
		{
			const ssize_t got = ::read(fd_, buffer, sizeof buffer);
			if (got <= 0)
			{
				break;
			}
			stream.insert(stream.end(), buffer, buffer + got);
		}
		std::vector<Bytes> packets = split_packets(stream);
		all_.insert(all_.end(), packets.begin(), packets.end());
		return packets;
	}

	const std::vector<Bytes> &all() const
	{
		return all_;
	}

private:
	int fd_;
	std::vector<Bytes> all_;
};

void test_rejects_bad_paths()
{
	std::filesystem::create_directories(kScratchDir);
	bool threw = false;
	try
	{
		InputWriter writer(kScratchDir + "/does-not-exist.fifo");
	}
	catch (const std::exception &)
	{
		threw = true;
	}
	check(threw, "a missing FIFO path throws");

	const std::string regular = kScratchDir + "/regular-file.txt";
	{
		std::ofstream(regular) << "not a fifo";
	}
	threw = false;
	try
	{
		InputWriter writer(regular);
	}
	catch (const std::exception &)
	{
		threw = true;
	}
	check(threw, "a regular file is rejected as an input FIFO");
	std::filesystem::remove(regular);
}

// The writer is created before the reader exists (ENXIO must not throw), and
// the queued state is delivered once the reader opens.
void test_writer_before_reader()
{
	const Fifo fifo("late");
	InputWriter writer(fifo.path());
	writer.key(kKeyA, true);
	writer.flush();
	check(writer.packets_written() == 0U, "nothing is written before a reader opens");

	const ReadEnd reader(fifo.path());
	Capture capture(reader.fd());
	const auto snapshots = of_kind(capture.pump(writer), "B3RI");
	check(snapshots.size() == 1U, "the queued snapshot is delivered once the reader opens");
	if (!snapshots.empty())
	{
		check(decode_snapshot(snapshots.back()).input.keys[kKeyA], "the delivered snapshot holds the held key");
	}
	check(writer.packets_written() == 1U, "the delivered snapshot is counted as written");
}

void test_keys_and_hotkeys()
{
	const Fifo fifo("keys");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer); // initial all-released snapshot

	writer.key(kKeyA, true);
	writer.key(kKeyF8, true);
	auto snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty(), "a key press produces a B3RI snapshot");
	if (!snapshots.empty())
	{
		const Snapshot raw = decode_snapshot(snapshots.back());
		check(raw.input.keys[kKeyA] && raw.input.keys[kKeyF8], "held keys are in the snapshot");
		check(raw.hotkeys == kCaptureBit, "F8 press sets only the Capture hotkey bit");
		check(raw.input.mouse_buttons == 0U, "no mouse buttons are reported");
	}

	writer.key(kKeyA, false);
	writer.key(kKeyF8, false);
	snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty(), "key releases produce a B3RI snapshot");
	if (!snapshots.empty())
	{
		const Snapshot raw = decode_snapshot(snapshots.back());
		check(raw.input.keys.none(), "released keys are cleared in the snapshot");
		check(raw.hotkeys == 0U, "F8 release clears the Capture hotkey bit");
	}

	// F6 and F7 pressed and released inside one flush interval still produce
	// rising edges, and the cleared state follows on the next flush.
	writer.key(kKeyF6, true);
	writer.key(kKeyF7, true);
	writer.key(kKeyF6, false);
	writer.key(kKeyF7, false);
	snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty() && decode_snapshot(snapshots.back()).hotkeys == (kStepBit | kAdvanceBit),
	      "a quick F6 and F7 press latches both hotkey bits once");
	snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty() && decode_snapshot(snapshots.back()).hotkeys == 0U,
	      "the latched hotkey bits are cleared on the next flush");
}

void test_mouse_pointer_wheel()
{
	const Fifo fifo("pointer");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	writer.mouse_button(1, true);
	writer.mouse_button(3, true);
	auto snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty() && decode_snapshot(snapshots.back()).input.mouse_buttons == 5U,
	      "left and right buttons are bits 0 and 2");
	writer.mouse_button(1, false);
	snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty() && decode_snapshot(snapshots.back()).input.mouse_buttons == 4U,
	      "releasing the left button clears only bit 0");

	writer.pointer(0.25F, 0.75F, true);
	auto pointers = of_kind(capture.pump(writer), "B3MP");
	check(!pointers.empty(), "a pointer move produces a B3MP packet");
	if (!pointers.empty())
	{
		const tp::PointerSnapshot pointer = tp::decode_pointer(pointers.back().data());
		check(pointer.x == 0.25F && pointer.y == 0.75F && pointer.inside, "pointer position and inside flag arrive");
		check(pointer.wheel == 0, "a position-only packet carries no wheel steps");
	}

	writer.wheel(3);
	writer.wheel(-1);
	writer.wheel(40000);
	pointers = of_kind(capture.pump(writer), "B3MP");
	check(wheel_sum(pointers) == 40002, "queued wheel steps are all delivered, none merged away");
	check(pointers.size() >= 2U, "wheel steps beyond the 16-bit field are split across packets");
	if (!pointers.empty())
	{
		const tp::PointerSnapshot pointer = tp::decode_pointer(pointers.back().data());
		check(pointer.x == 0.25F && pointer.y == 0.75F, "wheel packets repeat the latest pointer position");
	}

	writer.pointer(0.5F, 0.5F, false);
	writer.wheel(-32768);
	pointers = of_kind(capture.pump(writer), "B3MP");
	check(wheel_sum(pointers) == -32768, "a negative wheel step is delivered exactly");
	if (!pointers.empty())
	{
		const tp::PointerSnapshot pointer = tp::decode_pointer(pointers.back().data());
		check(pointer.x == 0.5F && pointer.y == 0.5F && !pointer.inside, "the pointer moved outside the picture");
	}
}

// A hotkey released and pressed again before the next flush: the reader still
// holds the bit from the first press, so the writer must send it low before the
// new press or the second press is not a rising edge.
void test_hotkey_retap_before_flush()
{
	const Fifo fifo("retap");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	writer.key(kKeyF8, true);
	capture.pump(writer); // the capture bit is high on the wire now

	writer.key(kKeyF8, false);
	writer.key(kKeyF8, true);
	const auto snapshots = of_kind(capture.pump(writer), "B3RI");
	int rising = 0;
	std::uint32_t previous = kCaptureBit;
	for (const Bytes &packet : snapshots)
	{
		const std::uint32_t hotkeys = word_at(packet, 4);
		if ((hotkeys & kCaptureBit) != 0U && (previous & kCaptureBit) == 0U)
		{
			++rising;
		}
		previous = hotkeys;
	}
	check(rising == 1, "a press after a release before the flush is one rising edge");
	check(!snapshots.empty() && decode_snapshot(snapshots.back()).hotkeys == kCaptureBit,
	      "the last snapshot holds the new press");
}

void test_text()
{
	const Fifo fifo("text");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	writer.text("h\xc3\xa9llo");
	writer.text_backspace();
	writer.text("x");
	writer.text_submit();
	writer.text_cancel();
	auto records = of_kind(capture.pump(writer), "B3TX");
	check(records.size() == 5U, "each text event is one B3TX packet");
	if (records.size() == 5U)
	{
		const Text a = decode_text(records[0]);
		const Text b = decode_text(records[1]);
		const Text c = decode_text(records[2]);
		const Text d = decode_text(records[3]);
		const Text e = decode_text(records[4]);
		check(a.kind == tp::TextInputKind::Text && a.text == "h\xc3\xa9llo", "UTF-8 text is delivered intact");
		check(b.kind == tp::TextInputKind::Backspace, "backspace is delivered in order");
		check(c.kind == tp::TextInputKind::Text && c.text == "x", "text after backspace is kept");
		check(d.kind == tp::TextInputKind::Submit, "submit is delivered in order");
		check(e.kind == tp::TextInputKind::Cancel, "cancel is delivered in order");
	}

	const std::string long_text(200U, 'a');
	writer.text(long_text);
	records = of_kind(capture.pump(writer), "B3TX");
	std::string joined;
	bool sized = true;
	for (const Bytes &packet : records)
	{
		sized = sized && packet[5] <= 68U;
		joined += decode_text(packet).text;
	}
	check(sized, "every text packet carries at most 68 bytes");
	check(records.size() >= 3U && joined == long_text, "long text is split and rejoined without loss");

	std::string euro;
	for (int i = 0; i < 30; ++i)
	{
		euro += "\xe2\x82\xac";
	}
	writer.text(euro);
	records = of_kind(capture.pump(writer), "B3TX");
	joined.clear();
	bool whole_code_points = true;
	for (const Bytes &packet : records)
	{
		const std::string piece = decode_text(packet).text;
		whole_code_points = whole_code_points && !piece.empty() && whole_utf8(piece);
		joined += piece;
	}
	check(whole_code_points, "text is never split inside a UTF-8 code point");
	check(joined == euro, "multi-byte text is rejoined exactly");
}

// A wheel delta too large for one flush is spread over several flushes: one flush
// must not drain the whole burst (611 packets for 20000000 steps), and every step
// still arrives.
void test_wheel_spread_over_flushes()
{
	const Fifo fifo("wheelspread");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	const int total = 20000000;
	writer.wheel(total);
	writer.flush();
	check(writer.packets_written() < 611U, "one flush does not drain a 20000000-step wheel burst");
	int delivered = wheel_sum(of_kind(capture.take(), "B3MP"));
	for (int round = 0; round < 20 && delivered != total; ++round)
	{
		delivered += wheel_sum(of_kind(capture.pump(writer), "B3MP"));
	}
	check(delivered == total, "a large wheel delta arrives in full across flushes");
}

void test_quit()
{
	const Fifo fifo("quit");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	writer.request_quit();
	auto quits = of_kind(capture.pump(writer), "B3IN");
	check(quits.size() == 1U, "request_quit sends one B3IN packet");
	if (!quits.empty())
	{
		check(quits.front().size() == 8U, "the B3IN packet is 8 bytes");
		check(word_at(quits.front(), 4) == kQuitBit, "the B3IN packet carries only the Quit bit");
	}

	writer.request_quit();
	quits = of_kind(capture.pump(writer), "B3IN");
	check(quits.empty(), "a second request_quit does not send another edge");
}

void test_focus_loss()
{
	const Fifo fifo("focus");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	capture.pump(writer);

	writer.key(kKeyA, true);
	writer.mouse_button(1, true);
	writer.key(kKeyF6, true);
	auto snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty(), "the held state is sent before focus is lost");
	if (!snapshots.empty())
	{
		const Snapshot raw = decode_snapshot(snapshots.back());
		check(raw.input.keys[kKeyA] && raw.input.mouse_buttons == 1U && raw.hotkeys == kStepBit,
		      "key, button and hotkey are held before focus loss");
	}

	writer.focus(false);
	snapshots = of_kind(capture.pump(writer), "B3RI");
	check(!snapshots.empty(), "focus loss sends a released snapshot");
	if (!snapshots.empty())
	{
		const Snapshot raw = decode_snapshot(snapshots.back());
		check(raw.input.keys.none() && raw.input.mouse_buttons == 0U && raw.hotkeys == 0U,
		      "focus loss releases every held key, button and hotkey");
	}
}

void test_refresh()
{
	const Fifo fifo("refresh");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());
	writer.key(kKeyA, true);
	capture.pump(writer);

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const auto repeats = of_kind(capture.pump(writer), "B3RI");
	check(!repeats.empty(), "the full state is repeated periodically without any change");
	if (!repeats.empty())
	{
		check(decode_snapshot(repeats.back()).input.keys[kKeyA], "the repeated snapshot still holds the key");
	}
}

// Nothing is read for a while: flush() must never block, text and the final
// state must not be lost, and everything must arrive in order afterwards.
void test_backpressure()
{
	const Fifo fifo("pressure");
	const ReadEnd reader(fifo.path());
	InputWriter writer(fifo.path());
	Capture capture(reader.fd());

	std::string expected;
	for (int i = 0; i < 3000; ++i)
	{
		writer.key(kKeyA, i % 2 == 0);
		writer.flush();
	}
	for (int i = 0; i < 500; ++i)
	{
		const std::string piece = std::to_string(i) + ";";
		writer.text(piece);
		expected += piece;
	}

	std::string received;
	bool done = false;
	for (int round = 0; round < 200000 && !done; ++round)
	{
		for (const Bytes &packet : of_kind(capture.pump(writer), "B3TX"))
		{
			received += decode_text(packet).text;
		}
		done = received == expected && writer.packets_written() == capture.all().size();
	}
	// A few more rounds drain any state that was held back by the cap.
	for (int round = 0; round < 10; ++round)
	{
		for (const Bytes &packet : of_kind(capture.pump(writer), "B3TX"))
		{
			received += decode_text(packet).text;
		}
	}
	check(received == expected, "text survives back-pressure in order and without loss");
	check(writer.packets_written() == capture.all().size(), "every counted packet was read back");

	const auto snapshots = of_kind(capture.all(), "B3RI");
	check(!snapshots.empty() && !decode_snapshot(snapshots.back()).input.keys[kKeyA],
	      "the final key state is delivered after back-pressure");
}

#if TAKEDOWN_TEST_HAS_READER
// The runtime's own reader decodes what the writer sends.
void test_runtime_reader()
{
	const Fifo fifo("runtime");
	tp::ParallelGsInputReader reader(fifo.path());
	auto writer = std::make_unique<InputWriter>(fifo.path());

	writer->key(kKeyA, true);
	writer->key(kKeyF8, true);
	writer->flush();
	reader.poll();
	check(reader.has_raw_input(), "the first B3RI makes the runtime use raw input");
	const tp::HostControllerState pad = reader.controller_state();
	check(!pad.cross && !pad.dpad_up && !pad.start && !pad.left_trigger,
	      "raw keys leave the controller bits to the runtime's bindings");
	check(reader.raw_input().keys[kKeyA] && reader.raw_input().keys[kKeyF8], "raw keys reach the runtime");
	check(reader.consume_capture_request(), "an F8 press requests a capture");
	check(!reader.consume_capture_request(), "a capture request is consumed once");

	writer->key(kKeyF8, false);
	writer->flush();
	reader.poll();
	check(!reader.consume_capture_request(), "an F8 release is not a capture request");
	writer->key(kKeyF8, true);
	writer->flush();
	reader.poll();
	check(reader.consume_capture_request(), "a second F8 press requests a capture again");

	writer->key(kKeyF6, true);
	writer->key(kKeyF7, true);
	writer->flush();
	reader.poll();
	check(reader.consume_frame_step_toggle_request(), "F6 requests a frame-step toggle");
	check(reader.consume_frame_advance_request(), "F7 requests a frame advance");

	writer->mouse_button(2, true);
	writer->flush();
	reader.poll();
	check(reader.raw_input().mouse_buttons == 2U, "mouse button 2 reaches the runtime");

	writer->pointer(0.5F, 0.25F, true);
	writer->wheel(5);
	writer->wheel(-2);
	writer->flush();
	reader.poll();
	const tp::PointerSnapshot pointer = reader.pointer();
	check(pointer.x == 0.5F && pointer.y == 0.25F && pointer.inside, "pointer position reaches the runtime");
	check(reader.consume_wheel() == 3, "wheel steps are accumulated by the runtime");
	check(reader.consume_wheel() == 0, "wheel steps are consumed once");

	writer->text("hi");
	writer->text_backspace();
	writer->text_submit();
	writer->flush();
	reader.poll();
	const auto events = reader.consume_text_input();
	check(events.size() == 3U, "text, backspace and submit all reach the runtime");
	if (events.size() == 3U)
	{
		check(events[0].kind == tp::TextInputKind::Text && events[0].text == "hi", "text reaches the runtime");
		check(events[1].kind == tp::TextInputKind::Backspace, "backspace reaches the runtime in order");
		check(events[2].kind == tp::TextInputKind::Submit, "submit reaches the runtime in order");
	}

	writer->focus(false);
	writer->flush();
	reader.poll();
	check(reader.raw_input().keys.none() && reader.raw_input().mouse_buttons == 0U,
	      "focus loss releases the keys the runtime holds");

	writer->request_quit();
	writer->flush();
	reader.poll();
	check(reader.consume_close_request(), "request_quit requests shutdown in the runtime");
	check(!reader.consume_close_request(), "the close request is consumed once");

	// The writer going away is read by the runtime as the renderer closing.
	writer.reset();
	reader.poll();
	check(reader.consume_close_request(), "a vanished renderer requests shutdown in the runtime");
	check(reader.raw_input().keys.none(), "a vanished renderer releases every key");
}
#endif

} // namespace

int main()
{
	try
	{
		test_rejects_bad_paths();
		test_writer_before_reader();
		test_keys_and_hotkeys();
		test_mouse_pointer_wheel();
		test_hotkey_retap_before_flush();
		test_wheel_spread_over_flushes();
		test_text();
		test_quit();
		test_focus_loss();
		test_refresh();
		test_backpressure();
#if TAKEDOWN_TEST_HAS_READER
		test_runtime_reader();
		std::cout << "runtime reader cross-check: compiled in\n";
#else
		std::cout << "runtime reader cross-check: skipped (needs -std=c++20)\n";
#endif
	}
	catch (const std::exception &e)
	{
		std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
	}
	if (g_failures != 0)
	{
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "input writer tests passed\n";
	return 0;
}
