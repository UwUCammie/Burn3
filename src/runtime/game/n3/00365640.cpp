// 0x00365640 BeginBatch (no arguments): starts a lit-vertex batch and appends its packet at the cursor (gp-24240).
// The slot index IDX (word at 0x00667050) selects the render-state record E = 0x00665ec0 + 560*IDX + 1136. Its ten
// state qwords (E+288) follow a VU1 UNPACK of 10 qwords to address 0x3f6, a parameter qword (the byte at gp-22600)
// follows a one-qword UNPACK to 0x3f5, and a DIRECT block of 8 qwords comes after: a GIF tag, the four A+D pairs
// copied from E+448, ALPHA_1 = 0x00000080_00000044, TEST_1 (its alpha reference is 128 times the float at gp-22588,
// truncated) and CLAMP_1 = 0. ReservePacketBuffer (0x001f3080, 25 quadwords) makes the room. The jal's delay slot
// clears the float before the reserve runs, and the float is read back after it. When the VU1 chain 0x0047a060 is
// not the cached one (word gp-28060) it is cached and a DMA call tag to it is appended. The packet ends with an
// MSCAL, the cursor moves past it, and the bytes at gp-22604 and gp-22608 are set to 1. Returns nothing.
//
// The copy loop runs exactly ten passes, so it is straight code with a constant per pass. The own path is 202
// instructions when the chain is cached and 215 when it is not; the reserve's count is added to it.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kSlotIndex = 0x00667050U;     // lui 0x66, lw 28752: the slot index IDX
constexpr GuestAddress kRecordTable = 0x00665ec0U;   // lui 0x66, addiu 24256
constexpr std::uint32_t kRecordOffset = 1136U;       // E = table + offset + 560 * IDX
constexpr std::uint32_t kRecordStride = 560U;        // 35 * 16 bytes per slot
constexpr std::uint32_t kStateOffset = 288U;         // E + 288: the ten state qwords
constexpr std::uint32_t kRegisterOffset = 448U;      // E + 448: the four A+D qwords
constexpr std::uint32_t kStateQwords = 10U;
constexpr std::uint32_t kRegisterQwords = 4U;
constexpr GuestAddress kChainAddress = 0x0047a060U;  // the VU1 chain a batch calls
constexpr std::uint32_t kAlphaScale = 0x43000000U;   // 128.0f (mtc1 $a3, $f1)

// $gp-relative globals (the original reads and writes them as $gp - offset).
constexpr std::uint32_t kBelowRegisters = 22584U;    // word: pointer to the saved GS state, E + 448
constexpr std::uint32_t kBelowFloat = 22588U;        // float: cleared before the reserve, read after it
constexpr std::uint32_t kBelowParamFlag = 22600U;    // byte: copied into the parameter qword
constexpr std::uint32_t kBelowStarted = 22604U;      // byte: set to 1
constexpr std::uint32_t kBelowBatchOpen = 22608U;    // byte: batch-open flag, set to 1
constexpr std::uint32_t kBelowChain = 28060U;        // word: the VU1 chain address last called
constexpr std::uint32_t kBelowCursor = 24240U;       // word: the packet write position

constexpr std::uint32_t kReserveQwords = 25U;
constexpr std::uint32_t kFrameBytes = 32U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions per block (delay slots included), from the listing.
constexpr std::uint32_t kBeforeCall = 9U;             // 0x00365640..0x00365660: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 30U;             // 0x00365664..0x003656d8: up to the first copy pass
constexpr std::uint32_t kCopyPass = 8U;               // 0x003656dc..0x003656f8: one pass of the copy loop
constexpr std::uint32_t kCopyCost = 10U * kCopyPass;  // the loop runs exactly ten passes
constexpr std::uint32_t kTailCost = 69U;              // 0x003656fc..0x0036580c: the DIRECT block and the chain test
constexpr std::uint32_t kUncachedCost = 13U;          // 0x00365810..0x00365840: the chain's DMA call tag
constexpr std::uint32_t kReturnCost = 14U;            // 0x00365844..0x00365878: MSCAL, cursor, flags, return

// A quadword from its two 64-bit halves (`low` = bits 0..63, `high` = bits 64..127).
constexpr n3::Gpr128 quadword_from(const std::uint64_t low, const std::uint64_t high) {
    return n3::Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                       static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Memory>
void put_quadword(Memory& m, const GuestAddress address, const std::uint64_t low, const std::uint64_t high) {
    n3::store_quad_any(m, address, quadword_from(low, high));
}

template <class Context>
n3::Run begin_batch_body(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    const std::uint32_t slot = m.read_u32_le(kSlotIndex);
    const GuestAddress record = kRecordTable + kRecordOffset + kRecordStride * slot;
    const GuestAddress state = record + kStateOffset;
    const GuestAddress registers = record + kRegisterOffset;

    // The jal's delay slot clears the float before ReservePacketBuffer(flags 0, 25 quadwords) runs.
    m.write_u32_le(gp - kBelowFloat, 0U);
    call.set_arg(0, 0U);
    call.set_arg(1, kReserveQwords);
    const n3::Run reserve = call.invoke([](auto& mm) { return n3::fn_001f3080(mm); }, kFrameBytes);
    if (reserve.declined) {
        return call.decline();
    }
    std::uint32_t retired = kBeforeCall + reserve.retired + kAfterCall + kCopyCost + kTailCost + kReturnCost;

    // The reserve moved the cursor, so the packet starts where it left it.
    const GuestAddress packet = m.read_u32_le(gp - kBelowCursor);
    put_quadword(m, packet, 0x10000000ULL, 0x1100000011000000ULL);        // count 0, FLUSH
    m.write_u32_le(gp - kBelowRegisters, registers);
    put_quadword(m, packet + 16U, 0x1000000aULL, 0x6c0a03f601000404ULL);  // STCYCL, UNPACK 10 to 0x3f6
    for (std::uint32_t i = 0U; i < kStateQwords; ++i) {
        n3::store_quad_any(m, packet + 32U + 16U * i, n3::load_quad_any(m, state + 16U * i));
    }
    put_quadword(m, packet + 192U, 0x10000001ULL, 0x6c0103f501000404ULL);  // UNPACK 1 to 0x3f5
    const std::uint64_t param_flag = m.read_u8(gp - kBelowParamFlag);
    put_quadword(m, packet + 208U, (param_flag << 32U) | 0x0aULL, 0x0000000300000000ULL);
    put_quadword(m, packet + 224U, 0x10000008ULL, 0x5000000800000000ULL);  // DIRECT 8
    put_quadword(m, packet + 240U, 0x1000000000008007ULL, 14U);            // GIF tag, 7 A+D registers
    for (std::uint32_t i = 0U; i < kRegisterQwords; ++i) {
        n3::store_quad_any(m, packet + 256U + 16U * i, n3::load_quad_any(m, registers + 16U * i));
    }
    put_quadword(m, packet + 320U, 0x0000008000000044ULL, 66U);           // ALPHA_1
    const std::uint32_t alpha_float = m.read_u32_le(gp - kBelowFloat);
    const std::int32_t alpha_ref = fpu::to_int(fpu::mul(kAlphaScale, alpha_float));
    const std::uint64_t test = (static_cast<std::uint64_t>(static_cast<std::int64_t>(alpha_ref)) << 4U) | 0x0005000bULL;
    put_quadword(m, packet + 336U, test, 71U);                            // TEST_1
    put_quadword(m, packet + 352U, 0U, 8U);                               // CLAMP_1

    GuestAddress cursor = packet + 368U;
    if (m.read_u32_le(gp - kBelowChain) != kChainAddress) {
        m.write_u32_le(gp - kBelowChain, kChainAddress);
        put_quadword(m, cursor, (static_cast<std::uint64_t>(kChainAddress) << 32U) | 0x50000000ULL,
                     (0x020000fcULL << 32U) | 0x03000000ULL);             // DMA call tag, OFFSET 0 BASE 0xfc
        cursor += 16U;
        retired += kUncachedCost;
    }
    put_quadword(m, cursor, 0x10000000ULL, 0x1400000000000000ULL);        // MSCAL 0
    m.write_u32_le(gp - kBelowCursor, cursor + 16U);
    m.write_u8(gp - kBelowStarted, 1U);
    m.write_u8(gp - kBelowBatchOpen, 1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00365640, 572U, 0x218c34b6dfb3af9bULL, "BeginBatch", kCeiling, begin_batch_body, EngineReturn::Nothing)
