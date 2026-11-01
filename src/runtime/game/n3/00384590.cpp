// 0x00384590 (a0 = object, a1 = output quadword): CopyObjectPositionToQuad. The holder is the pointer at
// object +8, and the position floats are at holder +120 (x), +124 (y) and +128 (z). With sp the entry stack
// pointer, the original stores x, y and z at sp - 32 (+0, +4, +8), reloads that quadword with LQ and stores
// it to the output with SQ. LQ and SQ ignore the low four address bits, as load_quad/store_quad do. With a
// 16-byte aligned sp, the fourth lane is the word already in memory at sp - 20: the function never
// writes it, so it is not a computed value. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kHolderOffset = 8U;      // lw $v1, 8($a0)
constexpr GuestAddress kXOffset = 120U;         // lwc1 $f2, 120($v1)
constexpr GuestAddress kYOffset = 124U;         // lwc1 $f1, 124($v1)
constexpr GuestAddress kZOffset = 128U;         // lwc1 $f0, 128($v1)
constexpr GuestAddress kFrameSize = 32U;        // addiu $sp, $sp, -32
// Straight code: every instruction retires on the one path, the jr delay slot included.
constexpr std::uint32_t kRetired = 15U;

template <class Context>
n3::Run copy_object_position_to_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress output = call.arg(1);
    const GuestAddress holder = m.read_u32_le(object + kHolderOffset);
    const std::uint32_t x = m.read_u32_le(holder + kXOffset);
    const std::uint32_t y = m.read_u32_le(holder + kYOffset);
    const std::uint32_t z = m.read_u32_le(holder + kZOffset);
    const GuestAddress frame = call.stack_pointer() - kFrameSize;
    m.write_u32_le(frame + 0U, x);
    m.write_u32_le(frame + 4U, y);
    m.write_u32_le(frame + 8U, z);
    const auto quad = n3::load_quad_any(m, frame);
    // The original's later frame stores come before its SQ, so an output inside the frame sees them.
    m.write_u32_le(frame + 16U, x);
    m.write_u32_le(frame + 20U, y);
    m.write_u32_le(frame + 24U, z);
    n3::store_quad_any(m, output, quad);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00384590{0x00384590U, 60U, 0xbd0157703533e778ULL,
                                                  "CopyObjectPositionToQuad", 15U, n3::live<&copy_object_position_to_quad<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00384590, copy_object_position_to_quad)
