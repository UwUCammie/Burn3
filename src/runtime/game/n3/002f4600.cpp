// 0x002f4600 (no arguments, no results): ClearGlobalPair. The original clears two globals in the 0x01e8xxxx page of
// main RAM, each addressed as lui 0x01e8 plus an offset: first the byte at 0x01e86d1c (sb $zero, 27932), then the word
// at 0x01e86d10 (sw $zero, 27920), which sits in the jr's delay slot and so runs before the return takes effect. The
// second lui leaves v1 = 0x01e80000, and the native sets v1 to that too. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagByte = 0x01e86d1cU;   // lui 0x01e8 (0x01e80000) + 27932, sb
constexpr GuestAddress kFlagWord = 0x01e86d10U;   // lui 0x01e8 (0x01e80000) + 27920, sw in the delay slot
constexpr std::uint32_t kPageBase = 0x01e80000U;  // v1 at return: the value of the second lui

// Retired instructions of the listing, delay slot included: lui, sb, lui, jr, sw.
constexpr std::uint32_t kRetired = 5U;

template <class Context>
n3::Run clear_global_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    m.write_u8(kFlagByte, 0U);
    m.write_u32_le(kFlagWord, 0U);
    call.cpu().write_gpr_sign_extended_32(3U, kPageBase);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002f4600{0x002f4600U, 20U, 0xf9456b46738cec04ULL, "ClearGlobalPair", 5U,
                                                  n3::live<&clear_global_pair<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f4600, clear_global_pair)
