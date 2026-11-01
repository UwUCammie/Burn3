// 0x00384700 (a0 = object, a1 = quad Q): SetSecondAxisAndCross. The record P is the u32 at object+8.
// The x, y and z words of Q become the second axis at P+108, +112 and +116. The cross product of the first
// axis (P+96, +100, +104) with the second is stored at P+120, +124 and +128, and bit 4 is set in the flags word
// at P+132. Like the original, the record pointer is reloaded before each block and every operand is read from
// memory after the stores before it, so the new second-axis words feed the cross terms. The original's copies
// of Q in its own stack frame are not reproduced (they are dead). Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRecordPointer = 8U;     // object+8 holds the record P
constexpr GuestAddress kSecondAxis = 108U;      // P+108, +112, +116: x, y, z of Q
constexpr GuestAddress kCross = 120U;           // P+120, +124, +128: the cross product
constexpr GuestAddress kFlags = 132U;           // P+132: flags word
constexpr std::uint32_t kFlagBit = 0x4U;        // ori $v1,$v1,0x0004
constexpr std::uint32_t kCost = 43U;            // retired instructions on the only path (no branches)

template <class Context>
n3::Run set_second_axis_and_cross(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const auto quad = n3::load_quad_any(m, call.arg(1U));

    // Second axis. The quad's words pass through the stack frame in the original and come back unchanged.
    GuestAddress record = m.read_u32_le(object + kRecordPointer);
    m.write_u32_le(record + kSecondAxis, quad.words[0]);
    m.write_u32_le(record + kSecondAxis + 4U, quad.words[1]);
    m.write_u32_le(record + kSecondAxis + 8U, quad.words[2]);

    // P+120 = P[100]*P[116] - P[104]*P[112]: MULA.S ACC = f1*f0, then MSUB.S f0 = ACC - f3*f2.
    record = m.read_u32_le(object + kRecordPointer);
    const std::uint32_t x_a = m.read_u32_le(record + 100U);
    const std::uint32_t x_b = m.read_u32_le(record + 116U);
    const std::uint32_t x_c = m.read_u32_le(record + 104U);
    const std::uint32_t x_d = m.read_u32_le(record + 112U);
    const std::uint32_t cross_x = fpu::msub(fpu::mul(x_a, x_b), x_c, x_d);
    m.write_u32_le(record + kCross, cross_x);

    // P+124 = P[104]*P[108] - P[96]*P[116].
    record = m.read_u32_le(object + kRecordPointer);
    const std::uint32_t y_a = m.read_u32_le(record + 104U);
    const std::uint32_t y_b = m.read_u32_le(record + 108U);
    const std::uint32_t y_c = m.read_u32_le(record + 96U);
    const std::uint32_t y_d = m.read_u32_le(record + 116U);
    const std::uint32_t cross_y = fpu::msub(fpu::mul(y_a, y_b), y_c, y_d);
    m.write_u32_le(record + kCross + 4U, cross_y);

    // P+128 = P[96]*P[112] - P[100]*P[108]. MSUB.S leaves ACC alone, so ACC keeps this block's product.
    record = m.read_u32_le(object + kRecordPointer);
    const std::uint32_t z_a = m.read_u32_le(record + 96U);
    const std::uint32_t z_b = m.read_u32_le(record + 112U);
    const std::uint32_t z_c = m.read_u32_le(record + 100U);
    const std::uint32_t z_d = m.read_u32_le(record + 108U);
    const std::uint32_t acc = fpu::mul(z_a, z_b);
    const std::uint32_t cross_z = fpu::msub(acc, z_c, z_d);
    m.write_u32_le(record + kCross + 8U, cross_z);

    // Flags: the original reloads the record into a0, then sets bit 4 of P+132.
    const GuestAddress target = m.read_u32_le(object + kRecordPointer);
    const std::uint32_t flags = m.read_u32_le(target + kFlags) | kFlagBit;
    m.write_u32_le(target + kFlags, flags);

    // Registers the original leaves behind (the checker compares only callee-saved ones here): ACC, f0 = last cross
    // term, f1 = P[96], f2 = P[108] (Q.x), f3 = P[100], v1 = the new flags, a0 = P, a1 = Q.
    cpu.set_fpu_accumulator_bits(acc);
    cpu.write_fpr_bits(0U, cross_z);
    cpu.write_fpr_bits(1U, z_a);
    cpu.write_fpr_bits(2U, z_d);
    cpu.write_fpr_bits(3U, z_c);
    cpu.write_gpr(5U, quad);
    cpu.write_gpr_sign_extended_32(4U, target);
    cpu.write_gpr_sign_extended_32(3U, flags);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00384700{0x00384700U, 172U, 0x86097be4219e23f9ULL, "SetSecondAxisAndCross",
                                                  43U, n3::live<&set_second_axis_and_cross<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00384700, set_second_axis_and_cross)
