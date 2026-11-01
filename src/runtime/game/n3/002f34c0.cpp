// 0x002f34c0 (a0 = dest, a1 = src, a2 = bytes): CopyViaScratchpadDma. A memcpy done by the EE DMA engine
// through an 8 KB window of scratchpad at 0x70002000. Each chunk of min(bytes left, 8192) bytes is DMAed
// main RAM at src -> window on channel 9 (to SPR), then window -> main RAM at dest on channel 8 (from SPR),
// and src and dest advance by 8192. A chunk moves its length >> 4 quadwords, so the last partial chunk drops
// the bytes past its last whole quadword (a chunk under 16 bytes moves nothing). A length <= 0 does nothing.
// Returns nothing: callers never read v0 or v1.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// DMAC registers. Channel 9 moves main RAM into the scratchpad, channel 8 moves it back.
constexpr GuestAddress kDmacStatus = 0x1000E010U;     // D_STAT
constexpr GuestAddress kChannel8Chcr = 0x1000D000U;   // D8_CHCR
constexpr GuestAddress kChannel8Madr = 0x1000D010U;   // D8_MADR
constexpr GuestAddress kChannel8Qwc = 0x1000D020U;    // D8_QWC
constexpr GuestAddress kChannel8Sadr = 0x1000D080U;   // D8_SADR
constexpr GuestAddress kChannel9Chcr = 0x1000D400U;   // D9_CHCR
constexpr GuestAddress kChannel9Madr = 0x1000D410U;   // D9_MADR
constexpr GuestAddress kChannel9Qwc = 0x1000D420U;    // D9_QWC
constexpr GuestAddress kChannel9Sadr = 0x1000D480U;   // D9_SADR
constexpr std::uint32_t kWindow = 0x70002000U;        // the scratchpad window, the SADR of both channels
constexpr std::uint32_t kStart = 0x100U;              // CHCR STR: set to start a transfer, clear when it ends
constexpr std::uint32_t kChannel8Acknowledge = 0x100U;  // D_STAT value the original writes before channel 8
constexpr std::uint32_t kChannel9Acknowledge = 0x200U;  // D_STAT value the original writes before channel 9
constexpr std::int32_t kChunkBytes = 8192;
constexpr unsigned kLengthRegister = 6U;              // a2

// Retired counts from the listing, delay slots included. The loop test runs once before each chunk and
// once more to leave the loop, so a call over k chunks retires 10 + 29k + 3(k + 1) + 4 = 17 + 32k.
constexpr std::uint32_t kNoCopyCost = 5U;   // length <= 0: slt, beq, daddu, jr, nop
constexpr std::uint32_t kSetupCost = 10U;   // slt, beq, daddu, lui, addiu, lui, addiu, ori, beq, addiu
constexpr std::uint32_t kTestCost = 3U;     // slt, bne, nop
constexpr std::uint32_t kChunkCost = 29U;   // subu through the three address advances
constexpr std::uint32_t kExitCost = 4U;     // beq, nop; jr, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so a reported count is
// always the original's exact count. The chunk loop stops here too, which keeps it finite for any length.
constexpr std::uint32_t kCeiling = 20000U;

// The original spins while a channel is still busy after its CHCR write. A normal-mode transfer completes
// inside that write (EeMemory clears STR before the write returns), so the bit is never still set here. If it
// ever is, that is a bug in the DMA model: it is reported by this throw, not emulated, because the original's
// spin would never end.
void require_completed(auto& memory, const GuestAddress chcr, const char* message) {
    if ((memory.read_u32_le(chcr) & kStart) != 0U) {
        throw std::logic_error(message);
    }
}

template <class Context>
n3::Run copy_via_scratchpad_dma(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    GuestAddress dest = call.arg(0);
    GuestAddress src = call.arg(1);
    const std::int32_t bytes = call.arg_signed(2);
    if (bytes <= 0) {
        return call.finish(kNoCopyCost);
    }
    std::uint32_t retired = kSetupCost;
    std::uint32_t copied = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(copied) >= bytes) {
            break;
        }
        // call_cost declines every call this long, so the original runs it; this only keeps the loop finite.
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        // subu, pminw, sra: this chunk's length and its quadword count.
        const std::int32_t left = static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes) - copied);
        const std::int32_t chunk = left < kChunkBytes ? left : kChunkBytes;
        const auto qwc = static_cast<std::uint32_t>(chunk >> 4);

        m.write_u32_le(kDmacStatus, kChannel9Acknowledge);
        m.write_u32_le(kChannel9Sadr, kWindow);
        m.write_u32_le(kChannel9Madr, src);
        m.write_u32_le(kChannel9Qwc, qwc);
        m.write_u32_le(kChannel9Chcr, kStart);
        require_completed(m, kChannel9Chcr, "CopyViaScratchpadDma: channel 9 still busy after its transfer");

        m.write_u32_le(kDmacStatus, kChannel8Acknowledge);
        m.write_u32_le(kChannel8Sadr, kWindow);
        m.write_u32_le(kChannel8Madr, dest);
        m.write_u32_le(kChannel8Qwc, qwc);
        m.write_u32_le(kChannel8Chcr, kStart);
        require_completed(m, kChannel8Chcr, "CopyViaScratchpadDma: channel 8 still busy after its transfer");

        retired += kChunkCost;
        dest += static_cast<std::uint32_t>(kChunkBytes);
        src += static_cast<std::uint32_t>(kChunkBytes);
        copied += static_cast<std::uint32_t>(kChunkBytes);
    }
    return call.finish(retired + kExitCost);
}

// The count in closed form (17 + 32k for k chunks, 5 for a length <= 0), so no dry run is needed. Declined (0)
// while a channel is busy on entry, since the original's busy branches are not replicated here; declined too when
// the length is not a sign-extended 32-bit value (the original compares it as 64 bits) and above kCeiling.
template <class Context>
std::uint32_t copy_via_scratchpad_dma_cost(const Context& context) {
    if ((context.memory.read_u32_le(kChannel8Chcr) & kStart) != 0U ||
        (context.memory.read_u32_le(kChannel9Chcr) & kStart) != 0U) {
        return 0U;
    }
    const auto length = static_cast<std::int32_t>(context.cpu.read_gpr_u32(kLengthRegister));
    if (context.cpu.read_gpr_u64(kLengthRegister) != static_cast<std::uint64_t>(static_cast<std::int64_t>(length))) {
        return 0U;
    }
    if (length <= 0) {
        return kNoCopyCost;
    }
    const auto chunk_bytes = static_cast<std::uint64_t>(kChunkBytes);
    const auto chunks = (static_cast<std::uint64_t>(length) + chunk_bytes - 1U) / chunk_bytes;
    const auto retired = kSetupCost + kChunkCost * chunks + kTestCost * (chunks + 1U) + kExitCost;
    return retired <= kCeiling ? static_cast<std::uint32_t>(retired) : 0U;
}

} // namespace

extern constinit const EngineFunction n3_002f34c0{0x002f34c0U, 308U, 0x545ce2efb40a107aULL, "CopyViaScratchpadDma",
                                                  kCeiling, n3::live<&copy_via_scratchpad_dma<NativeFunctionContext>>, EngineReturn::Nothing, {},
                                                  1e-4, 1e-6, {}, &copy_via_scratchpad_dma_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002f34c0, copy_via_scratchpad_dma, copy_via_scratchpad_dma_cost)
