// 0x0014e3a0 (a0 = S, a1 = car): CarSpeedStep. S+56 is copied from S+52 when S+80 or S+81 is set. Otherwise, with
// S+82 set, S+56 becomes 0 + S+56 - 0x0052267c * S+64 (adda.s / msub.s; the accumulator keeps the adda result). With
// S+82 set, S+86 is then set when S+85 is set. Every path ends with CarInputTimersUpdate (0x0030b190) on (S, car).
// The paths that reach 0x0030b100 (not native) decline, so the original runs the whole call. Counts are the listing's
// blocks, delay slots included, plus the callee's own count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0030b190)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 48U;           // addiu $sp, $sp, -48
constexpr GuestAddress kScaleAddress = 0x0052267cU;  // lui 0x52, lwc1 9852($v0)
constexpr std::int32_t kGlobalOffset = -29228;       // lwc1 -29228($gp)
// Retired on every path that reaches the call: 0x0014e4b8 (1), the jal and its delay slot (2), the return's beq and
// its delay slot (2), and the epilogue at 0x0014e4f0 (4).
constexpr std::uint32_t kCallTail = 9U;
constexpr std::uint32_t kCeiling = 20000U;

// lwc1 $f0, 52($s); swc1 $f0, 56($s) (blocks 0x0014e4cc and 0x0014e4e4)
template <class Memory>
void copy_word_52_to_56(Memory& m, const GuestAddress record) {
    m.write_u32_le(record + 56U, m.read_u32_le(record + 52U));
}

template <class Context>
n3::Run car_speed_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress car = call.arg(1);
    // Read on entry, as the original does: lwc1 $f4, 4300($a1) and lwc1 $f3, 9852($v0).
    const std::uint32_t car_value = m.read_u32_le(car + 4300U);
    const std::uint32_t scale = m.read_u32_le(kScaleAddress);
    std::uint32_t retired = 11U; // 0x0014e3a0-0x0014e3c8: prologue, the byte 82 test and its delay slot

    if (m.read_u8(record + 82U) == 0U) {
        retired += 3U; // 0x0014e4a0: byte 80
        if (m.read_u8(record + 80U) != 0U) {
            retired += 3U; // 0x0014e4e4
            copy_word_52_to_56(m, record);
        } else {
            retired += 3U; // 0x0014e4ac: byte 81
            if (m.read_u8(record + 81U) != 0U) {
                retired += 3U; // 0x0014e4e4
                copy_word_52_to_56(m, record);
            }
        }
    } else {
        retired += 3U; // 0x0014e3cc: byte 80
        if (m.read_u8(record + 80U) != 0U) {
            retired += 3U; // 0x0014e4cc
            copy_word_52_to_56(m, record);
        } else {
            retired += 3U; // 0x0014e3d8: byte 81
            if (m.read_u8(record + 81U) != 0U) {
                retired += 3U; // 0x0014e4cc
                copy_word_52_to_56(m, record);
            } else {
                retired += 7U; // 0x0014e3e4: adda.s / msub.s and the store
                const std::uint32_t accumulator = fpu::add(0U, m.read_u32_le(record + 56U));
                call.cpu().set_fpu_accumulator_bits(accumulator);
                m.write_u32_le(record + 56U, fpu::msub(accumulator, scale, m.read_u32_le(record + 64U)));
            }
        }
        retired += 3U; // 0x0014e400: byte 85
        if (m.read_u8(record + 85U) != 0U) {
            retired += 3U; // 0x0014e4d8
            m.write_u8(record + 86U, 1U);
        }
        retired += 3U; // 0x0014e40c: byte 83
        if (m.read_u8(record + 83U) == 0U) {
            retired += 6U; // 0x0014e468
            if (fpu::le(m.read_u32_le(record + 56U), 0U)) {
                return call.decline(); // 0x0014e480 calls 0x0030b100
            }
        } else {
            retired += 6U; // 0x0014e418
            const GuestAddress global_pointer = call.cpu().read_gpr_u32(n3::kGlobalPointer);
            const GuestAddress global_word = global_pointer + static_cast<GuestAddress>(kGlobalOffset);
            const std::uint32_t limit = fpu::add(m.read_u32_le(record + 36U), m.read_u32_le(global_word));
            if (!fpu::lt(car_value, limit)) {
                return call.decline(); // 0x0014e44c calls 0x0030b100
            }
        }
    }

    // 0x0014e4b8: a0 = S and a1 = car are the entry registers, which the original's daddu copies leave unchanged.
    const n3::Run callee = call.invoke([](auto& target) { return n3::fn_0030b190(target); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(retired + kCallTail + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014e3a0, 352U, 0xd26903d82154b8a9ULL, "CarSpeedStep", kCeiling, car_speed_step, EngineReturn::Nothing)
