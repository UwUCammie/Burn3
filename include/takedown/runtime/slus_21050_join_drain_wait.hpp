#pragma once

#include <cstdint>

namespace takedown::runtime::slus_21050 {

// Front-end queue completion and its earliest outstanding checkpoint. The
// caller owns queue advancement and checkpoint publication; no CPU register or
// instruction execution is part of this operation.
struct JoinDrainState {
    std::uint32_t consumed{};
    std::uint32_t submitted{};
    bool active{};
    std::uint64_t retained_checkpoint{};
    std::uint64_t requested_checkpoint{};
};

struct JoinDrainPoll {
    bool pending{};
    bool queue_pending{};
    bool publish_checkpoint{};
    std::uint64_t checkpoint{};
};

[[nodiscard]] constexpr JoinDrainPoll poll_join_drain(const JoinDrainState& state) noexcept {
    const bool queue_pending = state.consumed != state.submitted;
    const bool publish = state.requested_checkpoint < state.retained_checkpoint;
    return {queue_pending || state.active, queue_pending, publish,
            publish ? state.requested_checkpoint : state.retained_checkpoint};
}

} // namespace takedown::runtime::slus_21050
