#include "fsr_upscaler.hpp"

#include "logging.hpp"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using namespace Vulkan;

namespace TakedownRenderer
{
const char *fsr_preset_name(FsrPreset preset)
{
	switch (preset)
	{
	case FsrPreset::NativeAA: return "Native AA";
	case FsrPreset::Quality: return "Quality";
	case FsrPreset::Balanced: return "Balanced";
	case FsrPreset::Performance: return "Performance";
	case FsrPreset::UltraPerformance: return "Ultra Performance";
	default: return "Off";
	}
}

float fsr_preset_ratio(FsrPreset preset)
{
	switch (preset)
	{
	case FsrPreset::Quality: return 1.5f;
	case FsrPreset::Balanced: return 1.7f;
	case FsrPreset::Performance: return 2.0f;
	case FsrPreset::UltraPerformance: return 3.0f;
	default: return 1.0f;
	}
}

void fsr_jitter(uint32_t index, uint32_t render_width, uint32_t display_width, float &x, float &y)
{
	const int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount(int32_t(render_width), int32_t(std::max(display_width, render_width)));
	x = y = 0.0f;
	if (phases > 0)
		ffxFsr3UpscalerGetJitterOffset(&x, &y, int32_t(index % uint32_t(phases)), phases);
}

uint32_t fsr_render_scale(FsrPreset preset, uint32_t output_height)
{
	// The nearest whole multiple of the game's 448 lines, never above the
	// picture (FSR only scales up).
	const float lines = float(output_height) / fsr_preset_ratio(preset);
	const uint32_t nearest = uint32_t(std::lround(lines / 448.0f));
	const uint32_t fits = output_height / 448u;
	return std::max(1u, std::min({ 6u, fits, nearest }));
}

namespace
{
constexpr VkFormat kColourFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

ImageHandle make_image(Device &device, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                       const char *name)
{
	ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(width, height, format);
	info.usage = usage;
	info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.misc = 0;
	auto image = device.create_image(info);
	if (image)
		device.set_name(*image, name);
	return image;
}

VkImageCreateInfo vk_info(const Image &image)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = image.get_format();
	info.extent = { image.get_width(), image.get_height(), 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = image.get_create_info().usage;
	return info;
}

FfxResource ffx_image(const Image &image, const wchar_t *name, FfxResourceStates state, FfxResourceUsage usage)
{
	return ffxGetResourceVK(reinterpret_cast<void *>(image.get_image()),
	                        ffxGetImageResourceDescriptionVK(image.get_image(), vk_info(image), usage), name, state);
}

void ffx_message(FfxMsgType type, const wchar_t *message)
{
	char text[512];
	std::wcstombs(text, message, sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	if (type == FFX_MESSAGE_TYPE_ERROR)
		LOGE("FSR: %s\n", text);
	else
		LOGW("FSR: %s\n", text);
}

// The SDK asks for some entry points by their extension names
// (vkGetBufferMemoryRequirements2KHR, the debug-utils calls). Granite's device
// may only have the core Vulkan 1.1+ names, or no debug utils at all.
VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
VKAPI_ATTR void VKAPI_CALL no_end_label(VkCommandBuffer) {}
VKAPI_ATTR VkResult VKAPI_CALL no_object_name(VkDevice, const VkDebugUtilsObjectNameInfoEXT *) { return VK_SUCCESS; }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char *name)
{
	if (PFN_vkVoidFunction function = vkGetDeviceProcAddr(device, name))
		return function;
	const size_t length = std::strlen(name);
	for (const char *suffix : { "KHR", "EXT" })
	{
		if (length > 3 && std::strcmp(name + length - 3, suffix) == 0)
		{
			const std::string core(name, length - 3);
			if (PFN_vkVoidFunction function = vkGetDeviceProcAddr(device, core.c_str()))
				return function;
		}
	}
	if (std::strcmp(name, "vkCmdBeginDebugUtilsLabelEXT") == 0 || std::strcmp(name, "vkCmdInsertDebugUtilsLabelEXT") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(no_label);
	if (std::strcmp(name, "vkCmdEndDebugUtilsLabelEXT") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(no_end_label);
	if (std::strcmp(name, "vkSetDebugUtilsObjectNameEXT") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(no_object_name);
	// Vendor breadcrumb markers are optional; anything else is worth a note.
	if (!std::strstr(name, "AMD") && !std::strstr(name, "NV"))
		LOGW("FSR: the Vulkan device has no %s.\n", name);
	return nullptr;
}

void to_layout(CommandBuffer &cmd, const Image &image, VkImageLayout from, VkImageLayout to,
               VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
               VkAccessFlags2 dst_access)
{
	cmd.image_barrier(image, from, to, src_stage, src_access, dst_stage, dst_access);
}
} // namespace

struct FsrUpscaler::Impl
{
	Device *device = nullptr;
	VkDeviceContext device_context = {};
	std::vector<uint8_t> scratch;
	FfxInterface backend = {};
	bool backend_valid = false;

	// The SDK keeps its private context (structs with 8- and 16-byte members)
	// in this uint32_t array, so give it the alignment of what it really holds.
	alignas(64) FfxFsr3UpscalerContext context = {};
	bool context_valid = false;
	uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;

	ImageHandle colour[2], reactive, depth, motion, output;
	// FSR 3.1's shared resources (ffxFsr3UpscalerGetSharedResourceDescriptions),
	// which the application allocates: at the maximum render size, kept in
	// GENERAL (FFX_RESOURCE_STATE_UNORDERED_ACCESS).
	ImageHandle dilated_depth, dilated_motion, previous_depth;
	bool shared_ready = false;
	unsigned current = 0;
	bool have_previous = false;
	bool constants_ready = false;

	~Impl()
	{
		destroy_context();
	}

	void destroy_context()
	{
		if (context_valid)
		{
			// The context's resources may still be in use by queued frames.
			if (device)
				device->wait_idle();
			ffxFsr3UpscalerContextDestroy(&context);
			context_valid = false;
		}
	}

	bool init_backend(Device &dev)
	{
		device = &dev;
		device_context.vkDevice = dev.get_device();
		device_context.vkPhysicalDevice = dev.get_physical_device();
		device_context.vkDeviceProcAddr = device_proc;
		const size_t size = ffxGetScratchMemorySizeVK(dev.get_physical_device(), FFX_FSR3UPSCALER_CONTEXT_COUNT);
		scratch.assign(size + 64, 0);
		// 64-byte aligned start for the backend's own structs.
		auto *start = scratch.data() + ((64 - reinterpret_cast<uintptr_t>(scratch.data()) % 64) % 64);
		const FfxErrorCode error = ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&device_context), start, size,
		                                             FFX_FSR3UPSCALER_CONTEXT_COUNT);
		if (error != FFX_OK)
		{
			LOGE("FSR: could not set up the Vulkan backend (error %d).\n", int(error));
			return false;
		}
		backend_valid = true;
		return true;
	}

	// One context per output size; the render size may change from frame to
	// frame below it (a 1x menu picture, a native scene at the FSR scale).
	bool ensure_context(uint32_t ow, uint32_t oh)
	{
		if (context_valid && ow == output_w && oh == output_h)
			return true;
		destroy_context();
		FfxFsr3UpscalerContextDescription description = {};
		// No exposure input: the picture is display-referred LDR. Constant depth,
		// so the depth convention does not matter; zero motion at render size.
		// Depth as the native scene gives it (1 near, 0 far, no far plane); the constant depth is the far plane.
		description.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
		description.maxRenderSize = { ow, oh };
		description.maxUpscaleSize = { ow, oh };
		description.fpMessage = ffx_message;
		description.backendInterface = backend;
		const FfxErrorCode error = ffxFsr3UpscalerContextCreate(&context, &description);
		if (error != FFX_OK)
		{
			LOGE("FSR: could not create the upscaler (error %d).\n", int(error));
			return false;
		}
		context_valid = true;
		output_w = ow;
		output_h = oh;
		const VkImageUsageFlags shared_usage =
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		dilated_depth = make_image(*device, ow, oh, VK_FORMAT_R32_SFLOAT, shared_usage, "FSR dilated depth");
		dilated_motion = make_image(*device, ow, oh, VK_FORMAT_R16G16_SFLOAT, shared_usage, "FSR dilated motion");
		previous_depth = make_image(*device, ow, oh, VK_FORMAT_R32_UINT,
		                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                            "FSR reconstructed previous depth");
		shared_ready = false;
		if (!dilated_depth || !dilated_motion || !previous_depth)
		{
			LOGE("FSR: could not allocate its shared images.\n");
			destroy_context();
			return false;
		}
		LOGI("FSR 3.1: upscaler ready for %ux%u.\n", ow, oh);
		return true;
	}

	void ensure_images(Device &dev, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
	{
		const VkImageUsageFlags rw_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (!colour[0] || colour[0]->get_width() != rw || colour[0]->get_height() != rh)
		{
			colour[0] = make_image(dev, rw, rh, kColourFormat, rw_usage, "FSR colour A");
			colour[1] = make_image(dev, rw, rh, kColourFormat, rw_usage, "FSR colour B");
			reactive = make_image(dev, rw, rh, VK_FORMAT_R8_UNORM, rw_usage, "FSR reactive");
			depth = make_image(dev, rw, rh, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			                   "FSR depth (constant)");
			motion = make_image(dev, rw, rh, VK_FORMAT_R16G16_SFLOAT,
			                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "FSR motion (zero)");
			have_previous = false;
			constants_ready = false;
		}
		if (!output || output->get_width() != ow || output->get_height() != oh)
			output = make_image(dev, ow, oh, kColourFormat, rw_usage, "FSR output");
	}
};

FsrUpscaler::FsrUpscaler() = default;
FsrUpscaler::~FsrUpscaler() = default;

void FsrUpscaler::reset_device_objects()
{
	impl_.reset();
	failed_ = false;
	reset_ = true;
}

const Image *FsrUpscaler::upscale(Device &device, const Image &source, const float gain[3], uint32_t width,
                                  uint32_t height, float sharpness, float frame_ms, const FsrTaaInputs *taa)
{
	if (failed_)
		return nullptr;
	const uint32_t rw = source.get_width();
	const uint32_t rh = source.get_height();
	// FSR only scales up.
	if (width < rw || height < rh || width == 0 || height == 0)
		return nullptr;

	if (!impl_)
	{
		impl_ = std::make_unique<Impl>();
		if (!impl_->init_backend(device))
		{
			failed_ = true;
			impl_.reset();
			return nullptr;
		}
	}
	auto &d = *impl_;
	if (d.render_w != rw || d.render_h != rh || d.output_w != width || d.output_h != height)
	{
		reset_ = true;
		LOGI("FSR 3.1: %ux%u to %ux%u (%.2fx).\n", rw, rh, width, height, double(height) / double(rh));
		d.render_w = rw;
		d.render_h = rh;
	}
	d.ensure_images(device, rw, rh, width, height);
	if (!d.colour[0] || !d.colour[1] || !d.reactive || !d.depth || !d.motion || !d.output)
	{
		LOGE("FSR: could not allocate its images.\n");
		failed_ = true;
		return nullptr;
	}
	if (!d.ensure_context(width, height))
	{
		failed_ = true;
		return nullptr;
	}
	if (reset_)
		d.have_previous = false;

	const unsigned cur = d.current;
	const unsigned prev = cur ^ 1u;
	auto &colour = *d.colour[cur];
	auto &previous = *d.colour[prev];
	constexpr auto kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	constexpr auto kGeneral = VK_IMAGE_LAYOUT_GENERAL;
	constexpr auto kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

	// Inputs: colour (gain applied, clamped) and the reactive mask.
	{
		auto cmd = device.request_command_buffer();
		cmd->begin_region("FSR inputs");
		if (!d.constants_ready)
		{
			for (const Image *image : { d.depth.get(), d.motion.get() })
				to_layout(*cmd, *image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkClearValue far_depth = {};
			far_depth.color.float32[0] = 0.0f;
			cmd->clear_image(*d.depth, far_depth);
			VkClearValue zero = {};
			cmd->clear_image(*d.motion, zero);
			for (const Image *image : { d.depth.get(), d.motion.get() })
				to_layout(*cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRead, VK_PIPELINE_STAGE_2_CLEAR_BIT,
				          VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			d.constants_ready = true;
		}
		if (!d.shared_ready)
		{
			for (const Image *image : { d.dilated_depth.get(), d.dilated_motion.get(), d.previous_depth.get() })
				to_layout(*cmd, *image, VK_IMAGE_LAYOUT_UNDEFINED, kGeneral, VK_PIPELINE_STAGE_2_NONE, 0, kCompute,
				          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
			d.shared_ready = true;
		}
		to_layout(*cmd, colour, VK_IMAGE_LAYOUT_UNDEFINED, kGeneral, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		          kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		to_layout(*cmd, *d.reactive, VK_IMAGE_LAYOUT_UNDEFINED, kGeneral, kCompute,
		          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		// The other colour image was last written here and read by FSR, or is new.
		to_layout(*cmd, previous, d.have_previous ? kRead : VK_IMAGE_LAYOUT_UNDEFINED, kGeneral, kCompute,
		          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

		cmd->set_program("builtin://shaders/takedown_fsr_prepare.comp");
		cmd->set_texture(0, 0, source.get_view(), StockSampler::NearestClamp);
		cmd->set_storage_texture(0, 1, colour.get_view());
		cmd->set_storage_texture(0, 2, previous.get_view());
		cmd->set_storage_texture(0, 3, d.reactive->get_view());
		struct
		{
			float gain[4];
			uint32_t size[2];
			float low, high;
			uint32_t have_previous;
		} push = { { gain[0], gain[1], gain[2], 1.0f }, { rw, rh }, 0.02f, 0.12f, d.have_previous ? 1u : 0u };
		if (taa)
		{
			// With motion vectors the history is reprojected: no reactive mask.
			push.low = 2.0f;
			push.high = 3.0f;
		}
		cmd->push_constants(&push, 0, sizeof(push));
		cmd->dispatch((rw + 7) / 8, (rh + 7) / 8, 1);

		for (const Image *image : { &colour, d.reactive.get(), &previous })
			to_layout(*cmd, *image, kGeneral, kRead, kCompute,
			          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, kCompute,
			          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		// FSR writes the output as a storage image.
		to_layout(*cmd, *d.output, VK_IMAGE_LAYOUT_UNDEFINED, kGeneral, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute,
		          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
		cmd->end_region();
		device.submit(cmd);
	}

	// FSR records raw Vulkan commands, so it gets a command buffer of its own
	// (no Granite state, descriptor buffers or pipelines bound in it).
	{
		auto cmd = device.request_command_buffer();
		FfxFsr3UpscalerDispatchDescription dispatch = {};
		dispatch.commandList = ffxGetCommandListVK(cmd->get_command_buffer());
		dispatch.color = ffx_image(colour, L"FSR3_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ,
		                           FFX_RESOURCE_USAGE_READ_ONLY);
		const bool with_taa = taa && taa->depth && taa->motion && taa->depth->get_width() == rw &&
		                      taa->depth->get_height() == rh && taa->motion->get_width() == rw && taa->motion->get_height() == rh;
		dispatch.depth = ffx_image(with_taa ? *taa->depth : *d.depth, L"FSR3_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ,
		                           FFX_RESOURCE_USAGE_READ_ONLY);
		dispatch.motionVectors = ffx_image(with_taa ? *taa->motion : *d.motion, L"FSR3_InputMotionVectors",
		                                   FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
		dispatch.reactive = ffx_image(*d.reactive, L"FSR3_InputReactiveMask", FFX_RESOURCE_STATE_COMPUTE_READ,
		                              FFX_RESOURCE_USAGE_READ_ONLY);
		dispatch.dilatedDepth = ffx_image(*d.dilated_depth, L"FSR3UPSCALER_DilatedDepth",
		                                  FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
		dispatch.dilatedMotionVectors = ffx_image(*d.dilated_motion, L"FSR3UPSCALER_DilatedVelocity",
		                                          FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
		dispatch.reconstructedPrevNearestDepth =
		    ffx_image(*d.previous_depth, L"FSR3UPSCALER_ReconstructedPrevNearestDepth",
		              FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
		dispatch.output = ffx_image(*d.output, L"FSR3_OutputUpscaledColor", FFX_RESOURCE_STATE_UNORDERED_ACCESS,
		                            FFX_RESOURCE_USAGE_UAV);
		dispatch.jitterOffset = { with_taa ? taa->jitter_px[0] : 0.0f, with_taa ? taa->jitter_px[1] : 0.0f };
		dispatch.motionVectorScale = { float(rw), float(rh) };
		dispatch.renderSize = { rw, rh };
		dispatch.upscaleSize = { width, height };
		dispatch.enableSharpening = sharpness > 0.0f;
		dispatch.sharpness = std::max(0.0f, std::min(1.0f, sharpness));
		dispatch.frameTimeDelta = std::max(1.0f, std::min(100.0f, frame_ms));
		dispatch.preExposure = 1.0f;
		dispatch.reset = reset_;
		// Nominal camera values; with constant depth they only scale internal
		// thresholds.
		dispatch.cameraNear = with_taa ? taa->near_plane : 0.1f;
		dispatch.cameraFar = 1e6f;
		dispatch.cameraFovAngleVertical = with_taa ? taa->fov_y : 1.0f;
		dispatch.viewSpaceToMetersFactor = 1.0f;
		const FfxErrorCode error = ffxFsr3UpscalerContextDispatch(&d.context, &dispatch);
		to_layout(*cmd, *d.output, kGeneral, kRead, kCompute,
		          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kCompute,
		          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		device.submit(cmd);
		if (error != FFX_OK)
		{
			LOGE("FSR: dispatch failed (error %d); FSR is off for this session.\n", int(error));
			failed_ = true;
			return nullptr;
		}
	}

	d.have_previous = true;
	d.current = prev;
	reset_ = false;
	return d.output.get();
}
} // namespace TakedownRenderer
