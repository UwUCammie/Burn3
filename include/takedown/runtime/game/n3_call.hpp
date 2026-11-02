#pragma once

// Native-to-native calls for the n3 functions (src/runtime/game/n3/).
//
// Every n3 function body is a template over its context: the real NativeFunctionContext when the runtime calls it,
// or an n3::Machine (a CPU and a memory) when another native calls it. A body returns an n3::Run: the instructions
// the original retires on that path (callees included), or "declined" when the call must be left to the original.
//
// A caller makes the original's `jal` with Call::invoke: it puts the arguments in the CPU registers as the original
// does, invokes the callee's fn_<address> (declared with N3_CALLEE), and reads v0/f0 back. The caller's exact count
// is decided before it runs by a dry run (n3::dry_cost: the body on a copy of the CPU and a DryMemory), which also
// runs every callee dry, so a decline anywhere in the call tree hands the whole top-level call to the original
// before anything was changed. The live run then follows the same path.
//
// Only two machine types cross files: Machine<guest::EeMemory> (live) and Machine<DryMemory> (dry). A dry run
// inside a dry run copies the DryMemory, so it keeps that type.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>

namespace takedown::runtime::n3 {

struct Run {
    std::uint32_t retired{};
    bool declined{};
};
inline constexpr Run kDeclined{0U, true};

// What a body runs on when another native calls it. Same member names as NativeFunctionContext.
template <class Memory>
struct Machine {
    guest::R5900State& cpu;
    Memory& memory;
    GuestAddress entry_address{};
};
template <class Memory>
struct ConstMachine {
    const guest::R5900State& cpu;
    const Memory& memory;
    GuestAddress entry_address{};
};
using LiveMachine = Machine<guest::EeMemory>;
using DryMachine = Machine<DryMemory>;

// GuestCall's API over any context; finish() returns a Run (the live entry sets the PC).
template <class Memory>
class Call {
  public:
    template <class Context>
    explicit Call(Context& context) noexcept : cpu_(context.cpu), memory_(context.memory) {}

    [[nodiscard]] std::uint32_t arg(const unsigned index) const { return cpu_.read_gpr_u32(kArg0 + index); }
    [[nodiscard]] std::uint64_t arg64(const unsigned index) const { return cpu_.read_gpr_u64(kArg0 + index); }
    [[nodiscard]] Gpr128 arg128(const unsigned index) const { return cpu_.read_gpr(kArg0 + index); }
    [[nodiscard]] std::int32_t arg_signed(const unsigned index) const { return static_cast<std::int32_t>(arg(index)); }
    [[nodiscard]] float float_arg(const unsigned index) const {
        return std::bit_cast<float>(cpu_.read_fpr_bits(kFloatArg0 + index));
    }
    [[nodiscard]] GuestAddress stack_pointer() const { return cpu_.read_gpr_u32(kSp); }
    [[nodiscard]] std::uint32_t vector_random() const { return cpu_.read_vu0_control(kVectorRandom); }
    void set_vector_random(const std::uint32_t bits) { cpu_.write_vu0_control(kVectorRandom, bits); }

    void return_value(const std::uint32_t value) { cpu_.write_gpr_sign_extended_32(kV0, value); }
    void return_value64(const std::uint64_t value) { cpu_.write_gpr_u64(kV0, value); }
    void return_float(const float value) { cpu_.write_fpr_bits(0U, std::bit_cast<std::uint32_t>(value)); }

    [[nodiscard]] Run finish(const std::uint32_t cost) const { return {cost == 0U ? 1U : cost, false}; }
    [[nodiscard]] static constexpr Run decline() noexcept { return kDeclined; }

    // Calling another native. Arguments as the original's instructions leave them: set_arg sign-extends a 32-bit
    // value (lw, addiu, addu), set_arg64/set_arg128 write the whole register (daddu, ld, lq).
    void set_arg(const unsigned index, const std::uint32_t value) { cpu_.write_gpr_sign_extended_32(kArg0 + index, value); }
    void set_arg64(const unsigned index, const std::uint64_t value) { cpu_.write_gpr_u64(kArg0 + index, value); }
    void set_arg128(const unsigned index, const Gpr128& value) { cpu_.write_gpr(kArg0 + index, value); }
    void set_float_arg(const unsigned index, const std::uint32_t bits) { cpu_.write_fpr_bits(kFloatArg0 + index, bits); }
    [[nodiscard]] std::uint32_t result() const { return cpu_.read_gpr_u32(kV0); }
    [[nodiscard]] std::uint64_t result64() const { return cpu_.read_gpr_u64(kV0); }
    [[nodiscard]] std::uint32_t float_result() const { return cpu_.read_fpr_bits(0U); }

    // The original's `jal callee`: runs n3::fn_<address> with $sp lowered by the caller's frame (the callee sees the
    // caller's frame, for stack arguments), then restores $sp. The jal and its delay slot are the caller's own count.
    template <class Callee>
    [[nodiscard]] Run invoke(Callee&& callee, const std::uint32_t frame_bytes) {
        const Gpr128 sp = cpu_.read_gpr(kSp);
        cpu_.write_gpr_sign_extended_32(kSp, sp.words[0] - frame_bytes);
        Machine<Memory> machine{cpu_, memory_, 0U};
        const Run run = callee(machine);
        cpu_.write_gpr(kSp, sp);
        return run;
    }

    [[nodiscard]] Memory& memory() noexcept { return memory_; }
    [[nodiscard]] guest::R5900State& cpu() noexcept { return cpu_; }

  private:
    static constexpr unsigned kV0 = 2U;
    static constexpr unsigned kArg0 = 4U;
    static constexpr unsigned kSp = 29U;
    static constexpr unsigned kFloatArg0 = 12U;
    static constexpr std::size_t kVectorRandom = 20U;

    guest::R5900State& cpu_;
    Memory& memory_;
};

template <class Context>
[[nodiscard]] auto guest_call(Context& context) {
    return Call<std::remove_cvref_t<decltype(context.memory)>>{context};
}

// The last admitted dry run (call_cost) on this thread: its final CPU and writes. When the runtime then calls the
// same function on the very same CPU and unchanged guest memory (the usual prepare -> dispatch), the live entry
// applies this outcome instead of running the body a second time. The CPU is compared byte for byte with a copy
// taken before the dry run; memory is vouched for by EeMemory's main-RAM mutation generation (every tracked RAM write
// anywhere advances it, see capture_main_ram_mutation). Anything else (another call in between, a write, memory
// other than plain RAM or the scratchpad) runs the body live as before. TAKEDOWN_N3_COMMIT=0 always runs it live.
struct DryOutcome {
    bool valid{};
    std::thread::id thread{};
    GuestAddress entry{};
    std::uint32_t retired{};
    std::uint64_t generation{};
    std::array<std::byte, sizeof(guest::R5900State)> input{};
    std::optional<guest::R5900State> cpu;
    std::optional<DryMemory> memory;
};
// One per process: natives run on the EE emulation thread. The thread is recorded so another thread's call can
// never take it.
inline DryOutcome dry_outcome;

[[nodiscard]] inline bool dry_commit_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TAKEDOWN_N3_COMMIT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// The main-RAM mutation generation (0 when the memory has none).
[[nodiscard]] inline std::uint64_t main_ram_generation(guest::EeMemory& memory) noexcept {
    const std::uint64_t* const generation = memory.ee_aot_memory_view().mutation_generation;
    return generation != nullptr ? *generation : 0U;
}

[[nodiscard]] inline bool take_dry_outcome(NativeFunctionContext& context, std::uint32_t& retired) {
    DryOutcome& outcome = dry_outcome;
    if (!outcome.valid) return false;
    outcome.valid = false;
    if (outcome.entry != context.entry_address || outcome.thread != std::this_thread::get_id() ||
        std::memcmp(outcome.input.data(), &context.cpu, sizeof(guest::R5900State)) != 0 ||
        main_ram_generation(context.memory) != outcome.generation) {
        return false;
    }
    outcome.memory->commit(context.memory);
    context.cpu = *outcome.cpu;
    retired = outcome.retired;
    return true;
}

// The runtime's entry: the body on the real CPU and memory (or the admitted dry run's outcome), then the original's
// return (pc = ra).
template <auto Impl>
NativeFunctionExecution live(NativeFunctionContext& context) {
    std::uint32_t retired = 0U;
    if (!take_dry_outcome(context, retired)) {
        const Run run = Impl(context);
        if (run.declined) {
            // The dry run (call_cost) admitted this call, and the live run takes the same path, so this cannot happen.
            std::fprintf(stderr, "n3: native 0x%08x declined after its call_cost admitted it\n", context.entry_address);
            std::abort();
        }
        retired = run.retired;
    }
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return {.retired_instructions = retired, .ee_cycles = retired};
}

// call_cost of a body that can decline (or call other natives): the body on a copy of the CPU and a DryMemory.
// Returns its exact count, or 0 (the original runs) when it declines or retires more than Ceiling.
template <auto DryImpl, std::uint32_t Ceiling, bool Commit = true>
std::uint32_t dry_cost(const NativeFunctionPrepareContext& context) {
    DryOutcome& outcome = dry_outcome;
    outcome.valid = false;
    guest::R5900State cpu = context.cpu;
    DryMemory memory{context.memory};
    DryMachine machine{cpu, memory, context.entry_address};
    const Run run = DryImpl(machine);
    if (run.declined || run.retired > Ceiling) return 0U;
    if (Commit && dry_commit_enabled() && memory.writes_plain_memory()) {
        // ee_aot_memory_view only refreshes pointers to the memory's own state; it changes no guest-visible state.
        const std::uint64_t generation = main_ram_generation(const_cast<guest::EeMemory&>(context.memory));
        if (generation != 0U && generation != ~std::uint64_t{0U}) {
            outcome.entry = context.entry_address;
            outcome.thread = std::this_thread::get_id();
            outcome.retired = run.retired;
            outcome.generation = generation;
            std::memcpy(outcome.input.data(), &context.cpu, sizeof(guest::R5900State));
            outcome.cpu.emplace(cpu);
            outcome.memory.reset();
            outcome.memory.emplace(std::move(memory));
            outcome.valid = true;
        }
    }
    return run.retired;
}

} // namespace takedown::runtime::n3

// Declares the callable body of another n3 function (at file scope, before `namespace takedown::runtime {`).
#define N3_CALLEE(addr)                                                                                               \
    namespace takedown::runtime::n3 {                                                                                 \
    template <class Memory>                                                                                           \
    Run fn_##addr(Machine<Memory>&);                                                                                  \
    }

// The callable body of a function whose entry has its own call_cost (`cost`, a template over its context): the
// cost decides first (0 = declined), then `impl` runs. At file scope, after the entry.
#define N3_BODY(addr, impl, cost)                                                                                     \
    namespace takedown::runtime::n3 {                                                                                 \
    template <class Memory>                                                                                           \
    Run fn_##addr(Machine<Memory>& m) {                                                                               \
        if (cost(ConstMachine<Memory>{m.cpu, m.memory, 0x##addr##U}) == 0U) return kDeclined;                         \
        return impl(m);                                                                                               \
    }                                                                                                                 \
    template Run fn_##addr<guest::EeMemory>(Machine<guest::EeMemory>&);                                              \
    template Run fn_##addr<DryMemory>(Machine<DryMemory>&);                                                           \
    }

// The callable body of a function without call_cost (it never declines).
#define N3_BODY_PLAIN(addr, impl)                                                                                     \
    namespace takedown::runtime::n3 {                                                                                 \
    template <class Memory>                                                                                           \
    Run fn_##addr(Machine<Memory>& m) {                                                                               \
        return impl(m);                                                                                               \
    }                                                                                                                 \
    template Run fn_##addr<guest::EeMemory>(Machine<guest::EeMemory>&);                                               \
    template Run fn_##addr<DryMemory>(Machine<DryMemory>&);                                                           \
    }

// Entry + callable body for a function that calls other natives or can decline: `impl` returns call.decline() for
// a path it does not cover; call_cost is the dry run. Declared cost = ceiling (the dry run returns 0 above it).
#define N3_ENTRY(addr, bytes, hash, name, ceiling, impl, returns)                                                     \
    N3_BODY_PLAIN(addr, impl)                                                                                         \
    namespace takedown::runtime {                                                                                     \
    extern constinit const EngineFunction n3_##addr{0x##addr##U, bytes, hash, name, ceiling,                          \
                                                    n3::live<&impl<NativeFunctionContext>>, returns, {}, 1e-4, 1e-6,  \
                                                    {}, n3::dry_cost<&impl<n3::DryMachine>, ceiling>};               \
    }

// Entry + callable body for a function that never declines and calls no other native (no loops over data either):
// no call_cost; `cost` = the longest path through the listing.
#define N3_ENTRY_PLAIN(addr, bytes, hash, name, cost, impl, returns)                                                  \
    N3_BODY_PLAIN(addr, impl)                                                                                         \
    namespace takedown::runtime {                                                                                     \
    extern constinit const EngineFunction n3_##addr{0x##addr##U, bytes, hash, name, cost,                             \
                                                    n3::live<&impl<NativeFunctionContext>>, returns};                 \
    }

// N3_ENTRY for a body whose dry run deliberately leaves out live-only work (a VU0 microprogram, a DMA kick...): its
// dry run still gives the count, but its outcome is never committed, so the live entry always runs the body.
#define N3_ENTRY_LIVE_WORK(addr, bytes, hash, name, ceiling, impl, returns)                                           \
    N3_BODY_PLAIN(addr, impl)                                                                                         \
    namespace takedown::runtime {                                                                                     \
    extern constinit const EngineFunction n3_##addr{0x##addr##U, bytes, hash, name, ceiling,                          \
                                                    n3::live<&impl<NativeFunctionContext>>, returns, {}, 1e-4, 1e-6,  \
                                                    {}, n3::dry_cost<&impl<n3::DryMachine>, ceiling, false>};        \
    }
