#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace takedown::guest {

// Ahead-of-time compiled EE code. tools/ee_aot_generator.cpp turns whole game
// functions into C++ (a game-derived module that stays under local-analysis/);
// src/guest/ee_aot.cpp links it when TAKEDOWN_LOCAL_EE_AOT_MODULE names one.
//
// A run executes guest instructions exactly as consecutive step_r5900 calls
// would, inside the caller's scalar burst. Every instruction but the last
// executed one is committed and is one the burst would have continued past:
// it returned Executed, touched no device (only main RAM and the scratchpad,
// never with a store observer or PATH2 source marker live), changed neither
// COP0 Status nor VU0 microcode state, stored to no RAM range the caller
// polls (EeMemory::set_ee_aot_watch_range), and left the PC on an address
// that is not excluded. The run stops before an instruction it does not translate,
// before a block that does not fit in the budget and on reaching an excluded
// or uncompiled address, so the interpreter takes over at an exact boundary.
struct EeAotRun {
    // Instructions executed; zero when the run declined at once (the state
    // is then untouched).
    std::uint64_t executed{};
    // Result of the last executed instruction, as step_r5900 returned it,
    // except that an Executed result carries only the instruction's pc and
    // word (not decoded): a run ends after every instruction the caller
    // would have looked into, and decoding costs the same on every run.
    EeStepResult last{};
    // PC of that instruction.
    GuestAddress last_pc{};
    // How the caller continues (see EeAotNativeHook); kEeAotRunOrdinary unless
    // the hook handled native calls during the run.
    std::uint8_t outcome{};
};

// Outcomes of a run that called the native hook.
// Ordinary: as documented above.
inline constexpr std::uint8_t kEeAotRunOrdinary = 0U;
// One or more native calls were made and completed, nothing else was executed
// after them (`executed` is zero): the caller is in the state it would be in
// after a native call of its own, with no pending instruction.
inline constexpr std::uint8_t kEeAotRunNativeDone = 1U;
// The caller must not account the `executed` instructions' progress (steps,
// cycles, provenance) again; it has been done. Its own per-instruction burst
// counters still apply to `executed`.
inline constexpr std::uint8_t kEeAotRunAccounted = 2U;
// A prepared native call was not handled; the caller ends its burst at a hook.
inline constexpr std::uint8_t kEeAotRunNativeUnhandled = 3U;

// A caller-installed handler for the moment a run stops at a native entry
// (an excluded PC at which the caller dispatches a native replacement).
// `executed` instructions (>= 1) were run since the run began or since the
// previous hook call returned kContinue, the last of which was at `last_pc`;
// the state is exactly as run_ee_aot would return it. The hook either does
// nothing (kNone) or performs, for the caller, everything the caller's loop
// would do from there: accounting, the native call(s), the post-instruction
// checks. It must change nothing before it knows it can go on, except what the
// outcomes below say it has done.
struct EeAotNativeHookResult {
    enum class Kind : std::uint8_t {
        // Nothing was done; the run ends and returns as it would without a hook.
        None,
        // One or more native calls completed; the caller's loop would now run
        // compiled code at state.pc() with `budget` left and `first_step` as
        // the step number of its first instruction.
        Continue,
        // Progress of the `executed` instructions was accounted (kEeAotRunAccounted);
        // no native call was made. The run ends.
        HandOffAccounted,
        // Native call(s) completed, but the caller must run its own checks
        // (kEeAotRunNativeDone). The run ends.
        HandOffNativeDone,
        // A native call was prepared but not handled (kEeAotRunNativeUnhandled).
        HandOffUnhandled,
    };
    Kind kind{Kind::None};
    std::uint64_t budget{};
    std::uint64_t first_step{};
};
using EeAotNativeHook = EeAotNativeHookResult (*)(void* user, std::uint64_t executed, GuestAddress last_pc);
// Installs (or, with nullptr, removes) the hook for the following runs.
void ee_aot_set_native_hook(EeAotNativeHook hook, void* user) noexcept;

struct EeAotStatistics {
    std::uint64_t calls{};
    std::uint64_t runs{};
    std::uint64_t instructions{};
    // Entries through the dispatcher (compiled code also enters compiled
    // functions directly, uncounted).
    std::uint64_t function_entries{};
    std::uint64_t stop_uncompiled{};
    std::uint64_t stop_budget{};
    std::uint64_t stop_excluded{};
    std::uint64_t stop_untranslated{};
    std::uint64_t stop_memory{};
    std::uint64_t stop_device{};
    std::uint64_t stop_fault{};
    // Loads of EE/GS registers and stores to plain DMA channel parameter
    // registers that the run performed itself and went on from (see
    // ee_exit_stop); they are not stops.
    std::uint64_t register_loads{};
    std::uint64_t register_stores{};
    // How much budget the refusals counted in stop_budget found left: bucket
    // i holds the refusals with bit_width(remaining) == i (0, 1, 2-3, 4-7,
    // 8-15, 16-31, 32-63; the last bucket is 64 and more). stop_budget_at_entry
    // counts the refusals that came before the run executed anything.
    std::array<std::uint64_t, 8> stop_budget_remaining{};
    std::uint64_t stop_budget_at_entry{};
    // Runs that entered a native function's entry block as compiled code
    // because the caller found the native replacement declined (see
    // run_ee_aot).
    std::uint64_t native_declined_entries{};
    // Where the accesses behind stop_memory went (the effective address of the
    // instruction the run stopped before): EE registers by KiB of
    // 0x10000000..0x1000ffff, then VU memories, GS registers, misaligned
    // addresses and everything else.
    std::array<std::uint64_t, 64> stop_memory_ee_register_kib{};
    std::uint64_t stop_memory_vu{};
    std::uint64_t stop_memory_gs{};
    std::uint64_t stop_memory_unaligned{};
    std::uint64_t stop_memory_other{};
    std::uint64_t identity_checks{};
    std::uint64_t identity_failures{};
    // Native entries a run's hook was offered (native_hook_taken: it handled
    // them, native_hook_continued: and the run went on in compiled code).
    std::uint64_t native_hook_offered{};
    std::uint64_t native_hook_taken{};
    std::uint64_t native_hook_continued{};
};

[[nodiscard]] bool ee_aot_module_linked() noexcept;
[[nodiscard]] std::uint64_t ee_aot_compiled_function_count() noexcept;
// Installs the module for this memory (the code range whose stores are
// tracked). The PCs for which `excluded` is true are left to the caller's own
// dispatch: a run never executes an instruction at one, so it stops before
// reaching it, and enters a block that holds one only at an address after its
// last such PC (the caller has then already performed the hooked instruction).
//
// `declinable_entry`, when given, names the excluded PCs at which the only
// thing the caller does is offer a native replacement of the function that
// starts there. A block whose single excluded PC is such an entry (its first
// instruction) can be run from that entry by passing the PC as
// `declined_native_entry` to run_ee_aot once the caller found the replacement
// not applicable: the guest code is then the ordinary path.
void ee_aot_install(EeMemory& memory, const std::function<bool(GuestAddress)>& excluded,
                    const std::function<bool(GuestAddress)>& declinable_entry = {});
// Stop PCs that apply only while the caller says so (a live UI draw session,
// whose observer needs to see the instruction at each of them). Registered once
// after ee_aot_install: `fixed` is the standing set. ee_aot_update_session_stops
// then switches the set on or off and names up to two more PCs (a learned
// callback target and a return address; 0 for none). While active the PCs are
// treated exactly like `excluded` ones: a run never executes an instruction at
// one, and enters a block that holds one only at an address after it. Stopping
// early is always safe, so the caller may name a superset of what it needs.
// Cheap when nothing changed since the previous call.
void ee_aot_set_session_stops(std::span<const GuestAddress> fixed);
void ee_aot_update_session_stops(bool active, GuestAddress extra_a = 0U, GuestAddress extra_b = 0U);
// `first_step` is the guest step number the caller has published for the
// instruction at state.pc() (EeMemory::current_guest_step).
// `declined_native_entry`: the PC of state at which a declinable entry (see
// ee_aot_install) was offered to its native replacement and declined. Only the
// first block entered, at exactly that PC, may be an excluded entry block.
inline constexpr GuestAddress kEeAotNoDeclinedEntry = 0xffffffffU;
[[nodiscard]] EeAotRun run_ee_aot(R5900State& state, EeMemory& memory, std::uint64_t budget,
                                  std::uint64_t first_step,
                                  GuestAddress declined_native_entry = kEeAotNoDeclinedEntry);
[[nodiscard]] const EeAotStatistics& ee_aot_statistics() noexcept;
// Every compiled entry point (block head), for tools/verify_ee_aot.cpp.
[[nodiscard]] std::vector<GuestAddress> ee_aot_entry_points();
// With TAKEDOWN_EE_AOT_MISSES=<file>, runs count the addresses at which they
// found no compiled block (tab-separated pc and count, most frequent first;
// written by ee_aot_write_misses). tools/ee_aot_generator.cpp --misses reads
// them back as extra block heads and functions to compile. The addresses at
// which a run stopped for a compiled block the caller keeps (a hook PC) go to
// <file>.excluded in the same form, the PCs at which the caller's burst
// ended for a hook (ee_aot_note_hook_stop) to <file>.hooks, and the native
// entries it found not applicable and stepped through (ee_aot_note_native_decline)
// to <file>.declines; the generator reads none of them.
void ee_aot_write_misses();
// True while TAKEDOWN_EE_AOT_MISSES is recording.
[[nodiscard]] bool ee_aot_recording() noexcept;
// Counts one burst ending at the hook PC `pc` (recorded only while recording).
void ee_aot_note_hook_stop(GuestAddress pc);
// Counts one native entry at `pc` whose replacement did not apply.
void ee_aot_note_native_decline(GuestAddress pc);

} // namespace takedown::guest
