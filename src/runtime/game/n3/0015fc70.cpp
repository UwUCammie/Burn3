// 0x0015fc70 (a0 = a pointer to the key table header, a1 = an object): ChooseNeighbourSlot. The object's bytes at +68
// and +69 are slot indices into the 80-byte slots at 0x00551910 (255 = no slot). With one slot the result is that
// slot's address; with none it is 0. With two, the key table (the header's u32 at +4 points at 8-byte keys, each key a
// float at +0; the u32 at +16 is the key count) is evaluated at each slot's float at +48 and at the object's own float
// at +48: the result is slot A when (lookup(A) - lookup(object)) < (lookup(object) - lookup(B)), else slot B. The
// original writes no memory and has no loops or calls: 11 to 151 retired instructions, declared 151.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kSlots = 0x00551910U;   // lui 0x52 + addiu 9824, plus lui 0x2 + ori 0xf2b0
constexpr std::uint32_t kSlotBytes = 80U;
constexpr std::uint32_t kNoSlot = 255U;

constexpr GuestAddress slot_address(const std::uint32_t index) { return kSlots + index * kSlotBytes; }

// The key lookup of one float (bits `value`): t = min(int(value), count); the curve is key t-1 (0 below key 0). When
// t < count the curve moves toward key t: curve = ACC + (value - t) * (key t - curve), with ACC = 0 + curve from
// ADDA.S. `interpolate` is the instruction count of that block. ACC is left in `acc`, the count added to `retired`.
template <class Memory>
std::uint32_t curve_value(const Memory& m, const GuestAddress keys, const std::int32_t count, const std::uint32_t value,
                          const std::uint32_t interpolate, std::uint32_t& acc, std::uint32_t& retired) {
    std::int32_t t = fpu::to_int(value);
    if (count < t) {
        t = count;
        retired += 1U;                                // daddu t0,a0,zero
    }
    const std::uint32_t below = static_cast<std::uint32_t>(t) - 1U;
    retired += 3U;                                    // addiu; bltz; nop
    std::uint32_t curve = 0U;
    if (static_cast<std::int32_t>(below) < 0) {
        retired += 3U;                                // mtc1 zero; beq; slt (delay slot)
    } else {
        curve = m.read_u32_le(keys + (below << 3U));  // sll; addu; lwc1
        retired += 4U;                                // sll; addu; lwc1; slt
    }
    retired += 2U;                                    // beq at,zero; nop
    if (t < count) {
        const std::uint32_t upper = m.read_u32_le(keys + (static_cast<std::uint32_t>(t) << 3U));
        const std::uint32_t fraction = fpu::sub(value, fpu::from_int(t));
        acc = fpu::add(0U, curve);                    // adda.s ACC, f0 (zero), curve
        const std::uint32_t slope = fpu::sub(upper, curve);
        curve = fpu::madd(acc, fraction, slope);      // madd.s curve, fraction, slope
        retired += interpolate;
    }
    return curve;
}

template <class Context>
n3::Run choose_neighbour_slot(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress header_pointer = call.arg(0);
    const GuestAddress object = call.arg(1);

    std::uint32_t retired = 0U;
    std::uint32_t acc = cpu.fpu_accumulator_bits();
    const auto finish_with = [&](const std::uint32_t result) {
        cpu.set_fpu_accumulator_bits(acc);
        call.return_value(result);
        return call.finish(retired + 2U);             // jr ra; nop (0x0015ff98)
    };

    // 0x0015fc70: lbu a3,68(a1); addiu v0,zero,255; bne a3,v0 (not taken: no slot A); nop
    const std::uint32_t index_a = m.read_u8(object + 68U);
    retired += 4U;
    if (index_a == kNoSlot) {
        // 0x0015fc80: lbu a0,69(a1); beq a0,v0 (taken when there is no slot B); nop
        const std::uint32_t index_b = m.read_u8(object + 69U);
        retired += 3U;
        if (index_b == kNoSlot) {
            retired += 2U;                            // 0x0015ff0c: beq zero,zero; daddu v0,zero,zero
            return finish_with(0U);
        }
        // 0x0015fc8c: bne a0,v0 (taken: slot B is not 255); nop. 0x0015fc9c: v0 = slot B (9). 0x0015fcc0: beq; nop
        retired += 2U + 9U + 2U;
        return finish_with(slot_address(index_b));
    }

    // 0x0015fcc8: lbu a2,69(a1); beq a2,v0 (taken when there is no slot B); nop
    const std::uint32_t index_b = m.read_u8(object + 69U);
    retired += 3U;
    if (index_b == kNoSlot) {
        // 0x0015ff14: bne a3,v0 (taken: slot A is not 255); nop. 0x0015ff24: v0 = slot A (9). 0x0015ff48: beq; nop
        retired += 2U + 9U + 2U;
        return finish_with(slot_address(index_a));
    }

    // 0x0015fcd4: bne a3,v0 (taken: slot A is not 255); nop. 0x0015fce4: v0 = slot A (9).
    // 0x0015fd08: addiu v1,zero,255; bne a2,v1 (taken: slot B is not 255); nop. 0x0015fd1c: a2 = slot B (10).
    retired += 2U + 9U + 3U + 10U;
    const GuestAddress slot_a_address = slot_address(index_a);
    const GuestAddress slot_b_address = slot_address(index_b);

    // 0x0015fd44: lw v1,0(a0); lwc1 f3,48(v0); lw a0,16(v1); cvt.w.s; mfc1; nop; slt; beq; lw v1,4(v1) (9)
    const GuestAddress keys_header = m.read_u32_le(header_pointer);
    const std::uint32_t value_a = m.read_u32_le(slot_a_address + 48U);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(keys_header + 16U));
    const GuestAddress keys = m.read_u32_le(keys_header + 4U);
    retired += 9U;
    const std::uint32_t curve_a = curve_value(m, keys, count, value_a, 11U, acc, retired);

    // 0x0015fdbc: lwc1 f3,48(a1); cvt.w.s; mfc1; nop; slt; beq; daddu t0,a3 (7)
    const std::uint32_t value_object = m.read_u32_le(object + 48U);
    retired += 7U;
    const std::uint32_t curve_object = curve_value(m, keys, count, value_object, 10U, acc, retired);

    // 0x0015fe28: slt; beq; sub.s f6,f5,f4 (the delay slot runs either way) (3)
    const std::uint32_t gap_a = fpu::sub(curve_a, curve_object);
    retired += 3U;
    // 0x0015fe38 onwards: the object's curve again, at t = min(int(value_object), count)
    const std::uint32_t curve_object_again = curve_value(m, keys, count, value_object, 11U, acc, retired);

    // 0x0015fe88: lwc1 f4,48(a2); cvt.w.s; mfc1; nop; slt; beq; nop (7)
    const std::uint32_t value_b = m.read_u32_le(slot_b_address + 48U);
    retired += 7U;
    const std::uint32_t curve_b = curve_value(m, keys, count, value_b, 12U, acc, retired);

    // 0x0015ff80: sub.s f0,f5,f3; c.olt.s f6,f0; bc1f; nop (4)
    const bool choose_a = fpu::lt(gap_a, fpu::sub(curve_object_again, curve_b));
    cpu.set_fcr31_bits(choose_a ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                                : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
    retired += 4U;
    if (choose_a) {
        retired += 4U;                                // 0x0015ff90: beq; nop. 0x0015fefc: beq; nop
        return finish_with(slot_a_address);
    }
    retired += 2U;                                    // 0x0015ff04: beq zero,zero; daddu v0,a2,zero
    return finish_with(slot_b_address);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0015fc70, 816U, 0x9a03d14e68c5ea5eULL, "ChooseNeighbourSlot", 151U, choose_neighbour_slot, EngineReturn::Integer)
