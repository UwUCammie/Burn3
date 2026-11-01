// 0x001632c0 (a0 = B, one very large world/scene struct): InitScenePools. Re-initialises the struct:
// two magic header words, a run of cleared counters and flags, two free-list pools (254 nodes of 384
// bytes at B+0x127d0 chained through +0x108, 254 nodes of 80 bytes at B+0x2a4d0 numbered in a byte at
// +0x44), the small per-slot tables (three 8-word arrays and six slots of word/byte tables), and two
// global scratch buffers (16000 bytes of 0xFF at the pointer 0x0051d7c0, 4000 zero bytes at
// 0x005216c0). Every loop runs a fixed number of times, so the retired count is the same on every call
// (212541 instructions, tallied below from the listing). No calls. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kHi = 0x00030000U;          // lui 3: most fields are B + 0x30000 + small offset
constexpr GuestAddress kPool1 = 0x000127d0U;       // pool 1 head (node 0)
constexpr GuestAddress kPool2 = 0x0002a4d0U;       // pool 2 head (node 0)
constexpr GuestAddress kScratchPtrValue = 0x0051d7c0U;  // lui 0x52, -10304
constexpr GuestAddress kScratchZero = 0x005216c0U;      // lui 0x52, +5824
constexpr std::uint32_t kRetired = 212541U;

template <class Context>
n3::Run init_scene_pools(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress b = call.arg(0);
    const GuestAddress h = b + kHi;

    // --- Part 1: header, flags, pool 1, counters, pool 2 (0x1632c0 .. 0x1634dc) ---
    m.write_u32_le(h + 25420U, 0x02b9d6f8U);
    m.write_u32_le(h + 25416U, 0xfd462907U);
    m.write_u32_le(h + 25504U, 0U);
    m.write_u16_le(h + 25508U, 0U);
    m.write_u8(h + 25510U, 0U);
    for (std::uint32_t i = 0U; i < 2U; ++i) {
        m.write_u8(h + 25513U + i, 0U);
        m.write_u8(h + 25511U + i, 0U);
    }
    for (std::uint32_t k = 25515U; k <= 25521U; ++k) {
        m.write_u8(h + k, 0U);
    }
    m.write_u8(h + 25532U, 1U);
    m.write_u32_le(h + 25444U, b + kPool1);
    for (std::uint32_t i = 0U; i < 254U; ++i) {
        const GuestAddress node = b + kPool1 + 384U * i;
        if (i < 253U) {
            m.write_u32_le(node + 0x108U, node + 384U);
        } else {
            m.write_u32_le(node + 0x108U, 0U);
            m.write_u32_le(h + 25448U, node);
        }
    }
    m.write_u32_le(h + 25456U, 0U);
    m.write_u32_le(h + 25460U, 0U);
    for (std::uint32_t i = 0U; i < 32U; ++i) {
        m.write_u8(h + 25464U + i, 0U);
    }
    m.write_u8(h + 25496U, 0U);
    m.write_u8(h + 25497U, 0U);
    m.write_u32_le(h + 25452U, b + kPool2);
    for (std::uint32_t i = 0U; i < 253U; ++i) {
        m.write_u8(h - 23276U + 80U * i, static_cast<std::uint8_t>(i + 1U));
    }

    // --- Part 2: three 8-word arrays, six slots of tables (0x1634dc .. 0x16364c) ---
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        m.write_u32_le(h + 25312U + 4U * i, 0U);
    }
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        m.write_u32_le(h + 25344U + 4U * i, 0U);
    }
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        m.write_u32_le(h + 25376U + 4U * i, 0U);
    }
    for (std::uint32_t s = 0U; s < 6U; ++s) {
        m.write_u8(h + s + 25424U, 0U);
        m.write_u8(h + s + 25430U, 0U);
        m.write_u8(h + s + 25436U, 0U);
        const GuestAddress words = b + 0x34454U + 1016U * s;
        const GuestAddress bytes = b + 0x35c24U + 254U * s;
        for (std::uint32_t j = 0U; j < 254U; ++j) {
            m.write_u32_le(words + 4U * j, 0U);
            m.write_u8(bytes + j, 0U);
        }
        for (std::uint32_t j = 0U; j < 25U; ++j) {
            m.write_u8(b + 0x36218U + 25U * s + j, 0U);
        }
        for (std::uint32_t j = 0U; j < 8U; ++j) {
            m.write_u8(b + 0x362aeU + 8U * s + j, 0U);
        }
    }

    // --- Part 3: the two scratch buffers (0x16364c .. return) ---
    m.write_u32_le(h + 25500U, kScratchPtrValue);
    m.write_u32_le(h + 25408U, 0U);
    for (std::uint32_t i = 0U; i < 16000U; ++i) {
        // The pointer is reloaded every iteration, as the original does.
        const GuestAddress ptr = m.read_u32_le(h + 25500U);
        m.write_u8(ptr + i, 0xFFU);
    }
    for (std::uint32_t i = 0U; i < 4000U; ++i) {
        m.write_u8(kScratchZero + i, 0U);
    }
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_001632c0{0x001632c0U, 1080U, 0x359d5285c43a8251ULL, "InitScenePools", kRetired,
                                           n3::live<&init_scene_pools<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001632c0, init_scene_pools)
