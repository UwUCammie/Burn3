// 0x001ba950 (a0 = object A, a1 = object B): the word of a 7 x 7 table at 0x004b7bc0 for the pair of object kinds.
// Each object's kind comes from ClassifyObjectKind (0x001baa30, see n3/001baa30.cpp): kind A and kind B. The table
// word is at index 7 * row + column, where the row is kind A and the column kind B. Kind 1 paired with kind 5, in
// either order, is the one special pair: when the kind-1 object's byte at ((word at (word at object + 12)) + 5108)
// + 6572 equals the kind-5 object's byte at (word at object + 12) + 9259 (both signed), the kind-5 object counts as
// kind 4 for the lookup. Every other pair keeps its kinds. The word is returned in v0 (sign-extended, as lw does).
//
// Retired counts per block of the listing, delay slots included: the two calls and the kind-A test are 14
// (001ba950-001ba984), one kind test (addiu, bne, nop) is 3, the byte test is 7, the table tail
// (001ba9f0-001baa28) is 15.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001baa30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPairTable = 0x004b7bc0U;   // lui 0x4b, addiu 31680
constexpr std::uint32_t kRowLength = 7U;           // seven words per kind A row (sll 3, subu, sll 2)
constexpr std::uint32_t kKindOne = 1U;
constexpr std::uint32_t kKindFour = 4U;
constexpr std::uint32_t kKindFive = 5U;
constexpr GuestAddress kPayloadPointer = 12U;      // lw ..., 12(object)
constexpr GuestAddress kInnerPointer = 5108U;      // lw ..., 5108(payload)
constexpr GuestAddress kInnerByte = 6572U;         // lb ..., 6572(inner)
constexpr GuestAddress kPayloadByte = 9259U;       // lb ..., 9259(payload)
constexpr std::uint32_t kFrameBytes = 64U;         // addiu $sp, $sp, -64
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrefixCost = 14U;         // both calls and their delay slots, then the kind-A test
constexpr std::uint32_t kTestCost = 3U;            // addiu $v1; bne; nop: one kind test
constexpr std::uint32_t kBytesCost = 7U;           // the payload loads, the two lb, and the bne with its delay slot
constexpr std::uint32_t kColumnFourCost = 2U;      // beq to the table, addiu $v0, 4
constexpr std::uint32_t kRowFourCost = 1U;         // addiu $s0, 4
constexpr std::uint32_t kTailCost = 15U;           // table word, frame restore, jr $ra and its delay slot

// The table cell a pair selects and the instructions retired on the way (the two calls excluded).
struct PairPath {
    std::uint32_t row{};
    std::uint32_t column{};
    std::uint32_t retired{};
};

// The byte test between a kind-1 object (one) and a kind-5 object (five), loaded in the listing's order. lb sign-
// extends both bytes, so comparing the raw bytes gives the same answer as the bne on the extended words.
template <class Memory>
bool payload_bytes_match(const Memory& memory, const GuestAddress one, const GuestAddress five) {
    const GuestAddress one_payload = memory.read_u32_le(one + kPayloadPointer);
    const GuestAddress five_payload = memory.read_u32_le(five + kPayloadPointer);
    const GuestAddress one_inner = memory.read_u32_le(one_payload + kInnerPointer);
    const std::uint8_t five_byte = memory.read_u8(five_payload + kPayloadByte);
    const std::uint8_t one_byte = memory.read_u8(one_inner + kInnerByte);
    return one_byte == five_byte;
}

template <class Memory>
PairPath pair_path(const Memory& memory, const GuestAddress object_a, const GuestAddress object_b,
                   const std::uint32_t kind_a, const std::uint32_t kind_b) {
    if (kind_a == kKindOne && kind_b == kKindFive) {
        const std::uint32_t before = kPrefixCost + kTestCost + kBytesCost + kTailCost;
        if (payload_bytes_match(memory, object_a, object_b)) {
            return {kind_a, kKindFour, before + kColumnFourCost};
        }
        return {kind_a, kind_b, before};
    }
    if (kind_a == kKindFive && kind_b == kKindOne) {
        const std::uint32_t before = kPrefixCost + 2U * kTestCost + kBytesCost + kTailCost;
        if (payload_bytes_match(memory, object_b, object_a)) {
            return {kKindFour, kind_b, before + kRowFourCost};
        }
        return {kind_a, kind_b, before};
    }
    // Every other pair runs the kind-B test; each kind 1 among the two adds one more test block.
    const std::uint32_t tests = kTestCost + (kind_a == kKindOne ? kTestCost : 0U) +
                                (kind_b == kKindOne ? kTestCost : 0U);
    return {kind_a, kind_b, kPrefixCost + tests + kTailCost};
}

template <class Context>
n3::Run object_pair_table_word(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object_a = call.arg(0);
    const std::uint64_t object_b_register = call.arg64(1);  // ClassifyObjectKind overwrites $a1, so keep B first

    // jal 0x001baa30 with $a0 still object A.
    const n3::Run run_a = call.invoke([](auto& m) { return n3::fn_001baa30(m); }, kFrameBytes);
    if (run_a.declined) {
        return call.decline();
    }
    const std::uint32_t kind_a = call.result();

    call.set_arg64(0, object_b_register);  // daddu $a0, $s1, $zero
    const n3::Run run_b = call.invoke([](auto& m) { return n3::fn_001baa30(m); }, kFrameBytes);
    if (run_b.declined) {
        return call.decline();
    }
    const std::uint32_t kind_b = call.result();

    const auto object_b = static_cast<GuestAddress>(object_b_register);
    const PairPath path = pair_path(call.memory(), object_a, object_b, kind_a, kind_b);
    const GuestAddress cell = kPairTable + 4U * (kRowLength * path.row + path.column);
    call.return_value(call.memory().read_u32_le(cell));
    return call.finish(path.retired + run_a.retired + run_b.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001ba950, 220U, 0x5b094fd0f2920ca9ULL, "ObjectPairTableWord", kCeiling, object_pair_table_word,
         EngineReturn::Integer)
