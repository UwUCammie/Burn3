// 0x00389580 (a0 record): an unsigned 32-bit field reached through two loads,
// rec = *(a0 + 8) and field = *(rec + 16), converted to float in f0.
// A field with its sign bit clear converts directly (9 instructions). With the
// sign bit set it is halved first, keeping the low bit folded back in
// (h = (field >> 1) | (field & 1)), converted, and doubled with ADD.S (13
// instructions). This is the usual uint32 to float trick; the R5900 conversion
// and add are reproduced by the exact helpers, not host casts. v0/v1 are scratch
// and are not reproduced. Returns f0 (EngineReturn::Float).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;

template <class Context>
n3::Run read_indirect_unsigned_float(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const n3::GuestAddress record = call.arg(0);
    const std::uint32_t field = m.read_u32_le(m.read_u32_le(record + 8U) + 16U);
    if (static_cast<std::int32_t>(field) >= 0) {
        call.cpu().write_fpr_bits(0U, fpu::from_int(static_cast<std::int32_t>(field)));
        return call.finish(9U);
    }
    const std::uint32_t halved = (field >> 1U) | (field & 1U);
    const std::uint32_t half_float = fpu::from_int(static_cast<std::int32_t>(halved));
    call.cpu().write_fpr_bits(0U, fpu::add(half_float, half_float));
    return call.finish(13U);
}

} // namespace

extern constinit const EngineFunction n3_00389580{0x00389580U, 64U, 0x9bd2f24384d4c373ULL,
                                           "ReadIndirectFieldAsUnsignedFloat", 13U,
                                           n3::live<&read_indirect_unsigned_float<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(00389580, read_indirect_unsigned_float)
