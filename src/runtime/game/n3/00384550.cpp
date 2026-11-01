// 0x00384550 (a0 = object, a1 = destination vector): LoadOwnerPositionToVec4.
// The object's word at +8 is a pointer P to another object whose floats at P+84, P+88 and P+92 are a position
// (x, y, z). The original builds the vector on its own frame (sp - 32): x, y, z go to frame+0/+4/+8, the 16 bytes
// at frame are loaded with lq, and sq stores that quad at a1 (sq ignores the low 4 bits of a1). Lane w is the word
// at frame+12, which this function never writes, so it is whatever the frame held before the call. The same three
// words are written again at frame+16/+20/+24 after the lq. Only raw words move: no float arithmetic.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;       // addiu $sp,$sp,-32 (undone by the delay slot before jr)
constexpr GuestAddress kObjectPointer = 8U;      // lw $v1, 8($a0)
constexpr GuestAddress kPositionX = 84U;         // lwc1 $f2, 84($v1)
constexpr GuestAddress kPositionY = 88U;         // lwc1 $f1, 88($v1)
constexpr GuestAddress kPositionZ = 92U;         // lwc1 $f0, 92($v1)

template <class Context>
n3::Run load_owner_position_to_vec4(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress destination = call.arg(1);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    const GuestAddress position = m.read_u32_le(object + kObjectPointer);
    const std::uint32_t x = m.read_u32_le(position + kPositionX);
    const std::uint32_t y = m.read_u32_le(position + kPositionY);
    const std::uint32_t z = m.read_u32_le(position + kPositionZ);

    // Frame words as the original writes them: lane w (frame+12) is not stored here, so lq reads its old value.
    m.write_u32_le(frame, x);
    m.write_u32_le(frame + 4U, y);
    m.write_u32_le(frame + 8U, z);
    const n3::Gpr128 quad = n3::load_quad_any(m, frame);
    m.write_u32_le(frame + 16U, x);
    m.write_u32_le(frame + 20U, y);
    m.write_u32_le(frame + 24U, z);
    n3::store_quad_any(m, destination, quad);

    cpu.write_gpr(3U, quad);
    cpu.write_fpr_bits(2U, x);
    cpu.write_fpr_bits(1U, y);
    cpu.write_fpr_bits(0U, z);
    return call.finish(15U);
}

} // namespace

extern constinit const EngineFunction n3_00384550{0x00384550U, 60U, 0xe458ae2eb5fb50bcULL, "LoadOwnerPositionToVec4",
                                                  15U, n3::live<&load_owner_position_to_vec4<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00384550, load_owner_position_to_vec4)
