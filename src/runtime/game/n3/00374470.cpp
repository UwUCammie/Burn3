// 0x00374470 (a0 = buffer-cursor record, a1 = byte delta): the cursor at +8 becomes
// the +4 word plus the delta, and the high-water mark at +12 is raised to the cursor
// when it is below it (unsigned compare). Leaf with no result: v0/v1 are not written
// (the original leaves v1 holding the old peak, but its caller at 0x00155020 writes
// v1 again before reading it).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run advance_cursor_track_peak(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t delta = call.arg(1);
    // lw $v1, 4($a0) ; addu $v1, $v1, $a1 ; sw $v1, 8($a0)
    const std::uint32_t cursor_end = m.read_u32_le(record + 4U) + delta;
    m.write_u32_le(record + 8U, cursor_end);
    // lw $a1, 8($a0) ; lw $v1, 12($a0) ; sltu $at, $v1, $a1 ; bne $at, $zero
    const std::uint32_t cursor = m.read_u32_le(record + 8U);
    const std::uint32_t peak = m.read_u32_le(record + 12U);
    if (peak < cursor) {
        // The store at 0x0037449c runs only on this path (delay slot of the branch at 0x00374498): 14 retired.
        m.write_u32_le(record + 12U, cursor);
        return call.finish(14U);
    }
    // Peak not raised: 12 instructions retired.
    return call.finish(12U);
}

} // namespace

extern constinit const EngineFunction n3_00374470{0x00374470U, 56U, 0xce9e2fa66c4ba2a1ULL, "AdvanceCursorTrackPeak", 14U,
                                           n3::live<&advance_cursor_track_peak<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00374470, advance_cursor_track_peak)
