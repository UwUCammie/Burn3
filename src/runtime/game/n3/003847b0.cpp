// 0x003847b0 (368 bytes; one jal, to Vec3Length at 0x001de260). a0 = record S, f12 = a new sample T, a1 = the address
// of a quadword Q (lq ignores its low 4 address bits) whose words 0..2 are a point. S+4 holds the previous sample and
// S+8 the tracked point P: P+72..80 its position, P+84..92 its velocity, P+132 its flags.
//
// The step is d = T - S.f4. A small step (biased exponent 111 or less, so zero and denormals included) only moves the
// point: P+72..80 = Q and flag bit 0 is set. Otherwise the velocity (Q - position) / d is formed per component, the
// change from the stored velocity is written to the original's frame (sp+96..104) for Vec3Length, and the new velocity
// is stored when that change's length is 100 or less, the old one when it is more. Then flag bit 1 is set as well,
// P+72..80 = Q, flag bit 0 is set, and S+4 = T. Every store happens in the original's order.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001de260)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 128U;       // addiu $sp, $sp, -128
constexpr GuestAddress kChangeOffset = 96U;       // the change vector: sp+96 (x), sp+100 (y), sp+104 (z)
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kSmallStep = 0x37800001U; // at = 0x37800001: exponent bits at or below 111
constexpr std::uint32_t kLimit = 0x42c80000U;     // 100.0f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of the original's blocks, delay slots included.
constexpr std::uint32_t kPrologue = 33U;     // 0x003847b0-0x00384830: up to the first branch
constexpr std::uint32_t kStoreSample = 13U;  // 0x00384834-0x00384864: P+72..80 = Q, flag 1, S+4 = T
constexpr std::uint32_t kEpilogue = 11U;     // 0x003848f4-0x0038491c
constexpr std::uint32_t kVelocity = 23U;     // 0x00384868-0x003848c0: the change, the jal and the bc1t
constexpr std::uint32_t kKeepVelocity = 3U;  // 0x003848c4-0x003848cc: the old velocity (length above 100)
constexpr std::uint32_t kStoreVelocity = 9U; // 0x003848d0-0x003848f0: the velocity store and flag 2

// P+72..80 = Q, flag bit 0 set, S+4 = T.
template <class Memory>
void store_position(Memory& m, const GuestAddress record, const GuestAddress point, const std::uint32_t qx,
                    const std::uint32_t qy, const std::uint32_t qz, const std::uint32_t sample) {
    m.write_u32_le(point + 72U, qx);
    m.write_u32_le(point + 76U, qy);
    m.write_u32_le(point + 80U, qz);
    const std::uint32_t flags = m.read_u32_le(point + 132U) | 1U;
    m.write_u32_le(point + 132U, flags);
    m.write_u32_le(record + 4U, sample);
}

template <class Context>
n3::Run sample_tracked_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t sample = call.cpu().read_fpr_bits(12U);
    const std::uint32_t previous = m.read_u32_le(record + 4U);
    const GuestAddress point = m.read_u32_le(record + 8U);
    const auto start = n3::load_quad_any(m, call.arg(1));
    const std::uint32_t qx = start.words[0];
    const std::uint32_t qy = start.words[1];
    const std::uint32_t qz = start.words[2];
    const std::uint32_t step = fpu::sub(sample, previous);
    if ((step & kExponentMask) < kSmallStep) {
        store_position(m, record, point, qx, qy, qz, sample);
        return call.finish(kPrologue + kStoreSample + kEpilogue);
    }

    // Velocity per component: (Q - position) / step, from values read before anything is stored.
    const std::uint32_t velocity_x = fpu::div(fpu::sub(qx, m.read_u32_le(point + 72U)), step);
    const std::uint32_t velocity_y = fpu::div(fpu::sub(qy, m.read_u32_le(point + 76U)), step);
    const std::uint32_t velocity_z = fpu::div(fpu::sub(qz, m.read_u32_le(point + 80U)), step);
    const std::uint32_t stored_x = m.read_u32_le(point + 84U);
    const std::uint32_t stored_y = m.read_u32_le(point + 88U);
    const std::uint32_t stored_z = m.read_u32_le(point + 92U);

    // The change from the stored velocity is the callee's argument, held in the original's frame.
    const GuestAddress change = call.stack_pointer() - kFrameBytes + kChangeOffset;
    m.write_u32_le(change, fpu::sub(velocity_x, stored_x));
    m.write_u32_le(change + 4U, fpu::sub(velocity_y, stored_y));
    m.write_u32_le(change + 8U, fpu::sub(velocity_z, stored_z));
    call.set_arg(0, change);
    const n3::Run vector_length = call.invoke([](auto& machine) { return n3::fn_001de260(machine); }, kFrameBytes);
    if (vector_length.declined) {
        return call.decline();
    }

    const bool within_limit = fpu::le(call.float_result(), kLimit);
    m.write_u32_le(point + 84U, within_limit ? velocity_x : stored_x);
    m.write_u32_le(point + 88U, within_limit ? velocity_y : stored_y);
    m.write_u32_le(point + 92U, within_limit ? velocity_z : stored_z);
    const std::uint32_t flags = m.read_u32_le(point + 132U) | 2U;
    m.write_u32_le(point + 132U, flags);
    store_position(m, record, point, qx, qy, qz, sample);

    std::uint32_t retired = kPrologue + kVelocity + kStoreVelocity + kStoreSample + kEpilogue + vector_length.retired;
    if (!within_limit) {
        retired += kKeepVelocity;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003847b0, 368U, 0xd684df4c8772f3c2ULL, "SampleTrackedPoint", kCeiling, sample_tracked_point,
         EngineReturn::Nothing)
