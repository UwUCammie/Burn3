// 0x0031e800 (a0 = layout object L): FitHudItemsToGaps. A layout pass over 11 on-screen items. Slot s (0..10)
// has a rect-record pointer R_s at L+28+4s ({x, y, width, height} floats) and a neighbour pointer B_s at
// L+72+4s (non-null test, B_s[0] = x, B_s[8] = width). L[16] is a flags word (bit 0x80), L[148], L[152] and
// L[156] are optional bounding rects ([0] = x, [8] = width). Position i is visited as slot order[i] (word table
// at 0x004c5310); T[s] (float table at 0x004c52e0) is a per-slot threshold. An item whose y is at least 416.0 is
// shrunk (width and height by one ratio) when it overlaps the next visible item (forward mode: T[s] <= 0.5 or
// slot 2 with the flag set) or the previous one (backward mode), and then re-anchored at the left/top (forward)
// or right/top (backward) edge it had. t5, t6 (and the f5/f6 registers) deliberately stay live across outer
// iterations because the original reads stale values on a few paths (slot 0 with the flag set reuses t6).
// Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kThresholds = 0x004c52e0U;   // lui 0x4c, addiu 21216: float per slot
constexpr GuestAddress kOrder = 0x004c5310U;        // lui 0x4c, addiu 21264: 11 slot numbers
constexpr std::uint32_t kSlots = 11U;
constexpr std::uint32_t k416 = 0x43d00000U;
constexpr std::uint32_t kGap = 0x40a00000U;         // 5.0
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t k640 = 0x44200000U;
constexpr std::uint32_t kFlagBit = 0x80U;

constexpr GuestAddress kSlotPtr = 28U;
constexpr GuestAddress kNeighbourPtr = 72U;
constexpr GuestAddress kFlags = 16U;
constexpr GuestAddress kBounds148 = 148U;
constexpr GuestAddress kBounds152 = 152U;
constexpr GuestAddress kBounds156 = 156U;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Entry {
    std::uint32_t t5;
    std::uint32_t t6;
    std::uint32_t t8;
    std::uint32_t s1;
    std::uint32_t f5;
    std::uint32_t f6;
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as a loop passes that.
template <class Memory>
std::uint32_t fit(Memory& m, const GuestAddress a0, const Entry& entry) {
    std::uint32_t retired = 20U;                    // prologue 0x31e800..0x31e84c
    std::uint32_t i = 0U;                           // t7
    std::uint32_t t5 = entry.t5;
    std::uint32_t t6 = entry.t6;
    std::uint32_t t8 = entry.t8;
    std::uint32_t s1 = entry.s1;
    std::uint32_t f5 = entry.f5;
    std::uint32_t f6 = entry.f6;
    std::uint32_t f7 = 0U;
    std::uint32_t f8 = 0U;
    std::uint32_t f9 = 0U;
    std::uint32_t f10 = 0U;
    std::uint32_t f11 = 0U;
    std::uint32_t f12 = 0U;
    std::uint32_t acc = 0U;
    std::uint32_t s = 0U;                           // t9: slot number
    std::uint32_t off = 0U;                         // 4 * s
    std::uint32_t s0 = 0U;
    std::uint32_t threshold = 0U;
    std::uint32_t flags = 0U;
    GuestAddress t4 = 0U;                           // R of the slot
    GuestAddress slot_address = 0U;                 // t2: a0 + 4s + 28
    GuestAddress rect8 = 0U;                        // t3: R + 8
    GuestAddress t9 = 0U;
    std::uint32_t stack_x = 0U;                     // 112($sp): anchor x
    std::uint32_t stack_y = 0U;                     // 116($sp): anchor y
    std::uint32_t stack_px = 0U;                    // 120($sp)
    std::uint32_t stack_py = 0U;                    // 124($sp)

L_850:
    s = m.read_u32_le(kOrder + 4U * i);
    off = s << 2U;
    t4 = m.read_u32_le(a0 + off + kSlotPtr);
    retired += 8U;
    if (t4 == 0U) goto L_a00;
    f5 = m.read_u32_le(t4 + 4U);
    retired += 4U;
    if (fpu::lt(f5, k416)) goto L_a00;
    threshold = m.read_u32_le(kThresholds + off);
    retired += 5U;
    if (fpu::le(threshold, kHalf)) goto L_8ac;
    retired += 2U;
    if (s != 2U) goto L_af8;
    flags = m.read_u32_le(a0 + kFlags) & kFlagBit;
    retired += 4U;
    if (flags == 0U) goto L_af8;

L_8ac:
    // forward mode: anchor (0.0, 0.5)
    stack_x = 0U;
    stack_y = kHalf;
    slot_address = a0 + off + kSlotPtr;
    t8 = m.read_u32_le(slot_address);
    f7 = m.read_u32_le(t8 + 4U);
    rect8 = t8 + 8U;
    f8 = m.read_u32_le(t8);
    f5 = m.read_u32_le(t8 + 12U);
    acc = fpu::add(0U, f7);
    stack_px = f8;
    stack_py = f7;
    f5 = fpu::madd(acc, kHalf, f5);
    retired += 16U;
    if (s == 0U) goto L_a18;
L_8ec:
    retired += 2U;
    t6 = i + 1U;
    goto L_934;

L_8f4:
    t5 = m.read_u32_le(kOrder + 4U * t6);
    s0 = a0 + (t5 << 2U);
    t8 = m.read_u32_le(s0 + kNeighbourPtr);
    retired += 8U;
    if (t8 == 0U) goto L_928;
    s0 = m.read_u32_le(s0 + kSlotPtr);
    f7 = m.read_u32_le(s0 + 4U);
    retired += 5U;
    if (fpu::lt(f7, k416)) goto L_930;
L_928:
    retired += 2U;
    if (t8 != 0U) goto L_940;
L_930:
    retired += 1U;
    t6 += 1U;
L_934:
    retired += 3U;
    if (retired > kCeiling) return retired;
    if (static_cast<std::int32_t>(t6) < 11) goto L_8f4;
L_940:
    s0 = m.read_u32_le(a0 + kBounds156);
    retired += 3U;
    if (s0 == 0U) goto L_a30;
    f8 = m.read_u32_le(s0);
    f7 = m.read_u32_le(t4);
    retired += 5U;
    if (!fpu::lt(f7, f8)) goto L_a30;
    f6 = fpu::sub(f8, f7);
    f6 = fpu::sub(f6, kGap);
    retired += 2U;

L_968:
    // shared shrink
    f7 = m.read_u32_le(rect8);
    retired += 4U;
    if (fpu::le(f7, f6)) goto L_a00;
    rect8 = m.read_u32_le(slot_address);
    f10 = stack_x;
    f7 = stack_y;
    f9 = stack_px;
    f11 = m.read_u32_le(rect8 + 8U);
    f8 = m.read_u32_le(rect8 + 12U);
    f12 = fpu::div(f6, f11);
    f11 = fpu::mul(f12, f11);
    f8 = fpu::mul(f12, f8);
    m.write_u32_le(rect8 + 8U, f11);
    m.write_u32_le(rect8 + 12U, f8);
    rect8 = m.read_u32_le(slot_address);
    f11 = m.read_u32_le(rect8 + 8U);
    f8 = m.read_u32_le(rect8 + 12U);
    f10 = fpu::mul(f11, f10);
    f7 = fpu::mul(f8, f7);
    f8 = fpu::sub(f9, f10);
    f5 = fpu::sub(f5, f7);
    m.write_u32_le(rect8, f8);
    m.write_u32_le(rect8 + 4U, f5);
    retired += 34U;

L_a00:
    i += 1U;
    retired += 4U;
    if (retired > kCeiling) return retired;
    if (i < kSlots) goto L_850;
    return retired + 5U;                             // 0x31ea10 (2) + epilogue 0x31ecbc (3)

L_a18:
    flags = m.read_u32_le(a0 + kFlags) & kFlagBit;
    retired += 4U;
    if (flags == 0U) goto L_8ec;
    retired += 2U;
    t5 = 2U;
    goto L_940;

L_a30:
    retired += 3U;
    if (static_cast<std::int32_t>(t6) < 11) goto L_ab4;
L_a3c:
    retired += 2U;
    t6 = i + 1U;
    goto L_a98;

L_a44:
    t5 = m.read_u32_le(kOrder + 4U * t6);
    s0 = a0 + (t5 << 2U);
    s1 = m.read_u32_le(s0 + kNeighbourPtr);
    retired += 8U;
    if (s1 == 0U) goto L_a78;
    s0 = m.read_u32_le(s0 + kSlotPtr);
    f7 = m.read_u32_le(s0 + 4U);
    retired += 5U;
    if (fpu::lt(f7, k416)) goto L_a94;
L_a78:
    retired += 2U;
    if (s1 == 0U) goto L_a94;
    f7 = m.read_u32_le(s1);
    f6 = m.read_u32_le(t4);
    f6 = fpu::sub(f7, f6);
    f6 = fpu::sub(f6, kGap);
    retired += 5U;
    goto L_aa4;
L_a94:
    retired += 1U;
    t6 += 1U;
L_a98:
    retired += 3U;
    if (retired > kCeiling) return retired;
    if (static_cast<std::int32_t>(t6) < 11) goto L_a44;
L_aa4:
    retired += 2U;
    if (t6 != 11U) goto L_968;
    retired += 2U;
    f6 = k640;
    goto L_968;

L_ab4:
    t8 = m.read_u32_le(a0 + kBounds152);
    retired += 3U;
    if (t8 == 0U) goto L_a3c;
    f7 = m.read_u32_le(kThresholds + (t5 << 2U));
    retired += 6U;
    if (fpu::le(f7, kHalf)) goto L_a3c;
    f8 = m.read_u32_le(t8);
    f7 = m.read_u32_le(t4);
    retired += 5U;
    if (!fpu::lt(f7, f8)) goto L_a3c;
    f6 = fpu::sub(f8, f7);
    f6 = fpu::sub(f6, kGap);
    retired += 3U;
    goto L_968;

L_af8:
    // backward mode: anchor (1.0, 0.5)
    stack_x = kOne;
    stack_y = kHalf;
    slot_address = a0 + off + kSlotPtr;
    t8 = m.read_u32_le(slot_address);
    t6 = i - 1U;
    f9 = m.read_u32_le(t8);
    rect8 = t8 + 8U;
    f8 = m.read_u32_le(t8 + 8U);
    f7 = m.read_u32_le(t8 + 12U);
    f5 = m.read_u32_le(t8 + 4U);
    f8 = fpu::add(f9, f8);
    f5 = fpu::add(f5, f7);
    stack_px = f8;
    stack_py = f5;
    f5 = stack_py;
    acc = fpu::add(0U, f5);
    f5 = fpu::madd(acc, kHalf, f7);
    retired += 23U;
    goto L_b94;

L_b54:
    t5 = m.read_u32_le(kOrder + 4U * t6);
    s0 = a0 + (t5 << 2U);
    t8 = m.read_u32_le(s0 + kNeighbourPtr);
    retired += 8U;
    if (t8 == 0U) goto L_b88;
    s0 = m.read_u32_le(s0 + kSlotPtr);
    f7 = m.read_u32_le(s0 + 4U);
    retired += 5U;
    if (fpu::lt(f7, k416)) goto L_b90;
L_b88:
    retired += 2U;
    if (t8 != 0U) goto L_b9c;
L_b90:
    retired += 1U;
    t6 -= 1U;
L_b94:
    retired += 2U;
    if (retired > kCeiling) return retired;
    if (static_cast<std::int32_t>(t6) >= 0) goto L_b54;
L_b9c:
    s0 = m.read_u32_le(a0 + kBounds152);
    retired += 3U;
    if (s0 != 0U) goto L_c68;
L_ba8:
    retired += 2U;
    t6 = i - 1U;
    goto L_c50;

L_bb0:
    t5 = m.read_u32_le(kOrder + 4U * t6);
    s0 = t5 << 2U;
    t9 = a0 + s0;
    s1 = m.read_u32_le(t9 + kNeighbourPtr);
    retired += 8U;
    if (s1 == 0U) goto L_be4;
    t8 = m.read_u32_le(t9 + kSlotPtr);
    f7 = m.read_u32_le(t8 + 4U);
    retired += 5U;
    if (fpu::lt(f7, k416)) goto L_c4c;
L_be4:
    retired += 2U;
    if (s1 == 0U) goto L_c4c;
    f6 = m.read_u32_le(kThresholds + s0);
    retired += 5U;
    if (!fpu::le(f6, kHalf)) goto L_c28;
    f8 = m.read_u32_le(t4);
    t8 = m.read_u32_le(t9 + kSlotPtr);
    f6 = m.read_u32_le(t4 + 8U);
    f7 = m.read_u32_le(t8);
    f8 = fpu::add(f8, f6);
    f6 = m.read_u32_le(t8 + 8U);
    f6 = fpu::add(f7, f6);
    f6 = fpu::sub(f8, f6);
    f6 = fpu::sub(f6, kGap);
    retired += 10U;
    goto L_c58;
L_c28:
    f9 = m.read_u32_le(t4);
    f8 = m.read_u32_le(t4 + 8U);
    f7 = m.read_u32_le(s1);
    f6 = m.read_u32_le(s1 + 8U);
    f8 = fpu::add(f9, f8);
    f6 = fpu::add(f7, f6);
    f6 = fpu::sub(f8, f6);
    f6 = fpu::sub(f6, kGap);
    retired += 9U;
    goto L_c58;
L_c4c:
    retired += 1U;
    t6 -= 1U;
L_c50:
    retired += 2U;
    if (retired > kCeiling) return retired;
    if (static_cast<std::int32_t>(t6) >= 0) goto L_bb0;
L_c58:
    retired += 2U;
    if (static_cast<std::int32_t>(t6) >= 0) goto L_968;
    retired += 2U;
    f6 = k640;
    goto L_968;

L_c68:
    retired += 2U;
    if (static_cast<std::int32_t>(t6) < 0) goto L_ba8;
    f7 = m.read_u32_le(kThresholds + (t5 << 2U));
    retired += 6U;
    if (!fpu::le(f7, kHalf)) goto L_ba8;
    f9 = m.read_u32_le(t4 + 8U);
    t8 = m.read_u32_le(a0 + kBounds148);
    f7 = m.read_u32_le(t4);
    f8 = m.read_u32_le(t8 + 8U);
    f9 = fpu::add(f7, f9);
    f7 = m.read_u32_le(t8);
    f7 = fpu::add(f7, f8);
    retired += 10U;
    if (!fpu::lt(f7, f9)) goto L_ba8;
    f6 = fpu::sub(f9, f7);
    f6 = fpu::sub(f6, kGap);
    retired += 3U;
    goto L_c58;
}

Entry read_entry(const guest::R5900State& cpu) {
    return Entry{cpu.read_gpr_u32(13U), cpu.read_gpr_u32(14U), cpu.read_gpr_u32(24U),
                 cpu.read_gpr_u32(17U), cpu.read_fpr_bits(5U), cpu.read_fpr_bits(6U)};
}

template <class Context>
std::uint32_t fit_hud_items_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = fit(dry, context.cpu.read_gpr_u32(4U), read_entry(context.cpu));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run fit_hud_items(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(fit(call.memory(), call.arg(0), read_entry(call.cpu())));
}

} // namespace

extern constinit const EngineFunction n3_0031e800{0x0031e800U, 1224U, 0xbb995a8ea1230103ULL, "FitHudItemsToGaps",
                                                  kCeiling, n3::live<&fit_hud_items<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &fit_hud_items_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0031e800, fit_hud_items, fit_hud_items_cost)
