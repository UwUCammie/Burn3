#pragma once

#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu1_static_program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::guest {

// Compile-time identity for the garage A/B baseline.  The optimized default
// records only actual VI writes; the dense form is built in a separate binary
// when exact fixed-work evidence needs the pre-optimization reference.  This
// is intentionally a constexpr value so production execution has no mode
// lookup or runtime branch.
[[nodiscard]] constexpr bool vu1_dense_vi_branch_history_enabled() noexcept {
#if defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
    return true;
#else
    return false;
#endif
}

class EeMemory;

// VU1 is a standalone vector coprocessor in the same encoding as VU0 but with
// a larger on-chip memory (16 KiB MicroMem1 + 16 KiB VUMem1) and no macro
// (COP2) interface: the EE drives it purely through VIF1 (micro-program upload
// via MPG, execution start via MSCAL/MSCALF), and its results reach the GS
// through GIF path1. Unlike VU0, whose registers double as COP2 state inside
// R5900State, VU1's register file is private to the micro interpreter and is
// carried here so a single micro run can mutate and later resample it.
enum class Vu1RunStatus : std::uint8_t {
    Completed,
    InvalidEntryPoint,
    UnsupportedInstruction,
    UnsupportedOperandCase,
    PairLimitReached,
};

enum class Vu1Slot : std::uint8_t {
    None,
    Fetch,
    Lower,
    Upper,
};

struct Vu1RunOptions {
    std::size_t pair_limit{1U << 20U};
};

struct Vu1RunResult {
    Vu1RunStatus status{Vu1RunStatus::InvalidEntryPoint};
    Vu1Slot slot{Vu1Slot::None};
    std::uint16_t pc{};
    std::uint32_t lower_word{};
    std::uint32_t upper_word{};
    std::size_t committed_pairs{};
    std::size_t elapsed_cycles{};
    std::size_t pipeline_stall_cycles{};
};

// A VU1 program is identified by the complete 16 KiB MicroMem image and the
// 11-bit entry point.  Keeping the entry separate matters: the same uploaded
// image commonly contains several independently launched routines.
struct Vu1ProgramHash {
    std::uint64_t low{};
    std::uint64_t high{};

    [[nodiscard]] friend constexpr bool operator==(const Vu1ProgramHash&,
                                                   const Vu1ProgramHash&) noexcept = default;
};

struct Vu1State;

// Read-only context for the registration/probe ABI of a shared native block.
// A Declined callback must return before mutating any guest or machine state;
// this intentionally exposes no architectural pointers until the resumable
// machine façade has its own versioned contract.
class Vu1SharedNativeBlockContext {
public:
    [[nodiscard]] std::uint16_t pc() const noexcept { return pc_; }
    [[nodiscard]] std::uint16_t block_start() const noexcept { return block_start_; }
    [[nodiscard]] std::uint16_t block_pair_count() const noexcept {
        return block_pair_count_;
    }
    [[nodiscard]] std::size_t remaining_pair_budget() const noexcept {
        return remaining_pair_budget_;
    }

private:
    friend Vu1RunResult run_vu1_micro(Vu1State&, EeMemory&, std::uint16_t,
                                      Vu1RunOptions);
    Vu1SharedNativeBlockContext(const std::uint16_t pc, const std::uint16_t block_start,
                                const std::uint16_t block_pair_count,
                                const std::size_t remaining_pair_budget) noexcept
        : pc_(pc), block_start_(block_start), block_pair_count_(block_pair_count),
          remaining_pair_budget_(remaining_pair_budget) {}

    std::uint16_t pc_{};
    std::uint16_t block_start_{};
    std::uint16_t block_pair_count_{};
    std::size_t remaining_pair_budget_{};
};

// ABI-2 machine façade. Generated bodies do not own a pair cursor, recipe
// table, callback chain, or architectural state. Each compile-time request is
// checked against the live machine PC and is executed by that machine's
// canonical issue/stage/commit path. A body which cannot prove its complete
// range must return Declined before requesting a pair.
class Vu1SharedNativeBlockContextV2 {
public:
    [[nodiscard]] std::uint16_t pc() const noexcept { return pc_; }
    [[nodiscard]] std::uint16_t block_start() const noexcept { return block_start_; }
    [[nodiscard]] std::uint16_t block_pair_count() const noexcept {
        return block_pair_count_;
    }
    [[nodiscard]] std::size_t remaining_pair_budget() const noexcept {
        return remaining_pair_budget_;
    }
    [[nodiscard]] std::size_t pair_request_count() const noexcept {
        return pair_request_count_;
    }
    [[nodiscard]] bool pair_protocol_fault() const noexcept {
        return pair_protocol_fault_;
    }

    template <std::uint16_t ExpectedPc>
    [[nodiscard]] vu1_detail::Vu1SharedNativeBlockPairExit execute_pair() noexcept {
        return execute_pair_impl(ExpectedPc);
    }

private:
    using PairExecutor = vu1_detail::Vu1SharedNativeBlockPairExit (*)(
        void*, std::uint16_t) noexcept;

    friend Vu1RunResult run_vu1_micro(Vu1State&, EeMemory&, std::uint16_t,
                                      Vu1RunOptions);
    Vu1SharedNativeBlockContextV2(const Vu1SharedNativeBlockContextV2&) = delete;
    Vu1SharedNativeBlockContextV2& operator=(const Vu1SharedNativeBlockContextV2&) = delete;
    Vu1SharedNativeBlockContextV2(Vu1SharedNativeBlockContextV2&&) = delete;
    Vu1SharedNativeBlockContextV2& operator=(Vu1SharedNativeBlockContextV2&&) = delete;
    Vu1SharedNativeBlockContextV2(
        const std::uint16_t pc, const std::uint16_t block_start,
        const std::uint16_t block_pair_count, const std::size_t remaining_pair_budget,
        void* machine, const PairExecutor pair_executor) noexcept
        : pc_(pc), block_start_(block_start), block_pair_count_(block_pair_count),
          remaining_pair_budget_(remaining_pair_budget), machine_(machine),
          pair_executor_(pair_executor) {}

    [[nodiscard]] vu1_detail::Vu1SharedNativeBlockPairExit execute_pair_impl(
        const std::uint16_t expected_pc) noexcept {
        if (pair_terminated_ || pair_protocol_fault_) {
            pair_protocol_fault_ = true;
            return vu1_detail::Vu1SharedNativeBlockPairExit::ProtocolFault;
        }
        ++pair_request_count_;
        if (pair_executor_ == nullptr) {
            pair_protocol_fault_ = true;
            return vu1_detail::Vu1SharedNativeBlockPairExit::ProtocolFault;
        }
        const auto result = pair_executor_(machine_, expected_pc);
        if (result == vu1_detail::Vu1SharedNativeBlockPairExit::Terminated) {
            pair_terminated_ = true;
        } else if (result == vu1_detail::Vu1SharedNativeBlockPairExit::ProtocolFault) {
            pair_protocol_fault_ = true;
        }
        return result;
    }

    std::uint16_t pc_{};
    std::uint16_t block_start_{};
    std::uint16_t block_pair_count_{};
    std::size_t remaining_pair_budget_{};
    void* machine_{};
    PairExecutor pair_executor_{};
    std::size_t pair_request_count_{};
    bool pair_terminated_{};
    bool pair_protocol_fault_{};
};

struct Vu1ProgramProfile {
    Vu1ProgramHash hash{};
    std::uint16_t entry{};
    // Canonical first block reached by this launch. For the common VU1
    // dispatcher shape whose first instruction is JR VI15, this is the
    // launch-time VI15 target; otherwise it is equal to entry.
    std::uint16_t resolved_entry{};
    std::uint64_t runs{};
    std::uint64_t completed_runs{};
    std::uint64_t committed_pairs{};
    std::uint64_t elapsed_cycles{};
    std::uint64_t pipeline_stall_cycles{};
    std::uint64_t host_nanoseconds{};
    // Historical name retained for profile/API compatibility. This counts
    // launches which matched an exact registered descriptor, whether that
    // descriptor selected direct predecode, static blocks, or a native callback.
    std::uint64_t compiled_runs{};
    // Exact-entry launches which executed through the cached pair table. This
    // includes generated descriptors without a resolver/callback and runtime
    // hot promotions, but excludes static-block and native callbacks.
    std::uint64_t direct_predecoded_runs{};
    // Exact-entry launches which selected a generated immutable basic-block
    // resolver while retaining the shared scheduler/stager/commit path. Pair-
    // level hit/fallback counters reveal whether that resolver covered work.
    std::uint64_t static_block_runs{};
    // Exact-entry launches which actually entered a generated native callback.
    std::uint64_t native_executor_runs{};
};

inline constexpr std::size_t kVu1MicroProgramPairCount = 2048U;

// Why a launch that entered ahead-of-time translated code handed its
// remaining pairs to the interpreter at an exact pair boundary.
enum class Vu1AotDeoptReason : std::uint8_t {
    PairLimit,        // the remaining pair budget does not cover the next block
    DynamicTarget,    // JR/JALR target outside the translated candidate set
    UntranslatedPair, // instruction or control-flow shape the generator rejects
    VariantLimit,     // pipeline-variant cap reached while generating
    Count,
};

// Why a launch with a verified translation ran entirely in the interpreter.
// A decline happens before the first pair and leaves every state untouched.
enum class Vu1AotDeclineReason : std::uint8_t {
    ReferenceTier,    // the interpreter would use other upper kernels or tiers
    Profiling,        // per-pair profile counts were requested
    FloatEnvironment, // native-float/SIMD arithmetic off, non-default rounding,
                      // or flush-to-zero/masked underflow not in effect
    XgkickMode,       // anything but the delayed whole-packet XGKICK model
    PairLimit,        // the pair budget does not cover the first block
    StatusSticky,     // the sticky zero/sign STATUS bits are not both set yet
    Count,
};

// Which upper-slot implementation family the interpreter used for a launch.
// The two families are not arithmetically equivalent under
// TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC=1 (the reference switch stages ten
// upper families through scalar chop arithmetic which the per-pair stage
// thunks stage through native-float kernels), so anything that replays a
// launch as a reference for another executor must use the same family.
enum class Vu1ReferenceTier : std::uint8_t {
    Automatic,   // override only: follow the cache's own promotion state
    Switch,      // reference upper switch (lazy decode, or predecoded metadata
                 // without upper stages)
    UpperStages, // predecoded per-pair upper stage thunks (the tier an
                 // ahead-of-time translation reproduces)
    Compiled,    // a registered whole-program executor ran the launch
};

struct Vu1ProgramCacheStats {
    std::uint64_t micro_memory_write_notifications{};
    std::uint64_t generations_hashed{};
    std::uint64_t image_hits{};
    std::uint64_t image_misses{};
    std::uint64_t image_evictions{};
    std::uint64_t pairs_decoded{};
    std::uint64_t pairs_predecoded{};
    std::uint64_t control_flow_pairs{};
    std::uint64_t hot_promotions{};
    // Exact registered-descriptor matches. A match can select direct
    // predecode, a static-block resolver, or a native callback.
    std::uint64_t compiled_dispatches{};
    // Registered-descriptor matches which actually entered a native callback.
    std::uint64_t compiled_function_dispatches{};
    // Launches executed through a cached immutable pair table. These can come
    // from either a registered descriptor or a runtime hot promotion.
    std::uint64_t direct_predecoded_dispatches{};
    // Exact descriptor launches which selected generated basic-block
    // metadata while retaining the shared runtime scheduler and commit path.
    std::uint64_t static_block_dispatches{};
    // Resolver calls and successful block activations are counted separately
    // from pairs so profiles can distinguish fragmented control flow from
    // useful block coverage.
    std::uint64_t static_block_resolver_calls{};
    std::uint64_t static_block_resolver_hits{};
    std::uint64_t static_block_pairs{};
    std::uint64_t static_block_fallback_pairs{};
    // Validated generated blocks can admit a complete non-wrapping natural
    // range to the shared scheduler at once. These counters separate useful
    // range coverage from the historical per-pair static metadata counters.
    std::uint64_t static_block_range_lookups{};
    std::uint64_t static_block_range_activations{};
    std::uint64_t static_block_range_pairs{};
    std::uint64_t static_block_range_interior_fallback_pairs{};
    std::uint64_t static_block_range_short_fallback_pairs{};
    std::uint64_t static_block_range_budget_fallback_pairs{};
    // Includes validated emitted spans with an interior branch/E pair and
    // launches already carrying pending branch/E delay state. Unlike the
    // runtime-hot unexpected-PC invariant, this can be nonzero by design.
    std::uint64_t static_block_range_flow_fallback_pairs{};
    // Historical static-tier name retained for profile/output compatibility.
    // These pair counters cover both generated stage tables and the identical
    // immutable table attached to a runtime-hot direct predecode. They never
    // imply a static-block resolver lookup or dispatch.
    std::uint64_t static_upper_stage_hits{};
    std::uint64_t linear_scoreboard_pairs{};
    std::uint64_t minimal_execution_dispatches{};
    std::uint64_t local_inline_block_hits{};
    std::uint64_t local_inline_block_pairs{};
    std::uint64_t native_scheduled_blocks{};
    std::uint64_t native_scheduled_pairs{};
    std::uint64_t native_transfer_blocks{};
    std::uint64_t native_flag_liveness_blocks{};
    std::uint64_t native_control_pairs{};
    std::uint64_t native_control_fast_pairs{};
    std::uint64_t static_upper_stage_fallbacks{};
    // Complete admitted natural/static ranges may carry an exact cache-owned
    // lower-slot execution plan. Supported pairs bypass the large reference
    // lower-op switch while retaining the same scheduler and commit machine;
    // unsupported pairs fall back in place without replay.
    std::uint64_t opcode_threaded_lower_stage_hits{};
    std::uint64_t opcode_threaded_lower_stage_fallbacks{};
    // Every interpreter IBEQ/IBNE that reaches the lower-plan seam is
    // classified exactly once. A hit staged the opt-in compact branch plan; a
    // fallback retained the canonical switch, including lazy, gated,
    // bounded-range, and pending-flow execution. Native executors report their
    // own work and do not enter these interpreter counters.
    std::uint64_t branch_lower_plan_hits{};
    std::uint64_t branch_lower_plan_fallbacks{};
    // Every interpreter FMAND/FCGET that reaches the lower-plan seam is
    // classified exactly once. The aggregate and per-op counters make the
    // fixed B3IR A-to-C window auditable without changing architectural state.
    std::uint64_t flag_read_lower_plan_hits{};
    std::uint64_t flag_read_lower_plan_fallbacks{};
    std::uint64_t fmand_lower_plan_hits{};
    std::uint64_t fmand_lower_plan_fallbacks{};
    std::uint64_t fcget_lower_plan_hits{};
    std::uint64_t fcget_lower_plan_fallbacks{};
    // Every interpreter FCAND/FCOR that reaches the lower-plan seam is
    // classified exactly once. The aggregate and per-op counters expose the
    // fixed-window candidate without mixing it with FMAND/FCGET.
    std::uint64_t clip_predicate_lower_plan_hits{};
    std::uint64_t clip_predicate_lower_plan_fallbacks{};
    std::uint64_t fcand_lower_plan_hits{};
    std::uint64_t fcand_lower_plan_fallbacks{};
    std::uint64_t fcor_lower_plan_hits{};
    std::uint64_t fcor_lower_plan_fallbacks{};
    // Exact-image generated straight spans enter through one callback and
    // issue registration-validated fixed upper/lower recipes through the
    // authoritative pair machine. These counters expose every conservative
    // admission and resumable exit for differential performance work.
    std::uint64_t generated_span_lookups{};
    std::uint64_t generated_span_activations{};
    std::uint64_t generated_span_pairs{};
    std::uint64_t generated_span_recipe_mismatches{};
    std::uint64_t generated_span_budget_fallbacks{};
    std::uint64_t generated_span_pending_flow_fallbacks{};
    std::uint64_t generated_span_short_fallbacks{};
    std::uint64_t generated_span_partial_resumes{};
    std::uint64_t generated_span_terminal_exits{};
    // Registration-validated ABI-1 shared-native blocks expose a decline-only
    // probe. ABI-2 counters are separate so promotion evidence cannot be
    // confused with the legacy probe tier.
    std::uint64_t shared_native_block_lookups{};
    std::uint64_t shared_native_block_activations{};
    std::uint64_t shared_native_block_fallbacks{};
    std::uint64_t shared_native_block_v2_lookups{};
    std::uint64_t shared_native_block_v2_activations{};
    std::uint64_t shared_native_block_v2_declines{};
    std::uint64_t shared_native_block_v2_partial_exits{};
    std::uint64_t shared_native_block_v2_terminal_exits{};
    std::uint64_t shared_native_block_v2_protocol_faults{};
    // Runtime-hot direct predecode can walk an authoritative natural basic
    // block through a contiguous pointer cursor. These counters distinguish
    // useful cursor coverage from conservative indexed fallbacks without
    // changing the historical static-block classification above.
    std::uint64_t runtime_hot_block_range_lookups{};
    std::uint64_t runtime_hot_block_activations{};
    std::uint64_t runtime_hot_block_pairs{};
    std::uint64_t runtime_hot_block_interior_fallback_pairs{};
    std::uint64_t runtime_hot_block_short_fallback_pairs{};
    std::uint64_t runtime_hot_block_budget_fallback_pairs{};
    std::uint64_t runtime_hot_block_pending_flow_fallback_pairs{};
    std::uint64_t runtime_hot_block_flow_mismatch_fallback_pairs{};
    std::uint64_t background_predecode_requests{};
    std::uint64_t background_predecode_completions{};
    std::uint64_t background_predecode_activations{};
    std::uint64_t background_predecode_stale_results{};
    std::uint64_t background_predecode_queue_retries{};
    std::uint64_t profiles_dropped{};
    std::uint64_t hash_nanoseconds{};
    std::uint64_t decode_nanoseconds{};
    std::uint64_t execute_nanoseconds{};
    // Ahead-of-time translated launches (local TAKEDOWN_LOCAL_VU1_AOT_MODULE
    // builds only; all zero otherwise). An identity check compares the byte
    // ranges one translation depends on with a newly seen program image; a
    // miss leaves that image+entry on the interpreter. aot_launches counts
    // launches with a verified translation and is the sum of completed,
    // deoptimized, and declined launches.
    std::uint64_t aot_identity_checks{};
    std::uint64_t aot_identity_misses{};
    std::uint64_t aot_launches{};
    std::uint64_t aot_completed{};
    std::uint64_t aot_pairs{};
    std::array<std::uint64_t, static_cast<std::size_t>(Vu1AotDeoptReason::Count)>
        aot_deopts{};
    std::array<std::uint64_t, static_cast<std::size_t>(Vu1AotDeclineReason::Count)>
        aot_declines{};
};

// Diagnostic tap over the mutating half of the VU1 execution-service boundary
// (the calls a launch makes into the XGKICK/GIF/timing model). Verification
// tools compare the ordered trace of two executors; pure queries are omitted
// because they cannot change what EeMemory later produces.
enum class Vu1ServiceCall : std::uint8_t {
    SynchronizeTimedExecution,
    SubmitXgkick,
    AdvanceXgkick,
    FlushXgkick,
};

struct Vu1ServiceTraceEvent {
    Vu1ServiceCall call{};
    std::uint64_t argument{};
    std::uint64_t result{};

    [[nodiscard]] friend constexpr bool operator==(
        const Vu1ServiceTraceEvent&, const Vu1ServiceTraceEvent&) noexcept = default;
};

// Per-thread sink for launches started on the calling thread; nullptr (the
// default) binds the untraced services and costs one test per launch.
void set_vu1_service_trace(std::vector<Vu1ServiceTraceEvent>* sink) noexcept;

// Diagnostic fault injection for traced launches started on the calling
// thread: the `ordinal`-th XGKICK submission from now on is refused before it
// reaches EeMemory (and journaled with result 0), which stops that launch
// with UnsupportedOperandCase at the kick pair. The real producer refuses a
// kick only while an older one cannot be completed, a state verification
// tools cannot otherwise construct. 0 (the default) disables it; it only
// acts while a trace sink is installed.
void set_vu1_service_kick_refusal(std::uint32_t ordinal) noexcept;

// True when this build links a generated ahead-of-time VU1 module.
[[nodiscard]] bool vu1_aot_module_linked() noexcept;

// Execution marks of the linked module's block variants, one byte each, in
// the slot order of the generator's report. Empty unless the module was
// generated with --coverage (verification builds).
[[nodiscard]] std::span<const std::uint8_t> vu1_aot_coverage() noexcept;

// Compares the ahead-of-time arithmetic kernels with the interpreter's own
// scalar helpers on `vectors` random and edge-pattern operand sets. Returns a
// description of the first difference, or nothing (also when no module is
// linked).
[[nodiscard]] std::optional<std::string> vu1_aot_kernel_self_test(std::uint64_t seed,
                                                                  std::size_t vectors);

struct Vu1ProfiledProgramSnapshot {
    Vu1ProgramProfile profile{};
    std::vector<std::byte> micro_memory{};
    // Exact committed-PC histogram collected only while the reference/static
    // shared execution loop is active. It is intentionally profile-only and
    // lets the source generator choose profitable basic-block chains without
    // guessing from static reachability. A terminal native callback may leave
    // this histogram partially covered until its staging ABI reports PCs.
    std::array<std::uint64_t, kVu1MicroProgramPairCount> pair_counts{};
};

// Stable source-level handoff between the exact MicroMem dispatcher and a
// portable C++ implementation of one VU1 program entry. Compiled functions
// are built into the same executable (this is intentionally not a process ABI)
// and receive every piece of mutable architectural state explicitly.
inline constexpr std::uint32_t kVu1CompiledProgramAbiVersion = 2U;

class Vu1CompiledProgramContext {
public:
    using AdvanceCycles = void (*)(EeMemory& memory, std::uint64_t cycles) noexcept;

    Vu1CompiledProgramContext(Vu1State& state_value, EeMemory& memory_value,
                              const Vu1ProgramHash hash_value,
                              const std::uint16_t entry_value,
                              const std::uint16_t resolved_entry_value,
                              const Vu1RunOptions options_value,
                              const AdvanceCycles advance_cycles_service) noexcept
        : state(state_value), memory(memory_value), hash(hash_value), entry(entry_value),
          resolved_entry(resolved_entry_value), options(options_value),
          advance_cycles_(advance_cycles_service) {}

    // Compiled code must account for every issued or stalled VU cycle at the
    // same point as the reference executor. Keeping the private EeMemory
    // timing hook behind this service also avoids widening EeMemory's API.
    void advance_cycles(const std::uint64_t cycles) const noexcept {
        advance_cycles_(memory, cycles);
    }

    Vu1State& state;
    EeMemory& memory;
    Vu1ProgramHash hash{};
    std::uint16_t entry{};
    // Exact first routine selected by a launch-time JR VI15 trampoline. It is
    // equal to entry for programs without that dispatcher shape. Generated
    // callbacks are selected by both values and may therefore specialize the
    // real geometry routine without treating entry as a wildcard.
    std::uint16_t resolved_entry{};
    Vu1RunOptions options{};

private:
    AdvanceCycles advance_cycles_{};
};

using Vu1CompiledProgramExecutor = Vu1RunResult (*)(Vu1CompiledProgramContext& context);

// Generated local modules contain immutable exact-image descriptors for the
// predecoded-dispatch tier. Registration validates both the supplied hash and
// every byte before a descriptor can be selected; a mismatch always falls
// back to the ordinary cached interpreter. An optional executor upgrades that
// exact entry to portable compiled code; a null executor retains the existing
// predecoded-only behavior. Once an executor starts, its result is terminal:
// the dispatcher never retries through the interpreter after partial mutation.
struct Vu1CompiledProgramDescriptor {
    Vu1ProgramHash hash{};
    std::uint16_t entry{};
    // Omitted descriptors retain the original exact-entry behavior. Profile-
    // generated modules set this explicitly so multiple VI15-selected
    // routines in one MicroMem image can coexist without registration-order
    // dependence.
    std::optional<std::uint16_t> resolved_entry{};
    std::span<const std::byte> micro_memory{};
    // Optional generated immutable basic-block lookup. A hit supplies exact
    // predecoded metadata to the shared runtime executor; a miss falls back
    // in-place to the authoritative lazy decoder without restarting or
    // replaying any architectural work.
    vu1_detail::Vu1StaticBlockResolver static_block_resolver{};
    // Required when static_block_resolver is present and zero otherwise. This
    // validates the serialized CachedVu1Pair representation independently of
    // the source-level compiled-function ABI.
    std::uint32_t static_metadata_version{};
    // Optional exact-image straight-span resolver. It may coexist with static
    // block metadata, but never with a terminal whole-program executor. Its
    // independent ABI leaves both compiled ABI 2 and metadata schema 3 stable.
    vu1_detail::Vu1GeneratedSpanResolver generated_span_resolver{};
    std::uint32_t generated_span_abi_version{};
    // Distinct from both the terminal compiled-function ABI and the prepared
    // span ABI. The resolver is validated against the complete exact image;
    // the current probe executor must decline before guest mutation.
    vu1_detail::Vu1SharedNativeBlockResolver shared_native_block_resolver{};
    std::uint32_t shared_native_block_abi_version{};
    // ABI 2 is intentionally a separate field so ABI 1 remains decline-only.
    // The machine-owned pair façade is currently opt-in and fail-closed.
    vu1_detail::Vu1SharedNativeBlockResolverV2 shared_native_block_resolver_v2{};
    std::uint32_t shared_native_block_abi_version_v2{};
    Vu1CompiledProgramExecutor executor{};
    std::uint32_t abi_version{kVu1CompiledProgramAbiVersion};
};

// Per-EeMemory ownership is intentional: it makes cache lifetime and
// invalidation follow the corresponding emulated MicroMem instead of relying
// on process-global pointer identities.
class Vu1ProgramCache {
public:
    Vu1ProgramCache();
    ~Vu1ProgramCache();
    Vu1ProgramCache(Vu1ProgramCache&&) noexcept;
    Vu1ProgramCache& operator=(Vu1ProgramCache&&) noexcept;
    Vu1ProgramCache(const Vu1ProgramCache&) = delete;
    Vu1ProgramCache& operator=(const Vu1ProgramCache&) = delete;

    void notify_micro_memory_write() noexcept;
    void notify_untracked_micro_memory_view() noexcept;
    // Absent once a mutable MicroMem span has escaped, even after clear().
    // Consumers may cache byte comparisons only while this revision is tracked.
    [[nodiscard]] std::optional<std::uint64_t> tracked_micro_memory_generation() const noexcept;
    void clear() noexcept;
    void set_profiling_enabled(bool enabled) noexcept;
    [[nodiscard]] bool profiling_enabled() const noexcept;
    void set_hot_run_threshold(std::size_t runs) noexcept;
    // Diagnostic A/B switch for automatically promoted runtime-hot programs.
    // Generated exact descriptors retain their validated upper-stage tables.
    void set_runtime_hot_upper_stages_enabled(bool enabled) noexcept;
    // Diagnostic A/B switch for full-range admission of validated generated
    // blocks. Disabling it retains exact static metadata and upper-stage
    // dispatch but feeds one pair at a time through the shared executor.
    void set_static_block_ranges_enabled(bool enabled) noexcept;
    // Diagnostic A/B switch for cache-owned natural-block batching. It affects
    // only runtime-hot direct predecode; generated descriptors, the lazy
    // interpreter, and all architectural scheduling remain unchanged.
    void set_runtime_hot_natural_blocks_enabled(bool enabled) noexcept;
    void set_linear_scoreboard_enabled(bool enabled) noexcept;
    // Per-pair diagnostic counters are optional; architectural/profile counts
    // and activation counters remain available in the minimal path.
    void set_execution_statistics_enabled(bool enabled) noexcept;
    // Diagnostic A/B switch for cache-owned lower-slot execution plans on
    // complete admitted natural/static ranges. Provider batching and all
    // architectural scheduling remain enabled when this is disabled.
    void set_opcode_threaded_lower_stages_enabled(bool enabled) noexcept;
    // Setup-time diagnostic A/B switch for the bounded simple VI ALU
    // extension. Set it before registering or executing any program; immutable
    // predecode jobs snapshot the selected mode. Existing lower plans remain
    // enabled when this is disabled; IAND/IOR stay on the canonical lower-op
    // switch. These plans are interpreter-only and never enter the generated
    // prepared-span ABI.
    void set_simple_alu_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool simple_alu_lower_plans_enabled() const noexcept;
    // Setup-time diagnostic A/B switch for the interpreter-only ILW lower
    // plan. Set it before registering or executing any program; immutable
    // predecode jobs snapshot the selected mode. The prepared-span ABI stays
    // unchanged because this plan is intentionally not generated.
    void set_ilw_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool ilw_lower_plans_enabled() const noexcept;
    // Setup-time diagnostic A/B switch for the interpreter-only ISW lower
    // plan. Set it before registering or executing any program; immutable
    // predecode jobs snapshot the selected mode. The prepared-span ABI stays
    // unchanged because this plan is intentionally not generated.
    void set_isw_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool isw_lower_plans_enabled() const noexcept;
    // Setup-time diagnostic A/B switch for interpreter-only IBEQ/IBNE lower
    // plans. Set it before registering or executing any program; changing a
    // candidate switch later invalidates all plan-bearing metadata as one
    // six-flag identity. The broad opcode-threaded lower-stage switch must
    // also remain enabled for a plan to execute. Comparisons still use the
    // common delayed-VI history, pending-target state, and pair commit path.
    void set_branch_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool branch_lower_plans_enabled() const noexcept;
    // Setup-time diagnostic A/B switch for interpreter-only FMAND/FCGET lower
    // plans. The dynamic four-cycle MAC/CLIP visibility values are supplied by
    // the common execution machine; the prepared/generated ABI remains
    // unchanged. Changing the switch invalidates the complete lower-plan
    // configuration identity and rejects stale worker results.
    void set_flag_read_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool flag_read_lower_plans_enabled() const noexcept;
    // Setup-time diagnostic A/B switch for interpreter-only FCAND/FCOR lower
    // plans. They consume the live four-cycle CLIP value at the shared pair
    // seam and always write fixed VI1; the prepared/generated ABI remains
    // unchanged. Changing the switch invalidates all lower-plan metadata.
    void set_clip_predicate_lower_plans_enabled(bool enabled) noexcept;
    [[nodiscard]] bool clip_predicate_lower_plans_enabled() const noexcept;
    // Diagnostic A/B switch for validated generated prepared spans. This
    // experimental tier defaults off until it beats the natural-range path;
    // disabling it retains static metadata and all ordinary tiers unchanged.
    void set_generated_spans_enabled(bool enabled) noexcept;
    // Ahead-of-time translated launches (see docs in vu1_aot_runtime.inc).
    // Defaults to enabled unless TAKEDOWN_VU1_AOT is exactly "0"; has no
    // effect in builds without a linked module. A disabled cache is the
    // reference executor for differential comparison in the same process.
    void set_aot_enabled(bool enabled) noexcept;
    [[nodiscard]] bool aot_enabled() const noexcept;
    // Upper-slot family of the most recent launch (an ahead-of-time launch
    // reports UpperStages, the tier it reproduces). Differential tools hand
    // it to their reference executor through the override below.
    [[nodiscard]] Vu1ReferenceTier last_launch_tier() const noexcept;
    // Forces the upper-slot family of subsequent launches on this cache:
    // UpperStages predecodes the launched image+entry synchronously, Switch
    // ignores predecoded metadata. Automatic (the default) restores the
    // cache's own hot-promotion policy. Reference executors only.
    void set_reference_tier_override(Vu1ReferenceTier tier) noexcept;
    // Tooling/test synchronization. Normal execution waits for the worker at
    // exactly one point: the launch after a hot-promotion request for the
    // same image, so the launch at which a program leaves the reference
    // switch never depends on host thread timing.
    void wait_for_background_predecode();
    [[nodiscard]] std::span<const Vu1ProgramProfile> profiles() const noexcept;
    [[nodiscard]] std::vector<Vu1ProfiledProgramSnapshot> profiled_program_snapshots() const;
    [[nodiscard]] Vu1ProgramCacheStats statistics() const noexcept;
    [[nodiscard]] bool
    register_compiled_program(const Vu1CompiledProgramDescriptor& descriptor);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend Vu1RunResult run_vu1_micro(Vu1State&, EeMemory&, std::uint16_t,
                                      Vu1RunOptions);
};

// VU1's architectural register file. COP2 never touches these (that is VU0);
// the interpreter owns them for the lifetime of a micro run.
struct Vu1State {
    std::array<Gpr128, 32> vectors{};
    Gpr128 accumulator{};
    // Registers 0..15 are the integer (VI) bank; 16..31 the control bank.
    std::array<std::uint32_t, 32> control{};
    // FMAC, CLIP, and STATUS are four-pair delayed.  PCSX2 retains these
    // four visibility slots across VU1 dispatcher exits, and retail programs
    // may read them immediately after a later MSCAL/MSCNT launch.  Slot zero
    // is visible to the first pair of a run; slots one through three become
    // visible on the following pairs.
    std::array<std::uint32_t, 4> mac_flag_history{};
    std::array<std::uint32_t, 4> clip_flag_history{};
    std::array<std::uint32_t, 4> status_flag_history{};
    bool flag_history_initialized{};

    [[nodiscard]] Gpr128 read_vector(std::size_t index) const noexcept {
        return index == 0U ? Gpr128{.words = {0U, 0U, 0U, 0x3f800000U}} : vectors[index];
    }
    // Read-only execution stages can retain a reference until the first
    // architectural mutation of the referenced register.
    // Keeping that contract explicit avoids copying all four lanes for upper
    // instructions which consume only one component. VF0 uses immutable storage
    // because its architectural value is synthesized rather than backed by the
    // mutable vector array.
    [[nodiscard]] const Gpr128& read_vector_ref(std::size_t index) const noexcept {
        static constexpr Gpr128 kVectorZero{.words = {0U, 0U, 0U, 0x3f800000U}};
        return index == 0U ? kVectorZero : vectors[index];
    }
    void write_vector(std::size_t index, const Gpr128& value) noexcept {
        if (index != 0U) {
            vectors[index] = value;
        }
    }
    [[nodiscard]] Gpr128 read_accumulator() const noexcept { return accumulator; }
    [[nodiscard]] const Gpr128& read_accumulator_ref() const noexcept { return accumulator; }
    void set_accumulator(const Gpr128& value) noexcept { accumulator = value; }
    [[nodiscard]] std::uint16_t read_integer(std::size_t index) const noexcept {
        return index == 0U ? 0U : static_cast<std::uint16_t>(control[index]);
    }
    void write_integer(std::size_t index, std::uint16_t value) noexcept {
        if (index != 0U) {
            control[index] = value;
        }
    }
    [[nodiscard]] std::uint32_t read_control(std::size_t index) const noexcept {
        return index == 0U ? 0U : control[index];
    }
    void write_control(std::size_t index, std::uint32_t value) noexcept {
        if (index != 0U) {
            control[index] = value;
            if (index >= 16U && index <= 18U) {
                // An architectural flag write establishes a new launch
                // baseline.  The interpreter writes its retained histories
                // directly when committing a run so those four slots are not
                // accidentally collapsed here.
                flag_history_initialized = false;
            }
        }
    }
    void initialize_flag_history_from_controls() noexcept {
        status_flag_history.fill(control[16U]);
        mac_flag_history.fill(control[17U]);
        clip_flag_history.fill(control[18U]);
        flag_history_initialized = true;
    }
    void reset() noexcept {
        vectors.fill({});
        accumulator = {};
        control.fill(0U);
        mac_flag_history.fill(0U);
        clip_flag_history.fill(0U);
        status_flag_history.fill(0U);
        flag_history_initialized = false;
    }
};

// Exact compiled first tier for programs whose upper slots are NOP and whose
// non-literal lower slots are NOP. Generated modules select it only after
// validating the complete 16 KiB image; all other programs retain the full
// predecoded interpreter.
[[nodiscard]] Vu1RunResult execute_vu1_compiled_nop_program(
    Vu1CompiledProgramContext& context,
    std::span<const std::byte> exact_micro_memory);

// Exact compiled tier for straight-line control programs with upper NOPs and
// lower NOP/I-bit literals or VI integer ALU operations. The module builder
// proves that the complete reachable path belongs to this subset before it
// attaches the callback; complex geometry programs remain on the semantic
// reference executor until their operations have generated implementations.
[[nodiscard]] Vu1RunResult execute_vu1_compiled_linear_integer_program(
    Vu1CompiledProgramContext& context,
    std::span<const std::byte> exact_micro_memory);

[[nodiscard]] Vu1RunResult run_vu1_micro(Vu1State& state, EeMemory& memory, std::uint16_t entry,
                                         Vu1RunOptions options = {});
[[nodiscard]] std::string_view to_string(Vu1RunStatus status) noexcept;
[[nodiscard]] std::string_view to_string(Vu1Slot slot) noexcept;

} // namespace takedown::guest
