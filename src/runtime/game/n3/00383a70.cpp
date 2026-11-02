// 0x00383a70 (a0 = table T, a1 = pointer to a packed 64-bit name, a2 = pick a random variant when non-zero):
// LookupNameVariant. T holds 16-byte entries sorted by their packed 64-bit name (at +0) and a signed count byte at
// T + 937. A name's search key is ExtractNameChars(name, 0, 9) (0x0021ad60): the name without its last two characters,
// so the variants of one name share a key. A binary search (hi = count - 1, lo = 0, mid = (hi + lo) >> 1) finds an
// entry with the same key; v0 is that entry's address, or 0 when the name is 0, the table is empty or nothing matches.
// With a2 set, the entry is replaced by one of its variants: UnpackName (0x0021ab40) writes the name's characters at
// sp+136, ParseBase36Digit (0x00386f40) parses the last two (sp+146 into d at sp+158, sp+147 into m at sp+159), and the
// result is the entry at index (mid - (d - 1)) + x, x = (m + a) % m (x = m + a when m = 0: the R5900 divu leaves HI =
// rs). a comes from the generator words at $gp-22520 (g0) and $gp-22516 (g1), which are updated:
// a = (g0 << 16) + (g0 >> 16, arithmetic) + g1, g0 = a, g1 = g1 + a. A character that does not parse keeps the entry.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0021ad60)
N3_CALLEE(0021ab40)
N3_CALLEE(00386f40)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 160U;       // addiu $sp, $sp, -160
constexpr std::uint32_t kKeptLast = 9U;           // ExtractNameChars(name, 0, 9): positions 0..9 are kept
constexpr GuestAddress kCountOffset = 937U;       // signed count byte of the table
constexpr GuestAddress kNameBuffer = 136U;        // UnpackName's buffer: 12 characters, then a NUL
constexpr GuestAddress kCharBeforeLast = 146U;    // buffer[10]
constexpr GuestAddress kLastChar = 147U;          // buffer[11]
constexpr GuestAddress kIndexDigit = 158U;        // d: the digit of buffer[10]
constexpr GuestAddress kCountDigit = 159U;        // m: the digit of buffer[11]
constexpr std::int32_t kState0Offset = -22520;    // $gp-relative generator word g0
constexpr std::int32_t kState1Offset = -22516;    // $gp-relative generator word g1
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included. Each jal adds its callee's count.
constexpr std::uint32_t kEntryCost = 20U;         // 0x00383a70-0x00383abc: frame, saves, the first call, the name test
constexpr std::uint32_t kNameZeroCost = 2U;       // 0x00383b28-0x00383b2c: the name is 0, v0 = 0
constexpr std::uint32_t kCountTestCost = 3U;      // 0x00383ac0-0x00383ac8: the count byte and its test
constexpr std::uint32_t kEmptyCost = 2U;          // 0x00383b30-0x00383b34: the count is 0, v0 = 0
constexpr std::uint32_t kSearchSetupCost = 3U;    // 0x00383acc-0x00383ad4: hi = count - 1, lo = 0
constexpr std::uint32_t kTestCost = 3U;           // 0x00383b14-0x00383b1c: the loop test, hi < lo
constexpr std::uint32_t kExitCost = 2U;           // 0x00383b20-0x00383b24: not found, v0 = 0
constexpr std::uint32_t kProbeCost = 11U;         // 0x00383ad8-0x00383b00: mid, the entry's address, the call
constexpr std::uint32_t kHighCost = 2U;           // 0x00383b38-0x00383b3c: target < key, hi = mid - 1
constexpr std::uint32_t kCompareCost = 3U;        // 0x00383b04-0x00383b0c: key < target, or equal
constexpr std::uint32_t kLowCost = 1U;            // 0x00383b10: key < target, lo = mid + 1
constexpr std::uint32_t kFoundCost = 2U;          // 0x00383b40-0x00383b44: the flag test
constexpr std::uint32_t kSlotCost = 3U;           // 0x00383b48-0x00383b50: v0 = an entry's address (sll, beq, addu)
constexpr std::uint32_t kPickStartCost = 9U;      // 0x00383b54-0x00383b74: UnpackName, the first digit and its test
constexpr std::uint32_t kPickDigitCost = 5U;      // 0x00383b78-0x00383b88: the second digit and its test
constexpr std::uint32_t kPickCost = 18U;          // 0x00383b8c-0x00383bd0: the random word, the remainder, the index
constexpr std::uint32_t kEpilogueCost = 10U;      // 0x00383bd4-0x00383bf8: the restores and jr

// lb: the byte as a signed 32-bit value (set_arg sign-extends it, as the register holds it).
constexpr std::uint32_t signed_byte(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

// Every exit: v0 is the result and the epilogue is part of the count.
template <class CallType>
n3::Run exit_with(CallType& call, const std::uint32_t retired, const GuestAddress result) {
    call.return_value(result);
    return call.finish(retired + kEpilogueCost);
}

template <class Context>
n3::Run lookup_name_variant(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress table = call.arg(0U);
    const GuestAddress name_address = call.arg(1U);
    const bool pick_variant = call.arg64(2U) != 0U;
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // The key of the name. The original reads the name a second time for its zero test, and so does this.
    call.set_arg64(0U, m.read_u64_le(name_address));
    call.set_arg(1U, 0U);
    call.set_arg(2U, kKeptLast);
    const n3::Run name_key = call.invoke([](auto& machine) { return n3::fn_0021ad60(machine); }, kFrameBytes);
    if (name_key.declined) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost + name_key.retired;
    const std::uint64_t target = call.result64();
    if (m.read_u64_le(name_address) == 0U) {
        retired += kNameZeroCost;
        return exit_with(call, retired, 0U);
    }
    retired += kCountTestCost;
    const auto count = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(table + kCountOffset)));
    if (count == 0) {
        retired += kEmptyCost;
        return exit_with(call, retired, 0U);
    }
    retired += kSearchSetupCost;

    // Binary search over the keys. The range halves at every probe, so the loop runs at most eight times.
    std::int32_t lo = 0;
    std::int32_t hi = count - 1;
    std::int32_t mid = 0;
    for (;;) {
        retired += kTestCost;
        if (hi < lo) {
            retired += kExitCost;
            return exit_with(call, retired, 0U);
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        mid = (hi + lo) >> 1;
        call.set_arg64(0U, m.read_u64_le(table + (static_cast<GuestAddress>(mid) << 4U)));
        call.set_arg(1U, 0U);
        call.set_arg(2U, kKeptLast);
        const n3::Run probe = call.invoke([](auto& machine) { return n3::fn_0021ad60(machine); }, kFrameBytes);
        if (probe.declined) {
            return call.decline();
        }
        retired += kProbeCost + probe.retired;
        const std::uint64_t key = call.result64();
        if (target < key) {
            retired += kHighCost;
            hi = mid - 1;
        } else {
            retired += kCompareCost;
            if (!(key < target)) {
                break;
            }
            retired += kLowCost;
            lo = mid + 1;
        }
    }
    const GuestAddress found = table + (static_cast<GuestAddress>(mid) << 4U);
    retired += kFoundCost;
    if (!pick_variant) {
        retired += kSlotCost;
        return exit_with(call, retired, found);
    }

    // Variant pick: unpack the found entry's name, then parse its last two characters into digits.
    retired += kPickStartCost;
    call.set_arg64(0U, m.read_u64_le(found));
    call.set_arg(1U, frame + kNameBuffer);
    const n3::Run unpack = call.invoke([](auto& machine) { return n3::fn_0021ab40(machine); }, kFrameBytes);
    if (unpack.declined) {
        return call.decline();
    }
    retired += unpack.retired;
    call.set_arg(0U, signed_byte(m.read_u8(frame + kLastChar)));
    call.set_arg(1U, frame + kCountDigit);
    const n3::Run last_digit = call.invoke([](auto& machine) { return n3::fn_00386f40(machine); }, kFrameBytes);
    if (last_digit.declined) {
        return call.decline();
    }
    retired += last_digit.retired;
    if (call.result64() == 0U) {
        retired += kSlotCost;
        return exit_with(call, retired, found);
    }
    retired += kPickDigitCost;
    call.set_arg(0U, signed_byte(m.read_u8(frame + kCharBeforeLast)));
    call.set_arg(1U, frame + kIndexDigit);
    const n3::Run before_last = call.invoke([](auto& machine) { return n3::fn_00386f40(machine); }, kFrameBytes);
    if (before_last.declined) {
        return call.decline();
    }
    retired += before_last.retired;
    if (call.result64() == 0U) {
        retired += kSlotCost;
        return exit_with(call, retired, found);
    }
    retired += kPickCost;

    // The generator: a = (g0 << 16) + (g0 >> 16, arithmetic) + g1, then g0 = a and g1 = g1 + a (both stored).
    // The modulus is divu's: when m is 0 the remainder is the dividend itself.
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress state0_address = gp + static_cast<GuestAddress>(kState0Offset);
    const GuestAddress state1_address = gp + static_cast<GuestAddress>(kState1Offset);
    const std::uint32_t index_digit = m.read_u8(frame + kIndexDigit);
    const std::uint32_t count_digit = m.read_u8(frame + kCountDigit);
    const std::uint32_t state0 = m.read_u32_le(state0_address);
    const std::uint32_t state1 = m.read_u32_le(state1_address);
    const std::uint32_t random = (state0 << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(state0) >> 16) + state1;
    m.write_u32_le(state0_address, random);
    m.write_u32_le(state1_address, state1 + random);
    const std::uint32_t sum = count_digit + random;
    const std::uint32_t remainder = count_digit != 0U ? sum % count_digit : sum;
    const std::uint32_t index = (static_cast<std::uint32_t>(mid) - (index_digit - 1U)) + remainder;
    retired += kSlotCost;
    return exit_with(call, retired, table + (index << 4U));
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00383a70, 396U, 0xb7e9b15f97b92c5aULL, "LookupNameVariant", kCeiling, lookup_name_variant,
         EngineReturn::Integer)
