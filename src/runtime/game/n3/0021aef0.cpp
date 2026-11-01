// 0x0021aef0 (a0 = a NUL-terminated identifier of at most 12 characters): PackBase40Name12. The name becomes a
// 64-bit key of twelve base-40 digits, most significant first. Each character up to the NUL first multiplies the
// key by 40 and then adds its digit: '_' is 39 (tested before the ranges), a signed byte of 97 or more is c - 84,
// 65..96 is c - 52, 48..64 is c - 45, '/' is 2, '-' is 1, and anything else (negative signed bytes included) is 0.
// When the name ends early the key is multiplied by 40 for each position left (the pad loop at 0x0021afb4), so the
// key always has twelve digits. Only the bytes up to the NUL are read, nothing is written, and the whole 64-bit key
// is returned in v0 (the caller stores all of it).
//
// The loops are bounded by the 12-character name (at most 12 characters, then at most 12 pad steps), so this is
// straight code for counting: every block adds the listing's retired count for the path it takes, delay slots
// included. The longest path is twelve characters that each cost 26 ('-' and '0'..'@'), which is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kNameLength = 12U;

// key * 40 on the whole register: dsll 2, daddu, dsll 3 (wrapping 64-bit arithmetic).
constexpr std::uint64_t times_40(const std::uint64_t key) { return ((key << 2U) + key) << 3U; }

// addiu leaves a sign-extended 32-bit value, which the dsll32/dsra32 pair keeps as it is.
constexpr std::uint64_t word_value(const std::int32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

template <class Context>
n3::Run pack_base40_name12(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress name = call.arg(0);
    std::uint64_t key = 0U;
    std::uint32_t retired = 6U;  // aef0..af04: clear v0 and t1, three constants, the beq and its delay slot
    std::uint32_t position = 0U;
    for (;;) {
        retired += 3U;  // afa0..afa8: slti, bne, nop (position < 12)
        if (position >= kNameLength) {
            retired += 4U;  // afac, afb0 (beq to afc8), afc8, afcc: the pad test fails, so there is no pad
            break;
        }
        retired += 4U;  // af08..af14: addu, lb, beq and its nop (the NUL test)
        const auto c = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(name + position)));
        if (c == 0) {
            // afc4..afcc test position < 12; afb4..afc0 multiply the key by 40 once per position left.
            retired += 3U;
            while (position < kNameLength) {
                retired += 4U;
                key = times_40(key);
                ++position;
                retired += 3U;
            }
            break;
        }
        retired += 4U;  // af18..af24: key * 40 (the delay slot dsll runs whichever way the branch goes)
        key = times_40(key);
        if (c == '_') {
            retired += 2U;  // afd8, afdc: beq to af9c, daddiu +39 in its delay slot
            key += 39U;
        } else if (c >= 97) {
            retired += 3U + 5U;  // af28 (not taken), af34..af44
            key += word_value(c - 84);
        } else if (c >= 65) {
            retired += 3U + 3U + 5U;  // af28 (taken), af48 (not taken), af54..af64
            key += word_value(c - 52);
        } else if (c >= 48) {
            retired += 3U + 3U + 3U + 5U;  // af28, af48 (taken), af68 (not taken), af74..af84
            key += word_value(c - 45);
        } else if (c == '/') {
            retired += 3U + 3U + 3U + 2U + 2U;  // af28, af48, af68 (taken), af88 (taken), afe0
            key += 2U;
        } else if (c == '-') {
            retired += 3U + 3U + 3U + 2U + 2U + 1U;  // af28, af48, af68, af88 and af90 (not taken), af98
            key += 1U;
        } else {
            retired += 3U + 3U + 3U + 2U + 2U;  // af28, af48, af68, af88 (not taken), af90 (taken)
        }
        retired += 1U;  // af9c: position++
        ++position;
    }
    retired += 4U;  // afd0, afd4 (beq to afe8), afe8, afec (jr $ra and its nop)
    call.return_value64(key);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0021aef0{0x0021aef0U, 256U, 0xa3ed6aeddbf61fdeULL, "PackBase40Name12", 329U,
                                                  n3::live<&pack_base40_name12<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021aef0, pack_base40_name12)
