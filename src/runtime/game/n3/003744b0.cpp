// 0x003744b0 (a0 record): RangeLength, a leaf. Returns v0 = *(a0 + 8) - *(a0 + 4) as a 32-bit wrapping unsigned
// difference of two words of the record (a range or buffer length). The original is `lw v1,8(a0); lw v0,4(a0);
// jr ra; subu v0,v1,v0`. No stores, calls or branches: every call retires the same 4 instructions, the jr's delay slot
// included. v1 ends holding the word at +8, as the lw left it (sign-extended into the whole register).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kV1 = 3U;
constexpr GuestAddress kEndOffset = 8U;
constexpr GuestAddress kBeginOffset = 4U;

// lw, lw, jr and its delay slot (subu) always retire.
constexpr std::uint32_t kCost = 4U;

template <class Context>
n3::Run range_length(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress record = call.arg(0);
    // Same order as the listing: the word at +8 first, then the word at +4.
    const std::uint32_t end_word = call.memory().read_u32_le(record + kEndOffset);
    const std::uint32_t begin_word = call.memory().read_u32_le(record + kBeginOffset);
    call.cpu().write_gpr_sign_extended_32(kV1, end_word);
    call.return_value(end_word - begin_word);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_003744b0{0x003744b0U, 16U, 0x55064cbce90fd258ULL, "RangeLength", kCost,
                                                  n3::live<&range_length<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(003744b0, range_length)
