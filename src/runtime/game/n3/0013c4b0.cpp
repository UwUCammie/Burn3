// 0x0013c4b0 (a0 = record): zeroes the 354 bytes at a0+0 .. a0+353 with byte stores, as eleven
// runs (offset, length): (0,67) (67,20) (87,73) (160,100) (260,36) (296,10) (306,10) (316,20)
// (336,10) (346,5) (351,3). Each run is a counter from 0 that stops at a constant bound, so no
// path depends on the data: every call retires 2491 instructions (one counter reset per run, 7 per
// iteration over 354 iterations, and jr with its addiu delay slot). Returns v0 = 1.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

struct ZeroRun {
    std::uint32_t offset;
    std::uint32_t length;
};

constexpr ZeroRun kRuns[] = {
    {0U, 67U},   {67U, 20U},  {87U, 73U},  {160U, 100U}, {260U, 36U}, {296U, 10U},
    {306U, 10U}, {316U, 20U}, {336U, 10U}, {346U, 5U},   {351U, 3U},
};

// Instructions retired on every path: 11 counter resets + 7 * 354 loop instructions + jr and its delay slot.
constexpr std::uint32_t kRetired = 2491U;

template <class Context>
n3::Run clear_record_bytes(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    for (const ZeroRun& run : kRuns) {
        for (std::uint32_t i = 0U; i < run.length; ++i) {
            m.write_u8(record + run.offset + i, std::uint8_t{0U});
        }
    }
    // The original leaves its last loop counter in v1 (3); v0 is the return value (1).
    call.cpu().write_gpr_sign_extended_32(3U, 3U);
    call.return_value(1U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0013c4b0{0x0013c4b0U, 360U, 0xd68527159eb7ddd9ULL, "ClearRecordBytes354",
                                                  kRetired, n3::live<&clear_record_bytes<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0013c4b0, clear_record_bytes)
