// 0x0030a130 (a0 = object O, a1 = mode, a2 = value the original keeps in s0): ResolveModeStatus.
//
//   O+80 == 5 (0x0030a14c taken): O+80 = 14, O+0 = 0, returns 0. 19 retired.
//   otherwise a1 == 1 (0x00242e28) or a1 == 0 (0x00242e00): both callees have no native, so the whole call is
//     declined before anything is written.
//   otherwise (a1 is neither 0 nor 1): O+76 = 0, O+80 = 14, O+88 = 13, O+208 = 0, O+216 = -1, O+212 = 1,
//     O+0 = 13, returns 13. 32 retired.
//
// Everything after those two calls (0x0030a1c8 on) is only reached through them, so it never runs natively. The
// original's call to 0x003096e0 is therefore unreachable here. a2 only reaches that call (copied to s0, passed on as
// a3), so this file does not read it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Words of the object O that the routine writes (offsets from O).
constexpr GuestAddress kStatus = 0U;
constexpr GuestAddress kClearedFlag = 76U;
constexpr GuestAddress kPhase = 80U;
constexpr GuestAddress kLabel = 88U;
constexpr GuestAddress kRecordWord = 208U;
constexpr GuestAddress kRecordIndex = 212U;
constexpr GuestAddress kRecordLimit = 216U;

constexpr std::uint32_t kPhaseFinished = 5U;
constexpr std::uint32_t kPhaseDone = 14U;
constexpr std::uint32_t kStatusFinished = 0U;
constexpr std::uint32_t kStatusOther = 13U;
constexpr std::uint32_t kLabelOther = 13U;
constexpr std::uint32_t kRecordIndexOther = 1U;
constexpr std::uint32_t kRecordLimitOther = 0xffffffffU;

// Retired instruction counts of the two call-free paths, delay slots included.
constexpr std::uint32_t kFinishedRetired = 19U;
constexpr std::uint32_t kOtherRetired = 32U;

// Calls retiring more than this are left to the original (call_cost returns 0).
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run resolve_mode_status(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t mode = call.arg(1);

    if (m.read_u32_le(object + kPhase) == kPhaseFinished) {
        m.write_u32_le(object + kPhase, kPhaseDone);
        m.write_u32_le(object + kStatus, kStatusFinished);
        call.return_value(kStatusFinished);
        return call.finish(kFinishedRetired);
    }
    if (mode == 0U || mode == 1U) {
        // Modes 0 and 1 call 0x00242e00 / 0x00242e28, which have no native: the original runs the whole call.
        return call.decline();
    }
    m.write_u32_le(object + kClearedFlag, 0U);
    m.write_u32_le(object + kPhase, kPhaseDone);
    m.write_u32_le(object + kLabel, kLabelOther);
    m.write_u32_le(object + kRecordWord, 0U);
    m.write_u32_le(object + kRecordLimit, kRecordLimitOther);
    m.write_u32_le(object + kRecordIndex, kRecordIndexOther);
    m.write_u32_le(object + kStatus, kStatusOther);
    call.return_value(kStatusOther);
    return call.finish(kOtherRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0030a130, 472U, 0xb417b9369697dc7cULL, "ResolveModeStatus", kCeiling, resolve_mode_status, EngineReturn::Integer)
