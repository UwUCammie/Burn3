#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/recompiler/r5900_function_cfg.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takedown::recompiler {

// Opt-in bounded compiled-CFG region for the FE_JoinDrainPump hot prefix.
// Callers retain scheduler policy and resume scalar/ordinary dispatch after
// every exit.
struct JoinPumpRegionBudget {
    std::uint32_t maximum_retired_instructions{};
    std::uint32_t maximum_ee_cycles{};
};

using JoinPumpRegionProgressCallback = bool (*)(void* context, std::uint32_t retired,
                                                std::uint32_t ee_cycles);

struct JoinPumpRegionProgressCheckpoint {
    JoinPumpRegionProgressCallback callback{};
    void* context{};
    // The scheduler may retain control inside a bounded batch only when its
    // ordinary loop-boundary work cannot progress. Null preserves all exits.
    bool (*continue_loop)(void*, const guest::R5900State&, const guest::EeMemory&){};

    [[nodiscard]] explicit operator bool() const noexcept { return callback != nullptr; }
};

enum class JoinPumpRegionExit : std::uint8_t {
    YieldBeforeBoundary,
    YieldBeforeJalr,
    BudgetExhausted,
    ProgressCheckpoint,
    CodeMismatch,
    Fault,
    Count,
};

struct JoinPumpRegionResult {
    JoinPumpRegionExit exit{JoinPumpRegionExit::YieldBeforeBoundary};
    std::uint32_t retired_instructions{};
    std::uint32_t ee_cycles{};
    std::optional<guest::EeStepStatus> fault{};
};

class JoinPumpRegion {
  public:
    static constexpr guest::GuestAddress kEntryAddress = 0x001f24e0U;
    static constexpr guest::GuestAddress kCallbackAddress = 0x00228040U;
    // The legacy admitted source is the exact 13-word prefix through JALR at
    // 0x001f2510. The extended form also admits its delay slot and the exact
    // callback leaf at 0x00228040. The separately gated callback-tail form
    // additionally admits the post-return BNE and its LUI delay slot, then
    // yields at the resolved target before either another loop iteration or
    // the following DI at 0x001f2520.
    static constexpr std::size_t kSourceWordCount = 13U;
    static constexpr std::size_t kExtendedCallerSourceWordCount = 14U;
    static constexpr std::size_t kCallbackTailCallerSourceWordCount = 16U;
    static constexpr std::size_t kCallbackSourceWordCount = 12U;
    static constexpr guest::GuestAddress kCallbackEffectEndAddress =
        kCallbackAddress + static_cast<guest::GuestAddress>(kCallbackSourceWordCount * 4U);
    static constexpr guest::GuestAddress kExtendedCallerEffectEndAddress =
        kEntryAddress +
        static_cast<guest::GuestAddress>(kExtendedCallerSourceWordCount * 4U);
    static constexpr guest::GuestAddress kCallbackTailEffectEndAddress =
        kEntryAddress +
        static_cast<guest::GuestAddress>(kCallbackTailCallerSourceWordCount * 4U);
    // Exclusive legacy runner-admission boundary. The final legacy source word
    // is JALR and remains live-validated, but the single-span form yields before
    // executing it.
    static constexpr guest::GuestAddress kDirectEffectEndAddress =
        kEntryAddress + static_cast<guest::GuestAddress>((kSourceWordCount - 1U) * 4U);
    static constexpr std::uint32_t kMaximumRetiredInstructions = 32U;
    static constexpr std::uint32_t kMaximumLoopRetiredInstructions = 1024U;

    // Accept only a single explicitly provided bounded executable span. The
    // candidate is structurally decoded at admission; no retail bytes live in
    // committed source. Per-entry live source validation compares current RAM
    // against this caller-provided source before every direct effect.
    [[nodiscard]] static std::optional<JoinPumpRegion>
    from_words(R5900ExecutableWordSpan source) noexcept;

    // Add the caller JALR delay slot and the exact callback leaf as a second
    // independently extracted executable span. This form may cross the
    // indirect call only when V0 is the reviewed callback address. Both spans
    // remain live-validated before every direct effect.
    [[nodiscard]] static std::optional<JoinPumpRegion>
    from_words(R5900ExecutableWordSpan caller_source,
               R5900ExecutableWordSpan callback_source) noexcept;

    // Add the exact post-return BNE and delay-slot LUI after the callback
    // return. This form is separately gated so the callback promotion retains
    // its existing boundary until the tail has passed its own proof gates.
    [[nodiscard]] static std::optional<JoinPumpRegion>
    from_words(R5900ExecutableWordSpan caller_source,
               R5900ExecutableWordSpan callback_source,
               bool callback_tail_enabled) noexcept;

    // Directly executes the admitted caller operations and, for the extended
    // form, its exact JALR, callback leaf, store, and return. It live-validates
    // every instruction before its effect and yields on an incompatible entry,
    // changed source, budget cut, or unsupported memory boundary. No
    // CompiledBlockDispatcher or dispatch_one call occurs inside this body.
    [[nodiscard]] JoinPumpRegionResult
    execute(guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
            JoinPumpRegionProgressCheckpoint progress_checkpoint = {}) const;

    // Select constant-operand execution of the already validated source shape.
    // Live code, budget, fault, and per-instruction progress checks are shared.
    void enable_specialized_execution(bool enabled) noexcept { specialized_execution_ = enabled; }

    // Caller-owned, single-execution-context cache. Tokens bind validation to
    // one memory domain and invalidate on tracked writes or escaped views.
    // Exact source snapshots also prevent reuse for a different admitted shape.
    struct IdleCodeValidationCache {
        std::uint64_t hits{};
        std::uint64_t misses{};
      private:
        friend class JoinPumpRegion;
        std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words{};
        std::array<std::uint32_t, kCallbackSourceWordCount> callback_words{};
        guest::EeMainRamCodePageToken caller_page{}, callback_page{};
        bool validated{};
    };

    // Summarize complete no-store iterations. The caller must prove that no
    // device/scheduler event or observer can see intermediate state over the
    // supplied budget, then service devices at the returned endpoint.
    // Zero means the exact live shape/state was not eligible; no effects occur.
    [[nodiscard]] std::uint32_t summarize_idle_iterations(
        guest::R5900State& cpu, guest::EeMemory& memory,
        const JoinPumpRegionBudget& budget, IdleCodeValidationCache* code_cache = nullptr) const;

    // Temporary compatibility adapter for the native front-end wait operation.
    // Advances graphics at complete no-store polls (21/24 cycles), rather than
    // individual instructions. External scheduler deadlines must still bound
    // the supplied budget. This deliberately changes internal service timing;
    // it requires final-output validation and is not the exact reference lane.
    [[nodiscard]] JoinPumpRegionResult execute_native_wait(
        guest::R5900State& cpu, guest::EeMemory& memory,
        const JoinPumpRegionBudget& budget, JoinPumpRegionProgressCheckpoint checkpoint,
        IdleCodeValidationCache& code_cache) const;

  private:
    template <bool Specialized>
    [[nodiscard]] JoinPumpRegionResult execute_impl(
        guest::R5900State& cpu, guest::EeMemory& memory, const JoinPumpRegionBudget& budget,
        JoinPumpRegionProgressCheckpoint progress_checkpoint) const;
    explicit JoinPumpRegion(
        std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words,
        std::array<std::uint32_t, kCallbackSourceWordCount> callback_words, bool callback_enabled,
        bool callback_tail_enabled) noexcept;

    std::array<std::uint32_t, kCallbackTailCallerSourceWordCount> caller_words_{};
    std::array<std::uint32_t, kCallbackSourceWordCount> callback_words_{};
    std::array<guest::DecodedR5900Instruction, kCallbackTailCallerSourceWordCount>
        caller_instructions_{};
    std::array<guest::DecodedR5900Instruction, kCallbackSourceWordCount> callback_instructions_{};
    bool callback_enabled_{};
    bool callback_tail_enabled_{};
    bool specialized_execution_{};
};

} // namespace takedown::recompiler
