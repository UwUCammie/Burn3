// 0x002b7db0 (a0 = int* clip index, a1 = plane set, a2 = quad A (segment start), a3 = quad D (direction)):
// ClipRayToPlaneSet. B = A + 100 D. The plane set holds a pointer to 16-byte planes (n.xyz, w) at +4 and a
// count byte at +25; both are re-read on every pass. Plane i stores dB = dot(B, n) at frame word sp + 112 + 4i
// (for i >= 48 that word is in the caller's frame, and it is stored there). dB < w passes the plane. Otherwise
// *a0 = i and dA = dot(A, n): dA not below w returns L (the float at gp - 32740); |dB - dA| <= E (the float at
// gp - 32736) passes the plane as parallel; else B = A + (B - A) t with t = (w - dA) / (dB - dA). When no plane
// was hit *a0 is -1, so the result is L and *a0 becomes 0. Otherwise the result is |B - A| (VU0 square root),
// or L when that is below L.
//
// The listing runs instruction by instruction on the cpu state and memory, with the same VU0, FPU and compare
// primitives the interpreter uses, so the vector, accumulator, Q and register state match as well.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::int32_t kEpsilonOffset = -32736;    // float E at $gp + kEpsilonOffset
constexpr std::int32_t kLimitOffset = -32740;      // float L at $gp + kLimitOffset
constexpr std::uint32_t kFrameBytes = 304U;
constexpr std::uint32_t kScaleBits = 0x42c80000U;  // 100.0f (lui $a2, 0x42c8)
constexpr std::uint8_t kMaskX = 8U;
constexpr std::size_t kLaneX = 0U;
constexpr std::size_t kLaneY = 1U;
constexpr std::size_t kLaneZ = 2U;
constexpr std::size_t kLaneW = 3U;

// Retired instruction counts of the listing's blocks, delay slots included (brief.py).
constexpr std::uint32_t kEntryCost = 21U;        // 0x2b7db0..0x2b7e00: setup, then the jump to the loop test
constexpr std::uint32_t kTestCost = 4U;          // 0x2b7e50..0x2b7e5c: lbu, slt, bne and its delay slot
constexpr std::uint32_t kPlaneCost = 18U;        // 0x2b7e04..0x2b7e48: dB and its test
constexpr std::uint32_t kStepCost = 1U;          // 0x2b7e4c: the plane index step
constexpr std::uint32_t kHitCost = 12U;          // 0x2b7ed8..0x2b7f04: *a0 = i, dA and its test
constexpr std::uint32_t kParallelTestCost = 5U;  // 0x2b7f08..0x2b7f18: |dB - dA| <= E
constexpr std::uint32_t kClipCost = 14U;         // 0x2b7f1c..0x2b7f50: the clip point
constexpr std::uint32_t kOutsideCost = 5U;       // 0x2b7f54..0x2b7f6c: f0 = L and return
constexpr std::uint32_t kExitCost = 3U;          // 0x2b7e60..0x2b7e68: lw, bltz and its delay slot
constexpr std::uint32_t kNoClipCost = 5U;        // 0x2b7f5c..0x2b7f6c: f0 = L, *a0 = 0 and return
constexpr std::uint32_t kLengthCost = 24U;       // 0x2b7e6c..0x2b7ec8: |B - A| and its compare with L
constexpr std::uint32_t kLengthKeptCost = 3U;    // 0x2b7f64..0x2b7f6c: len >= L, return with f0 = len
constexpr std::uint32_t kLengthLimitCost = 5U;   // 0x2b7ecc..0x2b7ed4, 0x2b7f68..0x2b7f6c: f0 = L and return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The native's view of guest memory: direct accesses. lq/sq mask the address to 16 bytes, lqc2/sqc2 need it
// 16-byte aligned (the resolver has already declined any call that would not be).
template <class Base>
class LiveMemory {
public:
    explicit LiveMemory(Base& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint8_t read_byte(const GuestAddress address) { return memory_.read_u8(address); }
    [[nodiscard]] std::uint32_t read_word(const GuestAddress address) { return memory_.read_u32_le(address); }
    void write_word(const GuestAddress address, const std::uint32_t value) { memory_.write_u32_le(address, value); }
    [[nodiscard]] guest::Gpr128 read_quad(const GuestAddress address) { return n3::load_quad_any(memory_, address); }
    void write_quad(const GuestAddress address, const guest::Gpr128& value) {
        n3::store_quad_any(memory_, address, value);
    }
    [[nodiscard]] guest::Gpr128 read_vu_quad(const GuestAddress address) { return n3::load_quad_any(memory_, address); }
    void write_vu_quad(const GuestAddress address, const guest::Gpr128& value) {
        n3::store_quad_any(memory_, address, value);
    }

private:
    Base& memory_;
};

// The view call_cost uses: an overlay of the original's writes (n3::DryMemory) that also records any access the
// original would fault on (lw, sw and lwc1 need 4-byte alignment, lqc2 and sqc2 16-byte, every access must be
// mapped). A faulting call is declined, so the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_byte(const GuestAddress address) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_word(const GuestAddress address) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_word(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] guest::Gpr128 read_quad(const GuestAddress address) { return quad_read(address & ~15U); }
    void write_quad(const GuestAddress address, const guest::Gpr128& value) { quad_write(address & ~15U, value); }
    [[nodiscard]] guest::Gpr128 read_vu_quad(const GuestAddress address) {
        if ((address & 15U) != 0U) {
            faulted_ = true;
            return {};
        }
        return quad_read(address);
    }
    void write_vu_quad(const GuestAddress address, const guest::Gpr128& value) {
        if ((address & 15U) != 0U) {
            faulted_ = true;
            return;
        }
        quad_write(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] guest::Gpr128 quad_read(const GuestAddress aligned) {
        if (!memory_.contains(aligned, 16U)) {
            faulted_ = true;
            return {};
        }
        return n3::load_quad_any(dry_, aligned);
    }
    void quad_write(const GuestAddress aligned, const guest::Gpr128& value) {
        if (!memory_.contains(aligned, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(aligned, value.words);
    }

    const Base& memory_;
    n3::DryMemory dry_;
    bool faulted_ = false;
};

// The listing, instruction by instruction, on the cpu state and memory. Returns the retired count. Used by the
// native itself (LiveMemory on the real state) and by call_cost (CheckedDryMemory on a copy of the state).
template <class Memory>
std::uint32_t clip_ray(Memory& m, guest::R5900State& cpu) {
    const GuestAddress out = cpu.read_gpr_u32(4U);                    // a0: int* clip index
    const GuestAddress planes = cpu.read_gpr_u32(5U);                 // a1: plane set
    const GuestAddress segment = cpu.read_gpr_u32(6U);                // a2: quad A
    const GuestAddress direction = cpu.read_gpr_u32(7U);              // a3: quad D
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress sp = cpu.read_gpr_u32(29U) - kFrameBytes;      // addiu $sp,$sp,-304 (restored at return)
    const GuestAddress limit_address = gp + static_cast<GuestAddress>(kLimitOffset);

    std::uint32_t retired = kEntryCost;
    cpu.write_gpr(9U, m.read_quad(segment));                          // lq $t1, 0($a2)
    cpu.write_vu0_vector(1U, m.read_vu_quad(direction));              // lqc2 $vf1, 0($a3)
    cpu.write_gpr_sign_extended_32(8U, 0xffffffffU);                  // addiu $t0, $zero, -1
    cpu.write_fpr_bits(2U, m.read_word(gp + static_cast<GuestAddress>(kEpsilonOffset)));  // lwc1 $f2
    cpu.write_gpr_u64(10U, 0U);                                       // daddu $t2, $zero, $zero
    cpu.write_gpr_sign_extended_32(3U, sp + 16U);                     // addiu $v1, $sp, 16
    cpu.write_gpr_sign_extended_32(2U, sp);                           // addiu $v0, $sp, 0
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, kMaskX, kLaneW, false); // vaddw.x $vf3, $vf0, $vf0w
    m.write_word(out, 0xffffffffU);                                   // sw $t0, 0($a0)
    cpu.write_gpr_sign_extended_32(6U, kScaleBits);                   // lui $a2, 0x42c8
    m.write_quad(sp + 288U, cpu.read_gpr(9U));                        // sq $t1, 288($sp)
    cpu.write_gpr_sign_extended_32(7U, sp + 32U);                     // addiu $a3, $sp, 32
    cpu.write_vu0_vector(2U, cpu.read_gpr(6U));                       // qmtc2 $a2, $vf2
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, kLaneX);  // vmulx.xyz $vf2, $vf1, $vf2x
    cpu.write_vu0_vector(1U, cpu.read_gpr(9U));                       // qmtc2 $t1, $vf1
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);       // vadd.xyz $vf2, $vf2, $vf1
    cpu.write_vu0_vector(5U, m.read_vu_quad(sp + 288U));              // lqc2 $vf5, 288($sp)
    m.write_vu_quad(sp + 96U, cpu.read_vu0_vector(2U));               // sqc2 $vf2, 96($sp)
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 0U, kMaskX, kLaneW, false); // vaddw.x $vf6, $vf0, $vf0w

    std::uint32_t index = 0U;                                         // $t2
    for (;;) {
        retired += kTestCost;
        const std::uint32_t count = m.read_byte(planes + 25U);        // lbu $a2, 25($a1)
        cpu.write_gpr_u64(6U, count);
        const bool more = static_cast<std::int32_t>(index) < static_cast<std::int32_t>(count);
        cpu.write_gpr_sign_extended_32(6U, more ? 1U : 0U);           // slt $a2, $t2, $a2
        if (!more) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }

        // Plane `index`: dB = dot(B, n), stored in the frame array and compared with w.
        cpu.write_gpr_sign_extended_32(6U, m.read_word(planes + 4U));  // lw $a2, 4($a1)
        const std::uint32_t offset = index << 4U;
        cpu.write_gpr_sign_extended_32(8U, offset);                   // sll $t0, $t2, 4
        cpu.write_vu0_vector(7U, m.read_vu_quad(sp + 96U));           // lqc2 $vf7, 96($sp)
        const GuestAddress plane = cpu.read_gpr_u32(6U) + offset;
        cpu.write_gpr_sign_extended_32(8U, plane);                    // addu $t0, $a2, $t0
        cpu.write_vu0_vector(2U, m.read_vu_quad(plane));              // lqc2 $vf2, 0($t0)
        ee_cop2::vmul(cpu, 1U, 7U, 2U, n3::kMaskXyz);                // vmul.xyz $vf1, $vf7, $vf2
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, kLaneY);       // vadday.x $acc, $vf1, $vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 6U, 1U, kMaskX, kLaneZ, false, false, false);  // vmaddz.x $vf1, $vf6, $vf1z
        cpu.write_gpr(9U, cpu.read_vu0_vector(1U));                   // qmfc2 $t1, $vf1
        m.write_vu_quad(cpu.read_gpr_u32(7U), cpu.read_vu0_vector(2U));  // sqc2 $vf2, 0($a3)
        cpu.write_gpr_sign_extended_32(6U, index << 2U);              // sll $a2, $t2, 2
        const GuestAddress slot = cpu.read_gpr_u32(6U) + sp;          // addu $a2, $a2, $sp
        cpu.write_gpr_sign_extended_32(6U, slot);
        m.write_word(slot + 112U, cpu.read_gpr_u32(9U));             // sw $t1, 112($a2)
        cpu.write_fpr_bits(0U, m.read_word(sp + 44U));               // lwc1 $f0, 44($sp)
        cpu.write_fpr_bits(1U, m.read_word(slot + 112U));            // lwc1 $f1, 112($a2)
        ee_cop1::compare_s(cpu, ee_cop1::Compare::Less, 1U, 0U);     // c.olt.s $f1, $f0
        retired += kPlaneCost;
        if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {  // dB < w: the plane is passed
            retired += kStepCost;
            index += 1U;
            cpu.write_gpr_sign_extended_32(10U, index);               // addiu $t2, $t2, 1
            continue;
        }

        // dB >= w: the segment is hit by this plane.
        retired += kHitCost;
        m.write_word(out, index);                                     // sw $t2, 0($a0)
        cpu.write_vu0_vector(4U, m.read_vu_quad(plane));              // lqc2 $vf4, 0($t0)
        ee_cop2::vmul(cpu, 1U, 5U, 4U, n3::kMaskXyz);                // vmul.xyz $vf1, $vf5, $vf4
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, kLaneY);       // vadday.x $acc, $vf1, $vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 3U, 1U, kMaskX, kLaneZ, false, false, false);  // vmaddz.x $vf1, $vf3, $vf1z
        cpu.write_gpr(6U, cpu.read_vu0_vector(1U));                   // qmfc2 $a2, $vf1
        m.write_vu_quad(sp + 16U, cpu.read_vu0_vector(4U));           // sqc2 $vf4, 0($v1)
        cpu.write_fpr_bits(3U, cpu.read_gpr_u32(6U));                 // mtc1 $a2, $f3
        cpu.write_fpr_bits(0U, m.read_word(sp + 28U));                // lwc1 $f0, 28($sp)
        ee_cop1::compare_s(cpu, ee_cop1::Compare::Less, 3U, 0U);     // c.olt.s $f3, $f0
        if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) == 0U) {  // dA not below w: A is outside too
            retired += kOutsideCost;
            cpu.write_fpr_bits(0U, m.read_word(limit_address));       // lwc1 $f0, -32740($gp)
            return retired;
        }
        retired += kParallelTestCost;
        cpu.write_fpr_bits(1U, fpu::sub(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(3U)));  // sub.s $f1, $f1, $f3
        cpu.write_fpr_bits(0U, fpu::abs(cpu.read_fpr_bits(1U)));     // abs.s $f0, $f1
        ee_cop1::compare_s(cpu, ee_cop1::Compare::LessEqual, 0U, 2U);  // c.ole.s $f0, $f2
        if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {  // |dB - dA| <= E: parallel, passed
            retired += kStepCost;
            index += 1U;
            cpu.write_gpr_sign_extended_32(10U, index);
            continue;
        }

        // Clip B at the plane: t = (w - dA) / (dB - dA), B = A + (B - A) t.
        retired += kClipCost;
        cpu.write_gpr_sign_extended_32(6U, cpu.read_fpr_bits(3U));   // mfc1 $a2, $f3
        ee_cop2::vadd_vector(cpu, 1U, 7U, 5U, n3::kMaskXyz, true);   // vsub.xyz $vf1, $vf7, $vf5
        cpu.write_vu0_vector(2U, cpu.read_gpr(6U));                   // qmtc2 $a2, $vf2
        ee_cop2::vadd_broadcast(cpu, 2U, 4U, 2U, n3::kMaskW, kLaneX, true);  // vsubx.w $vf2, $vf4, $vf2x
        m.write_vu_quad(cpu.read_gpr_u32(2U), cpu.read_vu0_vector(2U));     // sqc2 $vf2, 0($v0)
        cpu.write_fpr_bits(0U, m.read_word(sp + 12U));               // lwc1 $f0, 12($sp)
        cpu.write_fpr_bits(0U, fpu::div(cpu.read_fpr_bits(0U), cpu.read_fpr_bits(1U)));  // div.s $f0, $f0, $f1
        cpu.write_gpr_sign_extended_32(6U, cpu.read_fpr_bits(0U));   // mfc1 $a2, $f0
        cpu.write_vu0_vector(2U, cpu.read_gpr(6U));                   // qmtc2 $a2, $vf2
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, kLaneX);  // vmulx.xyz $vf1, $vf1, $vf2x
        ee_cop2::vadd_vector(cpu, 1U, 5U, 1U, n3::kMaskXyz, false);  // vadd.xyz $vf1, $vf5, $vf1
        m.write_vu_quad(sp + 96U, cpu.read_vu0_vector(1U));          // sqc2 $vf1, 96($sp)
        retired += kStepCost;
        index += 1U;
        cpu.write_gpr_sign_extended_32(10U, index);
    }

    // The loop ended without a return. *a0 is -1 when no plane was hit, else the last plane that was hit.
    retired += kExitCost;
    cpu.write_gpr_sign_extended_32(2U, m.read_word(out));            // lw $v0, 0($a0)
    if (static_cast<std::int32_t>(cpu.read_gpr_u32(2U)) < 0) {       // bltz $v0: nothing clipped
        retired += kNoClipCost;
        cpu.write_fpr_bits(0U, m.read_word(limit_address));           // lwc1 $f0, -32740($gp)
        m.write_word(out, 0U);                                        // sw $zero, 0($a0)
        return retired;
    }

    // len = VU0 length of B - A (vsub, vmul, vadda, vmadd, vsqrt, vmulq by Q).
    retired += kLengthCost;
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, kLaneW, false);  // vaddw.x $vf1, $vf0, $vf0w
    cpu.write_vu0_vector(3U, m.read_vu_quad(sp + 288U));              // lqc2 $vf3, 288($sp)
    cpu.write_vu0_vector(2U, m.read_vu_quad(sp + 96U));               // lqc2 $vf2, 96($sp)
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                       // qmfc2 $v0, $vf1
    ee_cop2::vadd_vector(cpu, 1U, 3U, 2U, n3::kMaskXyz, true);        // vsub.xyz $vf1, $vf3, $vf2
    ee_cop2::vmul(cpu, 2U, 1U, 1U, n3::kMaskXyz);                    // vmul.xyz $vf2, $vf1, $vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, kLaneY);           // vadday.x $acc, $vf2, $vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                       // qmtc2 $v0, $vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, kLaneZ, false, false, false);  // vmaddz.x $vf1, $vf1, $vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                       // qmfc2 $v0, $vf1
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                       // qmtc2 $v0, $vf1
    ee_cop2::vsqrt(cpu, 1U, kLaneX);                                 // vsqrt $q, $vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, kLaneW, false); // vaddw.x $vf1, $vf0, $vf0w
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                       // qmfc2 $v0, $vf1
    // vwaitq: the interpreter completes it with nothing to wait for.
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                       // qmtc2 $v0, $vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);  // vmulq.x $vf1, $vf1, $q
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                       // qmfc2 $v0, $vf1
    cpu.write_fpr_bits(1U, m.read_word(limit_address));              // lwc1 $f1, -32740($gp)
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(2U));                    // mtc1 $v0, $f0
    ee_cop1::compare_s(cpu, ee_cop1::Compare::Less, 0U, 1U);         // c.olt.s $f0, $f1
    if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) == 0U) {      // len >= L: f0 stays len
        retired += kLengthKeptCost;
        return retired;
    }
    retired += kLengthLimitCost;
    cpu.write_fpr_bits(0U, m.read_word(limit_address));              // lwc1 $f0, -32740($gp)
    return retired;
}

// Declines (0) a call whose count is above the ceiling or that would fault; the original then runs.
template <class Context>
std::uint32_t clip_ray_cost(const Context& context) {
    guest::R5900State cpu = context.cpu;
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = clip_ray(dry, cpu);
    if (retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return retired;
}

template <class Context>
n3::Run clip_ray_to_plane_set(Context& context) {
    auto call = n3::guest_call(context);
    LiveMemory memory{call.memory()};
    return call.finish(clip_ray(memory, call.cpu()));
}

} // namespace

extern constinit const EngineFunction n3_002b7db0{0x002b7db0U, 448U, 0x7f4ee067384562cdULL, "ClipRayToPlaneSet",
                                                  kCeiling, n3::live<&clip_ray_to_plane_set<NativeFunctionContext>>, EngineReturn::Float, {}, 1e-4,
                                                  1e-6, {}, &clip_ray_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b7db0, clip_ray_to_plane_set, clip_ray_cost)
