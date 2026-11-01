// 0x00263350 (no arguments): AppendDirectGifHeader. Appends two fixed quadwords to the current packet buffer and
// advances the buffer pointer by 32 bytes. The pointer is the 32-bit global at $gp-24240. It is read first, the
// quadwords are stored at the pointer and at the pointer + 16, and the advanced pointer is stored last (the original
// does that store in the delay slot of the return). The first quadword is a DMA CNT tag (QWC 1) followed by a VIF
// NOP and a VIF DIRECT tag of one quadword; the second is a GIF tag with EOP set, NLOOP 0 and one register
// descriptor, A+D. Returns nothing: v1 and a0..a3 are scratch, and the single caller does not read them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kBufferPointerOffset = -24240;  // $gp-relative global holding the packet write pointer
constexpr std::uint32_t kAppendBytes = 32U;
// Retired instructions of the listing, delay slot included. There is no loop and no call, so every path retires
// the same 17 instructions.
constexpr std::uint32_t kRetired = 17U;

// The two quadwords as (low doubleword, high doubleword), built by the original's lui/ori/dsll32 sequence.
constexpr std::uint64_t kDmaTagLow = 0x0000000010000001ULL;   // DMA CNT tag: QWC 1
constexpr std::uint64_t kVifTagHigh = 0x5000000100000000ULL;  // VIF NOP (bits 64..95), VIF DIRECT 1 (bits 96..127)
constexpr std::uint64_t kGifTagLow = 0x1000000000008000ULL;   // GIF tag: EOP (bit 15), NLOOP 0, NREG 1 (bits 60..63)
constexpr std::uint64_t kGifTagHigh = 14U;                    // one register descriptor: A+D

guest::Gpr128 make_quad(const std::uint64_t low, const std::uint64_t high) {
    return guest::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                          static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run append_direct_gif_header(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress pointer_address =
        cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kBufferPointerOffset);
    const GuestAddress buffer = m.read_u32_le(pointer_address);
    n3::store_quad_any(m, buffer, make_quad(kDmaTagLow, kVifTagHigh));
    n3::store_quad_any(m, buffer + 16U, make_quad(kGifTagLow, kGifTagHigh));
    m.write_u32_le(pointer_address, buffer + kAppendBytes);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00263350{0x00263350U, 68U, 0x84d209ca7cf10be7ULL, "AppendDirectGifHeader",
                                                  kRetired, n3::live<&append_direct_gif_header<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00263350, append_direct_gif_header)
