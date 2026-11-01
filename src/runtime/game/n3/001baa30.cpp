// 0x001baa30 (a0 = object O): ClassifyObjectKind. The type t = (int8) O[0] picks one word of the jump table at
// 0x004b7cb0 (one word per type, 0..7); a type outside 0..7, negative ones included, returns 6 without a table read.
// The table words name five case bodies: 0 for types 0 and 2, 1 for type 1, 2 for type 3, 6 for types 5..7, and for
// type 4 the result is 3 when the signed byte at (the word at O+12) + 9259 equals the signed byte at 0x0064ed5c, else
// 5. Returns the category in v0. A leaf with no stores; call_cost declines a table word that names none of the bodies.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kKindByte = 0x0064ed5cU;     // lui 0x65, lb -4772: a signed byte
constexpr GuestAddress kJumpTable = 0x004b7cb0U;    // lui 0x4b, addiu 31920: one word per type
constexpr std::uint32_t kTypeCount = 8U;            // sltiu $at, $v0, 8
constexpr GuestAddress kPayloadPointer = 12U;       // lw $v0, 12($a0)
constexpr GuestAddress kPayloadByte = 9259U;        // lb $v0, 9259($v0)

// The case bodies the table words name (the address of each body's first instruction).
constexpr GuestAddress kCaseZero = 0x001baa64U;     // types 0 and 2: v0 = 0
constexpr GuestAddress kCaseOne = 0x001baa6cU;      // type 1: v0 = 1
constexpr GuestAddress kCaseTwo = 0x001baa74U;      // type 3: v0 = 2
constexpr GuestAddress kCaseCompare = 0x001baa7cU;  // type 4: v0 = 3 when the bytes are equal, else 5
constexpr GuestAddress kCaseSix = 0x001baaa0U;      // types 5, 6 and 7: v0 = 6

// Retired instruction counts from the listing, delay slots included. The prologue (lui, lb, lb, sltiu, beq and its
// nop) is 6 instructions and the table dispatch (lui, sll, addiu, addu, lw, jr and its nop) is 7.
constexpr std::uint32_t kOutOfRangeCost = 9U;       // prologue, addiu $v0 = 6, jr $ra and its nop
constexpr std::uint32_t kCaseCost = 17U;            // prologue, dispatch, a two-instruction body, jr $ra and its nop
constexpr std::uint32_t kEqualCost = 23U;           // the longest path: type 4 with equal bytes
constexpr std::uint32_t kDifferCost = 22U;          // type 4 with differing bytes (one instruction less in its body)

// The listing's path for one object: the category it returns, the instructions it retires and the temporaries it
// leaves behind. `covered` is false when the table word names none of the case bodies.
struct KindPath {
    std::uint32_t kind{};
    std::uint32_t retired{};
    std::uint32_t global{};  // $a1: the signed byte at 0x0064ed5c, sign-extended
    bool in_range{};         // $at: the type is 0..7
    bool covered{true};
};

// lb: the byte as a sign-extended 32-bit value.
std::uint32_t sign_extended_byte(const std::uint8_t byte) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(byte)));
}

KindPath classify_path(const auto& memory, const GuestAddress object) {
    KindPath path{};
    path.global = sign_extended_byte(memory.read_u8(kKindByte));
    // sltiu compares the sign-extended type as unsigned, so a negative type is out of range as well.
    const std::uint32_t type = sign_extended_byte(memory.read_u8(object));
    path.in_range = type < kTypeCount;
    if (!path.in_range) {
        path.kind = 6U;
        path.retired = kOutOfRangeCost;
        return path;
    }
    const GuestAddress target = memory.read_u32_le(kJumpTable + 4U * type);
    switch (target) {
    case kCaseZero:
        path.kind = 0U;
        path.retired = kCaseCost;
        break;
    case kCaseOne:
        path.kind = 1U;
        path.retired = kCaseCost;
        break;
    case kCaseTwo:
        path.kind = 2U;
        path.retired = kCaseCost;
        break;
    case kCaseSix:
        path.kind = 6U;
        path.retired = kCaseCost;
        break;
    case kCaseCompare: {
        // The bne compares the two sign-extended bytes, which are equal exactly when the bytes are.
        const GuestAddress payload = memory.read_u32_le(object + kPayloadPointer) + kPayloadByte;
        const bool equal = memory.read_u8(payload) == memory.read_u8(kKindByte);
        path.kind = equal ? 3U : 5U;
        path.retired = equal ? kEqualCost : kDifferCost;
        break;
    }
    default:
        path.covered = false;
        break;
    }
    return path;
}

// A table word that names no case body is declined: the original runs that call.
template <class Context>
std::uint32_t classify_object_kind_cost(const Context& context) {
    const KindPath path = classify_path(context.memory, context.cpu.read_gpr_u32(4U));
    return path.covered ? path.retired : 0U;
}

template <class Context>
n3::Run classify_object_kind(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const KindPath path = classify_path(call.memory(), call.arg(0));
    if (!path.covered) {
        throw std::logic_error("ClassifyObjectKind: jump-table target has no case body");
    }
    cpu.write_gpr_sign_extended_32(5U, path.global);     // lb $a1, -4772($v0)
    cpu.write_gpr_u64(1U, path.in_range ? 1U : 0U);      // sltiu $at, $v0, 8
    if (path.in_range) {
        cpu.write_gpr_sign_extended_32(3U, kJumpTable);  // lui/addiu $v1: the table address
    }
    call.return_value(path.kind);
    return call.finish(path.retired);
}

} // namespace

extern constinit const EngineFunction n3_001baa30{0x001baa30U, 132U, 0x25066a5e9275842bULL, "ClassifyObjectKind",
                                                  kEqualCost, n3::live<&classify_object_kind<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &classify_object_kind_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001baa30, classify_object_kind, classify_object_kind_cost)
