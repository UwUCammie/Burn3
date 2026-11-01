// 0x00269ec0 (a0 = three-word vector record, a1 = integer offset): AddOffsetToVec3Words. Each of the words at
// a0+0, a0+4 and a0+8 gets a1 added (32-bit wrapping addu, no float math) and is stored back. The order is word 0,
// then word 4, then word 8, each read-modify-written before the next is touched. The last sw is in the delay slot
// of the jr, so all ten instructions retire on the only path. Returns nothing. The original leaves the last sum in
// v1; v1 is a temporary, so the native does not write it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run add_offset_to_vec3_words(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress vec = call.arg(0U);
    const std::uint32_t offset = call.arg(1U);
    // lw, addu, sw on word 0; then word 4; then word 8.
    const std::uint32_t x0 = m.read_u32_le(vec) + offset;
    m.write_u32_le(vec, x0);
    const std::uint32_t x1 = m.read_u32_le(vec + 4U) + offset;
    m.write_u32_le(vec + 4U, x1);
    const std::uint32_t x2 = m.read_u32_le(vec + 8U) + offset;
    m.write_u32_le(vec + 8U, x2);
    return call.finish(10U);
}

} // namespace

extern constinit const EngineFunction n3_00269ec0{0x00269ec0U, 40U, 0x114a1dbf632f1c84ULL, "AddOffsetToVec3Words",
                                                  10U, n3::live<&add_offset_to_vec3_words<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00269ec0, add_offset_to_vec3_words)
