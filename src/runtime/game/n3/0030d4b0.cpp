// 0x0030d4b0 (a0 = state record P, a1 = mode): ResetSelectionMode. Stores the mode in the word at P+488,
// clears the words at P+476 and P+480 and the byte at P+6655, and sets the word at P+484 to 3. The mode is
// then read back from P+488 and, when it is below 3 (signed), the signed byte at 0x004f6060 is stored
// sign-extended to the word at P+492. Otherwise P+492 is left alone. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagByte = 0x004f6060U;  // lui 0x4f, lb offset 24672 (a signed byte)
constexpr std::uint32_t kLowMode = 3U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kLowModeCost = 18U;   // mode < 3: the byte load and store, then the return
constexpr std::uint32_t kHighModeCost = 14U;  // mode >= 3: straight to the return

template <class Context>
n3::Run reset_selection_mode(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress state = call.arg(0);
    const std::uint32_t mode = call.arg(1);
    m.write_u32_le(state + 488U, mode);
    m.write_u32_le(state + 476U, 0U);
    m.write_u32_le(state + 480U, 0U);
    m.write_u8(state + 6655U, 0U);
    m.write_u32_le(state + 484U, kLowMode);
    const std::uint32_t reloaded = m.read_u32_le(state + 488U);
    if (static_cast<std::int32_t>(reloaded) < static_cast<std::int32_t>(kLowMode)) {
        const auto flag = static_cast<std::int8_t>(m.read_u8(kFlagByte));
        m.write_u32_le(state + 492U, static_cast<std::uint32_t>(static_cast<std::int32_t>(flag)));
        return call.finish(kLowModeCost);
    }
    return call.finish(kHighModeCost);
}

} // namespace

extern constinit const EngineFunction n3_0030d4b0{0x0030d4b0U, 72U, 0x75fa64d02a1e04f5ULL, "ResetSelectionMode",
                                                  18U, n3::live<&reset_selection_mode<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030d4b0, reset_selection_mode)
