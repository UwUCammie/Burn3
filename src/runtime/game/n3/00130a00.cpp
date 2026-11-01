// 0x00130a00 (a0: a 4-byte record r[0..3]). Six pending event flags are single
// bytes at $gp-24552 (G0), -24548 (G1), -24544 (G2), -24540 (G3), -24536 (G4)
// and -24532 (G5). They set the state bytes of the record, then all six flags
// are cleared. The steps run in listing order and each one reads the record and
// the flags as the earlier steps left them, so every read and store below keeps
// that order (the record may alias the flags).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Distances below $gp of the six flags.
constexpr std::uint32_t kBelowG0 = 24552U;
constexpr std::uint32_t kBelowG1 = 24548U;
constexpr std::uint32_t kBelowG2 = 24544U;
constexpr std::uint32_t kBelowG3 = 24540U;
constexpr std::uint32_t kBelowG4 = 24536U;
constexpr std::uint32_t kBelowG5 = 24532U;

template <class Context>
n3::Run resolve_button_pair_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress g0 = gp - kBelowG0;
    const GuestAddress g1 = gp - kBelowG1;
    const GuestAddress g2 = gp - kBelowG2;
    const GuestAddress g3 = gp - kBelowG3;
    const GuestAddress g4 = gp - kBelowG4;
    const GuestAddress g5 = gp - kBelowG5;
    const GuestAddress record = call.arg(0);
    // Retired original instructions on the path taken (no loops; each block's
    // count from brief.py, delay slots included).
    std::uint32_t retired = 3U;

    // G0: (r2, r3) set, (r0, r1) cleared.
    if (m.read_u8(g0) != 0U) {
        m.write_u8(record + 2U, 1U);
        m.write_u8(record + 3U, 1U);
        m.write_u8(record + 0U, 0U);
        m.write_u8(record + 1U, 0U);
        retired += 6U;
    }
    retired += 3U;
    // G1: r2 set, r0 cleared.
    if (m.read_u8(g1) != 0U) {
        m.write_u8(record + 2U, 1U);
        m.write_u8(record + 0U, 0U);
        retired += 4U;
    }
    retired += 3U;
    // G4: (r2, r3) cleared, (r0, r1) set.
    if (m.read_u8(g4) != 0U) {
        m.write_u8(record + 2U, 0U);
        m.write_u8(record + 3U, 0U);
        m.write_u8(record + 0U, 1U);
        m.write_u8(record + 1U, 1U);
        retired += 6U;
    }
    retired += 3U;
    // G5: r3 cleared, r1 set.
    if (m.read_u8(g5) != 0U) {
        m.write_u8(record + 3U, 0U);
        m.write_u8(record + 1U, 1U);
        retired += 4U;
    }
    retired += 3U;
    // Step 5, while r2 is set: G2 picks (r0, G3) = (1, 1) or (0, 0).
    if (m.read_u8(record + 2U) != 0U) {
        retired += 3U;
        if (m.read_u8(g2) != 0U) {
            m.write_u8(record + 0U, 1U);
            m.write_u8(g3, 1U);
            retired += 4U;
        } else {
            m.write_u8(record + 0U, 0U);
            m.write_u8(g3, 0U);
            retired += 2U;
        }
    }
    retired += 3U;
    // Step 6, while r3 is set: r1 follows G3, which step 5 may just have written.
    if (m.read_u8(record + 3U) != 0U) {
        retired += 3U;
        if (m.read_u8(g3) != 0U) {
            m.write_u8(record + 1U, 1U);
            retired += 3U;
        } else {
            m.write_u8(record + 1U, 0U);
            retired += 1U;
        }
    }
    // Step 7: all six flags cleared (six stores and the branch to the return,
    // 7 instructions), then jr and its delay slot (2).
    m.write_u8(g0, 0U);
    m.write_u8(g1, 0U);
    m.write_u8(g2, 0U);
    m.write_u8(g3, 0U);
    m.write_u8(g4, 0U);
    m.write_u8(g5, 0U);
    retired += 7U + 2U;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00130a00{0x00130a00U, 252U, 0x6b636705cd0f513dULL, "ResolveButtonPairState",
                                                  60U, n3::live<&resolve_button_pair_state<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00130a00, resolve_button_pair_state)
