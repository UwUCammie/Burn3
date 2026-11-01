// 0x00447ab0 (a0 = corner floats, a1 = list header, a2 = record table): BuildLinkStripQuads.
// Walks the zero-terminated u16 id list at (u32)a1[0]. Each id is looked up in the record table (buckets at
// a2+32+4*(id&0x7f), end sentinel u32 at a2+28, linear list head at a2+160). A record with float0 >= 0 emits
// a quad strip into two parallel point arrays A (0x0066eb20) and B (0x0066fb00). Returns the number of points
// emitted in v0 (EngineReturn::Integer). The stack scratch of the original is modelled as plain values: every
// reload reads the value stored just before it, so the ld/sd bit moves carry no extra state.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kArrayA = 0x0066eb20U;     // a3 (lui 0x67, addiu -5344)
constexpr GuestAddress kArrayB = 0x0066fb00U;     // v1 (lui 0x67, addiu -1280)
constexpr GuestAddress kScaleOffset = 31580U;     // -31580($gp): float scale of the strip
constexpr GuestAddress kDivOffset = 31396U;       // -31396($gp): float factor of the id-32 record

// Calls retiring more than this are left to the original (call_cost returns 0).
constexpr std::uint32_t kCeiling = 40000U;

struct Outcome {
    std::uint32_t retired;
    std::uint32_t count;
};

template <class Memory>
std::uint32_t lhu(const Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(m.read_u16_le(address));
}

template <class Memory>
std::uint32_t lw(const Memory& m, const GuestAddress address) {
    return m.read_u32_le(address);
}

// The whole function on guest memory (the implementation) or on a checked DryMemory (call_cost): returns the
// retired count and the points emitted. Above kCeiling every loop returns early.
template <class Memory>
Outcome strip_body(Memory& m, const GuestAddress gp, const GuestAddress corner, const GuestAddress list,
                   const GuestAddress table) {
    // 447ab0..447ad0: prologue and the first id's bucket head.
    std::uint32_t retired = 9U;
    const GuestAddress ids = lw(m, list);
    std::uint32_t id = lhu(m, ids);
    GuestAddress rec = lw(m, table + 32U + 4U * (id & 0x7fU));
    const GuestAddress end = lw(m, table + 28U);
    // 447ae0 / 447ad4 / 447adc: first lookup (no zero test on the first id).
    for (;;) {
        retired += 5U;
        if (lhu(m, rec + 28U) == id) {
            break;
        }
        retired += 2U;
        if (rec == end) {
            break;
        }
        retired += 1U;
        rec += 32U;
        if (retired > kCeiling) {
            return {retired, 0U};
        }
    }

    // 447af4..447b98: once-per-call setup; ACC order as in the listing (adda, madd, adda, msub).
    retired += 42U;
    const std::uint32_t p = lw(m, list + 4U);
    const std::uint32_t g = lw(m, gp - kScaleOffset);
    const std::uint32_t t8 = lw(m, table + 8U);
    GuestAddress t3 = lw(m, table + 160U);
    const std::uint32_t t12 = lw(m, table + 12U);
    const std::uint32_t x1 = lw(m, corner + 4U);
    const std::uint32_t x2 = lw(m, corner + 8U);
    const std::uint32_t x3 = lw(m, corner + 12U);
    const std::uint32_t x0 = lw(m, corner + 0U);
    const std::uint32_t t16 = lw(m, table + 16U);
    const std::uint32_t q = lw(m, list + 8U);
    const std::uint32_t r16 = lw(m, rec + 16U);
    const std::uint32_t gp_p = fpu::mul(g, p);
    const std::uint32_t f2a = fpu::mul(t8, x2);
    const std::uint32_t f2b = fpu::mul(gp_p, x2);
    const std::uint32_t f7 = fpu::div(f2b, x3);
    const std::uint32_t f0c = fpu::mul(t12, x3);
    const std::uint32_t acc_x1 = fpu::add(0U, x1);
    const std::uint32_t f6 = fpu::madd(acc_x1, x3, t16);
    const std::uint32_t f1p = fpu::mul(p, f2a);
    const std::uint32_t f4 = fpu::mul(q, f1p);
    const std::uint32_t acc_x0 = fpu::add(0U, x0);
    std::uint32_t f3 = fpu::msub(acc_x0, r16, f4);

    // 447ba8 / 447b9c / 447ba4: the linear search for id 32 (record t3; the end sentinel on a miss).
    for (;;) {
        retired += 5U;
        if (lhu(m, t3 + 28U) == 32U) {
            break;
        }
        retired += 2U;
        if (t3 == end) {
            break;
        }
        retired += 1U;
        t3 += 32U;
        if (retired > kCeiling) {
            return {retired, 0U};
        }
    }
    // 447bbc..447bdc
    retired += 9U;
    const std::uint32_t f24 = lw(m, t3 + 24U);
    const std::uint32_t h = lw(m, gp - kDivOffset);
    const std::uint32_t f5 = fpu::mul(h, fpu::mul(f24, f4));
    const std::uint32_t f2 = f0c;
    const std::uint32_t f1 = x1;

    std::uint32_t count = 0U;
    GuestAddress a3 = kArrayA;
    GuestAddress v1 = kArrayB;
    GuestAddress cursor = ids;
    // 447be0: the first record's float0.
    retired += 1U;
    std::uint32_t f8 = lw(m, rec);

    for (;;) {
        if (retired > kCeiling) {
            return {retired, count};
        }
        // 447be4..447bf4: skip when float0 < 0 (c.olt.s f8, f0 with f0 = +0).
        retired += 3U;
        const bool negative = fpu::lt(f8, 0U);
        if (negative) {
            retired += 1U;
        }
        retired += 3U;
        if (!negative) {
            // 447c00: the room test (v0 + 5 must stay below 509).
            retired += 4U;
            if (count + 5U >= 509U) {
                return {retired + 2U, count};
            }
            // 447c10..447d18: corner floats and the strip values.
            retired += 67U;
            const std::uint32_t r0 = lw(m, rec + 0U);
            const std::uint32_t r4 = lw(m, rec + 4U);
            const std::uint32_t r8 = lw(m, rec + 8U);
            const std::uint32_t r12 = lw(m, rec + 12U);
            const std::uint32_t r16r = lw(m, rec + 16U);
            const std::uint32_t r20 = lw(m, rec + 20U);
            const std::uint32_t a1v = fpu::mul(r20, f2);
            const std::uint32_t b1 = fpu::mul(r16r, f4);
            const std::uint32_t c1 = fpu::mul(f4, r8);
            const std::uint32_t d1 = fpu::add(r0, r8);
            const std::uint32_t e1 = fpu::mul(f2, r12);
            const std::uint32_t f1s = fpu::add(f3, b1);
            const std::uint32_t g1 = fpu::add(f1, a1v);
            const std::uint32_t h1 = fpu::add(r4, r12);
            const std::uint32_t i1 = fpu::add(g1, e1);
            const std::uint32_t j1 = fpu::add(f1s, c1);
            const std::uint32_t k1 = fpu::sub(f6, g1);
            const std::uint32_t l1 = fpu::mul(f7, k1);
            const std::uint32_t mm1 = fpu::add(l1, f1s);
            const std::uint32_t n1 = fpu::sub(f6, i1);
            const std::uint32_t o1 = fpu::mul(f7, n1);
            const std::uint32_t p1 = fpu::add(l1, j1);
            const std::uint32_t q1 = fpu::add(o1, f1s);
            const std::uint32_t r1 = fpu::add(o1, j1);

            // blez v0: a join (two points copied from the previous ones) only when v0 > 0.
            const bool join = static_cast<std::int32_t>(count) > 0;
            if (join) {
                retired += 16U;
                const std::uint32_t ja0 = lw(m, a3 - 8U);
                const std::uint32_t ja1 = lw(m, a3 - 4U);
                m.write_u32_le(a3 + 0U, ja0);
                m.write_u32_le(a3 + 4U, ja1);
                const std::uint32_t jb0 = lw(m, v1 - 8U);
                const std::uint32_t jb1 = lw(m, v1 - 4U);
                m.write_u32_le(v1 + 0U, jb0);
                m.write_u32_le(v1 + 4U, jb1);
                m.write_u32_le(a3 + 8U, mm1);
                m.write_u32_le(a3 + 12U, g1);
                m.write_u32_le(v1 + 8U, r0);
                a3 += 16U;
                m.write_u32_le(v1 + 12U, r4);
                v1 += 16U;
                count += 2U;
            }
            // 447d5c..447de4: the four points of the strip.
            retired += 35U;
            count += 4U;
            m.write_u32_le(a3 + 0U, mm1);
            m.write_u32_le(a3 + 4U, g1);
            m.write_u32_le(v1 + 0U, r0);
            m.write_u32_le(v1 + 4U, r4);
            m.write_u32_le(a3 + 8U, p1);
            m.write_u32_le(a3 + 12U, g1);
            m.write_u32_le(v1 + 8U, d1);
            m.write_u32_le(v1 + 12U, r4);
            m.write_u32_le(a3 + 16U, q1);
            m.write_u32_le(a3 + 20U, i1);
            m.write_u32_le(v1 + 16U, r0);
            m.write_u32_le(v1 + 20U, h1);
            m.write_u32_le(a3 + 24U, r1);
            m.write_u32_le(a3 + 28U, i1);
            m.write_u32_le(v1 + 24U, d1);
            a3 += 32U;
            m.write_u32_le(v1 + 28U, h1);
            v1 += 32U;
        }

        // 447de8..447df4: the next id; a zero id ends the list.
        retired += 4U;
        cursor += 2U;
        id = lhu(m, cursor);
        if (id == 0U) {
            return {retired + 2U, count};
        }
        // 447df8..447e18: ACC = 0 + f3; f3 = f5 + (ACC + R24 * f4) with R24 of the record just used.
        retired += 9U;
        const std::uint32_t acc = fpu::add(0U, f3);
        const std::uint32_t r24 = lw(m, rec + 24U);
        f3 = fpu::add(f5, fpu::madd(acc, r24, f4));
        rec = lw(m, table + 32U + 4U * (id & 0x7fU));
        // 447e28 / 447e1c / 447e24: lookup of the new id; the end sentinel is used on a miss.
        bool matched = false;
        for (;;) {
            retired += 5U;
            if (lhu(m, rec + 28U) == id) {
                matched = true;
                break;
            }
            retired += 2U;
            if (rec == end) {
                break;
            }
            retired += 1U;
            rec += 32U;
            if (retired > kCeiling) {
                return {retired, count};
            }
        }
        // 447e3c (2, the lwc1 is its delay slot) or 447be0 (1) on the sentinel.
        retired += matched ? 2U : 1U;
        f8 = lw(m, rec);
    }
}

// A DryMemory whose loads and stores are checked the way the interpreter checks them (lhu needs a 2-byte
// aligned address, lw and sw a 4-byte one, and each must lie in guest memory). An access the original would
// fault on is not performed and is recorded, so call_cost declines and the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
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
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t build_link_strip_quads_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Outcome out = strip_body(dry, context.cpu.read_gpr_u32(n3::kGlobalPointer),
                                   context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                   context.cpu.read_gpr_u32(6U));
    if (out.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return out.retired;
}

template <class Context>
n3::Run build_link_strip_quads(Context& context) {
    auto call = n3::guest_call(context);
    const Outcome out = strip_body(call.memory(), call.cpu().read_gpr_u32(n3::kGlobalPointer), call.arg(0),
                                   call.arg(1), call.arg(2));
    call.return_value(out.count);
    return call.finish(out.retired);
}

} // namespace

extern constinit const EngineFunction n3_00447ab0{0x00447ab0U, 924U, 0x2929cc29047460b7ULL, "BuildLinkStripQuads",
                                                  kCeiling, n3::live<&build_link_strip_quads<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &build_link_strip_quads_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00447ab0, build_link_strip_quads, build_link_strip_quads_cost)
