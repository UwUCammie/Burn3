#pragma once

// Native replacements of small, frequently called original functions that
// neither touch hardware nor call other original code. Their arithmetic uses
// the vector unit's own primitives (guest/vu0_macro_fmac.hpp), so results are
// bit-identical to the original's. Registered with the render group
// (src/runtime/game/render.cpp).

#include "takedown/runtime/native_function_dispatcher.hpp"

namespace takedown::runtime::hot_leaves {

// 0x0019d1c0: appends four coloured vertices to a vertex list.
[[nodiscard]] NativeFunctionExecution append_coloured_quad(NativeFunctionContext& context);

// 0x001c6f90: one step of a car engine's revs and gear-shift state.
[[nodiscard]] NativeFunctionExecution update_engine_revs(NativeFunctionContext& context);

// 0x001de198: normalises a 3-vector, returns its length (declines a zero length).
[[nodiscard]] std::uint32_t normalize_vector_cost(const NativeFunctionPrepareContext& context);
[[nodiscard]] NativeFunctionExecution normalize_vector(NativeFunctionContext& context);
// 0x002ae3c0: two points into a frame's normalised axes.
[[nodiscard]] std::uint32_t to_frame_axes_cost(const NativeFunctionPrepareContext& context);
[[nodiscard]] NativeFunctionExecution to_frame_axes(NativeFunctionContext& context);

} // namespace takedown::runtime::hot_leaves
