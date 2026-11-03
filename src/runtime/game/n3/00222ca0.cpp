// 0x00222ca0 BuildTextureUploadPacket: builds the GIF upload packet for the textures listed in object O.
//
// O+4500 is the entry count n; entries 0..n-2 are used. Per entry i: O+4512+4i is an optional DMA call address,
// O+4532+4i the log2 width, O+4552+4i the log2 height. The packet is reserved from the queue with
// 001f3080(0, 4n + 16) and appended at the queue cursor (*(gp-24240)):
//   DIRECT 8 { FRAME_1, XYOFFSET_1, SCISSOR_1, TEST_1, ALPHA_1, CLAMP_1, TEX1_1 }
//   per entry: DIRECT 2 { TEX0_1 } and, when the entry has an address, a DMA call tag
//   DIRECT 6 { TEXFLUSH, four state qwords copied from 0x006664f0 }
// The cursor is stored past the footer, then 00221570(0x0067d640) runs.
//
// Counts: 74 up to the first test, 14 to enter the loop, 5 per loop test, 37 per entry (+2 when the texel
// area is negative as a signed word, +2 when the width in texels is negative, +7 for the DMA call tag), 38 for
// the footer, plus the two callees.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)
N3_CALLEE(00221570)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 64U;
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::int32_t kCursorFromGp = -24240;
constexpr GuestAddress kFrameBase = 0x006662d8U;
constexpr GuestAddress kBaseBlock = 0x006662dcU;
constexpr GuestAddress kStateTable = 0x006664f0U;
constexpr GuestAddress kNextStage = 0x0067d640U;

constexpr GuestAddress kCount = 4500U;
constexpr GuestAddress kCallAddress = 4512U;
constexpr GuestAddress kLogWidth = 4532U;
constexpr GuestAddress kLogHeight = 4552U;

constexpr std::uint32_t kToFirstTest = 74U;
constexpr std::uint32_t kEnterLoop = 14U;
constexpr std::uint32_t kLoopTest = 5U;
constexpr std::uint32_t kPerEntry = 37U;
constexpr std::uint32_t kNegativeArea = 2U;
constexpr std::uint32_t kNegativeWidth = 2U;
constexpr std::uint32_t kCallTag = 7U;
constexpr std::uint32_t kFooter = 38U;

constexpr std::uint64_t kDirectTag = 0x50000000ULL;
constexpr std::uint64_t kGifHead = 0x10000000ULL;

template <class Memory>
void put(Memory& m, const GuestAddress at, const std::uint64_t lo, const std::uint64_t hi) {
    n3::store_quad_any(m, at,
                       guest::Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32),
                               static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32)}});
}

// x / 64 rounded toward zero, as the original computes it for a signed word.
std::uint32_t div64(const std::uint32_t x) {
    const std::int32_t s = static_cast<std::int32_t>(x);
    return static_cast<std::uint32_t>((s >= 0 ? s : static_cast<std::int32_t>(x + 63U)) >> 6);
}

template <class Context>
n3::Run build_texture_upload_packet(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    auto& m = call.memory();
    std::uint32_t retired = kToFirstTest;

    // reserve(0, 4n + 16): n is read before the call.
    call.set_arg(0, 0U);
    call.set_arg(1, (m.read_u32_le(object + kCount) << 2) + 16U);
    const n3::Run reserve = call.invoke([](auto& mem) { return n3::fn_001f3080(mem); }, kFrameBytes);
    if (reserve.declined) {
        return call.decline();
    }
    retired += reserve.retired;

    GuestAddress at = m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kCursorFromGp));
    const GuestAddress packet = at;

    put(m, at + 0U, 0x10000008ULL, 0x50000008ULL << 32);
    put(m, at + 16U, kGifHead << 32 | 0x8007U, 14U);
    put(m, at + 32U, (m.read_u32_le(kFrameBase) >> 5) | 0x40000U, 76U);
    put(m, at + 48U, 2048ULL << 32 | 2048U, 24U);
    put(m, at + 64U, 0x00ff000000ff0000ULL, 64U);
    put(m, at + 80U, 0x00031001ULL, 71U);
    put(m, at + 96U, 128ULL << 32 | 168U, 66U);
    put(m, at + 112U, 5U, 8U);
    put(m, at + 128U, 97U, 20U);
    at = packet + 144U;

    std::uint32_t block = 0U;
    const auto last = [&] { return static_cast<std::int32_t>(m.read_u32_le(object + kCount) - 1U); };
    if (last() > 0) {
        retired += kEnterLoop;
        for (std::int32_t i = 0;; ++i) {
            retired += kLoopTest;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            if (!(i < last())) {
                break;
            }
            const GuestAddress entry = object + static_cast<std::uint32_t>(i) * 4U;
            put(m, at + 0U, 0x10000002ULL, (kDirectTag + 2U) << 32);
            put(m, at + 16U, kGifHead << 32 | 0x8001U, 14U);
            const std::uint32_t base = block + m.read_u32_le(kBaseBlock);
            const std::uint32_t log_w = m.read_u32_le(entry + kLogWidth);
            const std::uint32_t log_h = m.read_u32_le(entry + kLogHeight);
            const std::uint32_t width = 1U << (log_w & 31U);
            const std::uint32_t height = 1U << (log_h & 31U);
            const std::uint32_t area = width * height;
            block += div64(area);
            const std::uint64_t buffer_width = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int32_t>(div64(width))));
            const std::uint64_t tex0 =
                static_cast<std::uint64_t>(base) | buffer_width << 14 |
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(log_w))) << 26 |
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(log_h))) << 30 |
                4ULL << 32;
            put(m, at + 32U, tex0, 6U);
            at += 48U;
            const std::uint32_t target = m.read_u32_le(entry + kCallAddress);
            retired += kPerEntry;
            if (static_cast<std::int32_t>(area) < 0) {
                retired += kNegativeArea;
            }
            if (static_cast<std::int32_t>(width) < 0) {
                retired += kNegativeWidth;
            }
            if (target != 0U) {
                put(m, at, static_cast<std::uint64_t>(target) << 32 | kDirectTag, 0U);
                at += 16U;
                retired += kCallTag;
            }
        }
    }

    put(m, at + 0U, 0x10000006ULL, (kDirectTag + 6U) << 32);
    put(m, at + 16U, kGifHead << 32 | 0x8005U, 14U);
    put(m, at + 32U, 0U, 63U);
    for (GuestAddress k = 0U; k < 4U; ++k) {
        n3::store_quad_any(m, at + 48U + 16U * k, n3::load_quad_any(m, kStateTable + 16U * k));
    }
    m.write_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kCursorFromGp), at + 112U);
    retired += kFooter;

    call.set_arg(0, kNextStage);
    const n3::Run next = call.invoke([](auto& mem) { return n3::fn_00221570(mem); }, kFrameBytes);
    if (next.declined) {
        return call.decline();
    }
    return call.finish(retired + next.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00222ca0, 716U, 0x84eff2ec53ff411fULL, "BuildTextureUploadPacket", kCeiling, build_texture_upload_packet,
         EngineReturn::Nothing)
