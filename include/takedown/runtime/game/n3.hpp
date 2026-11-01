#pragma once

// Native replacements written one function per file (src/runtime/game/n3/<address>.cpp).
// Each file defines `constinit const EngineFunction n3_<address>` in this namespace; the
// list of files linked into a build is src/runtime/game/n3_list.inc (one N3(<address>)
// line each), or the file named by N3_LIST when a single function is checked alone.
// The group is appended to the render group (src/runtime/game/render.cpp).

#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <span>

namespace takedown::runtime {

[[nodiscard]] std::span<const EngineFunction> n3_engine_functions();

} // namespace takedown::runtime
