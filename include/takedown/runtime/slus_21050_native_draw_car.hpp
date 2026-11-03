#pragma once

// P1 / WP5: the native DrawCar (original 0x0021e6f0): the car's draw-list entries become CarSubmits and one small stub each in the
// guest's display list instead of VIF/VU1 packets (see platform/native_car_submit.hpp, runtime/native_car_draw.hpp).
//
// Where it hooks: not at the function entry but at 0x0021e8cc, the return address of DrawCar's call to the texture bind
// (0x0021ff60). By then the guest has run the prologue, the S+248/S+252 stores, the LOD choice, StartEffect and the bind itself
// (texture residency, the bind packet, S+32, gp-24080), and the registers hold the function's live state (s0 LOD block, s1 BGV
// file, s2 draw block, s3 car frame, s4 draw context, s5 light bits). The hook either takes the call over (writes the stubs, pushes
// the submissions, applies the glass patch and continues at the function's own epilogue, 0x0021f754) or declines, in which case the
// guest simply goes on drawing the car as before: a decline, or a native dispatch refused by the runner, is always safe.

#include "takedown/guest/types.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <cstdint>
#include <string>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime::slus_21050 {

// Hook decision (integration, 2026-11-03): the mid-function address 0x0021e8cc IS reachable under the EE AOT. main.cpp marks every
// registered native address in the scalar-burst hook filter (`hooks.mark_pc(function.address)`), and ee_aot_install() treats a marked PC
// as excluded: a compiled block that holds one is not entered as a whole, the run stops before the instruction at the PC and the caller's
// burst loop offers the native replacement there (a block that starts at the PC is a "declinable entry": after a decline the guest code
// runs on from it). 0x0021e8cc follows the delay slot of DrawCar's `jal 0x0021ff60`, so it is a block head. Registration therefore has to
// come after the emitter exists (it owns the ring) and before the hook filter is collected: main.cpp registers right after the emitter
// block. No function-entry hook and no guest-call helper were needed.
//
// State not reproduced (checked against the guest run in a garage scenario): VU0 registers (VF1..VF31, ACC, VI1..15 -- zeroing them
// at DrawCar's epilogue in the guest run changes nothing downstream), scratchpad record W beyond StartEffect's part, VU0 data memory
// 0..10/16 (microprogram 0xfb's image). Reproduced: VU1 962..1013 (an UNPACK tail, last sent value per quadword, so a declined
// guest call and the emitter see what the original left) and W+144..207 (the lighting matrix a call with an unlit first part reuses).
// The only difference left in the stream are PATH1 strips drawn
// after the cars pass whose vertex ST / mesh data come from stale VU1 buffers the guest's car launches left behind.
inline constexpr guest::GuestAddress kDrawCarEntry = 0x0021e6f0U;
inline constexpr guest::GuestAddress kDrawCarAfterBind = 0x0021e8ccU;
inline constexpr guest::GuestAddress kDrawCarEpilogue = 0x0021f754U;

// Registers the hook when native car submission is live: platform::native_car_submit_ring() is non-null (TAKEDOWN_NATIVE_CAR_SUBMIT=1
// with the native command emitter in token mode and car trust, so the emitter must have been constructed), the retail DrawCar code
// is in memory and TAKEDOWN_DISABLE_NATIVE_FUNCTIONS does not name "DrawCar". Decided once; returns whether the hook was added.
// Call it where the engine functions are registered (before the scalar-burst hooks are collected).
// TAKEDOWN_NATIVE_CAR_DECLINE_EVERY=N declines every Nth call (tests the mixed native/guest stream).
bool register_native_draw_car(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);

// One line of counters for the log (calls taken, declines by reason, submissions, flushes).
[[nodiscard]] std::string native_draw_car_statistics();

} // namespace takedown::runtime::slus_21050
