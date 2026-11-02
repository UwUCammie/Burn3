// 0x00173d30 ReleaseObjectChain(n, a0): releases object n and the chain of objects hanging off its "next" word, deepest
// first. For every node on the way down it
//   - sets bit 0 of the flag byte at +378,
//   - frees its resource record when the word at +276 is nonzero (0x00161310 and 0x001613b0 with the record, then
//     0x00163dd0 with the pool manager 0x00527440 and the record), and clears the word,
//   - clears the word at +280 and sets the float at +284 to -1.0f when +280 is nonzero,
//   - re-reads the mode byte at 0x0055d7f7 and follows +272 to the next node when there is one and the mode byte is
//     nonzero or the node's state byte (+372) does not have bit 1 ("keep children") set.
// Coming back up each node is retired with RetireQueueEntry (0x00163e90, pool manager, node, 1) and its state byte
// is cleared. No result is returned.
//
// The original is a recursive routine of about 75 KB that the compiler unrolled nine levels deep, once per outcome of
// the mode byte (511 inlined copies). Levels 0..7 inline the resource release; the level-8 copy calls the outlined
// release 0x00172000, and its onward recursion is a real `jal 0x00173d30`. Neither has a native, so a chain that
// reaches a ninth node is declined and the original runs it (checked against a symbolic run of the whole listing
// for chains up to eleven nodes, all mode/state/flag combinations sampled: counts, calls and store order agree).
//
// Retired counts (callees excluded), per node: 6 flag set and resource test, +9 when a resource is freed, 3 for the
// +280 test, +4 when it is cleared, 4 for the mode gate, 3 for the child read, +4 for the state test when the mode
// byte is zero and there is a child, 5 to set up the retire call and +2 after it (jump + state store), or +1 when the
// copy falls through into its parent's retire, which happens exactly when the parent saw a zero mode byte.
// The top level adds a 12-instruction prologue and a 12-instruction epilogue.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00161310)
N3_CALLEE(001613b0)
N3_CALLEE(00163dd0)
N3_CALLEE(00163e90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPool = 0x00527440U;            // pool manager passed to the queue/pool natives
constexpr GuestAddress kModeByte = 0x0055d7f7U;        // byte: nonzero follows the chain whatever the state byte says

// Fields of a node.
constexpr GuestAddress kNext = 272U;                   // word: next node of the chain
constexpr GuestAddress kResource = 276U;               // word: resource record, or 0
constexpr GuestAddress kRateWord = 280U;               // word: cleared together with kRateValue
constexpr GuestAddress kRateValue = 284U;              // float bits: -1.0f when kRateWord was set
constexpr GuestAddress kState = 372U;                  // byte: bit 1 keeps the node's children
constexpr GuestAddress kFlags = 378U;                  // byte: bit 0 set on release

constexpr std::uint32_t kStateKeepChildren = 0x02U;
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;
constexpr std::uint32_t kFrameBytes = 160U;
constexpr std::uint32_t kInlineLevels = 8U;            // nodes at depth 0..7 are inlined with the resource release
// RetireQueueEntry frees the node's pool record (ReleaseRecordMoveLast) and retires about 15000 instructions per node,
// so a chain of up to eight nodes can reach well over 100 thousand.
constexpr std::uint32_t kCeiling = 400000U;

constexpr std::uint32_t kEntryExitCost = 12U + 12U;    // prologue + epilogue of the top-level call
constexpr std::uint32_t kPrefixCost = 6U;
constexpr std::uint32_t kResourceCost = 9U;
constexpr std::uint32_t kRateTestCost = 3U;
constexpr std::uint32_t kRateClearCost = 4U;
constexpr std::uint32_t kGateCost = 4U;
constexpr std::uint32_t kChildReadCost = 3U;
constexpr std::uint32_t kStateTestCost = 4U;
constexpr std::uint32_t kRetireCost = 5U;
constexpr std::uint32_t kRetireJumpCost = 2U;
constexpr std::uint32_t kRetireFallCost = 1U;

// Releases the node at `depth` and everything below it. False when the call has to be left to the original.
template <class Call>
bool release_node(Call& call, const GuestAddress node, const std::uint32_t depth, const bool parent_mode_zero,
                  std::uint32_t& retired) {
    if (depth >= kInlineLevels) {
        return false;
    }
    auto& m = call.memory();
    m.write_u8(node + kFlags, static_cast<std::uint8_t>(m.read_u8(node + kFlags) | 0x01U));
    retired += kPrefixCost;

    if (const std::uint32_t resource = m.read_u32_le(node + kResource); resource != 0U) {
        call.set_arg(0, resource);
        const n3::Run first = call.invoke([](auto& mm) { return n3::fn_00161310(mm); }, kFrameBytes);
        if (first.declined) {
            return false;
        }
        retired += kResourceCost;
        // The callees run with the node's word read again after each call, as the listing does.
        call.set_arg(0, m.read_u32_le(node + kResource));
        const n3::Run second = call.invoke([](auto& mm) { return n3::fn_001613b0(mm); }, kFrameBytes);
        if (second.declined) {
            return false;
        }
        call.set_arg(1, m.read_u32_le(node + kResource));
        call.set_arg(0, kPool);
        const n3::Run third = call.invoke([](auto& mm) { return n3::fn_00163dd0(mm); }, kFrameBytes);
        if (third.declined) {
            return false;
        }
        m.write_u32_le(node + kResource, 0U);
        retired += first.retired + second.retired + third.retired;
    }
    retired += kRateTestCost;
    if (m.read_u32_le(node + kRateWord) != 0U) {
        m.write_u32_le(node + kRateWord, 0U);
        m.write_u32_le(node + kRateValue, kMinusOneBits);
        retired += kRateClearCost;
    }

    const bool mode_zero = m.read_u8(kModeByte) == 0U;
    retired += kGateCost + kChildReadCost;
    if (const GuestAddress child = m.read_u32_le(node + kNext); child != 0U) {
        bool follow = true;
        if (mode_zero) {
            retired += kStateTestCost;
            follow = (m.read_u8(node + kState) & kStateKeepChildren) == 0U;
        }
        if (follow && !release_node(call, child, depth + 1U, mode_zero, retired)) {
            return false;
        }
    }

    call.set_arg(0, kPool);
    call.set_arg(1, node);
    call.set_arg(2, 1U);
    const n3::Run retire = call.invoke([](auto& mm) { return n3::fn_00163e90(mm); }, kFrameBytes);
    if (retire.declined) {
        return false;
    }
    m.write_u8(node + kState, 0U);
    retired += kRetireCost + retire.retired + ((depth > 0U && parent_mode_zero) ? kRetireFallCost : kRetireJumpCost);
    return retired <= kCeiling;
}

template <class Context>
n3::Run release_object_chain(Context& context) {
    auto call = n3::guest_call(context);
    std::uint32_t retired = kEntryExitCost;
    if (!release_node(call, call.arg(0), 0U, false, retired)) {
        return call.decline();
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00173d30, 8204U, 0x3ca033408f80b89fULL, "ReleaseObjectChain", kCeiling, release_object_chain,
         EngineReturn::Nothing)
