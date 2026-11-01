// 0x002f7390 (a0 = point P, a1 = group index A): FindBoxOverlapAtPoint. P is a quadword (xyz used); the group A is the
// 48-byte record at 0x0055efec + 48 * A whose first word is the group record G. Returns 1 as soon as P (grown by a
// margin of 1.5) touches one of the oriented boxes below, else 0. Three searches run in order, each testing P first
// against a sphere and then against the box, with the same six-comparison box test:
//
//   1. when byte G+528 is non-zero: the objects listed at 0x00522660 + 0x122a10 (count at +0x122b1c) whose signed
//      byte +536 equals G's signed byte +536;
//   2. every group record 0 .. count-1 (count at 0x00522660 + 0x12ad1c) taken from the same 48-byte table;
//   3. the boxes of group A listed in the byte table at 0x0055d064 + 254 * A + j (j < byte 0x0055d790 + A), each box
//      a 384-byte record at 0x00539c10 + 384 * index (matrix rows at +112..+160, bounds from the owner at +176).
//
// Sphere test: |object centre (+48 of the pointer at +516) - P|^2 <= (w of the quad at +448 + 1.5)^2. Box test: P is
// moved into the box frame (matrix rows +112, +128, +144, translation +160) as T, then
// T >= H - 1.5 (H = quad +480) and T <= L + 1.5 (L = quad +464) on x, y and z, which the original evaluates as
// !(T < H - 1.5) and (T <= L + 1.5). The listing writes the intermediate quads to its own stack frame and reads some
// back; the native keeps them in locals (stack stores are not part of the contract). Every VU0 operation runs on the
// real VU0 state in the original order (VF1..VF14, ACC, MAC and STATUS flags), and the 128-bit registers that feed
// QMTC2 keep their entry upper doublewords.
//
// The loops depend on the data, so the retired count is exact only through the dry run (N3_ENTRY). Loads that the
// interpreter would fault on (lqc2 alignment, addresses outside guest memory) decline the call.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kGroupTable = 0x0055efecU;       // lui 0x56, addiu -4116: 48-byte group rows
constexpr GuestAddress kGlobalBase = 0x00522660U;       // lui 0x52, addiu 9824
constexpr GuestAddress kObjectList = 0x00122a10U;       // ori: object pointers at base + this + 4 * i
constexpr GuestAddress kObjectCount = 0x00122b1cU;      // ori: word at base + this
constexpr GuestAddress kGroupCount = 0x0012ad1cU;       // ori: word at base + this
constexpr GuestAddress kBoxBase = 0x00527440U;          // lui 0x52, addiu 29760
constexpr GuestAddress kBoxIndexes = 0x00035c24U;       // ori: indexes at kBoxBase + 254 * A + this + j
constexpr GuestAddress kBoxCounts = 0x00036350U;        // ori: byte at kBoxBase + A + this
constexpr GuestAddress kBoxRecords = 0x000127d0U;       // ori: record at kBoxBase + this + 384 * index
constexpr std::uint32_t kMargin = 0x3fc00000U;          // 1.5f
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kBlockCost[3][6] = {
    {31U, 10U, 10U, 10U, 11U, 12U},   // phase 1: 0x74b8, 0x7544, 0x757c, 0x75b4, 0x75ec, 0x7628
    {31U, 10U, 10U, 10U, 10U, 11U},   // phase 2: 0x7870, 0x78fc, 0x7934, 0x796c, 0x79a4, 0x79dc
    {44U, 10U, 10U, 10U, 10U, 10U},   // phase 3: 0x7a20, 0x7ae0, 0x7b18, 0x7b50, 0x7b88, 0x7bc0
};
constexpr std::uint32_t kCheckCost[6] = {3U, 2U, 2U, 2U, 2U, 2U};   // andi/beq (and nop) after each test

// Memory with the interpreter's fault conditions: an access that would fault returns zero and sets the flag, and
// the body declines at the end of the step (the dry run decides before the live run changes anything).
template <class Memory>
class Access {
public:
    explicit Access(Memory& memory) noexcept : memory_(memory) {}
    [[nodiscard]] bool fault() const noexcept { return fault_; }
    // lqc2: 16-byte aligned.
    [[nodiscard]] Gpr128 vquad(const GuestAddress address) {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            fault_ = true;
            return Gpr128{};
        }
        return n3::load_quad_any(memory_, address);
    }
    // lq: the low four address bits are ignored.
    [[nodiscard]] Gpr128 quad(const GuestAddress address) {
        if (!memory_.contains(address & ~15U, 16U)) {
            fault_ = true;
            return Gpr128{};
        }
        return n3::load_quad_any(memory_, address);
    }
    // lw / lwc1: 4-byte aligned.
    [[nodiscard]] std::uint32_t word(const GuestAddress address) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            fault_ = true;
            return 0U;
        }
        return memory_.read_u32_le(address);
    }
    // lbu / lb: any address inside guest memory.
    [[nodiscard]] std::uint32_t byte(const GuestAddress address) {
        if (!memory_.contains(address, 1U)) {
            fault_ = true;
            return 0U;
        }
        return memory_.read_u8(address);
    }

private:
    Memory& memory_;
    bool fault_ = false;
};

// A 32-bit result (lui, mfc1...) written to a 128-bit register: the low doubleword is the sign-extended word, the
// high doubleword keeps what the register held.
[[nodiscard]] Gpr128 with_low_word(Gpr128 reg, const std::uint32_t bits) {
    reg.words[0] = bits;
    reg.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return reg;
}

struct Chain {
    bool pass;
    std::uint32_t retired;
};

// The six comparisons of a box test. eval(k) runs test k's VU0 operations and returns the original's boolean; a test
// is only evaluated while every earlier one passed, and each later check still costs its two instructions.
template <class Eval>
Chain run_chain(const unsigned phase, Eval&& eval) {
    std::uint32_t retired = 0U;
    bool pass = true;
    for (unsigned k = 0U; k < 6U; ++k) {
        if (pass) {
            const bool flag = eval(k);
            retired += kBlockCost[phase][k] + (flag ? 0U : 1U) + (k > 0U ? 1U : 0U);
            pass = flag;
        }
        retired += kCheckCost[k];
    }
    return {pass, retired};
}

// The matrix-transformed point of phases 1 and 2 and its six comparisons (0x74b8..0x7660 / 0x7870..0x7a10).
// object is the pointer the listing keeps in t6 / t5.
template <class Cpu, class Mem>
Chain object_box_test(Cpu& cpu, Mem& access, const unsigned phase, const GuestAddress object) {
    const Gpr128 m0 = access.quad(object + 112U);
    const Gpr128 low = access.quad(object + 464U);
    const Gpr128 high = access.quad(object + 480U);
    const Gpr128 m1 = access.quad(object + 128U);
    const Gpr128 m2 = access.quad(object + 144U);
    const Gpr128 m3 = access.quad(object + 160U);
    cpu.write_vu0_vector(1U, m0);                                                  // qmtc2 s2,vf1
    ee_cop2::vmula_broadcast(cpu, 1U, 2U, 14U, false);                             // vmulax.xyz acc,vf1,vf2x
    cpu.write_vu0_vector(1U, m1);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 1U, false, true, false);        // vmadday.xyz acc,vf1,vf2y
    cpu.write_vu0_vector(1U, m2);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 2U, false, true, false);        // vmaddaz.xyz acc,vf1,vf2z
    cpu.write_vu0_vector(1U, m3);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 14U, 3U, false, false, false);       // vmaddw.xyz vf1,vf1,vf0w
    const Gpr128 point = cpu.read_vu0_vector(1U);                                  // qmfc2 s0,vf1
    return run_chain(phase, [&](const unsigned k) {
        if (k < 3U) {
            cpu.write_vu0_vector(1U, high);                                        // qmtc2 / lqc2 vf1 = H
            ee_cop2::vadd_broadcast(cpu, 1U, 1U, 5U, static_cast<std::uint8_t>(8U >> k), 0U, true);   // vsubx.x/y/z
            const std::uint32_t t = point.words[k];
            const std::uint32_t bound = cpu.read_vu0_vector(1U).words[k];
            return !fpu::lt(t, bound);
        }
        cpu.write_vu0_vector(1U, low);                                             // lqc2 vf1,1600($sp) = L
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 5U, static_cast<std::uint8_t>(8U >> (k - 3U)), 0U, false);   // vaddx.x/y/z
        const std::uint32_t t = point.words[k - 3U];
        const std::uint32_t bound = cpu.read_vu0_vector(1U).words[k - 3U];
        return fpu::le(t, bound);
    });
}

// Sphere test of phases 1 and 2 (0x7474..0x74b4 / 0x76fc..0x7738): the object's centre against P.
template <class Cpu, class Mem>
bool sphere_test(Cpu& cpu, Mem& access, const GuestAddress object) {
    const GuestAddress holder = access.word(object + 516U);                        // lw s0,516(t6)
    cpu.write_vu0_vector(4U, access.vquad(object + 448U));                         // lqc2 vf4,448(t6)
    cpu.write_vu0_vector(1U, access.vquad(holder + 48U));                          // lqc2 vf1,48(s0)
    ee_cop2::vadd_broadcast(cpu, 4U, 4U, 5U, 1U, 0U, false);                       // vaddx.w vf4,vf4,vf5x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, 14U, true);                              // vsub.xyz vf1,vf1,vf2
    const std::uint32_t radius = cpu.read_vu0_vector(4U).words[3];                 // lwc1 f0,1420($sp)
    ee_cop2::vmul(cpu, 1U, 1U, 1U, 14U);                                           // vmul.xyz vf1,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                                 // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 3U, 1U, 8U, 2U, false, false, false);        // vmaddz.x vf1,vf3,vf1z
    const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];               // qmfc2 s0,vf1; mtc1 s0,f1
    return fpu::le(distance, fpu::mul(radius, radius));                            // mul.s f0,f0,f0; c.ole.s f1,f0
}

template <class Context>
n3::Run find_box_overlap_at_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    Access access{call.memory()};
    if ((call.stack_pointer() & 15U) != 0U) {
        return call.decline();                                   // lqc2 vf2,1632($sp) would fault
    }
    // Entry upper doublewords that QMTC2 copies into VF5..VF11 together with the low word.
    const Gpr128 entry_v0 = cpu.read_gpr(2U);
    const Gpr128 entry_t0 = cpu.read_gpr(8U);
    const Gpr128 entry_t1 = cpu.read_gpr(9U);
    const Gpr128 entry_t2 = cpu.read_gpr(10U);
    const Gpr128 entry_t3 = cpu.read_gpr(11U);
    const std::uint32_t index = call.arg(1U);
    const Gpr128 point = access.quad(call.arg(0U));              // lq a0,0(a0)
    const GuestAddress group = access.word(kGroupTable + 48U * index);
    const std::uint32_t phase1_flag = access.byte(group + 528U);
    if (access.fault()) {
        return call.decline();
    }
    std::uint32_t retired = 25U;                                 // 0x2f7390..0x2f73f0
    std::uint32_t result = 0U;

    const auto finish = [&](const std::uint32_t value) {
        if (access.fault()) {
            return call.decline();
        }
        call.return_value(value);
        return call.finish(retired + 11U);                       // 0x2f7bfc..0x2f7c24
    };

    if (phase1_flag != 0U) {
        // Phase 1: the listed objects whose key byte equals the group's.
        retired += 25U;                                          // 0x2f73f4..0x2f7454
        const auto key = static_cast<std::int8_t>(access.byte(group + 536U));
        cpu.write_vu0_vector(5U, with_low_word(entry_t2, kMargin));      // qmtc2 t2,vf5
        const std::int32_t count = static_cast<std::int32_t>(access.word(kGlobalBase + kObjectCount));
        cpu.write_vu0_vector(2U, point);                                 // lqc2 vf2,1632($sp)
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, 8U, 3U, false);         // vaddw.x vf3,vf0,vf0w
        for (std::uint32_t i = 0U;; ++i) {
            retired += 3U;                                       // 0x2f7670: slt, bne, nop
            if (!(static_cast<std::int32_t>(i) < count)) {
                break;
            }
            if (retired > kCeiling || access.fault()) {
                return access.fault() ? call.decline() : call.finish(retired);
            }
            retired += 7U;                                       // 0x2f7458..0x2f7470
            const GuestAddress object = access.word(kGlobalBase + kObjectList + 4U * i);
            if (key != static_cast<std::int8_t>(access.byte(object + 536U))) {
                retired += 2U;                                   // 0x2f7868
                continue;
            }
            retired += 17U;                                      // 0x2f7474..0x2f74b4
            if (!sphere_test(cpu, access, object)) {
                retired += 2U;
                continue;
            }
            const Chain chain = object_box_test(cpu, access, 0U, object);
            retired += chain.retired + 2U;                       // 0x2f7668 or 0x2f7868
            if (chain.pass) {
                return finish(1U);
            }
        }
    }

    // Phase 2: every group's record in the 48-byte table.
    retired += 26U;                                              // 0x2f767c..0x2f76e0
    const std::int32_t groups = static_cast<std::int32_t>(access.word(kGlobalBase + kGroupCount));
    cpu.write_vu0_vector(2U, point);                             // lqc2 vf2,1632($sp)
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, 8U, 3U, false);     // vaddw.x vf3,vf0,vf0w
    cpu.write_vu0_vector(5U, with_low_word(point, kMargin));     // qmtc2 a0,vf5 (a0 = lui 0x3fc0 over the quad)
    for (std::uint32_t g = 0U;; ++g) {
        retired += 3U;                                           // 0x2f7744: slt, bne, nop
        if (!(static_cast<std::int32_t>(g) < groups)) {
            break;
        }
        if (retired > kCeiling || access.fault()) {
            return access.fault() ? call.decline() : call.finish(retired);
        }
        retired += 23U;                                          // 0x2f76e4..0x2f773c
        const GuestAddress object = access.word(kGroupTable + 48U * g);
        if (!sphere_test(cpu, access, object)) {
            retired += 1U;                                       // 0x2f7740
            continue;
        }
        const Chain chain = object_box_test(cpu, access, 1U, object);
        retired += chain.retired;
        if (chain.pass) {
            retired += 2U;                                       // 0x2f7a18
            return finish(1U);
        }
        retired += 1U;                                           // 0x2f7740
    }

    // Phase 3: the boxes listed for the group A.
    retired += 41U;                                              // 0x2f7750..0x2f77f0
    cpu.write_vu0_vector(1U, point);                             // lqc2 vf1,1632($sp)
    cpu.write_vu0_vector(11U, with_low_word(entry_v0, kMargin));
    cpu.write_vu0_vector(9U, with_low_word(entry_t3, kMargin));
    cpu.write_vu0_vector(8U, with_low_word(entry_t2, kMargin));
    cpu.write_vu0_vector(7U, with_low_word(entry_t1, kMargin));
    cpu.write_vu0_vector(6U, with_low_word(entry_t0, kMargin));
    cpu.write_vu0_vector(10U, with_low_word(entry_v0, kMargin));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 8U, 3U, false);     // vaddw.x vf2,vf0,vf0w
    const GuestAddress row = kBoxBase + 254U * index;
    const std::int32_t boxes = static_cast<std::int32_t>(access.byte(kBoxBase + index + kBoxCounts));
    for (std::uint32_t j = 0U;; ++j) {
        retired += 3U;                                           // 0x2f7854: slt, bne, nop
        if (!(static_cast<std::int32_t>(j) < boxes)) {
            break;
        }
        if (retired > kCeiling || access.fault()) {
            return access.fault() ? call.decline() : call.finish(retired);
        }
        retired += 23U;                                          // 0x2f77f4..0x2f784c
        const std::uint32_t box_index = access.byte(row + j + kBoxIndexes);
        const GuestAddress record = kBoxBase + 384U * box_index + kBoxRecords;
        const GuestAddress owner = access.word(record + 176U);
        cpu.write_vu0_vector(3U, access.vquad(record + 160U));                   // lqc2 vf3,160(a1)
        const std::uint32_t owner_radius = access.word(owner + 20U);             // lwc1 f0,20(t0)
        ee_cop2::vadd_vector(cpu, 3U, 3U, 1U, 14U, true);                        // vsub.xyz vf3,vf3,vf1
        ee_cop2::vmul(cpu, 3U, 3U, 3U, 14U);                                     // vmul.xyz vf3,vf3,vf3
        const std::uint32_t radius = fpu::add(kMargin, owner_radius);            // add.s f0,f2,f0
        ee_cop2::vadda_broadcast(cpu, 3U, 3U, 8U, 1U);                           // vadday.x acc,vf3,vf3y
        ee_cop2::vmadd_broadcast(cpu, 3U, 2U, 3U, 8U, 2U, false, false, false);  // vmaddz.x vf3,vf2,vf3z
        const std::uint32_t distance = cpu.read_vu0_vector(3U).words[0];
        if (!fpu::le(distance, fpu::mul(radius, radius))) {
            retired += 1U;                                       // 0x2f7850
            continue;
        }
        // The box moves P into the owner's frame: rows vf5, vf13 (lq 112/128/144) and the translation vf3.
        const Gpr128 bound_high = access.quad(owner + 3712U);   // lq t2,3712(t0)
        const Gpr128 bound_low = access.quad(owner + 3728U);    // lq t1,3728(t0)
        // (the listing stores them to 1600($sp) and 1616($sp); the tests read them back as vf3 sources)
        cpu.write_vu0_vector(5U, access.vquad(record + 128U));                   // lqc2 vf5,128(a1)
        cpu.write_vu0_vector(13U, access.vquad(record + 144U));                  // lqc2 vf13,144(a1)
        const Gpr128 row0 = access.quad(record + 112U);                          // lq t0,112(a1)
        ee_cop2::vmove(cpu, 12U, 5U, 15U);                                       // vmove.xyzw vf12,vf5
        cpu.write_vu0_vector(3U, access.vquad(record + 160U));                   // lqc2 vf3,160(a1)
        cpu.write_vu0_vector(14U, row0);                                         // qmtc2 t0,vf14
        ee_cop2::vmove(cpu, 4U, 14U, 15U);                                       // vmove.xyzw vf4,vf14
        ee_cop2::vadd_broadcast(cpu, 14U, 0U, 5U, 4U, 0U, false);                // vaddx.y vf14,vf0,vf5x
        ee_cop2::vadd_broadcast(cpu, 12U, 0U, 4U, 8U, 1U, false);                // vaddy.x vf12,vf0,vf4y
        ee_cop2::vadd_broadcast(cpu, 14U, 0U, 13U, 2U, 0U, false);               // vaddx.z vf14,vf0,vf13x
        ee_cop2::vadd_broadcast(cpu, 12U, 0U, 13U, 2U, 1U, false);               // vaddy.z vf12,vf0,vf13y
        ee_cop2::vadd_broadcast(cpu, 13U, 0U, 4U, 8U, 2U, false);                // vaddz.x vf13,vf0,vf4z
        ee_cop2::vadd_broadcast(cpu, 13U, 0U, 5U, 4U, 2U, false);                // vaddz.y vf13,vf0,vf5z
        ee_cop2::vmul_broadcast(cpu, 5U, 13U, 3U, 14U, 2U);                      // vmulz.xyz vf5,vf13,vf3z
        ee_cop2::vmul_broadcast(cpu, 4U, 14U, 3U, 14U, 0U);                      // vmulx.xyz vf4,vf14,vf3x
        ee_cop2::vmul_broadcast(cpu, 3U, 12U, 3U, 14U, 1U);                      // vmuly.xyz vf3,vf12,vf3y
        ee_cop2::vadd_vector(cpu, 3U, 4U, 3U, 14U, false);                       // vadd.xyz vf3,vf4,vf3
        ee_cop2::vadd_vector(cpu, 3U, 3U, 5U, 14U, false);                       // vadd.xyz vf3,vf3,vf5
        ee_cop2::vadd_vector(cpu, 3U, 0U, 3U, 14U, true);                        // vsub.xyz vf3,vf0,vf3
        ee_cop2::vmula_broadcast(cpu, 14U, 1U, 14U, false);                      // vmulax.xyz acc,vf14,vf1x
        ee_cop2::vmadd_broadcast(cpu, 0U, 12U, 1U, 14U, 1U, false, true, false); // vmadday.xyz acc,vf12,vf1y
        ee_cop2::vmadd_broadcast(cpu, 0U, 13U, 1U, 14U, 2U, false, true, false); // vmaddaz.xyz acc,vf13,vf1z
        ee_cop2::vmadd_broadcast(cpu, 3U, 3U, 0U, 14U, 3U, false, false, false); // vmaddw.xyz vf3,vf3,vf0w
        const Gpr128 local = cpu.read_vu0_vector(3U);                            // qmfc2 t0,vf3
        const Chain chain = run_chain(2U, [&](const unsigned k) {
            if (k < 3U) {
                cpu.write_vu0_vector(3U, bound_low);                             // qmtc2 / lqc2 vf3 = [1616]
                ee_cop2::vadd_broadcast(cpu, 3U, 3U, 11U - k, static_cast<std::uint8_t>(8U >> k), 0U, true);
                return !fpu::lt(local.words[k], cpu.read_vu0_vector(3U).words[k]);
            }
            cpu.write_vu0_vector(3U, bound_high);                                // lqc2 vf3,1600($sp)
            ee_cop2::vadd_broadcast(cpu, 3U, 3U, 11U - k, static_cast<std::uint8_t>(8U >> (k - 3U)), 0U, false);
            return fpu::le(local.words[k - 3U], cpu.read_vu0_vector(3U).words[k - 3U]);
        });
        retired += chain.retired + 1U;                           // 0x2f7bf8 or 0x2f7850
        if (chain.pass) {
            return finish(1U);
        }
    }
    retired += 2U;                                               // 0x2f7860: beq, daddu v0,zero,zero
    result = 0U;
    return finish(result);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f7390, 2200U, 0x45ee2da9d2908db0ULL, "FindBoxOverlapAtPoint", kCeiling, find_box_overlap_at_point,
         EngineReturn::Integer)
