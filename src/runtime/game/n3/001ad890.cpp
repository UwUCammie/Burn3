// 0x001ad890 (a0 = a two-word record R, a1 = object O): LatchSpawnMarker, a void leaf. P = the u32 at O+3268 is a second
// object with a flags byte at P+4117, a quadword at P+4096 and bytes at P+4116 and P+4118. The function returns at once
// when that flags byte has bit 4 set, and when VU0 computes x = (word at O+4460) - (quad at O+352).y with x <= 30.0.
// Otherwise it rewrites R in the original's order, reading each word back from memory: word0 = (word0 << 16) +
// (signed word0 >> 16), then word0 += word1, then word1 += word0. The final word0 is converted to a float as an
// unsigned value (negative words are halved keeping their low bit, converted, and doubled). It returns when
// (float at $gp-32672) * (float at O+404 - 8000.0) <= 2^-32 * that float. Otherwise P's quadword at +4096 takes the
// quad at O+352 in its xyz lanes and the word at 0x00522680 in its w lane (VU0 adds), its flags byte gets 0x18, the
// byte at P+4118 of a re-read P gets 0xf0, the flags byte is masked with 0xf9, and when the byte at P+4116 is not 2 it
// becomes 2 and the flags byte gets bit 0. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kSecondOffset = 3268U;          // lw 3268(a1): the second object P
constexpr GuestAddress kQuadOffset = 352U;             // lq 352(a1)
constexpr GuestAddress kFloatOffset = 404U;            // lwc1 404(a1)
constexpr GuestAddress kWordOffset = 4460U;            // lw 4460(a1)
constexpr GuestAddress kVectorOffset = 4096U;          // P+4096: quadword
constexpr GuestAddress kFlagsOffset = 4117U;           // P+4117: flags byte
constexpr GuestAddress kStateOffset = 4116U;           // P+4116: state byte
constexpr GuestAddress kMarkerOffset = 4118U;          // P+4118: byte
constexpr GuestAddress kGlobalWord = 0x00522680U;      // lui 0x52, lw 9856
constexpr std::uint32_t kGlobalPointerOffset = 0xffff8060U;  // lwc1 -32672($gp), as a 32-bit offset
constexpr unsigned kTemporaryV1 = 3U;
constexpr std::uint32_t kThirty = 0x41f00000U;         // 30.0
constexpr std::uint32_t kEightThousand = 0x45fa0000U;  // 8000.0
constexpr std::uint32_t kTwoToMinus32 = 0x2f800000U;   // 2^-32

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 6U;       // 0x001ad890..0x001ad8a4: frame, flags test, bne
constexpr std::uint32_t kTestCost = 16U;      // 0x001ad8a8..0x001ad8e4: VU0 test, bc1t
constexpr std::uint32_t kRecordCost = 20U;    // 0x001ad8e8..0x001ad934: record, float product, bltz
constexpr std::uint32_t kNegativeCost = 7U;   // 0x001ad944..0x001ad95c: negative conversion
constexpr std::uint32_t kPositiveCost = 3U;   // 0x001ad938..0x001ad940: positive conversion, jump
constexpr std::uint32_t kScaleCost = 7U;      // 0x001ad960..0x001ad978: scale, bc1t
constexpr std::uint32_t kWriteCost = 26U;     // 0x001ad97c..0x001ad9e0: VU0 write, flags, beq
constexpr std::uint32_t kStateCost = 4U;      // 0x001ad9e4..0x001ad9f0: state byte, flag bit
constexpr std::uint32_t kReturnCost = 2U;     // 0x001ad9f4..0x001ad9f8: jr and its delay slot
// The longest path: 6 + 16 + 20 + 7 + 7 + 26 + 4 + 2.
constexpr std::uint32_t kDeclaredCost = 88U;

// The high word a 32-bit load leaves in a register's low doubleword (lw sign-extends).
std::uint32_t sign_extension(const std::uint32_t word) noexcept {
    return (word & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

// C.LE.S sets the FCR31 condition bit; a path without a compare leaves it alone.
void set_condition(guest::R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                             : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
}

template <class Context>
n3::Run latch_spawn_marker(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const GuestAddress object = call.arg(1U);
    // qmtc2 copies a whole 128-bit register into VU0. Before the qmtc2 of $v1 and of $a0, every write to those
    // registers is a 32-bit or 64-bit operation that keeps the upper doubleword, so the entry upper doublewords
    // are what VU0 receives.
    const guest::Gpr128 entry_v1 = cpu.read_gpr(kTemporaryV1);
    const guest::Gpr128 entry_a0 = call.arg128(0U);

    // 0x001ad890..0x001ad8a4: a set flags bit 4 returns at once.
    std::uint32_t retired = kHeadCost;
    const GuestAddress second = m.read_u32_le(object + kSecondOffset);
    if ((m.read_u8(second + kFlagsOffset) & 0x10U) != 0U) {
        return call.finish(retired + kReturnCost);
    }

    // 0x001ad8a8..0x001ad8e4: VU0 x = word - quad.y, returns when x <= 30.0.
    retired += kTestCost;
    const guest::Gpr128 quad = n3::load_quad_any(m, object + kQuadOffset);
    const std::uint32_t float_word = m.read_u32_le(object + kFloatOffset);
    const std::uint32_t word = m.read_u32_le(object + kWordOffset);
    guest::Gpr128 word_register = entry_v1;
    word_register.words[0] = word;
    word_register.words[1] = sign_extension(word);
    cpu.write_vu0_vector(2U, word_register);                                  // qmtc2 $v1,$vf2
    cpu.write_vu0_vector(1U, quad);                                           // qmtc2 $a2,$vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, 8U, 1U, true);                   // vsuby.x vf1,vf2,vf1y
    const std::uint32_t test_x = cpu.read_vu0_vector(1U).words[0];            // qmfc2 $v1,$vf1; mtc1 $v1,$f1
    const bool at_most_thirty = fpu::le(test_x, kThirty);                     // c.ole.s $f1,$f0
    set_condition(cpu, at_most_thirty);
    if (at_most_thirty) {                                                     // bc1t
        return call.finish(retired + kReturnCost);
    }

    // 0x001ad8e8..0x001ad934: the float product comes first (its gp word is read before the record is written, and
    // the mul.s is the bltz delay slot, so it always runs), then the record is rewritten in memory order.
    retired += kRecordCost;
    const std::uint32_t gp_word = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kGlobalPointerOffset);
    const std::uint32_t difference = fpu::sub(float_word, kEightThousand);     // sub.s $f1,$f2,$f1
    const std::uint32_t product = fpu::mul(gp_word, difference);               // mul.s $f2,$f0,$f1
    const std::uint32_t head_source = m.read_u32_le(record);                   // lw $a3,0($a0)
    const std::uint32_t head = (head_source << 16U) +
                               static_cast<std::uint32_t>(static_cast<std::int32_t>(head_source) >> 16U);
    m.write_u32_le(record, head);                                              // sw $v1,0($a0)
    const std::uint32_t head_back = m.read_u32_le(record);                     // lw $a2,0($a0)
    const std::uint32_t word_one = m.read_u32_le(record + 4U);                 // lw $v1,4($a0)
    const std::uint32_t sum = head_back + word_one;                            // addu $v1,$a2,$v1
    m.write_u32_le(record, sum);                                               // sw $v1,0($a0)
    const std::uint32_t word_one_back = m.read_u32_le(record + 4U);            // lw $a2,4($a0)
    const std::uint32_t sum_back = m.read_u32_le(record);                      // lw $v1,0($a0)
    m.write_u32_le(record + 4U, word_one_back + sum_back);                     // addu $v1,$a2,$v1; sw $v1,4($a0)
    const std::uint32_t sign_word = m.read_u32_le(record);                     // lw $v1,0($a0)

    // bltz (0x001ad930): a negative word takes the unsigned conversion, a positive one the signed.
    std::uint32_t converted = 0U;
    if (static_cast<std::int32_t>(sign_word) < 0) {
        // 0x001ad944..0x001ad95c: halve keeping the low bit, convert, then add the float to itself.
        retired += kNegativeCost;
        const std::uint32_t halved = (sign_word >> 1U) | (sign_word & 1U);
        const std::uint32_t half_float = fpu::from_int(static_cast<std::int32_t>(halved));   // cvt.s.w $f1,$f0
        converted = fpu::add(half_float, half_float);                                         // add.s $f1,$f1,$f1
    } else {
        // 0x001ad938..0x001ad940
        retired += kPositiveCost;
        converted = fpu::from_int(static_cast<std::int32_t>(sign_word));                      // cvt.s.w $f1,$f0
    }

    // 0x001ad960..0x001ad974: returns when the product is at most 2^-32 times the converted word.
    retired += kScaleCost;
    const std::uint32_t scaled = fpu::mul(kTwoToMinus32, converted);           // mul.s $f0,$f0,$f1
    const bool product_done = fpu::le(product, scaled);                        // c.ole.s $f2,$f0
    set_condition(cpu, product_done);
    if (product_done) {                                                        // bc1t
        return call.finish(retired + kReturnCost);
    }

    // 0x001ad97c..0x001ad9e0: the second object's quadword, then its flags and state bytes.
    retired += kWriteCost;
    const GuestAddress second_now = m.read_u32_le(object + kSecondOffset);     // lw $a2,3268($a1)
    const GuestAddress vector_address = second_now + kVectorOffset;
    cpu.write_vu0_vector(1U, quad);                                            // lqc2 $vf1,0($sp): the entry quad
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vector_address));                // lqc2 $vf2,4096($a2)
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, 14U, false);                         // vadd.xyz vf2,vf0,vf1
    n3::store_quad_any(m, vector_address, cpu.read_vu0_vector(2U));                // sqc2 $vf2,4096($a2)
    const std::uint32_t global_word = m.read_u32_le(kGlobalWord);              // lw $a0,9856($v1)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 1U, 3U, true);                    // vsubw.w vf2,vf0,vf0w
    guest::Gpr128 global_register = entry_a0;
    global_register.words[0] = global_word;
    global_register.words[1] = sign_extension(global_word);
    cpu.write_vu0_vector(1U, global_register);                                 // qmtc2 $a0,$vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, 1U, 0U, false);                   // vaddx.w vf2,vf2,vf1x
    n3::store_quad_any(m, vector_address, cpu.read_vu0_vector(2U));                // sqc2 $vf2,4096($a2)
    m.write_u8(second_now + kFlagsOffset,
               static_cast<std::uint8_t>(m.read_u8(second_now + kFlagsOffset) | 0x18U));    // ori 0x18; sb 4117
    const GuestAddress third = m.read_u32_le(object + kSecondOffset);          // lw $a1,3268($a1)
    m.write_u8(third + kMarkerOffset,
               static_cast<std::uint8_t>(m.read_u8(third + kMarkerOffset) | 0xf0U));       // ori 0xf0; sb 4118
    m.write_u8(second_now + kFlagsOffset,
               static_cast<std::uint8_t>(m.read_u8(second_now + kFlagsOffset) & 0xf9U));   // andi 0xf9; sb 4117
    if (m.read_u8(second_now + kStateOffset) == 2U) {                          // lbu 4116; beq with 2
        return call.finish(retired + kReturnCost);
    }

    // 0x001ad9e4..0x001ad9f0: the state byte becomes 2, and the flags byte gets bit 0.
    retired += kStateCost;
    m.write_u8(second_now + kStateOffset, static_cast<std::uint8_t>(2U));       // sb $v1,4116($a2)
    m.write_u8(second_now + kFlagsOffset,
               static_cast<std::uint8_t>(m.read_u8(second_now + kFlagsOffset) | 0x01U));   // ori 1; sb 4117
    return call.finish(retired + kReturnCost);
}

} // namespace

extern constinit const EngineFunction n3_001ad890{0x001ad890U, 364U, 0xc8a0b1651f06e21bULL, "LatchSpawnMarker",
                                                  kDeclaredCost, n3::live<&latch_spawn_marker<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001ad890, latch_spawn_marker)
