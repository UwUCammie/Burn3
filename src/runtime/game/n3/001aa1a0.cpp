// 0x001aa1a0 (a0 = record): SetClampBoundsAndResetRecord. Six signed bytes give bit counts: for a byte b the count is
// p(b) = (1 << ((15 - b) & 31)) - 1 in 32 bits (the shift takes the low five bits of 15 - b). The bytes are A0..A2 at
// 0x004b7a48 and B0..B2 at 0x004b7a50. Four VU0 quads (w lane kept) are rebuilt from them and from the offset quad C at
// 0x004b7a70 (x, y, z used):
//   0x0066c550 (V5) = (+p(A0), +p(A1), +p(A2)) - C     0x0066c540 (V4) = (-p(A0), -p(A1), -p(A2)) + C
//   0x0066c570 (V3) = (+p(B0), +p(B1), +p(B2)) - C     0x0066c560 (V2) = (-p(B0), -p(B1), -p(B2)) + C
// Each lane is the float of its 32-bit count (CVT.S.W), built as a VADDx from vf0 and every VU0 op publishes its
// flags in the original's order. The word at 0x01e86598 is cleared when it reads -1. Then a0 goes to
// ResetSlotLinkRecordTail (0x001a8540) as a call with an 80-byte frame. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

N3_CALLEE(001a8540)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kSignedBytesA = 0x004b7a48U;   // lb A0, A1, A2
constexpr GuestAddress kSignedBytesB = 0x004b7a50U;   // lb B0, B1, B2
constexpr GuestAddress kOffsetQuad = 0x004b7a70U;     // C (x, y, z used)
constexpr GuestAddress kQuadNegA = 0x0066c540U;       // V4
constexpr GuestAddress kQuadPosA = 0x0066c550U;       // V5
constexpr GuestAddress kQuadNegB = 0x0066c560U;       // V2
constexpr GuestAddress kQuadPosB = 0x0066c570U;       // V3
constexpr GuestAddress kFlagWord = 0x01e86598U;
constexpr std::uint32_t kFlagSet = 0xffffffffU;

// VU0 destination masks (bit 3 is x) for the lane of each byte in a group.
constexpr std::uint8_t kDestX = 8U;
constexpr std::uint8_t kDestY = 4U;
constexpr std::uint8_t kDestZ = 2U;
constexpr std::array<std::uint8_t, 3> kLaneMasks{kDestX, kDestY, kDestZ};

constexpr std::uint32_t kCallFrameBytes = 80U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts by path, delay slots included: up to and including the flag test (the bytes, the VU0 ops
// and the stores), the flag clear (lui, beq, sw), the call block (jal, nop, beq, ld) and the return (four lq, jr, addiu).
constexpr std::uint32_t kBeforeFlagTest = 132U;
constexpr std::uint32_t kFlagClearCost = 3U;
constexpr std::uint32_t kCallBlockCost = 4U;
constexpr std::uint32_t kReturnCost = 6U;

// p(b) for a signed byte b, in 32 bits: the shift is a 32-bit SLLV, so it only sees the low five bits.
std::uint32_t bit_count(const std::int8_t byte) {
    const std::uint32_t shift = (15U - static_cast<std::uint32_t>(static_cast<std::int32_t>(byte))) & 31U;
    return (1U << shift) - 1U;
}

template <class Memory>
std::array<std::int8_t, 3> read_signed_bytes(const Memory& memory, const GuestAddress first) {
    return {static_cast<std::int8_t>(memory.read_u8(first)), static_cast<std::int8_t>(memory.read_u8(first + 1U)),
            static_cast<std::int8_t>(memory.read_u8(first + 2U))};
}

// A VU0 register holding a 32-bit value in lane x (qmtc2 of a GPR; a broadcast reads only that lane).
guest::Gpr128 scalar(const std::uint32_t bits) {
    guest::Gpr128 value{};
    value.words[0] = bits;
    return value;
}

// Macro-mode FMAC instructions publish their MAC and STATUS flags along with the destination.
guest::Gpr128 published(guest::R5900State& cpu, const guest::Vu0MacroFmacResult& result) {
    guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
    return result.value;
}

// VADDx: the lane in `mask` becomes vf0 + the value (0.0 + value); the other lanes are kept.
guest::Gpr128 add_value_lane(guest::R5900State& cpu, const guest::Gpr128& destination, const std::uint32_t bits,
                             const std::uint8_t mask) {
    return published(cpu, guest::execute_vu0_macro_broadcast_add_subtract(destination, n3::kVf0, scalar(bits), mask,
                                                                          0U, false));
}

// VADD / VSUB on x, y and z: FD = FS +/- FT, w kept.
guest::Gpr128 add_subtract_xyz(guest::R5900State& cpu, const guest::Gpr128& destination, const guest::Gpr128& left,
                               const guest::Gpr128& right, const bool subtract) {
    return published(cpu, guest::execute_vu0_macro_vector_add_subtract(destination, left, right, n3::kMaskXyz, subtract));
}

// One group of three bytes: lane i of `positive` gets +p(byte i) and lane i of `negative` gets -p(byte i). The
// positive op comes before the negative op for each byte, as the original issues them.
void fill_group(guest::R5900State& cpu, const std::array<std::int8_t, 3>& bytes, guest::Gpr128& positive,
                guest::Gpr128& negative) {
    for (std::size_t lane = 0U; lane < bytes.size(); ++lane) {
        const std::uint32_t count = bit_count(bytes[lane]);
        positive = add_value_lane(cpu, positive, fpu::from_int(static_cast<std::int32_t>(count)), kLaneMasks[lane]);
        negative = add_value_lane(cpu, negative, fpu::from_int(static_cast<std::int32_t>(0U - count)), kLaneMasks[lane]);
    }
}

template <class Context>
n3::Run set_clamp_bounds_and_reset_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // Everything the original reads is read before anything is stored; the quads' w lanes are kept from these loads.
    const auto bytes_a = read_signed_bytes(m, kSignedBytesA);
    const auto bytes_b = read_signed_bytes(m, kSignedBytesB);
    const guest::Gpr128 offset = n3::load_quad_any(m, kOffsetQuad);
    guest::Gpr128 pos_a = n3::load_quad_any(m, kQuadPosA);
    guest::Gpr128 neg_a = n3::load_quad_any(m, kQuadNegA);
    guest::Gpr128 pos_b = n3::load_quad_any(m, kQuadPosB);
    guest::Gpr128 neg_b = n3::load_quad_any(m, kQuadNegB);
    const std::uint32_t flag = m.read_u32_le(kFlagWord);

    fill_group(cpu, bytes_a, pos_a, neg_a);
    fill_group(cpu, bytes_b, pos_b, neg_b);
    pos_a = add_subtract_xyz(cpu, pos_a, pos_a, offset, true);
    neg_a = add_subtract_xyz(cpu, neg_a, neg_a, offset, false);
    pos_b = add_subtract_xyz(cpu, pos_b, pos_b, offset, true);
    neg_b = add_subtract_xyz(cpu, neg_b, neg_b, offset, false);
    n3::store_quad_any(m, kQuadPosA, pos_a);
    n3::store_quad_any(m, kQuadNegA, neg_a);
    n3::store_quad_any(m, kQuadPosB, pos_b);
    n3::store_quad_any(m, kQuadNegB, neg_b);

    // The VU0 registers the original leaves behind: VF5 = V5, VF4 = V4, VF3 = V3, VF2 = V2 (the quads just stored),
    // and VF1 = C, its last load.
    cpu.write_vu0_vector(5U, pos_a);
    cpu.write_vu0_vector(4U, neg_a);
    cpu.write_vu0_vector(3U, pos_b);
    cpu.write_vu0_vector(2U, neg_b);
    cpu.write_vu0_vector(1U, offset);

    std::uint32_t retired = kBeforeFlagTest;
    if (flag == kFlagSet) {
        m.write_u32_le(kFlagWord, 0U);
        retired += kFlagClearCost;
    }
    // The original's jal: the record stays in a0, and the callee only reads a0 and writes memory.
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_001a8540(machine); }, kCallFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(retired + kCallBlockCost + kReturnCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001aa1a0, 580U, 0x165a42f3839f03a8ULL, "SetClampBoundsAndResetRecord", kCeiling,
         set_clamp_bounds_and_reset_record, EngineReturn::Nothing)
