// 0x0014a940 (a0 = car C): CarStripEdgeFraction. The car's object (u32 at C+4) gives its track (u32 at +9264),
// whose record (u32 at +516) holds the point quadword at +48. The point is stored in this call's frame, at sp+16
// (the original's `sq $v0, 16($sp)` in the delay slot of its jal), because StripEdgeFraction (0x00145ef0) reads
// the point through a2. The callee gets a0 = u32[C+468], a1 = u16[C+472] and a zero margin in f12. Returns the
// callee's f0; the saved ra is left alone (the original's `sd`/`ld` pair restores the same value).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00145ef0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kObjectOffset = 4U;        // C+4: the car's object
constexpr GuestAddress kTrackOffset = 9264U;      // object+9264: its track
constexpr GuestAddress kRecordOffset = 516U;      // track+516: the record
constexpr GuestAddress kPointOffset = 48U;        // record+48: the point quadword
constexpr GuestAddress kStripOffset = 468U;       // C+468: the strip object (the callee's a0)
constexpr GuestAddress kEdgeOffset = 472U;        // C+472: the edge index (the callee's a1, a halfword)
constexpr std::uint32_t kFrameBytes = 32U;        // addiu $sp, $sp, -32 (and +32 on return)
constexpr GuestAddress kFramePoint = 16U;         // sp+16 inside that frame: where the point is stored
constexpr std::uint32_t kMarginBits = 0U;         // mtc1 $zero, $f12: the margin is +0.0
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kBeforeCall = 12U;        // 0x0014a940..0x0014a96c: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 3U;          // 0x0014a970..0x0014a978: ld ra, jr ra and the frame restore

template <class Context>
n3::Run car_strip_edge_fraction(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress car = call.arg(0);
    const GuestAddress object = m.read_u32_le(car + kObjectOffset);
    const GuestAddress track = m.read_u32_le(object + kTrackOffset);
    const GuestAddress record = m.read_u32_le(track + kRecordOffset);
    const n3::Gpr128 point = n3::load_quad_any(m, record + kPointOffset);
    const std::uint32_t strip = m.read_u32_le(car + kStripOffset);
    const std::uint32_t edge = m.read_u16_le(car + kEdgeOffset);

    const GuestAddress point_address = call.stack_pointer() - kFrameBytes + kFramePoint;
    n3::store_quad_any(m, point_address, point);
    call.set_arg(0, strip);
    call.set_arg(1, edge);
    call.set_arg(2, point_address);
    call.set_float_arg(0, kMarginBits);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00145ef0(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014a940, 60U, 0x79f5cf33e253e3caULL, "CarStripEdgeFraction", kCeiling, car_strip_edge_fraction,
         EngineReturn::Float)
