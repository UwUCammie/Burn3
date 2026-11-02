// 0x00141890 (a0 = AI controller C): TickAvoidanceUpdate. The byte at C+1199 counts the calls and wraps at 256. Below
// 3 nothing else happens. At 3 the byte is cleared, GetLinkedRecordByte (0x001419b0) reads the byte of the car object
// at C+1188, BuildAvoidanceProfiles (0x00140850) rebuilds the profiles of C from it and ChooseLaneTarget (0x00140a30)
// chooses the lane target of C. Returns nothing.
//
// Listing: 0x00141890..0x001418b4 are the prologue, the count and its test (10 retired, the bne's delay slot
// included). 0x001418b8..0x001418d4 are the clear and the three calls (8 retired, each jal and its delay slot
// included). 0x001418d8..0x001418e4 are the epilogue (4 retired). A call below the third tick retires 14; a call that
// reaches it retires 22 plus the three callees. The prologue's stores of ra and s0 sit below the entry $sp
// and are not reproduced; the epilogue's loads of them are not needed, since this function never writes s0 or ra.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001419b0)
N3_CALLEE(00140850)
N3_CALLEE(00140a30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTickCount = 1199U;      // u8 at C+1199: calls since the last rebuild
constexpr GuestAddress kCarObject = 1188U;      // u32 at C+1188: the car object GetLinkedRecordByte reads
constexpr std::uint8_t kRebuildTick = 3U;       // slti 3: the tick (after the increment) that rebuilds
constexpr std::uint32_t kFrameBytes = 48U;      // the caller's frame: every jal here lowers $sp by it
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the straight-line blocks, delay slots included (addresses from the listing).
constexpr std::uint32_t kTickCost = 10U;        // 0x00141890..0x001418b4: prologue, count, test, bne and its delay slot
constexpr std::uint32_t kRebuildCost = 8U;      // 0x001418b8..0x001418d4: sb, daddu $a1, the three jal with their delay slots
constexpr std::uint32_t kReturnCost = 4U;       // 0x001418d8..0x001418e4: epilogue and the jr's delay slot
constexpr std::uint32_t kEarlyCost = kTickCost + kReturnCost;

template <class Context>
n3::Run tick_avoidance_update(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    // The original copies all of $a0 into $s0 and back into $a0 for the second and third calls (daddu), so keep the
    // 64-bit value.
    const std::uint64_t controller = call.arg64(0);
    const GuestAddress tick_address = object + kTickCount;
    const auto tick = static_cast<std::uint8_t>(m.read_u8(tick_address) + 1U);
    m.write_u8(tick_address, tick);
    if (tick < kRebuildTick) {
        return call.finish(kEarlyCost);
    }
    m.write_u8(tick_address, 0U);

    // lw $a0, 1188($s0) in the delay slot of the first jal: a sign-extended 32-bit load.
    call.set_arg(0, m.read_u32_le(object + kCarObject));
    const n3::Run racer = call.invoke([](auto& machine) { return n3::fn_001419b0(machine); }, kFrameBytes);
    if (racer.declined) {
        return call.decline();
    }
    // daddu $a1, $v0, $zero, then daddu $a0, $s0, $zero in the delay slot of the second jal.
    call.set_arg64(1, call.result64());
    call.set_arg64(0, controller);
    const n3::Run profiles = call.invoke([](auto& machine) { return n3::fn_00140850(machine); }, kFrameBytes);
    if (profiles.declined) {
        return call.decline();
    }
    call.set_arg64(0, controller);
    const n3::Run target = call.invoke([](auto& machine) { return n3::fn_00140a30(machine); }, kFrameBytes);
    if (target.declined) {
        return call.decline();
    }
    return call.finish(kTickCost + kRebuildCost + kReturnCost + racer.retired + profiles.retired + target.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00141890, 88U, 0x0ffd1f106664a57eULL, "TickAvoidanceUpdate", kCeiling, tick_avoidance_update,
         EngineReturn::Nothing)
