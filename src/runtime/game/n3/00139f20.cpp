// 0x00139f20 (no arguments, returns nothing): SnapshotStateBlock. Copies the fixed 354-byte block of globals
// 0x0051bfa0..0x0051c101 to 0x0051c220..0x0051c381 (destination = source + 0x280; the two never overlap). It
// does so in the original's chunks: four quadwords (lq/sq), one halfword (lh/sh) and one byte (lbu/sb), the
// copy loops (1 or 2 bytes per trip, each trip reading its bytes before storing them), then three single
// bytes. Every store has the width and the value of the original's store.
//
// The listing has ten loops, but each trip count is an immediate set by an addiu $x, $zero, N right before
// its loop, and nothing else writes that counter. The path is therefore fixed: every call retires the same
// 1465 instructions, which is also the declared cost. No call_cost resolver is needed.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSource = 0x0051bfa0U;       // lui $a1, 0x0052 and addiu -16480
constexpr GuestAddress kDestination = 0x0051c220U;  // lui $t0, 0x0052 and addiu -15840
constexpr GuestAddress kDestinationOffset = kDestination - kSource;

// Retired instructions on the one path, delay slots included: the setup and block sections (5, 9, 5, ...)
// plus the loop bodies (7 per quadword or 1-byte trip, 8 per 2-byte trip) from the listing.
constexpr std::uint32_t kRetired = 5U + 4U * 7U + 9U + 10U * 8U + 5U + 73U * 7U + 5U + 50U * 8U + 5U + 18U * 8U +
                                   5U + 5U * 8U + 5U + 5U * 8U + 5U + 10U * 8U + 5U + 5U * 8U + 5U + 5U * 7U + 13U;

// One copy loop: `trips` trips over `width` bytes (1 or 2) from `first` to `first + kDestinationOffset`.
struct CopyRun {
    GuestAddress first;
    std::uint32_t trips;
    std::uint32_t width;
};

// The copy loops after the quadwords and the halfword, in listing order.
constexpr std::array<CopyRun, 9> kRuns{{
    {0x0051bfe3U, 10U, 2U},
    {0x0051bff7U, 73U, 1U},
    {0x0051c040U, 50U, 2U},
    {0x0051c0a4U, 18U, 2U},
    {0x0051c0c8U, 5U, 2U},
    {0x0051c0d2U, 5U, 2U},
    {0x0051c0dcU, 10U, 2U},
    {0x0051c0f0U, 5U, 2U},
    {0x0051c0faU, 5U, 1U},
}};

void copy_run(auto& memory, const CopyRun& run) {
    for (std::uint32_t trip = 0U; trip < run.trips; ++trip) {
        const GuestAddress source = run.first + run.width * trip;
        std::array<std::uint8_t, 2> bytes{};
        for (std::uint32_t i = 0U; i < run.width; ++i) {
            bytes[i] = memory.read_u8(source + i);
        }
        for (std::uint32_t i = 0U; i < run.width; ++i) {
            memory.write_u8(source + kDestinationOffset + i, bytes[i]);
        }
    }
}

template <class Context>
n3::Run snapshot_state_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    for (GuestAddress offset = 0U; offset < 64U; offset += 16U) {
        n3::store_quad_any(m, kDestination + offset, n3::load_quad_any(m, kSource + offset));
    }
    m.write_u16_le(kDestination + 64U, m.read_u16_le(kSource + 64U));
    m.write_u8(kDestination + 66U, m.read_u8(kSource + 66U));
    for (const CopyRun& run : kRuns) {
        copy_run(m, run);
    }
    m.write_u8(0x0051c37fU, m.read_u8(0x0051c0ffU));
    m.write_u8(0x0051c380U, m.read_u8(0x0051c100U));
    m.write_u8(0x0051c381U, m.read_u8(0x0051c101U));
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00139f20{0x00139f20U, 576U, 0xde144a6cd631bfbfULL, "SnapshotStateBlock",
                                                  kRetired, n3::live<&snapshot_state_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00139f20, snapshot_state_block)
