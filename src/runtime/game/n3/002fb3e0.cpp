// 0x002fb3e0 (a0 = record P, a1 = block index k): stores the floats at P+16, P+20, P+24 and P+28 as fixed
// point (times 65536, truncated) in words +56, +52, +64 and +60 of the 272-byte block at 0x01e7aa40 + 272 * k.
// Then it calls SetFixedPointParameter (0x002fbcd0) for slots 0 to 12 of that block, slot s with the float at
// P + 32 + 4 * s. Returns nothing.
//
// The loop runs exactly 13 times (s1 counts from 0 while s1 < 13, unsigned), so it is straight code here. The
// callee's paths make the total count data dependent, so the entry uses the dry-run cost (N3_ENTRY).
//
//   002fb3e0..002fb46c  prologue and the four stores: 36 instructions
//   002fb470..002fb494  loop step (jal, its delay slot, the index step, sltiu, bne and its delay slot): 10
//   002fb498..002fb4ac  epilogue (ld, three lq, jr and its delay slot): 6

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002fbcd0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kBlockTable = 0x01e7aa40U;   // lui 0x01e8, addiu -21952
constexpr std::uint32_t kBlockBytes = 272U;         // ((k << 4) + k) << 4
constexpr std::uint32_t kScale = 0x47800000U;       // 65536.0f
constexpr std::uint32_t kSlotCount = 13U;
constexpr std::uint32_t kFrameBytes = 64U;          // addiu $sp, $sp, -64

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 36U;
constexpr std::uint32_t kLoopStepCost = 10U;
constexpr std::uint32_t kEpilogueCost = 6U;

constexpr std::uint32_t kCeiling = 20000U;

// The stored word: value * 65536, truncated toward zero (mul.s with 65536 in $f4, cvt.w.s, mfc1).
std::uint32_t to_fixed(const std::uint32_t value_bits) {
    return static_cast<std::uint32_t>(fpu::to_int(fpu::mul(kScale, value_bits)));
}

template <class Context>
n3::Run fill_fixed_point_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t index = call.arg(1);

    // All four floats are read before the first store, as the original's lwc1 instructions are.
    const std::uint32_t f3 = m.read_u32_le(record + 28U);
    const std::uint32_t f2 = m.read_u32_le(record + 24U);
    const std::uint32_t f1 = m.read_u32_le(record + 20U);
    const std::uint32_t f0 = m.read_u32_le(record + 16U);
    const GuestAddress block = kBlockTable + index * kBlockBytes;
    m.write_u32_le(block + 60U, to_fixed(f3));
    m.write_u32_le(block + 64U, to_fixed(f2));
    m.write_u32_le(block + 52U, to_fixed(f1));
    m.write_u32_le(block + 56U, to_fixed(f0));

    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t slot = 0U; slot < kSlotCount; ++slot) {
        // Each float is read just before its call, so the earlier calls' writes are seen.
        const std::uint32_t float_bits = m.read_u32_le(record + 32U + 4U * slot);
        call.set_arg(0, block);
        call.set_arg64(1, slot);
        call.set_float_arg(0, float_bits);
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_002fbcd0(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kLoopStepCost + callee.retired;
    }
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002fb3e0, 208U, 0xadb58d7b7843f86aULL, "FillFixedPointRecord", kCeiling, fill_fixed_point_record,
         EngineReturn::Nothing)
