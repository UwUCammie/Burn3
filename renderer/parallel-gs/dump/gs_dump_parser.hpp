#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <stdio.h>
#include "gs_registers.hpp"
#include "gs_renderer.hpp"
#include "image.hpp"

struct GsShm; // takedown/platform/gs_shm_ring.h

namespace ParallelGS
{
class GSInterface;

// Current GSdx state version.
static constexpr uint32_t STATE_VERSION = 9;
static constexpr uint32_t STATE_VERSION_MIN = 8;

enum class GSDumpPacketType : uint8_t
{
	Transfer = 0,
	Vsync = 1,
	ReadFIFO = 2,
	PrivRegisters = 3,
	// Takedown live-stream records (takedown/platform/parallel_gs_stream.hpp).
	NativeModel = 4,
	HostPresentation = 5,
	PaniniWarp = 6,
	HostUi = 7,
	NativeCmd = 8,   // takedown/platform/native_cmd_protocol.hpp
};

struct DumpHeader
{
	uint32_t version;
	uint32_t state_size;
	uint32_t serial_offset;
	uint32_t serial_size;
	uint32_t crc;
	uint32_t screenshot_width;
	uint32_t screenshot_height;
	uint32_t screenshot_offset;
	uint32_t screenshot_size;
};

class GSDumpParser
{
public:
	struct PaniniRecord
	{
		uint16_t tangent;
		uint8_t strength;
		uint8_t flags;
		uint16_t half_width;
	};

	bool open(const char *path, uint32_t vram_size, GSInterface *iface);
	bool open_raw(const char *path, uint32_t vram_size, GSInterface *iface);
	// Takes ownership of an already-open raw stream (e.g. a pipe) and closes it.
	bool open_raw_file(FILE *file, uint32_t vram_size, GSInterface *iface);
	// Takedown: the game moved the stream into a shared-memory ring (stream_pipe.hpp). Call after
	// open_raw_file(); the parser takes ownership of the ring and reads the stream from it instead of the
	// FILE, which stays open only as the connection to the game.
	void attach_ring(GsShm *ring);
	bool iterate_until_vsync(bool high_res_scanout = false, bool conservative_crtc = false);
	// Takedown: size of the high-resolution scanout (VSyncInfo::scanout_scale; 0 is 2x).
	void set_scanout_scale(uint32_t scale) { scanout_scale = scale; }
	ScanoutResult consume_vsync_result();
	// Raw pipes cannot be rewound: restart() fails for them.
	bool restart();

	// Payload of the latest HostPresentation record, and the number of such records read.
	const std::vector<uint8_t> &get_host_presentation() const { return host_presentation; }
	uint64_t get_host_presentation_serial() const { return host_presentation_serial; }
	// Payload of the latest HostUi record, and the number of such records read.
	const std::vector<uint8_t> &get_host_ui() const { return host_ui; }
	uint64_t get_host_ui_serial() const { return host_ui_serial; }

	// Invoked synchronously while parsing, at the record's position in the stream: after every
	// earlier Transfer was handed to the interface, before any later one.
	void set_panini_callback(std::function<void(const PaniniRecord &)> callback);

	// NS-3 native command records (kind 8): the payload, at its position in the stream. Without a callback a record is a
	// stream error (the game only sends them when the renderer announced the capability).
	void set_native_cmd_callback(std::function<void(const uint8_t *, size_t)> callback);

	// NativeModel payloads read since the last call, in stream order.
	std::vector<std::vector<uint8_t>> take_native_models();

	// Number of Vsync packets consumed so far, i.e. the raw VBlank index of the most recent one.
	uint64_t get_vsync_count() const { return vsync_count; }

	// True when the stream ended inside a packet, as opposed to a clean end at a packet boundary.
	bool was_truncated() const { return truncated; }

private:
	struct FileDeleter { void operator()(FILE *file) { if (file) fclose(file); } };
	struct RingDeleter { void operator()(GsShm *ring); };
	std::unique_ptr<GsShm, RingDeleter> ring;
	GSInterface *iface = nullptr;
	std::unique_ptr<FILE, FileDeleter> file;
	std::vector<GIFTagBits> gif_tag_buffer;
	ScanoutResult vsync_result = {};
	uint32_t scanout_scale = 0;
	uint32_t vram_size;
	bool is_raw = false;
	bool seekable = true;

	std::vector<uint8_t> host_presentation;
	uint64_t host_presentation_serial = 0;
	std::vector<uint8_t> host_ui;
	uint64_t host_ui_serial = 0;
	std::function<void(const PaniniRecord &)> panini_callback;
	std::function<void(const uint8_t *, size_t)> native_cmd_callback;
	std::vector<std::vector<uint8_t>> native_models;
	uint64_t vsync_count = 0;
	bool truncated = false;

	uint8_t read_u8();
	uint16_t read_u16();
	uint32_t read_u32();
	float read_f32();
	uint64_t read_u64();
	template <typename T>
	void read_reg(T &reg);
	bool read_bytes(void *data, size_t size);
	void read_data(void *data, size_t size);
	void read_payload(std::vector<uint8_t> &payload, uint32_t size);
	bool eof = false;

	void read_register_state();
	void read_skip(size_t size);
};
}
