#pragma once

// The ABI between the generated EE AOT function shards
// (tools/ee_aot_generator.cpp) and the dispatcher (src/guest/ee_aot.cpp): the
// context layout, the out-of-line helpers the shards call and the inline
// fast paths they use. Every shard includes this header and nothing of the
// runtime (no ee_memory.hpp, ee_kernel.hpp, ...), so an ordinary runtime
// change does not rebuild the shards; only a change here, or in the small
// guest state and arithmetic headers below, does. Keep the runtime-side
// definitions (anything that needs EeMemory) in ee_aot_internal.hpp.
// See include/takedown/guest/ee_aot.hpp for the contract.

#include "takedown/guest/ee_aot_memory_view.hpp"
#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/ee_mmi.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace takedown::guest {
class EeMemory;
struct EeStepResult;
} // namespace takedown::guest

namespace takedown::guest::ee_aot_detail {

struct EeAotEntryInfo;
struct EeAotFunctionInfo;

struct EeAotContext {
    R5900State* state{};
    EeMemory* memory{};
    // The memory's store fast path state (EeMemory::ee_aot_memory_view).
    const EeAotMemoryView* view{};
    Gpr128* gpr{};
    std::uint64_t* hi{};
    std::uint64_t* lo{};
    std::byte* ram{};
    std::byte* scratchpad{};
    // Instructions executed before the current block; generated code keeps
    // it in a local, adds a path's constant count at every transfer and
    // stores it back at every exit and call-out.
    std::uint64_t executed{};
    // `executed` when the current compiled function was entered.
    std::uint64_t entry_executed{};
    std::uint64_t budget{};
    std::uint64_t first_step{};
    std::uint64_t hardware_epoch{};
    std::uint64_t code_epoch{};
    std::uint64_t watch_epoch{};
    const std::uint8_t* block_ok{};
    // Per function: the code-write epoch at which its words were last found
    // identical to the compiled ones (the run stops when that epoch moves).
    const std::uint64_t* verified_epoch{};
    // The dispatcher's tables, for transfers that enter the next function
    // directly: entry index + 1 per code word from code_begin.
    GuestAddress code_begin{};
    std::uint32_t code_bytes{};
    const std::uint32_t* entry_by_word{};
    const EeAotEntryInfo* entries{};
    const EeAotFunctionInfo* functions{};
    // Exit: the continuation PC of a transfer out of the function, and the
    // PC of the last instruction executed (on entry to a block: the
    // instruction that transferred into it).
    GuestAddress next_pc{};
    GuestAddress last_pc{};
    // Set when the last instruction was an interpreter call-out whose result
    // the caller must see unchanged (a fault, system call or VU0 program).
    bool callout_last{};
    EeStepResult* callout{};
    // EeMemory::ee_aot_stores_plain() as of the last time anything could have
    // changed it (the start of the run, an interpreted instruction, a store
    // that took the ordinary route).
    bool stores_plain{};
};

using EeAotFunction = bool (*)(EeAotContext&, std::uint32_t);

struct EeAotFunctionInfo {
    EeAotFunction function;
    GuestAddress begin;
    GuestAddress end;
    std::uint32_t first_word; // index into kEeAotCodeWords
};

// A block head, or an address inside a block where a run may resume (it
// executes the rest of that block).
struct EeAotEntryInfo {
    GuestAddress pc;
    std::uint32_t function;
    std::uint32_t entry;
    std::uint32_t block;
    std::uint32_t maximum; // most instructions any path from here to the block's end executes
};

struct EeAotBlockInfo {
    GuestAddress begin;
    std::uint32_t words; // instruction words the block spans
};

// Equal to kEeMainRamSize and kEeScratchpadSize (checked in ee_aot_internal.hpp).
inline constexpr std::size_t kMainRamBytes = 32U * 1024U * 1024U;
inline constexpr GuestAddress kScratchpadBase = 0x70000000U;
inline constexpr std::size_t kScratchpadBytes = 16U * 1024U;

// ---- Out-of-line exits (src/guest/ee_aot.cpp) -------------------------------

// How a run stops (ee_exit_stop's `kind`); `j` is the instruction's index in
// its block and `pc` its address.
enum : std::uint32_t {
    // The target block (j: its id) does not fit the budget or is excluded;
    // the transfer to pc is already counted.
    kEeExitRefused,
    // A transfer, already counted, to pc, where this function has no code.
    kEeExitAt,
    // Before the instruction at pc.
    kEeExitBeforeUntranslated,
    kEeExitBeforeMemory,
    kEeExitBeforeFault,
    // Before the delay slot at pc of a taken transfer to `next`.
    kEeExitSlotUntranslated,
    kEeExitSlotMemory,
    kEeExitSlotFault,
    // After the instruction at pc; `next` follows it.
    kEeExitAfter,
};
// Generated code has written its registers back, `executed` (without the
// instructions of the current block) and `last_pc` (the last transfer's
// source, or the caller's when there was none) before calling this.
[[gnu::cold]] bool ee_exit_stop(EeAotContext& c, std::uint32_t kind, std::uint32_t j, GuestAddress pc,
                                GuestAddress next) noexcept;
// One untranslated instruction through the interpreter; false when the run
// must stop after it (its result is kept for the caller).
bool ee_callout(EeAotContext& c, const DecodedR5900Instruction& instruction, GuestAddress next,
                bool delay_slot, std::uint32_t index);
// VCALLMS (instruction word `word` at pc): the microprogram launched exactly
// as the interpreter launches it (run_vu0_micro_call); false, as for a
// call-out, when the run must stop after it (a fault, or a device change) with
// its result kept for the caller.
bool ee_vcallms(EeAotContext& c, GuestAddress pc, std::uint32_t word, GuestAddress next, bool delay_slot,
                std::uint32_t index);
[[gnu::cold]] bool ee_callout_stop(EeAotContext& c, std::uint32_t j, GuestAddress pc) noexcept;
// The bookkeeping of a scratchpad store of `length` bytes at p (the bytes are
// already written); only needed while ee_aot_scratchpad_store_records().
[[gnu::cold]] void ee_store_scratchpad(EeAotContext& c, std::byte* p, std::uint32_t length) noexcept;
// The ordinary route of a store that ee_store_fast declined, for `length`
// bytes already at `source`: 1 when the store must not be performed (a
// write observer needs it; the run stops before the instruction at `pc`),
// 2 when it was performed and moved a device epoch (the run stops after the
// instruction), else 0 (done). `executed` is the run's count before the
// instruction.
[[gnu::cold]] unsigned ee_store_slow(EeAotContext& c, std::byte* p, const void* source, std::uint32_t length,
                                     GuestAddress pc, std::uint64_t executed) noexcept;

// ---- Inline vocabulary ------------------------------------------------------

// Low 64 bits of a GPR; generated code keeps them in locals in between.
[[gnu::always_inline]] inline std::uint64_t ee_get(const Gpr128* const gpr, const unsigned n) noexcept {
    std::uint64_t value;
    std::memcpy(&value, &gpr[n].words[0], sizeof(value));
    return value;
}
[[gnu::always_inline]] inline void ee_put(Gpr128* const gpr, const unsigned n,
                                          const std::uint64_t value) noexcept {
    std::memcpy(&gpr[n].words[0], &value, sizeof(value));
}
[[gnu::always_inline]] inline std::uint64_t ee_sx(const std::uint32_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

[[gnu::always_inline]] inline bool ee_leave(EeAotContext& c, const GuestAddress pc,
                                            const GuestAddress last_pc) noexcept {
    c.next_pc = pc;
    c.last_pc = last_pc;
    return true;
}
[[gnu::always_inline]] inline bool ee_block_fits(const EeAotContext& c, const std::uint32_t block,
                                                 const std::uint32_t maximum) noexcept {
    return c.budget - c.executed >= maximum && c.block_ok[block] != 0U;
}
// The compiled function and entry the dispatcher would enter at pc without
// further checks (the entry fits, and the function was found identical at
// this run's code epoch); nullptr when the dispatcher must decide.
[[gnu::always_inline]] inline EeAotFunction ee_direct_entry(const EeAotContext& c, const GuestAddress pc,
                                                            std::uint32_t& entry) noexcept {
    const auto offset = pc - c.code_begin;
    if ((pc & 3U) != 0U || offset >= c.code_bytes) {
        return nullptr;
    }
    const auto slot = c.entry_by_word[offset / 4U];
    if (slot == 0U) {
        return nullptr;
    }
    const auto& info = c.entries[slot - 1U];
    if (!ee_block_fits(c, info.block, info.maximum) || c.verified_epoch[info.function] != c.code_epoch) {
        return nullptr;
    }
    entry = info.entry;
    return c.functions[info.function].function;
}
// Main RAM through the TLB aliases the BIOS maps (EeMemory::main_ram_offset):
// the uncached range 0x20080000..0x22000000 and the uncached-accelerated range
// 0x30100000..0x32000000, which hold the same bytes as physical 0x00080000.. and
// 0x00100000... The ordinary path rejects an access that straddles a mapping
// entry; entries are multiples of 0x80000 long, so one that stays inside an
// aligned 0x80000 block certainly does not. nullptr for anything else.
[[gnu::always_inline]] inline std::byte* ee_ram_alias(const EeAotContext& c, const std::uint32_t address,
                                                      const std::uint32_t length) noexcept {
    std::uint32_t physical;
    if (address - 0x30100000U < 0x01f00000U) {
        physical = address - 0x30000000U;
    } else if (address - 0x20080000U < 0x01f80000U) {
        physical = address - 0x20000000U;
    } else {
        return nullptr;
    }
    if ((physical & 0x7ffffU) > 0x80000U - length || physical > kMainRamBytes - length) {
        return nullptr;
    }
    return c.ram + physical;
}

// Host pointer for an access the ordinary path serves from main RAM through
// the direct/cached segments or from the scratchpad without any device side
// effect; nullptr for anything else (the interpreter then performs it).
// ee_ram_other is everything after the one-compare common case.
[[gnu::always_inline]] inline std::byte* ee_ram_other(const EeAotContext& c, const std::uint32_t address,
                                                      const std::uint32_t length) noexcept {
    const auto segment = address & 0xe0000000U;
    if (segment == 0U || segment == 0x80000000U || segment == 0xa0000000U) [[likely]] {
        const auto physical = static_cast<std::size_t>(address & 0x1fffffffU);
        return physical <= kMainRamBytes - length ? c.ram + physical : nullptr;
    }
    const auto offset = static_cast<std::size_t>(address - kScratchpadBase);
    if (address >= kScratchpadBase && offset <= kScratchpadBytes - length) {
        return c.scratchpad + offset;
    }
    return ee_ram_alias(c, address, length);
}
// The cold shards (TAKEDOWN_EE_AOT_COLD_SHARD) take the cases after the first
// out of line: they are compiled for coverage, so their size matters more
// than the few cycles a scratchpad or segment-aliased access then costs.
[[gnu::noinline]] std::byte* ee_ram_other_out_of_line(const EeAotContext& c, std::uint32_t address,
                                                      std::uint32_t length) noexcept;
[[gnu::always_inline]] inline std::byte* ee_ram(const EeAotContext& c, const std::uint32_t address,
                                                const std::uint32_t length) noexcept {
    // Main RAM through kuseg (the common case) in one compare.
    if (address <= kMainRamBytes - length) [[likely]] {
        return c.ram + address;
    }
#if defined(TAKEDOWN_EE_AOT_COLD_SHARD)
    return ee_ram_other_out_of_line(c, address, length);
#else
    return ee_ram_other(c, address, length);
#endif
}
template <typename T>
[[gnu::always_inline]] inline T ee_load(const std::byte* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}
// The common case of a store in one test, before anything is written: no
// observer is live and the ordinary bookkeeping would only advance the mutation
// generation (done here), or the scratchpad store records nothing. True means
// the store is complete; false means nothing has happened and the caller takes
// the ordinary route (ee_store_slow). The provenance (current PC and step) is
// only read by that bookkeeping, so the fast path leaves it unwritten; the
// burst republishes it after every run.
[[gnu::always_inline]] inline bool ee_store_fast_inline(EeAotContext& c, std::byte* p, const void* source,
                                                        const std::uint32_t length) noexcept {
    const auto offset = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) -
                                                 reinterpret_cast<std::uintptr_t>(c.ram));
    if (offset < kMainRamBytes) [[likely]] {
        if (!c.stores_plain || !ee_aot_plain_main_ram_store(*c.view, offset, length)) [[unlikely]] {
            return false;
        }
    } else if (ee_aot_scratchpad_store_needs_slow_path(*c.view)) [[unlikely]] {
        return false;
    }
    std::memcpy(p, source, length);
    return true;
}
// The cold shards call it instead of expanding it at every store.
[[gnu::noinline]] bool ee_store_fast_out_of_line(EeAotContext& c, std::byte* p, const void* source,
                                                 std::uint32_t length) noexcept;
[[gnu::always_inline]] inline bool ee_store_fast(EeAotContext& c, std::byte* p, const void* source,
                                                 const std::uint32_t length) noexcept {
#if defined(TAKEDOWN_EE_AOT_COLD_SHARD) && !defined(TAKEDOWN_EE_AOT_COLD_INLINE_STORES)
    return ee_store_fast_out_of_line(c, p, source, length);
#else
    return ee_store_fast_inline(c, p, source, length);
#endif
}

// Shards generated before the store slow path moved out of line
// (ee_store_slow) expanded these in place; they need EeMemory, so the shards
// no longer see them. A stale module fails here with a pointer to the fix
// instead of a bare "not declared". (Not for the runtime side, which defines
// the real ones in ee_aot_internal.hpp.)
#if !defined(TAKEDOWN_EE_AOT_RUNTIME_SIDE)
template <typename... Arguments>
inline bool ee_aot_stale_module(Arguments&&...) noexcept {
    static_assert(sizeof...(Arguments) == ~std::size_t{0U},
                  "this EE AOT module was generated by an older takedown-ee-aot: regenerate it (README, EE ahead-of-time compilation)");
    return false;
}
template <typename... Arguments> inline bool ee_store_allowed(Arguments&&... a) noexcept { return ee_aot_stale_module(a...); }
template <typename... Arguments> inline bool ee_provenance_at(Arguments&&... a) noexcept { return ee_aot_stale_module(a...); }
template <typename... Arguments> inline bool ee_store(Arguments&&... a) noexcept { return ee_aot_stale_module(a...); }
template <typename... Arguments> inline bool ee_device_changed(Arguments&&... a) noexcept { return ee_aot_stale_module(a...); }
template <typename... Arguments> inline bool ee_refresh_stores_plain(Arguments&&... a) noexcept { return ee_aot_stale_module(a...); }
#endif

} // namespace takedown::guest::ee_aot_detail

// ---- Generated-code macros ----------------------------------------------------
// A generated function holds GPR n in local gn (HI/LO lane 0 in g32/g33), the
// run's count in `ex` (less the current block's instructions so far), the
// last transfer's source in `last`, and has `gpr`, `budget`, `ok` and the
// stop arguments xk/xj/xpc/xnext, with one `ee_exit` label that writes every
// register it changes back and calls ee_exit_stop. `j` is the instruction's
// index in its block.

#define EE_W(n) ee_put(gpr, n##U, g##n)
#define EE_R(n) g##n = ee_get(gpr, n##U)
#define EE_EXIT(kind, j, pc, next)                                                              \
    do {                                                                                        \
        xk = (kind);                                                                            \
        xj = (j);                                                                               \
        xpc = (pc);                                                                             \
        xnext = (next);                                                                         \
        goto ee_exit;                                                                           \
    } while (0)
#define EE_GOTO(label, block, maximum, count, from, target)                                     \
    do {                                                                                        \
        ex += (count);                                                                          \
        last = (from);                                                                          \
        if (budget - ex < (maximum) || ok[(block)] == 0U) [[unlikely]] {                        \
            EE_EXIT(kEeExitRefused, (block), (target), 0U);                                     \
        }                                                                                       \
        goto label;                                                                             \
    } while (0)
// The registers have been written back.
#define EE_LEAVE(count, from, target)                                                           \
    do {                                                                                        \
        c.executed = ex + (count);                                                              \
        return ee_leave(c, (target), (from));                                                   \
    } while (0)
// A transfer to entry `entry` of compiled function `function`, with the
// registers written back: entered directly, as a tail call, when the
// dispatcher would enter it (the block fits and is not excluded and the
// function's words are known identical), and through the dispatcher
// otherwise.
#define EE_CALL(count, from, target, function, entry, block, maximum)                           \
    do {                                                                                        \
        c.executed = ex + (count);                                                              \
        c.last_pc = (from);                                                                     \
        if (budget - c.executed >= (maximum) && ok[(block)] != 0U &&                            \
            c.verified_epoch[(function)] == c.code_epoch) [[likely]] {                          \
            [[gnu::musttail]] return ee_f##function(c, (entry));                                \
        }                                                                                       \
        c.next_pc = (target);                                                                   \
        return true;                                                                            \
    } while (0)
// A transfer to a register target, with the registers written back: the
// compiled entry there is entered directly when the dispatcher would enter
// it at once (see ee_direct_entry), and the dispatcher takes it otherwise.
#define EE_JUMP(count, from, target)                                                            \
    do {                                                                                        \
        c.executed = ex + (count);                                                              \
        c.last_pc = (from);                                                                     \
        c.next_pc = (target);                                                                   \
        std::uint32_t jump_entry;                                                               \
        if (const auto jump_function = ee_direct_entry(c, c.next_pc, jump_entry);               \
            jump_function != nullptr) [[likely]] {                                              \
            [[gnu::musttail]] return jump_function(c, jump_entry);                              \
        }                                                                                       \
        return true;                                                                            \
    } while (0)
#define EE_STOP_AT(count, from, target)                                                         \
    do {                                                                                        \
        ex += (count);                                                                          \
        last = (from);                                                                          \
        EE_EXIT(kEeExitAt, 0U, (target), 0U);                                                   \
    } while (0)
// The registers have been written back; the interpreter has left the state
// as it should be after a stop.
#define EE_CALLOUT(instruction, j, pc, next, slot)                                              \
    do {                                                                                        \
        c.executed = ex;                                                                        \
        if (!ee_callout(c, (instruction), (next), (slot), (j))) [[unlikely]] {                  \
            return ee_callout_stop(c, (j), (pc));                                               \
        }                                                                                       \
    } while (0)
// The registers have been written back (a call observer may read them).
#define EE_VCALLMS(word, j, pc, next, slot)                                                     \
    do {                                                                                        \
        c.executed = ex;                                                                        \
        if (!ee_vcallms(c, (pc), (word), (next), (slot), (j))) [[unlikely]] {                   \
            return ee_callout_stop(c, (j), (pc));                                               \
        }                                                                                       \
    } while (0)
