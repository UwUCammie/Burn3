// 0x001864f0 (a0 = object O, a1 = index byte, a2 = flag word; 796 bytes): InitWheelState.
//
// O gets two up-vector blocks cleared at O+0 and O+48 (0x0015fa60, InitUpVectorBlock). When the record R = O[176] is
// set, O+288 takes R's float at +24, raised to the float at gp-30264 when it is below that. Four slots of R then feed
// the wheel words: the flag at R+5818+s selects the quadword at R+5788+4s, and slots 0, 1 and 3 copy its lane 2 to
// O+336, O+332 and O+308. Slot 2 stores lane z of a VU0 vmulx.z (the quadword times -1.0) at O+312. Then the reset
// words and bytes are written, O+340 and O+344 take the global gain (gp-30832) divided by lanes of the quadword at
// R+3712 (the gain itself without R), O+348 comes from the float table at 0x004b7150 indexed by the signed a1 byte,
// and O+360 from the table at 0x01d9bb10 indexed by O[374] (stride 176, offset 136). O+372 becomes 1, or 5 for an
// index of 6..10. Finally, when a2 has bit 0 set, the byte at 0x0055d7f9 decides: nonzero returns 0 and writes nothing
// more; zero sets bit 3 of O+372, stores O at 0x0064ce44 and returns 1. Without bit 0 the result is 1.
//
// Retired counts follow the blocks of the listing (brief.py). The two callee calls add their own counts. The loop over
// O+240..O+260 always runs six passes, so it is written as straight code with a per-pass count.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015fa60)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 336U;        // addiu $sp,$sp,-336
constexpr std::uint32_t kCeiling = 20000U;

constexpr unsigned kV0 = 2U;
constexpr std::uint8_t kMaskZ = 2U;                // VU0 destination lane z (x is 8, as the encoding has it)

constexpr GuestAddress kSecondBlock = 48U;         // the second up-vector block is O + 48
constexpr GuestAddress kRecord = 176U;             // O[176] = R
constexpr GuestAddress kRecordFloat = 24U;         // R + 24
constexpr GuestAddress kRecordGainQuad = 3712U;    // R + 3712
constexpr GuestAddress kSlotQuadPointers = 5788U;  // R + 5788 + 4s
constexpr GuestAddress kSlotFlags = 5818U;         // R + 5818 + s
constexpr GuestAddress kGpFloor = static_cast<GuestAddress>(-30264);
constexpr GuestAddress kGpGain = static_cast<GuestAddress>(-30832);
constexpr GuestAddress kIndexTable = 0x004b7150U;  // float table, entry 4 * (signed index byte)
constexpr GuestAddress kStrideTable = 0x01d9bb10U; // float table, entry 176 * O[374] + 136
constexpr GuestAddress kFlagByte = 0x0055d7f9U;    // global byte: nonzero makes the call return 0
constexpr GuestAddress kRegisteredObject = 0x0064ce44U;

// Retired instruction counts of the listing blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 22U;      // 0x001864f0..0x00186544: frame, both calls (jal and delay slot), record test
constexpr std::uint32_t kFloorTest = 5U;       // 0x00186548..0x00186558
constexpr std::uint32_t kFloorTaken = 3U;      // 0x001867a4..0x001867ac
constexpr std::uint32_t kSlotTest = 4U;        // each slot's flag test (0x0018655c, 0x00186584, 0x001865ac, 0x001865e0)
constexpr std::uint32_t kSlotCopy = 6U;        // the quadword copy of a used slot
constexpr std::uint32_t kSlotProduct = 9U;     // slot 2's VU0 product, 0x001865bc..0x001865dc
constexpr std::uint32_t kZeroTest = 6U;        // 0x00186608..0x0018661c
constexpr std::uint32_t kZeroAgain = 4U;       // 0x001867b0..0x001867bc
constexpr std::uint32_t kZeroAgainTaken = 3U;  // 0x001867c0..0x001867c8
constexpr std::uint32_t kResets = 13U;         // 0x00186620..0x00186650
constexpr std::uint32_t kNoRecordGain = 4U;    // 0x001867cc..0x001867d8
constexpr std::uint32_t kRecordGain = 14U;     // 0x00186654..0x00186688
constexpr std::uint32_t kTableStores = 11U;    // 0x0018668c..0x001866b4
constexpr std::uint32_t kClearPass = 7U;       // one pass of 0x001866b8..0x001866d0
constexpr std::uint32_t kClearPasses = 6U;
constexpr std::uint32_t kFlagBytes = 8U;       // 0x001866d4..0x001866f0
constexpr std::uint32_t kIndexAbove = 3U;      // 0x001866f4..0x001866fc (index 6 or more)
constexpr std::uint32_t kIndexBelow = 3U;      // 0x00186700..0x00186708 (index 6..10)
constexpr std::uint32_t kGlobals = 32U;        // 0x0018670c..0x00186788
constexpr std::uint32_t kFlagTest = 4U;        // 0x0018678c..0x00186798
constexpr std::uint32_t kEarlyReturn = 2U;     // 0x0018679c..0x001867a0
constexpr std::uint32_t kRegister = 5U;        // 0x001867dc..0x001867ec
constexpr std::uint32_t kReturnInstruction = 1U;  // 0x001867f0 (addiu $v0,$zero,1)
constexpr std::uint32_t kEpilogue = 6U;        // 0x001867f4..0x00186808

template <class Context>
n3::Run init_wheel_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    // The arguments are read before the callee runs: it overwrites a1 and a2 (temporaries in the original too).
    const GuestAddress object = call.arg(0);
    const std::uint32_t index_byte = call.arg(1) & 0xffU;
    const auto index = static_cast<std::int32_t>(static_cast<std::int8_t>(index_byte));
    const bool flag_bit = (call.arg(2) & 1U) != 0U;

    // 0x001864f4..0x00186524: O+276..O+284 are written before the first call; the store to O+284 sits in that jal's
    // delay slot, so it precedes the call.
    m.write_u32_le(object + 276U, 0U);
    m.write_u32_le(object + 280U, 0U);
    m.write_u32_le(object + 284U, 0xbf800000U);
    const n3::Run first = call.invoke([](auto& machine) { return n3::fn_0015fa60(machine); }, kFrameBytes);
    if (first.declined) {
        return call.decline();
    }
    call.set_arg(0, object + kSecondBlock);
    const n3::Run second = call.invoke([](auto& machine) { return n3::fn_0015fa60(machine); }, kFrameBytes);
    if (second.declined) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost + first.retired + second.retired;
    m.write_u32_le(object + 268U, 0U);
    m.write_u32_le(object + 272U, 0U);
    m.write_u32_le(object + 332U, 0U);
    m.write_u32_le(object + 336U, 0U);

    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress record = m.read_u32_le(object + kRecord);
    if (record != 0U) {
        // 0x00186548..0x001867ac: O+288 is the record's float, raised to the global float when it is below it.
        const std::uint32_t at_float = m.read_u32_le(record + kRecordFloat);
        const std::uint32_t floor_float = m.read_u32_le(gp + kGpFloor);
        m.write_u32_le(object + 288U, at_float);
        retired += kFloorTest;
        if (fpu::lt(at_float, floor_float)) {
            m.write_u32_le(object + 288U, floor_float);
            retired += kFloorTaken;
        }

        // 0x0018655c..0x001865dc and 0x001865e0..0x00186604: the four slots, in order.
        retired += kSlotTest;
        if (m.read_u8(record + kSlotFlags) != 0U) {
            m.write_u32_le(object + 336U, n3::load_quad_any(m, m.read_u32_le(record + kSlotQuadPointers)).words[2]);
            retired += kSlotCopy;
        }
        retired += kSlotTest;
        if (m.read_u8(record + kSlotFlags + 1U) != 0U) {
            m.write_u32_le(object + 332U, n3::load_quad_any(m, m.read_u32_le(record + kSlotQuadPointers + 4U)).words[2]);
            retired += kSlotCopy;
        }
        retired += kSlotTest;
        if (m.read_u8(record + kSlotFlags + 2U) != 0U) {
            // lqc2 faults unless the quadword is 16-byte aligned and mapped: the original runs the call then.
            const GuestAddress quad = m.read_u32_le(record + kSlotQuadPointers + 8U);
            if ((quad & 15U) != 0U || !m.contains(quad, 16U)) {
                return call.decline();
            }
            cpu.write_gpr_sign_extended_32(kV0, 0xbf800000U);      // lui $v0,0xbf80
            cpu.write_vu0_vector(2U, cpu.read_gpr(kV0));           // qmtc2 $v0,$vf2
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, quad));  // lqc2 $vf1,0($v1)
            ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kMaskZ, 0U);  // vmulx.z $vf1,$vf1,$vf2x
            m.write_u32_le(object + 312U, cpu.read_vu0_vector(1U).words[2]);
            retired += kSlotProduct;
        }
        retired += kSlotTest;
        if (m.read_u8(record + kSlotFlags + 3U) != 0U) {
            m.write_u32_le(object + 308U, n3::load_quad_any(m, m.read_u32_le(record + kSlotQuadPointers + 12U)).words[2]);
            retired += kSlotCopy;
        }
    }
    // 0x00186608..0x0018661c: when O+332 is 0.0, the test looks at O+336 too, and when both are 0.0, O+332 becomes -0.5.
    retired += kZeroTest;
    if (fpu::eq(0U, m.read_u32_le(object + 332U))) {
        retired += kZeroAgain;
        if (fpu::eq(0U, m.read_u32_le(object + 336U))) {
            retired += kZeroAgainTaken;
            m.write_u32_le(object + 332U, 0xbf000000U);
        }
    }
    m.write_u8(object + 370U, 0U);
    m.write_u8(object + 368U, 0xffU);
    m.write_u32_le(object + 304U, 0U);
    m.write_u32_le(object + 180U, 0U);
    m.write_u32_le(object + 184U, 0x3f800000U);
    m.write_u32_le(object + 188U, 0U);
    m.write_u32_le(object + 192U, 0x3f800000U);
    retired += kResets;

    // 0x001867cc..0x001867d8 (no record: the gain itself) or 0x00186654..0x00186688 (with a record: the gain divided by
    // lanes 0 and 2 of the quadword at R+3712).
    if (record == 0U) {
        const std::uint32_t gain = m.read_u32_le(gp + kGpGain);
        m.write_u32_le(object + 344U, gain);
        m.write_u32_le(object + 340U, gain);
        retired += kNoRecordGain;
    } else {
        const std::uint32_t gain = m.read_u32_le(gp + kGpGain);
        m.write_u32_le(object + 344U, fpu::div(gain, n3::load_quad_any(m, record + kRecordGainQuad).words[0]));
        m.write_u32_le(object + 340U, fpu::div(gain, n3::load_quad_any(m, record + kRecordGainQuad).words[2]));
        retired += kRecordGain;
    }

    // 0x0018668c..0x001866b4: the index byte and the table entries that depend on it.
    m.write_u8(object + 371U, static_cast<std::uint8_t>(index_byte));
    m.write_u32_le(object + 348U, m.read_u32_le(kIndexTable + static_cast<GuestAddress>(index * 4)));
    m.write_u32_le(object + 356U, 0U);
    m.write_u32_le(object + 292U, 0x3f800000U);
    retired += kTableStores;

    // 0x001866b8..0x001866d0: the loop, six fixed passes clearing O+240..O+260.
    for (GuestAddress field = 240U; field < 240U + 4U * kClearPasses; field += 4U) {
        m.write_u32_le(object + field, 0U);
    }
    retired += kClearPasses * kClearPass;

    // 0x001866d4..0x00186708: the flag byte is 1, or 5 for an index from 6 up to 10.
    m.write_u8(object + 372U, 0U);
    m.write_u8(object + 372U, static_cast<std::uint8_t>(m.read_u8(object + 372U) | 1U));
    retired += kFlagBytes;
    if (index >= 6) {
        retired += kIndexAbove;
        if (index < 11) {
            retired += kIndexBelow;
            m.write_u8(object + 372U, static_cast<std::uint8_t>(m.read_u8(object + 372U) | 4U));
        }
    }
    // 0x0018670c..0x00186788: float constants from gp into O+208..O+236 (reads and writes in the original's order),
    // then the entry of the second table into O+360, which the original stores in its branch delay slot.
    const auto gp_word = [&m, gp](const std::int32_t offset) {
        return m.read_u32_le(gp + static_cast<GuestAddress>(offset));
    };
    const std::uint32_t word_208 = gp_word(-29816);
    const std::uint32_t word_212 = gp_word(-31688);
    m.write_u32_le(object + 204U, 0x3f200000U);
    const std::uint32_t word_216 = gp_word(-29812);
    const std::uint32_t word_220 = gp_word(-29808);
    const std::uint32_t word_224 = gp_word(-29804);
    const std::uint32_t word_228 = gp_word(-30532);
    m.write_u32_le(object + 208U, word_208);
    m.write_u32_le(object + 212U, word_212);
    m.write_u32_le(object + 216U, word_216);
    m.write_u32_le(object + 220U, word_220);
    m.write_u32_le(object + 224U, word_224);
    m.write_u32_le(object + 228U, word_228);
    const std::uint32_t word_232 = gp_word(-31668);
    const std::uint32_t word_236 = gp_word(-30524);
    m.write_u32_le(object + 232U, word_232);
    m.write_u32_le(object + 236U, word_236);
    const std::uint8_t byte_374 = m.read_u8(object + 374U);
    m.write_u32_le(object + 360U, m.read_u32_le(kStrideTable + 176U * byte_374 + 136U));
    retired += kGlobals;

    // 0x00186784..0x00186798: bit 0 of a2 clear returns 1 at once; otherwise the global byte decides.
    if (!flag_bit) {
        retired += kReturnInstruction + kEpilogue;
        call.return_value(1U);
        return call.finish(retired);
    }
    retired += kFlagTest;
    if (m.read_u8(kFlagByte) != 0U) {
        retired += kEarlyReturn + kEpilogue;
        call.return_value(0U);
        return call.finish(retired);
    }
    // 0x001867dc..0x001867ec: register the object.
    m.write_u8(object + 372U, static_cast<std::uint8_t>(m.read_u8(object + 372U) | 8U));
    m.write_u32_le(kRegisteredObject, object);
    retired += kRegister + kReturnInstruction + kEpilogue;
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001864f0, 796U, 0xae7401f0c8f84111ULL, "InitWheelState", kCeiling, init_wheel_state, EngineReturn::Integer)
