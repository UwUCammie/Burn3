// 0x001b4ab0 TransformObjectBounds(a1 = object): moves the object's local axis-aligned box into the world with the
// object's matrix (Arvo's method) and writes the world box into the object.
//
// Object layout (u32 pointers at +4 and +8, quadwords at +16 and +32):
//   +4   matrix: rows r0, r1, r2 at +0/+16/+32 (xyz used), translation at +48
//   +8   local box: minimum quadword at +0, maximum quadword at +16
//   +16  world minimum (xyz written, the other lane keeps what the object held)
//   +32  world maximum (same)
// For every world axis a the column c_a = (r0.a, r1.a, r2.a) scales both box corners; the lane-wise smaller product
// vector gives the minimum and the larger one the maximum, each summed as (x + y) + z, then the translation is added.
//
// The result lives in VU0 state as well: the function leaves VF1..VF10 (and the MAC / STATUS flags) behind, and VF4,
// VF7 and VF10 keep their w lane from guest stack words the function never wrote (entry_sp - 48 / -64 / -80), so those
// three quadwords are read. The VU0 operations are the macro-mode primitives (flush / clamp / min / max, 0 + x turning
// -0 into +0). Loads and stores follow the original's order because the object's quadwords can alias the matrix.
// Straight line, 77 instructions. A call whose lqc2 / sqc2 / lq would fault is declined so the original faults itself.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kMatrixPointer = 4U;
constexpr GuestAddress kBoxPointer = 8U;
constexpr GuestAddress kWorldMin = 16U;
constexpr GuestAddress kWorldMax = 32U;
constexpr GuestAddress kRow1 = 16U;
constexpr GuestAddress kRow2 = 32U;
constexpr GuestAddress kTranslation = 48U;
constexpr GuestAddress kBoxMax = 16U;
constexpr GuestAddress kStaleX = 48U;   // the frame quadwords below the entry sp that VF10 / VF4 / VF7 start from
constexpr GuestAddress kStaleY = 64U;
constexpr GuestAddress kStaleZ = 80U;
constexpr std::uint32_t kRetired = 77U;

// VU0 macro masks and lanes.
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXyz = 14U;

// lqc2 / sqc2 fault unless the address is 16-byte aligned and inside guest memory.
template <class Memory>
bool vquad_ok(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

// lq / sq ignore the low four address bits.
template <class Memory>
bool quad_ok(const Memory& m, const GuestAddress address) {
    return m.contains(address & ~15U, 16U);
}

template <class Context>
n3::Run transform_object_bounds(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(1U);
    const GuestAddress entry_sp = call.stack_pointer();

    if ((object & 3U) != 0U || !m.contains(object, 12U)) {
        return call.decline();
    }
    const GuestAddress matrix = m.read_u32_le(object + kMatrixPointer);
    const GuestAddress box = m.read_u32_le(object + kBoxPointer);
    const GuestAddress world_min = object + kWorldMin;
    const GuestAddress world_max = object + kWorldMax;
    const bool loadable = vquad_ok(m, matrix) && vquad_ok(m, matrix + kRow1) && vquad_ok(m, matrix + kRow2) &&
                          vquad_ok(m, matrix + kTranslation) && quad_ok(m, box) && quad_ok(m, box + kBoxMax) &&
                          vquad_ok(m, world_min) && vquad_ok(m, world_max) && vquad_ok(m, entry_sp - kStaleX) &&
                          vquad_ok(m, entry_sp - kStaleY) && vquad_ok(m, entry_sp - kStaleZ);
    if (!loadable) {
        return call.decline();
    }

    const auto lqc2 = [&](const unsigned vf, const GuestAddress address) {
        cpu.write_vu0_vector(vf, n3::load_quad_any(m, address));
    };
    const auto sqc2 = [&](const GuestAddress address, const unsigned vf) {
        n3::store_quad_any(m, address, cpu.read_vu0_vector(vf));
    };
    // vadd<lane>.<mask> fd, fs, ft<lane>
    const auto add_lane = [&](const unsigned fd, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                              const std::size_t lane) { ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, false); };

    // The matrix columns, built over the stale VF10 / VF4 / VF7: VF10 = (r0.x, r1.x, r2.x), VF4 = y column, VF7 = z.
    lqc2(10U, entry_sp - kStaleX);
    lqc2(6U, matrix);
    const Gpr128 box_min = n3::load_quad_any(m, box & ~15U);
    lqc2(4U, entry_sp - kStaleY);
    lqc2(7U, entry_sp - kStaleZ);
    const Gpr128 box_max = n3::load_quad_any(m, (box + kBoxMax) & ~15U);
    add_lane(10U, 0U, 6U, kX, 0U);
    lqc2(5U, matrix + kRow1);
    add_lane(4U, 0U, 6U, kX, 1U);
    add_lane(7U, 0U, 6U, kX, 2U);
    lqc2(2U, matrix + kRow2);
    add_lane(10U, 0U, 5U, kY, 0U);
    add_lane(4U, 0U, 5U, kY, 1U);
    add_lane(7U, 0U, 5U, kY, 2U);
    cpu.write_vu0_vector(3U, box_max);
    add_lane(10U, 0U, 2U, kZ, 0U);
    add_lane(4U, 0U, 2U, kZ, 1U);
    cpu.write_vu0_vector(1U, box_min);
    add_lane(7U, 0U, 2U, kZ, 2U);

    // Per world axis: both corners scaled by the column, then the lane-wise smaller / larger product.
    ee_cop2::vmul(cpu, 2U, 10U, 1U, kXyz);
    ee_cop2::vmul(cpu, 5U, 4U, 1U, kXyz);
    ee_cop2::vmul(cpu, 6U, 4U, 3U, kXyz);
    ee_cop2::vmul(cpu, 8U, 10U, 3U, kXyz);
    ee_cop2::vmul(cpu, 4U, 7U, 3U, kXyz);
    ee_cop2::vmul(cpu, 1U, 7U, 1U, kXyz);
    ee_cop2::vmini(cpu, 7U, 8U, 2U, kXyz);   // x minimum
    ee_cop2::vmax(cpu, 3U, 8U, 2U, kXyz);    // x maximum
    ee_cop2::vmini(cpu, 2U, 6U, 5U, kXyz);   // y minimum
    ee_cop2::vmax(cpu, 6U, 6U, 5U, kXyz);    // y maximum
    ee_cop2::vmini(cpu, 5U, 4U, 1U, kXyz);   // z minimum
    ee_cop2::vmax(cpu, 4U, 4U, 1U, kXyz);    // z maximum

    // (x + y) + z of each, 0 + sum into the object's lane, stored after every lane as the original does.
    add_lane(1U, 7U, 7U, kX, 1U);
    lqc2(9U, world_min);
    add_lane(1U, 1U, 7U, kX, 2U);
    add_lane(9U, 0U, 1U, kX, 0U);
    sqc2(world_min, 9U);
    add_lane(1U, 3U, 3U, kX, 1U);
    add_lane(3U, 1U, 3U, kX, 2U);
    lqc2(7U, world_max);
    add_lane(1U, 2U, 2U, kX, 1U);
    add_lane(7U, 0U, 3U, kX, 0U);
    sqc2(world_max, 7U);
    add_lane(2U, 1U, 2U, kX, 2U);
    lqc2(7U, world_min);
    add_lane(1U, 6U, 6U, kX, 1U);
    add_lane(7U, 0U, 2U, kY, 0U);
    add_lane(3U, 1U, 6U, kX, 2U);
    sqc2(world_min, 7U);
    lqc2(6U, world_max);
    add_lane(1U, 5U, 5U, kX, 1U);
    add_lane(6U, 0U, 3U, kY, 0U);
    sqc2(world_max, 6U);
    add_lane(2U, 1U, 5U, kX, 2U);
    lqc2(3U, world_min);
    add_lane(1U, 4U, 4U, kX, 1U);
    add_lane(3U, 0U, 2U, kZ, 0U);
    sqc2(world_min, 3U);
    lqc2(2U, world_max);
    add_lane(1U, 1U, 4U, kX, 2U);
    add_lane(2U, 0U, 1U, kZ, 0U);
    sqc2(world_max, 2U);

    // Both corners move by the translation (read from the matrix again each time).
    lqc2(1U, matrix + kTranslation);
    lqc2(2U, world_min);
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, false);
    sqc2(world_min, 2U);
    lqc2(1U, matrix + kTranslation);
    lqc2(2U, world_max);
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, false);
    sqc2(world_max, 2U);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001b4ab0, 308U, 0x54999647e69ef76fULL, "TransformObjectBounds", 77U, transform_object_bounds,
               EngineReturn::Nothing)
