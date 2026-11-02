// 0x001309c0 ResetUsageBlock: resets the subsystem object X (a0) by running its four teardown steps in order:
//   0x00275ec0 (empty stub) on X + 16
//   0x002bebb0 UpdateUsageHighWaterMarks on X + 1808
//   0x002f0330 ClearFlagByteIfUnset on X + 9712
//   0x0035ef30 StepDownCounter (no arguments)
// It makes no stores of its own and returns nothing. The 16 original instructions (prologue, epilogue, four
// jals with their delay slots) are always executed; the callees' counts are added.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00275ec0)
N3_CALLEE(002bebb0)
N3_CALLEE(002f0330)
N3_CALLEE(0035ef30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
constexpr std::uint32_t kOwnInstructions = 16U;
constexpr std::uint32_t kCeiling = 20000U;
constexpr GuestAddress kStubOffset = 16U;
constexpr GuestAddress kHighWaterOffset = 1808U;
constexpr GuestAddress kFlagOffset = 9712U;

template <class Context>
n3::Run reset_usage_block(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress block = call.arg(0);
    std::uint32_t retired = kOwnInstructions;

    call.set_arg(0, block + kStubOffset);
    const n3::Run stub = call.invoke([](auto& m) { return n3::fn_00275ec0(m); }, kFrameBytes);
    if (stub.declined) {
        return call.decline();
    }
    retired += stub.retired;

    call.set_arg(0, block + kHighWaterOffset);
    const n3::Run marks = call.invoke([](auto& m) { return n3::fn_002bebb0(m); }, kFrameBytes);
    if (marks.declined) {
        return call.decline();
    }
    retired += marks.retired;

    call.set_arg(0, block + kFlagOffset);
    const n3::Run flag = call.invoke([](auto& m) { return n3::fn_002f0330(m); }, kFrameBytes);
    if (flag.declined) {
        return call.decline();
    }
    retired += flag.retired;

    const n3::Run counter = call.invoke([](auto& m) { return n3::fn_0035ef30(m); }, kFrameBytes);
    if (counter.declined) {
        return call.decline();
    }
    retired += counter.retired;

    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001309c0, 64U, 0x25f5ffe7896b515bULL, "ResetUsageBlock", kCeiling, reset_usage_block,
         EngineReturn::Nothing)
