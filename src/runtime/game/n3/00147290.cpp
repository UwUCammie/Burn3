// 0x00147290 (a0 = handle out, a1 = quad index out (u16), a2 = found byte out, a3 = point quadword):
// FindNearestStripQuad. Stores 0 to the handle and 0xffff to the index. When the object count at 0x0064d358 is not
// zero and the point's x, y and z are not all zero (sign bits aside), every object is walked: 8 bytes at 0x00522660,
// the handle at +44 and the strip pointer at +48. A strip has its corner index list at +0 and its quad count at +12;
// quad k has its four corners at halfwords 2k..2k+3 of the list, each a 16-byte vertex at 16 * index from the vertex
// array pointer at 0x0064d354. The centre is the corner sum times 0.25 (VF5.x), taken on VU0; its squared distance to
// the point (VF1.x) is compared with the best so far (f1, starting at the float at gp-31324). A nearer quad stores the
// handle at a0, the quad index at a1 and a zero byte at a2. Returns nothing. The point's spill below sp (the sq in the
// delay slot of the count test) is not reproduced: the check ignores words of the callee's own frame.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <type_traits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kObjectCount = 0x0064d358U;  // lui 0x65, lw -11432: s32 number of strip objects
constexpr GuestAddress kVertexBase = 0x0064d354U;   // lui 0x65, addiu -11436: pointer to the 16-byte vertices
constexpr GuestAddress kObjects = 0x00522660U;      // lui 0x52, addiu 9824: 8-byte objects
constexpr GuestAddress kHandleOffset = 44U;         // object + 44: the handle stored to a0
constexpr GuestAddress kStripOffset = 48U;          // object + 48: the strip pointer
constexpr GuestAddress kStripIndexList = 0U;        // strip + 0: pointer to the corner indices
constexpr GuestAddress kStripQuadCount = 12U;       // strip + 12: u16 quad count
constexpr std::uint32_t kLimitOffset = static_cast<std::uint32_t>(-31324);  // gp-relative float: best distance
constexpr std::uint32_t kScaleBits = 0x3e800000U;   // 0.25, the lui that sets VF5.x

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 9U;        // 0x147290..0x1472b0: the stores, the point, the count test
constexpr std::uint32_t kZeroTestCost = 9U;     // 0x1472b4..0x1472d4: x, y and z of the point
constexpr std::uint32_t kSetupCost = 12U;       // 0x1472d8..0x147304: VF4, VF5, VF6, f1, the loop entry
constexpr std::uint32_t kEarlyExitCost = 3U;    // 0x147400..0x147408: frame restore, jr $ra and its delay slot
constexpr std::uint32_t kObjectTestCost = 4U;   // 0x1473d4..0x1473e0: object against the count
constexpr std::uint32_t kLoopExitCost = 4U;     // 0x1473e4..0x1473e8 and 0x147404..0x147408: the exit and return
constexpr std::uint32_t kObjectHeadCost = 8U;   // 0x147308..0x147324: the strip and its quad count
constexpr std::uint32_t kObjectEnterCost = 2U;  // 0x147328..0x14732c: into the quad loop
constexpr std::uint32_t kObjectNextCost = 1U;   // 0x1473d0: next object
constexpr std::uint32_t kQuadTestCost = 4U;     // 0x1473c0..0x1473cc: quad against the quad count
constexpr std::uint32_t kQuadBodyCost = 34U;    // 0x147330..0x1473b4: corners, centre, distance, compare
constexpr std::uint32_t kQuadBetterCost = 5U;   // 0x1473ec..0x1473fc: a nearer quad is stored
constexpr std::uint32_t kQuadStepCost = 2U;     // 0x1473b8..0x1473bc: next quad, strip read again

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count. The captured calls retire about 241 thousand instructions each.
constexpr std::uint32_t kCeiling = 400000U;

// The three outputs the call stores: the handle (4 bytes at a0), the quad index (2 bytes at a1), the found byte (a2).
struct Outputs {
    GuestAddress handle;
    GuestAddress index;
    GuestAddress found;
};

// Whether [address, address + size) shares a byte with [start, start + length).
bool overlaps(const GuestAddress address, const std::uint32_t size, const GuestAddress start,
              const std::uint32_t length) {
    return (address - start) < length || (start - address) < size;
}

// An access the interpreter performs without faulting: aligned as the instruction needs, inside guest memory.
template <class Memory>
bool loadable(const Memory& m, const GuestAddress address, const std::uint32_t size, const std::uint32_t alignment) {
    return (address & (alignment - 1U)) == 0U && m.contains(address, size);
}

// A read the call may make: it faults nowhere and reads no byte the call stores. The dry run keeps no stores, so a read
// that could see one is declined, and the original then runs the call and sees the stores itself.
template <class Memory>
bool readable(const Memory& m, const Outputs& out, const GuestAddress address, const std::uint32_t size,
              const std::uint32_t alignment) {
    return loadable(m, address, size, alignment) && !overlaps(address, size, out.handle, 4U) &&
           !overlaps(address, size, out.index, 2U) && !overlaps(address, size, out.found, 1U);
}

// The whole function on guest memory (the live run) or on a DryMemory (call_cost, which keeps no stores).
template <class Context>
n3::Run find_nearest_strip_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const Outputs out{call.arg(0), call.arg(1), call.arg(2)};
    const GuestAddress point_address = call.arg(3);

    // 0x147294: sw $zero, 0($a0)
    if (!loadable(m, out.handle, 4U, 4U)) {
        return call.decline();
    }
    m.write_u32_le(out.handle, 0U);
    // 0x14729c: lq $a3, 0($a3): the quadword at the aligned address
    if (!readable(m, out, point_address & ~15U, 16U, 16U)) {
        return call.decline();
    }
    const guest::Gpr128 point = n3::load_quad_any(m, point_address);
    // 0x1472a0: sh $v1, 0($a1) with v1 = 0xffff
    if (!loadable(m, out.index, 2U, 2U)) {
        return call.decline();
    }
    m.write_u16_le(out.index, static_cast<std::uint16_t>(0xffffU));
    // 0x1472a8: lw $v1, -11432($v1): the object count, and the beq against zero
    if (!readable(m, out, kObjectCount, 4U, 4U)) {
        return call.decline();
    }
    if (m.read_u32_le(kObjectCount) == 0U) {
        return call.finish(kEntryCost + kEarlyExitCost);
    }
    // 0x1472b4..0x1472d4: the funnel-shift rotations and the ors leave x | y | z in the low word of the tested
    // doubleword, so the branch is taken exactly when their low 31 bits are zero
    if (((point.words[0] | point.words[1] | point.words[2]) & 0x7fffffffU) == 0U) {
        return call.finish(kEntryCost + kZeroTestCost + kEarlyExitCost);
    }
    std::uint32_t retired = kEntryCost + kZeroTestCost + kSetupCost;

    // 0x1472d8..0x147304: VF4 = point; f1 = the limit; VF6.x = 1 (vaddw.x of VF0); VF5.x = 0.25. The lui writes only
    // the low doubleword of $a3, so VF5.z and VF5.w keep the point's words.
    const GuestAddress limit_address = cpu.read_gpr_u32(n3::kGlobalPointer) + kLimitOffset;
    if (!readable(m, out, limit_address, 4U, 4U)) {
        return call.decline();
    }
    cpu.write_vu0_vector(4U, point);
    cpu.write_fpr_bits(1U, m.read_u32_le(limit_address));
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 0U, 8U, 3U, false);
    guest::Gpr128 scale = point;
    scale.words[0] = kScaleBits;
    scale.words[1] = 0U;
    cpu.write_vu0_vector(5U, scale);

    std::uint32_t object = 0U;
    for (;;) {
        // 0x1473d4..0x1473e0: object < count (signed); the count is read again every time
        retired += kObjectTestCost;
        if (!readable(m, out, kObjectCount, 4U, 4U)) {
            return call.decline();
        }
        const auto count = static_cast<std::int32_t>(m.read_u32_le(kObjectCount));
        if (static_cast<std::int32_t>(object) >= count) {
            return call.finish(retired + kLoopExitCost);
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }

        // 0x147308..0x147324: the object's strip and the strip's quad count
        retired += kObjectHeadCost;
        const GuestAddress entry = kObjects + 8U * object;
        const GuestAddress handle = entry + kHandleOffset;
        if (!readable(m, out, entry + kStripOffset, 4U, 4U)) {
            return call.decline();
        }
        const GuestAddress first_strip = m.read_u32_le(entry + kStripOffset);
        if (!readable(m, out, first_strip + kStripQuadCount, 2U, 2U)) {
            return call.decline();
        }
        if (m.read_u16_le(first_strip + kStripQuadCount) != 0U) {
            // 0x147328..0x14732c: the strip is read again, from the handle's +4 (the object's strip pointer)
            retired += kObjectEnterCost;
            if (!readable(m, out, handle + 4U, 4U, 4U)) {
                return call.decline();
            }
            GuestAddress strip = m.read_u32_le(handle + 4U);
            std::uint32_t quad = 0U;
            for (;;) {
                // 0x1473c0..0x1473cc: quad < quad count (signed); the count is read again every time
                retired += kQuadTestCost;
                if (!readable(m, out, strip + kStripQuadCount, 2U, 2U)) {
                    return call.decline();
                }
                const auto quads = static_cast<std::int32_t>(m.read_u16_le(strip + kStripQuadCount));
                if (static_cast<std::int32_t>(quad) >= quads) {
                    break;
                }
                if (retired > kCeiling) {
                    return call.finish(retired);
                }

                // 0x147330..0x147348: the index list, the vertex array and the quad's four corner indices
                retired += kQuadBodyCost;
                if (!readable(m, out, strip + kStripIndexList, 4U, 4U)) {
                    return call.decline();
                }
                const GuestAddress list = m.read_u32_le(strip + kStripIndexList);
                if (!readable(m, out, kVertexBase, 4U, 4U)) {
                    return call.decline();
                }
                const GuestAddress vertices = m.read_u32_le(kVertexBase);
                const GuestAddress slot = list + (quad << 2U);
                if (!readable(m, out, slot, 8U, 2U)) {
                    return call.decline();
                }
                const GuestAddress corner0 = vertices + (static_cast<GuestAddress>(m.read_u16_le(slot)) << 4U);
                const GuestAddress corner1 =
                    vertices + (static_cast<GuestAddress>(m.read_u16_le(slot + 2U)) << 4U);
                const GuestAddress corner2 =
                    vertices + (static_cast<GuestAddress>(m.read_u16_le(slot + 4U)) << 4U);
                const GuestAddress corner3 =
                    vertices + (static_cast<GuestAddress>(m.read_u16_le(slot + 6U)) << 4U);

                // 0x14735c..0x147380: VF2 = corner 1, VF3 = corner 0, VF1 = corner 2; VF2 = c0 + c1, then + c2
                if (!readable(m, out, corner1, 16U, 16U)) {
                    return call.decline();
                }
                cpu.write_vu0_vector(2U, n3::load_quad_any(m, corner1));
                if (!readable(m, out, corner0, 16U, 16U)) {
                    return call.decline();
                }
                cpu.write_vu0_vector(3U, n3::load_quad_any(m, corner0));
                if (!readable(m, out, corner2, 16U, 16U)) {
                    return call.decline();
                }
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, corner2));
                ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, false);
                ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 14U, false);

                // 0x147384..0x147388: VF1 = corner 3; VF1 = (c0 + c1 + c2) + c3
                if (!readable(m, out, corner3, 16U, 16U)) {
                    return call.decline();
                }
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, corner3));
                ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);

                // 0x14738c..0x14739c: centre = sum * 0.25 - point, squared; ACC.x = x + y; VF1.x = ACC.x + z (squared)
                ee_cop2::vmul_broadcast(cpu, 1U, 1U, 5U, 14U, 0U);
                ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 14U, true);
                ee_cop2::vmul(cpu, 1U, 1U, 1U, 14U);
                ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);
                ee_cop2::vmadd_broadcast(cpu, 1U, 6U, 1U, 8U, 2U, false, false, false);

                // 0x1473a0..0x1473ac: f0 = the distance (VF1.x); c.olt.s f0, f1 sets the condition bit
                cpu.write_fpr_bits(0U, cpu.read_vu0_vector(1U).words[0]);
                ee_cop1::compare_s(cpu, ee_cop1::Compare::Less, 0U, 1U);
                if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {
                    // 0x1473ec..0x1473fc: this quad is the nearest so far
                    retired += kQuadBetterCost;
                    if (!loadable(m, out.found, 1U, 1U)) {
                        return call.decline();
                    }
                    m.write_u32_le(out.handle, handle);
                    cpu.write_fpr_bits(1U, cpu.read_fpr_bits(0U));
                    m.write_u16_le(out.index, static_cast<std::uint16_t>(quad));
                    m.write_u8(out.found, static_cast<std::uint8_t>(0U));
                }

                // 0x1473b8..0x1473bc: next quad; the strip is read again from the handle's +4
                retired += kQuadStepCost;
                ++quad;
                if (!readable(m, out, handle + 4U, 4U, 4U)) {
                    return call.decline();
                }
                strip = m.read_u32_le(handle + 4U);
            }
        }
        // 0x1473d0: next object
        retired += kObjectNextCost;
        ++object;
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00147290, 380U, 0x5265d69dabeb0478ULL, "FindNearestStripQuad", kCeiling, find_nearest_strip_quad,
         EngineReturn::Nothing)
