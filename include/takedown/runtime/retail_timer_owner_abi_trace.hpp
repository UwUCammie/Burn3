#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {

// This is the exact SLUS-21050 owner routine discussed by the reviewed
// function map. The range includes the JR delay slot. Its dynamic control
// flow may revisit body addresses, so instruction counts are recorded from
// the executed path rather than inferred from the 107 static words.
inline constexpr guest::GuestAddress kRetailTimerOwnerEntry = 0x001d3a60U;
inline constexpr guest::GuestAddress kRetailTimerOwnerReturn = 0x001d3c04U;
inline constexpr guest::GuestAddress kRetailTimerOwnerReturnDelay = 0x001d3c08U;
inline constexpr std::size_t kRetailTimerOwnerFunctionBytes = 428U;
inline constexpr std::uint64_t kRetailTimerOwnerFunctionFingerprint = 0x456b09679e7f3436ULL;
inline constexpr std::uint64_t kRetailTimerOwnerRawMemoryFingerprint = 0x06e22dc8694fdedbULL;
inline constexpr std::size_t kRetailTimerOwnerWitnessBytes = 64U;
inline constexpr std::size_t kRetailTimerOwnerMaximumPathEntries = 512U;
// Keep a bounded artifact sample while validating and counting every
// invocation in the measured interval. The reviewed A-to-C window has 24
// owner calls, so 64 records cover the current evidence without making the
// trace unbounded.
inline constexpr std::size_t kRetailTimerOwnerMaximumInvocations = 64U;

// The reviewed-function fingerprint is domain separated from a raw memory
// hash. Its input is the literal domain (including its NUL), the start and
// size as little-endian u32 values, then the exact 428 live bytes.
[[nodiscard]] std::uint64_t retail_timer_owner_function_fingerprint(
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint64_t retail_timer_owner_raw_memory_fingerprint(
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint64_t retail_timer_owner_live_function_fingerprint(
    const guest::EeMemory& memory) noexcept;

enum class RetailTimerOwnerAbiTraceFailure : std::uint8_t {
    None,
    NoCompleteInvocation,
    FingerprintMismatch,
    MalformedEntry,
    OffBody,
    NestedEntry,
    NestedControlFlow,
    Syscall,
    MissingReturnDelay,
    ReturnInstructionMismatch,
    ReturnDelayMismatch,
    InstructionNotExecuted,
    CounterMismatch,
    PathOverflow,
    InvocationOverflow,
    MemoryRangeInvalid,
    IncompleteInvocation,
    HleBoundary,
    SchedulerBoundary,
    ContextChanged,
    PublicationFailed,
};

[[nodiscard]] std::string_view to_string(RetailTimerOwnerAbiTraceFailure failure) noexcept;

struct RetailTimerOwnerAbiTraceContext {
    std::int32_t thread_id{};
    bool callback_active{};
    std::uint64_t callback_identity{};

    [[nodiscard]] constexpr bool operator==(
        const RetailTimerOwnerAbiTraceContext&) const noexcept = default;
};

class RetailTimerOwnerAbiTrace {
  public:
    static constexpr std::size_t kWitnessBytes = kRetailTimerOwnerWitnessBytes;
    static constexpr std::size_t kMaximumPathEntries = kRetailTimerOwnerMaximumPathEntries;

    struct PathEntry {
        guest::GuestAddress pc{};
        std::uint32_t word{};
        std::uint64_t guest_steps{};
        std::uint64_t ee_cycles{};
    };

    struct Invocation {
        guest::R5900State entry_state{};
        guest::R5900State return_jr_state{};
        guest::R5900State return_state{};
        RetailTimerOwnerAbiTraceContext context{};
        std::uint64_t entry_guest_steps{};
        std::uint64_t return_guest_steps{};
        std::uint64_t entry_ee_cycles{};
        std::uint64_t return_ee_cycles{};
        std::uint64_t retired_instructions{};
        std::uint64_t ee_cycles{};
        std::uint64_t path_digest{};
        std::uint32_t return_jr_word{};
        std::uint32_t return_delay_word{};
        std::uint32_t object_base{};
        std::uint32_t global_base{};
        std::uint32_t stack_base{};
        bool object_witness_valid{};
        bool global_witness_valid{};
        bool stack_witness_valid{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> object_entry{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> object_return{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> global_entry{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> global_return{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> stack_entry{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> stack_return{};
        std::array<PathEntry, kRetailTimerOwnerMaximumPathEntries> path{};
        std::size_t path_count{};
    };

    struct Summary {
        bool enabled{};
        bool published{};
        bool passed{};
        bool gate_applicable{};
        bool collection_opened{};
        bool collection_completed{};
        std::uint64_t collection_start_guest_steps{};
        std::uint64_t collection_start_ee_cycles{};
        std::uint64_t collection_stop_guest_steps{};
        std::uint64_t collection_end_guest_steps{};
        std::uint64_t collection_end_ee_cycles{};
        std::filesystem::path output_path{};
        RetailTimerOwnerAbiTraceFailure failure{
            RetailTimerOwnerAbiTraceFailure::None};
        std::uint64_t function_fingerprint{};
        std::uint64_t raw_memory_fingerprint{};
        std::uint64_t attempted_entries{};
        std::uint64_t completed_invocations{};
        std::uint64_t retained_invocations{};
        std::uint64_t dropped_invocations{};
        std::uint64_t completed_path_digest{};
        std::uint64_t observed_instructions{};
        std::uint64_t observed_cycles{};
        std::uint64_t off_body_rejections{};
        std::uint64_t nested_rejections{};
        std::uint64_t syscall_rejections{};
        std::uint64_t missing_delay_rejections{};
        std::uint64_t context_rejections{};
        std::uint64_t scheduler_boundaries{};
    };

    // An empty path leaves the trace entirely disabled. No native dispatcher
    // registration is associated with this type.
    explicit RetailTimerOwnerAbiTrace(std::filesystem::path output_path = {});
    ~RetailTimerOwnerAbiTrace() = default;

    RetailTimerOwnerAbiTrace(const RetailTimerOwnerAbiTrace&) = delete;
    RetailTimerOwnerAbiTrace& operator=(const RetailTimerOwnerAbiTrace&) = delete;
    RetailTimerOwnerAbiTrace(RetailTimerOwnerAbiTrace&&) = delete;
    RetailTimerOwnerAbiTrace& operator=(RetailTimerOwnerAbiTrace&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool collecting() const noexcept { return collecting_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
        return output_path_;
    }

    // The collection gate is explicit so callers can prove that the whole
    // observed interval was interpreter-only guest work.
    void begin_guest_work_collection(std::uint64_t guest_steps,
                                     std::uint64_t ee_cycles,
                                     std::uint64_t stop_guest_steps = 0U) noexcept;
    void complete_guest_work_collection(std::uint64_t guest_steps,
                                        std::uint64_t ee_cycles) noexcept;

    void observe_pre_instruction(const guest::R5900State& state,
                                 const guest::EeMemory& memory,
                                 std::uint64_t guest_steps,
                                 std::uint64_t ee_cycles,
                                 RetailTimerOwnerAbiTraceContext context = {}) noexcept;
    void observe_post_instruction(const guest::R5900State& state,
                                  const guest::EeMemory& memory,
                                  const guest::EeStepResult& result,
                                  std::uint64_t guest_steps,
                                  std::uint64_t ee_cycles,
                                  RetailTimerOwnerAbiTraceContext context = {}) noexcept;

    // These explicit boundaries let the runner prove that scheduler seams
    // preserve the owner context, even when no scalar instruction is retired.
    void observe_hle_boundary() noexcept;
    void observe_scheduler_boundary(RetailTimerOwnerAbiTraceContext context) noexcept;

    [[nodiscard]] Summary finalize() noexcept;
    [[nodiscard]] Summary summary() const noexcept { return summary_; }
    [[nodiscard]] const std::vector<Invocation>& invocations() const noexcept {
        return invocations_;
    }

  private:
    struct Witnesses {
        std::uint32_t object_base{};
        std::uint32_t global_base{};
        std::uint32_t stack_base{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> object{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> global{};
        std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> stack{};
    };

    void fail(RetailTimerOwnerAbiTraceFailure failure) noexcept;
    [[nodiscard]] bool capture_witnesses(const guest::R5900State& state,
                                         const guest::EeMemory& memory,
                                         Witnesses& witnesses) const noexcept;
    [[nodiscard]] bool in_body(guest::GuestAddress pc) const noexcept;
    [[nodiscard]] bool read_word(const guest::EeMemory& memory,
                                 guest::GuestAddress pc,
                                 std::uint32_t& word) const noexcept;
    void append_path(const guest::R5900State& state,
                     guest::GuestAddress pc,
                     std::uint32_t word,
                     std::uint64_t guest_steps,
                     std::uint64_t ee_cycles) noexcept;
    void write_artifact() noexcept;

    bool enabled_{};
    bool collecting_{};
    bool collection_completed_{};
    bool active_{};
    bool pending_instruction_{};
    bool return_jr_seen_{};
    bool finalized_{};
    std::filesystem::path output_path_{};
    std::uint64_t stop_guest_steps_{};
    std::uint64_t last_guest_steps_{};
    std::uint64_t last_ee_cycles_{};
    std::uint64_t function_fingerprint_{};
    std::uint64_t raw_memory_fingerprint_{};
    std::uint64_t completed_path_digest_{};
    std::uint64_t path_digest_{};
    std::uint64_t entry_guest_steps_{};
    std::uint64_t entry_ee_cycles_{};
    std::uint64_t pending_guest_steps_{};
    std::uint64_t pending_ee_cycles_{};
    guest::GuestAddress pending_pc_{};
    std::uint32_t pending_word_{};
    guest::R5900State entry_state_{};
    guest::R5900State return_jr_state_{};
    RetailTimerOwnerAbiTraceContext owner_context_{};
    Invocation current_invocation_{};
    Witnesses entry_witnesses_{};
    std::vector<Invocation> invocations_{};
    Summary summary_{};
};

} // namespace takedown::runtime
