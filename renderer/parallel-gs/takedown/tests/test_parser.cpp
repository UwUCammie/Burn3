// Parser test for the Takedown record types of the raw paraLLEl-GS stream
// (dump/gs_dump_parser). Replays synthetic streams through a headless Vulkan
// device and a GSInterface, the way tools/gs_dump_replayer.cpp does.

#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"
#include "gs_renderer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "global_managers_init.hpp"
#include "takedown/platform/parallel_gs_host_presentation.hpp"
#include "takedown/platform/parallel_gs_host_ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace Vulkan;
using namespace ParallelGS;

namespace
{
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kVramSize = 4 * 1024 * 1024;

int failures = 0;

#define CHECK(cond)                                                                 \
	do                                                                              \
	{                                                                               \
		if (!(cond))                                                                \
		{                                                                           \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);            \
			failures++;                                                             \
		}                                                                           \
	} while (0)

// Little-endian byte builder, matching the parser's native reads.
void put_bytes(Bytes &out, const void *data, size_t size)
{
	const auto *p = static_cast<const uint8_t *>(data);
	out.insert(out.end(), p, p + size);
}

void put_u8(Bytes &out, uint8_t v) { put_bytes(out, &v, sizeof(v)); }
void put_u16(Bytes &out, uint16_t v) { put_bytes(out, &v, sizeof(v)); }
void put_u32(Bytes &out, uint32_t v) { put_bytes(out, &v, sizeof(v)); }
void put_u64(Bytes &out, uint64_t v) { put_bytes(out, &v, sizeof(v)); }

Bytes host_presentation_payload()
{
	takedown::platform::ParallelGsHostPresentation presentation;
	presentation.widescreen = true;
	presentation.resolution_scale = 2;
	presentation.samples = 4;
	presentation.spatial_aa = 4;
	presentation.overlay.push_back({takedown::platform::ParallelGsOverlayStyle::Hint, "PRESS START"});
	return takedown::platform::encode_parallel_gs_host_presentation(presentation);
}

Bytes host_ui_payload()
{
	takedown::platform::ParallelGsUiItem item;
	item.kind = takedown::platform::ParallelGsUiKind::Text;
	item.x = 12;
	item.y = 34;
	item.width = 200;
	item.height = 16;
	item.text = "MODS";
	takedown::platform::ParallelGsHostUi ui;
	ui.items.push_back(item);
	return takedown::platform::encode_parallel_gs_host_ui(ui);
}

void put_host_presentation(Bytes &out, const Bytes &payload)
{
	put_u8(out, uint8_t(GSDumpPacketType::HostPresentation));
	put_u32(out, uint32_t(payload.size()));
	put_bytes(out, payload.data(), payload.size());
}

void put_host_ui(Bytes &out, const Bytes &payload)
{
	put_u8(out, uint8_t(GSDumpPacketType::HostUi));
	put_u32(out, uint32_t(payload.size()));
	put_bytes(out, payload.data(), payload.size());
}

void put_panini(Bytes &out, uint16_t tangent, uint8_t strength, uint8_t flags, uint16_t half_width)
{
	put_u8(out, uint8_t(GSDumpPacketType::PaniniWarp));
	put_u32(out, 6);
	put_u16(out, tangent);
	put_u8(out, strength);
	put_u8(out, flags);
	put_u16(out, half_width);
}

void put_native_model(Bytes &out, const Bytes &payload)
{
	put_u8(out, uint8_t(GSDumpPacketType::NativeModel));
	put_u32(out, uint32_t(payload.size()));
	put_bytes(out, payload.data(), payload.size());
}

void put_priv_registers(Bytes &out)
{
	put_u8(out, uint8_t(GSDumpPacketType::PrivRegisters));
	Bytes zeros(0x2000, 0);
	put_bytes(out, zeros.data(), zeros.size());
}

void put_vsync(Bytes &out, uint8_t phase)
{
	put_u8(out, uint8_t(GSDumpPacketType::Vsync));
	put_u8(out, phase);
}

// One path-3 Transfer holding a single A+D write of `value` to PRMODECONT.
void put_prmodecont_transfer(Bytes &out, uint64_t value)
{
	GIFTagBits tag = {};
	tag.NLOOP = 1;
	tag.EOP = 1;
	tag.NREG = 1;
	tag.REGS = uint64_t(GIFAddr::A_D);

	Bytes qwords;
	put_bytes(qwords, &tag, sizeof(tag));
	put_u64(qwords, value);                               // A+D data
	put_u64(qwords, uint64_t(RegisterAddr::PRMODECONT));  // A+D address, low 7 bits

	put_u8(out, uint8_t(GSDumpPacketType::Transfer));
	put_u8(out, 3);
	put_u32(out, uint32_t(qwords.size()));
	put_bytes(out, qwords.data(), qwords.size());
}

const Bytes kNativeModelA = {1, 2, 3, 4, 5};
const Bytes kNativeModelB = {9, 8, 7};
const uint16_t kTangentA = 0x1234;
const uint16_t kTangentB = 0x0100;

// Clean stream: two frames. The first Vsync has no Transfer before it, so the
// first iterate_until_vsync() call returns only after the second Vsync.
Bytes build_stream(uint64_t prmodecont_sentinel, bool with_truncated_tail)
{
	Bytes out;
	put_host_presentation(out, host_presentation_payload());
	put_host_ui(out, host_ui_payload());
	put_panini(out, kTangentA, 0x55, 0x01, 0x0BB8);
	put_native_model(out, kNativeModelA);
	put_priv_registers(out);
	put_vsync(out, 0);
	put_prmodecont_transfer(out, prmodecont_sentinel);
	put_panini(out, kTangentB, 0x03, 0x00, 0x0001);
	put_native_model(out, kNativeModelB);
	put_vsync(out, 1);
	if (with_truncated_tail)
	{
		// A Transfer header claiming 4096 bytes, followed by only 16.
		put_u8(out, uint8_t(GSDumpPacketType::Transfer));
		put_u8(out, 1);
		put_u32(out, 4096);
		put_u64(out, 0x1122334455667788ull);
		put_u64(out, 0x99aabbccddeeff00ull);
	}
	return out;
}

std::filesystem::path temp_path(const char *name)
{
	return std::filesystem::temp_directory_path() / (std::string("takedown-parser-test-") + name + ".gs");
}

bool write_file(const std::filesystem::path &path, const Bytes &bytes)
{
	FILE *f = std::fopen(path.string().c_str(), "wb");
	if (!f)
		return false;
	bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return std::fclose(f) == 0 && ok;
}

struct PaniniEvent
{
	uint16_t tangent;
	uint8_t strength;
	uint8_t flags;
	uint16_t half_width;
	uint64_t vsyncs_seen;
	uint64_t prmodecont;
};

// Records what the callback sees: the Vsync count at its position in the
// stream and the PRMODECONT state left by the Transfers before it.
void attach_panini_callback(GSDumpParser &parser, GSInterface &iface, std::vector<PaniniEvent> &events)
{
	parser.set_panini_callback([&parser, &iface, &events](const GSDumpParser::PaniniRecord &record) {
		events.push_back(PaniniEvent{record.tangent, record.strength, record.flags, record.half_width,
		                             parser.get_vsync_count(), iface.get_register_state().prmodecont.bits});
	});
}

void check_panini_events(const std::vector<PaniniEvent> &events, uint64_t sentinel, size_t offset)
{
	CHECK(events.size() >= offset + 2);
	if (events.size() < offset + 2)
		return;
	const auto &first = events[offset];
	const auto &second = events[offset + 1];
	CHECK(first.tangent == kTangentA);
	CHECK(first.strength == 0x55);
	CHECK(first.flags == 0x01);
	CHECK(first.half_width == 0x0BB8);
	CHECK(first.vsyncs_seen == 0);
	// Before the Transfer: the sentinel has not been written yet.
	CHECK(first.prmodecont != sentinel);
	CHECK(second.tangent == kTangentB);
	CHECK(second.strength == 0x03);
	CHECK(second.flags == 0x00);
	CHECK(second.half_width == 0x0001);
	CHECK(second.vsyncs_seen == 1);
	// After the Transfer and before the second Vsync.
	CHECK(second.prmodecont == sentinel);
}

void check_native_models(const std::vector<Bytes> &models)
{
	CHECK(models.size() == 2);
	if (models.size() != 2)
		return;
	CHECK(models[0] == kNativeModelA);
	CHECK(models[1] == kNativeModelB);
}

void check_host_records(GSDumpParser &parser)
{
	CHECK(parser.get_host_presentation() == host_presentation_payload());
	CHECK(parser.get_host_presentation_serial() == 1);
	CHECK(parser.get_host_ui() == host_ui_payload());
	CHECK(parser.get_host_ui_serial() == 1);
}

// Clean stream through open_raw(path), including restart() on a regular file.
bool test_clean_file_open_raw(GSInterface &iface)
{
	const uint64_t sentinel = 0x5a5a;
	const auto path = temp_path("clean");
	CHECK(write_file(path, build_stream(sentinel, false)));

	GSDumpParser parser;
	CHECK(parser.open_raw(path.string().c_str(), kVramSize, &iface));
	std::vector<PaniniEvent> events;
	attach_panini_callback(parser, iface, events);

	CHECK(parser.iterate_until_vsync());
	CHECK(parser.get_vsync_count() == 2);
	check_panini_events(events, sentinel, 0);
	check_host_records(parser);
	check_native_models(parser.take_native_models());
	CHECK(parser.take_native_models().empty());
	CHECK(!parser.was_truncated());

	// The clean end is not a truncation.
	CHECK(!parser.iterate_until_vsync());
	CHECK(!parser.was_truncated());
	CHECK(events.size() == 2);

	// A regular file rewinds, and the counters start over.
	CHECK(parser.restart());
	CHECK(parser.iterate_until_vsync());
	CHECK(parser.get_vsync_count() == 2);
	CHECK(events.size() == 4);
	if (events.size() == 4)
	{
		CHECK(events[2].vsyncs_seen == 0);
		CHECK(events[3].vsyncs_seen == 1);
	}
	check_native_models(parser.take_native_models());

	std::filesystem::remove(path);
	return failures == 0;
}

// Same stream plus a truncated tail, through open_raw_file() with a FILE*
// from fopen(). The tail must not reach the interface or the callback.
void test_truncated_file_open_raw_file(GSInterface &iface)
{
	const uint64_t sentinel = 0x6b6b;
	const auto path = temp_path("truncated");
	CHECK(write_file(path, build_stream(sentinel, true)));

	GSDumpParser parser;
	CHECK(parser.open_raw_file(std::fopen(path.string().c_str(), "rb"), kVramSize, &iface));
	std::vector<PaniniEvent> events;
	attach_panini_callback(parser, iface, events);

	// The first call stops at the second Vsync, before the tail.
	CHECK(parser.iterate_until_vsync());
	CHECK(!parser.was_truncated());
	CHECK(parser.get_vsync_count() == 2);
	check_panini_events(events, sentinel, 0);
	check_host_records(parser);
	check_native_models(parser.take_native_models());

	// The next call reaches the tail and reports truncation.
	CHECK(!parser.iterate_until_vsync());
	CHECK(parser.was_truncated());
	CHECK(parser.get_vsync_count() == 2);
	CHECK(events.size() == 2);

	std::filesystem::remove(path);
}

#ifndef _WIN32
// Clean stream through a pipe: parsing works without seeking, restart() fails.
void test_pipe(GSInterface &iface)
{
	const uint64_t sentinel = 0x7c7c;
	const Bytes stream = build_stream(sentinel, false);

	int fds[2] = {-1, -1};
	CHECK(pipe(fds) == 0);
	if (fds[0] < 0 || fds[1] < 0)
		return;
	// The stream is far below the pipe buffer, so writing it before reading does not block.
	CHECK(write(fds[1], stream.data(), stream.size()) == ssize_t(stream.size()));
	close(fds[1]);

	GSDumpParser parser;
	CHECK(parser.open_raw_file(fdopen(fds[0], "rb"), kVramSize, &iface));
	std::vector<PaniniEvent> events;
	attach_panini_callback(parser, iface, events);

	CHECK(parser.iterate_until_vsync());
	CHECK(parser.get_vsync_count() == 2);
	check_panini_events(events, sentinel, 0);
	check_host_records(parser);
	check_native_models(parser.take_native_models());
	CHECK(!parser.iterate_until_vsync());
	CHECK(!parser.was_truncated());
	CHECK(!parser.restart());
}
#endif

void expect_parse_error(GSInterface &iface, const char *name, const Bytes &stream, const char *message)
{
	const auto path = temp_path(name);
	CHECK(write_file(path, stream));

	GSDumpParser parser;
	CHECK(parser.open_raw(path.string().c_str(), kVramSize, &iface));
	std::string what;
	try
	{
		parser.iterate_until_vsync();
	}
	catch (const std::runtime_error &error)
	{
		what = error.what();
	}
	CHECK(what == message);

	std::filesystem::remove(path);
}

// Budgets and the Panini size check throw before the payload is read.
void test_budgets(GSInterface &iface)
{
	Bytes presentation_over;
	put_u8(presentation_over, uint8_t(GSDumpPacketType::HostPresentation));
	put_u32(presentation_over, 64 * 1024 + 1);
	expect_parse_error(iface, "presentation-over", presentation_over, "host presentation record exceeds budget");

	Bytes ui_over;
	put_u8(ui_over, uint8_t(GSDumpPacketType::HostUi));
	put_u32(ui_over, 512 * 1024 + 1);
	expect_parse_error(iface, "ui-over", ui_over, "host UI record exceeds budget");

	Bytes model_over;
	put_u8(model_over, uint8_t(GSDumpPacketType::NativeModel));
	put_u32(model_over, 32 * 1024 * 1024 + 1);
	expect_parse_error(iface, "model-over", model_over, "native model stream record exceeds budget");

	Bytes panini_bad;
	put_u8(panini_bad, uint8_t(GSDumpPacketType::PaniniWarp));
	put_u32(panini_bad, 7);
	Bytes filler(7, 0);
	put_bytes(panini_bad, filler.data(), filler.size());
	expect_parse_error(iface, "panini-bad", panini_bad, "malformed Panini warp record");

	// The exact presentation budget is accepted.
	Bytes presentation_exact;
	Bytes exact_payload(64 * 1024, 0x42);
	put_host_presentation(presentation_exact, exact_payload);
	const auto path = temp_path("presentation-exact");
	CHECK(write_file(path, presentation_exact));
	GSDumpParser parser;
	CHECK(parser.open_raw(path.string().c_str(), kVramSize, &iface));
	CHECK(!parser.iterate_until_vsync());
	CHECK(!parser.was_truncated());
	CHECK(parser.get_host_presentation() == exact_payload);
	CHECK(parser.get_host_presentation_serial() == 1);
	std::filesystem::remove(path);
}

// GIF Transfers are whole quadwords. A size that is not a multiple of 16 is malformed, and
// it must throw before the payload is read into a buffer sized for fewer bytes.
void test_transfer_not_quadword_sized(GSInterface &iface)
{
	Bytes eight;
	put_u8(eight, uint8_t(GSDumpPacketType::Transfer));
	put_u8(eight, 0);
	put_u32(eight, 8);
	Bytes filler(8, 0);
	put_bytes(eight, filler.data(), filler.size());
	expect_parse_error(iface, "transfer-8", eight, "malformed GIF transfer record");

	Bytes seventeen;
	put_u8(seventeen, uint8_t(GSDumpPacketType::Transfer));
	put_u8(seventeen, 0);
	put_u32(seventeen, 17);
	Bytes filler17(17, 0);
	put_bytes(seventeen, filler17.data(), filler17.size());
	expect_parse_error(iface, "transfer-17", seventeen, "malformed GIF transfer record");
}
} // namespace

int main()
{
	if (!Context::init_loader(nullptr))
		return EXIT_FAILURE;

	Context ctx;
	ctx.set_num_thread_indices(1);
	if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
	                                  CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
	                                  CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
	                                  CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
		return EXIT_FAILURE;

	Device device;
	device.set_context(ctx);
	device.init_frame_contexts(4);

	GSInterface iface;
	GSOptions opts = {};
	if (!iface.init(&device, opts))
		return EXIT_FAILURE;

	test_clean_file_open_raw(iface);
	test_truncated_file_open_raw_file(iface);
#ifndef _WIN32
	test_pipe(iface);
#endif
	test_budgets(iface);
	test_transfer_not_quadword_sized(iface);

	if (failures != 0)
	{
		std::fprintf(stderr, "takedown-parser-tests: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("takedown-parser-tests: all checks passed\n");
	return EXIT_SUCCESS;
}
