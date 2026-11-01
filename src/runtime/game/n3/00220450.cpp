// 0x00220450 (no arguments): CopyQuadBlockToStaging. A plain 160-byte copy of ten quadwords (lq/sq, 16-byte
// aligned) from the fixed global block at 0x00666330 to the fixed staging block at 0x0067d5a0. The original's
// loop always makes three passes (a1 starts at 3 and bgtz tests it, so the trip count is fixed, not data
// dependent) and copies two quadwords per pass; the tail copies the quadwords at +96..+144 to 0x0067d600..
// 0x0067d630. Source and destination do not overlap, so this is straight code with no call_cost. Every store
// is made, in the original's order. Returns nothing: the temporaries the original leaves holding quadwords and
// addresses (a0-a3, t0-t2, v1) are not written, as a call may leave any temporary with anything.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSource = 0x00666330U;       // lui 0x66, addiu 25392
constexpr GuestAddress kStaging = 0x0067d5a0U;      // lui 0x68, addiu -10848
constexpr GuestAddress kBlockBytes = 160U;          // ten quadwords, offsets +0..+144

// Retired instructions on every path: six before the loop, eight per pass (the bgtz delay slot included) over
// three passes, and thirteen after it (the last sq is jr's delay slot).
constexpr std::uint32_t kCost = 43U;

template <class Context>
n3::Run copy_quad_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    for (GuestAddress offset = 0U; offset < kBlockBytes; offset += 16U) {
        n3::store_quad_any(m, kStaging + offset, n3::load_quad_any(m, kSource + offset));
    }
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00220450{0x00220450U, 108U, 0x13bec9a5760eae8dULL, "CopyQuadBlockToStaging",
                                                  kCost, n3::live<&copy_quad_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00220450, copy_quad_block)
