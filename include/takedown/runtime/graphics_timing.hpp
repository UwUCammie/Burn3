#pragma once

#include <cstdint>
#include <algorithm>

namespace takedown::runtime {

// Advance the two EE-timed graphics producers to one precise scheduler
// boundary. PCSX2's EE event scan services DMAC_VIF1 before DMAC_GIF when
// both events mature together; preserving that tie order lets a due DIRECT
// request participate in GIF arbitration before PATH3 consumes its next QW.
// The cursors are updated only after their respective callback succeeds.
template <typename AdvanceVif1, typename AdvanceGif>
void service_graphics_dmacs_until(const std::uint64_t target_cycle,
                                  std::uint64_t& vif1_serviced_cycle,
                                  std::uint64_t& gif_serviced_cycle,
                                  AdvanceVif1&& advance_vif1,
                                  AdvanceGif&& advance_gif) {
    if (target_cycle > vif1_serviced_cycle) {
        advance_vif1(target_cycle - vif1_serviced_cycle);
        vif1_serviced_cycle = target_cycle;
    }
    if (target_cycle > gif_serviced_cycle) {
        advance_gif(target_cycle - gif_serviced_cycle);
        gif_serviced_cycle = target_cycle;
    }
}

// Temporary compatibility bridge for a native operation that owns a whole
// polling interval. Preserve producer interleaving until native transfer
// ownership can replace it: advancing all VIF cycles before all GIF cycles
// changes arbitration when both producers can make progress.
template <typename AdvanceVif1, typename AdvanceGif>
void service_graphics_dmacs_interleaved_until(const std::uint64_t target_cycle,
                                             std::uint64_t& vif1_serviced_cycle,
                                             std::uint64_t& gif_serviced_cycle,
                                             AdvanceVif1&& advance_vif1,
                                             AdvanceGif&& advance_gif) {
    auto cursor = std::min(vif1_serviced_cycle, gif_serviced_cycle);
    while (cursor < target_cycle) {
        service_graphics_dmacs_until(++cursor, vif1_serviced_cycle,
                                    gif_serviced_cycle, advance_vif1, advance_gif);
    }
}

} // namespace takedown::runtime
