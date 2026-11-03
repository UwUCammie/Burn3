#pragma once

// P2 WP3: the native SubmitWorldObject (original 0x0026af50): the guest stops building the per-object mesh packets of world strips; a
// stub with a world marker stands in their place in the staging output and the object's draw data goes through the ring to the emitter
// (see platform/native_world_submit.hpp, runtime/slus_21050_native_world_submit_core.hpp for what is kept and what is not).
//
// Modes (decided once at registration, from the rings the live NativeCmdEmitter published):
//   TAKEDOWN_NATIVE_WORLD_SUBMIT=1  replace: the call is taken over (visibility, ctx / staging side effects, stub, WorldSubmit).
//   TAKEDOWN_P2_SHADOW=1            shadow: the guest packets stay; the WorldSubmit goes to the shadow ring and the original code
//                                   continues at 0x0026af54 (the native executes its first instruction, `addiu sp, sp, -16`).
// Per call it declines (the guest runs the original code from the entry) for kinds / states it does not support.
// TAKEDOWN_NATIVE_WORLD_DECLINE_EVERY=N declines every Nth call (mixed native / guest stream test). TAKEDOWN_DISABLE_NATIVE_FUNCTIONS
// names it "SubmitWorldObject" like the older packet-exact native (slus_21050_world_object_submit.cpp), which this replaces when on:
// register it first; the older one then finds the address taken.

#include "takedown/runtime/native_function_dispatcher.hpp"

#include <string>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kNativeWorldSubmitEntry = 0x0026AF50U;

// Registers the hook when platform::native_world_submit_ring() or native_world_shadow_ring() is non-null (so after the emitter was
// constructed), the retail code is in memory and the function is not disabled by the environment. Once per process; returns whether
// the hook was added.
bool register_native_world_submit(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);

// One line of counters for the log ("[NATIVEWORLD] ...").
[[nodiscard]] std::string native_world_submit_statistics();

} // namespace takedown::runtime::slus_21050
