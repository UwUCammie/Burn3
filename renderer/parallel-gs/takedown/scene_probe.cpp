// takedown-scene-probe: replays a recorded stream (TAKEDOWN_DEV_STREAM_DUMP)
// headless and writes the GS picture and the native scene (native_scene.hpp)
// of chosen frames as PNGs, to compare them without running the game.
//
//   takedown-scene-probe <stream> <out-dir> <first> <every> <count> [scale]
//
// Frames are counted in vsyncs. The native scene runs for the 8 published
// frames before each written one too, so the effect buffers it keeps from
// frame to frame (motion blur, reflections) are warm.
#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"
#include "native_scene.hpp"
#include "device.hpp"
#include "context.hpp"
#include "global_managers_init.hpp"
#include "global_managers.hpp"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

using namespace Vulkan;
using namespace ParallelGS;

namespace
{
float half_to_float(uint16_t h)
{
	const uint32_t sign = uint32_t(h >> 15) << 31;
	uint32_t exponent = (h >> 10) & 31u;
	uint32_t mantissa = h & 1023u;
	uint32_t bits;
	if (exponent == 0)
	{
		if (mantissa == 0)
			bits = sign;
		else
		{
			exponent = 127 - 15 + 1;
			while (!(mantissa & 1024u))
			{
				mantissa <<= 1;
				exponent--;
			}
			bits = sign | (exponent << 23) | ((mantissa & 1023u) << 13);
		}
	}
	else if (exponent == 31)
		bits = sign | 0x7f800000u | (mantissa << 13);
	else
		bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

// Copies a shader-readable colour image to the host as RGBA floats.
std::vector<float> read_image(Device &device, const Image &image, VkImageLayout layout, uint32_t level = 0)
{
	const uint32_t w = image.get_width(level), h = image.get_height(level);
	const VkFormat format = image.get_format();
	const uint32_t texel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
	BufferCreateInfo info = {};
	info.size = VkDeviceSize(w) * h * texel;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.domain = BufferDomain::CachedHost;
	auto buffer = device.create_buffer(info);

	auto cmd = device.request_command_buffer();
	cmd->image_barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	                   VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	cmd->copy_image_to_buffer(*buffer, image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 });
	cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
	                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
	             VK_ACCESS_2_HOST_READ_BIT);
	device.submit(cmd);
	device.wait_idle();

	std::vector<float> rgba(size_t(w) * h * 4);
	const auto *data = static_cast<const uint8_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
	for (size_t i = 0; i < size_t(w) * h * 4; i++)
	{
		if (texel == 8)
		{
			uint16_t v;
			std::memcpy(&v, data + i * 2, 2);
			rgba[i] = half_to_float(v);
		}
		else
		{
			const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
			const size_t c = i & 3;
			const size_t src = bgra && c != 3 ? (i - c) + (2 - c) : i;
			rgba[i] = float(data[src]) / 255.0f;
		}
	}
	device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
	return rgba;
}

bool write_png(const std::string &path, const std::vector<float> &rgba, uint32_t w, uint32_t h, const float gain[3])
{
	std::vector<uint8_t> rgb(size_t(w) * h * 3);
	for (size_t i = 0; i < size_t(w) * h; i++)
		for (int c = 0; c < 3; c++)
			rgb[i * 3 + c] = uint8_t(std::lround(std::min(std::max(rgba[i * 4 + c] * gain[c], 0.0f), 1.0f) * 255.0f));
	return stbi_write_png(path.c_str(), int(w), int(h), 3, rgb.data(), int(w * 3)) != 0;
}

// The alpha channel as a grey picture: GS alpha byte = alpha * 128 for the
// native scene (RGBA16F, 128 = 1.0).
bool write_alpha_png(const std::string &path, const std::vector<float> &rgba, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> grey(size_t(w) * h);
	for (size_t i = 0; i < size_t(w) * h; i++)
		grey[i] = uint8_t(std::lround(std::min(std::max(rgba[i * 4 + 3] * 128.0f, 0.0f), 255.0f)));
	return stbi_write_png(path.c_str(), int(w), int(h), 1, grey.data(), int(w)) != 0;
}
} // namespace

#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <link.h>
#include <ucontext.h>
#include <cstring>
#include <execinfo.h>
#include <sys/time.h>

// PROBE_PROFILE=<file>: CPU-time samples of the main thread (every 2 ms), written
// as module offsets for addr2line at exit.
namespace
{
constexpr int kProfileDepth = 24;
constexpr size_t kProfileSamples = 400000;
void *profile_frames[kProfileSamples][kProfileDepth];
int profile_depths[kProfileSamples];
std::atomic<size_t> profile_count{ 0 };
// Modules the unwinder can walk (they have unwind tables); a sample taken in
// any other (the GPU driver) records only the interrupted address.
struct ProfileRange
{
	uintptr_t begin, end;
};
ProfileRange profile_ranges[16];
int profile_range_count = 0;
bool profile_unwindable(uintptr_t pc)
{
	for (int r = 0; r < profile_range_count; r++)
		if (pc >= profile_ranges[r].begin && pc < profile_ranges[r].end)
			return true;
	return false;
}
void profile_signal(int, siginfo_t *, void *context)
{
	const size_t i = profile_count.fetch_add(1);
	if (i >= kProfileSamples)
		return;
	const auto pc = uintptr_t(static_cast<ucontext_t *>(context)->uc_mcontext.gregs[REG_RIP]);
	if (profile_unwindable(pc))
		profile_depths[i] = backtrace(profile_frames[i], kProfileDepth);
	else
	{
		profile_frames[i][2] = reinterpret_cast<void *>(pc + 1);
		profile_depths[i] = 3;
	}
}
void profile_start()
{
	void *warm[4];
	backtrace(warm, 4); // loads the unwinder outside the signal handler
	dl_iterate_phdr([](dl_phdr_info *info, size_t, void *) -> int {
		const char *name = info->dlpi_name;
		const bool main_module = !name || !*name;
		const bool runtime = name && (std::strstr(name, "/libc.so") || std::strstr(name, "/libstdc++") ||
		                              std::strstr(name, "/libm.so") || std::strstr(name, "/libgcc_s"));
		if (!main_module && !runtime)
			return 0;
		for (int h = 0; h < info->dlpi_phnum && profile_range_count < 16; h++)
		{
			const auto &ph = info->dlpi_phdr[h];
			if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
				profile_ranges[profile_range_count++] = { uintptr_t(info->dlpi_addr + ph.p_vaddr),
				                                          uintptr_t(info->dlpi_addr + ph.p_vaddr + ph.p_memsz) };
		}
		return 0;
	}, nullptr);
	struct sigaction action = {};
	action.sa_sigaction = profile_signal;
	action.sa_flags = SA_RESTART | SA_SIGINFO;
	sigaction(SIGPROF, &action, nullptr);
	itimerval timer = {};
	timer.it_interval.tv_usec = 2000;
	timer.it_value.tv_usec = 2000;
	setitimer(ITIMER_PROF, &timer, nullptr);
}
void profile_write(const char *path)
{
	itimerval timer = {};
	setitimer(ITIMER_PROF, &timer, nullptr);
	std::FILE *out = std::fopen(path, "w");
	if (!out)
		return;
	const size_t n = std::min(profile_count.load(), kProfileSamples);
	for (size_t i = 0; i < n; i++)
	{
		for (int f = 2; f < profile_depths[i]; f++)
		{
			Dl_info info = {};
			if (dladdr(profile_frames[i][f], &info) && info.dli_fname)
				std::fprintf(out, "%s:%lx ", info.dli_fname,
				             (unsigned long)((uintptr_t)profile_frames[i][f] - (uintptr_t)info.dli_fbase - 1));
		}
		std::fprintf(out, "\n");
	}
	std::fclose(out);
}
}

int main(int argc, char **argv)
{
	if (argc < 6)
	{
		std::fprintf(stderr, "usage: %s <stream> <out-dir> <first> <every> <count> [scale]\n", argv[0]);
		return 2;
	}
	const std::string stream = argv[1], out = argv[2];
	const uint64_t first = std::strtoull(argv[3], nullptr, 10);
	const uint64_t every = std::max<uint64_t>(1, std::strtoull(argv[4], nullptr, 10));
	const uint64_t count = std::strtoull(argv[5], nullptr, 10);
	const uint32_t scale = argc > 6 ? uint32_t(std::max(1, std::atoi(argv[6]))) : 2u;

	// Decode every texture natively too, for PROBE_TEXTURES.
	setenv("TAKEDOWN_NATIVE_TEXTURES", "1", 0);
	Granite::Global::init(Granite::Global::MANAGER_FEATURE_DEFAULT_BITS, 1);
	if (!Context::init_loader(nullptr))
		return 1;
	Context ctx;
	ctx.set_num_thread_indices(1);
	// The native scene compiles its shaders from builtin:// at run time.
	Context::SystemHandles handles = {};
	handles.filesystem = GRANITE_FILESYSTEM();
	ctx.set_system_handles(handles);
	if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
	                                  CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
	                                  CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
	                                  CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
		return 1;
	Device device;
	device.set_context(ctx);
	const char *profile_path = getenv("PROBE_PROFILE");
	// PROBE_PROFILE_FROM=<vsync>: sample only from that vsync on.
	const uint64_t profile_from = getenv("PROBE_PROFILE_FROM") ? std::strtoull(getenv("PROBE_PROFILE_FROM"), nullptr, 10) : 0;
	bool profiling = false;
	if (profile_path && profile_from == 0)
	{
		profile_start();
		profiling = true;
	}
	device.init_frame_contexts(4);

	struct VramSnapshot
	{
		uint64_t frame = 0;
		std::vector<uint8_t> gs;
		std::vector<std::array<uint32_t, 4>> owners;
		std::vector<uint8_t> exact;
	};
	std::deque<VramSnapshot> vram_snapshots;
	GSInterface iface;
	GSOptions opts = {};
	if (!iface.init(&device, opts))
		return 1;
	// PROBE_NO_SCENE=1: the GS alone (for its cost; no native frames are written).
	iface.takedown_scene_set_enabled(getenv("PROBE_NO_SCENE") == nullptr);
	// PROBE_NO_GS: GS drawing off while native frames come, as in the game (the
	// GS PNGs are then stale).
	iface.takedown_scene_set_gs_rendering(getenv("PROBE_NO_GS") == nullptr);
	if (getenv("PROBE_UI_TEXTURES"))
		iface.takedown_ui_set_enabled(true);

	FILE *file = std::fopen(stream.c_str(), "rb");
	GSDumpParser parser;
	if (!file || !parser.open_raw_file(file, 4 * 1024 * 1024, &iface))
	{
		std::fprintf(stderr, "cannot open %s\n", stream.c_str());
		return 1;
	}

	{
		auto *shader = device.get_shader_manager().register_graphics("builtin://shaders/takedown_native_scene.vert",
		                                                             "builtin://shaders/takedown_native_scene.frag");
		auto *variant = shader ? shader->register_variant({}) : nullptr;
		if (!variant || !variant->get_program())
		{
			std::fprintf(stderr, "cannot build the native scene shaders (builtin://shaders)\n");
			return 1;
		}
	}
	TakedownRenderer::NativeScene scene;
	uint64_t written = 0, serial = ~uint64_t(0);
	const uint64_t warmup = 8;
	while (written < count && parser.iterate_until_vsync(false))
	{
		ScanoutResult vsync = parser.consume_vsync_result();
		parser.take_native_models();
		const uint64_t frame = parser.get_vsync_count();
		if (profile_path && !profiling && frame >= profile_from)
		{
			profile_start();
			profiling = true;
		}
		if (getenv("PROBE_VSYNC_LOG"))
			std::fprintf(stderr, "vsync %llu\n", (unsigned long long)frame);
		device.next_frame_context();
		const uint64_t next = first + written * every;
		// PROBE_VRAM: GS memory and native memory's owners at the last few vsyncs
		// (the native scene draws a frame a vsync or two after the GS).
		if (getenv("PROBE_VRAM") && frame + 4 >= next)
		{
			iface.flush();
			const auto &memory = iface.takedown_native_memory();
			VramSnapshot snap;
			snap.frame = frame;
			const auto *gs = static_cast<const uint8_t *>(iface.map_vram_read(0, memory.size()));
			snap.gs.assign(gs, gs + memory.size());
			for (uint32_t block = 0; block < memory.size() / 256u; block++)
			{
				snap.owners.push_back(memory.block_owners(block));
				snap.exact.push_back(memory.block_exact(block));
			}
			vram_snapshots.push_back(std::move(snap));
			if (vram_snapshots.size() > 4)
				vram_snapshots.pop_front();
		}

		// Every published frame is drawn, in order, from the start of the stream.
		const auto published = iface.takedown_scene_take_published();
		const auto &displayed = iface.takedown_scene_displayed();
		if (published.empty() || displayed.batches.empty())
			continue;
		const Image *native = nullptr;
		for (const auto &each : published)
		{
			if (const char *watch = getenv("PROBE_TARGET"))
				for (size_t i = 0; i < each->batches.size(); i++)
				{
					const auto &b = each->batches[i];
					const char *tex_watch = getenv("PROBE_TEXTURE");
					if ((tex_watch ? b.tex_tbp0 != uint32_t(atoi(tex_watch)) || !b.textured : b.fbp * 32u != uint32_t(atoi(watch))) ||
					    b.vertices.empty())
						continue;
					const auto &v = b.vertices[0];
					if (b.native_image && getenv("PROBE_TARGET_DUMP") && frame >= first)
					{
						const auto &t = *b.native_image;
						std::vector<float> rgba(t.rgba.size() * 4);
						for (size_t k = 0; k < t.rgba.size(); k++)
							for (int c = 0; c < 4; c++)
								rgba[k * 4 + c] = float((t.rgba[k] >> (8 * c)) & 0xffu) / (c == 3 ? 128.0f : 255.0f);
						const float one[3] = { 1, 1, 1 };
						char path[512];
						std::snprintf(path, sizeof(path), "%s/texture-%llu-%zu.png", out.c_str(), (unsigned long long)frame, i);
						write_png(path, rgba, t.width, t.height, one);
						std::snprintf(path, sizeof(path), "%s/texture-%llu-%zu-alpha.png", out.c_str(), (unsigned long long)frame, i);
						write_alpha_png(path, rgba, t.width, t.height);
					}
					if (b.native_clut)
					{
						uint32_t nonzero = 0;
						for (uint16_t e : *b.native_clut)
							nonzero += e != 0;
						const auto &t0 = b.native_texture.tex0.desc;
						std::printf("  clut: %u of 512 halfwords nonzero; CBP %u CPSM %02x CSM %u CSA %u CLD %u\n", nonzero,
						            uint32_t(t0.CBP), uint32_t(t0.CPSM), uint32_t(t0.CSM), uint32_t(t0.CSA), uint32_t(t0.CLD));
					}
					if (b.image && !b.arrayed && getenv("PROBE_TARGET_DUMP") && frame >= first)
					{
						const auto gs = read_image(device, *b.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
						std::vector<float> scaled(gs);
						for (size_t k = 3; k < scaled.size(); k += 4)
							scaled[k] *= 255.0f / 128.0f;
						char path[512];
						std::snprintf(path, sizeof(path), "%s/gsimage-%llu-%zu-alpha.png", out.c_str(), (unsigned long long)frame, i);
						write_alpha_png(path, scaled, b.image->get_width(), b.image->get_height());
						const float one_gain[3] = { 1, 1, 1 };
						std::snprintf(path, sizeof(path), "%s/gsimage-%llu-%zu.png", out.c_str(), (unsigned long long)frame, i);
						write_png(path, gs, b.image->get_width(), b.image->get_height(), one_gain);
					}
					for (uint32_t py = 0; py < b.native_pages_h && getenv("PROBE_PAGES"); py++)
					{
						std::printf("    page row %u:", py);
						for (uint32_t px = 0; px < b.native_pages_w; px++)
							std::printf(" %08x/%08x", b.native_page_rgb[py * b.native_pages_w + px], b.native_page_alpha[py * b.native_pages_w + px]);
						std::printf("\n");
					}
					if (getenv("PROBE_VERTS") && frame >= first)
						for (const auto &w : b.vertices)
							std::printf("    vert %.2f,%.2f z %.0f fog %.3f stq %.2f,%.2f,%.3f rgba %.3f %.3f %.3f %.3f\n", w.x, w.y,
							            double(w.z) * 16777216.0, double(w.fog), w.s, w.t, w.q, w.r, w.g, w.b, w.a);
					std::printf("  fog %u fogcol %06x; lod: lcm %u mxl %u mmin %u l %u k %d; mmag linear %u; native levels %u, rect levels %u %ux%u\n",
					            b.fog, b.fogcol, b.lod_fixed, b.lod_max, b.lod_filter, b.lod_l, b.lod_k, b.linear ? 1u : 0u,
					            b.native_image ? b.native_image->levels : 0u, b.native_texture.rect.levels,
					            b.native_texture.rect.width, b.native_texture.rect.height);
					std::printf("  pages %ux%u, self %d, blend A%uB%uC%uD%u fix %u\n", b.native_pages_w, b.native_pages_h,
					            int(b.self_read), b.blend_a, b.blend_b, b.blend_c, b.blend_d, b.blend_fix);
					std::printf("  frame %llu batch %zu -> %u/%u: %zu verts tex %u/%u psm %02x native %s source %08x tfx %u tcc %u atst %u afail %u fbmsk %08x blend %u zw %u zbp %u ztst %u v0 (%.1f,%.1f) stq %.1f,%.1f,%.2f rgba %.2f %.2f %.2f %.2f scissor %.0f,%.0f,%.0f,%.0f\n",
					            (unsigned long long)frame, i, b.fbp * 32u, b.fbw, b.vertices.size(), b.tex_tbp0, b.tex_tbw, b.tex_psm,
					            b.native_image ? "yes" : "no", b.native_source, b.tfx, b.tcc, b.alpha_test, b.alpha_fail, b.fbmsk,
					            b.blend, b.depth_write, b.zbp * 32u, b.depth_test, v.x, v.y, v.s, v.t, v.q, v.r, v.g, v.b, v.a, b.scissor.x, b.scissor.y,
					            b.scissor.z, b.scissor.w);
				}
			auto cmd = device.request_command_buffer();
			if (frame < next && !getenv("PROBE_FALLBACKS_ALL"))
			{
				scene.gs_fallbacks.clear(); // report the written frame's own
				scene.notes.clear();
			}
			native = scene.render(device, *cmd, *each, scale);
			device.submit(cmd);
		}
		if (frame < next)
			continue;
		if (!native)
		{
			std::printf("frame %llu skipped: native %p GS %p\n", (unsigned long long)frame, (const void *)native,
			            (const void *)vsync.image.get());
			continue;
		}
		(void)serial;
		(void)warmup;
		char name[64];
		std::snprintf(name, sizeof(name), "%06llu", (unsigned long long)frame);
		const float one[3] = { 1.0f, 1.0f, 1.0f };
		const float gain[3] = { displayed.gain.x, displayed.gain.y, displayed.gain.z };
		// No GS picture while the GS is idle (PROBE_NO_GS).
		if (vsync.image)
		{
			const auto gs = read_image(device, *vsync.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
			write_png(out + "/" + name + "-gs.png", gs, vsync.image->get_width(), vsync.image->get_height(), one);
		}
		const auto nat = read_image(device, *native, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		write_png(out + "/" + name + "-native.png", nat, native->get_width(), native->get_height(), gain);
		write_alpha_png(out + "/" + name + "-native-alpha.png", nat, native->get_width(), native->get_height());
		if (getenv("PROBE_VRAM") && scene.vram().buffer())
		{
			// The GPU copy of GS memory against the GS's own, over bytes exact draws
			// wrote last (native memory's view now, after the frame), per owner.
			const auto &src = *scene.vram().buffer();
			BufferCreateInfo info = {};
			info.size = src.get_create_info().size;
			info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			info.domain = BufferDomain::CachedHost;
			auto host = device.create_buffer(info);
			auto copy = device.request_command_buffer();
			copy->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
			              VK_ACCESS_2_TRANSFER_READ_BIT);
			copy->copy_buffer(*host, src);
			copy->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
			              VK_ACCESS_2_HOST_READ_BIT);
			device.submit(copy);
			device.wait_idle();
			const auto *ours = static_cast<const uint8_t *>(device.map_host_buffer(*host, MEMORY_ACCESS_READ_BIT));
			const auto &memory = iface.takedown_native_memory();
			struct Stat
			{
				uint64_t bytes = 0, differ = 0, off1 = 0, off2_3 = 0, worse = 0;
			};
			// Over the bytes exact draws wrote last, per owner, against each snapshot;
			// the one that matches best is the vsync this frame was drawn from.
			const VramSnapshot *best = nullptr;
			std::map<uint32_t, Stat> best_stats;
			uint64_t best_differ = ~0ull;
			for (const auto &snap : vram_snapshots)
			{
				std::map<uint32_t, Stat> stats;
				uint64_t differ = 0, total = 0;
				for (uint32_t block = 0; block < memory.size() / 256u; block++)
				{
					const uint8_t exact = snap.exact[block];
					if (!exact)
						continue;
					for (uint32_t w = 0; w < 64; w++)
						for (uint32_t lane = 0; lane < 4; lane++)
						{
							if (!(exact & (1u << lane)))
								continue;
							const size_t at = size_t(block) * 256u + w * 4u + lane;
							Stat &st = stats[snap.owners[block][lane]];
							st.bytes++;
							total++;
							const int d = std::abs(int(ours[at]) - int(snap.gs[at]));
							if (d)
							{
								st.differ++;
								differ++;
								(d == 1 ? st.off1 : d <= 3 ? st.off2_3 : st.worse)++;
							}
						}
				}
				std::printf("  GS memory copy vs GS at vsync %llu: %llu of %llu exact bytes differ\n",
				            (unsigned long long)snap.frame, (unsigned long long)differ, (unsigned long long)total);
				if (differ < best_differ)
				{
					best_differ = differ;
					best = &snap;
					best_stats = std::move(stats);
				}
			}
			for (const auto &entry : best_stats)
				std::printf("  GS memory copy vs GS, owner %08x (%u/%u): %llu bytes, %llu differ (1: %llu, 2-3: %llu, more: %llu)\n",
				            entry.first, (entry.first & 0xffffu) * 32u, (entry.first >> 16) & 0xffu,
				            (unsigned long long)entry.second.bytes, (unsigned long long)entry.second.differ,
				            (unsigned long long)entry.second.off1, (unsigned long long)entry.second.off2_3,
				            (unsigned long long)entry.second.worse);
			const uint8_t *theirs = best ? best->gs.data() : static_cast<const uint8_t *>(iface.map_vram_read(0, memory.size()));
			// PROBE_VRAM_WORDS=<block>: that block's words, ours / GS.
			if (const char *words = getenv("PROBE_VRAM_WORDS"))
			{
				const uint32_t block = uint32_t(std::atoi(words));
				for (uint32_t w = 0; w < 64; w++)
				{
					uint32_t a, b;
					std::memcpy(&a, ours + block * 256u + w * 4u, 4);
					std::memcpy(&b, theirs + block * 256u + w * 4u, 4);
					std::printf("%s%08x/%08x", w % 8 ? " " : "\n    ", a, b);
				}
				std::printf("\n");
			}
			if (const char *dump = getenv("PROBE_VRAM_DUMP"))
			{
				const std::pair<std::string, const void *> files[] = { { "-ours.bin", ours }, { "-gs.bin", theirs },
					                                                   { "-native.bin", memory.data() } };
				for (const auto &f : files)
					if (std::FILE *fp = std::fopen((std::string(dump) + f.first).c_str(), "wb"))
					{
						std::fwrite(f.second, 1, memory.size(), fp);
						std::fclose(fp);
					}
			}
			device.unmap_host_buffer(*host, MEMORY_ACCESS_READ_BIT);
			if (const char *dump = getenv("PROBE_VRAM_DUMP"))
				for (int previous = 0; previous < 2; previous++)
					if (const auto &before = previous ? scene.vram().debug_before_previous : scene.vram().debug_before)
						if (std::FILE *fp = std::fopen(
						        (std::string(dump) + (previous ? "-before-sprite-previous.bin" : "-before-sprite.bin")).c_str(),
						        "wb"))
						{
							std::fwrite(device.map_host_buffer(*before, MEMORY_ACCESS_READ_BIT), 1, memory.size(), fp);
							device.unmap_host_buffer(*before, MEMORY_ACCESS_READ_BIT);
							std::fclose(fp);
						}
		}
		if (getenv("PROBE_DUMP_MIRRORS"))
			for (const auto &mirror : scene.mirror_images())
			{
				const auto pixels = read_image(device, *mirror.second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				write_png(out + "/" + name + "-mirror-" + mirror.first + ".png", pixels, mirror.second->get_width(),
				          mirror.second->get_height(), one);
				write_alpha_png(out + "/" + name + "-mirror-" + mirror.first + "-alpha.png", pixels,
				                mirror.second->get_width(), mirror.second->get_height());
			}
		if (const char *probe = std::getenv("PROBE_PIXEL"))
		{
			// Every triangle covering this GS pixel centre, in draw order.
			float px = 0, py = 0;
			std::sscanf(probe, "%f,%f", &px, &py);
			for (size_t i = 0; i < displayed.batches.size(); i++)
			{
				const auto &b = displayed.batches[i];
				if (b.fbw != 10)
					continue;
				for (size_t t = 0; t + 2 < b.vertices.size(); t += 3)
				{
					const auto &a = b.vertices[t], &c = b.vertices[t + 1], &d = b.vertices[t + 2];
					const float area = (c.x - a.x) * (d.y - a.y) - (d.x - a.x) * (c.y - a.y);
					if (area == 0.0f)
						continue;
					const float w1 = ((d.x - c.x) * (py - c.y) - (d.y - c.y) * (px - c.x)) / area;
					const float w2 = ((a.x - d.x) * (py - d.y) - (a.y - d.y) * (px - d.x)) / area;
					const float w3 = 1.0f - w1 - w2;
					const float miss = std::min(w1, std::min(w2, w3));
					if (miss < -0.02f)
						continue;
					if (miss < 0)
						std::printf("  (near miss %.4f)", double(miss));
					const double z = (double(w1) * a.z + double(w2) * c.z + double(w3) * d.z) * 16777216.0;
					std::printf("  batch %4zu fbp %u tri %4zu z %10.1f tex %u/%u psm %02x ztst %u zw %u atst %u aref %u afail %u blend %u A%uB%uC%uD%u fix %u fbmsk %08x a %.3f%s\n",
					            i, b.fbp, t / 3, z, b.tex_tbp0, b.tex_tbw, b.tex_psm, b.depth_test, b.depth_write, b.alpha_test, b.alpha_reference,
					            b.alpha_fail, b.blend, b.blend_a, b.blend_b, b.blend_c, b.blend_d, b.blend_fix, b.fbmsk,
					            double(w1 * a.a + w2 * c.a + w3 * d.a), b.depth_to_alpha ? " D2A" : "");
				}
				if (b.depth_to_alpha)
					std::printf("  batch %4zu depth to alpha\n", i);
				if (getenv("PROBE_NEAR"))
					for (size_t t = 0; t + 2 < b.vertices.size(); t += 3)
					{
						const auto *v = &b.vertices[t];
						const float x0 = std::min({ v[0].x, v[1].x, v[2].x }), x1 = std::max({ v[0].x, v[1].x, v[2].x });
						const float y0 = std::min({ v[0].y, v[1].y, v[2].y }), y1 = std::max({ v[0].y, v[1].y, v[2].y });
						if (x0 <= px + 6 && x1 >= px - 6 && y0 <= py + 3 && y1 >= py - 3)
							std::printf("  near: batch %zu tex %u tri %zu (%.2f,%.2f z%.0f) (%.2f,%.2f z%.0f) (%.2f,%.2f z%.0f)\n", i,
							            b.tex_tbp0, t / 3, v[0].x, v[0].y, v[0].z * 16777216.0, v[1].x, v[1].y,
							            v[1].z * 16777216.0, v[2].x, v[2].y, v[2].z * 16777216.0);
					}
			}
		}
		if (getenv("PROBE_GLOW_PALETTE"))
			for (size_t i = 0; i < displayed.batches.size(); i++)
			{
				const auto &b = displayed.batches[i];
				if (b.textured != 3 || !b.palette)
					continue;
				std::printf("  glow batch %zu palette alpha:", i);
				for (size_t e = 0; e < b.palette->size(); e++)
					std::printf(" %u", (*b.palette)[e] >> 24);
				std::printf("\n");
				for (const auto &v : b.vertices)
					std::printf("    v %.3f %.3f z %.1f st %.3f %.3f q %.3f\n", v.x, v.y, v.z * 16777216.0, v.s, v.t, v.q);
			}
		if (getenv("PROBE_ZFIGHT"))
		{
			// Near ties: a depth-tested draw whose depth at a GS pixel centre is within
			// 2 units of the depth another batch wrote there (coplanar layers).
			const int W = 640, H = 448;
			std::vector<double> zbuf(size_t(W) * H, -1.0);
			std::vector<int> writer(size_t(W) * H, -1);
			std::vector<uint8_t> mask(size_t(W) * H, 0);
			std::map<std::pair<int, int>, uint64_t> pairs;
			std::map<std::pair<int, int>, double> worst;
			std::map<std::pair<int, int>, uint64_t> failing;
			for (size_t i = 0; i < displayed.batches.size(); i++)
			{
				const auto &b = displayed.batches[i];
				if (b.fbw != 10 || b.depth_to_alpha)
					continue;
				for (size_t t = 0; t + 2 < b.vertices.size(); t += 3)
				{
					const auto &a = b.vertices[t], &c = b.vertices[t + 1], &d = b.vertices[t + 2];
					const double area = (double(c.x) - a.x) * (double(d.y) - a.y) - (double(d.x) - a.x) * (double(c.y) - a.y);
					if (area == 0.0)
						continue;
					const int x0 = std::max(0, int(std::floor(std::min({ a.x, c.x, d.x }))));
					const int x1 = std::min(W - 1, int(std::ceil(std::max({ a.x, c.x, d.x }))));
					const int y0 = std::max(0, int(std::floor(std::min({ a.y, c.y, d.y }))));
					const int y1 = std::min(H - 1, int(std::ceil(std::max({ a.y, c.y, d.y }))));
					for (int y = y0; y <= y1; y++)
						for (int x = x0; x <= x1; x++)
						{
							const double px = x + 0.5, py = y + 0.5;
							const double w1 = ((double(d.x) - c.x) * (py - c.y) - (double(d.y) - c.y) * (px - c.x)) / area;
							const double w2 = ((double(a.x) - d.x) * (py - d.y) - (double(a.y) - d.y) * (px - d.x)) / area;
							const double w3 = 1.0 - w1 - w2;
							if (w1 < 0 || w2 < 0 || w3 < 0)
								continue;
							const double z = (w1 * a.z + w2 * c.z + w3 * d.z) * 16777216.0;
							const size_t k = size_t(y) * W + size_t(x);
							bool pass = true;
							if (b.depth_test == 0)
								pass = false;
							else if (b.depth_test >= 2 && zbuf[k] >= 0)
							{
								const double dz = z - zbuf[k];
								static const double tie = std::atof(getenv("PROBE_ZFIGHT"));
								if (std::abs(dz) < std::max(tie, 2.0) && writer[k] != int(i))
								{
									mask[k] = 255;
									const auto key = std::make_pair(writer[k], int(i));
									pairs[key]++;
									worst[key] = std::max(worst[key], std::abs(dz));
									if (dz < 0)
										failing[key]++;
								}
								pass = b.depth_test == 2 ? dz >= 0 : dz > 0;
							}
							if (pass && b.depth_write)
							{
								zbuf[k] = z;
								writer[k] = int(i);
							}
						}
				}
			}
			std::vector<std::pair<uint64_t, std::pair<int, int>>> sorted;
			for (const auto &p : pairs)
				sorted.push_back({ p.second, p.first });
			std::sort(sorted.rbegin(), sorted.rend());
			for (size_t n = 0; n < sorted.size() && n < 30; n++)
			{
				const auto &[count, key] = sorted[n];
				const auto &w = displayed.batches[size_t(key.first)], &r = displayed.batches[size_t(key.second)];
				std::printf("  zfight %6llu px: batch %d (tex %u psm %02x zw %u) under batch %d (tex %u psm %02x ztst %u zw %u blend %u A%uB%uC%uD%u) max |dz| %.2f, %llu fail\n",
				            (unsigned long long)count, key.first, w.tex_tbp0, w.tex_psm, w.depth_write, key.second, r.tex_tbp0,
				            r.tex_psm, r.depth_test, r.depth_write, r.blend, r.blend_a, r.blend_b, r.blend_c, r.blend_d,
				            worst[key], (unsigned long long)failing[key]);
			}
			FILE *f = std::fopen((out + "/" + name + "-zfight.pgm").c_str(), "wb");
			if (f)
			{
				std::fprintf(f, "P5\n%d %d\n255\n", W, H);
				std::fwrite(mask.data(), 1, mask.size(), f);
				std::fclose(f);
			}
		}
		if (getenv("PROBE_ZQ"))
		{
			// Depth against Q of perspective-textured scene vertices (Q = 1/w up to a scale).
			uint32_t n = 0;
			for (const auto &b : displayed.batches)
			{
				if (b.fbw != 10 || !b.textured || b.vertices.empty() || n > 4000)
					continue;
				for (size_t k = 0; k < b.vertices.size(); k += 7)
				{
					const auto &v = b.vertices[k];
					if (v.q != 1.0f && v.q > 0.0f)
					{
						std::printf("  zq %.1f %.8g %.1f %.1f\n", double(v.z) * 16777216.0, double(v.q), double(v.x), double(v.y));
						n++;
					}
				}
			}
		}
		if (getenv("PROBE_WIRES"))
			for (size_t i = 0; i < displayed.batches.size(); i++)
			{
				const auto &b = displayed.batches[i];
				if (b.blend_a == 2 && b.blend_b == 0 && b.blend_c == 0 && b.blend_d == 1 && !b.vertices.empty())
				{
					float zmin = 1e30f, zmax = 0.0f;
					for (const auto &w : b.vertices)
					{
						zmin = std::min(zmin, w.z);
						zmax = std::max(zmax, w.z);
					}
					std::printf("  wires z range %.0f..%.0f\n", double(zmin) * 16777216.0, double(zmax) * 16777216.0);
					const auto &v = b.vertices[0];
					std::printf("  wires: batch %zu %zu verts rgba %.3f %.3f %.3f %.3f z %.0f ztst %u zw %u fbmsk %08x (%.1f,%.1f)-(%.1f,%.1f)\n", i,
					            b.vertices.size(), v.r, v.g, v.b, v.a, v.z * 16777216.0, b.depth_test, b.depth_write, b.fbmsk,
					            v.x, v.y, b.vertices[1].x, b.vertices[1].y);
				}
			}
		if (getenv("PROBE_BATCHES"))
			for (size_t i = 0; i < displayed.batches.size(); i++)
			{
				const auto &b = displayed.batches[i];
				if (b.fbw >= 10 && !getenv("PROBE_BATCHES_ALL"))
					continue;
				static const char *kinds[] = { "point", "line", "tri", "sprite" };
				std::printf("  batch %4zu -> %u/%u %s x%zu tex %u/%u psm %02x tfx %u blend %u A%uB%uC%uD%u fix %u atst %u afail %u ztst %u zw %u zbp %u fbmsk %08x%s%s%s%s\n",
				            i, b.fbp * 32u, b.fbw, b.depth_to_alpha ? "d2a" : b.invalid_blocks ? "upload" : kinds[b.prim_kind & 3u],
				            b.vertices.size(), b.textured ? b.tex_tbp0 : 0u, b.tex_tbw, b.tex_psm, b.tfx, b.blend, b.blend_a,
				            b.blend_b, b.blend_c, b.blend_d, b.blend_fix, b.alpha_test, b.alpha_fail, b.depth_test,
				            b.depth_write, b.zbp * 32u, b.fbmsk, b.self_read ? " self" : "", b.textured == 3 ? " glow" : "",
				            b.exact ? " exact" : "", b.vram_texture ? " vram-texture" : "");
				if (i < scene.batch_sources.size() && scene.batch_sources[i])
					std::printf("    source: %s\n", scene.batch_sources[i]);
			}
		if (getenv("PROBE_UI_TEXTURES"))
		{
			// The HUD's textures: GS image against native decode.
			uint32_t same = 0, differ = 0, missing = 0, other = 0;
			std::unordered_map<const void *, bool> seen_ui;
			for (const auto &b : iface.takedown_ui_displayed())
			{
				if (!b.image || seen_ui.count(b.image.get()))
					continue;
				seen_ui[b.image.get()] = true;
				if (!b.native_image)
				{
					missing++;
					continue;
				}
				const auto &img = *b.image;
				if (img.get_width() != b.native_image->width || img.get_height() != b.native_image->height ||
				    img.get_format() != VK_FORMAT_R8G8B8A8_UNORM)
				{
					other++;
					continue;
				}
				const auto gs = read_image(device, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				size_t bad = 0;
				for (size_t t = 0; t < size_t(img.get_width()) * img.get_height(); t++)
				{
					uint32_t g = 0;
					for (int c = 0; c < 4; c++)
						g |= uint32_t(std::lround(gs[t * 4 + c] * 255.0f)) << (8 * c);
					bad += g != b.native_image->rgba[t];
				}
				(bad ? differ : same)++;
			}
			std::printf("  UI textures: %u identical, %u differ, %u without native, %u other\n", same, differ, missing, other);
		}
		if (getenv("PROBE_TEXTURES"))
		{
			// Every GS texture image of the frame against its native decode.
			std::unordered_map<const void *, bool> seen;
			uint32_t same = 0, differ = 0, rendered = 0, other = 0, printed = 0, mipped = 0;
			for (const auto &b : displayed.batches)
			{
				if (!b.image || b.arrayed || !b.textured || seen.count(b.image.get()))
					continue;
				seen[b.image.get()] = true;
				if (!b.native_image)
				{
					rendered++;
					const auto &t0 = b.native_texture.tex0.desc;
					std::printf("  render-target texture: TBP %u TBW %u psm %02x %ux%u\n", uint32_t(t0.TBP0), uint32_t(t0.TBW),
					            uint32_t(t0.PSM), b.native_texture.rect.width, b.native_texture.rect.height);
					continue;
				}
				const auto &img = *b.image;
				if (img.get_width() != b.native_image->width || img.get_height() != b.native_image->height ||
				    img.get_format() != VK_FORMAT_R8G8B8A8_UNORM)
				{
					other++;
					if (printed++ < 12)
						std::printf("  texture size/format: GS %ux%u fmt %d native %ux%u psm %02x\n", img.get_width(),
						            img.get_height(), int(img.get_format()), b.native_image->width,
						            b.native_image->height, uint32_t(b.native_texture.tex0.desc.PSM));
					continue;
				}
				const auto gs = read_image(device, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				size_t bad = 0, first = 0;
				for (size_t t = 0; t < size_t(b.native_image->width) * b.native_image->height; t++)
				{
					uint32_t g = 0;
					for (int c = 0; c < 4; c++)
						g |= uint32_t(std::lround(gs[t * 4 + c] * 255.0f)) << (8 * c);
					if (g != b.native_image->rgba[t] && !bad++)
						first = t;
				}
				// Mip levels as well.
				for (uint32_t l = 1; l < b.native_image->levels && l < img.get_create_info().levels && !bad; l++)
				{
					const auto gl = read_image(device, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, l);
					const size_t n = size_t(img.get_width(l)) * img.get_height(l);
					for (size_t t = 0; t < n; t++)
					{
						uint32_t g = 0;
						for (int c = 0; c < 4; c++)
							g |= uint32_t(std::lround(gl[t * 4 + c] * 255.0f)) << (8 * c);
						if (g != b.native_image->rgba[b.native_image->level_offsets[l] + t] && !bad++)
						{
							first = t;
							std::printf("  mip %u differs at %zu: GS %08x native %08x\n", l, t, g,
							            b.native_image->rgba[b.native_image->level_offsets[l] + t]);
						}
					}
				}
				if (!bad)
				{
					same++;
					mipped += b.native_image->levels > 1;
					continue;
				}
				differ++;
				if (printed++ < 12)
				{
					uint32_t g = 0;
					for (int c = 0; c < 4; c++)
						g |= uint32_t(std::lround(gs[first * 4 + c] * 255.0f)) << (8 * c);
					const auto &t0 = b.native_texture.tex0.desc;
					std::printf("  texture differs: TBP %u TBW %u psm %02x cpsm %02x csm %u csa %u %ux%u rect %u,%u: %zu texels, first %zu GS %08x native %08x\n",
					            uint32_t(t0.TBP0), uint32_t(t0.TBW), uint32_t(t0.PSM), uint32_t(t0.CPSM), uint32_t(t0.CSM),
					            uint32_t(t0.CSA), b.native_image->width, b.native_image->height, b.native_texture.rect.x,
					            b.native_texture.rect.y, bad, first, g, b.native_image->rgba[first]);
				}
			}
			std::printf("  textures: %u identical (%u with mip levels), %u differ, %u render targets, %u other\n", same, mipped,
			            differ, rendered, other);
		}
		for (const auto &entry : displayed.left_out)
			std::printf("  left out: %u x %s\n", entry.second, entry.first.c_str());
		{
			const auto &c = scene.counters;
			std::printf("  sources: %llu native textures, %llu render targets, %llu GS images, %llu left to GS (Z memory)\n",
			            (unsigned long long)c.native_samples, (unsigned long long)c.mirror_samples,
			            (unsigned long long)c.gs_samples, (unsigned long long)c.gs_only_batches);
			const auto &v = scene.vram().counters;
			std::printf("  GS memory copy: %llu sprites, %llu textures (%llu sampled), %llu blocks from bytes, %llu filled "
			            "from native images, %llu owners missing, %llu feedback copies\n",
			            (unsigned long long)v.sprites, (unsigned long long)v.decodes, (unsigned long long)c.vram_samples,
			            (unsigned long long)v.byte_blocks, (unsigned long long)v.filled_blocks,
			            (unsigned long long)v.missing_owners, (unsigned long long)v.feedback_copies);
			std::printf("  GS memory copy refreshes: %llu, pages %llu seen, %llu processed, %llu with bytes\n",
			            (unsigned long long)v.refreshes, (unsigned long long)v.pages_seen,
			            (unsigned long long)v.pages_processed, (unsigned long long)v.byte_pages);
			for (const auto &entry : scene.vram().missing_by_owner)
				std::printf("    missing owner %08x: %llu blocks\n", entry.first, (unsigned long long)entry.second);
			for (const auto &entry : scene.vram().filled_by_owner)
				std::printf("    filled from %08x: %llu blocks\n", entry.first, (unsigned long long)entry.second);
			for (const auto &entry : scene.gs_fallbacks)
				std::printf("    GS image: %s x %llu\n", entry.first.c_str(), (unsigned long long)entry.second);
			scene.gs_fallbacks.clear();
			for (const auto &entry : scene.notes)
				std::printf("    note: %s x %llu\n", entry.first.c_str(), (unsigned long long)entry.second);
			scene.notes.clear();
			if (getenv("PROBE_MIRRORS"))
				for (const auto &line : scene.describe_mirrors())
					std::printf("    mirror %s\n", line.c_str());
			scene.counters = {};
		}
		std::printf("frame %s: %zu batches, GS %ux%u, native %ux%u\n", name, displayed.batches.size(),
		            vsync.image ? vsync.image->get_width() : 0u, vsync.image ? vsync.image->get_height() : 0u,
		            native->get_width(), native->get_height());
		std::fflush(stdout);
		written++;
	}
	const auto stats = iface.takedown_scene_consume_stats();
	std::printf("capture: %llu captured, %llu published, %llu unsupported\n", (unsigned long long)stats.captured,
	            (unsigned long long)stats.published, (unsigned long long)stats.unsupported);
	std::printf("read %llu vsyncs, wrote %llu frames%s\n", (unsigned long long)parser.get_vsync_count(),
	            (unsigned long long)written, parser.was_truncated() ? " (stream truncated)" : "");
	if (profiling)
		profile_write(profile_path);
	return written ? 0 : 1;
}
