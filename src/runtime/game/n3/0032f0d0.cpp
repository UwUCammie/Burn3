// 0x0032f0d0 PairCurveWindowCheck (628 bytes): a 0/1 test on the records at +68 and +72 of the object in a0.
//
// Read from the listing; the branch structure is exact, the names are descriptive:
//   - record72 (object +72) must be non-zero, and record68 (object +68) must have its byte +9249 clear.
//   - CheckRemainingRangeFits (0x00156170) on record68 + 4288 with delta 3.0 must return 0.
//   - slope is 40.0 when record68's mode word (+9280) is 1, else the float at 0x01e7559c.
//     CurveTableValue (0x0015ac00) at record68 + 4288 minus the same at record72 + 4288 is the gap; its magnitude
//     must be <= 3.0 + f9168(record68) * (slope - 3.0), the sum formed as ADDA.S then MADD.S.
//   - Only when the object's byte +34 is clear and the mode is not 1: a tolerance test on two 48-byte table rows at
//     0x0055efec (selected by the signed bytes +6572 of record68 and record72; each row's first word is a record,
//     and the test reads the quadword at +176 of that record).
//   - Then the threshold test (100.0, or 60.0 when the mode is 1) on GP1 times word 3 of the quadword at +176 of
//     the record that record72's signed byte selects in the table, and the final compare of the object's float at
//     +56 with GP2 * f9268(record68).
// The result is v0 (0 or 1). The original spills quadwords to its own frame and reads word 3 back; the native keeps
// those words in locals, so the frame stores are not reproduced. ACC and the other temporaries are not reproduced,
// and f20/f21 are restored by the original's epilogue, so the native never writes them.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00156170)
N3_CALLEE(0015ac00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 144U;             // addiu $sp, $sp, -144
constexpr std::uint32_t kEpilogue = 6U;                 // 0x0032f32c-0x0032f340: ld, lwc1, lq, lwc1, jr, addiu
constexpr std::uint32_t kCeiling = 20000U;
constexpr GuestAddress kRecordBlock = 4288U;            // the curve block both callees read
constexpr GuestAddress kCurveRows = 0x0055EFECU;        // 48-byte rows: kCurveRows + 48 * signed byte
constexpr GuestAddress kRowQuad = 176U;                 // lq 176(row record)
constexpr GuestAddress kSlopeFloat = 0x01E7559CU;       // lwc1 21916 from 0x01e70000
constexpr GuestAddress kToleranceFloat = 0x01E755A0U;   // lwc1 21920 from 0x01e70000
constexpr unsigned kScaleFloat = 30980U;                // lwc1 -30980($gp), GP1
constexpr unsigned kWeightFloat = 30700U;               // lwc1 -30700($gp), GP2
constexpr std::uint32_t kThree = 0x40400000U;           // 3.0f
constexpr std::uint32_t kForty = 0x42200000U;           // 40.0f
constexpr std::uint32_t kSixty = 0x42700000U;           // 60.0f
constexpr std::uint32_t kHundred = 0x42C80000U;         // 100.0f

// lb: the byte sign-extended to 32 bits, as the row index arithmetic sees it.
std::uint32_t signed_byte(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

// kCurveRows + 48 * selector (sll/addu chain, 32-bit wrap).
GuestAddress curve_row(const std::uint32_t selector) {
    return kCurveRows + (((selector << 1U) + selector) << 4U);
}

// Every exit runs the 6-instruction epilogue after its own count.
template <class CallType>
n3::Run exit_with(CallType& call, const std::uint32_t value, const std::uint32_t retired) {
    call.return_value(value);
    return call.finish(retired + kEpilogue);
}

template <class Context>
n3::Run pair_curve_window_check(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);                  // $s0
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress record72 = m.read_u32_le(object + 72U);
    std::uint32_t retired = 8U;                               // 0x0032f0d0-0x0032f0ec

    if (record72 == 0U) {                                     // beq $v0, $zero, 0x0032f108
        return exit_with(call, 0U, retired + 2U);
    }

    retired += 4U;                                            // 0x0032f0f0-0x0032f0fc
    const GuestAddress record68 = m.read_u32_le(object + 68U);
    if (m.read_u8(record68 + 9249U) != 0U) {                  // beq $v0, $zero, 0x0032f110
        return exit_with(call, 0U, retired + 2U);             // 0x0032f100
    }

    // CheckRemainingRangeFits(record68 + 4288, 3.0) must return 0.
    retired += 6U;                                            // 0x0032f110-0x0032f124: jal, bne and their delay slots
    call.set_arg(0, record68 + kRecordBlock);
    call.set_float_arg(0, kThree);
    const n3::Run remaining = call.invoke([](auto& mm) { return n3::fn_00156170(mm); }, kFrameBytes);
    if (remaining.declined) {
        return call.decline();
    }
    retired += remaining.retired;
    if (call.result() != 0U) {                                // bne $v0, $zero, 0x0032f258
        return exit_with(call, 0U, retired + 2U);
    }

    // slope (f21) from the mode word of record68.
    const std::uint32_t mode = m.read_u32_le(record68 + 9280U);
    retired += 5U;                                            // 0x0032f128-0x0032f138
    std::uint32_t slope = kForty;
    if (mode != 1U) {                                         // bne $v1, $v0, 0x0032f14c
        retired += 3U;                                        // 0x0032f14c-0x0032f154
        slope = m.read_u32_le(kSlopeFloat);
    } else {
        retired += 4U;                                        // 0x0032f13c-0x0032f148
    }

    // CurveTableValue at record68 + 4288 (its result stays in f20) and at record72 + 4288.
    retired += 19U;                                           // 0x0032f158-0x0032f1a0
    call.set_arg(0, record68 + kRecordBlock);
    const n3::Run curveA = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
    if (curveA.declined) {
        return call.decline();
    }
    const std::uint32_t valueA = call.float_result();
    call.set_arg(0, record72 + kRecordBlock);
    const n3::Run curveB = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
    if (curveB.declined) {
        return call.decline();
    }
    const std::uint32_t valueB = call.float_result();
    retired += curveA.retired + curveB.retired;

    // |valueA - valueB| <= ACC + spread * (slope - 3.0), with ACC = 0 + 3.0 (ADDA.S, then MADD.S).
    const std::uint32_t spread = m.read_u32_le(record68 + 9168U);
    const std::uint32_t gap = fpu::abs(fpu::sub(valueA, valueB));
    const std::uint32_t bound = fpu::madd(fpu::add(0U, kThree), spread, fpu::sub(slope, kThree));
    if (!fpu::le(gap, bound)) {                               // c.ole.s $f4, $f0; bc1f 0x0032f260
        return exit_with(call, 0U, retired + 2U);
    }

    retired += 3U;                                            // 0x0032f1a4-0x0032f1ac
    if (m.read_u8(object + 34U) == 0U) {                      // beq $v0, $zero, 0x0032f268
        retired += 4U;                                        // 0x0032f268-0x0032f274
        if (mode != 1U) {                                     // beq $v1, $v0, 0x0032f1b0 (taken when mode is 1)
            // Tolerance test: the two table rows picked by the signed bytes +6572 of record68 and record72.
            retired += 37U;                                   // 0x0032f278-0x0032f308
            const std::uint32_t selectorB = signed_byte(m.read_u8(record68 + 6572U));
            const std::uint32_t selectorC = signed_byte(m.read_u8(record72 + 6572U));
            const std::uint32_t scale = m.read_u32_le(gp - kScaleFloat);
            const std::uint32_t tolerance = m.read_u32_le(kToleranceFloat);
            const GuestAddress rowB = m.read_u32_le(curve_row(selectorB));
            const n3::Gpr128 quadB = n3::load_quad_any(m, rowB + kRowQuad);
            const GuestAddress rowC = m.read_u32_le(curve_row(selectorC));
            const n3::Gpr128 quadC = n3::load_quad_any(m, rowC + kRowQuad);
            const std::uint32_t acc = fpu::mul(scale, quadB.words[3]);                       // mula.s
            const std::uint32_t deviation = fpu::abs(fpu::msub(acc, scale, quadC.words[3])); // msub.s, abs.s
            if (!fpu::le(deviation, tolerance)) {             // c.ole.s $f1, $f0; bc1t 0x0032f1b0
                return exit_with(call, 0U, retired + 2U);     // 0x0032f30c
            }
        }
    }

    // Threshold: 100.0, or 60.0 when the mode is 1.
    retired += 7U;                                            // 0x0032f1b0-0x0032f1c8
    std::uint32_t threshold = kHundred;
    if (mode == 1U) {
        retired += 2U;                                        // 0x0032f1cc-0x0032f1d0
        threshold = kSixty;
    }

    // GP1 times word 3 of a row quadword (row picked by the signed byte +6572 of record72) against the threshold.
    retired += 18U;                                           // 0x0032f1d4-0x0032f218
    const GuestAddress rowA = m.read_u32_le(curve_row(signed_byte(m.read_u8(record72 + 6572U))));
    const n3::Gpr128 quadA = n3::load_quad_any(m, rowA + kRowQuad);
    const std::uint32_t scaledA = fpu::mul(m.read_u32_le(gp - kScaleFloat), quadA.words[3]);
    if (fpu::lt(scaledA, threshold)) {                        // c.olt.s $f0, $f2; bc1t 0x0032f314
        return exit_with(call, 0U, retired + 2U);
    }

    // Mode 0 also needs the byte +4574 of record72 to be clear.
    retired += 4U;                                            // 0x0032f21c-0x0032f228
    if (mode == 0U) {                                         // beq $v0, $zero, 0x0032f31c
        retired += 3U;                                        // 0x0032f31c-0x0032f324
        if (m.read_u8(record72 + 4574U) != 0U) {              // beq $v0, $zero, 0x0032f22c
            return exit_with(call, 0U, retired + 1U);         // 0x0032f328
        }
    }

    // GP2 * f9268(record68) against the object's float at +56.
    retired += 7U;                                            // 0x0032f22c-0x0032f244
    const std::uint32_t weight = m.read_u32_le(record68 + 9268U);
    const std::uint32_t product = fpu::mul(m.read_u32_le(gp - kWeightFloat), weight);
    const bool below = fpu::lt(m.read_u32_le(object + 56U), product);   // c.olt.s $f2, $f0; bc1f 0x0032f250
    return exit_with(call, below ? 0U : 1U, retired + 2U);              // 0x0032f248 returns 0, 0x0032f250 returns 1
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0032f0d0, 628U, 0x1bfb12523545b6a7ULL, "PairCurveWindowCheck", kCeiling, pair_curve_window_check,
         EngineReturn::Integer)
