// Shader lookup for the FidelityFX Vulkan backend, in place of the SDK's
// src/backends/shared/ffx_shader_blobs.cpp and its FidelityFX_SC tables. Only
// the FSR 3.1 upscaler is built (generated/ffx_fsr3upscaler_takedown_shaders.h,
// tools/gen_fsr3_shaders.py). FP16 and wave64 requests get the 32-bit shader.

#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include "ffx_shader_blobs.h"
#include "generated/ffx_fsr3upscaler_takedown_shaders.h"

#include <cstring>
#include <new>

namespace
{
// FSR3UPSCALER_SHADER_PERMUTATION_* bits the generated table keys on.
constexpr uint32_t kKeyBits = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5);
}

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId, FfxPass passId, FfxBindStage,
                                                     uint32_t permutationOptions, FfxShaderBlob *outBlob)
{
	if (effectId != FFX_EFFECT_FSR3UPSCALER || !outBlob)
		return FFX_ERROR_BACKEND_API_ERROR;
	const uint32_t key = permutationOptions & kKeyBits;
	for (const auto &shader : takedown_fsr3::kShaders)
	{
		if (shader.pass != uint32_t(passId) || shader.flags != key)
			continue;
		// FfxShaderBlob has const members: build it in place.
		outBlob->~FfxShaderBlob();
		new (outBlob) FfxShaderBlob{
			reinterpret_cast<const uint8_t *>(shader.spirv), shader.size,
			shader.cbv_count, shader.srv_tex_count, shader.uav_tex_count, shader.srv_buf_count, shader.uav_buf_count,
			0, 0,
			shader.cbv_names, shader.cbv_bindings, shader.cbv_counts, shader.cbv_spaces,
			shader.srv_tex_names, shader.srv_tex_bindings, shader.srv_tex_counts, shader.srv_tex_spaces,
			shader.uav_tex_names, shader.uav_tex_bindings, shader.uav_tex_counts, shader.uav_tex_spaces,
			shader.srv_buf_names, shader.srv_buf_bindings, shader.srv_buf_counts, shader.srv_buf_spaces,
			shader.uav_buf_names, shader.uav_buf_bindings, shader.uav_buf_counts, shader.uav_buf_spaces,
			nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr,
		};
		return FFX_OK;
	}
	return FFX_ERROR_BACKEND_API_ERROR;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &isWave64)
{
	isWave64 = false;
	return FFX_OK;
}

// Frame generation (and its replacement swapchain) is not built; the backend
// still refers to this hook.
#include <FidelityFX/host/backends/vk/ffx_vk.h>
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *)
{
	return FFX_ERROR_INVALID_ARGUMENT;
}
