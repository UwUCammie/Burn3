// 0x00309300 (a0 = region table T, a1 = point P, 16 bytes of x, y, z, w): FindRegionContainingPoint.
// T holds a signed count at +2480. Region i (0 <= i < count) has three quadwords A, B, C at T+2000+48*i,
// T+2016+48*i and T+2032+48*i, and an id word at T+2432+4*i. P is copied to the frame at sp-16 and loaded
// into vf2. For each region in order the VU0 computes, in all four lanes, ACC = A*P.x, ACC += B*P.z and
// vf1 = ACC - C*vf0.w, through the same FMAC primitives as the interpreter, so the MAC/STATUS flags follow
// every instruction, rejected regions included. A region contains P when no lane has its sign bit set: the
// raw sign bits decide, so -0.0 and negative NaNs count as negative. The result is the id of the first
// containing region, or -1 when none does or the count is not positive. vf2 keeps P, and vf1 and ACC keep the
// last region's values. The loop depends on the data, so the exact count comes from a dry run (call_cost) on
// n3::DryMemory; above kCeiling the original runs.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

constexpr GuestAddress kCountOffset = 2480U;       // lw: the signed count
constexpr GuestAddress kIdOffset = 2432U;          // lw: the ids, 4 bytes each
constexpr GuestAddress kFirstQuadOffset = 2000U;   // A: lqc2 (16-byte aligned)
constexpr GuestAddress kSecondQuadOffset = 2016U;  // B: lq
constexpr GuestAddress kThirdQuadOffset = 2032U;   // C: lq
constexpr GuestAddress kRegionStride = 48U;        // (3 * index) << 4
constexpr GuestAddress kFrameOffset = 16U;         // the copy of P goes to sp - 16

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 7U;        // 0x309300..0x309318: up to the jump to the check
constexpr std::uint32_t kCheckCost = 3U;           // 0x309368..0x309370: slt, bne and its delay slot
constexpr std::uint32_t kRegionCost = 18U;         // 0x30931c..0x309360: a region's VU0 work and its lane test
constexpr std::uint32_t kRejectCost = 1U;          // 0x309364: the counter step after a rejected region
constexpr std::uint32_t kFoundCost = 5U;           // 0x30937c..0x309384 (3) and 0x309388..0x30938c (2)
constexpr std::uint32_t kNotFoundCost = 4U;        // 0x309374..0x309378 (2) and 0x309388..0x30938c (2)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// A load or store the original would fault on. The dry run stops with it, so call_cost declines and the
// original runs and faults itself. The checks follow the interpreter (ee_interpreter.cpp): lw needs a 4-byte
// aligned address in guest memory, lqc2 a 16-byte aligned one, and lq / sq clear the low four bits and then
// need the quadword in guest memory.
struct Fault {};

template <class Base>
class DryEnv {
public:
    explicit DryEnv(const Base& memory) noexcept : memory_(memory) {}
    void word(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            throw Fault{};
        }
    }
    void quad_aligned(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            throw Fault{};
        }
    }
    void quad(const GuestAddress address) const {
        if (!memory_.contains(address & ~15U, 16U)) {
            throw Fault{};
        }
    }
    void fmac(const std::uint16_t) const noexcept {}
    void vf(const unsigned, const Gpr128&) const noexcept {}
    void acc(const Gpr128&) const noexcept {}

private:
    const Base& memory_;
};

// The real run: the VU0 state and its MAC/STATUS flags, in the order the original publishes them.
class RealEnv {
public:
    explicit RealEnv(guest::R5900State& cpu) noexcept : cpu_(cpu) {}
    void word(const GuestAddress) const noexcept {}
    void quad_aligned(const GuestAddress) const noexcept {}
    void quad(const GuestAddress) const noexcept {}
    void fmac(const std::uint16_t mac_flags) noexcept { guest::publish_vu0_macro_fmac_result(cpu_, mac_flags); }
    void vf(const unsigned index, const Gpr128& value) { cpu_.write_vu0_vector(index, value); }
    void acc(const Gpr128& value) noexcept { cpu_.set_vu0_accumulator(value); }

private:
    guest::R5900State& cpu_;
};

struct Result {
    std::uint32_t retired;
    std::uint32_t value;  // v0
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// count and v0; above kCeiling the loop stops early.
template <class Memory, class Env>
Result find_region(Memory& m, Env& env, const GuestAddress table, const GuestAddress point_address,
                   const GuestAddress stack) {
    const GuestAddress frame = stack - kFrameOffset;                  // addiu sp, sp, -16
    // lq the point, sq it to the frame, lqc2 vf2 back from the frame.
    env.quad(point_address);
    const Gpr128 point = n3::load_quad_any(m, point_address);
    env.quad(frame);
    n3::store_quad_any(m, frame, point);
    env.quad_aligned(frame);
    const Gpr128 vf2 = n3::load_quad_any(m, frame);
    env.vf(2U, vf2);
    env.word(table + kCountOffset);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(table + kCountOffset));   // lw a1, 2480(a0)

    std::uint32_t retired = kPrologueCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kCheckCost;
        if (!(static_cast<std::int32_t>(index) < count)) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const GuestAddress region = table + kRegionStride * index;
        const GuestAddress first = region + kFirstQuadOffset;
        env.quad_aligned(first);                                      // lqc2 vf1, 2000(v0)
        const Gpr128 a = n3::load_quad_any(m, first);
        env.quad(region + kSecondQuadOffset);                         // lq v1, 2016(v0)
        const Gpr128 b = n3::load_quad_any(m, region + kSecondQuadOffset);
        env.quad(region + kThirdQuadOffset);                          // lq v0, 2032(v0)
        const Gpr128 c = n3::load_quad_any(m, region + kThirdQuadOffset);

        // vmulax.xyzw ACC, vf1, vf2x: ACC = A * P.x
        const auto scaled = guest::execute_vu0_macro_broadcast_multiply(Gpr128{}, a, vf2, n3::kMaskXyzw, 0U);
        env.fmac(scaled.mac_flags);
        // vmaddaz.xyzw ACC, vf1, vf2z: ACC += B * P.z
        const auto summed = guest::execute_vu0_macro_broadcast_multiply_accumulate(
            scaled.value, scaled.value, b, vf2, n3::kMaskXyzw, 2U, false);
        env.fmac(summed.mac_flags);
        // vmsubw.xyzw vf1, vf1, vf0w: vf1 = ACC - C * 1.0
        const auto plane = guest::execute_vu0_macro_broadcast_multiply_accumulate(
            c, summed.value, c, n3::kVf0, n3::kMaskXyzw, 3U, true);
        env.fmac(plane.mac_flags);
        env.vf(1U, plane.value);
        env.acc(summed.value);
        retired += kRegionCost;

        // qmfc2, psraw, pcpyud, or, beq: any lane with its sign bit set rejects the region.
        const std::uint32_t signs =
            (plane.value.words[0] | plane.value.words[1] | plane.value.words[2] | plane.value.words[3]) & 0x80000000U;
        if (signs == 0U) {
            const GuestAddress id = table + kIdOffset + 4U * index;
            env.word(id);
            return {retired + kFoundCost, m.read_u32_le(id)};
        }
        retired += kRejectCost;
        ++index;
    }
    return {retired + kNotFoundCost, 0xffffffffU};
}

template <class Context>
std::uint32_t find_region_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    DryEnv env{context.memory};
    try {
        const Result result = find_region(dry, env, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                          context.cpu.read_gpr_u32(29U));
        return result.retired <= kCeiling ? result.retired : 0U;
    } catch (const Fault&) {
        return 0U;
    }
}

template <class Context>
n3::Run find_region_containing_point(Context& context) {
    auto call = n3::guest_call(context);
    RealEnv env{call.cpu()};
    const Result result = find_region(call.memory(), env, call.arg(0), call.arg(1), call.stack_pointer());
    call.return_value(result.value);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_00309300{0x00309300U, 144U, 0xe38d691129f7a0aaULL, "FindRegionContainingPoint",
                                                  kCeiling, n3::live<&find_region_containing_point<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_region_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00309300, find_region_containing_point, find_region_cost)
