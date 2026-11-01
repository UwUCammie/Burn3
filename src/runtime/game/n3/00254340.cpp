// 0x00254340 (a0 = mesh list L): BuildMeshDmaChain. L+0 is a pointer to the render context (t7) and L+12 the
// entry count, which is re-read at every loop test. Entry i is the word at L+4+4i, so the count word itself is
// entry 2. Each entry appends a 336-byte display-list DMA packet at the global write cursor (the word at
// gp-24240), then a 16-byte tag when the entry's second word is set. The packet's quadwords come from the entry's
// first word, from rows of the render context (the float at t7+116 is replaced by each entry's float first),
// from the VU0 sequence below and from constants. The cursor is stored back at the end. Leaf: returns nothing.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

constexpr GuestAddress kRow1Address = 0x01d9cfd0U;  // lqc2 into VF1 (lui 0x01da, -12336)
constexpr GuestAddress kRow2Address = 0x01d9cfe0U;  // lq into a1 (lui 0x01da, -12320)
constexpr GuestAddress kRow3Address = 0x01d9cff0U;  // lq into v1 (lui 0x01da, -12304)
constexpr std::uint32_t kCursorOffset = static_cast<std::uint32_t>(-24240);  // gp-relative write cursor
constexpr std::uint32_t kScaleOffset = static_cast<std::uint32_t>(-23704);   // gp-relative word, VF1.x for vmulx
constexpr std::uint32_t kFloorOffset = static_cast<std::uint32_t>(-23700);   // gp-relative float for MAX.S
constexpr std::uint32_t kCountOffset = 12U;
constexpr std::uint32_t kFrameBytes = 224U;
constexpr std::uint32_t kPacketBytes = 336U;
constexpr std::uint32_t kTagBytes = 16U;
constexpr std::size_t kStackPointer = 29U;
constexpr std::uint8_t kMaskZ = 2U;

// Retired instruction counts from the listing (brief.py's block table), delay slots included.
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kPreambleCost = 16U;     // 0x340..0x37c, through the taken blez
constexpr std::uint32_t kPreambleWideCost = 8U;  // 0x380..0x39c, only when the packet byte is at least 4
constexpr std::uint32_t kSetupCost = 57U;        // 0x3a0..0x480, through the jump to the loop test
constexpr std::uint32_t kTestCost = 4U;          // 0x558..0x564, once per loop test
constexpr std::uint32_t kEntryCost = 45U;        // 0x484..0x534, once per entry
constexpr std::uint32_t kTagCost = 7U;           // 0x538..0x550, only when the entry's second word is set
constexpr std::uint32_t kStepCost = 1U;          // 0x554, the index step
constexpr std::uint32_t kReturnCost = 4U;        // 0x568..0x574
// Every entry retires at least kEntryCost + kStepCost + kTestCost, so a count above this exceeds kCeiling.
constexpr std::uint32_t kMaxEntries =
    (kCeiling - kPreambleCost - kSetupCost - kReturnCost - kTestCost) / (kEntryCost + kStepCost + kTestCost);

struct Inputs {
    GuestAddress list{};
    std::uint32_t gp{};
    GuestAddress sp{};
    Gpr128 s0{};
    Gpr128 vf0{};
    Gpr128 acc{};
};

struct Outcome {
    std::uint32_t retired{};
    Gpr128 vf1{};
    Gpr128 vf2{};
    Gpr128 vf3{};
    Gpr128 acc{};
    std::array<std::uint16_t, 6> mac_flags{};
    std::uint32_t floor_bits{};
    std::uint32_t f1_bits{};
    std::uint32_t f0_bits{};
};

Gpr128 make_quad(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// A 32-bit GPR result: the low doubleword is the sign-extended word, the high doubleword is kept.
Gpr128 with_sign_extended_low(Gpr128 value, const std::uint32_t word) {
    value.words[0] = word;
    value.words[1] = (word & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

// MAX.S on raw bits, ordered as guest::ee_cop1::max_s orders them.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// The original's memory accesses on a read-only view of guest memory that keeps its own writes, checked the way
// the interpreter checks them: lw/lwu/lwc1/swc1 need 4-byte alignment, lqc2/sqc2 16-byte alignment, lq/sq are
// masked, and every access must lie in guest memory. A failing access is recorded and not performed, so
// call_cost declines and the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    void write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
        const GuestAddress aligned = address & ~15U;
        if (!memory_.contains(aligned, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(aligned, words);
    }
    void note_quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// lqc2/sqc2 fault on a misaligned or unmapped address. call_cost checks the addresses first, so the
// implementation (which only runs after call_cost accepted them) needs no check on guest memory.
void need_quad(const auto& memory, const GuestAddress address) noexcept {
    if constexpr (requires { memory.note_quad(address); }) memory.note_quad(address);
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). The VU0 steps
// use the same pure primitives as the interpreter's COP2 macro ops, in the original's order.
template <class Memory>
Outcome build_packets(Memory& m, const Inputs& in) {
    Outcome out{};
    const GuestAddress list = in.list;
    const std::uint32_t gp = in.gp;
    const GuestAddress frame = in.sp - kFrameBytes;

    n3::store_quad_any(m, frame, in.s0);                                             // sq s0, 0(sp)

    const GuestAddress t7 = m.read_u32_le(list);                                     // lw t7, 0(a0)
    const GuestAddress record = m.read_u32_le(t7 + 224U);                            // lw v1, 224(t7)
    const std::uint64_t k_low = static_cast<std::uint64_t>(m.read_u32_le(record + 60U)) |
                                (static_cast<std::uint64_t>(m.read_u32_le(record + 64U)) << 32U);  // a2
    const std::uint32_t shifted = static_cast<std::uint32_t>(m.read_u8(record + 74U)) >> 2U;  // lbu, sra 2
    const bool wide = shifted != 0U;                                                 // blez falls through
    std::uint64_t t2_low = 0U;
    std::uint64_t t1_low = 0U;
    if (wide) {
        const GuestAddress records = record + 52U;                                   // t0
        t2_low = static_cast<std::uint64_t>(m.read_u32_le(records + 24U)) |
                 (static_cast<std::uint64_t>(m.read_u32_le(records + 28U)) << 32U);
        t1_low = static_cast<std::uint64_t>(m.read_u32_le(records + 32U)) |
                 (static_cast<std::uint64_t>(m.read_u32_le(records + 36U)) << 32U);
    }
    std::uint32_t retired = (wide ? kPreambleCost + kPreambleWideCost : kPreambleCost) + kSetupCost;

    need_quad(m, t7 + 80U);
    const Gpr128 m80 = n3::load_quad_any(m, t7 + 80U);                              // lqc2 vf2, 80(t7)
    need_quad(m, frame + 208U);
    const Gpr128 stale = n3::load_quad_any(m, frame + 208U);                        // lqc2 vf3, 208(sp)
    need_quad(m, kRow1Address);
    const Gpr128 row1 = n3::load_quad_any(m, kRow1Address);                         // lqc2 vf1
    const Gpr128 row2 = n3::load_quad_any(m, kRow2Address);                         // lq a1
    const Gpr128 row3 = n3::load_quad_any(m, kRow3Address);                         // lq v1
    const std::uint32_t scale = m.read_u32_le(gp + kScaleOffset);                   // lw a1, -23704(gp)

    Gpr128 vf3 = stale;                                                              // vmove.w vf3, vf2
    vf3.words[3] = m80.words[3];
    const auto step0 = guest::execute_vu0_macro_broadcast_multiply(in.acc, row1, m80, n3::kMaskXyz, 0U);
    const auto step1 = guest::execute_vu0_macro_broadcast_multiply_accumulate(step0.value, step0.value, row2, m80,
                                                                              n3::kMaskXyz, 1U, false);
    const auto step2 = guest::execute_vu0_macro_broadcast_multiply_accumulate(row3, step1.value, row3, m80,
                                                                              n3::kMaskXyz, 2U, false);
    const auto step3 = guest::execute_vu0_macro_vector_add_subtract(vf3, in.vf0, step2.value, n3::kMaskXyz, false);
    const Gpr128 row208 = step3.value;
    const Gpr128 a1_scaled = with_sign_extended_low(row2, scale);
    need_quad(m, frame + 208U);
    n3::store_quad_any(m, frame + 208U, row208);                                    // sqc2 vf2, 208(sp)
    need_quad(m, t7 + 128U);
    const Gpr128 m128 = n3::load_quad_any(m, t7 + 128U);                            // lqc2 vf2, 128(t7)
    const std::uint32_t floor_bits = m.read_u32_le(gp + kFloorOffset);              // lwc1 f0, -23700(gp)
    const auto step4 = guest::execute_vu0_macro_broadcast_multiply(m128, m128, a1_scaled, n3::kMaskXyzw, 0U);
    need_quad(m, frame + 32U);
    n3::store_quad_any(m, frame + 32U, step4.value);                                // sqc2 vf2, 0(v1)
    const std::uint32_t f1_bits = step4.value.words[2];                             // lwc1 f1, 40(sp)
    const std::uint32_t max_value = max_bits(f1_bits, floor_bits);                  // max.s f0, f1, f0
    const Gpr128 a1_max = with_sign_extended_low(a1_scaled, max_value);             // mfc1 a1, f0
    const auto step5 = guest::execute_vu0_macro_broadcast_add_subtract(step4.value, in.vf0, a1_max, kMaskZ, 0U, false);
    const Gpr128 row16 = step5.value;

    std::uint32_t cursor = m.read_u32_le(gp + kCursorOffset);                       // lw t3, -24240(gp)
    need_quad(m, frame + 16U);
    n3::store_quad_any(m, frame + 16U, row16);                                      // sqc2 vf2, 16(sp)

    const Gpr128 t2_quad = make_quad(0x0000000010000000ULL, 0x0000000010000000ULL);
    const Gpr128 t1_quad = make_quad(0x0000000010000011ULL, 0x6c1103e801000101ULL);
    const Gpr128 t0_quad = make_quad(0x0000000010000000ULL, 0x0000000017000000ULL);
    const Gpr128 t6_quad = make_quad(k_low, 6U);
    const Gpr128 t5_quad = make_quad(t2_low, 52U);
    const Gpr128 t4_quad = make_quad(t1_low, 54U);

    std::uint32_t last_float = max_value;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        const std::uint32_t count = m.read_u32_le(list + kCountOffset);            // lw a3, 12(a0)
        const auto count_wide =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(count)));
        if (!(static_cast<std::uint64_t>(index) < count_wide)) {                    // sltu, bne (not taken)
            break;
        }
        if (retired > kCeiling) {
            out.retired = retired;
            return out;
        }
        const std::uint32_t entry = m.read_u32_le(list + 4U * index + 4U);          // lw a3, 4(a3)
        const std::uint32_t tag = m.read_u32_le(entry);                             // lwu s0, 0(a3)
        n3::store_quad_any(m, cursor, make_quad((static_cast<std::uint64_t>(tag) << 32U) | 0x50000000ULL, 0U));
        n3::store_quad_any(m, cursor + 16U, t2_quad);
        const std::uint32_t entry_float = m.read_u32_le(entry + 36U);               // lwc1 f0, 36(a3)
        m.write_u32_le(t7 + 116U, entry_float);                                     // swc1 f0, 116(t7)
        n3::store_quad_any(m, cursor + 32U, t1_quad);
        n3::store_quad_any(m, cursor + 48U, n3::load_quad_any(m, t7 + 0U));
        n3::store_quad_any(m, cursor + 64U, n3::load_quad_any(m, t7 + 16U));
        n3::store_quad_any(m, cursor + 80U, n3::load_quad_any(m, t7 + 32U));
        n3::store_quad_any(m, cursor + 96U, n3::load_quad_any(m, t7 + 48U));
        n3::store_quad_any(m, cursor + 112U, n3::load_quad_any(m, t7 + 64U));
        n3::store_quad_any(m, cursor + 128U, row208);
        n3::store_quad_any(m, cursor + 144U, n3::load_quad_any(m, t7 + 96U));
        n3::store_quad_any(m, cursor + 160U, n3::load_quad_any(m, t7 + 112U));
        n3::store_quad_any(m, cursor + 176U, row16);
        n3::store_quad_any(m, cursor + 192U, n3::load_quad_any(m, t7 + 144U));
        n3::store_quad_any(m, cursor + 208U, n3::load_quad_any(m, t7 + 160U));
        n3::store_quad_any(m, cursor + 224U, n3::load_quad_any(m, t7 + 176U));
        n3::store_quad_any(m, cursor + 240U, n3::load_quad_any(m, t7 + 192U));
        n3::store_quad_any(m, cursor + 256U, t6_quad);
        n3::store_quad_any(m, cursor + 272U, n3::load_quad_any(m, t7 + 208U));
        n3::store_quad_any(m, cursor + 288U, t5_quad);
        n3::store_quad_any(m, cursor + 304U, t4_quad);
        n3::store_quad_any(m, cursor + 320U, t0_quad);
        const std::uint32_t extra = m.read_u32_le(entry + 4U);                      // lw a3, 4(a3)
        cursor += kPacketBytes;                                                     // addiu t3, 336
        retired += kEntryCost;
        if (extra != 0U) {
            n3::store_quad_any(m, cursor, make_quad((static_cast<std::uint64_t>(extra) << 32U) | 0x50000000ULL, 0U));
            cursor += kTagBytes;
            retired += kTagCost;
        }
        retired += kStepCost;
        last_float = entry_float;
        ++index;
    }
    retired += kReturnCost;
    m.write_u32_le(gp + kCursorOffset, cursor);                                     // sw t3, -24240(gp)

    out.retired = retired;
    out.vf1 = a1_max;
    out.vf2 = row16;
    out.vf3 = vf3;
    out.acc = step1.value;
    out.mac_flags = {step0.mac_flags, step1.mac_flags, step2.mac_flags, step3.mac_flags, step4.mac_flags,
                     step5.mac_flags};
    out.floor_bits = floor_bits;
    out.f1_bits = f1_bits;
    out.f0_bits = last_float;
    return out;
}

Inputs make_inputs(const guest::R5900State& cpu, const GuestAddress list) {
    return Inputs{list,
                  cpu.read_gpr_u32(n3::kGlobalPointer),
                  cpu.read_gpr_u32(kStackPointer),
                  cpu.read_gpr(16U),
                  cpu.read_vu0_vector(0U),
                  cpu.vu0_accumulator()};
}

// The retired count of the whole function, decided before it runs: 0 declines (the original runs), which also
// covers faulting addresses and counts whose retired total exceeds kCeiling.
template <class Context>
std::uint32_t build_mesh_dma_chain_cost(const Context& context) {
    const GuestAddress list = context.cpu.read_gpr_u32(4U);
    CheckedDryMemory dry{context.memory};
    const std::uint32_t count = dry.read_u32_le(list + kCountOffset);
    if (dry.faulted() || static_cast<std::int32_t>(count) < 0 || count > kMaxEntries) {
        return 0U;
    }
    const Outcome outcome = build_packets(dry, make_inputs(context.cpu, list));
    if (dry.faulted() || outcome.retired > kCeiling) {
        return 0U;
    }
    return outcome.retired;
}

template <class Context>
n3::Run build_mesh_dma_chain(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const Outcome outcome = build_packets(call.memory(), make_inputs(cpu, call.arg(0)));
    cpu.write_vu0_vector(1U, outcome.vf1);
    cpu.write_vu0_vector(2U, outcome.vf2);
    cpu.write_vu0_vector(3U, outcome.vf3);
    cpu.set_vu0_accumulator(outcome.acc);
    for (const std::uint16_t flags : outcome.mac_flags) {
        guest::publish_vu0_macro_fmac_result(cpu, flags);
    }
    // MAX.S f0, f1, f0 on the FPU itself, for its FCR31 side effect; the value is the same as max_bits.
    cpu.write_fpr_bits(1U, outcome.f1_bits);
    cpu.write_fpr_bits(0U, outcome.floor_bits);
    guest::ee_cop1::max_s(cpu, 0U, 1U, 0U);
    cpu.write_fpr_bits(0U, outcome.f0_bits);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00254340{0x00254340U, 568U, 0x7971e2e43fd286c5ULL, "BuildMeshDmaChain",
                                                  kCeiling, n3::live<&build_mesh_dma_chain<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &build_mesh_dma_chain_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00254340, build_mesh_dma_chain, build_mesh_dma_chain_cost)
