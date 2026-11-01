// 0x001ff798 (GetSystemSettingById): a0 = setting id, a1 = out pointer. A leaf switch on
// a0 in 1..5 that reads one global setting through $gp and stores it to *a1. The jump
// table at 0x004b8f40 (word n-1 for id n) sends ids 1..5 to 0x1ff7c0, 0x1ff7e4, 0x1ff810,
// 0x1ff818 and 0x1ff834. The switch below is fixed to those five entries, as read from the
// executable image. v0 is 1 on every path that stores, and 0 on the paths that store
// nothing: id 1 with its record flag at 32 or its gate clear, and an id out of range.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// $gp-relative displacements (signed 16-bit, as the listing encodes them).
constexpr std::int32_t kCaseOneRecordOffset = -28264;  // pointer to a record (id 1)
constexpr std::int32_t kCaseOneGateOffset = -24048;    // gate word (id 1)
constexpr std::int32_t kCaseOneValueOffset = -24044;   // stored word (id 1)
constexpr std::int32_t kCaseTwoOffset = -28168;        // 64-bit setting (id 2)
constexpr std::int32_t kCaseThreeOffset = -28208;      // stored word (id 3)
constexpr std::int32_t kCaseFourGateOffset = -24000;   // gate word (id 4)
constexpr std::int32_t kCaseFourValueOffset = -23996;  // float bits (id 4)
constexpr std::int32_t kCaseFiveOffset = -28136;       // stored word (id 5)

// The record's word at +8 that makes id 1 return 0 at once.
constexpr GuestAddress kRecordFlagOffset = 8U;
constexpr std::uint32_t kRecordFlagValue = 32U;

template <class Context>
n3::Run get_system_setting_by_id(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t id = call.arg(0U);
    const GuestAddress out = call.arg(1U);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    switch (id) {
    case 1U: {
        // 0x1ff7c0: a record whose +8 word is 32 returns 0 at once. Otherwise a zero gate
        // word returns 0, and any other gate stores the value word and returns 1.
        const GuestAddress record = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseOneRecordOffset));
        if (m.read_u32_le(record + kRecordFlagOffset) == kRecordFlagValue) {
            call.return_value(0U);
            return call.finish(17U);
        }
        if (m.read_u32_le(gp + static_cast<GuestAddress>(kCaseOneGateOffset)) == 0U) {
            call.return_value(0U);
            return call.finish(19U);
        }
        const std::uint32_t value = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseOneValueOffset));
        m.write_u32_le(out, value);
        call.return_value(1U);
        return call.finish(20U);
    }
    case 2U: {
        // 0x1ff7e4: a 64-bit load of the setting. The stored word is its low byte with bits 32..39
        // moved down to bits 24..31. The arithmetic shift in the listing only fills bits 56..63,
        // which the 0xff000000 mask drops, so a logical shift gives the same word. The sign
        // extension the listing then applies to v0 never reaches memory (sw keeps the low 32 bits).
        const std::uint64_t setting = m.read_u64_le(gp + static_cast<GuestAddress>(kCaseTwoOffset));
        const std::uint64_t word = (setting & 0xffU) | ((setting >> 8U) & 0xff000000U);
        m.write_u32_le(out, static_cast<std::uint32_t>(word));
        call.return_value(1U);
        return call.finish(23U);
    }
    case 3U: {
        // 0x1ff810: the setting word is stored as it is.
        const std::uint32_t value = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseThreeOffset));
        m.write_u32_le(out, value);
        call.return_value(1U);
        return call.finish(15U);
    }
    case 4U: {
        // 0x1ff818: f0 always takes the float bits (the lwc1 sits in a branch delay slot). The
        // stored word is those bits when the gate word is zero, and zero otherwise.
        const std::uint32_t gate = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseFourGateOffset));
        const std::uint32_t bits = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseFourValueOffset));
        call.cpu().write_fpr_bits(0U, bits);
        m.write_u32_le(out, gate == 0U ? bits : 0U);
        call.return_value(1U);
        return call.finish(17U);
    }
    case 5U: {
        // 0x1ff834: the setting word is stored as it is.
        const std::uint32_t value = m.read_u32_le(gp + static_cast<GuestAddress>(kCaseFiveOffset));
        m.write_u32_le(out, value);
        call.return_value(1U);
        return call.finish(14U);
    }
    default:
        // 0x1ff7a0: (id - 1) is not 0..4 as an unsigned value, so v0 = 0 and nothing is stored.
        call.return_value(0U);
        return call.finish(6U);
    }
}

} // namespace

extern constinit const EngineFunction n3_001ff798{0x001ff798U, 172U, 0xa6c3a806d86efe8fULL, "GetSystemSettingById", 23U,
                                                  n3::live<&get_system_setting_by_id<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001ff798, get_system_setting_by_id)
