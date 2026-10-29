#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_native_functions.hpp"
#include "takedown/runtime/slus_21050_stream_environment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::guest {

class EeMemory;
class R5900State;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kStreamEnvironmentPacketAddress = 0x00219830U;
inline constexpr std::size_t kStreamEnvironmentPacketBodyBytes = 496U;
inline constexpr std::uint64_t kStreamEnvironmentPacketFingerprint = 0xfb1139b59671afaaULL;
inline constexpr std::uint32_t kStreamEnvironmentPacketMaximumElementCount = 254U;

// Register-independent guest inputs to the retail environment-packet
// streamer. Every pointer is a canonical, direct main-RAM address. The two
// rectangle streams each contain element_count consecutive 16-byte vectors.
struct StreamEnvironmentPacketGuestInput {
    guest::GuestAddress header_attribute_vector{};
    guest::GuestAddress translation_xy{};
    guest::GuestAddress position_rectangles{};
    guest::GuestAddress texture_rectangles{};
    std::uint32_t element_count{};

    bool operator==(const StreamEnvironmentPacketGuestInput&) const = default;
};

enum class StreamEnvironmentPacketCapacityMode : std::uint8_t {
    ExistingRoom,
    FlushThenStream,
};

enum class StreamEnvironmentPacketHeaderMode : std::uint8_t {
    CreateForActiveRows,
    CreateAfterDescriptorMismatch,
    ReuseMatchingDescriptor,
};

enum class StreamEnvironmentPacketCursorMode : std::uint8_t {
    Unchanged,
    AdvanceByEight,
};

// Complete values read from caller-owned storage and the retail packet
// constants. Raw binary32 bits are retained even when two different inputs
// produce the same descriptor or fixed-point output.
struct StreamEnvironmentPacketSourceSnapshot {
    RawVector4Bits header_attribute_vector;
    StreamEnvironmentTranslationBits translation;
    std::vector<StreamEnvironmentPositionRectangleBits> positions;
    std::vector<StreamEnvironmentTextureRectangleBits> textures;
    RawVector4Bits descriptor_scale;
    RawVector4Bits global_position_offset;
    RawVector4Bits header_template_a;
    std::uint64_t header_template_b{};
    RawVector4Bits header_template_c;

    bool operator==(const StreamEnvironmentPacketSourceSnapshot&) const = default;
};

// Entry values of every packet global consulted by the reviewed body. The
// current header is retained even on fresh paths so an otherwise invisible
// concurrent state change invalidates the atomic plan.
struct StreamEnvironmentPacketGlobalSnapshot {
    guest::GuestAddress ring_begin{};
    guest::GuestAddress packet_cursor{};
    guest::GuestAddress current_header{};
    std::uint32_t active_rows{};
    std::uint64_t header_descriptor{};
    std::uint32_t packet_tag{};

    bool operator==(const StreamEnvironmentPacketGlobalSnapshot&) const = default;
};

// Side-effect-free resolution of capacity, header selection, cursor repair,
// packet bytes, and exact scheduler cost. A flush plan is complete and is
// committed only after this entire object has been reproduced exactly.
struct StreamEnvironmentPacketGuestPlan {
    StreamEnvironmentPacketGuestInput input;
    StreamEnvironmentPacketCapacityMode capacity_mode{
        StreamEnvironmentPacketCapacityMode::ExistingRoom};
    StreamEnvironmentPacketHeaderMode header_mode{
        StreamEnvironmentPacketHeaderMode::CreateForActiveRows};
    StreamEnvironmentPacketCursorMode cursor_mode{StreamEnvironmentPacketCursorMode::Unchanged};
    StreamEnvironmentPacketSourceSnapshot sources;
    StreamEnvironmentPacketGlobalSnapshot globals;
    RawVector4Bits position_offset;
    std::uint64_t descriptor{};
    std::optional<RawVector4Bits> existing_header_snapshot;
    std::vector<EncodedStreamEnvironmentElement> encoded_elements;
    VifPacketQwordRange entry_pending;
    guest::GuestAddress stream_packet_cursor{};
    VifPacketQwordRange packet_output;
    VifPacketQwordRange header_output;
    VifPacketQwordRange element_output;
    guest::GuestAddress packet_end{};
    std::optional<VifPacketRingFlushPlan> flush_plan;
    std::uint64_t function_fingerprint{};
    std::uint32_t exact_retired_instructions{};
    std::uint32_t exact_ee_cycles{};

    bool operator==(const StreamEnvironmentPacketGuestPlan&) const = default;
};

struct StreamEnvironmentPacketGuestResult {
    StreamEnvironmentPacketCapacityMode capacity_mode{
        StreamEnvironmentPacketCapacityMode::ExistingRoom};
    StreamEnvironmentPacketHeaderMode header_mode{
        StreamEnvironmentPacketHeaderMode::CreateForActiveRows};
    StreamEnvironmentPacketCursorMode cursor_mode{StreamEnvironmentPacketCursorMode::Unchanged};
    VifPacketQwordRange packet_output;
    VifPacketQwordRange header_output;
    VifPacketQwordRange element_output;
    guest::GuestAddress packet_end{};
    std::uint64_t descriptor{};
    std::uint32_t header_element_count{};
    std::uint32_t element_count{};
    std::optional<VifPacketRingFlushResult> flush_result;

    bool operator==(const StreamEnvironmentPacketGuestResult&) const = default;
};

namespace detail {

// Complete bytes of the live three-qword environment header. Projected
// packet calls carry these values forward so a later compatible call can
// validate and reuse a header that has not been written to EeMemory yet.
struct StreamEnvironmentPacketHeaderSnapshot {
    guest::GuestAddress address{};
    RawVector4Bits prefix_a;
    std::uint64_t prefix_b{};
    std::uint64_t descriptor{};
    RawVector4Bits header;

    bool operator==(const StreamEnvironmentPacketHeaderSnapshot&) const = default;
};

// Mutable packet-ring state consumed by one projected call. current_word is
// retained independently of live_header because the flush path only reads the
// first word selected by the current-header global. A second flush is outside
// the reviewed cold-allocator profile and is rejected during projection.
struct StreamEnvironmentPacketProjectedState {
    StreamEnvironmentPacketGlobalSnapshot globals;
    std::optional<std::uint32_t> current_word;
    std::optional<StreamEnvironmentPacketHeaderSnapshot> live_header;
    bool prior_flush{};

    bool operator==(const StreamEnvironmentPacketProjectedState&) const = default;
};

struct StreamEnvironmentPacketPersistentSourceRange {
    guest::GuestAddress begin{};
    std::size_t length{};

    bool operator==(const StreamEnvironmentPacketPersistentSourceRange&) const = default;
};

// Address-free semantic packet operation used by aggregate guest
// transactions. Unlike StreamEnvironmentPacketGuestPlan, this does not claim
// that owned values currently exist at transient EE argument addresses.
struct StreamEnvironmentPacketProjectedPlan {
    StreamEnvironmentPacketCapacityMode capacity_mode{
        StreamEnvironmentPacketCapacityMode::ExistingRoom};
    StreamEnvironmentPacketHeaderMode header_mode{
        StreamEnvironmentPacketHeaderMode::CreateForActiveRows};
    StreamEnvironmentPacketCursorMode cursor_mode{StreamEnvironmentPacketCursorMode::Unchanged};
    StreamEnvironmentPacketSourceSnapshot sources;
    StreamEnvironmentPacketGlobalSnapshot globals;
    RawVector4Bits position_offset;
    std::uint64_t descriptor{};
    std::optional<RawVector4Bits> existing_header_snapshot;
    std::vector<EncodedStreamEnvironmentElement> encoded_elements;
    VifPacketQwordRange entry_pending;
    guest::GuestAddress stream_packet_cursor{};
    VifPacketQwordRange packet_output;
    VifPacketQwordRange header_output;
    VifPacketQwordRange element_output;
    guest::GuestAddress packet_end{};
    std::optional<VifPacketRingFlushPlan> flush_plan;
    std::uint64_t function_fingerprint{};
    std::uint32_t element_count{};
    std::uint32_t exact_retired_instructions{};
    std::uint32_t exact_ee_cycles{};

    bool operator==(const StreamEnvironmentPacketProjectedPlan&) const = default;
};

struct StreamEnvironmentPacketProjectedStep {
    StreamEnvironmentPacketProjectedPlan plan;
    StreamEnvironmentPacketProjectedState next_state;

    bool operator==(const StreamEnvironmentPacketProjectedStep&) const = default;
};

// Reads only the entry packet globals and any directly addressable current
// header. It does not validate a particular packet call.
[[nodiscard]] std::optional<StreamEnvironmentPacketProjectedState>
snapshot_stream_environment_packet_state(const guest::EeMemory& memory);

// Plans from owned source values and explicit mutable packet state. The
// caller separately lists persistent guest ranges that packet and flush writes
// must not overlap; transient values need no invented EE address. Fixed retail
// constants and code are still checked against memory.
[[nodiscard]] std::optional<StreamEnvironmentPacketProjectedStep> project_stream_environment_packet(
    const guest::EeMemory& memory, StreamEnvironmentPacketSourceSnapshot sources,
    std::span<const StreamEnvironmentPacketPersistentSourceRange> persistent_sources,
    const StreamEnvironmentPacketProjectedState& state);

// Applies a packet plan already covered by a larger transaction's complete
// stale-plan comparison.
[[nodiscard]] StreamEnvironmentPacketGuestResult
commit_stream_environment_packet_prevalidated(guest::EeMemory& memory,
                                              const StreamEnvironmentPacketGuestPlan& plan);

[[nodiscard]] StreamEnvironmentPacketGuestResult
commit_stream_environment_packet_prevalidated(guest::EeMemory& memory,
                                              const StreamEnvironmentPacketProjectedPlan& plan);

// Replays the complete EE calling-convention effects of one packet call after
// a containing transaction has validated the corresponding semantic plan.
// The register array holds the live GPR file at the retail call site; A0-A3,
// T0, SP, and RA must therefore already contain that call's values, including
// an A2 low word equal to the plan's element count. SP must be canonical and
// 16-byte aligned. Before its first write, the containing transaction must
// prove [SP-0xc0, SP) mapped and disjoint from every source, packet output,
// structural range, and code range; a flush plan expands that window to
// [SP-0x110, SP) and includes its pending, current-word, and allocation ranges.
// The helper updates caller-visible GPR, FPR, VU0, MAC-history, stack, and
// packet-memory residue, but leaves publishing the GPR array and return PC to
// its caller.
[[nodiscard]] StreamEnvironmentPacketGuestResult
apply_stream_environment_packet_ee_abi_prevalidated(std::array<guest::Gpr128, 32U>& registers,
                                                    guest::R5900State& cpu, guest::EeMemory& memory,
                                                    const StreamEnvironmentPacketGuestPlan& plan);

[[nodiscard]] StreamEnvironmentPacketGuestResult
apply_stream_environment_packet_ee_abi_prevalidated(
    std::array<guest::Gpr128, 32U>& registers, guest::R5900State& cpu, guest::EeMemory& memory,
    const StreamEnvironmentPacketProjectedPlan& plan);

} // namespace detail

[[nodiscard]] std::optional<StreamEnvironmentPacketGuestPlan>
plan_stream_environment_packet_from_guest(const guest::EeMemory& memory,
                                          const StreamEnvironmentPacketGuestInput& input);

// Replans and exact-compares before the first store. A stale, forged, aliased,
// or differently fingerprinted plan throws std::invalid_argument atomically.
[[nodiscard]] StreamEnvironmentPacketGuestResult
commit_stream_environment_packet(guest::EeMemory& memory,
                                 const StreamEnvironmentPacketGuestPlan& plan);

} // namespace takedown::runtime::slus_21050
