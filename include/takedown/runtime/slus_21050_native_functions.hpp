#pragma once

#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime {

class NativeFunctionDispatcher;

namespace slus_21050 {

struct SubmitPlainTextLinesTransportClaimBinding;

// First promoted, register-independent helpers. The dispatcher adapters below
// translate between the EE ABI and these readable C++ operations.
[[nodiscard]] guest::GuestAddress get_global_runtime_state() noexcept;
[[nodiscard]] guest::GuestAddress
get_inline_utf16_buffer(guest::GuestAddress object_address) noexcept;

// Reads the byte stored at object+8. Several retail controller/action-device
// call sites use this tiny leaf, but the concrete object type remains unnamed
// until more of that hierarchy is recovered. GuestAddress arithmetic wraps in
// the same 32-bit domain as the EE LBU effective-address calculation.
[[nodiscard]] std::uint8_t
read_object_byte_at_offset_8(const guest::EeMemory& memory,
                             guest::GuestAddress object_address);

// Raw EE single-precision absolute value. This is deliberately a bit-pattern
// operation rather than host floating-point arithmetic: signed zero,
// denormals, infinities, and NaN payloads all retain their retail encodings
// with only the sign bit cleared.
[[nodiscard]] std::uint32_t absolute_float_bits(std::uint32_t bits) noexcept;

// Raw EE single-precision floor. Keeping this as a bit-pattern operation
// avoids host rounding-mode and libm differences, preserves NaN payloads and
// signed zero on the retail fast paths, and mirrors the PS2's treatment of
// denormal encodings without converting them on the host.
[[nodiscard]] std::uint32_t floor_float_bits(std::uint32_t bits) noexcept;

// Shared body of compiler-emitted constructor callbacks used by the
// retail vector-construction helper. Their element types remain intentionally
// unnamed until their users are recovered: both complete constructors leave
// storage untouched and return the incoming raw EE `this` scalar, including
// its non-address high word.
[[nodiscard]] std::uint64_t
trivial_array_element_constructor(std::uint64_t raw_object_scalar) noexcept;

// Conservatively named identity body for a newly reached retail placement
// constructor whose concrete object type has not yet been recovered. Like the
// compiler-emitted array callbacks above, it leaves storage untouched and
// returns the incoming raw EE `this` scalar without canonicalizing aliases.
[[nodiscard]] std::uint64_t
trivial_placement_constructor(std::uint64_t raw_object_scalar) noexcept;

// Readable high-level body used by the reviewed startup profile of the retail
// optimized memset. The dispatcher adapter proves alignment and ordinary
// main-RAM residency before calling it.
void fill_aligned_8192_bytes(guest::EeMemory& memory,
                             guest::GuestAddress destination,
                             std::uint8_t value);

// Conservatively named body of the reviewed element-vector constructor. It
// preserves the three raw input words and canonicalizes W to positive-zero
// bits; no host floating-point conversion is involved.
[[nodiscard]] std::array<std::uint32_t, 4U>
initialize_element_vector_w_zero(guest::EeMemory& memory,
                                 guest::GuestAddress destination);

// The retail movie controller embeds this small owned-string state. Its
// constructor deliberately initializes only byte zero, the optional owned
// pointer, and the four-byte inline capacity; the other bytes remain intact.
void construct_small_owned_string(guest::EeMemory& memory,
                                  guest::GuestAddress object_address);

// Compiler-emitted static-destruction bookkeeping. GuestAddress keeps the
// stored node/object/callback values raw: KSEG and default-TLB aliases must not
// be canonicalized before they are written into the retail list.
struct StaticDestructorRegistration {
    guest::GuestAddress object{};
    guest::GuestAddress destructor{};
    guest::GuestAddress node{};
};

[[nodiscard]] guest::GuestAddress register_static_destructor(
    guest::EeMemory& memory,
    const StaticDestructorRegistration& registration);

// One resolved SDK SIF command-handler slot. The retail table stores a
// callback, its opaque argument, and the registering module's GP so the SIF
// dispatcher can enter callbacks compiled with a different small-data base.
struct SifCommandHandlerRegistration {
    std::uint32_t command_id{};
    guest::GuestAddress table_pointer_field{};
    guest::GuestAddress table{};
    guest::GuestAddress entry{};
    std::uint32_t byte_offset{};
    guest::GuestAddress handler{};
    guest::GuestAddress argument{};
    guest::GuestAddress global_pointer{};
    bool system{};

    bool operator==(const SifCommandHandlerRegistration&) const = default;
};

struct SifCommandHandlerCommit {
    guest::GuestAddress table{};
    guest::GuestAddress entry{};

    bool operator==(const SifCommandHandlerCommit&) const = default;
};

// Resolves only an in-range aligned record in direct EE RAM, disjoint from
// the live SDK command-data block and this retail function's code. The SDK
// itself trusts its caller; unproved IDs and aliases remain guest work.
[[nodiscard]] std::optional<SifCommandHandlerRegistration>
plan_sif_command_handler_registration(
    const guest::EeMemory& memory, std::uint32_t command_id,
    guest::GuestAddress handler, guest::GuestAddress argument,
    guest::GuestAddress global_pointer);

// Revalidates a plan before mutation, then preserves the retail write order:
// argument, handler, table-pointer reload, and finally registering GP.
[[nodiscard]] SifCommandHandlerCommit
commit_sif_command_handler_registration(
    guest::EeMemory& memory,
    const SifCommandHandlerRegistration& registration);

// Typed view of one VIF packet payload. Keeping the unit in the type prevents
// the allocator's qword counts from being confused with byte lengths at the
// high-level boundary.
struct VifPacketQwordRange {
    guest::GuestAddress begin{};
    std::uint32_t quadword_count{};

    bool operator==(const VifPacketQwordRange&) const = default;
};

// The side-effect-free plan for the verified cold path through the retail VIF
// packet allocator at 0x001f3080. The four extra qwords and 128-byte backing
// alignment are part of the retail allocation contract, not host padding.
struct ColdVifPacketReservation {
    VifPacketQwordRange output;
    guest::GuestAddress backing_begin{};
    guest::GuestAddress backing_end{};
    guest::GuestAddress descriptor_cursor{};
    std::uint32_t reserved_quadwords{};

    bool operator==(const ColdVifPacketReservation&) const = default;
};

// Returns no plan unless the live allocator is in the fully reviewed cold
// state and both its scratchpad output and ordinary-RAM backing ranges fit.
// Busy/flush/descriptor-refill states intentionally remain interpreter work.
[[nodiscard]] std::optional<ColdVifPacketReservation>
plan_cold_vif_packet_reservation(const guest::EeMemory& memory,
                                 std::uint32_t payload_quadwords);

// Commits exactly the five allocator globals written by that cold path. A
// stale or forged plan is rejected before any guest memory is changed.
void commit_cold_vif_packet_reservation(
    guest::EeMemory& memory, const ColdVifPacketReservation& reservation);

// The flush-path cold allocation used by the retail ring flush at 0x21a4f0:
// its caller passes the 0x80000000 system flag, which grows the retail
// reservation to six header qwords and makes the allocator leave the
// system-initiated state 0x80001101, a descriptor header at the backing
// begin (0x4e2828), and a backing cursor advanced by one qword.
struct FlushColdPacketReservation {
    VifPacketQwordRange output;
    guest::GuestAddress backing_begin{};
    guest::GuestAddress backing_end{};
    guest::GuestAddress descriptor_cursor{};
    std::uint32_t reserved_quadwords{};

    bool operator==(const FlushColdPacketReservation&) const = default;
};

// The caller-visible register residue of that allocator cold path: the flush
// caller reads A0/A1/V1 after the call returns, so the promotion republishes
// them exactly.
struct FlushColdPacketResidue {
    std::uint32_t v1{};
    std::uint32_t a0{};
    std::uint32_t a1{};
};

[[nodiscard]] std::optional<FlushColdPacketReservation>
plan_flush_cold_packet_reservation(const guest::EeMemory& memory,
                                   std::uint32_t payload_quadwords);

[[nodiscard]] FlushColdPacketResidue
commit_flush_cold_packet_reservation(
    guest::EeMemory& memory, const FlushColdPacketReservation& reservation);

// A side-effect-free description of the reviewed VIF packet-ring drain. The
// pending source may be canonical direct main RAM or the canonical EE
// scratchpad window. The raw cursor is retained separately because retail
// rounds an eight-byte cursor residue up when deriving the qword count.
// Exact costs use plain integers here so this semantic API does not depend on
// the dispatcher ABI.
struct VifPacketRingFlushPlan {
    VifPacketQwordRange pending;
    guest::GuestAddress raw_cursor{};
    guest::GuestAddress current_word{};
    std::uint32_t current_word_value{};
    FlushColdPacketReservation reservation;
    std::uint32_t group_iterations{};
    std::uint32_t tail_quadwords{};
    std::uint32_t exact_retired_instructions{};
    std::uint32_t exact_ee_cycles{};

    bool operator==(const VifPacketRingFlushPlan&) const = default;
};

// Semantic residue after a successful ring drain. The current packet word is
// marked with END before the pending qwords are copied, the allocator output
// cursor advances past the copy, and the ring state returns to its eleven-row
// post-flush form.
struct VifPacketRingFlushResult {
    VifPacketQwordRange drained;
    VifPacketQwordRange output;
    guest::GuestAddress raw_cursor{};
    guest::GuestAddress output_end{};
    std::uint32_t ended_current_word{};
    std::uint32_t active_rows{};

    bool operator==(const VifPacketRingFlushResult&) const = default;
};

// Resolves only the verified cold-allocator path with an exact retail
// allocator fingerprint. Main-RAM rings allow both the established external
// current-word shape and the in-ring shape used by the environment packet
// composite; a scratchpad ring requires that word to live wholly inside the
// pending source. In either region aliases, wrapping, and a scratch source
// overlapping the allocator output are rejected.
[[nodiscard]] std::optional<VifPacketRingFlushPlan>
plan_vif_packet_ring_flush(const guest::EeMemory& memory);

// Revalidates the complete plan before the first store, then commits the END
// marker, cold reservation, qword copy, and post-flush globals in retail
// order. A stale or forged plan throws std::invalid_argument without mutation.
[[nodiscard]] VifPacketRingFlushResult
commit_vif_packet_ring_flush(guest::EeMemory& memory,
                             const VifPacketRingFlushPlan& plan);

namespace detail {

// Internal composition seam for a caller that has just revalidated a larger
// atomic plan containing this exact flush plan. These functions deliberately
// skip a second stale check so a composite cannot fail after an earlier store.
// The ABI form expects SP and RA to hold their retail call-site values.
[[nodiscard]] std::optional<VifPacketRingFlushPlan> plan_vif_packet_ring_flush_from_snapshot(
    const guest::EeMemory& memory, guest::GuestAddress ring, guest::GuestAddress raw_cursor,
    guest::GuestAddress current_word, std::uint32_t current_word_value);
[[nodiscard]] VifPacketRingFlushResult
commit_vif_packet_ring_flush_prevalidated(
    guest::EeMemory& memory, const VifPacketRingFlushPlan& plan);
[[nodiscard]] VifPacketRingFlushResult
apply_vif_packet_ring_flush_ee_abi_prevalidated(
    std::array<guest::Gpr128, 32U>& registers, guest::EeMemory& memory,
    const VifPacketRingFlushPlan& plan);

} // namespace detail

// Copies complete 128-bit packet units from direct EE RAM to scratchpad. This
// deliberately rejects aliases and overlapping/incorrectly mapped ranges;
// callers outside the proven profile must use the guest implementation.
void copy_vif_packet_quadwords(guest::EeMemory& memory,
                               VifPacketQwordRange destination,
                               VifPacketQwordRange source);

// Raw endpoint vectors consumed by the retail fxspark ring appender. They are
// kept as bits because VU0 normalizes denormals and exponent-255 encodings in
// a way that a host float API would not preserve.
struct SparkEndpointPair {
    std::array<std::uint32_t, 4U> first{};
    std::array<std::uint32_t, 4U> second{};

    bool operator==(const SparkEndpointPair&) const = default;
};

// One fully resolved slot in the four-descriptor fxspark particle ring. The
// names describe verified storage roles without guessing whether every caller
// interprets the two endpoint vectors as positions, velocities, or segment
// endpoints.
struct SparkParticleRingAppend {
    guest::GuestAddress emitter{};
    guest::GuestAddress bank{};
    guest::GuestAddress descriptor{};
    guest::GuestAddress group{};
    guest::GuestAddress scalar_destination{};
    guest::GuestAddress first_endpoint_destination{};
    guest::GuestAddress second_endpoint_destination{};
    std::uint32_t descriptor_index{};
    std::uint32_t old_count{};
    std::uint32_t incremented_count{};
    std::uint32_t stored_count{};
    std::uint32_t lane{};
    bool alternate_bank{};
    bool wrapped{};

    bool operator==(const SparkParticleRingAppend&) const = default;
};

// Resolves only the verified, well-formed retail ring profile: a direct,
// aligned emitter bank; one of four descriptors; a positive capacity; and a
// non-negative in-range count backed by aligned ordinary RAM. No guest state
// is changed when the profile is rejected.
[[nodiscard]] std::optional<SparkParticleRingAppend>
plan_spark_particle_ring_append(const guest::EeMemory& memory,
                                guest::GuestAddress emitter,
                                bool alternate_bank);

// Rechecks and commits one planned append in retail store order. Endpoint
// lanes pass through the exact VU0 add-with-zero normalization performed by
// 0x00263f30..0x00263f4c; Y is duplicated into W in each stored vector.
void commit_spark_particle_ring_append(
    guest::EeMemory& memory, const SparkParticleRingAppend& append,
    const SparkEndpointPair& endpoints, std::uint32_t scalar_bits);

} // namespace slus_21050

// Registers the reviewed high-level replacements for the US retail ELF. The
// baseline set is all-or-nothing and uses exact instruction signatures. The
// Packet-copy, vertex-batch, text-glyph, plain-text, and outlined-text promotions are
// independently optional: each is registered only when its complete retail
// fingerprints match.
// False means that no baseline function was registered because a signature
// differed.
[[nodiscard]] bool register_slus_21050_native_functions(
    NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);
[[nodiscard]] bool register_slus_21050_native_functions(
    NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory,
    slus_21050::SubmitPlainTextLinesTransportClaimBinding plain_text_transport);

inline constexpr std::size_t kSlus21050BaselineNativeFunctionCount = 31U;
// Maximum inventory when all seven optional promotions match the local ELF.
inline constexpr std::size_t kSlus21050NativeFunctionCount = 38U;

} // namespace takedown::runtime
