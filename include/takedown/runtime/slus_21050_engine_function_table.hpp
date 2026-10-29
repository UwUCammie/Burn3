#pragma once

// The groups of native replacements, one per game system. Each group lives
// in src/runtime/game/<system>.cpp next to the adapters it lists.

#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <array>
#include <span>

namespace takedown::runtime {

[[nodiscard]] std::span<const EngineFunction> audio_engine_functions();
[[nodiscard]] std::span<const EngineFunction> c_library_engine_functions();
[[nodiscard]] std::span<const EngineFunction> content_engine_functions();
[[nodiscard]] std::span<const EngineFunction> math_library_engine_functions();
[[nodiscard]] std::span<const EngineFunction> physics_engine_functions();
[[nodiscard]] std::span<const EngineFunction> render_engine_functions();
[[nodiscard]] std::span<const EngineFunction> text_engine_functions();
[[nodiscard]] std::span<const EngineFunction> track_engine_functions();
[[nodiscard]] std::span<const EngineFunction> world_visibility_engine_functions();

[[nodiscard]] inline std::array<std::span<const EngineFunction>, 9U> slus_21050_engine_function_groups() {
    return {audio_engine_functions(), c_library_engine_functions(), content_engine_functions(), math_library_engine_functions(), physics_engine_functions(),
            render_engine_functions(), text_engine_functions(), track_engine_functions(), world_visibility_engine_functions()};
}

} // namespace takedown::runtime
