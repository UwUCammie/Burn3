#pragma once

#include "takedown/presentation/native_ui_rectangle_batch.hpp"

#include <optional>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime::slus_21050 {

// Read only at the authenticated text callback boundary, after Begin and all
// preceding callbacks. These values are separate from the retained UI graph:
// the streamer reads the color-scale W and all four global position lanes.
// No assumed retail constant, packet allocation, or guest write is involved.
// This data snapshot does not authenticate code, ambient GS state, resources,
// or callback identity. The live producer must still establish those contracts.
[[nodiscard]] std::optional<presentation::NativeUiTextConversionInput>
read_ui_text_conversion_from_guest(const guest::EeMemory& memory);

// Exact dependency revalidation for a previously captured conversion input.
// This check must be in the same serialized callback transaction as dispatch;
// it provides no synchronization against concurrent memory mutation itself.
[[nodiscard]] bool
is_ui_text_conversion_current(const guest::EeMemory& memory,
                              const presentation::NativeUiTextConversionInput& expected);

} // namespace takedown::runtime::slus_21050
