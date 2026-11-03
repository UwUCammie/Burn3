#pragma once

// P2 / WP5: native props. The per-cell cull + prepare (0x0026a610) and the drain (0x0026a1c0) of the guest's instanced prop sections
// run as plain C++ (slus_21050_native_props_core.hpp) and hand the prepared batches to the command emitter as PropSubmits
// (platform/native_world_submit.hpp) instead of building VU0 buffers, a mesh CALL, instance UNPACKs and a VU1 run.
//
// Design: stateless, per call, VU0 memory mirrored.
//   The section setup (0x0026a7e0) and the view registration (0x0026a710) are NOT replaced: they leave the section's state in guest
//   visible places (ctx 0x01e32a10 bytes 128..156, VU0 data memory qw 213..252 / 229.., VU0 integer registers vi1 vi4 vi8 vi15), and
//   every native call reads its inputs from there (box corners, planes, tint, distance reference, views, pending records) and writes its
//   result back in the same form (view buffer counts and records). Consequences:
//     * no host state is kept between calls, so a call the runner does not hand to the native (scheduler boundary), or one the native
//       declines (lit tint, a mesh the importer cannot take, staging buffer full, ring full, kind 5, retained transform, scrolling
//       texture, ...), simply runs the original code on exactly the state the native left, and a section may mix native and original
//       calls freely (the original flush writes its packets straight to the ring, the native one stubs; props are alpha tested, so their
//       order inside a section is not visible);
//     * the decision "native or decline for the whole section" of the recon is replaced by the same decision per call, made from the
//       same inputs (material kind / flags in ctx word 148, ctx byte 138), so it is the same for every call of a section.
//   The one thing the native may not skip is a view buffer holding records after the section: the drain (native or original) flushes
//   them. The VU0 registers the original leaves behind besides vi1 / vi4 / vi8 / vi15 are scratch the next vcallms reloads.
//
// Replace mode (TAKEDOWN_NATIVE_WORLD_SUBMIT=1, platform::native_world_submit_ring() non-null): for each batch the call (1) selects the
// render target as the original flush does (ctx 153 / masks, 0x26baa0 ported), (2) copies the queued view packets and a FLUSHE to the
// staging output (scratchpad draw list), (3) pushes a PropSubmit (<= 14 instances, one view, the groups of the section's mesh pair) and
// puts the 3-quadword stub (CNT, DIRECT 1, marker) behind them. It then sets ctx[155] = 1 and continues at 0x0026b2b0 with a0 = ctx and
// ra unchanged (a tail call: the guest's group prologue, which binds program 14 and the texture, then the copy of the staging output to
// the ring), or returns to its caller when there was nothing to flush.
//
// Shadow mode (TAKEDOWN_P2_SHADOW=1, platform::native_world_shadow_ring() non-null): the resolver computes the same batches the guest
// is about to flush and pushes them to the shadow ring; it always declines, so the guest path runs untouched.
//
// Not covered, left to the original: kind 5 (lit) sections, retained-transform sections (section flag bit 2), scrolling textures,
// batches with a tint outside 0..255. Mesh packets are imported by runtime/native_world_mesh.hpp (program 14, V2-32 uv).

#include "takedown/guest/types.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <cstdint>
#include <string>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kPropCellAddress = 0x0026a610U;   // per cell: (ctx, count (signed byte), instance pointer)
inline constexpr guest::GuestAddress kPropDrainAddress = 0x0026a1c0U;  // end of a section: (ctx)
inline constexpr guest::GuestAddress kPropStageCopyAddress = 0x0026b2b0U; // prologue + staging copy, tail-called

// Registers the two hooks when a world submit ring (replace) or shadow ring exists and the retail code is in memory.
// TAKEDOWN_DISABLE_NATIVE_FUNCTIONS names "PropCell" / "PropDrain" switch one off. Returns whether the hooks were added.
// TAKEDOWN_NATIVE_PROPS_DECLINE_EVERY=N declines every Nth call (tests mixed streams). Call it with the other native registrations,
// after platform::NativeCmdEmitter exists. The n3 natives of 0x0026a7e0 / 0x0026a710 stay as they are.
bool register_native_props(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory);

// One line of counters for the log.
[[nodiscard]] std::string native_props_statistics();

} // namespace takedown::runtime::slus_21050
