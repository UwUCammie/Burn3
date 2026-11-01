// 0x00222650 (a0 pool manager): InitMemoryPoolTable. Sets +2436 = -1 and +2440 = 0, stores the
// arena base 0x0067d880 at gp-23856, then fills 165 16-byte region descriptors (words: +0 = 0,
// +4 = tag, +8 = size, +12 = base) by calling the leaf 0x003e8c00 (SetRegion: sw $zero,0($a0);
// sw $a1,12($a0); sw $a2,4($a0); jr $ra; sw $a3,8($a0) = 5 instructions, inlined here and
// counted). Bases are the arena s2 (first group) or s1 = s2 + 0x3e3800 (the rest).
// Every loop runs a fixed number of times (8,2,6,10,18,12,6,19,35,12) so there is no
// data-dependent branch: the retired count is one constant.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kArena = 0x0067d880U;         // 0x680000 - 10112
constexpr std::uint32_t kS1 = kArena + 0x003e3800U;
constexpr std::int32_t kGpOffset = -23856;
constexpr std::uint32_t kSetRegionInstructions = 5U;  // 0x003e8c00

// Retired instructions, from brief.py block sums (delay slots included, callee not).
constexpr std::uint32_t kHead = 87U;                  // 0x222650..0x2227a8
constexpr std::uint32_t kTail = 7U;                   // 0x222c70..0x222c88
constexpr std::uint32_t kJalSingles = 10U + 9U + 2U + 2U + 2U;  // calls outside loops (see below)

struct Total {
    std::uint32_t retired;
    std::uint32_t calls;
};

constexpr Total total() {
    // {iterations, instructions per iteration (loop body incl. nop delay slot)}
    constexpr std::uint32_t loops[][2] = {
        {8U, 19U}, {2U, 17U}, {6U, 17U}, {10U, 18U}, {18U, 18U}, {12U, 15U},
        {6U, 17U}, {19U, 18U}, {35U, 18U}, {12U, 15U}};
    std::uint32_t retired = kHead + kTail;
    std::uint32_t calls = 10U;  // head singles (0x22269c..0x2227a0)
    for (const auto& l : loops) {
        retired += l[0] * l[1];
        calls += l[0];
    }
    // Straight blocks between loops (brief.py), each ending in the preceding loop's fall-through.
    // 0x2227f8 (1) | 0x222840..0x222948 (67, 9 calls) | 0x222990 (15, 2 calls) | 0x222a14 (1)
    // | 0x222a60 (16, 2 calls) | 0x222adc (1) | 0x222b24 (15, 2 calls) | 0x222ba8 (1)
    // | 0x222bf4 (16, 2 calls)
    retired += 1U + 67U + 15U + 1U + 16U + 1U + 15U + 1U + 16U;
    calls += 9U + 2U + 2U + 2U + 2U;
    return {retired + calls * kSetRegionInstructions, calls};
}

constexpr Total kTotal = total();

template <class Memory>
inline void set_region(Memory& m, GuestAddress desc, std::uint32_t base, std::uint32_t tag, std::uint32_t size) {
    m.write_u32_le(desc, 0U);
    m.write_u32_le(desc + 12U, base);
    m.write_u32_le(desc + 4U, tag);
    m.write_u32_le(desc + 8U, size);
}

template <class Context>
n3::Run init_memory_pool_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress a0 = call.arg(0);
    constexpr std::uint32_t kNone = 0xffffffffU;

    m.write_u32_le(a0 + 2436U, kNone);
    m.write_u32_le(a0 + 2440U, 0U);
    m.write_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kGpOffset), kArena);

    // Tag -1: arena-relative regions.
    set_region(m, a0 + 4U, kArena, kNone, 0x127000U);
    set_region(m, a0 + 20U, kArena + 0x127000U, kNone, 0x41800U);
    set_region(m, a0 + 36U, kArena + 0x168800U, kNone, 2048U);
    set_region(m, a0 + 52U, kArena + 0x169000U, kNone, 20480U);
    set_region(m, a0 + 68U, kArena + 0x16e000U, kNone, 0x9000U);
    set_region(m, a0 + 84U, kArena + 0x177000U, kNone, 0x8000U);
    set_region(m, a0 + 2444U, kArena + 0x17f000U, kNone, 0x200000U);
    set_region(m, a0 + 2492U, kArena + 0x37f000U, kNone, 22528U);
    set_region(m, a0 + 100U, kArena + 0x39d800U, kNone, 16384U);
    set_region(m, a0 + 244U, kArena + 0x3c3000U, kNone, 0x20800U);
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        set_region(m, a0 + 116U + (i << 4U), kArena + ((((i << 5U) + i) * 2U + i) << 8U) + 0x3a1800U, kNone, 17152U);
    }

    // Tag 0.
    for (std::uint32_t i = 0U; i < 2U; ++i) {
        set_region(m, a0 + 2276U + (i << 4U), kS1 + (((((i << 3U) - i) << 3U) + i) << 13U), 0U, 0x72000U);
    }
    set_region(m, a0 + 2308U, kS1 + 0xe4000U, 0U, 0x20000U);
    set_region(m, a0 + 2324U, kS1 + 0x104000U, 0U, 0x8800U);
    set_region(m, a0 + 2356U, kS1 + 0x10c800U, 0U, 0xe0000U);
    set_region(m, a0 + 2372U, kS1 + 0x1ec800U, 0U, 0x900000U);
    set_region(m, a0 + 2404U, kS1 + 0xaecc00U, 0U, 0x4eb000U);
    set_region(m, a0 + 2388U, kS1 + 0xaec800U, 0U, 1024U);
    set_region(m, a0 + 2420U, kS1 + 0xfd7c00U, 0U, 0x64000U);
    set_region(m, a0 + 2460U, kS1 + 0x103bc00U, 0U, 1U);
    set_region(m, a0 + 2476U, kS1 + 0x103bc80U, 0U, 0x221000U);

    // Tag 1.
    for (std::uint32_t i = 0U; i < 6U; ++i) {
        set_region(m, a0 + 260U + (i << 4U), kS1 + (((((i << 3U) - i) << 3U) + i) << 13U), 1U, 0x72000U);
    }
    set_region(m, a0 + 356U, kS1 + 0x5ac000U, 1U, 0x8800U);
    set_region(m, a0 + 372U, kS1 + 0x5b4800U, 1U, 0x780000U);
    for (std::uint32_t i = 0U; i < 10U; ++i) {
        set_region(m, a0 + 388U + (i << 4U), kS1 + ((((i << 2U) + i) << 15U)) + 0xd34800U, 1U, 0x28000U);
    }
    for (std::uint32_t i = 0U; i < 18U; ++i) {
        set_region(m, a0 + 548U + (i << 4U), kS1 + ((((i << 1U) + i) << 15U)) + 0xec4800U, 1U, 0x18000U);
    }
    set_region(m, a0 + 836U, kS1 + 0x1074800U, 1U, 0x180000U);
    set_region(m, a0 + 852U, kS1 + 0x11f4800U, 1U, 0x18000U);
    for (std::uint32_t i = 0U; i < 12U; ++i) {
        set_region(m, a0 + 868U + (i << 4U), kS1 + (i << 18U) + 0x2ac000U, 1U, 0x40000U);
    }

    // Tag 2.
    for (std::uint32_t i = 0U; i < 6U; ++i) {
        set_region(m, a0 + 1060U + (i << 4U), kS1 + (((((i << 3U) - i) << 3U) + i) << 13U), 2U, 0x72000U);
    }
    set_region(m, a0 + 1156U, kS1 + 0x5ac000U, 2U, 0x8800U);
    set_region(m, a0 + 1172U, kS1 + 0x5b4800U, 2U, 0x580000U);
    for (std::uint32_t i = 0U; i < 19U; ++i) {
        set_region(m, a0 + 1188U + (i << 4U), kS1 + ((((i << 2U) + i) << 15U)) + 0xb34800U, 2U, 0x28000U);
    }
    for (std::uint32_t i = 0U; i < 35U; ++i) {
        set_region(m, a0 + 1492U + (i << 4U), kS1 + ((((i << 1U) + i) << 15U)) + 0xe2c800U, 2U, 0x18000U);
    }
    set_region(m, a0 + 2052U, kS1 + 0x1174800U, 2U, 0x180000U);
    set_region(m, a0 + 2068U, kS1 + 0x12f4800U, 2U, 0x18000U);
    for (std::uint32_t i = 0U; i < 12U; ++i) {
        set_region(m, a0 + 2084U + (i << 4U), kS1 + (i << 18U) + 0x2ac000U, 2U, 0x40000U);
    }

    return call.finish(kTotal.retired);
}

} // namespace

extern constinit const EngineFunction n3_00222650{0x00222650U, 1596U, 0x5a30ae5a51ea1abdULL, "InitMemoryPoolTable",
                                           kTotal.retired, n3::live<&init_memory_pool_table<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00222650, init_memory_pool_table)
