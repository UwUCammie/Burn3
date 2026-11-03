// 0x0010d630 (no arguments): InitModuleRpcChannel. It calls the syscall stub 0x0010c0a0 (a0 = 0), then 0x0010cf70
// (a0 = 528, a1 = the record at 0x004e3b90, a2 = 0x0010d250) and stores its result v0 in the record's first word.
// A negative result returns 0. Otherwise it clears record +4, +8 and +12, points +16 at the buffer 0x204e3bc0 and +20
// at 0x204e3d00, writes a header into the buffer (halfword 528 at +4, bytes 69 and 72 at +6 and +7, zeros at +2 and
// +8), calls 0x0010d1a8 (a0 = 256), stores its result at record +24 and returns 1.
// Cold-decline: the three callees never ran in the recorded game, and the call to 0x0010c0a0 sits in the first block,
// which every path runs. call_cost therefore returns 0 on every input and the original runs the whole function. The
// native cannot run those calls, so its implementation is never dispatched; it throws rather than return a partial
// result.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

// Retired instructions of the longest path (a non-negative handle): 17 in the first block, 23 in the second and 5 in
// the epilogue, delay slots included. There are no loops, so this is also the declared cost.
constexpr std::uint32_t kLongestPath = 45U;

// call_cost: 0 on every input, since every path reaches the cold call to 0x0010c0a0. It reads no guest memory.
std::uint32_t init_module_rpc_channel_cost(const NativeFunctionPrepareContext&) { return 0U; }

// Never dispatched, because call_cost declined every input. The cold call is the first instruction after the prologue,
// so no store of the original precedes it that the native could make: it refuses instead of returning a partial result.
NativeFunctionExecution init_module_rpc_channel(NativeFunctionContext&) {
    throw std::logic_error("InitModuleRpcChannel: the cold call 0x0010c0a0 cannot run natively");
}

} // namespace

extern constinit const EngineFunction n3_0010d630{0x0010d630U, 188U, 0xc15171f7e22976b7ULL, "InitModuleRpcChannel",
                                                  kLongestPath, init_module_rpc_channel, EngineReturn::Integer, {},
                                                  1e-4, 1e-6, {}, init_module_rpc_channel_cost};

} // namespace takedown::runtime
