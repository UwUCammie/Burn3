#pragma once

#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <cstdint>

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kBuildRenderStatePacketAddress = 0x0026baa0U;
using RenderStateQuadword = std::array<std::uint32_t, 4>;

// Complete state-template input, independent of guest registers and addresses.
// Names remain conservative until the individual GS state fields are recovered.
struct RenderStatePacketInput {
    std::array<RenderStateQuadword, 14> state_template{};
    std::uint64_t object_state_a{};
    std::uint64_t object_state_b{};
};

using RenderStatePacket = std::array<RenderStateQuadword, 17>;
[[nodiscard]] RenderStatePacket
build_render_state_packet(const RenderStatePacketInput& input) noexcept;

// Temporary EE boundary adapter. The operation above has no interpreter or
// guest scheduling dependency. Registration requires the user's matching ELF.
[[nodiscard]] bool register_render_state_packet_native_function(
    NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);

} // namespace takedown::runtime::slus_21050
