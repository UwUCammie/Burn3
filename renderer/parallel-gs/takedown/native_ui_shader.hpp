#pragma once
// Vertex and push-constant layouts of builtin://shaders/takedown_native_ui.{vert,frag},
// shared by the host UI layer and the game's UI layer.
#include <cstddef>
#include <cstdint>

namespace TakedownRenderer
{
// Position (640x448 picture pixels), UV, Color.
struct NativeVertex
{
	float x, y;
	float u, v;
	float r, g, b, a;
};
static_assert(sizeof(NativeVertex) == 32, "NativeVertex must match takedown_native_ui.vert");

// std430, 112 bytes. textured: 0 none, 1 a GS texture (texel coordinates,
// wrapped/clamped in the shader, 128 = full alpha), 2 an ordinary image (0..1).
struct NativePushConstants
{
	float region[4];                // 0
	std::uint32_t textured;         // 16
	std::uint32_t tfx;              // 20
	std::uint32_t tcc;              // 24
	std::uint32_t alpha_reference;  // 28
	float fixed_alpha;              // 32
	std::uint32_t alpha_test;       // 36
	std::uint32_t alpha_fail;       // 40
	std::uint32_t padding;          // 44
	std::uint32_t wrap[4];          // 48
	float gain[4];                  // 64
	float sizes[4];                 // 80
	float bias[4];                  // 96
};
static_assert(sizeof(NativePushConstants) == 112, "NativePushConstants must match takedown_native_ui.frag");
static_assert(offsetof(NativePushConstants, tfx) == 20, "push constant offset");
static_assert(offsetof(NativePushConstants, fixed_alpha) == 32, "push constant offset");
static_assert(offsetof(NativePushConstants, wrap) == 48, "push constant offset");
static_assert(offsetof(NativePushConstants, gain) == 64, "push constant offset");
static_assert(offsetof(NativePushConstants, bias) == 96, "push constant offset");
}
