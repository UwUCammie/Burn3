#pragma once

#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace takedown::guest {

struct Vu1State;
class Vu1GeneratedSpanContext;
class Vu1SharedNativeBlockContext;
class Vu1SharedNativeBlockContextV2;

namespace vu1_detail {

using Vu1GeneratedSpanContext = ::takedown::guest::Vu1GeneratedSpanContext;

// Repo-internal, image-derived VU1 schedule metadata shared by the reference
// executor and the portable static-program builder. This is deliberately not
// architectural mutable state: generated code may embed these immutable
// records, while all timing and commit rules remain in Vu1ExecutionMachine.
enum class UpperOp : std::uint8_t {
    Nop,
    AddLane, SubLane, MaddLane, MsubLane, MulLane,
    MaxLane, MiniLane, MaxI, MiniI,
    MulQ, MulI, AddQ, AddI, SubI,
    MaddQ, MaddI, SubQ, MsubQ, MsubI,
    Add, Madd, Mul, Max, Sub, Msub, Opmsub, Mini,
    AddaLane, SubaLane, MaddaLane, MsubaLane, MulaLane,
    Mula, OpMula, Adda, Madda, Suba, Msuba,
    MulaQ, MulaI, AddaQ, AddaI, MaddaQ, MaddaI,
    SubaQ, SubaI, MsubaQ, MsubaI,
    Vitof, Vftoi, Abs, Clip,
};

struct DecodedUpper {
    UpperOp op{UpperOp::Nop};
    std::uint8_t lane{};
    std::uint8_t ft{};
    std::uint8_t fs{};
    std::uint8_t fd{};
    std::uint8_t fraction{};
};

enum class LowerOp : std::uint8_t {
    Nop,
    Lq, Sq, Isw, Ilw,
    Iaddiu, Isubiu, Fceq, Fcset, Fcand, Fcor, Fcget, Fmand, Fmor, Fmeq,
    B, Bal, Jr, Jalr, Ibeq, Ibne, Iblt, Ibgt, Ible, Ibge,
    Iadd, Isub, Iaddi, Iand, Ior,
    Move, Mr32, Lqi, Lqd, Sqi, Xtop, Xitop, Xgkick, Sqrt, Rsqrt, Div, Waitq,
    Ercpr, Esqrt, Ersqrt, Mfp, Waitp, Mtir, Mfir,
};

struct DecodedLower {
    LowerOp op{LowerOp::Nop};
    std::uint8_t mask{};
    std::uint8_t it{};
    std::uint8_t is{};
    std::uint8_t ft{};
    std::uint8_t fs{};
    std::uint8_t fd{};
    std::int16_t immediate{};
    std::uint32_t immediate24{};
};

enum class CachedVu1Flow : std::uint8_t {
    Fallthrough,
    DirectBranch,
    ConditionalBranch,
    DynamicBranch,
};

struct CachedVu1Pair {
    static constexpr std::size_t kMaximumVfReadLanes = 16U;

    std::uint32_t lower_word{};
    std::uint32_t upper_word{};
    std::optional<DecodedUpper> upper{};
    std::optional<DecodedLower> lower{};
    std::array<std::uint8_t, kMaximumVfReadLanes> vf_read_lanes{};
    std::uint8_t vf_read_lane_count{};
    std::array<std::uint8_t, 4U> upper_vf_write_lanes{};
    std::array<std::uint8_t, 4U> lower_vf_write_lanes{};
    std::uint8_t upper_vf_write_lane_count{};
    std::uint8_t lower_vf_write_lane_count{};
    std::uint16_t vi_read_mask{};
    std::uint16_t direct_branch_target{};
    std::uint16_t unsigned_immediate15{};
    std::uint8_t destination_mask{};
    CachedVu1Flow flow{CachedVu1Flow::Fallthrough};
    bool i_bit{};
    bool e_bit{};
    bool lower_decoded{};
    bool lower_uses_fdiv_pipe{};
    bool lower_uses_efu_pipe{};
    bool exits_after_delay{};
    bool initialized{};
};

// Upper-slot effects staged from the immutable pre-pair register state. Both
// generated thunks and the ordinary reference switch populate this same frame;
// the execution machine remains the sole owner of architectural commit order.
struct Vu1UpperStageEffects {
    Gpr128 upper_fd{};
    std::uint32_t upper_mask{};
    bool upper_writes_fd{};
    bool upper_writes_acc{};
    bool upper_writes_clip{};
    bool upper_updates_mac{};
    std::uint32_t upper_clip{};
    std::uint16_t upper_flags{};
    std::uint32_t upper_underflow_mask{};
    std::uint32_t upper_overflow_mask{};
};

using Vu1UpperStageThunk = void (*)(const Vu1State& state, std::uint32_t q_bits,
                                    std::uint32_t logical_clip,
                                    const CachedVu1Pair& pair,
                                    Vu1UpperStageEffects& effects) noexcept;

void stage_vu1_upper_madd_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                               const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_madda_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                                const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_add_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                              const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_sub_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                              const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_msub_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                               const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_mula_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                               const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_mul(const Vu1State&, std::uint32_t, std::uint32_t,
                         const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_add(const Vu1State&, std::uint32_t, std::uint32_t,
                         const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_sub(const Vu1State&, std::uint32_t, std::uint32_t,
                         const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_madd(const Vu1State&, std::uint32_t, std::uint32_t,
                          const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_msub(const Vu1State&, std::uint32_t, std::uint32_t,
                          const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_opmula(const Vu1State&, std::uint32_t, std::uint32_t,
                            const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_opmsub(const Vu1State&, std::uint32_t, std::uint32_t,
                            const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_adda_lane(const Vu1State&, std::uint32_t, std::uint32_t,
                               const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_adda(const Vu1State&, std::uint32_t, std::uint32_t,
                          const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_madda(const Vu1State&, std::uint32_t, std::uint32_t,
                           const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_mul_q(const Vu1State&, std::uint32_t, std::uint32_t,
                           const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_madd_q(const Vu1State&, std::uint32_t, std::uint32_t,
                            const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_abs(const Vu1State&, std::uint32_t, std::uint32_t,
                         const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_vitof(const Vu1State&, std::uint32_t, std::uint32_t,
                           const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_clip(const Vu1State&, std::uint32_t, std::uint32_t,
                          const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;
void stage_vu1_upper_vftoi(const Vu1State&, std::uint32_t, std::uint32_t,
                           const CachedVu1Pair&, Vu1UpperStageEffects&) noexcept;

// Separately versioned source ABI for generated straight-line prepared spans.
// It deliberately does not expose Vu1ExecutionMachine, VUMem, scheduler state,
// or timing. Generated code can only request one exact, registration-validated
// pair recipe at a time through issue_vu1_prepared_pair below.
inline constexpr std::uint32_t kVu1GeneratedSpanAbiVersion = 1U;

enum class Vu1PreparedLowerKind : std::uint8_t {
    Noop,
    Iaddiu,
    LqFull,
    LqMasked,
    SqFull,
    SqMasked,
    Mr32Full,
    Mr32Masked,
};

struct Vu1PreparedPairRecipe {
    // nullptr is an exact upper NOP identity, not a request to enter the
    // reference upper switch. Every non-NOP recipe names one supported stage.
    Vu1UpperStageThunk upper_stage{};
    Vu1PreparedLowerKind lower_kind{Vu1PreparedLowerKind::Noop};

    [[nodiscard]] friend constexpr bool operator==(
        const Vu1PreparedPairRecipe&,
        const Vu1PreparedPairRecipe&) noexcept = default;
};

template <Vu1UpperStageThunk UpperStage, Vu1PreparedLowerKind LowerKind>
[[nodiscard]] bool issue_vu1_prepared_pair(
    Vu1GeneratedSpanContext& context) noexcept;

using Vu1GeneratedSpanExecutor = void (*)(Vu1GeneratedSpanContext& context) noexcept;

// Registration-only resolver payload. The recipes may be transient; accepted
// identities and leader mappings are copied into cache-owned storage before
// the descriptor can execute.
struct Vu1GeneratedSpan {
    std::uint16_t start_pc{};
    std::span<const Vu1PreparedPairRecipe> recipes{};
    Vu1GeneratedSpanExecutor executor{};
};

using Vu1GeneratedSpanResolver =
    Vu1GeneratedSpan (*)(std::uint16_t pc) noexcept;

// Separately versioned handoff for a future generated native basic-block body.
// Registration owns the immutable block geometry and executor identity; the
// runtime remains responsible for constructing the machine context. The first
// implementation deliberately exposes only a decline-capable probe contract:
// a body which cannot prove a complete machine implementation must return
// Declined before mutating guest state so the canonical executor can continue
// at the same PC. The ABI will be extended (and versioned) when the machine
// façade can express issue/commit and resumable exits.
inline constexpr std::uint32_t kVu1SharedNativeBlockAbiVersion = 1U;

enum class Vu1SharedNativeBlockExit : std::uint8_t {
    Declined,
};

using Vu1SharedNativeBlockExecutor =
    Vu1SharedNativeBlockExit (*)(Vu1SharedNativeBlockContext&) noexcept;

struct Vu1SharedNativeBlock {
    std::uint16_t start_pc{};
    std::uint16_t pair_count{};
    Vu1SharedNativeBlockExecutor executor{};
};

using Vu1SharedNativeBlockResolver =
    Vu1SharedNativeBlock (*)(std::uint16_t pc) noexcept;

// ABI 1 remains a decline-only registration probe. ABI 2 is deliberately a
// distinct source contract: a body may request exact pairs through the
// machine-owned façade and must report the resulting bounded exit explicitly.
// Keeping the versioned types separate prevents an ABI-1 callback compiled
// against the old one-value enum from becoming a mutating executor by accident.
inline constexpr std::uint32_t kVu1SharedNativeBlockAbiVersion2 = 2U;

enum class Vu1SharedNativeBlockPairExit : std::uint8_t {
    Continued,
    Partial,
    Terminated,
    ProtocolFault,
};

enum class Vu1SharedNativeBlockExitV2 : std::uint8_t {
    Declined,
    Continued,
    Partial,
    Terminated,
    ProtocolFault,
};

using Vu1SharedNativeBlockExecutorV2 =
    Vu1SharedNativeBlockExitV2 (*)(Vu1SharedNativeBlockContextV2&) noexcept;

struct Vu1SharedNativeBlockV2 {
    std::uint16_t start_pc{};
    std::uint16_t pair_count{};
    Vu1SharedNativeBlockExecutorV2 executor{};
};

using Vu1SharedNativeBlockResolverV2 =
    Vu1SharedNativeBlockV2 (*)(std::uint16_t pc) noexcept;

// Generated modules serialize CachedVu1Pair rather than decoding it at load
// time. Keep that derived representation independently versioned from the
// compiled-function ABI: any enum-layout, decoder, dependency, or scheduling
// metadata change must bump this value so a stale local artifact is rejected
// instead of executing correct instruction bytes with obsolete metadata.
inline constexpr std::uint32_t kVu1StaticPairMetadataVersion = 3U;

// A generated static module may group pair metadata into natural basic blocks.
// The resolver is registration-only input: registration queries every 11-bit
// PC, validates the complete mapping against an authoritative decode, and
// copies accepted pairs/stages into cache-owned immutable storage. Returned
// spans therefore need remain valid only for the registration call. All
// mutable scheduling, latency, control-flow, and commit behavior remains in
// the runtime's authoritative Vu1ExecutionMachine.
struct Vu1StaticBasicBlock {
    std::uint16_t start_pc{};
    std::span<const CachedVu1Pair> pairs{};
    // Empty retains metadata-only execution. Otherwise this is exactly
    // parallel to pairs; nullptr entries use the authoritative upper switch.
    std::span<const Vu1UpperStageThunk> upper_stages{};
};

using Vu1StaticBlockResolver =
    Vu1StaticBasicBlock (*)(std::uint16_t pc) noexcept;

// Pure 16 KiB-image + pair-PC decode. The caller supplies exactly one complete
// MicroMem image and an in-range 11-bit PC. It never observes VU registers,
// VUMem, GIF state, or guest clocks and is therefore safe for both offline
// source generation and background predecode.
[[nodiscard]] CachedVu1Pair decode_vu1_pair_metadata(
    std::span<const std::byte> micro_memory, std::uint16_t pc);

} // namespace vu1_detail
} // namespace takedown::guest
