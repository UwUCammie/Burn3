// 0x001c6f30 (a0 = gearbox G, f12 = wheel radius, f13 = wheel rate): PickGear, the gear for a car moving at f13 on
// wheels of radius f12. wheel = f13 / f12 and limit = (the float at $gp-30960) * (the redline float at G+40).
// Gears 1, 2, ... are tried while gear < count (the signed word at G+132): gear g's rate is (the drive float at
// G+32) * (wheel * ratio), with ratio the float at G+4+4g, and the first gear whose rate is at or below the limit is
// taken. v0 is that gear, or the count when none fits. G+84 gets the last rate tested; with a count of one or less
// nothing is tested and G+84 gets f3 as the caller left it.
//
//   001c6f30-001c6f4c  entry, 8 retired: wheel, count, the rpm and redline floats, limit (the mul.s is the beq's delay slot)
//   001c6f78-001c6f80  test, 3 each time: slt, bne and its delay slot
//   001c6f50-001c6f70  one gear, 9: the rate, then c.ole.s and bc1t (its delay slot always runs)
//   001c6f74           addiu, 1: when the gear did not fit
//   001c6f84-001c6f88  return, 2: jr $ra and the swc1 in its delay slot
//
// A count of one or less retires 13. A search that stops at gear m retires 9 + 13m. A search that finds no gear
// retires 13 * count.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kRpmOffset = -30960;   // lwc1 $f1, -30960($gp): the rpm-to-radians float
constexpr GuestAddress kRedline = 40U;        // lwc1 $f0, 40($a0)
constexpr GuestAddress kGearCount = 132U;     // lw $a1, 132($a0)
constexpr GuestAddress kRatios = 4U;          // gear g's ratio at G + 4 + 4g (lwc1 4($v1) with v1 = G + 4g)
constexpr GuestAddress kFinalDrive = 32U;     // lwc1 $f0, 32($a0), inside each test
constexpr GuestAddress kEngineRate = 84U;     // swc1 $f3, 84($a0)
constexpr std::uint32_t kFirstGear = 1U;      // addiu $v0, $zero, 1

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;      // 001c6f30-001c6f4c
constexpr std::uint32_t kTestCost = 3U;       // 001c6f78-001c6f80
constexpr std::uint32_t kGearCost = 9U;       // 001c6f50-001c6f70
constexpr std::uint32_t kStepCost = 1U;       // 001c6f74
constexpr std::uint32_t kReturnCost = 2U;     // 001c6f84-001c6f88

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). It returns early once the
// retired count passes kCeiling, so a call with a huge count cannot hang the dry run.
template <class Context>
n3::Run pick_gear(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gearbox = call.arg(0);
    const std::uint32_t wheel = fpu::div(call.cpu().read_fpr_bits(13U), call.cpu().read_fpr_bits(12U));
    const std::uint32_t count = m.read_u32_le(gearbox + kGearCount);
    const GuestAddress rpm_address =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kRpmOffset);
    const std::uint32_t rpm = m.read_u32_le(rpm_address);
    const std::uint32_t redline = m.read_u32_le(gearbox + kRedline);
    const std::uint32_t limit = fpu::mul(rpm, redline);
    std::uint32_t gear = kFirstGear;
    std::uint32_t rate = call.cpu().read_fpr_bits(3U);
    std::uint32_t retired = kEntryCost;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(gear) >= static_cast<std::int32_t>(count)) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t ratio = m.read_u32_le(gearbox + (gear << 2U) + kRatios);
        const std::uint32_t drive = m.read_u32_le(gearbox + kFinalDrive);
        rate = fpu::mul(drive, fpu::mul(wheel, ratio));
        retired += kGearCost;
        if (fpu::le(rate, limit)) {
            break;
        }
        retired += kStepCost;
        gear += 1U;
    }
    m.write_u32_le(gearbox + kEngineRate, rate);
    call.return_value(gear);
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001c6f30, 92U, 0x8c29677e9519a462ULL, "PickGear", kCeiling, pick_gear, EngineReturn::Integer)
