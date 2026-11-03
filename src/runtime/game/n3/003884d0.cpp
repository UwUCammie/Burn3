// 0x003884d0 (a0 = fade record, a1 = parameter block): ApplyFadeRecordParams, null guard only.
// Its callers fill a0 from the 128-entry record pool at 0x01e7569c (0x00388d40 / 0x00388f20), which returns 0 when
// the pool is empty, and some callers (0x0028b89c / 0x0028b8a8) pass that 0 on without checking it. The original then
// reads the word at guest address 4 as the record's object and loads a byte through it (0x00388928), which faults
// unless that word happens to be a mapped address: a Crash event crashed this way after the pool ran out.
// With a0 == 0 this returns at once: nothing is applied, as if the effect had no record. Every other call is handed
// to the original code by call_cost (one register read, no dry run), so it runs exactly as before.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// The prologue up to the first load through a0 (0x003884d0..0x00388500), plus jr $ra and its delay slot.
constexpr std::uint32_t kNullRecordCost = 15U;

template <class Context>
std::uint32_t null_record_cost(const Context& context) {
    return context.cpu.read_gpr_u32(4U) == 0U ? kNullRecordCost : 0U;
}

template <class Context>
n3::Run apply_fade_record_params_null_guard(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(kNullRecordCost);
}

} // namespace

extern constinit const EngineFunction n3_003884d0{
    0x003884d0U, 1332U, 0xf9ce90f6ccacd9c8ULL, "ApplyFadeRecordParamsNullGuard", kNullRecordCost,
    n3::live<&apply_fade_record_params_null_guard<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
    &null_record_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003884d0, apply_fade_record_params_null_guard, null_record_cost)
