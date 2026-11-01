// 0x0015ac00 (a0 record): a value read off the curve table at 0x0064d364
// (pairs of floats, the count at 0x0064d368), interpolated by the record's
// fraction +840 between entry (+844 - 1) and entry +844. When the byte at
// 0x0064d378 is clear the result also adds (+848 + 1) times the last entry.
// Returns f0 (0 when the table is empty).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTable = 0x0064d364U;      // lui 0x65, -11420
constexpr GuestAddress kCount = 0x0064d368U;      // lui 0x65, -11416
constexpr GuestAddress kFlag = 0x0064d378U;       // lui 0x65, -11400 (byte)

template <class Context>
n3::Run curve_value(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t count = m.read_u32_le(kCount);
    if (count == 0U) {
        call.cpu().write_fpr_bits(0U, 0U);
        return call.finish(7U);
    }
    const std::uint32_t index = m.read_u32_le(record + 844U);
    const GuestAddress table = m.read_u32_le(kTable);
    std::uint32_t retired = 4U + 3U;
    std::uint32_t f4 = 0U;
    if (index == 0U) {
        retired += 3U;
    } else {
        f4 = m.read_u32_le(table + (index << 3U) - 8U);
        retired += 6U;
    }
    const std::uint32_t f0 = m.read_u32_le(table + (index << 3U));
    const std::uint32_t f3 = fpu::sub(f0, f4);
    const std::uint32_t f1 = m.read_u32_le(record + 840U);
    retired += 8U;
    std::uint32_t result = 0U;
    if (m.read_u8(kFlag) != 0U) {
        // ADDA.S f0(=0), f4 ; MADD.S f0 = ACC + f1*f3
        const std::uint32_t acc = fpu::add(0U, f4);
        result = fpu::madd(acc, f1, f3);
        retired += 6U + 2U + 2U;
    } else {
        const std::uint32_t last = m.read_u32_le(table + (count << 3U) - 8U);
        const std::uint32_t steps = m.read_u32_le(record + 848U) + 1U;
        std::uint32_t acc = fpu::add(0U, f4);
        acc = fpu::madd(acc, f1, f3);
        result = fpu::madd(acc, fpu::from_int(static_cast<std::int32_t>(steps)), last);
        retired += 13U + 2U + 2U;
    }
    call.cpu().write_fpr_bits(0U, result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0015ac00{0x0015ac00U, 192U, 0x05661fdc08452c9dULL, "CurveTableValue", 38U,
                                           n3::live<&curve_value<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015ac00, curve_value)
