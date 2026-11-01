// 0x00369090 (a0 = the object, not read; a1 = a pending code id): ShouldClearPendingCode, a pure classifier.
// Its only caller (0x00369270) stores 0 to obj+304 when v0 is non-zero, so v0 = 1 means "clear the pending code".
// v0 is 0 for a1 in 197..199, 147..196 (signed), 133, 134, 122..126 and 118..120, and 1 for everything else
// (negatives, 0..117, 121, 127..132, 135..146, 200 and up). The tests run in the original's order: a1 - 197
// (unsigned), a1 < 147 (signed), a1 < 197 (signed), a1 == 133, a1 == 134, a1 - 122 (unsigned), a1 - 118
// (unsigned). Each exit counts the blocks the original runs on that path, delay slots included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Retired instructions per block of the listing, delay slots included (brief.py's block table).
constexpr std::uint32_t kBlock197 = 4U;        // 0x00369090: addiu, sltiu, bne (to 0x003690f8), nop
constexpr std::uint32_t kBlock147 = 3U;        // 0x003690a0: slti 147, bne (to 0x003690b8), nop
constexpr std::uint32_t kBlockSigned197 = 3U;  // 0x003690ac: slti 197, bne (to 0x003690f0), nop
constexpr std::uint32_t kBlock133 = 3U;        // 0x003690b8: addiu 133, beq (to 0x003690f0), nop
constexpr std::uint32_t kBlock134 = 3U;        // 0x003690c4: addiu 134, beq (to 0x00369100), nop
constexpr std::uint32_t kBlock122 = 4U;        // 0x003690d0: addiu -122, sltiu 5, bne (to 0x00369108), nop
constexpr std::uint32_t kBlock118 = 4U;        // 0x003690e0: addiu -118, sltiu 3, beq (to 0x0036910c), xori
constexpr std::uint32_t kExitZero = 2U;        // 0x003690f0, 0x003690f8, 0x00369100: beq zero, daddu zero
constexpr std::uint32_t kExitFall = 1U;        // 0x00369108: daddu zero, falls into the return block
constexpr std::uint32_t kExitReturn = 2U;      // 0x0036910c: jr ra and its delay slot

struct PendingCodeVerdict {
    std::uint32_t clear;    // v0: 1 clears the pending code
    std::uint32_t retired;  // original instructions executed on this path
};

PendingCodeVerdict classify_pending_code(const std::uint64_t code) {
    // slti and beq compare the whole 64-bit register. addiu wraps at 32 bits and sltiu sees its sign-extended
    // result, so the range tests use the low word.
    const auto low = static_cast<std::uint32_t>(code);
    const auto signed_code = static_cast<std::int64_t>(code);
    if (low - 197U < 3U) {
        return {0U, kBlock197 + kExitZero + kExitReturn};  // 8
    }
    // A signed a1 below 147 skips the signed 197 test (0x003690ac) and goes straight to the 133 test.
    std::uint32_t retired = kBlock197 + kBlock147;  // 7
    if (signed_code >= 147) {
        retired += kBlockSigned197;  // 10
        if (signed_code < 197) {
            return {0U, retired + kExitZero + kExitReturn};  // 14
        }
    }
    retired += kBlock133;  // 10 below 147, 13 from 200 up
    if (code == 133U) {
        return {0U, retired + kExitZero + kExitReturn};  // 14
    }
    retired += kBlock134;
    if (code == 134U) {
        return {0U, retired + kExitZero + kExitReturn};  // 17
    }
    retired += kBlock122;
    if (low - 122U < 5U) {
        return {0U, retired + kExitFall + kExitReturn};  // 20
    }
    retired += kBlock118;
    // 0 for 118..120, 1 otherwise. Below 147 this path retires 23, from 200 up 26.
    const std::uint32_t clear = (low - 118U < 3U) ? 0U : 1U;
    return {clear, retired + kExitReturn};
}

template <class Context>
n3::Run should_clear_pending_code(Context& context) {
    auto call = n3::guest_call(context);
    const PendingCodeVerdict verdict = classify_pending_code(call.arg64(1));
    call.return_value(verdict.clear);
    return call.finish(verdict.retired);
}

} // namespace

extern constinit const EngineFunction n3_00369090{0x00369090U, 132U, 0x216cab4dd0a05d18ULL,
                                                  "ShouldClearPendingCode", 26U, n3::live<&should_clear_pending_code<NativeFunctionContext>>,
                                                  EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00369090, should_clear_pending_code)
