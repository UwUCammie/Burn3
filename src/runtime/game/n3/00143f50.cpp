// 0x00143f50 (GetModeTableValue): a per-mode word lookup. The object base
// 0x0064d3b0 is never zero, so the first beq is never taken (its delay slot
// still retires). The mode is the word at the object + 0x1970. Mode 8 returns
// 2 (11 retired). Any other mode returns the word at 0x004b5e50 + mode * 4,
// with 32-bit wrap and no bounds check (16 retired). v1 is left as the original
// leaves it (mode, or mode * 4 on the table path). No stores, calls or loops.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kModeObject = 0x0064d3b0U;  // lui 0x65, addiu -11344
constexpr std::uint32_t kModeOffset = 6512U;       // lw offset, 0x1970
constexpr GuestAddress kModeTable = 0x004b5e50U;   // lui 0x4b, addiu 24144
constexpr std::uint32_t kFixedMode = 8U;

template <class Context>
n3::Run mode_table_value(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t mode = m.read_u32_le(kModeObject + kModeOffset);
    if (mode == kFixedMode) {
        call.cpu().write_gpr_sign_extended_32(3U, mode);
        call.return_value(2U);
        return call.finish(11U);
    }
    const std::uint32_t scaled = mode << 2U;
    call.cpu().write_gpr_sign_extended_32(3U, scaled);
    call.return_value(m.read_u32_le(kModeTable + scaled));
    return call.finish(16U);
}

} // namespace

extern constinit const EngineFunction n3_00143f50{0x00143f50U, 68U, 0x06c2e308934a5650ULL, "GetModeTableValue", 16U,
                                                  n3::live<&mode_table_value<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00143f50, mode_table_value)
