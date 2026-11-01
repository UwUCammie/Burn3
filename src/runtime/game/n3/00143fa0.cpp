// 0x00143fa0 (a0 = record R, f12 = float F): CurveRemainderGate. Clears the float at R+2524, then calls
// CurveTableRemainder (0x0015a9c0) on O+4288, where O = [R+1952]. Unless that result is below the float at R+2536,
// or the byte at R+2609 is already set, it clears the word at R+2564, sets the byte to 1 and returns F. Otherwise
// the kind word [O+9280] and the mode word of the record at [O+9264] pick the tail: a search of the object table at
// 0x0064d388 (count at 0x0064d37c), bounded by GetModeTableValue (0x00143f50), and GetLinkedRecordByte (0x001419b0)
// on O. The result is always in f0 (EngineReturn::Float).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015a9c0)
N3_CALLEE(00143f50)
N3_CALLEE(001419b0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 64U;          // the original's addiu $sp, $sp, -64
constexpr GuestAddress kObjectOffset = 1952U;       // R+1952: the object O
constexpr GuestAddress kEntryBlock = 4288U;         // O+4288: CurveTableRemainder's argument; a table entry's halfword
constexpr GuestAddress kOutputOffset = 2524U;       // R+2524: float cleared on entry and set on one tail path
constexpr GuestAddress kLimitOffset = 2536U;        // R+2536: float limit for the curve remainder
constexpr GuestAddress kFlagWordOffset = 2564U;     // R+2564: word cleared on the return-F path
constexpr GuestAddress kFlagByteOffset = 2609U;     // R+2609: byte, set to 1 on the return-F path
constexpr GuestAddress kKindOffset = 9280U;         // O+9280: kind word
constexpr GuestAddress kRecordOffset = 9264U;       // O+9264: record pointer
constexpr GuestAddress kRecordModeOffset = 5464U;   // record+5464: mode word, compared with 3
constexpr GuestAddress kRecordScaleOffset = 5468U;  // record+5468: float scale
constexpr GuestAddress kEntryFlagOffset = 6420U;    // table entry+6420: word that rejects the entry when set
constexpr GuestAddress kTableCount = 0x0064D37CU;   // word: number of table entries
constexpr GuestAddress kTable = 0x0064D388U;        // entry pointers: 0x00522660 + 4 * index + 0x0012AD28
constexpr std::uint32_t kGlobalFloatOffset = static_cast<std::uint32_t>(-30636);  // lwc1 $f2, -30636($gp)
constexpr GuestAddress kAddedFloat = 0x01E754ECU;   // float added to 45.0 on one tail path
constexpr std::uint32_t kFortyFive = 0x42340000U;   // 45.0f
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run curve_remainder_gate(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t input = call.cpu().read_fpr_bits(12U);
    m.write_u32_le(record + kOutputOffset, 0U);
    const GuestAddress object = m.read_u32_le(record + kObjectOffset);
    call.set_arg(0, object + kEntryBlock);
    const n3::Run curve = call.invoke([](auto& mm) { return n3::fn_0015a9c0(mm); }, kFrameBytes);
    if (curve.declined) {
        return call.decline();
    }
    // 143fa0..143fc8 (11) before the call, 143fcc..143fd8 (4) after it.
    std::uint32_t retired = 15U + curve.retired;
    const bool below = fpu::lt(call.float_result(), m.read_u32_le(record + kLimitOffset));
    if (below) {
        retired += 4U;  // 143fdc, then 143fe0..143fe8 (the beq is taken)
    } else {
        retired += 6U;  // 143fe0..143fe8, then 143fec..143ff4 (lbu, bne, nop)
        if (m.read_u8(record + kFlagByteOffset) == 0U) {
            m.write_u32_le(record + kFlagWordOffset, 0U);
            m.write_u8(record + kFlagByteOffset, 1U);
            call.cpu().write_fpr_bits(0U, input);
            return call.finish(retired + 11U);  // 143ff8..144008 (5), 144164 (1), 144168..144178 (5)
        }
    }
    // 14400c..14401c (5): the object is loaded again and its kind word tested; s0 = 0 in the delay slot.
    retired += 5U;
    const GuestAddress object_again = m.read_u32_le(record + kObjectOffset);
    const std::uint32_t kind = m.read_u32_le(object_again + kKindOffset);
    GuestAddress search_object = object_again;
    bool s0 = false;
    if (kind == 1U) {
        // 1440c8..1440d8 (5); 1440dc (1) sets s0 = a1 & 0xff = 1 when the mode test passes.
        retired += 5U;
        const GuestAddress kind_record = m.read_u32_le(object_again + kRecordOffset);
        if (m.read_u32_le(kind_record + kRecordModeOffset) == 3U) {
            retired += 1U;
            s0 = true;
        }
    } else if (kind == 0U) {
        // 144020..144024 (2), 144030..144040 (5).
        retired += 7U;
        const GuestAddress kind_record = m.read_u32_le(object_again + kRecordOffset);
        if (m.read_u32_le(kind_record + kRecordModeOffset) == 3U) {
            // 144044..14406c (11) around GetModeTableValue, then the bounds of the search.
            call.set_arg(0, object_again);
            const n3::Run mode_run = call.invoke([](auto& mm) { return n3::fn_00143f50(mm); }, kFrameBytes);
            if (mode_run.declined) {
                return call.decline();
            }
            retired += 11U + mode_run.retired;
            const std::uint32_t mode = call.result();
            const GuestAddress object_loaded = m.read_u32_le(record + kObjectOffset);
            search_object = object_loaded;
            const std::int32_t high_bound =
                static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(object_loaded + kEntryBlock)));
            const std::int32_t low_bound = static_cast<std::int32_t>(static_cast<std::uint32_t>(high_bound) - mode);
            const std::uint32_t count = m.read_u32_le(kTableCount);
            std::uint32_t index = 0U;
            for (;;) {
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(count)) {
                    retired += 5U;  // 1440b4..1440bc (not taken), 1440c0..1440c4 (beq to 1440e0)
                    break;
                }
                retired += 3U;  // 1440b4..1440bc: slt, bne to the body, nop
                const GuestAddress entry = m.read_u32_le(kTable + (index << 2U));
                const std::int32_t value =
                    static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(entry + kEntryBlock)));
                retired += 9U;  // 144070..144090
                if (value < low_bound) {
                    retired += 2U;  // 1440f0..1440f4: index + 1, back to the test
                    ++index;
                    continue;
                }
                retired += 3U;  // 144094..14409c
                if (!(value < high_bound)) {
                    retired += 2U;
                    ++index;
                    continue;
                }
                retired += 3U;  // 1440a0..1440a8
                if (m.read_u32_le(entry + kEntryFlagOffset) != 0U) {
                    retired += 2U;
                    ++index;
                    continue;
                }
                retired += 2U;  // 1440ac..1440b0: beq to 1440e0, daddiu s0 = 1 in its delay slot
                s0 = true;
                break;
            }
        }
    } else {
        // 144020..144024 (2), 144028..14402c (2): beq to 1440e0.
        retired += 4U;
    }
    // 1440e0..1440e4 (2): bne s0 to 1440f8.
    retired += 2U;
    if (!s0) {
        // 1440e8..1440ec (2), 144164 (1), 144168..144178 (5): return F.
        retired += 8U;
        call.cpu().write_fpr_bits(0U, input);
        return call.finish(retired);
    }
    // 1440f8..144104: jal GetLinkedRecordByte(search_object), its delay slot, beq v0 == 0 to 14414c, nop.
    call.set_arg(0, search_object);
    const n3::Run byte_run = call.invoke([](auto& mm) { return n3::fn_001419b0(mm); }, kFrameBytes);
    if (byte_run.declined) {
        return call.decline();
    }
    retired += 4U + byte_run.retired;
    if (call.result() == 0U) {
        // 14414c..144160 (6), 144144..144148 (2), 144168..144178 (5).
        retired += 13U;
        call.cpu().write_fpr_bits(0U, fpu::add(kFortyFive, m.read_u32_le(kAddedFloat)));
        return call.finish(retired);
    }
    // 144108..144124 (8): the object is read again; the add.s in the bne's delay slot runs on both outcomes.
    retired += 8U;
    const GuestAddress object_last = m.read_u32_le(record + kObjectOffset);
    const GuestAddress record_last = m.read_u32_le(object_last + kRecordOffset);
    const std::uint32_t result = fpu::add(input, kFortyFive);
    if (m.read_u32_le(record_last + kRecordModeOffset) != 3U) {
        // 144164 (1), 144168..144178 (5).
        retired += 6U;
        call.cpu().write_fpr_bits(0U, result);
        return call.finish(retired);
    }
    // 144128..144140 (7): ACC = 0 + f2, then f1 = ACC + f2 * f3, stored at R+2524.
    retired += 7U;
    const std::uint32_t f2 = m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + kGlobalFloatOffset);
    const std::uint32_t f3 = m.read_u32_le(record_last + kRecordScaleOffset);
    m.write_u32_le(record + kOutputOffset, fpu::madd(fpu::add(0U, f2), f2, f3));
    // 144144..144148 (2), 144168..144178 (5).
    retired += 7U;
    call.cpu().write_fpr_bits(0U, result);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00143fa0, 476U, 0x85df2dd5ecf152b8ULL, "CurveRemainderGate", kCeiling, curve_remainder_gate, EngineReturn::Float)
