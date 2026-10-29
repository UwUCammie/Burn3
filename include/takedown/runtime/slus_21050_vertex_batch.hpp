#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/slus_21050_native_functions.hpp"

#include <cstdint>
#include <optional>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime {

class NativeFunctionDispatcher;

namespace slus_21050 {

inline constexpr guest::GuestAddress kAppend2DVertexBatchToVifPacketAddress =
    0x00218d00U;
inline constexpr std::uint32_t kVertex2DBatchMaximumCount = 506U;
inline constexpr NativeFunctionLimits kVertex2DBatchMaximumCost{11325U,
                                                                 11325U};

// Raw binary32 coordinates remain bits until the exact EE/VU arithmetic is
// selected. This keeps signed zero, denormals, and exponent-255 encodings from
// being changed by a host-facing float abstraction.
struct Position2DBits {
    std::uint32_t x{};
    std::uint32_t y{};

    bool operator==(const Position2DBits&) const = default;
};

// One semantic 16-byte record emitted after the VIF header. The first half is
// the caller's parallel attribute value. The second half carries the packed
// fixed-point position and the global vertex flags exactly as the retail EE
// bit operations produce them.
struct Vertex2DPacketRecord {
    std::uint64_t attribute{};
    std::uint64_t packed_position_and_flags{};

    bool operator==(const Vertex2DPacketRecord&) const = default;
};

// Register-independent description of the parallel streams consumed by the
// retail 2D vertex-batch packet appender. All addresses are raw canonical EE
// addresses: the source streams live in direct main RAM, while the packet ring
// may live in direct main RAM or in the canonical 16 KiB scratchpad window.
struct Vertex2DBatchInput {
    guest::GuestAddress batch_attribute_vector{};
    guest::GuestAddress origin{};
    guest::GuestAddress coordinates{};
    guest::GuestAddress attributes{};
    std::uint32_t vertex_count{};

    bool operator==(const Vertex2DBatchInput&) const = default;
};

enum class Vertex2DBatchHeaderMode : std::uint8_t {
    // The live packet kind differs from six, so retail enters the fresh-header
    // allocation directly from its first comparison.
    Fresh,
    // Packet kind six matched, but its live descriptor did not. Retail
    // performs the second comparison and then allocates the same fresh header.
    FreshAfterDescriptorMismatch,
    // Packet kind and descriptor both matched, so retail extends the live
    // header and emits the strip-continuity bridge records.
    ReuseExisting,
};

enum class Vertex2DBatchCursorMode : std::uint8_t {
    // The packet cursor is already on the 16-byte boundary required by the
    // fresh-header stores.
    AlreadyAligned,
    // The cursor has the retail eight-byte residue. The function advances and
    // publishes it before allocating a fresh header at the aligned address.
    AdvanceByEight,
};

enum class Vertex2DBatchCapacityMode : std::uint8_t {
    // The entry ring has at least vertex_count+5 free qwords, so retail
    // proceeds directly to its live-header inspection.
    ExistingRoom,
    // The entry ring is too full. Retail drains it, then retries header
    // selection at the aligned ring begin and necessarily creates a header.
    FlushThenAppend,
};

// A side-effect-free resolution of the reviewed capacity, header, and cursor
// paths. A near-full plan carries the complete shared drain plan and describes
// the fresh aligned append that follows at ring_begin. The exact subtree cost
// is carried for scheduler preflight and checked again immediately before
// commit. Reuse plans retain the preceding record and old header count so a
// changed live packet is rejected atomically.
struct Vertex2DBatchAppendPlan {
    Vertex2DBatchInput input;
    Vertex2DBatchCapacityMode capacity_mode{
        Vertex2DBatchCapacityMode::ExistingRoom};
    Vertex2DBatchHeaderMode header_mode{Vertex2DBatchHeaderMode::Fresh};
    Vertex2DBatchCursorMode cursor_mode{
        Vertex2DBatchCursorMode::AlreadyAligned};
    guest::GuestAddress ring_begin{};
    // Raw cursor observed before the optional eight-byte repair.
    guest::GuestAddress initial_packet_cursor{};
    // Cursor selected after the capacity branch. This is the entry cursor
    // when room exists and ring_begin after a successful flush.
    guest::GuestAddress append_packet_cursor{};
    // Normalized address at which this invocation begins writing its packet.
    guest::GuestAddress packet_begin{};
    guest::GuestAddress header{};
    // First emitted record. Reuse emits its bridge records here and starts
    // the common vertex loop 48 bytes later.
    guest::GuestAddress records_begin{};
    guest::GuestAddress packet_end{};
    std::uint64_t descriptor{};
    std::uint32_t initial_header_vertex_count{};
    Vertex2DPacketRecord preceding_record{};
    std::optional<VifPacketRingFlushPlan> flush_plan;
    std::uint32_t pair_iterations{};
    std::uint32_t tail_vertices{};
    NativeFunctionLimits cost{};

    bool operator==(const Vertex2DBatchAppendPlan&) const = default;
};

// Semantic packet result returned by the typed core. Architectural EE register
// residue is published separately by the native compatibility adapter.
struct Vertex2DBatchAppendResult {
    Vertex2DBatchCapacityMode capacity_mode{
        Vertex2DBatchCapacityMode::ExistingRoom};
    Vertex2DBatchHeaderMode header_mode{Vertex2DBatchHeaderMode::Fresh};
    Vertex2DBatchCursorMode cursor_mode{
        Vertex2DBatchCursorMode::AlreadyAligned};
    guest::GuestAddress packet_begin{};
    guest::GuestAddress header{};
    // First record emitted by this invocation. Reuse begins with the bridge
    // copy; fresh mode begins with the first input vertex.
    guest::GuestAddress records_begin{};
    guest::GuestAddress packet_end{};
    std::uint64_t descriptor{};
    std::uint32_t header_vertex_count{};
    std::uint32_t vertex_count{};
    std::optional<VifPacketRingFlushResult> flush_result;

    bool operator==(const Vertex2DBatchAppendResult&) const = default;
};

[[nodiscard]] std::optional<Vertex2DBatchAppendPlan>
plan_2d_vertex_batch_append(const guest::EeMemory& memory,
                            const Vertex2DBatchInput& input);

// Revalidates the plan, then appends to the selected packet/header. Throws
// std::invalid_argument when the plan is stale or outside the reviewed path.
[[nodiscard]] Vertex2DBatchAppendResult
append_2d_vertex_batch_to_vif_packet(
    guest::EeMemory& memory, const Vertex2DBatchAppendPlan& plan);

} // namespace slus_21050

// Optional, independently fingerprinted native registration. A signature
// mismatch returns false without changing the dispatcher. The live resolver
// repeats the complete fingerprint check for every invocation.
[[nodiscard]] bool register_slus_21050_vertex_batch_native_function(
    NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);

} // namespace takedown::runtime
