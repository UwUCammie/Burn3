// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+
//
// Takedown Recomp: live renderer for the runtime's paraLLEl-GS stream.
//
// The runtime (takedown-recomp --parallel-gs-live <pipe>) writes the raw GS
// stream of include/takedown/platform/parallel_gs_stream.hpp into a pipe. This
// application replays it with paraLLEl-GS and presents each VBlank, draws the
// host presentation state (PC options) and host UI layer on top, and returns
// input (--input-fifo) and GPU completion records (--completion-fifo) to the
// runtime. A regular file path replays a recorded capture the same way.

#include "application.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "os_filesystem.hpp"
#include "gs_interface.hpp"
#include "gs_dump_parser.hpp"
#include "flat_renderer.hpp"
#include "ui_manager.hpp"
#include "math.hpp"

#include "completion_writer.hpp"
#include "crash_report.hpp"
#include "discord_presence.hpp"
#include "host_ui_renderer.hpp"
#include "input_writer.hpp"
#include "native_scene.hpp"
#include "ns2_rt.hpp"
#include "native_ui_layer.hpp"
#include "fsr_upscaler.hpp"
#include "smaa_filter.hpp"
#include "ns2_hybrid.hpp"
#include "ns3_live.hpp"
#include "global_managers.hpp"
#include "thread_group.hpp"
#include "picture_layout.hpp"
#include "stream_pipe.hpp"
#include "takedown/version.hpp"
#include "takedown/platform/parallel_gs_host_presentation.hpp"
#if defined(_WIN32)
#include "takedown/platform/windows_frame_timestamps.hpp"
#endif

#include <SDL3/SDL.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif
#include <map>
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string.h>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

using namespace Granite;
using namespace Vulkan;

namespace ParallelGS
{
namespace td = takedown::platform;

struct StreamOptions
{
	std::string stream_path;
	std::string input_fifo;
	std::string frame_timestamps;
	std::string completion_fifo;
	// Directory of the executable, with a trailing separator.
	std::string exe_directory;
};

// First line of frame-dump.txt beside the executable (Windows), or empty.
static std::string frame_dump_config_dir()
{
#if defined(_WIN32)
	char module[MAX_PATH];
	const DWORD length = GetModuleFileNameA(nullptr, module, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return {};
	std::string config(module, length);
	const size_t slash = config.find_last_of("\\/");
	config = (slash == std::string::npos ? std::string() : config.substr(0, slash + 1)) + "frame-dump.txt";
	FILE *file = fopen(config.c_str(), "rb");
	if (!file)
		return {};
	char line[1024] = {};
	const bool ok = fgets(line, sizeof(line), file) != nullptr;
	fclose(file);
	if (!ok)
		return {};
	std::string path(line);
	while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
		path.pop_back();
	return path;
#else
	return {};
#endif
}

#ifdef _WIN32
// The window and taskbar use the executable's icon resource 1 (the B3 logo,
// takedown/assets/parallel_gs_stream.rc). Set before SDL creates its window class.
static const bool kWindowIconHint = SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1") &&
                                    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif

// Granite starts hardware_concurrency() - 1 worker threads (15 on a 16-thread CPU,
// whatever the process may use). The renderer's one steady pool task is the native
// scene's draw: a single task per frame that waits on nothing, so extra workers only
// cost stacks and wake-ups, and on a machine with few cores they compete with the
// stream parser and the capture worker. Take the CPUs this process may run on
// (affinity, not the machine's total), keep one for the main thread and cap the
// pool. An explicit GRANITE_NUM_WORKER_THREADS wins.
static unsigned usable_cpu_count()
{
#if defined(_WIN32)
	DWORD_PTR process = 0, system = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
		return unsigned(__builtin_popcountll(static_cast<unsigned long long>(process)));
#elif defined(__linux__)
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0)
		return unsigned(CPU_COUNT(&set));
#endif
	return std::max(1u, std::thread::hardware_concurrency());
}

static const bool kPoolPolicy = [] {
	if (getenv("GRANITE_NUM_WORKER_THREADS"))
		return false;
	const unsigned workers = std::min(4u, std::max(1u, usable_cpu_count() - 1u));
	const std::string text = std::to_string(workers);
#if defined(_WIN32)
	_putenv_s("GRANITE_NUM_WORKER_THREADS", text.c_str());
#else
	setenv("GRANITE_NUM_WORKER_THREADS", text.c_str(), 0);
#endif
	return true;
}();

// The native scene is on unless TAKEDOWN_NATIVE_SCENE=0 (read again where the scene is switched, apply_presentation).
static bool native_scene_wanted()
{
	const char *scene_env = getenv("TAKEDOWN_NATIVE_SCENE");
	return !scene_env || !*scene_env || strcmp(scene_env, "0") != 0;
}

static bool env_enabled(const char *name)
{
	const char *value = getenv(name);
	return value && *value && strcmp(value, "0") != 0;
}

static SuperSampling to_super_sampling(uint8_t samples)
{
	switch (samples)
	{
	case 2: return SuperSampling::X2;
	case 4: return SuperSampling::X4;
	case 8: return SuperSampling::X8;
	case 16: return SuperSampling::X16;
	case 64: return SuperSampling::X64;
	default: return SuperSampling::X1;
	}
}

// Input from the renderer window travels to the runtime. SDL events are
// observed with an event filter on the thread that pumps them; the filter
// also holds back window close requests in live mode so the runtime can shut
// down cleanly (it closes the graphics pipe, and then this process exits).
struct InputBridge
{
	std::unique_ptr<TakedownRenderer::InputWriter> writer;
	std::mutex lock;
	TakedownRenderer::PictureRect picture = { 0.0f, 0.0f, 1.0f, 1.0f };
	float surface_width = 1.0f, surface_height = 1.0f;
	std::atomic<bool> close_requested{ false };
	bool hold_close = false;
	// F10 (developer comparison): show the GS picture instead of the native scene.
	std::atomic<bool> native_scene_hidden{ false };
	// F11: log the next native scene frame's batches and left-out draws.
	std::atomic<bool> native_scene_dump{ false };
	// F9 (developer): save the next 90 frames (TAKEDOWN_DEV_FRAME_DUMP directory).
	std::atomic<bool> frame_burst{ false };

	void set_picture(const TakedownRenderer::PictureRect &rect, float width, float height)
	{
		std::lock_guard<std::mutex> hold{ lock };
		picture = rect;
		surface_width = width;
		surface_height = height;
		// The runtime expects the input state continuously, not only on
		// events: flush also repeats the full snapshot periodically.
		if (writer)
			writer->flush();
	}

	void pointer_moved(SDL_Window *window, float x, float y)
	{
		int w = 0, h = 0;
		if (window)
			SDL_GetWindowSize(window, &w, &h);
		// Event coordinates are in window units; the picture is in swapchain
		// pixels. Scale between them so HiDPI windows map correctly.
		const float sx = w > 0 ? surface_width / float(w) : 1.0f;
		const float sy = h > 0 ? surface_height / float(h) : 1.0f;
		float px = (x * sx - picture.x) / (picture.width > 0.0f ? picture.width : 1.0f);
		const float py = (y * sy - picture.y) / (picture.height > 0.0f ? picture.height : 1.0f);
		// The game's UI is its 4:3 picture, narrowed about the centre on a
		// wider picture (ui_layout.cpp). The pointer is reported over that 4:3
		// block, so a menu row or button is where the game (and the host UI)
		// puts it: 0 is the block's left edge and 1 its right, and the sides
		// of a wide picture are below 0 and above 1.
		if (picture.width > 0.0f && picture.height > 0.0f)
		{
			const float scale = (4.0f / 3.0f) / (picture.width / picture.height);
			if (scale < 1.0f)
				px = (px - (1.0f - scale) * 0.5f) / scale;
		}
		writer->pointer(px, py, true);
	}

	bool handle(const SDL_Event &e)
	{
		if (!writer)
		{
			if (hold_close && (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED))
			{
				close_requested = true;
				return false;
			}
			return true;
		}

		std::lock_guard<std::mutex> hold{ lock };
		bool keep = true;
		switch (e.type)
		{
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (e.key.scancode == SDL_SCANCODE_F10)
			{
				if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
					native_scene_hidden = !native_scene_hidden;
				break;
			}
			if (e.key.scancode == SDL_SCANCODE_F9)
			{
				if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
					frame_burst = true;
				break;
			}
			if (e.key.scancode == SDL_SCANCODE_F11)
			{
				if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
					native_scene_dump = true;
				break;
			}
			if (!e.key.repeat || e.type == SDL_EVENT_KEY_UP)
				writer->key(uint32_t(e.key.scancode), e.type == SDL_EVENT_KEY_DOWN);
			if (e.type == SDL_EVENT_KEY_DOWN && SDL_TextInputActive(SDL_GetWindowFromID(e.key.windowID)))
			{
				if (e.key.scancode == SDL_SCANCODE_BACKSPACE)
					writer->text_backspace();
				else if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER)
					writer->text_submit();
				else if (e.key.scancode == SDL_SCANCODE_ESCAPE)
					writer->text_cancel();
			}
			break;
		case SDL_EVENT_TEXT_INPUT:
			if (e.text.text)
				writer->text(e.text.text);
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			writer->mouse_button(e.button.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			break;
		case SDL_EVENT_MOUSE_MOTION:
			pointer_moved(SDL_GetWindowFromID(e.motion.windowID), e.motion.x, e.motion.y);
			break;
		case SDL_EVENT_MOUSE_WHEEL:
		{
			const float steps = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
			if (steps != 0.0f)
				writer->wheel(steps > 0.0f ? int(steps + 0.5f) : int(steps - 0.5f));
			break;
		}
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			writer->pointer(0.0f, 0.0f, false);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			writer->focus(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			writer->focus(false);
			break;
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			if (hold_close)
			{
				writer->request_quit();
				close_requested = true;
				keep = false;
			}
			break;
		default:
			break;
		}
		writer->flush();
		return keep;
	}

	static bool SDLCALL filter(void *user, SDL_Event *e)
	{
		return static_cast<InputBridge *>(user)->handle(*e);
	}
};

struct StreamApplication : Granite::Application, Granite::EventHandler
{
	explicit StreamApplication(StreamOptions options_)
		: options(std::move(options_))
	{
#ifndef _WIN32
		if (getenv("RC_PTRACER_ANY")) prctl(PR_SET_PTRACER, (unsigned long)-1, 0, 0, 0);
#endif
		EVENT_MANAGER_REGISTER_LATCH(StreamApplication, on_device_created, on_device_destroyed, DeviceCreatedEvent);
		get_wsi().set_backbuffer_format(BackbufferFormat::UNORM);

		live = TakedownRenderer::is_live_pipe_name(options.stream_path);
		if (!options.exe_directory.empty())
			host_ui.set_default_font_directory(options.exe_directory + "../../../cache/ui");

		if (!options.input_fifo.empty() && !env_enabled("TAKEDOWN_DEV_NO_INPUT"))
			input.writer = std::make_unique<TakedownRenderer::InputWriter>(options.input_fifo);
		input.hold_close = live;

		if (!options.completion_fifo.empty())
		{
			completion = std::make_unique<TakedownRenderer::CompletionWriter>(options.completion_fifo);
			if (!completion->hello())
				LOGE("Failed to queue completion hello\n");
		}

#if defined(_WIN32)
		if (!options.frame_timestamps.empty())
			timestamps = std::make_unique<td::WindowsFrameTimestampSink>(options.frame_timestamps.c_str());
#else
		if (!options.frame_timestamps.empty())
			LOGW("--frame-timestamps is only recorded on Windows.\n");
#endif

		const char *policy = getenv("TAKEDOWN_GS_NORMAL_PRESENT_POLICY");
		normal_present_policy = policy && *policy && strcmp(policy, "0") != 0;
		LOGI("Completion presentation policy: %s.\n", normal_present_policy ? "normal" : "all fields");
		stats_enabled = env_enabled("TAKEDOWN_GS_STATS");
		// TAKEDOWN_GS_STATS=<n> above 1 logs every n frames instead of 600.
		if (const char *every = getenv("TAKEDOWN_GS_STATS"); every && strtoul(every, nullptr, 10) > 1)
			stats_every = unsigned(strtoul(every, nullptr, 10));
		if (stats_enabled)
			watchdog = std::thread([this]() {
				int last = -1, same = 0;
				while (!watchdog_stop)
				{
					std::this_thread::sleep_for(std::chrono::seconds(1));
					const int now = stage.load();
					same = now == last ? same + 1 : 0;
					last = now;
					if (same == 2)
						LOGW("Watchdog: render_frame has been in stage %d for 2 s.\n", now);
				}
			});

		// Developer aid: save the game picture as PNG every N frames, so the
		// renderer can be checked without looking at its window.
		if (const char *seconds = getenv("TAKEDOWN_DEV_QUIT_AFTER"))
			quit_after = std::chrono::seconds(atoi(seconds));
		// No environment (launcher sessions): frame-dump.txt beside the executable
		// names the F9 directory; periodic dumps stay off in that case.
		if (!getenv("TAKEDOWN_DEV_FRAME_DUMP"))
		{
			const std::string dir = frame_dump_config_dir();
			if (!dir.empty())
			{
				dump_directory = dir;
				dump_every = ~0u;
				LOGI("Frame dump: F9 saves the next 90 frames to %s.\n", dump_directory.c_str());
			}
		}
		if (const char *dir = getenv("TAKEDOWN_DEV_FRAME_DUMP"))
		{
			dump_directory = dir;
			if (const char *every = getenv("TAKEDOWN_DEV_FRAME_DUMP_EVERY"))
				dump_every = std::max(1u, unsigned(strtoul(every, nullptr, 10)));
			// Optional window: from TAKEDOWN_DEV_FRAME_DUMP_AFTER seconds after the start
			// and presented frame TAKEDOWN_DEV_FRAME_DUMP_FROM, at most
			// TAKEDOWN_DEV_FRAME_DUMP_COUNT frames.
			if (const char *after = getenv("TAKEDOWN_DEV_FRAME_DUMP_AFTER"))
				dump_after = std::chrono::seconds(atoi(after));
			if (const char *from = getenv("TAKEDOWN_DEV_FRAME_DUMP_FROM"))
				dump_from = strtoull(from, nullptr, 10);
			if (const char *count = getenv("TAKEDOWN_DEV_FRAME_DUMP_COUNT"))
				dump_count = unsigned(strtoul(count, nullptr, 10));
		}
	}

	std::chrono::seconds quit_after{ 0 };
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	std::string dump_directory;
	unsigned dump_every = 300;
	std::chrono::seconds dump_after{ 0 };
	unsigned dump_count = 0;
	uint64_t dump_from = 0; // first presented frame to dump
	unsigned dumped = 0;
	unsigned burst_left = 0; // F9

	static float half_to_float(uint16_t h)
	{
		const uint32_t sign = uint32_t(h >> 15) << 31;
		uint32_t exponent = (h >> 10) & 0x1f;
		uint32_t mantissa = h & 0x3ff;
		uint32_t bits;
		if (exponent == 0)
		{
			if (mantissa == 0)
				bits = sign;
			else
			{
				exponent = 127 - 15 + 1;
				while ((mantissa & 0x400) == 0)
				{
					mantissa <<= 1;
					exponent--;
				}
				bits = sign | (exponent << 23) | ((mantissa & 0x3ff) << 13);
			}
		}
		else if (exponent == 31)
			bits = sign | 0x7f800000u | (mantissa << 13);
		else
			bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
		float f;
		memcpy(&f, &bits, sizeof(f));
		return f;
	}

	// Writes the presented picture: the composed UI target when the UI layer is
	// on, else the scanout. Call after the frame's command buffer is submitted.
	void dump_frame(Device &device)
	{
		const bool composed = bool(ui_target);
		if (!composed && !vsync.image)
			return;
		auto &image = composed ? *ui_target : *vsync.image;
		const VkImageLayout layout = composed ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
		const uint32_t texel_bytes = composed ? 8 : 4;
		const uint32_t w = image.get_width(), h = image.get_height();
		BufferCreateInfo info = {};
		info.domain = BufferDomain::CachedHost;
		info.size = VkDeviceSize(w) * h * texel_bytes;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		auto buffer = device.create_buffer(info);
		auto cmd = device.request_command_buffer();
		cmd->image_barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd->copy_image_to_buffer(*buffer, image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
		cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, 0,
		                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		Fence fence;
		device.submit(cmd, &fence);
		fence->wait();
		auto *mapped = static_cast<uint8_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
		// Conversion and PNG encoding run on background threads so a dump burst
		// (F9) does not stall the renderer, and through it the game. Frames wait
		// in memory (capped); past the cap a frame is skipped and logged.
		const size_t bytes = size_t(w) * h * (composed ? 8 : 4);
		static std::atomic<size_t> queued_bytes{ 0 };
		static std::atomic<unsigned> encoders{ 0 };
		constexpr size_t kQueueCap = size_t(3) << 30;
		char path[1024];
		snprintf(path, sizeof(path), "%s/frame-%06llu.png", dump_directory.c_str(),
		         static_cast<unsigned long long>(parser.get_vsync_count()));
		if (queued_bytes.load() + bytes > kQueueCap)
		{
			LOGW("Frame dump: queue full, skipped %s.\n", path);
			device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
			return;
		}
		queued_bytes += bytes;
		auto raw = std::make_shared<std::vector<uint8_t>>(mapped, mapped + bytes);
		device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
		const bool swap_rb = image.get_format() == VK_FORMAT_B8G8R8A8_UNORM || image.get_format() == VK_FORMAT_B8G8R8A8_SRGB;
		const int format = int(image.get_format());
		std::thread([raw, bytes, composed, swap_rb, w, h, format, file = std::string(path)]() {
			// At most four encoders at once; the rest wait their turn.
			while (encoders.fetch_add(1) >= 4)
			{
				encoders.fetch_sub(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			std::vector<uint8_t> converted;
			uint8_t *pixels = raw->data();
			if (composed)
			{
				converted.resize(size_t(w) * h * 4);
				const auto *halves = reinterpret_cast<const uint16_t *>(raw->data());
				for (size_t i = 0; i < size_t(w) * h * 4; i++)
					converted[i] = uint8_t(std::min(1.0f, std::max(0.0f, half_to_float(halves[i]))) * 255.0f + 0.5f);
				pixels = converted.data();
			}
			if (swap_rb)
				for (size_t i = 0; i < size_t(w) * h; i++)
					std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
			for (size_t i = 0; i < size_t(w) * h; i++)
				pixels[i * 4 + 3] = 0xff;
			stbi_write_png_compression_level = 1;
			if (!stbi_write_png(file.c_str(), int(w), int(h), 4, pixels, int(w) * 4))
				LOGE("Frame dump: could not write %s.\n", file.c_str());
			else
				LOGI("Frame dump: %s (%ux%u, format %d).\n", file.c_str(), w, h, format);
			encoders.fetch_sub(1);
			queued_bytes -= bytes;
		}).detach();
	}
	bool dump_requested = false;
	// TAKEDOWN_DEV_UI_LAYOUT_LOG: log the UI layout of the frame after each dump.
	bool dump_layout_next = false;

	~StreamApplication() override
	{
		wait_native_task();
		watchdog_stop = true;
		if (watchdog.joinable())
			watchdog.join();
		SDL_SetEventFilter(nullptr, nullptr);
	}

	StreamOptions options;
	bool live = false;
	std::unique_ptr<GSInterface> iface;
	GSDumpParser parser;
	InputBridge input;
	std::unique_ptr<TakedownRenderer::CompletionWriter> completion;
#if defined(_WIN32)
	std::unique_ptr<td::WindowsFrameTimestampSink> timestamps;
#endif
	bool normal_present_policy = false;
	bool stats_enabled = false;
	unsigned stats_every = 600;
	std::chrono::steady_clock::time_point stats_window_start = {};
	unsigned stats_window_frames = 0;
	double stream_wait_seconds = 0.0;
	double render_seconds = 0.0;
	// UI layer CPU time: layout, placement, recording.
	double ui_seconds[3] = {};
	double ui_render_seconds = 0.0;
	bool gpu_timestamps = false;
	double gs_gpu_seconds_logged = 0.0;
	std::chrono::steady_clock::time_point last_heartbeat = {};
	// Diagnostics (TAKEDOWN_GS_STATS): which step of render_frame is running.
	std::atomic<int> stage{ 0 };
	std::atomic<bool> watchdog_stop{ false };
	std::thread watchdog;

	ScanoutResult vsync = {};
	uint64_t frames_drawn = 0;
	bool is_eof = false;
	bool finished = false;
	FlatRenderer flat_renderer;
	TakedownRenderer::HostUiRenderer host_ui;
	// The game's UI layer (GSInterface::takedown_ui_*), on with widescreen or High
	// Resolution UI: the world and the UI are composed in ui_target (window
	// resolution), which is then drawn over the picture rectangle. Without High
	// Resolution UI the UI is drawn at the game's 448 lines into ui_low_target first.
	TakedownRenderer::NativeUiLayer ui_layer;
	TakedownRenderer::NativeUiLayer::Layout ui_layout;
	ImageHandle ui_target;
	ImageHandle ui_low_target;
	// TAKEDOWN_NATIVE_SCENE=1: the game's 640x448 target redrawn by the host
	// rasterizer (GSInterface::takedown_scene_*) replaces the GS picture of it.
	// TAKEDOWN_NATIVE_SCENE_SCALE sets its resolution (default: the render scale).
	TakedownRenderer::NativeScene native_scene;
	const Vulkan::Image *native_scene_image = nullptr;
	vec4 native_scene_gain = vec4(1.0f);
	// The native picture is drawn one VSync late, on a worker thread, while the
	// main thread reads the next VSync's stream: collect_native_scene takes a
	// VSync's frames after its stream, kick_native_scene starts drawing them when
	// the next frame begins, finish_native_scene waits for them after that frame's
	// stream. The UI layer shows the same VSync's batches (native_ui_shown) so it
	// stays in step with the picture. NativeScene is used only by that task.
	struct NativeJob
	{
		bool valid = false;
		std::vector<std::shared_ptr<const GSInterface::TakedownSceneFrame>> frames; // drawn in order
		std::shared_ptr<const GSInterface::TakedownSceneFrame> displayed;
		bool keep = false; // nothing new: the last picture stays
		bool hidden = false;
		uint32_t scale = 1;
		uint64_t vsync = 0;
		std::vector<GSInterface::TakedownUiBatch> ui;
	} native_job;
	uint64_t native_shown_cv = 0;
	Granite::TaskGroupHandle native_task;
	const Vulkan::Image *native_task_image = nullptr;
	std::vector<GSInterface::TakedownUiBatch> native_ui_shown;
	uint64_t native_scene_serial = ~uint64_t(0);
	bool native_scene_logged = false;
	// The GS's own render scale (1 while the native scene replaces its picture).
	uint32_t gs_resolution_scale = 1;
	bool gs_low_applied = false;
	bool native_scene_hidden_logged = false;

	// Host presentation state (PC options) as the renderer currently applies it.
	td::ParallelGsHostPresentation presentation;
	// FSR 3.1 (record version 4). While it is on, the render scale is FSR's:
	// chosen from the picture height (fsr_picture_height, the last frame's).
	td::ParallelGsUpscaling upscaling;
	td::ParallelGsUpscaling upscaling_record;
	TakedownRenderer::FsrUpscaler fsr;
	// Until the first frame gives the real picture height.
	uint32_t fsr_picture_height = 1080;
	bool fsr_height_known = false;
	uint32_t fsr_logged_scale = 0;
	const Vulkan::Image *fsr_output = nullptr;
	// Temporal AA (FSR with the native scene's jitter, depth and motion; TAKEDOWN_FSR_TAA=0 turns it off): the jitter
	// sequence's position, whether the last native job drew a new frame, and the output kept for a repeated one.
	uint32_t taa_index = 0;
	bool taa_new_frame = false;
	const Vulkan::Image *taa_last_output = nullptr;
	bool taa_logged = false;
	uint64_t taa_frames = 0, taa_moving = 0, taa_static = 0, taa_unmatched = 0, taa_disagree = 0;
	bool taa_wanted() const
	{
		static const bool off = getenv("TAKEDOWN_FSR_TAA") && atoi(getenv("TAKEDOWN_FSR_TAA")) == 0;
		return !off && upscaling.fsr_preset != 0 && fsr_height_known;
	}
	std::chrono::steady_clock::time_point fsr_last_frame = {};
	uint64_t presentation_serial = 0;
	uint64_t host_ui_serial = 0;
	bool presentation_applied = false;
	bool display_mode_applied = false;

	// Close handling in live mode: wait for the runtime to close the pipe.
	std::chrono::steady_clock::time_point close_deadline = {};
	bool close_pending = false;

	// FPS counter over presented game frames.
	std::chrono::steady_clock::time_point fps_window_start = std::chrono::steady_clock::now();
	unsigned fps_window_frames = 0;
	float fps = 0.0f;
	std::chrono::steady_clock::time_point fps_sample_start = std::chrono::steady_clock::now();
	unsigned fps_sample_frames = 0;

	// Recent FPS samples (one per 0.25 s), used for the 5 s average and the
	// graph. The graph spans exactly the same 5 s window as the average.
	static constexpr double kFpsGraphSeconds = 5.0;
	static constexpr double kFpsSampleSeconds = 0.25;
	struct FpsSample
	{
		float fps;
		std::chrono::steady_clock::time_point time;
	};
	static constexpr size_t kFpsHistory = 48;
	std::array<FpsSample, kFpsHistory> fps_history = {};
	size_t fps_history_count = 0;
	size_t fps_history_head = 0;

	// Last logged world blit geometry.
	uint32_t logged_source_w = 0, logged_source_h = 0, logged_output_w = 0, logged_output_h = 0, logged_taps = ~0u;

	std::string get_name() override
	{
		return "Takedown Recomp " TAKEDOWN_RENDERER_VERSION;
	}

	bool event_filter_installed = false;

	void on_device_created(const DeviceCreatedEvent &e)
	{
		// SDL is initialized by now (the platform starts after the app is created).
		if (!event_filter_installed)
		{
			SDL_SetEventFilter(&InputBridge::filter, &input);
			event_filter_installed = true;
		}

		// We will cycle through many memory contexts per frame most likely.
		e.get_device().init_frame_contexts(12);

		iface = std::make_unique<GSInterface>();

		GSOptions opts = {};
		opts.dynamic_super_sampling = true;

		if (!iface->init(&e.get_device(), opts))
		{
			request_shutdown();
			LOGE("Failed to init GSInterface.\n");
			return;
		}
		iface->takedown_ui_watch(kTitleScreenLogo);
		iface->takedown_ui_watch(kRaceHudPanel);
		iface->takedown_ui_watch(kMenuArc);
		// TAKEDOWN_GS_TIMESTAMPS: GPU time of the GS and of the presentation, in
		// the frame stats line.
		gpu_timestamps = env_enabled("TAKEDOWN_GS_TIMESTAMPS");
		if (gpu_timestamps)
		{
			DebugMode mode;
			mode.timestamps = true;
			iface->set_debug_mode(mode);
		}

		std::string error;
		GsShm *stream_ring = nullptr;
		FILE *file = TakedownRenderer::open_graphics_stream(options.stream_path, error, native_scene_wanted() && !env_enabled("TAKEDOWN_NS3_OFF"), &stream_ring);
		if (!file || !parser.open_raw_file(file, 4 * 1024 * 1024, iface.get()))
		{
			request_shutdown();
			iface.reset();
			LOGE("Failed to open stream: %s\n", error.c_str());
			return;
		}

		ns2h_setup();
		ns3_setup();

		if (stream_ring)
			parser.attach_ring(stream_ring);

		parser.set_panini_callback([this](const GSDumpParser::PaniniRecord &record) {
			if (iface)
				iface->takedown_panini_arm(float(record.tangent) / 1000.0f, float(record.strength) / 100.0f,
				                           float(record.half_width) / 1000.0f,
				                           (record.flags & 1u) ? 16.0f / 9.0f : 4.0f / 3.0f);
		});

		presentation_applied = false;
		display_mode_applied = false;
		apply_presentation(true);
	}

	// ---- NS2H: hybrid replay of an NS-1 recording (tools/ns2_hybrid.hpp) ----
	std::unique_ptr<nsc::Scene> ns2h_scene;
	std::shared_ptr<ns2h::Plan> ns2h_plan;
	void ns2h_setup()
	{
		const char *path = getenv("NS2H_NSC");
		if ((!path || !*path) && getenv("NS2RT_PLAN") && iface)
		{
			ns2rt_setup(nullptr);
			if (rt_plan) iface->takedown_set_hybrid(rt_plan);
			return;
		}
		if ((!path || !*path) && getenv("NS2RT_COUNT") && iface)
		{
			// Counts the drawing kicks (batches then carry their kick ranges) without dropping any.
			struct EmptyPlan final : ParallelGS::GSInterface::TakedownHybridPlan
			{
				void build(uint32_t, std::vector<ParallelGS::GSInterface::TakedownHybridDraw> &) override {}
				std::shared_ptr<const ParallelGS::GSInterface::TakedownNativeTexture> palettize(uint64_t, const uint32_t *, uint64_t) override { return nullptr; }
			};
			iface->takedown_set_hybrid(std::make_shared<EmptyPlan>());
			return;
		}
		if (!path || !*path || !iface) return;
		ns2h::Options o;
		o.world = o.props = o.cars = false;
		const std::string cats = getenv("NS2H_CATS") ? getenv("NS2H_CATS") : "A,B";
		for (char c : cats) { if (c == 'A') o.world = true; if (c == 'B') o.props = true; if (c == 'C') o.cars = true; }
		if (const char *d = getenv("NS2H_KICK_DELTA")) o.kick_delta = atoll(d);
		if (const char *sp = getenv("NS2H_SKIP_PROGS")) for (const char *q = sp; *q;) { o.skip_programs |= 1u << (atoi(q) & 31); while (*q && *q != ',') q++; if (*q) q++; }
		if (getenv("NS2H_LIVE_PAL")) o.live_palette = atoi(getenv("NS2H_LIVE_PAL")) != 0;
		if (const char *dn = getenv("NS2H_DONOR")) { o.donor_cars = o.donor_world = o.donor_props = false; for (const char *q = dn; *q; q++) { if (*q == 'C') o.donor_cars = true; if (*q == 'A') o.donor_world = true; if (*q == 'B') o.donor_props = true; } }
		if (getenv("NS2H_RT")) o.rt = atoi(getenv("NS2H_RT")) != 0;
		if (getenv("NS2H_DAMAGE")) o.damage = atoi(getenv("NS2H_DAMAGE")) != 0;
		if (getenv("NS2H_GPU_CARS")) o.gpu_cars = atoi(getenv("NS2H_GPU_CARS")) != 0;
		o.faces = getenv("NS2H_FACES") && atoi(getenv("NS2H_FACES")) != 0;
		if (const char *fr = getenv("NS2H_FRAMES")) { unsigned a = 0, b = ~0u; if (sscanf(fr, "%u-%u", &a, &b) == 2) { o.first_frame = a; o.last_frame = b; } }
		const auto t0 = std::chrono::steady_clock::now();
		ns2h_scene = std::make_unique<nsc::Scene>();
		try { ns2h_scene->open(path); }
		catch (const std::exception &e) { LOGE("NS2H: %s\n", e.what()); ns2h_scene.reset(); return; }
		ns2h_plan = std::make_shared<ns2h::Plan>(*ns2h_scene, o);
		const auto &c = ns2h_plan->counts();
		LOGI("NS2H: %zu frames, %llu draws (%llu kicks); converted %llu draws / %llu kicks in %llu ranges; %llu meshes, %llu textures; load %.2f s\n",
		     ns2h_scene->frames().size(), (unsigned long long)c.draws_total, (unsigned long long)c.kicks_total,
		     (unsigned long long)c.draws_converted, (unsigned long long)c.kicks_converted, (unsigned long long)c.ranges,
		     (unsigned long long)c.meshes, (unsigned long long)c.textures, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
		LOGI("NS2H: not eligible: material %llu, mesh %llu, cluster %llu\n", (unsigned long long)c.not_eligible_material,
		     (unsigned long long)c.not_eligible_mesh, (unsigned long long)c.not_eligible_cluster);
		for (int k = 1; k <= 3; k++)
			LOGI("NS2H: category %d: %llu draws / %llu kicks recorded, %llu draws / %llu kicks converted\n", k, (unsigned long long)c.by_category_draws[k],
			     (unsigned long long)c.by_category_kicks[k], (unsigned long long)ns2h_plan->converted_draws_[k], (unsigned long long)ns2h_plan->converted_kicks_[k]);
		static const char *reasons[8] = { "mesh", "target", "primitive", "render-target texture", "texture missing", "other", "overlap", "-" };
		for (int k = 1; k <= 3; k++)
			for (int r = 0; r < 7; r++)
				if (c.left_draws[k][r])
					LOGI("NS2H: category %d left to the GS, %s: %llu draws / %llu kicks\n", k, reasons[r], (unsigned long long)c.left_draws[k][r],
					     (unsigned long long)c.left_kicks[k][r]);
		ns2rt_setup(ns2h_plan);
		iface->takedown_set_hybrid(rt_plan ? rt_plan : std::shared_ptr<ParallelGS::GSInterface::TakedownHybridPlan>(ns2h_plan));
	}
	std::shared_ptr<ns2rt::Plan> rt_plan;
	// NS2RT_PLAN=<file.rtp>: the render-target and post chain passes (tools/ns2_rt.hpp), on top of the recording's plan if any.
	void ns2rt_setup(std::shared_ptr<ParallelGS::GSInterface::TakedownHybridPlan> inner)
	{
		const char *path = getenv("NS2RT_PLAN");
		if (!path || !*path || !iface) return;
		ns2rt::Plan::Options o;
		if (const char *kinds = getenv("NS2RT_KINDS")) { o.env = strstr(kinds, "env") != nullptr; o.bloom = strstr(kinds, "bloom") != nullptr; o.mb = strstr(kinds, "mb") != nullptr; }
		else if (const char *sc = getenv("RC_SCALE"); sc && atoi(sc) > 1 && !getenv("NS2RT_BLOOM4")) o.bloom = false; // bloom above 1x stays on the GS (see REPORT.md)
		if (const char *fr = getenv("NS2RT_FRAMES")) { unsigned long long a = 0, b = ~0ull; if (sscanf(fr, "%llu-%llu", &a, &b) == 2) { o.first_vsync = a; o.last_vsync = b; } }
		try { rt_plan = std::make_shared<ns2rt::Plan>(path, std::move(inner), o); }
		catch (const std::exception &e) { LOGE("NS2RT: %s\n", e.what()); return; }
		const auto &c = rt_plan->counts();
		LOGI("NS2RT: %llu frames; ranges env %llu (%llu kicks), bloom %llu (%llu), motion blur %llu (%llu)\n", (unsigned long long)c.frames,
		     (unsigned long long)c.ranges[0], (unsigned long long)c.kicks[0], (unsigned long long)c.ranges[1], (unsigned long long)c.kicks[1],
		     (unsigned long long)c.ranges[2], (unsigned long long)c.kicks[2]);
	}
	// ---- NS3: live native commands (tools/ns3_live.hpp, include/takedown/platform/native_cmd_protocol.hpp) ----
	std::shared_ptr<ns3::LivePlan> ns3_plan;
	void ns3_setup()
	{
		if (!iface || env_enabled("TAKEDOWN_NS3_OFF") || !native_scene_wanted())
			return;
		ns3_plan = std::make_shared<ns3::LivePlan>();
		iface->takedown_set_live(ns3_plan, false);
		parser.set_native_cmd_callback([this](const uint8_t *data, size_t size) {
			ns3_plan->on_record(data, size);
			iface->takedown_set_live_shadow(ns3_plan->shadow());
		});
		LOGI("NS3: native command consumer on (protocol v%u)\n", unsigned(takedown::native_cmd::kVersion));
	}
	void ns3_report(const char *tag)
	{
		if (!ns3_plan || !iface)
			return;
		wait_native_task();
		iface->flush();
		ns3_plan->fold_capture_counters();
		ns3_plan->report(tag);
		const auto s = iface->takedown_live_stats();
		fprintf(stderr, "[NS3] %s gs: vsync %llu tokens %llu token-vertices %llu vertex-kicks %llu (synthetic %llu, real %llu) donor hits %llu misses %llu\n", tag,
		        (unsigned long long)parser.get_vsync_count(), (unsigned long long)s.tokens, (unsigned long long)s.strip_vertices, (unsigned long long)s.vertex_kicks,
		        (unsigned long long)s.fake_kicks, (unsigned long long)(s.vertex_kicks - s.fake_kicks), (unsigned long long)s.donor_hits, (unsigned long long)s.donor_misses);
		fprintf(stderr, "[NS3] %s blended native batches: world %llu prop %llu car %llu\n", tag, (unsigned long long)s.blended[1], (unsigned long long)s.blended[2], (unsigned long long)s.blended[3]);
		fflush(stderr);
	}
	void ns2h_log_vsync()
	{
		static const bool on = getenv("NS2H_LOG_VSYNC") != nullptr;
		if (!on || !iface) return;
		const auto s = iface->takedown_hybrid_stats();
		LOGI("NS2H vs=%llu kicks=%llu skipped=%llu ranges=%llu batches=%llu misaligned=%llu/%llu donors=%llu/%llu prims=%llu degenerate=%llu\n", (unsigned long long)parser.get_vsync_count(),
		     (unsigned long long)s.kicks, (unsigned long long)s.skipped, (unsigned long long)s.ranges, (unsigned long long)s.batches,
		     (unsigned long long)s.misaligned_begin, (unsigned long long)s.misaligned_end, (unsigned long long)s.donors, (unsigned long long)s.donor_misses, (unsigned long long)s.donor_prims, (unsigned long long)s.donor_degenerate);
		LOGI("NS2H queue at range begin %llu/%llu/%llu/%llu, after %llu/%llu/%llu/%llu\n", (unsigned long long)s.begin_queue[0], (unsigned long long)s.begin_queue[1], (unsigned long long)s.begin_queue[2], (unsigned long long)s.begin_queue[3], (unsigned long long)s.after_queue[0], (unsigned long long)s.after_queue[1], (unsigned long long)s.after_queue[2], (unsigned long long)s.after_queue[3]);
	}

	void on_device_destroyed(const DeviceCreatedEvent &)
	{
		wait_native_task();
		native_job = {};
		native_ui_shown.clear();
		iface.reset();
		vsync = {};
		host_ui.reset_device_objects();
		ui_layer.reset_device_objects();
		ui_target.reset();
		ui_low_target.reset();
		native_scene.reset_device_objects();
		fsr.reset_device_objects();
		fsr_output = nullptr;
		native_scene_image = nullptr;
		native_scene_serial = ~uint64_t(0);
	}

	// Applies the latest PC options record. force = apply the defaults too.
	void apply_presentation(bool force)
	{
		const uint64_t serial = parser.get_host_presentation_serial();
		td::ParallelGsHostPresentation next = presentation;
		if (serial != presentation_serial)
		{
			presentation_serial = serial;
			const auto &bytes = parser.get_host_presentation();
			td::ParallelGsUpscaling next_upscaling = upscaling_record;
			if (!td::decode_parallel_gs_host_presentation(bytes.data(), bytes.size(), next, next_upscaling))
			{
				LOGE("Ignoring a malformed host presentation record.\n");
				next = presentation;
			}
			else
				upscaling_record = next_upscaling;
		}
		else if (!force)
			return;
		if (const char *e = getenv("RC_AA"))
			next.spatial_aa = uint8_t(atoi(e));
		apply_upscaling(next);
		// SMAA x2 works on the samples of a render-scale picture: at least 200%.
		if (upscaling.fsr_preset == 0 &&
		    TakedownRenderer::spatial_aa_smaa_mode(next.spatial_aa) == TakedownRenderer::SmaaMode::X2 &&
		    next.resolution_scale < 2)
		{
			next.resolution_scale = 2;
			next.samples = std::max<uint8_t>(next.samples, 4);
		}

		if (const char *e = getenv("RC_SCALE"))
		{
			next.resolution_scale = uint8_t(atoi(e));
			next.samples = next.resolution_scale >= 5 ? 64 : next.resolution_scale >= 3 ? 16 : next.resolution_scale == 2 ? 4 : 1;
		}
		if (const char *e = getenv("RC_WIDE")) next.widescreen = atoi(e) != 0;
		if (const char *e = getenv("RC_HIUI")) next.high_resolution_ui = atoi(e) != 0;
		LOGI("RC presentation serial %llu: scale %u samples %u wide %d hiui %d\n", (unsigned long long)serial,
		     unsigned(next.resolution_scale), unsigned(next.samples), int(next.widescreen), int(next.high_resolution_ui));
		const bool first = !presentation_applied;
		if (first || next.widescreen != presentation.widescreen || next.vsync != presentation.vsync ||
		    next.high_resolution_ui != presentation.high_resolution_ui || next.fps_counter != presentation.fps_counter ||
		    next.spatial_aa != presentation.spatial_aa || next.display_mode != presentation.display_mode ||
		    next.overlay.size() != presentation.overlay.size())
		{
			LOGI("PC options: widescreen %s, vsync %s, high-res UI %s, FPS counter %s, spatial AA %u, display %u, overlay lines %u.\n",
			     next.widescreen ? "on" : "off", next.vsync ? "on" : "off", next.high_resolution_ui ? "on" : "off",
			     next.fps_counter ? "on" : "off", unsigned(next.spatial_aa), unsigned(next.display_mode),
			     unsigned(next.overlay.size()));
		}
		// With the native scene the GS picture of the game's target is replaced, and
		// the GS only feeds the scene's small effect buffers (reflections, bloom,
		// blur): it renders at the game's own resolution, without supersampling.
		// TAKEDOWN_NATIVE_SCENE_GS_FULL=1 keeps the full render scale (comparisons).
		// On unless TAKEDOWN_NATIVE_SCENE=0 (the GS picture, for comparisons).
		const char *scene_env = getenv("TAKEDOWN_NATIVE_SCENE");
		const bool scene_on = !scene_env || !*scene_env || strcmp(scene_env, "0") != 0;
		const bool gs_low = scene_on && !env_enabled("TAKEDOWN_NATIVE_SCENE_GS_FULL");
		const bool sampling_changed = first || next.samples != presentation.samples ||
		                              next.resolution_scale != presentation.resolution_scale ||
		                              gs_low != gs_low_applied;
		const bool dither_changed = first || next.dithering != presentation.dithering;
		const bool display_changed = !display_mode_applied || next.display_mode != presentation.display_mode;
		const bool vsync_changed = first || next.vsync != presentation.vsync;
		presentation = std::move(next);
		presentation_applied = true;

		if (sampling_changed && iface)
		{
			gs_low_applied = gs_low;
			gs_resolution_scale = gs_low ? 1u : std::max<uint32_t>(1u, presentation.resolution_scale);
			iface->set_super_sampling_rate(to_super_sampling(gs_low ? 1 : presentation.samples), true,
			                               gs_resolution_scale >= 2);
			// 3x and 4x take their detail from the 4x4 grid of 16 samples, 5x and 6x
			// from the 8x8 grid of 64 (wave64 GPUs; others fall back to 16). The
			// scanout makes the requested size from whichever grid is in use.
			parser.set_scanout_scale(gs_resolution_scale);
			LOGI("PC options: %ux supersampling, %ux resolution%s.\n", unsigned(presentation.samples),
			     unsigned(presentation.resolution_scale),
			     gs_low ? " (native scene; the GS renders at 1x without supersampling)" : "");
		}

		if (iface)
		{
			// TAKEDOWN_DEV_NO_UI_LAYER keeps the UI in the GS picture (for comparisons).
			// With FSR the UI is kept out of the upscaled picture and drawn after it.
			const bool ui_layer_on = (presentation.widescreen || presentation.high_resolution_ui ||
			                          upscaling.fsr_preset != 0) &&
			                         !env_enabled("TAKEDOWN_DEV_NO_UI_LAYER");
			if (ui_layer_on != iface->takedown_ui_is_enabled())
				LOGI("PC options: UI layer %s (%s).\n", ui_layer_on ? "on" : "off",
				     presentation.high_resolution_ui ? "window resolution" : "game resolution");
			iface->takedown_ui_set_enabled(ui_layer_on);
			if (scene_on != iface->takedown_scene_is_enabled())
				LOGI("Native scene: %s.\n", scene_on ? "on" : "off");
			iface->takedown_scene_set_enabled(scene_on);
			// While native frames are shown the GS's own drawing is not needed (F10,
			// which shows the GS picture, and TAKEDOWN_GS_BACKGROUND=1 keep it).
			iface->takedown_scene_set_gs_rendering(!scene_on || input.native_scene_hidden ||
			                                       env_enabled("TAKEDOWN_GS_BACKGROUND"));
		}

		if (dither_changed && iface)
		{
			iface->set_dithering(presentation.dithering);
			LOGI("PC options: dithering %s.\n", presentation.dithering ? "on" : "off");
		}

		if (vsync_changed)
			get_wsi().set_present_mode(presentation.vsync ? PresentMode::SyncToVBlank : PresentMode::UnlockedMaybeTear);

		if (display_changed)
		{
			display_mode_applied = true;
			apply_display_mode(presentation.display_mode);
		}
	}

	// FSR from the record, or from TAKEDOWN_FSR=<0..5> and
	// TAKEDOWN_FSR_SHARPNESS=<0..100> (tests and replays). With FSR on, the
	// render scale and its samples follow the preset and the picture height, and
	// the spatial AA pass is not used.
	void apply_upscaling(td::ParallelGsHostPresentation &next)
	{
		td::ParallelGsUpscaling wanted = upscaling_record;
		if (const char *e = getenv("TAKEDOWN_FSR"); e && *e)
			wanted.fsr_preset = uint8_t(std::max(0, std::min(int(td::kParallelGsFsrPresetMaximum), atoi(e))));
		if (const char *e = getenv("TAKEDOWN_FSR_SHARPNESS"); e && *e)
			wanted.sharpness = uint8_t(std::max(0, std::min(100, atoi(e))));
		if (wanted.fsr_preset != upscaling.fsr_preset || wanted.sharpness != upscaling.sharpness)
		{
			LOGI("PC options: FSR 3.1 %s, sharpness %u%%.\n",
			     TakedownRenderer::fsr_preset_name(TakedownRenderer::FsrPreset(wanted.fsr_preset)),
			     unsigned(wanted.sharpness));
			fsr.reset_history();
			fsr_logged_scale = 0;
		}
		upscaling = wanted;
		if (upscaling.fsr_preset == 0)
			return;
		const uint32_t scale =
		    TakedownRenderer::fsr_render_scale(TakedownRenderer::FsrPreset(upscaling.fsr_preset), fsr_picture_height);
		next.resolution_scale = uint8_t(scale);
		next.samples = uint8_t(scale >= 5 ? 64 : scale >= 3 ? 16 : scale == 2 ? 4 : 1);
		next.spatial_aa = 0;
		if (scale != fsr_logged_scale && fsr_height_known)
		{
			fsr_logged_scale = scale;
			LOGI("FSR 3.1: %s renders at %ux (%ux%u) for a %u-line picture.\n",
			     TakedownRenderer::fsr_preset_name(TakedownRenderer::FsrPreset(upscaling.fsr_preset)), scale,
			     640 * scale, 448 * scale, fsr_picture_height);
		}
	}

	// Before the frame's command buffer: the FSR picture for draw_world.
	void run_fsr(Device &device, const TakedownRenderer::PictureRect &picture)
	{
		fsr_output = nullptr;
		const auto now = std::chrono::steady_clock::now();
		const float frame_ms = fsr_last_frame.time_since_epoch().count() == 0
		                           ? 16.7f
		                           : float(std::chrono::duration<double, std::milli>(now - fsr_last_frame).count());
		fsr_last_frame = now;
		if (upscaling.fsr_preset == 0)
			return;
		const uint32_t height = uint32_t(picture.height + 0.5f);
		const uint32_t width = uint32_t(picture.width + 0.5f);
		if ((height != fsr_picture_height || !fsr_height_known) && height > 0)
		{
			// The first frame, or the window changed size: the render scale may follow.
			fsr_picture_height = height;
			fsr_height_known = true;
			fsr_logged_scale = 0;
			apply_presentation(true);
		}
		if (!vsync.image && !native_scene_image)
			return;
		const Vulkan::Image &source = native_scene_image ? *native_scene_image : *vsync.image;
		const vec4 gain4 = native_scene_image ? native_scene_gain : vec4(1.0f);
		const float gain[3] = { gain4.x, gain4.y, gain4.z };
		TakedownRenderer::FsrTaaInputs taa_inputs;
		const TakedownRenderer::FsrTaaInputs *taa = nullptr;
		if (native_scene_image && taa_wanted())
		{
			const auto &out = native_scene.taa_outputs();
			if (!taa_new_frame && taa_last_output)
			{
				// The same native frame again: FSR has already taken it (with its jitter).
				fsr_output = taa_last_output;
				return;
			}
			if (out.valid)
			{
				taa_inputs.depth = out.depth;
				taa_inputs.motion = out.motion;
				taa_inputs.jitter_px[0] = out.jitter_px[0];
				taa_inputs.jitter_px[1] = out.jitter_px[1];
				taa_inputs.near_plane = out.near_plane;
				taa_inputs.fov_y = out.fov_y;
				taa = &taa_inputs;
				taa_frames++;
				taa_moving += out.moving_draws;
				taa_static += out.static_draws;
				taa_unmatched += out.unmatched_draws;
				taa_disagree += out.camera_disagreements;
				if (!taa_logged)
				{
					taa_logged = true;
					LOGI("FSR 3.1: temporal AA on (jitter, depth and motion from the native scene).\n");
				}
				if ((taa_frames % 600) == 1 || getenv("TAKEDOWN_TAA_LOG"))
					LOGI("TAA: frames %llu, per frame %.1f moving, %.1f static, %.1f unmatched draws, %.2f camera disagreements; jitter %.3f %.3f\n",
					     (unsigned long long)taa_frames, double(taa_moving) / double(taa_frames), double(taa_static) / double(taa_frames),
					     double(taa_unmatched) / double(taa_frames), double(taa_disagree) / double(taa_frames),
					     double(out.jitter_px[0]), double(out.jitter_px[1]));
			}
		}
		fsr_output = fsr.upscale(device, source, gain, width, height, float(upscaling.sharpness) / 100.0f, frame_ms, taa);
		taa_last_output = taa ? fsr_output : nullptr;
		taa_new_frame = false;
	}

	struct DisplayModeTask
	{
		td::ParallelGsDisplayMode mode;
	};

	static void SDLCALL run_display_mode(void *user)
	{
		std::unique_ptr<DisplayModeTask> task(static_cast<DisplayModeTask *>(user));
		int count = 0;
		SDL_Window **windows = SDL_GetWindows(&count);
		SDL_Window *window = windows && count > 0 ? windows[0] : nullptr;
		SDL_free(windows);
		if (!window)
			return;

		if (task->mode == td::ParallelGsDisplayMode::Windowed)
		{
			if (!SDL_SetWindowFullscreen(window, false))
				LOGE("Display mode: could not leave fullscreen: %s\n", SDL_GetError());
			LOGI("Display mode: windowed.\n");
			return;
		}

		const SDL_DisplayMode *mode = nullptr;
		if (task->mode == td::ParallelGsDisplayMode::Fullscreen)
		{
			// Exclusive fullscreen at the desktop resolution and refresh rate.
			const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
			mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
			if (!mode)
				LOGE("Display mode: could not choose the fullscreen mode: %s\n", SDL_GetError());
		}

		if (!SDL_SetWindowFullscreenMode(window, mode))
			LOGE("Display mode: could not choose the fullscreen mode: %s\n", SDL_GetError());
		if (!SDL_SetWindowFullscreen(window, true))
			LOGE("Display mode: could not enter fullscreen: %s\n", SDL_GetError());
		LOGI("Display mode: %s.\n", mode ? "fullscreen" : "borderless");
	}

	void apply_display_mode(td::ParallelGsDisplayMode mode)
	{
		// SDL window calls belong to the thread that owns the window.
		SDL_RunOnMainThread(&StreamApplication::run_display_mode, new DisplayModeTask{ mode }, false);
	}

	void finish_stream()
	{
		if (finished)
			return;
		finished = true;
		ns3_report("final");
		const bool truncated = parser.was_truncated();
		if (completion)
		{
			if (truncated)
				completion->error(td::ParallelGsCompletionError::StreamTruncated, parser.get_vsync_count());
			completion->finish(!truncated);
		}
#if defined(_WIN32)
		if (timestamps && !timestamps->finish(parser.get_vsync_count()))
			LOGE("Failed to finalize scanout/present timestamps.\n");
#endif
		if (live)
		{
			LOGI("Graphics pipe closed; exiting.\n");
			request_shutdown();
		}
	}

	static void log_native_scene(const ParallelGS::GSInterface::TakedownSceneFrame &frame)
	{
		LOGI("Native scene dump: FBP block %u, gain %.2f %.2f %.2f, %zu batches.\n", frame.fbp * 32u,
		     double(frame.gain.x), double(frame.gain.y), double(frame.gain.z), frame.batches.size());
		for (size_t i = 0; i < frame.batches.size(); i++)
		{
			const auto &b = frame.batches[i];
			float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 2.0f, z1 = -1.0f;
			for (const auto &v : b.vertices)
			{
				x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
				y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
				z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
			}
			const auto &v0 = b.vertices.empty() ? ParallelGS::GSInterface::TakedownSceneVertex{} : b.vertices[0];
			if (b.invalid_blocks)
			{
				LOGI("  %zu: upload over blocks %u-%u\n", i, b.tex_tbp0, b.tex_tbp0 + b.invalid_blocks);
				continue;
			}
			LOGI("  %zu: fb %u/%u z %u tex %u/%u psm %02x%s | %zu tris box %.0f,%.0f-%.0f,%.0f z %.6f-%.6f | tex %s %ux%u sizes %.0fx%.0f region %.1f,%.1f,%.1f,%.1f "
			     "bias %.3f,%.3f wrap %u,%u tfx %u tcc %u %s | blend %u A%u B%u C%u D%u FIX %u | ztst %u zw %u fbmsk %08x | "
			     "atst %u aref %u afail %u | fog %u | v0 stq %.2f,%.2f,%.3f rgba %.2f,%.2f,%.2f,%.2f\n",
			     i, b.fbp * 32u, b.fbw, b.zbp * 32u, b.tex_tbp0, b.tex_tbw, b.tex_psm, b.self_read ? " self" : "",
			     b.vertices.size() / 3, double(x0), double(y0), double(x1), double(y1), double(z0), double(z1),
			     b.textured ? "on" : "off", b.image ? b.image->get_width() : 0u, b.image ? b.image->get_height() : 0u,
			     double(b.sizes.x), double(b.sizes.y), double(b.region.x), double(b.region.y), double(b.region.z),
			     double(b.region.w), double(b.bias.x), double(b.bias.y), b.wrap.x, b.wrap.y, b.tfx, b.tcc,
			     b.linear ? "linear" : "nearest", b.blend, b.blend_a, b.blend_b, b.blend_c, b.blend_d, b.blend_fix,
			     b.depth_test, b.depth_write, b.fbmsk, b.alpha_test, b.alpha_reference, b.alpha_fail, b.fog,
			     double(v0.s), double(v0.t), double(v0.q), double(v0.r), double(v0.g), double(v0.b), double(v0.a));
		}
		for (const auto &entry : frame.left_out)
			LOGI("  left to the GS: %u x %s\n", entry.second, entry.first.c_str());
	}

	void wait_native_task()
	{
		if (native_task)
		{
			native_task->wait();
			native_task.reset();
		}
	}

	// ---- NS2 probe (not shipped): NS2_LOG_BATCHES=<a>-<b> NS2_LOG_DIR=<dir> dumps batches ----
	void ns2_log_batches(const ParallelGS::GSInterface::TakedownSceneFrame &frame, uint64_t vs, size_t fi)
	{
		if (getenv("NS2_SUMMARY"))
		{
			size_t verts = 0;
			for (const auto &b2 : frame.batches) verts += b2.vertices.size();
			printf("NS2SUM %llu %zu %zu %zu\n", (unsigned long long)vs, fi, frame.batches.size(), verts);
		}
		if (const char *xr = getenv("NS2_LOG_XYZ"))
		{
			unsigned long long xa = 0, xb = 0;
			const char *xd = getenv("NS2_LOG_DIR");
			if (xd && sscanf(xr, "%llu-%llu", &xa, &xb) == 2 && vs >= xa && vs <= xb && fi == 0)
			{
				char xp[512]; snprintf(xp, sizeof xp, "%s/xyz_%llu.bin", xd, (unsigned long long)vs);
				if (FILE *xf = fopen(xp, "wb"))
				{
					for (const auto &b2 : frame.batches)
						if (b2.fbw == 10 && !b2.invalid_blocks)
							for (const auto &v : b2.vertices) { float t[3] = { v.x, v.y, v.z }; fwrite(t, 12, 1, xf); }
					fclose(xf);
				}
			}
		}
		const char *range = getenv("NS2_LOG_BATCHES"), *dir = getenv("NS2_LOG_DIR");
		if (!range || !dir) return;
		unsigned long long a = 0, b = 0;
		if (sscanf(range, "%llu-%llu", &a, &b) < 2 || vs < a || vs > b) return;
		char path[512]; snprintf(path, sizeof path, "%s/batches_%llu_%zu.txt", dir, (unsigned long long)vs, fi);
		FILE *f = fopen(path, "w"); if (!f) return;
		for (size_t i = 0; i < frame.batches.size(); i++)
		{
			const auto &b2 = frame.batches[i];
			fprintf(f, "B %zu fb %u fbw %u zbp %u tbp %u tbw %u psm %u tw %u th %u tex %u tfx %u tcc %u ztst %u zw %u fbmsk %08x atst %u aref %u afail %u fog %u fogcol %06x blend %u %u %u %u %u %u sizes %g %g region %g %g %g %g scissor %g %g %g %g nverts %zu native_img %d vram %d lin %d invalid %u d2a %d exact %d kf %llu kl %llu wrap %u %u %u %u bias %g %g prim %u tfx2 %u\n",
			        i, b2.fbp * 32u, b2.fbw, b2.zbp * 32u, b2.tex_tbp0, b2.tex_tbw, b2.tex_psm, b2.tex_tw, b2.tex_th, b2.textured, b2.tfx, b2.tcc, b2.depth_test, b2.depth_write,
			        b2.fbmsk, b2.alpha_test, b2.alpha_reference, b2.alpha_fail, b2.fog, b2.fogcol, b2.blend, b2.blend_a, b2.blend_b, b2.blend_c, b2.blend_d, b2.blend_fix,
			        double(b2.sizes.x), double(b2.sizes.y), double(b2.region.x), double(b2.region.y), double(b2.region.z), double(b2.region.w),
			        double(b2.scissor.x), double(b2.scissor.y), double(b2.scissor.z), double(b2.scissor.w), b2.vertices.size(), b2.native_image ? 1 : 0, b2.vram_texture ? 1 : 0, b2.linear ? 1 : 0, b2.invalid_blocks, b2.depth_to_alpha ? 1 : 0, b2.exact ? 1 : 0,
			        (unsigned long long)b2.kick_first, (unsigned long long)b2.kick_last, b2.wrap.x, b2.wrap.y, b2.wrap.z, b2.wrap.w,
			        double(b2.bias.x), double(b2.bias.y), b2.prim_kind, b2.alpha_fail);
			for (const auto &v : b2.vertices)
				fprintf(f, "V %.5f %.5f %.9f %.5f %.5f %.5f %.7f %.5f %.5f %.5f %.5f\n", double(v.x), double(v.y), double(v.z), double(v.fog), double(v.s), double(v.t), double(v.q), double(v.r), double(v.g), double(v.b), double(v.a));
		}
		fclose(f);
	}
	// NS2_INJECT=<file> (an/mkinject.py): at the file's vsync, replaces vertex ranges of captured batches with
	// native batches of the file's mesh and matrices. NS2_INJECT_DROP=1 only drops them; NS2_INJECT_GS=1 puts the
	// replaced vertices back as a captured batch (reference); NS2_INJECT_AT=late moves the batch after the last
	// depth-writing batch of its target. NS2_ROUNDTRIP=1: every world-strip batch of the frame's main target is
	// un-projected with the file's matrices to a world-space mesh and drawn natively (the whole picture through the
	// native path); NS2_ROUNDTRIP_GS=1: the same selection kept as captured batches, moved last (reference).
	// NS2_INJECT_RANGE=a-b NS2_INJECT_KEYS=n: the first native batch is appended to every frame a..b with n mesh keys (stress).
	using GSInterface_t = ParallelGS::GSInterface::TakedownSceneBatch;
	GSInterface_t ns2_stress_batch; bool ns2_have_stress = false;
	std::shared_ptr<const ParallelGS::GSInterface::TakedownSceneFrame> ns2_inject(
	    const std::shared_ptr<const ParallelGS::GSInterface::TakedownSceneFrame> &in, uint64_t vs)
	{
		using namespace ParallelGS;
		static std::vector<uint8_t> blob;
		static bool loaded = false;
		const char *path = getenv("NS2_INJECT");
		if (!path) return in;
		if (!loaded)
		{
			loaded = true;
			if (FILE *f = fopen(path, "rb"))
			{
				fseek(f, 0, SEEK_END); blob.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET);
				if (fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
				fclose(f);
			}
		}
		if (blob.size() < 40) return in;
		const uint32_t *h = reinterpret_cast<const uint32_t *>(blob.data());
		if (h[0] != 0x32534e32u) return in;
		const uint32_t ntargets = h[2], nverts = h[3], nindices = h[4];
		const uint8_t *p = blob.data() + 28 + 12 * ntargets;
		auto draw = std::make_shared<GSInterface::TakedownNativeDraw>();
		auto mesh = std::make_shared<GSInterface::TakedownNativeMesh>();
		memcpy(&mesh->key, h + 5, 8);
		mesh->vertices.resize(nverts); mesh->indices.resize(nindices);
		memcpy(draw->model, p, 64); p += 64; memcpy(draw->view, p, 64); p += 64; memcpy(draw->projection, p, 64); p += 64;
		memcpy(&draw->fog_start, p, 4); memcpy(&draw->fog_scale, p + 4, 4); p += 8;
		memcpy(draw->light_direction, p, 16); memcpy(draw->light_colour, p + 16, 16); memcpy(draw->ambient, p + 32, 16); p += 48;
		uint32_t cull; memcpy(&cull, p, 4); p += 4; draw->cull = cull != 0;
		memcpy(mesh->vertices.data(), p, nverts * sizeof(GSInterface::TakedownNativeVertex)); p += nverts * sizeof(GSInterface::TakedownNativeVertex);
		memcpy(mesh->indices.data(), p, nindices * 4);
		draw->mesh = mesh;
		if (const char *lt = getenv("NS2_INJECT_LIGHT"))
		{
			// ambient,light colour: a light straight down the world's -y axis
			float amb = 0.0f, col = 0.0f; sscanf(lt, "%f,%f", &amb, &col);
			draw->light_direction[0] = 0.0f; draw->light_direction[1] = -1.0f; draw->light_direction[2] = 0.0f; draw->light_direction[3] = 1.0f;
			for (int k = 0; k < 3; k++) { draw->ambient[k] = amb; draw->light_colour[k] = col; }
		}
		const auto fog_state = [&](GSInterface_t &b) {
			if (const char *fg = getenv("NS2_INJECT_FOG"))
			{
				float start = 100.0f, end = 220.0f; sscanf(fg, "%f,%f", &start, &end);
				b.fog = 1; b.fogcol = 0x808080;
				draw->fog_start = start; draw->fog_scale = -255.0f / (end - start);
			}
		};
		if (h[1] != vs)
		{
			unsigned long long ra = 0, rb = 0;
			const char *rng = getenv("NS2_INJECT_RANGE");
			if (!rng || sscanf(rng, "%llu-%llu", &ra, &rb) < 2 || vs < ra || vs > rb || !ns2_have_stress) return in;
			const unsigned keys = getenv("NS2_INJECT_KEYS") ? unsigned(atoi(getenv("NS2_INJECT_KEYS"))) : 1u;
			auto frame = std::make_shared<GSInterface::TakedownSceneFrame>(*in);
			size_t last_writer = ~size_t(0);
			for (size_t i = 0; i < frame->batches.size(); i++)
				if (frame->batches[i].fbp == ns2_stress_batch.fbp && frame->batches[i].fbw == ns2_stress_batch.fbw &&
				    frame->batches[i].depth_write && !frame->batches[i].vertices.empty())
					last_writer = i;
			if (last_writer == ~size_t(0)) return in;
			auto nat = ns2_stress_batch;
			auto m2 = std::make_shared<GSInterface::TakedownNativeMesh>(*nat.native->mesh);
			m2->key += vs % keys;
			auto d2 = std::make_shared<GSInterface::TakedownNativeDraw>(*nat.native);
			d2->mesh = m2; nat.native = d2;
			frame->batches.insert(frame->batches.begin() + ptrdiff_t(last_writer) + 1, nat);
			return frame;
		}
		auto frame = std::make_shared<GSInterface::TakedownSceneFrame>(*in);
		const char *at = getenv("NS2_INJECT_AT");
		const bool as_gs = getenv("NS2_INJECT_GS") != nullptr, drop_only = getenv("NS2_INJECT_DROP") != nullptr;
		std::vector<GSInterface_t> out;
		out.reserve(frame->batches.size() + 8);
		std::vector<GSInterface_t> late;
		for (size_t i = 0; i < frame->batches.size(); i++)
		{
			const auto &orig = in->batches[i];
			const uint32_t *tg = nullptr;
			for (uint32_t k = 0; k < ntargets; k++)
				if (h[7 + 3 * k] == i) tg = h + 7 + 3 * k;
			if (!tg) { out.push_back(orig); continue; }
			const uint32_t first = tg[1], count = tg[2];
			if (first + count > orig.vertices.size()) return in;
			GSInterface_t pre = orig, post = orig, nat = orig;
			pre.vertices.assign(orig.vertices.begin(), orig.vertices.begin() + first);
			post.vertices.assign(orig.vertices.begin() + first + count, orig.vertices.end());
			nat.vertices.clear(); nat.native = draw;
			fog_state(nat);
			if (as_gs) { nat.native = nullptr; nat.vertices.assign(orig.vertices.begin() + first, orig.vertices.begin() + first + count); }
			if (!pre.vertices.empty()) out.push_back(std::move(pre));
			if (!drop_only)
			{
				ns2_stress_batch = nat; ns2_have_stress = nat.native != nullptr;
				if (at && !strcmp(at, "late")) late.push_back(nat); else out.push_back(nat);
			}
			if (!post.vertices.empty()) out.push_back(std::move(post));
		}
		if (!late.empty())
		{
			size_t last_writer = 0;
			for (size_t i = 0; i < out.size(); i++)
				if (out[i].fbp == late[0].fbp && out[i].fbw == late[0].fbw && out[i].depth_write && !out[i].vertices.empty()) last_writer = i;
			out.insert(out.begin() + ptrdiff_t(last_writer) + 1, late.begin(), late.end());
		}
		if (getenv("NS2_ROUNDTRIP"))
		{
			// The camera of the frame is the file's view and projection: un-project the captured triangles.
			const auto md = [&](const float *m, int r, int c) { return double(m[c * 4 + r]); };
			double V[16], inv[16];
			for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) V[r * 4 + c] = md(draw->view, r, c);
			// invert the affine view (rows 0..2 = 3x3 A and translation t, row 3 = 0 0 0 1)
			const double a = V[0], b = V[1], c = V[2], d = V[4], e = V[5], f = V[6], g = V[8], hh = V[9], ii = V[10];
			const double det = a * (e * ii - f * hh) - b * (d * ii - f * g) + c * (d * hh - e * g);
			double A3[9] = { (e * ii - f * hh) / det, (c * hh - b * ii) / det, (b * f - c * e) / det,
			                 (f * g - d * ii) / det, (a * ii - c * g) / det, (c * d - a * f) / det,
			                 (d * hh - e * g) / det, (b * g - a * hh) / det, (a * e - b * d) / det };
			const double tv[3] = { V[3], V[7], V[11] };
			for (int r = 0; r < 3; r++)
			{
				for (int cc = 0; cc < 3; cc++) inv[r * 4 + cc] = A3[r * 3 + cc];
				inv[r * 4 + 3] = -(A3[r * 3] * tv[0] + A3[r * 3 + 1] * tv[1] + A3[r * 3 + 2] * tv[2]);
			}
			const bool keep_gs = getenv("NS2_ROUNDTRIP_GS") != nullptr;
			std::vector<GSInterface_t> result, moved;
			size_t converted = 0, tris = 0;
			const double kA = 1174.5, kB = 5032309.0;
			for (auto &b : out)
			{
				bool ok = !b.native && !b.vertices.empty() && b.fbw == 10 && !b.fog && !b.self_read &&
				          !b.exact && !b.depth_to_alpha && !b.invalid_blocks && b.vertices.size() % 3 == 0 && b.depth_test >= 2;
				if (ok) for (const auto &v : b.vertices)
				{
					const double z24 = double(v.z) * 16777216.0;
					const double zp = kB / (z24 - kA);
					if (z24 <= kA + 1.0 || v.q <= 0.0f || std::fabs(double(v.q) * zp - 1.0) > 0.02 || !(v.fog >= 0.999f)) { ok = false; break; }
				}
				if (!ok) { result.push_back(b); continue; }
				auto m3 = std::make_shared<GSInterface::TakedownNativeMesh>();
				uint64_t key = 1469598103934665603ull;
				for (const auto &v : b.vertices)
				{
					const double z24 = double(v.z) * 16777216.0, zp = kB / (z24 - kA);
					const double xp = double(v.x) * zp / 640.0, yp = double(v.y) * zp / 448.0;
					GSInterface::TakedownNativeVertex nv = {};
					for (int r = 0; r < 3; r++) nv.position[r] = float(inv[r * 4] * xp + inv[r * 4 + 1] * yp + inv[r * 4 + 2] * zp + inv[r * 4 + 3]);
					const float q = v.q;
					nv.u = b.textured ? v.s / q / b.sizes.x : 0.0f;
					nv.v = b.textured ? v.t / q / b.sizes.y : 0.0f;
					const float sc = b.textured ? 128.0f : 255.0f;
					const auto by = [](float x) { return uint32_t(std::min(255.0f, std::max(0.0f, std::floor(x + 0.5f)))); };
					nv.colour = by(v.r * sc) | by(v.g * sc) << 8 | by(v.b * sc) << 16 | by(v.a * 128.0f) << 24;
					m3->vertices.push_back(nv);
					for (size_t k = 0; k < sizeof nv; k++) key = (key ^ reinterpret_cast<const uint8_t *>(&nv)[k]) * 1099511628211ull;
				}
				m3->indices.resize(b.vertices.size());
				for (size_t k = 0; k < m3->indices.size(); k++) m3->indices[k] = uint32_t(k);
				m3->key = key;
				GSInterface_t nat = b;
				if (keep_gs) { moved.push_back(b); continue; }
				auto d3 = std::make_shared<GSInterface::TakedownNativeDraw>(*draw);
				d3->mesh = m3; d3->cull = false; d3->light_direction[3] = 0.0f; d3->fog_start = 0; d3->fog_scale = 0;
				nat.vertices.clear(); nat.native = d3;
				result.push_back(nat);
				converted++; tris += m3->indices.size() / 3;
			}
			if (keep_gs) { converted = moved.size(); for (auto &b : moved) tris += b.vertices.size() / 3; }
			LOGI("NS2 roundtrip: %zu batches, %zu triangles %s\n", converted, tris, keep_gs ? "kept as GS batches (moved to the end of their order)" : "drawn natively");
			out = std::move(result);
			if (keep_gs) out.insert(out.end(), moved.begin(), moved.end());
		}
		frame->batches = std::move(out);
		LOGI("NS2 inject: vsync %llu, %u target batch(es), native mesh of %u vertices, %u indices\n", (unsigned long long)vs, ntargets, nverts, nindices);
		return frame;
	}
	// ---- end NS2 probe ----

	// After a VSync's stream: what the native scene draws next.
	void collect_native_scene()
	{
		ns2h_log_vsync();
		native_job = {};
		if (!iface || !iface->takedown_scene_is_enabled())
			return;
		const bool hidden = input.native_scene_hidden;
		if (hidden != native_scene_hidden_logged)
		{
			native_scene_hidden_logged = hidden;
			LOGI("Native scene: %s (F10).\n", hidden ? "hidden, showing the GS picture" : "shown");
		}
		native_job.valid = true;
		native_job.vsync = parser.get_vsync_count();
		native_job.hidden = hidden;
		// Every published frame is drawn, in order, even while hidden: the mirrors
		// must see each draw (some effect buffers are drawn only once, at load).
		native_job.frames = iface->takedown_scene_take_published();
		for (auto &each : native_job.frames) each = ns2_inject(each, native_job.vsync);
		for (size_t fi = 0; fi < native_job.frames.size(); fi++) ns2_log_batches(*native_job.frames[fi], native_job.vsync, fi);
		native_job.displayed = iface->takedown_scene_displayed_frame();
		if (native_job.displayed) native_job.displayed = ns2_inject(native_job.displayed, native_job.vsync);
		if (!native_job.displayed || native_job.displayed->batches.empty())
			return;
		if (native_job.frames.empty() && native_scene_image)
			native_job.keep = true;
		else if (native_job.frames.empty())
			native_job.frames.push_back(native_job.displayed);
		uint32_t scale = std::max<uint32_t>(1u, presentation.resolution_scale);
		if (const char *value = getenv("TAKEDOWN_NATIVE_SCENE_SCALE"))
			scale = uint32_t(std::max(1, std::min(8, atoi(value))));
		native_job.scale = scale;
		if (!hidden && iface->takedown_ui_is_enabled())
			native_job.ui = iface->takedown_ui_displayed();
	}

	// When a frame begins: draws the collected frames on a worker.
	void kick_native_scene(Device &device)
	{
		if (!native_job.valid || native_job.keep || native_job.frames.empty())
			return;
		if (taa_wanted())
		{
			float jx = 0.0f, jy = 0.0f;
			TakedownRenderer::fsr_jitter(taa_index++, 448u * native_job.scale, std::max(fsr_picture_height, 448u * native_job.scale), jx, jy);
			native_scene.set_taa(true, jx, jy);
		}
		else
			native_scene.set_taa(false, 0.0f, 0.0f);
		native_task_image = nullptr;
		auto draw = [this, &device]() {
			auto cmd = device.request_command_buffer();
			const Vulkan::Image *image = nullptr;
			Vulkan::QueryPoolHandle ts0;
			if (gpu_timestamps) ts0 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
			for (const auto &each : native_job.frames)
				image = native_scene.render(device, *cmd, *each, native_job.scale);
			if (gpu_timestamps)
			{
				auto ts1 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
				if (rc_have_a && !rc_done) { std::lock_guard<std::mutex> l(rc_lock); rc_ts.emplace_back(ts0, ts1); }
				device.register_time_interval("Native scene", std::move(ts0), std::move(ts1), "scene");
			}
			device.submit(cmd);
			native_task_image = image;
		};
		static const bool inline_draw = env_enabled("TAKEDOWN_NATIVE_SCENE_INLINE");
		auto *group = GRANITE_THREAD_GROUP();
		if (!group || inline_draw)
		{
			draw();
			return;
		}
		native_task = group->create_task(std::move(draw));
		native_task->flush();
	}

	// After the frame's stream: the drawn picture.
	void finish_native_scene()
	{
		wait_native_task();
		auto job = std::move(native_job);
		native_job = {};
		if (!job.valid || !job.displayed || job.displayed->batches.empty())
		{
			native_scene_image = nullptr;
			native_ui_shown.clear();
			return;
		}
		taa_new_frame = !job.keep;
		if (!job.keep)
			native_scene_image = job.hidden ? nullptr : native_task_image;
		if (!job.keep)
			native_shown_cv = job.vsync;
		if (job.hidden)
		{
			native_scene_image = nullptr;
			return;
		}
		if (!job.keep)
			native_ui_shown = std::move(job.ui);
		if (input.native_scene_dump.exchange(false))
			log_native_scene(*job.displayed);
		native_scene_gain = job.displayed->gain;
		if (!native_scene_logged && native_scene_image)
		{
			native_scene_logged = true;
			LOGI("Native scene: first frame, %ux%u, %zu batches.\n", native_scene_image->get_width(),
			     native_scene_image->get_height(), job.displayed->batches.size());
		}
	}

	// The UI the picture is shown with: the native picture's own VSync's, else the latest.
	const std::vector<GSInterface::TakedownUiBatch> &shown_ui_batches() const
	{
		return native_scene_image ? native_ui_shown : iface->takedown_ui_displayed();
	}

	TakedownRenderer::SmaaFilter smaa;
	const Vulkan::Image *smaa_output = nullptr;

	void draw_world(CommandBuffer &cmd, const TakedownRenderer::PictureRect &rect)
	{
		// The GS has no picture while it is idle (the native scene draws).
		if (!vsync.image && !native_scene_image)
			return;
		// The native scene, when it has a frame, in place of the GS picture; with
		// FSR, its upscaled picture (gain already applied).
		// With SMAA, its picture (gain applied); with FSR, the upscaled picture.
		const Vulkan::Image &source = smaa_output ? *smaa_output
		                              : fsr_output ? *fsr_output
		                              : native_scene_image ? *native_scene_image : *vsync.image;
		const vec4 source_gain =
		    smaa_output || fsr_output ? vec4(1.0f) : native_scene_image ? native_scene_gain : vec4(1.0f);

		const uint32_t source_w = source.get_width();
		const uint32_t source_h = source.get_height();
		const uint32_t output_w = uint32_t(rect.width + 0.5f);
		const uint32_t output_h = uint32_t(rect.height + 0.5f);
		const uint32_t taps = smaa_output ? presentation.spatial_aa : 0;
		if (source_w != logged_source_w || source_h != logged_source_h || output_w != logged_output_w ||
		    output_h != logged_output_h || taps != logged_taps)
		{
			logged_source_w = source_w;
			logged_source_h = source_h;
			logged_output_w = output_w;
			logged_output_h = output_h;
			logged_taps = taps;
			LOGI("Native world: source %ux%u, output %ux%u, %s.\n", source_w, source_h, output_w, output_h,
			     taps == 0 ? "no SMAA"
			     : TakedownRenderer::spatial_aa_smaa_mode(uint8_t(taps)) == TakedownRenderer::SmaaMode::X2
			         ? "SMAA x2 (on the samples)"
			         : "SMAA x1");
		}

		VkViewport viewport = {};
		viewport.x = rect.x;
		viewport.y = rect.y;
		viewport.width = rect.width;
		viewport.height = rect.height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		const VkViewport saved = cmd.get_viewport();
		cmd.set_viewport(viewport);
		cmd.set_texture(0, 0, source.get_view(), StockSampler::LinearClamp);

		const vec4 gain = vec4(source_gain.xyz(), 1.0f);
		cmd.push_constants(&gain, 0, sizeof(gain));
		CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
		                                        "builtin://shaders/takedown_world_blit.frag");
		cmd.set_viewport(saved);
	}

	// Before the frame's render passes: the SMAA picture for draw_world (not
	// with FSR, which anti-aliases itself). x1 works at the window's size, or
	// at the render size when that is smaller; x2 on the render-scale picture's
	// samples before draw_world combines them.
	void prepare_smaa(Device &device, CommandBuffer &cmd, const TakedownRenderer::PictureRect &picture)
	{
		smaa_output = nullptr;
		const auto mode = TakedownRenderer::spatial_aa_smaa_mode(presentation.spatial_aa);
		if (mode == TakedownRenderer::SmaaMode::Off || fsr_output || (!vsync.image && !native_scene_image) ||
		    picture.width < 1.0f || picture.height < 1.0f)
			return;
		const Vulkan::Image &source = native_scene_image ? *native_scene_image : *vsync.image;
		const vec4 gain = native_scene_image ? native_scene_gain : vec4(1.0f);
		uint32_t width = source.get_width();
		uint32_t height = source.get_height();
		const uint32_t output_w = uint32_t(picture.width + 0.5f);
		const uint32_t output_h = uint32_t(picture.height + 0.5f);
		if (mode == TakedownRenderer::SmaaMode::X1 && (width > output_w || height > output_h))
		{
			width = output_w;
			height = output_h;
		}
		smaa_output = smaa.run(device, cmd, source, gain, width, height);
	}

	// Draws the world and the game's UI layer into ui_target, at the picture's
	// size in window pixels. With High Resolution UI the UI is drawn there too;
	// otherwise it is drawn at the game's own 448 lines into ui_low_target (a
	// transparent layer, premultiplied) and composited over the world, so its
	// resolution does not follow the render scale. Floating point, so the GS
	// blend factors above 1.0 are kept, and blending happens on the stored values
	// as on the GS.
	static ImageHandle &ensure_target(Device &device, ImageHandle &image, uint32_t width, uint32_t height,
	                                  const char *name)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);
		if (!image || image->get_width() != width || image->get_height() != height)
		{
			auto info = ImageCreateInfo::render_target(width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
			info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			image = device.create_image(info);
			device.set_name(*image, name);
		}
		return image;
	}

	static void begin_target(CommandBuffer &cmd, Image &image, float alpha)
	{
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
		RenderPassInfo rp;
		rp.num_color_attachments = 1;
		rp.color_attachments[0] = &image.get_view();
		rp.clear_attachments = 1;
		rp.store_attachments = 1;
		rp.clear_color[0].float32[0] = 0.0f;
		rp.clear_color[0].float32[1] = 0.0f;
		rp.clear_color[0].float32[2] = 0.0f;
		rp.clear_color[0].float32[3] = alpha;
		cmd.begin_render_pass(rp);
	}

	static void end_target(CommandBuffer &cmd, Image &image)
	{
		cmd.end_render_pass();
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}

	void compose_ui_target(Device &device, CommandBuffer &cmd, const TakedownRenderer::PictureRect &picture,
	                       const std::vector<TakedownRenderer::UiPlacement> &placements)
	{
		if ((!vsync.image && !native_scene_image) || picture.width < 1.0f || picture.height < 1.0f)
		{
			ui_target.reset();
			return;
		}
		const auto &batches = shown_ui_batches();
		const bool high_resolution = presentation.high_resolution_ui;
		ui_layer.prepare_video_fill(device, cmd, batches, ui_layout, placements);
		if (!high_resolution && !batches.empty())
		{
			const uint32_t low_height = uint32_t(TakedownRenderer::kUiPictureHeight);
			const uint32_t low_width = uint32_t(float(low_height) * picture.width / picture.height + 0.5f);
			auto &low = ensure_target(device, ui_low_target, low_width, low_height, "Takedown UI layer");
			begin_target(cmd, *low, 0.0f);
			VkViewport viewport = {};
			viewport.width = float(low->get_width());
			viewport.height = float(low->get_height());
			viewport.maxDepth = 1.0f;
			const auto r0 = std::chrono::steady_clock::now();
			ui_layer.render(device, cmd, batches, ui_layout, placements, viewport, true);
			ui_render_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
			end_target(cmd, *low);
		}

		auto &target = ensure_target(device, ui_target, uint32_t(picture.width + 0.5f),
		                             uint32_t(picture.height + 0.5f), "Takedown UI target");
		begin_target(cmd, *target, 1.0f);
		const TakedownRenderer::PictureRect whole = { 0.0f, 0.0f, float(target->get_width()),
		                                              float(target->get_height()) };
		draw_world(cmd, whole);
		VkViewport viewport = {};
		viewport.width = whole.width;
		viewport.height = whole.height;
		viewport.maxDepth = 1.0f;
		if (high_resolution)
			ui_layer.render(device, cmd, batches, ui_layout, placements, viewport, false);
		else if (!batches.empty() && ui_low_target)
		{
			cmd.set_viewport(viewport);
			CommandBufferUtil::setup_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                         "builtin://shaders/takedown_world_blit.frag", {});
			cmd.set_blend_enable(true);
			cmd.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			cmd.set_blend_op(VK_BLEND_OP_ADD);
			cmd.set_texture(0, 0, ui_low_target->get_view(), StockSampler::LinearClamp);
			const vec4 gain(1.0f);
			cmd.push_constants(&gain, 0, sizeof(gain));
			CommandBufferUtil::draw_fullscreen_quad(cmd);
			cmd.set_blend_enable(false);
		}
		end_target(cmd, *target);
	}

	void draw_ui_target(CommandBuffer &cmd, const TakedownRenderer::PictureRect &rect)
	{
		VkViewport viewport = {};
		viewport.x = rect.x;
		viewport.y = rect.y;
		viewport.width = rect.width;
		viewport.height = rect.height;
		viewport.maxDepth = 1.0f;
		const VkViewport saved = cmd.get_viewport();
		cmd.set_viewport(viewport);
		cmd.set_texture(0, 0, ui_target->get_view(), StockSampler::LinearClamp);
		const vec4 gain(1.0f);
		cmd.push_constants(&gain, 0, sizeof(gain));
		CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
		                                        "builtin://shaders/takedown_world_blit.frag");
		cmd.set_viewport(saved);
	}

	static vec4 overlay_color(td::ParallelGsOverlayStyle style)
	{
		switch (style)
		{
		case td::ParallelGsOverlayStyle::Title: return vec4(1.0f, 0.78f, 0.25f, 1.0f);
		case td::ParallelGsOverlayStyle::Focused: return vec4(1.0f, 1.0f, 1.0f, 1.0f);
		case td::ParallelGsOverlayStyle::Unavailable: return vec4(0.5f, 0.5f, 0.5f, 1.0f);
		case td::ParallelGsOverlayStyle::Hint: return vec4(0.7f, 0.7f, 0.7f, 1.0f);
		default: return vec4(0.85f, 0.85f, 0.85f, 1.0f);
		}
	}

	// The options menu as a centered panel of text lines.
	void render_host_overlay(float viewport_width, float viewport_height)
	{
		if (presentation.overlay.empty())
			return;
		const float line_height = 28.0f;
		const float width = std::min(viewport_width - 40.0f, 720.0f);
		const float height = line_height * float(presentation.overlay.size()) + 24.0f;
		vec2 origin((viewport_width - width) * 0.5f, std::max(10.0f, (viewport_height - height) * 0.5f));
		flat_renderer.render_quad(vec3(origin, 0.2f), vec2(width, height), vec4(0.0f, 0.0f, 0.0f, 0.82f));
		vec2 at = origin + vec2(16.0f, 12.0f);
		for (const auto &line : presentation.overlay)
		{
			const bool focused = line.style == td::ParallelGsOverlayStyle::Focused;
			if (focused)
				flat_renderer.render_quad(vec3(at - vec2(8.0f, 0.0f), 0.15f), vec2(width - 16.0f, line_height),
				                          vec4(0.25f, 0.35f, 0.6f, 0.9f));
			auto &font = GRANITE_UI_MANAGER()->get_font(line.style == td::ParallelGsOverlayStyle::Title ?
			                                            UI::FontSize::Large : UI::FontSize::Normal);
			flat_renderer.render_text(font, line.text.c_str(), vec3(at, 0.1f), vec2(width - 32.0f, line_height),
			                          overlay_color(line.style), Font::Alignment::CenterLeft);
			at.y += line_height;
		}
	}

	// The title screen's large BURNOUT 3 TAKEDOWN logo (a hash of its texture
	// data, GSInterface::TakedownUiBatch::texture_key): only that screen draws it.
	static constexpr uint64_t kTitleScreenLogo = 0x125bcea4ab2fedacull;
	// The race HUD's panels and the menus' blue arc (ui_placements.inc).
	static constexpr uint64_t kRaceHudPanel = 0x3f7903c13f585625ull;
	static constexpr uint64_t kMenuArc = 0x42884cff5f0c9ce6ull;

	TakedownRenderer::DiscordPresence discord;
	std::chrono::steady_clock::time_point discord_checked;

	// What the player is doing, for Discord, from the screen being drawn.
	void update_discord()
	{
		if (!discord.enabled() || !iface)
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now - discord_checked < std::chrono::seconds(1))
			return;
		discord_checked = now;
		const char *details = "Starting up";
		if (iface->takedown_ui_watched_seen(kRaceHudPanel, 120))
			details = "Racing";
		else if (iface->takedown_ui_watched_seen(kTitleScreenLogo, 120))
			details = "On the title screen";
		else if (iface->takedown_ui_watched_seen(kMenuArc, 120))
			details = "In the menus";
		discord.set(details, "Version " TAKEDOWN_RENDERER_VERSION);
	}

	// The release's version in the bottom-left corner of the title screen.
	void render_title_version(const TakedownRenderer::PictureRect &picture)
	{
		if (!iface || !iface->takedown_ui_watched_seen(kTitleScreenLogo, 4))
			return;
		// In the footer strip, on the copyright line's baseline (y 425 of the
		// game's 448), near the picture's left edge.
		const float line = std::max(14.0f, picture.height * 0.05f);
		const float bottom = picture.y + picture.height * (425.0f / 448.0f);
		const vec3 at(picture.x + picture.height * 0.045f, bottom - line, 0.0f);
		const auto font = picture.height >= 1000.0f ? UI::FontSize::Large :
		                  picture.height >= 600.0f ? UI::FontSize::Normal : UI::FontSize::Small;
		auto &text_font = GRANITE_UI_MANAGER()->get_font(font);
		const vec2 size(picture.width * 0.3f, line);
		flat_renderer.render_text(text_font, TAKEDOWN_RENDERER_VERSION, at + vec3(1.0f, 1.0f, 0.01f), size,
		                          vec4(0.0f, 0.0f, 0.0f, 0.6f), Font::Alignment::BottomLeft);
		flat_renderer.render_text(text_font, TAKEDOWN_RENDERER_VERSION, at, size, vec4(0.85f, 0.88f, 0.95f, 0.85f),
		                          Font::Alignment::BottomLeft);
	}

	void render_fps_counter(float viewport_width)
	{
		if (!presentation.fps_counter)
			return;

		char label[64];
		snprintf(label, sizeof(label), "%.0f FPS   5s avg %.0f", double(fps), double(average_fps(5.0)));
		const vec2 size(std::min(210.0f, std::max(120.0f, viewport_width - 12.0f)), 78.0f);
		const vec2 at(6.0f, 6.0f);
		flat_renderer.render_quad(vec3(at, 0.05f), size, vec4(0.0f, 0.0f, 0.0f, 0.6f));
		flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(UI::FontSize::Small), label,
		                          vec3(at + vec2(6.0f, 4.0f), 0.0f), vec2(size.x - 12.0f, 20.0f), vec4(1.0f),
		                          Font::Alignment::TopLeft);

		// Graph of the last kFpsGraphSeconds, oldest on the left, positioned by
		// sample age so a young history fills in from the right. A peak of at
		// least 60 FPS keeps the vertical scale stable at the game's normal rate.
		const float graph_left = at.x + 6.0f;
		const float graph_top = at.y + 28.0f;
		const float graph_width = size.x - 12.0f;
		const float graph_height = size.y - 36.0f;

		const auto now = std::chrono::steady_clock::now();
		std::array<vec2, kFpsHistory> points;
		unsigned count = 0;
		float peak = 60.0f;
		for (size_t i = fps_history_count; i > 0; i--)
		{
			const auto &sample = fps_history[(fps_history_head + kFpsHistory - i) % kFpsHistory];
			if (std::chrono::duration<double>(now - sample.time).count() <= kFpsGraphSeconds)
				peak = std::max(peak, sample.fps);
		}
		peak = std::ceil(peak / 30.0f) * 30.0f;

		for (size_t i = fps_history_count; i > 0; i--)
		{
			const auto &sample = fps_history[(fps_history_head + kFpsHistory - i) % kFpsHistory];
			const double age = std::chrono::duration<double>(now - sample.time).count();
			if (age > kFpsGraphSeconds)
				continue;
			const float t = float(1.0 - age / kFpsGraphSeconds);
			const float level = std::min(sample.fps / peak, 1.0f);
			points[count++] = vec2(graph_left + t * graph_width, graph_top + graph_height - level * graph_height);
		}
		if (count < 2)
			return;
		// Alpha below one keeps the strip in the transparent queue so it draws
		// over the panel quad.
		flat_renderer.render_line_strip(points.data(), 0.02f, count, vec4(0.3f, 1.0f, 0.4f, 0.99f));
	}

	float average_fps(double seconds) const
	{
		const auto now = std::chrono::steady_clock::now();
		double total = 0.0;
		unsigned count = 0;
		for (size_t i = 0; i < fps_history_count; i++)
		{
			const auto &sample = fps_history[(fps_history_head + kFpsHistory - 1 - i) % kFpsHistory];
			if (std::chrono::duration<double>(now - sample.time).count() > seconds)
				continue;
			total += double(sample.fps);
			count++;
		}
		return count ? float(total / double(count)) : fps;
	}

	void update_fps()
	{
		fps_window_frames++;
		const auto now = std::chrono::steady_clock::now();
		const double elapsed = std::chrono::duration<double>(now - fps_window_start).count();
		if (elapsed >= 0.5)
		{
			fps = float(double(fps_window_frames) / elapsed);
			fps_window_frames = 0;
			fps_window_start = now;
		}

		fps_sample_frames++;
		const double sample_elapsed = std::chrono::duration<double>(now - fps_sample_start).count();
		if (sample_elapsed >= kFpsSampleSeconds)
		{
			fps_history[fps_history_head] = { float(double(fps_sample_frames) / sample_elapsed), now };
			fps_history_head = (fps_history_head + 1) % kFpsHistory;
			if (fps_history_count < kFpsHistory)
				fps_history_count++;
			fps_sample_frames = 0;
			fps_sample_start = now;
		}
	}

	struct RcSnap { uint64_t drawn = 0, calls = 0; std::map<int, std::pair<std::string, double>> t; std::chrono::steady_clock::time_point wall; double gpu = 0; uint64_t vs = 0; };
	RcSnap rc_snap(uint64_t vs)
	{
		RcSnap r; r.wall = std::chrono::steady_clock::now(); r.vs = vs; r.drawn = frames_drawn; r.calls = rc_calls;
#ifndef _WIN32
		const long hz = sysconf(_SC_CLK_TCK);
		if (DIR *d = opendir("/proc/self/task"))
		{
			while (auto *de = readdir(d))
			{
				int tid = atoi(de->d_name);
				if (!tid) continue;
				char path[128]; snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
				FILE *f = fopen(path, "r"); if (!f) continue;
				char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
				char *lp = strchr(buf, '('); char *rp = strrchr(buf, ')'); if (!lp || !rp) continue;
				std::string comm(lp + 1, rp - lp - 1);
				unsigned long ut = 0, st = 0;
				sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
				r.t[tid] = { comm, double(ut + st) / double(hz) };
			}
			closedir(d);
		}
#endif
		if (iface)
			for (int t = 0; t < int(TimestampType::Count); t++)
				r.gpu += iface->get_accumulated_timestamps(TimestampType(t));
		return r;
	}
	uint64_t rc_calls = 0; std::mutex rc_lock; std::vector<std::pair<Vulkan::QueryPoolHandle, Vulkan::QueryPoolHandle>> rc_ts;
	RcSnap rc_a; bool rc_have_a = false, rc_done = false;
	void rc_probe()
	{
		static const uint64_t A = getenv("RC_A") ? strtoull(getenv("RC_A"), nullptr, 10) : ~0ull;
		static const uint64_t B = getenv("RC_B") ? strtoull(getenv("RC_B"), nullptr, 10) : ~0ull;
		rc_calls++;
		const uint64_t vs = parser.get_vsync_count();
		if (!rc_have_a && vs >= A) { rc_a = rc_snap(vs); rc_have_a = true; ns3_report("window-start"); }
		if (rc_have_a && !rc_done && vs >= B)
		{
			RcSnap b = rc_snap(vs); rc_done = true;
			wait_native_task();
			get_wsi().get_device().wait_idle();
			{ double sum = 0; size_t n = 0; std::lock_guard<std::mutex> l(rc_lock);
			  for (auto &pr : rc_ts) if (pr.first && pr.second && pr.first->is_signalled() && pr.second->is_signalled()) { sum += get_wsi().get_device().convert_device_timestamp_delta(pr.first->get_timestamp_ticks(), pr.second->get_timestamp_ticks()); n++; }
			  printf("RCSCENEGPU draws %zu mean_ms %.3f per_vsync_ms %.3f\n", n, n ? 1000.0 * sum / n : 0.0, 1000.0 * sum / double(b.vs - rc_a.vs)); }
			const double frames = double(b.vs - rc_a.vs);
			const double wall = std::chrono::duration<double>(b.wall - rc_a.wall).count();
			double total = 0;
			for (auto &kv : b.t)
			{
				double a0 = rc_a.t.count(kv.first) ? rc_a.t[kv.first].second : 0.0;
				double d = kv.second.second - a0; total += d;
				if (d > 0.0) printf("RCTHREAD %-16s tid %d cpu_ms_per_frame %.3f\n", kv.second.first.c_str(), kv.first, 1000.0 * d / frames);
			}
			printf("RCDRAWN %llu of %.0f vsyncs, %llu render_frame calls\n", (unsigned long long)(b.drawn - rc_a.drawn), frames, (unsigned long long)(b.calls - rc_a.calls));
			printf("RCTOTAL frames %.0f wall_s %.2f wall_ms_per_frame %.3f cpu_ms_per_frame %.3f gpu_ms_per_frame %.3f\n",
			       frames, wall, 1000.0 * wall / frames, 1000.0 * total / frames, 1000.0 * (b.gpu - rc_a.gpu) / frames);
			fflush(stdout);
			ns3_report("window-end");
			request_shutdown();
		}
	}


	// ---- RD hash probe (r-d): RD_HASH_FILE=<out>, RD_PIC=a-b/step[,a-b/step] ----
	static uint64_t rd_fnv(uint64_t h, const void *p, size_t n)
	{
		const uint8_t *b = static_cast<const uint8_t *>(p);
		for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
		return h;
	}
	template <typename T> static uint64_t rd_fnv_t(uint64_t h, const T &v) { return rd_fnv(h, &v, sizeof(T)); }
	FILE *rd_file = nullptr; bool rd_init = false;
	std::vector<std::array<uint64_t, 3>> rd_ranges;
	void rd_setup()
	{
		if (rd_init) return;
		rd_init = true;
		if (const char *f = getenv("RD_HASH_FILE")) rd_file = fopen(f, "w");
		if (const char *e = getenv("RD_PIC"))
		{
			std::string s = e; size_t pos = 0;
			while (pos < s.size())
			{
				size_t c = s.find(',', pos); if (c == std::string::npos) c = s.size();
				unsigned long long a = 0, b = 0, st = 1;
				if (sscanf(s.substr(pos, c - pos).c_str(), "%llu-%llu/%llu", &a, &b, &st) >= 2) rd_ranges.push_back({ a, b, st ? st : 1 });
				pos = c + 1;
			}
		}
	}
	bool rd_pic_wanted(uint64_t vs)
	{
		for (auto &r : rd_ranges) if (vs >= r[0] && vs <= r[1] && (vs - r[0]) % r[2] == 0) return true;
		return false;
	}
	uint64_t rd_hash_image(Device &device, Image &image, VkImageLayout layout)
	{
		const uint32_t w = image.get_width(), h = image.get_height();
		const uint32_t texel = TextureFormatLayout::format_block_size(image.get_format(), VK_IMAGE_ASPECT_COLOR_BIT);
		BufferCreateInfo info = {};
		info.domain = BufferDomain::CachedHost;
		info.size = VkDeviceSize(w) * h * texel;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		auto buffer = device.create_buffer(info);
		auto cmd = device.request_command_buffer();
		cmd->image_barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd->copy_image_to_buffer(*buffer, image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
		cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
		                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		Fence fence;
		device.submit(cmd, &fence);
		fence->wait();
		auto *mapped = static_cast<uint8_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
		uint64_t hh = rd_fnv(0xcbf29ce484222325ull, mapped, size_t(w) * h * texel);
		if (const char *dir = getenv("RD_RAW_DIR"))
		{
			static const uint64_t ra = getenv("RD_RAW_A") ? strtoull(getenv("RD_RAW_A"), nullptr, 10) : 0, rb = getenv("RD_RAW_B") ? strtoull(getenv("RD_RAW_B"), nullptr, 10) : 0;
			const uint64_t vs = parser.get_vsync_count();
			if (vs >= ra && vs <= rb)
			{
				char path[512]; snprintf(path, sizeof path, "%s/%llu_%ux%u_%u.raw", dir, (unsigned long long)vs, w, h, texel);
				if (FILE *f = fopen(path, "wb")) { fwrite(mapped, 1, size_t(w) * h * texel, f); fclose(f); }
			}
		}
		device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
		return hh;
	}
	void rd_hash_pictures(Device &device)
	{
		rd_setup();
		if (!rd_file) return;
		const uint64_t vs = parser.get_vsync_count();
		if (!rd_pic_wanted(vs)) return;
		if (native_scene_image)
			fprintf(rd_file, "P %llu scene %016llx\n", (unsigned long long)vs,
			        (unsigned long long)rd_hash_image(device, const_cast<Image &>(*native_scene_image), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		else
			fprintf(rd_file, "P %llu scene none\n", (unsigned long long)vs);
		if (vsync.image)
			fprintf(rd_file, "P %llu gs %016llx\n", (unsigned long long)vs,
			        (unsigned long long)rd_hash_image(device, const_cast<Image &>(*vsync.image), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL));
		if (ui_target)
			fprintf(rd_file, "P %llu ui %016llx\n", (unsigned long long)vs,
			        (unsigned long long)rd_hash_image(device, *ui_target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		fflush(rd_file);
	}
	// ---- end RD probe ----

	// ---- R-E probe: hash / dump the picture of chosen content vsyncs ----
	struct ReRange { uint64_t a, b, step; };
	std::vector<ReRange> re_ranges; std::string re_dir; bool re_init = false; uint64_t re_max = 0; uint64_t re_last_cv = ~0ull;
	unsigned re_pres_every = 4; unsigned re_count = 0; FILE *re_log = nullptr;
	void re_setup()
	{
		re_init = true;
		const char *d = getenv("RE_DIR"), *set = getenv("RE_SET");
		if (!d || !set) return;
		re_dir = d;
		if (const char *e = getenv("RE_PRES_EVERY")) re_pres_every = unsigned(atoi(e));
		std::string t = set; size_t pos = 0;
		while (pos < t.size())
		{
			size_t e = t.find(',', pos); if (e == std::string::npos) e = t.size();
			unsigned long long a = 0, b = 0, st = 1;
			if (sscanf(t.substr(pos, e - pos).c_str(), "%llu:%llu:%llu", &a, &b, &st) >= 2) { re_ranges.push_back({ a, b, st ? st : 1 }); re_max = std::max<uint64_t>(re_max, b); }
			pos = e + 1;
		}
		std::string lp = re_dir + "/hashes.txt"; re_log = fopen(lp.c_str(), "w");
	}
	bool re_wanted(uint64_t cv) const
	{
		for (auto &r : re_ranges) if (cv >= r.a && cv <= r.b && ((cv - r.a) % r.step) == 0) return true;
		return false;
	}
	static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
	BufferHandle re_pres; bool re_pres_pending = false;
	void re_record_present(Device &device, CommandBuffer &cmd, uint64_t)
	{
		auto &image = const_cast<Image &>(device.get_swapchain_view().get_image());
		const uint32_t w = image.get_width(), h = image.get_height();
		BufferCreateInfo info = {};
		info.domain = BufferDomain::CachedHost;
		info.size = VkDeviceSize(w) * h * 4;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		re_pres = device.create_buffer(info); re_pres_pending = true;
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd.copy_image_to_buffer(*re_pres, image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
		cmd.image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		                  VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
		cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	}
	void re_after_submit(Device &device, uint64_t cv)
	{
		device.wait_idle();
		const bool on = native_scene_image != nullptr;
		const Image &image = on ? *native_scene_image : *vsync.image;
		const VkImageLayout layout = on ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
		const uint32_t w = image.get_width(), h = image.get_height();
		const size_t texel = on ? 8 : 4;
		BufferCreateInfo info = {};
		info.domain = BufferDomain::CachedHost; info.size = VkDeviceSize(w) * h * texel; info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		auto buffer = device.create_buffer(info);
		auto cmd = device.request_command_buffer();
		auto &mimg = const_cast<Image &>(image);
		cmd->image_barrier(mimg, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		cmd->copy_image_to_buffer(*buffer, mimg, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
		cmd->image_barrier(mimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		Fence fence; device.submit(cmd, &fence); fence->wait();
		auto *m = static_cast<uint8_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
		const uint64_t hs = fnv(m, size_t(w) * h * texel);
		char path[1024];
		snprintf(path, sizeof(path), "%s/src-%06llu-%ux%u-f%d.raw", re_dir.c_str(), (unsigned long long)cv, w, h, int(image.get_format()));
		if (FILE *f = fopen(path, "wb")) { fwrite(m, 1, size_t(w) * h * texel, f); fclose(f); }
		device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
		uint64_t hp = 0; uint32_t pw = 0, ph = 0; int pf = 0;
		if (re_pres_pending && re_pres)
		{
			auto &sc = device.get_swapchain_view().get_image();
			pw = sc.get_width(); ph = sc.get_height(); pf = int(sc.get_format());
			auto *pp = static_cast<uint8_t *>(device.map_host_buffer(*re_pres, MEMORY_ACCESS_READ_BIT));
			hp = fnv(pp, size_t(pw) * ph * 4);
			if (re_pres_every && (re_count % re_pres_every) == 0)
			{
				snprintf(path, sizeof(path), "%s/pres-%06llu-%ux%u-f%d.raw", re_dir.c_str(), (unsigned long long)cv, pw, ph, pf);
				if (FILE *f = fopen(path, "wb")) { fwrite(pp, 1, size_t(pw) * ph * 4, f); fclose(f); }
			}
			device.unmap_host_buffer(*re_pres, MEMORY_ACCESS_READ_BIT);
			re_pres.reset(); re_pres_pending = false;
		}
		re_dump_mirrors(device, cv);
		re_count++;
		fprintf(re_log, "REHASH cv=%llu vs=%llu on=%d src=%016llx %ux%u f%d gain=%.6f,%.6f,%.6f,%.6f pres=%016llx %ux%u f%d\n", (unsigned long long)cv,
		        (unsigned long long)parser.get_vsync_count(), on ? 1 : 0, (unsigned long long)hs, w, h, int(image.get_format()),
		        double(native_scene_gain.x), double(native_scene_gain.y), double(native_scene_gain.z), double(native_scene_gain.w),
		        (unsigned long long)hp, pw, ph, pf);
		fflush(re_log);
	}
	// RE_MIRRORS=<substring list> (e.g. "363_4,350_3"): also dumps the native scene's mirrors whose "block_width" name contains one of them.
	void re_dump_mirrors(Device &device, uint64_t cv)
	{
		static const char *want = getenv("RE_MIRRORS");
		if (!want) return;
		for (const auto &entry : native_scene.mirror_images())
		{
			bool wanted = false;
			std::string list = want;
			size_t pos = 0;
			while (pos < list.size())
			{
				size_t e = list.find(',', pos); if (e == std::string::npos) e = list.size();
				if (entry.first == list.substr(pos, e - pos)) wanted = true;
				pos = e + 1;
			}
			if (!wanted) continue;
			const Image &image = *entry.second;
			const uint32_t w = image.get_width(), h = image.get_height();
			BufferCreateInfo info = {};
			info.domain = BufferDomain::CachedHost; info.size = VkDeviceSize(w) * h * 8; info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			auto buffer = device.create_buffer(info);
			auto cmd = device.request_command_buffer();
			auto &mimg = const_cast<Image &>(image);
			cmd->image_barrier(mimg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			cmd->copy_image_to_buffer(*buffer, mimg, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
			cmd->image_barrier(mimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
			Fence fence; device.submit(cmd, &fence); fence->wait();
			auto *m = static_cast<uint8_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
			char path[1024];
			snprintf(path, sizeof(path), "%s/mir-%06llu-%s-%ux%u.raw", re_dir.c_str(), (unsigned long long)cv, entry.first.c_str(), w, h);
			if (FILE *f = fopen(path, "wb")) { fwrite(m, 1, size_t(w) * h * 8, f); fclose(f); }
			device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
		}
	}
	uint64_t re_current_cv() const { return native_scene_image ? native_shown_cv : parser.get_vsync_count(); }

	void render_frame(double, double) override
	{
		// Time spent in here, for the frame stats line.
		struct Timer
		{
			double &total;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			~Timer() { total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
		} timer{ render_seconds };
		if (!iface)
		{
			request_shutdown();
			return;
		}
		auto &wsi = get_wsi();
		auto &device = wsi.get_device();

		if (quit_after.count() > 0 && !close_pending && std::chrono::steady_clock::now() - started > quit_after)
		{
			LOGI("Dev: sending Quit after %d s.\n", int(quit_after.count()));
			std::lock_guard<std::mutex> hold{ input.lock };
			if (input.writer)
			{
				input.writer->request_quit();
				input.writer->flush();
			}
			input.close_requested = true;
		}
		if (input.close_requested && !close_pending)
		{
			close_pending = true;
			close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		}
		if (close_pending && std::chrono::steady_clock::now() > close_deadline)
		{
			LOGW("The runtime did not close the graphics pipe; exiting.\n");
			finish_stream();
			request_shutdown();
			return;
		}

		if (stats_enabled)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - last_heartbeat >= std::chrono::seconds(1))
			{
				last_heartbeat = now;
				LOGI("Heartbeat: raw vsyncs %llu, frames drawn %llu.\n",
				     static_cast<unsigned long long>(parser.get_vsync_count()),
				     static_cast<unsigned long long>(frames_drawn));
			}
		}

		{
			const auto now = std::chrono::steady_clock::now();
			if (stats_window_start.time_since_epoch().count() == 0)
				stats_window_start = now;
			const double elapsed = std::chrono::duration<double>(now - stats_window_start).count();
			if (elapsed >= 5.0)
			{
				LOGI("Frame stats: %.1f FPS over %.1f s, %.1f%% of the time waiting for the game, "
				     "%.2f ms of renderer work per frame.\n",
				     double(stats_window_frames) / elapsed, elapsed, 100.0 * stream_wait_seconds / elapsed,
				     stats_window_frames ? 1000.0 * (render_seconds - stream_wait_seconds) / stats_window_frames : 0.0);
				render_seconds = 0.0;
				if (iface && iface->takedown_ui_is_enabled() && stats_window_frames)
				{
					LOGI("UI layer CPU: layout %.2f ms, placement %.2f ms, drawing %.2f ms per frame; "
					     "%.0f batches, %.0f draws, %.0f triangles per frame; layer draws %.2f ms.\n",
					     1000.0 * ui_seconds[0] / stats_window_frames, 1000.0 * ui_seconds[1] / stats_window_frames,
					     1000.0 * ui_seconds[2] / stats_window_frames,
					     double(ui_layer.counters.batches) / stats_window_frames,
					     double(ui_layer.counters.draws) / stats_window_frames,
					     double(ui_layer.counters.triangles) / stats_window_frames,
					     1000.0 * ui_render_seconds / stats_window_frames);
					const auto &c = ui_layer.counters;
					LOGI("UI layer draw: setup %.2f, state %.2f, texture %.2f, vertices %.2f, draw %.2f ms per frame; "
					     "%llu batches from GS textures.\n",
					     1000.0 * c.setup / stats_window_frames, 1000.0 * c.state / stats_window_frames,
					     1000.0 * c.texture / stats_window_frames, 1000.0 * c.vertices / stats_window_frames,
					     1000.0 * c.draw / stats_window_frames, static_cast<unsigned long long>(c.gs_textures));
					ui_layer.counters = {};
					ui_render_seconds = 0.0;
				}
				ui_seconds[0] = ui_seconds[1] = ui_seconds[2] = 0.0;
				if (gpu_timestamps && iface && stats_window_frames)
				{
					double total = 0.0;
					for (int t = 0; t < int(TimestampType::Count); t++)
						total += iface->get_accumulated_timestamps(TimestampType(t));
					LOGI("GPU: GS %.2f ms per frame.\n", 1000.0 * (total - gs_gpu_seconds_logged) / stats_window_frames);
					gs_gpu_seconds_logged = total;
					get_wsi().get_device().timestamp_log([](const std::string &tag, const TimestampIntervalReport &report) {
						LOGI("GPU: %s %.3f ms per frame, %.3f ms per accumulation (%.3f per frame context).\n", tag.c_str(), 1000.0 * report.time_per_frame_context, 1000.0 * report.time_per_accumulation, report.accumulations_per_frame_context);
					});
					get_wsi().get_device().timestamp_log_reset();
				}
				if (iface && iface->takedown_ui_is_enabled())
				{
					const auto ui = iface->takedown_ui_consume_stats();
					LOGI("UI layer: %llu primitives captured, %llu drawn in the game picture (unsupported state), "
					     "%llu frames published, %llu expired, %llu late world runs, %u batches shown.\n",
					     static_cast<unsigned long long>(ui.captured), static_cast<unsigned long long>(ui.rejected),
					     static_cast<unsigned long long>(ui.published), static_cast<unsigned long long>(ui.expired),
					     static_cast<unsigned long long>(ui.late_world),
					     unsigned(iface->takedown_ui_displayed().size()));
				}
				if (iface && iface->takedown_scene_is_enabled())
				{
					const auto scene = iface->takedown_scene_consume_stats();
					const auto &c = native_scene.counters;
					LOGI("Native scene: %llu primitives recorded, %llu feedback and %llu unsupported left to the GS; "
					     "%llu frames published, %llu expired; drew %llu frames, %llu batches, %llu triangles, "
					     "skipped %llu batches (blend); %.1f render passes, %.1f copies, %.1f mirror and %.1f GS "
					     "texture batches per frame; %.1f batches left to the GS (Z memory); %.1f depth-to-alpha per frame.\n",
					     static_cast<unsigned long long>(scene.captured), static_cast<unsigned long long>(scene.feedback),
					     static_cast<unsigned long long>(scene.unsupported), static_cast<unsigned long long>(scene.published),
					     static_cast<unsigned long long>(scene.expired), static_cast<unsigned long long>(c.frames),
					     static_cast<unsigned long long>(c.batches), static_cast<unsigned long long>(c.triangles),
					     static_cast<unsigned long long>(c.skipped_batches),
					     c.frames ? double(c.passes) / double(c.frames) : 0.0,
					     c.frames ? double(c.copies) / double(c.frames) : 0.0,
					     c.frames ? double(c.mirror_samples) / double(c.frames) : 0.0,
					     c.frames ? double(c.gs_samples) / double(c.frames) : 0.0,
					     c.frames ? double(c.gs_only_batches) / double(c.frames) : 0.0,
					     c.frames ? double(c.depth_to_alpha) / double(c.frames) : 0.0);
					LOGI("NS2 native: %llu draws, %llu triangles, %llu mesh uploads.\n", (unsigned long long)c.native_draws, (unsigned long long)c.native_triangles, (unsigned long long)c.native_mesh_uploads);
					native_scene.counters = {};
				}
				if (iface)
				{
					const auto panini = iface->takedown_panini_consume_stats();
					if (panini.records || panini.held_frames)
						LOGI("Panini: %llu frames, %llu records, %llu with a warp in force, %llu warped, "
						     "%llu not reached, %llu failed, longest gap between records %u frames.\n",
						     static_cast<unsigned long long>(panini.vsyncs),
						     static_cast<unsigned long long>(panini.records),
						     static_cast<unsigned long long>(panini.held_frames),
						     static_cast<unsigned long long>(panini.warps),
						     static_cast<unsigned long long>(panini.unfired_frames),
						     static_cast<unsigned long long>(panini.failed), panini.max_record_gap);
				}
				stats_window_start = now;
				stats_window_frames = 0;
				stream_wait_seconds = 0.0;
			}
		}

		rc_probe();
		kick_native_scene(get_wsi().get_device());
		bool new_frame = false;
		stage = 1; // reading the stream
		const auto read_start = std::chrono::steady_clock::now();
		if (!is_eof)
		{
			try
			{
				if (parser.iterate_until_vsync(gs_resolution_scale >= 2))
				{
					vsync = parser.consume_vsync_result();
					new_frame = true;
				}
				else
					is_eof = true;
			}
			catch (const std::exception &e)
			{
				LOGE("Graphics stream error: %s\n", e.what());
				is_eof = true;
			}

			stream_wait_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start).count();
			// NativeModel records are not replayed by this renderer yet; the
			// runtime falls back to ordinary GS draws when they are absent.
			parser.take_native_models();
			apply_presentation(false);
			if (parser.get_host_ui_serial() != host_ui_serial)
			{
				host_ui_serial = parser.get_host_ui_serial();
				if (!host_ui.set_record(parser.get_host_ui()))
					LOGE("Ignoring a malformed host UI record.\n");
				else if (env_enabled("TAKEDOWN_DEV_HOST_UI_LOG"))
				{
					td::ParallelGsHostUi ui;
					const auto &bytes = parser.get_host_ui();
					td::decode_parallel_gs_host_ui(bytes.data(), bytes.size(), ui);
					LOGI("Host UI record %llu: %u items.\n", static_cast<unsigned long long>(host_ui_serial),
					     unsigned(ui.items.size()));
					for (const auto &item : ui.items)
						LOGI("  kind %u flags %02x at %d,%d size %ux%u colour %08x corners %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f text \"%s\"\n",
						     unsigned(item.kind), unsigned(item.flags), item.x, item.y, unsigned(item.width),
						     unsigned(item.height), item.colour, double(item.corners[0]), double(item.corners[1]),
						     double(item.corners[2]), double(item.corners[3]), double(item.corners[4]),
						     double(item.corners[5]), double(item.corners[6]), double(item.corners[7]),
						     item.text.substr(0, 120).c_str());
				}
			}
		}

		finish_native_scene();
		collect_native_scene();

		stage = 2; // acquiring the swapchain / recording
		const auto &swapchain_image = device.get_swapchain_view().get_image();
		const float viewport_width = float(swapchain_image.get_width());
		const float viewport_height = float(swapchain_image.get_height());
		const auto picture = TakedownRenderer::fit_picture(viewport_width, viewport_height, presentation.widescreen);
		input.set_picture(picture, viewport_width, viewport_height);
		// FSR submits its own command buffers ahead of the frame's.
		run_fsr(device, picture);
		auto cmd = device.request_command_buffer();

		QueryPoolHandle present_start;
		if (gpu_timestamps)
			present_start = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		prepare_smaa(device, *cmd, picture);
		// The game's UI and the host UI are placed together for the picture's shape.
		const bool ui_layer_on = iface && iface->takedown_ui_is_enabled();
		std::vector<TakedownRenderer::UiPlacement> ui_placements, host_placements;
		if (ui_layer_on)
		{
			const auto &batches = shown_ui_batches();
			const auto t0 = std::chrono::steady_clock::now();
			ui_layout = ui_layer.layout(batches);
			const auto t1 = std::chrono::steady_clock::now();
			std::vector<TakedownRenderer::UiElement> elements = ui_layout.elements;
			const size_t game_elements = elements.size();
			const auto host_elements = host_ui.elements();
			elements.insert(elements.end(), host_elements.begin(), host_elements.end());
			std::vector<int> matched;
			auto placements = TakedownRenderer::place_ui(elements, picture.width / picture.height,
			                                             TakedownRenderer::builtin_ui_rules(),
			                                             dump_layout_next ? &matched : nullptr);
			const auto t2 = std::chrono::steady_clock::now();
			ui_placements.assign(placements.begin(), placements.begin() + ptrdiff_t(game_elements));
			host_placements.assign(placements.begin() + ptrdiff_t(game_elements), placements.end());
			compose_ui_target(device, *cmd, picture, ui_placements);
			const auto t3 = std::chrono::steady_clock::now();
			ui_seconds[0] += std::chrono::duration<double>(t1 - t0).count();
			ui_seconds[1] += std::chrono::duration<double>(t2 - t1).count();
			ui_seconds[2] += std::chrono::duration<double>(t3 - t2).count();
			if (dump_layout_next)
			{
				dump_layout_next = false;
				// One line per item: the fields a placement rule matches on.
				const auto &rules = TakedownRenderer::builtin_ui_rules();
				LOGI("UI layout: vsync %llu, %zu items\n", static_cast<unsigned long long>(parser.get_vsync_count()),
				     elements.size());
				for (size_t i = 0; i < elements.size(); i++)
				{
					const auto &e = elements[i];
					const int rule = matched[i];
					LOGI("UI item %s %zu area=%.0f,%.0f,%.0f,%.0f tex=%llx texels=%.0f,%.0f,%.0f,%.0f text=%d -> %s "
					     "scale %.3f offset %.1f%s\n",
					     i < game_elements ? "game" : "host", i, double(e.x0), double(e.y0), double(e.x1), double(e.y1),
					     static_cast<unsigned long long>(e.texture), double(e.u0), double(e.v0), double(e.u1),
					     double(e.v1), e.text ? 1 : 0,
					     rule < 0 ? "default" : (rules[size_t(rule)].name.empty() ? "rule" : rules[size_t(rule)].name.c_str()),
					     double(placements[i].scale), double(placements[i].offset), placements[i].pin ? " pin" : "");
				}
				for (size_t b = 0; b < batches.size(); b++)
					if (ui_layout.ticker[b] > 0.0f)
						LOGI("UI layout: ticker, batch %zu, period %.2f\n", b, double(ui_layout.ticker[b]));
			}
		}
		else
			ui_target.reset();

		cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
		if (ui_layer_on && ui_target)
			draw_ui_target(*cmd, picture);
		else
			draw_world(*cmd, picture);

		flat_renderer.begin();
		host_ui.render_flat(device, flat_renderer, picture, host_placements);
		render_host_overlay(viewport_width, viewport_height);
		render_fps_counter(viewport_width);
		render_title_version(picture);
		update_discord();
		flat_renderer.flush(*cmd, vec3(0.0f), vec3(viewport_width, viewport_height, 1.0f));
		host_ui.render_native(device, *cmd, picture, host_placements);
		if (gpu_timestamps && present_start)
		{
			// Ends inside the swapchain pass: the UI composition and the world.
			auto present_end = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
			device.register_time_interval("present", std::move(present_start), std::move(present_end),
			                              ui_layer_on ? "picture with UI layer" : "picture");
		}

		cmd->end_render_pass();

		if (new_frame)
		{
			frames_drawn++;
			update_fps();
			stats_window_frames++;
			if (!dump_directory.empty() && (frames_drawn % dump_every) == 0 &&
			    std::chrono::steady_clock::now() - started >= dump_after && frames_drawn >= dump_from && (dump_count == 0 || dumped < dump_count))
			{
				dumped++;
				dump_requested = true;
				dump_layout_next = env_enabled("TAKEDOWN_DEV_UI_LAYOUT_LOG");
			}
			if (input.frame_burst.exchange(false))
			{
				burst_left = 90;
				LOGI("Frame dump: F9 at vsync %llu (presented frame %llu): saving the next %u frames to %s.\n",
				     (unsigned long long)parser.get_vsync_count(), (unsigned long long)frames_drawn, burst_left, dump_directory.c_str());
			}
			if (burst_left && !dump_directory.empty())
			{
				burst_left--;
				dump_requested = true;
			}
		}

		// TAKEDOWN_DEV_FRAME_DUMP_PRESENTED: dump what reaches the window (host
		// text and overlays included) instead of the composed game picture.
		BufferHandle presented;
		if (dump_requested && env_enabled("TAKEDOWN_DEV_FRAME_DUMP_PRESENTED"))
		{
			auto &image = const_cast<Image &>(swapchain_image);
			const uint32_t w = image.get_width(), h = image.get_height();
			BufferCreateInfo info = {};
			info.domain = BufferDomain::CachedHost;
			info.size = VkDeviceSize(w) * h * 4;
			info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			presented = device.create_buffer(info);
			cmd->image_barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			cmd->copy_image_to_buffer(*presented, image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
			cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			                   VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
			cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		}

		bool re_want = false; uint64_t re_cv = 0;
		if (!re_init) re_setup();
		if (re_log && new_frame && (vsync.image || native_scene_image))
		{
			re_cv = re_current_cv();
			if (re_wanted(re_cv) && re_cv != re_last_cv) { re_want = true; re_last_cv = re_cv; re_record_present(device, *cmd, re_cv); }
		}
		stage = 3; // submitting
		if (completion && new_frame)
		{
			Fence fence;
			device.submit(cmd, &fence);
			completion->on_vsync_submitted(parser.get_vsync_count(), [fence]() mutable {
				return fence->wait_timeout(0);
			});
		}
		else
			device.submit(cmd);
		if (dump_requested && presented)
		{
			dump_requested = false;
			device.wait_idle();
			const uint32_t w = swapchain_image.get_width(), h = swapchain_image.get_height();
			auto *pixels = static_cast<uint8_t *>(device.map_host_buffer(*presented, MEMORY_ACCESS_READ_BIT));
			const VkFormat format = swapchain_image.get_format();
			const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
			const bool abgr10 = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			const bool argb10 = format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
			for (size_t i = 0; i < size_t(w) * h; i++)
			{
				uint8_t *p = pixels + i * 4;
				if (abgr10 || argb10)
				{
					uint32_t v;
					memcpy(&v, p, 4);
					const uint8_t lo = uint8_t((v & 0x3ffu) >> 2), mid = uint8_t(((v >> 10) & 0x3ffu) >> 2),
					              hi = uint8_t(((v >> 20) & 0x3ffu) >> 2);
					p[0] = abgr10 ? lo : hi;
					p[1] = mid;
					p[2] = abgr10 ? hi : lo;
				}
				else if (bgra)
					std::swap(p[0], p[2]);
				p[3] = 0xff;
			}
			char path[1024];
			snprintf(path, sizeof(path), "%s/presented-%06llu.png", dump_directory.c_str(),
			         static_cast<unsigned long long>(parser.get_vsync_count()));
			stbi_write_png(path, int(w), int(h), 4, pixels, int(w) * 4);
			device.unmap_host_buffer(*presented, MEMORY_ACCESS_READ_BIT);
		}
		else if (dump_requested)
		{
			dump_requested = false;
			dump_frame(device);
		}

		if (new_frame) rd_hash_pictures(device);
		if (re_want) re_after_submit(device, re_cv);
		if (re_log && parser.get_vsync_count() > re_max + 3) { wait_native_task(); device.wait_idle(); request_shutdown(); }
#if defined(_WIN32)
		if (timestamps && new_frame &&
		    !timestamps->record(frames_drawn, parser.get_vsync_count(), std::chrono::steady_clock::now()))
		{
			LOGE("Frame timestamp capacity was exhausted.\n");
		}
#endif

		if (completion)
			completion->poll();

		if (stats_enabled && new_frame && (frames_drawn % stats_every) == 0)
		{
			auto stats = iface->consume_flush_stats();
			LOGI("GS stats: frame %llu, %u render passes, %u primitives, %u copies, %u MiB images.\n",
			     static_cast<unsigned long long>(frames_drawn), stats.num_render_passes, stats.num_primitives,
			     stats.num_copies, unsigned(stats.allocated_image_memory / (1024 * 1024)));
		}

		// Just in case.
		stage = 4; // GS flush
		iface->flush();
		stage = 5; // outside render_frame (present / event loop)

		if (is_eof)
		{
			ns3_report("eof");
			finish_stream();
			if (!live)
				request_shutdown();
		}
	}

	unsigned get_default_width() override
	{
		return 1280;
	}

	unsigned get_default_height() override
	{
		return 224 * 4;
	}
};
}

// Developer aid: KEY=VALUE lines in takedown-dev-env.txt next to the
// executable become environment variables, so diagnostics can be switched on
// for sessions started by the launcher.
static void load_dev_environment(const char *argv0)
{
	std::string path = argv0 ? argv0 : "";
	const auto slash = path.find_last_of("/\\");
	path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "takedown-dev-env.txt";
	FILE *file = fopen(path.c_str(), "r");
	if (!file)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), file))
	{
		std::string entry = line;
		while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
			entry.pop_back();
		const auto eq = entry.find('=');
		if (entry.empty() || entry[0] == '#' || eq == std::string::npos)
			continue;
#if defined(_WIN32)
		_putenv_s(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str());
#else
		setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
#endif
		LOGI("Dev environment: %s\n", entry.c_str());
	}
	fclose(file);
}

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	TakedownRenderer::install_crash_report();
	// TAKEDOWN_DEV_CRASH_TEST=segv|abort|throw: crash on purpose to check the crash report.
	if (const char *test = getenv("TAKEDOWN_DEV_CRASH_TEST"))
	{
		const std::string kind = test;
		if (kind == "segv")
			*static_cast<volatile int *>(nullptr) = 1;
		else if (kind == "abort")
			abort();
		else if (kind == "throw")
			std::thread([]() { throw std::runtime_error("crash test"); }).join();
	}
	load_dev_environment(argc > 0 ? argv[0] : nullptr);
	ParallelGS::StreamOptions options;
	if (argc > 0 && argv[0])
	{
		const std::string exe = argv[0];
		const auto slash = exe.find_last_of("/\\");
		if (slash != std::string::npos)
			options.exe_directory = exe.substr(0, slash + 1);
	}
	bool valid = argc >= 2;
	for (int i = 1; valid && i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto value = [&](std::string &out) {
			if (i + 1 >= argc)
				return false;
			out = argv[++i];
			return true;
		};

		if (arg == "--input-fifo")
			valid = value(options.input_fifo);
		else if (arg == "--frame-timestamps")
			valid = value(options.frame_timestamps);
		else if (arg == "--completion-fifo")
			valid = value(options.completion_fifo);
		else if (options.stream_path.empty() && !arg.empty() && arg.compare(0, 2, "--") != 0)
			options.stream_path = arg;
		else
			valid = false;
	}

	if (!valid || options.stream_path.empty())
	{
		LOGE("Usage: parallel-gs-stream <graphics-fifo> [--input-fifo <input-fifo>] [--frame-timestamps <csv>] "
		     "[--completion-fifo <fifo>]\n");
		return nullptr;
	}

	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	try
	{
		auto *app = new ParallelGS::StreamApplication(std::move(options));
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
}
