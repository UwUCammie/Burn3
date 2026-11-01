// 0x0015a9c0 (a0 record): CurveTableRemainder. Interpolates the curve table at the
// record's +840 fraction exactly as 0x0015ac00 (CurveTableValue) does, then returns
// (cv - F5) / (last entry * (float)A3), where F5 is the float at +652 and A3 the word
// at +708. The early exits (F5 == -1.0f, or A3 <= 0 signed) return f0 = +0.
// Leaf function with no stores; the result is f0 (EngineReturn::Float).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTable = 0x0064d364U;      // lui 0x65, -11420
constexpr GuestAddress kCount = 0x0064d368U;      // lui 0x65, -11416
constexpr GuestAddress kFlag = 0x0064d378U;       // lui 0x65, -11400 (byte)
constexpr std::uint32_t kMinusOne = 0xbf800000U;  // -1.0f

template <class Context>
n3::Run curve_remainder(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t f5 = m.read_u32_le(record + 652U);
    if (fpu::eq(kMinusOne, f5)) {
        // a9c0..a9d8 and aaf0..aaf8, ab08..ab0c.
        call.cpu().write_fpr_bits(0U, 0U);
        return call.finish(12U);
    }
    const std::uint32_t a3 = m.read_u32_le(record + 708U);
    if (static_cast<std::int32_t>(a3) <= 0) {
        // a9c0..a9e4 (blez taken), aaf0..aaf8, ab08..ab0c.
        call.cpu().write_fpr_bits(0U, 0U);
        return call.finish(15U);
    }
    // a9c0..aa00 retired: 7 + 3 + 3 (the repeated -1.0f test, never taken here) + 4.
    std::uint32_t retired = 17U;
    const std::uint32_t count = m.read_u32_le(kCount);
    const GuestAddress table = m.read_u32_le(kTable);
    std::uint32_t f2 = 0U;
    if (count == 0U) {
        // aa04..aa0c: f0 = +0, f2 = 0 - F5.
        retired += 3U;
        f2 = fpu::sub(0U, f5);
    } else {
        // aa10..aa18.
        retired += 3U;
        const std::uint32_t index = m.read_u32_le(record + 844U);
        std::uint32_t f4 = 0U;
        if (index == 0U) {
            // aa1c..aa24: f4 = +0.
            retired += 3U;
        } else {
            // aa28..aa3c: f4 = entry[index - 1].
            f4 = m.read_u32_le(table + (index << 3U) - 8U);
            retired += 6U;
        }
        // aa40..aa5c: f0 = entry[index]; the sub.s in the flag branch's delay slot always runs.
        retired += 8U;
        const std::uint32_t f0 = m.read_u32_le(table + (index << 3U));
        const std::uint32_t f3 = fpu::sub(f0, f4);
        const std::uint32_t f1 = m.read_u32_le(record + 840U);
        std::uint32_t result = 0U;
        if (m.read_u8(kFlag) != 0U) {
            // aa60..aa74: ADDA.S f0(=+0), f4 ; MADD.S f0 = ACC + f1*f3.
            retired += 6U;
            result = fpu::madd(fpu::add(0U, f4), f1, f3);
        } else {
            // aa78..aaa8: ACC = f4 + f1*f3 (ADDA.S, MADDA.S), then f0 = ACC + (float)(word +848 + 1) * last entry.
            retired += 13U;
            const std::uint32_t last_entry = m.read_u32_le(table + (count << 3U) - 8U);
            const std::uint32_t steps = m.read_u32_le(record + 848U) + 1U;
            const std::uint32_t acc = fpu::madd(fpu::add(0U, f4), f1, f3);
            result = fpu::madd(acc, fpu::from_int(static_cast<std::int32_t>(steps)), last_entry);
        }
        // aaac: f2 = f0 - F5.
        retired += 1U;
        f2 = fpu::sub(result, f5);
    }
    // aab0..aaec and the jr/nop: f0 = f2 / (last entry * (float)A3). Nothing in this function stores, so the
    // TABLE and COUNT values read above are the ones the original re-reads at this point.
    retired += 18U;
    const std::uint32_t a3_float = fpu::from_int(static_cast<std::int32_t>(a3));
    const std::uint32_t last = m.read_u32_le(table + (count << 3U) - 8U);
    const std::uint32_t divisor = fpu::mul(last, a3_float);
    call.cpu().write_fpr_bits(0U, fpu::div(f2, divisor));
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0015a9c0{0x0015a9c0U, 336U, 0x6cce7bfcfa701615ULL, "CurveTableRemainder", 66U,
                                           n3::live<&curve_remainder<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015a9c0, curve_remainder)
