// 0x0028b220 (a0 = object O, a1 = parameter record P): ApplyParamRecordToObject. Returns nothing.
//   f20 = (float)(int8)P[52] * K, where K is the float at $gp-27348. Call 0x0027a900 (a0 = O+11984, f12 = f20).
//   Store the byte O+29204 = the low byte of the truncating float-to-int conversion of 255.0 * f20. When that product
//   is at least 2^31, 2^31 is subtracted first and bit 31 is set (the low byte does not show it).
//   Call 0x003f8890 (a0 = O+21064, f12 = (float)(int8)P[53] * K), then 0x0028ad10 (a0 = O, a1 = (int8)P[51]).
//   Call 0x003f8870 (a0 = O+21064, a1 = 1 if (int8)P[57] != 0, else 0), then 0x003fbb50 (a0 = O+21064, a1 = 0,
//   a2 = sp+80), which fills a struct whose word at sp+84 is a count n. For i < n (signed): call 0x003fbaa0
//   (a0 = O+21064, a1 = i, a2 = sp+80, a3 = (uint8)P[i]).
// Cold-decline: every path makes the call to 0x0027a900 near its start, and the native never makes any of the six calls
// (0x0027a900, 0x003f8890, 0x0028ad10, 0x003f8870, 0x003fbb50, 0x003fbaa0). call_cost therefore returns 0 on every
// input and the original runs the whole function. The coverage record marks all six callees as never run in the
// recorded game, but the one recorded capture executes 1659 original instructions, far more than the function's own
// path (about 70, plus 11 per loop pass), so the callees did run inside it. The loop count is written by 0x003fbb50,
// so no dry run can bound it. No non-stack store precedes the first call, so there is no partial result to make: the
// native refuses.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

// Declared cost. The loop has no static bound (its count is written by 0x003fbb50), so this is the ceiling the other
// loop natives declare.
constexpr std::uint32_t kCeiling = 20000U;

// call_cost: 0 on every input, since every path reaches the cold call to 0x0027a900. It reads no guest memory.
std::uint32_t apply_param_record_to_object_cost(const NativeFunctionPrepareContext&) { return 0U; }

// Never dispatched, because call_cost declined every input. The first cold call comes before any non-stack store, so
// the native refuses instead of returning a partial result.
NativeFunctionExecution apply_param_record_to_object(NativeFunctionContext&) {
    throw std::logic_error("ApplyParamRecordToObject: the cold call 0x0027a900 cannot run natively");
}

} // namespace

extern constinit const EngineFunction n3_0028b220{0x0028b220U, 340U, 0x9c120e29e7c99302ULL,
                                                  "ApplyParamRecordToObject", kCeiling, apply_param_record_to_object,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  apply_param_record_to_object_cost};

} // namespace takedown::runtime
