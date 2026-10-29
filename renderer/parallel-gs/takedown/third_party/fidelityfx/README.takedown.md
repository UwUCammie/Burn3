# AMD FidelityFX SDK (FSR 3.1 upscaler only)

Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v1.1.4`
(commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`), the last release with a Vulkan
backend. MIT licence: `LICENSE.txt`.

Kept: the FSR 3.1 upscaler component, the Vulkan backend (`ffx_vk.cpp`), the
shared host code, the host headers, and the GPU headers and GLSL passes the
upscaler uses (`fsr3upscaler`, `fsr1` for RCAS, `spd`).

Not kept: every other effect, frame generation and its swapchain, the DX12
backend, `ffx_shader_blobs.cpp` and the FidelityFX_SC shader compiler.

Local changes (everything else is byte-identical to the tag):

- `include/FidelityFX/host/ffx_fsr3upscaler.h`: the context is twice as large
  outside Windows, where `wchar_t` is 4 bytes (the Linux replay rig).
- `src/backends/vk/ffx_vk.cpp`: the arrays in the backend's scratch memory
  start 64-byte aligned (upstream: 4). GCC at -O3 uses aligned vector stores
  on them (`EffectContext` needs 32-byte alignment), which crashed context
  creation; UBSan is clean with the change. The caller (fsr_upscaler.cpp) also
  passes a 64-byte aligned scratch buffer and context.
- `ffx_fsr3upscaler_takedown_blobs.cpp` replaces `ffx_shader_blobs.cpp` and
  the per-effect blob accessors: it serves the upscaler's SPIR-V from
  `generated/` and stubs the frame-generation swapchain hook.
- `tools/gen_fsr3_shaders.py` builds `generated/ffx_fsr3upscaler_takedown_shaders.h`
  with glslangValidator and its own SPIR-V reflection, in place of
  FidelityFX_SC (Windows only). Only the permutations the renderer can ask for
  are built (no HDR input, low-resolution motion vectors without jitter;
  Lanczos LUT, inverted depth and sharpening on and off). FP16 and wave64
  requests get the 32-bit shader. Rerun it after updating the SDK.
- `ffx_takedown_compat.h` is force-included: Vulkan calls go through Granite's
  volk loader, and outside Windows it supplies the MSVC "secure" CRT calls.
