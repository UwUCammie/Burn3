#pragma once

#include "takedown/runtime/native_function_dispatcher.hpp"

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

// Burnout 3 submits each visible world object (scenery pieces, props) to the
// frame's draw chain through one routine. It culls the object's bounding box
// against the frustum of the view it is drawn in (engine::classify), and when
// any of it is visible appends a call to the object's prebuilt draw program,
// the draw-list entries queued since the last object, an optional clip-mode
// change and a tail.
//
// Registers the native version in place of the original routine. Returns
// false when the loaded executable is not the retail build this matches.
[[nodiscard]] bool
register_slus_21050_world_object_submit_native_function(NativeFunctionDispatcher& dispatcher,
                                                        const guest::EeMemory& memory);

} // namespace takedown::runtime::slus_21050
