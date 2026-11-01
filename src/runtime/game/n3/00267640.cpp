// 0x00267640 (a1 = selector): PickWeightedEntry. P is the word at 0x00527440, a config struct. The selector
// picks an entry array (v0), its count (v1) and a modulus (t1) from P; any other selector loads nothing and
// keeps the caller's v0, v1 and t1. Unless v1 is 1 (then v0 is returned as it stands), the RNG
// at 0x0055d788 (S) and 0x0055d78c (T) advances: n = (S << 16) + (S >> 16, arithmetic) + T, S = n, T = T + n,
// and the draw is n % modulus (a zero modulus gives n). Entries are 24 bytes with a weight word at +16; the first
// entry whose running weight sum is not below the draw (signed) is returned, or 0 when none is.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kConfigPointer = 0x00527440U;  // lui 0x52, lw 29760
constexpr GuestAddress kSeed = 0x0055d788U;           // lui 0x56, -10360: RNG word S
constexpr GuestAddress kSeedTail = 0x0055d78cU;       // lui 0x56, -10356: RNG word T
constexpr std::uint32_t kEntryStride = 24U;           // ((i << 1) + i) << 3
constexpr GuestAddress kWeightOffset = 16U;           // lw $a0, 16($a1)
constexpr unsigned kRegisterV0 = 2U;
constexpr unsigned kRegisterV1 = 3U;
constexpr unsigned kRegisterA1 = 5U;
constexpr unsigned kRegisterT1 = 9U;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Sign-extends a 32-bit word the way lw and the 32-bit ALU instructions fill a 64-bit register.
constexpr std::uint64_t word64(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

struct PickResult {
    std::uint32_t retired{};  // original instructions retired on this path, delay slots included
    std::uint64_t v0{};       // v0 as the original leaves it
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). The registers are
// taken whole: the original compares the selector, the count and the fall-through v1 as 64-bit values (beq,
// slt), so their upper halves decide some paths.
template <class Memory>
PickResult pick_weighted(Memory& m, const std::uint64_t selector, std::uint64_t v0, std::uint64_t v1,
                         std::uint64_t t1) {
    const GuestAddress config = m.read_u32_le(kConfigPointer);
    std::uint32_t retired = 0U;
    // The compare chain is 11, 4, 3, 5, 2, 1; the selectors are distinct, so the order only sets the retired count.
    // Selector 11 retires 9: the entry compare, its three loads and the shared addiu. Each later selector adds its
    // compare block (3) and has a load block of 4 that ends with the branch. The fall-through retires 5 + 15 + 2.
    if (selector == 11U) {
        retired = 9U;
        v0 = word64(m.read_u32_le(config + 144U));
        v1 = word64(m.read_u32_le(config + 148U));
        t1 = word64(m.read_u32_le(config + 152U));
    } else if (selector == 4U) {
        retired = 13U;
        v0 = word64(m.read_u32_le(config + 132U));
        v1 = word64(m.read_u32_le(config + 136U));
        t1 = word64(m.read_u32_le(config + 140U));
    } else if (selector == 3U) {
        retired = 16U;
        v0 = word64(m.read_u32_le(config + 120U));
        v1 = word64(m.read_u32_le(config + 124U));
        t1 = word64(m.read_u32_le(config + 128U));
    } else if (selector == 5U) {
        retired = 19U;
        v0 = word64(m.read_u32_le(config + 108U));
        v1 = word64(m.read_u32_le(config + 112U));
        t1 = word64(m.read_u32_le(config + 116U));
    } else if (selector == 2U) {
        retired = 22U;
        v0 = word64(m.read_u32_le(config + 96U));
        v1 = word64(m.read_u32_le(config + 100U));
        t1 = word64(m.read_u32_le(config + 104U));
    } else if (selector == 1U) {
        retired = 25U;
        v0 = word64(m.read_u32_le(config + 84U));
        v1 = word64(m.read_u32_le(config + 88U));
        t1 = word64(m.read_u32_le(config + 92U));
    } else {
        retired = 22U;  // every compare fails, then an unconditional branch past the loads
    }
    retired += 2U;  // beq $v1, 1 (64-bit) and its delay slot
    if (v1 == 1U) {
        return {retired + 2U, v0};  // jr $ra and its delay slot; the RNG is not touched
    }

    // RNG update (18 retired, through the branch into the loop test). The stores happen in this order.
    const std::uint32_t seed = m.read_u32_le(kSeed);
    const std::uint32_t tail = m.read_u32_le(kSeedTail);
    const std::uint32_t next_seed =
        (seed << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(seed) >> 16U) + tail;
    m.write_u32_le(kSeed, next_seed);
    m.write_u32_le(kSeedTail, tail + next_seed);
    const auto modulus = static_cast<std::uint32_t>(t1);
    const std::uint32_t draw = modulus != 0U ? next_seed % modulus : next_seed;
    retired += 18U;

    // Entry loop. The counter is a 32-bit value sign-extended for the signed 64-bit compare with the count.
    // The weight sum is a 32-bit value, compared signed with the draw.
    const auto base = static_cast<std::uint32_t>(v0);
    std::uint32_t sum = 0U;
    for (std::uint32_t index = 0U;; ++index) {
        retired += 3U;  // slt, bne and its delay slot
        if (!(static_cast<std::int64_t>(static_cast<std::int32_t>(index)) < static_cast<std::int64_t>(v1))) {
            return {retired + 4U, 0U};  // beq $zero (v0 = 0 in its delay slot), then jr $ra and its delay slot
        }
        if (retired > kCeiling) {
            return {retired, 0U};  // above the ceiling: call_cost declines this call, the original runs it
        }
        const std::uint32_t entry = base + kEntryStride * index;
        sum += m.read_u32_le(entry + kWeightOffset);
        retired += 9U;  // sll, addu, sll, addu, lw, addu, slt, beq and its delay slot
        if (!(static_cast<std::int32_t>(sum) < static_cast<std::int32_t>(draw))) {
            return {retired + 3U, word64(entry)};  // daddu v0 = entry, then jr $ra and its delay slot
        }
        retired += 1U;  // addiu $t0, 1
    }
}

template <class Context>
n3::Run pick_weighted_entry(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t selector = call.cpu().read_gpr_u64(kRegisterA1);
    const std::uint64_t v0 = call.cpu().read_gpr_u64(kRegisterV0);
    const std::uint64_t v1 = call.cpu().read_gpr_u64(kRegisterV1);
    const std::uint64_t t1 = call.cpu().read_gpr_u64(kRegisterT1);
    const PickResult result = pick_weighted(call.memory(), selector, v0, v1, t1);
    call.return_value64(result.v0);
    return call.finish(result.retired);
}

template <class Context>
std::uint32_t pick_weighted_entry_cost(const Context& context) {
    const std::uint64_t selector = context.cpu.read_gpr_u64(kRegisterA1);
    const std::uint64_t v0 = context.cpu.read_gpr_u64(kRegisterV0);
    const std::uint64_t v1 = context.cpu.read_gpr_u64(kRegisterV1);
    const std::uint64_t t1 = context.cpu.read_gpr_u64(kRegisterT1);
    n3::DryMemory dry{context.memory};
    const PickResult result = pick_weighted(dry, selector, v0, v1, t1);
    return result.retired <= kCeiling ? result.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_00267640{0x00267640U, 336U, 0x49ca176da5fa7f1dULL, "PickWeightedEntry",
                                                  kCeiling, n3::live<&pick_weighted_entry<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &pick_weighted_entry_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00267640, pick_weighted_entry, pick_weighted_entry_cost)
