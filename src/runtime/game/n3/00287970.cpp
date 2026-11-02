// 0x00287970 SampleListenerResponse: writes two response floats (a2, a3 point at them) for the source record a1,
// relative to the global listener object. The listener (first word of the global handle at 0x01e75688) has a
// flag byte at +12: when it is 0 both responses are 1.0 and the call is done. Otherwise the original measures
// direction and distance with VU0 and calls SampleResponseAndFalloff (0x002f4ea0) through callees that have no
// n3 file (0x003845d0, 0x0012ddf8, 0x0011d630, 0x0012ea30), so that path is left to the original (decline).
//
// Early-out path retired: 18 (prologue up to the delay slot of the beq/bne, incl. the jal and its delay slot)
// + 2 (LoadWordAtArg0) + 4 (0x002879b8..0x002879c4) + 9 (epilogue at 0x00287b50) = 33.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00384a50)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kListenerHandle = 0x01e75688U;
constexpr GuestAddress kListenerActiveFlag = 12U;
constexpr std::uint32_t kFrameBytes = 160U;
constexpr std::uint32_t kFullResponseBits = 0x3f800000U;  // 1.0f
constexpr std::uint32_t kEarlyOutRetired = 18U + 4U + 9U; // plus the listener getter's own count

template <class Context>
n3::Run sample_listener_response(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out_first = call.arg(2);
    const GuestAddress out_second = call.arg(3);

    call.set_arg(0, kListenerHandle);
    const n3::Run getter = call.invoke([](auto& mem) { return n3::fn_00384a50(mem); }, kFrameBytes);
    if (getter.declined) {
        return call.decline();
    }
    const GuestAddress listener = call.result();
    if (m.read_u8(listener + kListenerActiveFlag) != 0U) {
        return call.decline();
    }
    m.write_u32_le(out_first, kFullResponseBits);
    m.write_u32_le(out_second, kFullResponseBits);
    return call.finish(kEarlyOutRetired + getter.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00287970, 516U, 0x7f76f52e540dc518ULL, "SampleListenerResponse", 33U, sample_listener_response,
         EngineReturn::Nothing)
