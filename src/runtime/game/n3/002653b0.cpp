// 0x002653b0 BuildGifTilePacket: appends a GIF packet to the draw buffer whose write pointer is the word at
// $gp - 24240. The fixed part is 19 quadwords (288 bytes): a PACKED A+D header and register writes (TEX0/TEX1-style
// words with constants such as 0x10a34000, 0x00030003, 0x00031001, 0x00808080), then for each of (a1 >> 6) tiles a
// block of 128 bytes (even tiles) or 128 bytes with a different header (odd tiles), each ending with sprite
// coordinates stepping by 512 / 1024 per tile. The colour words come from a float argument at (a0): it is scaled by
// 255 and then split into three more byte-sized values by divisions around 128 and 256.
// Arguments: a0 = pointer to a float, a1, a2, a3, t0, t1, t2, t3; two stack arguments at sp+0 and sp+8.
// Returns nothing the callers read (v1 and f0 are scratch). The new write pointer is stored back at $gp - 24240.
//
// Written as a faithful transcription of the listing: every register that is stored with `sq` is modelled as a
// 128-bit value (pcpyld fills the upper half), everything else works on the low 64 bits as the instructions do.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::int32_t kGpWritePointer = -24240;
constexpr std::int32_t kGpFloatA = -30952;
constexpr std::int32_t kGpFloatB = -30404;
constexpr std::uint32_t kCeiling = 20000U;

struct Q {
    std::uint64_t lo{};
    std::uint64_t hi{};
};

constexpr std::uint64_t sx(const std::uint32_t v) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)));
}
constexpr std::uint32_t low32(const std::uint64_t v) { return static_cast<std::uint32_t>(v); }
// PCPYLD rd, rs, rt: rd = { rt.lo, rs.lo }.
constexpr Q pcpyld(const Q& rs, const Q& rt) { return Q{rt.lo, rs.lo}; }

template <class Memory>
void sq(Memory& m, const std::uint64_t base, const std::uint32_t offset, const Q& q) {
    n3::store_quad_any(m, low32(base) + offset,
                       n3::Gpr128{{low32(q.lo), static_cast<std::uint32_t>(q.lo >> 32U), low32(q.hi),
                                   static_cast<std::uint32_t>(q.hi >> 32U)}});
}

// The whole function on guest memory (live) or a DryMemory (call_cost): returns the retired count.
template <class Memory>
std::uint32_t build(Memory& m, const guest::R5900State& cpu) {
    const auto reg = [&cpu](const unsigned index) {
        const n3::Gpr128 g = cpu.read_gpr(index);
        return Q{static_cast<std::uint64_t>(g.words[0]) | (static_cast<std::uint64_t>(g.words[1]) << 32U),
                 static_cast<std::uint64_t>(g.words[2]) | (static_cast<std::uint64_t>(g.words[3]) << 32U)};
    };
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t entry_sp = cpu.read_gpr_u32(29U);

    Q at{}, v1{}, a0 = reg(4), a1 = reg(5), a2 = reg(6), a3 = reg(7);
    Q t0 = reg(8), t1 = reg(9), t2 = reg(10), t3 = reg(11), t4{}, t5{}, t6{}, t7{}, t8{}, t9{};
    Q s0{}, s1{}, s2{}, s3{}, s4{}, s5{}, s6{}, s7{}, fp{};
    Q q144{};

    std::uint32_t retired = 0U;

    // ---- 0x002653b0: byte 1 of the float channel ----
    const std::uint32_t f3 = 0x437f0000U;
    std::uint32_t f0 = 0U;
    std::uint32_t f1 = 0x3f000000U;
    std::uint32_t f2 = 0U;
    const std::uint32_t acc = fpu::add(f0, f1);                 // adda.s
    f2 = m.read_u32_le(low32(a0.lo));                           // lwc1 0($a0)
    f0 = fpu::madd(acc, f3, f2);                                // madd.s
    f0 = static_cast<std::uint32_t>(fpu::to_int(f0));           // cvt.w.s
    v1.lo = sx(f0);                                             // mfc1
    t4.lo = sx(low32(v1.lo));                                   // dsll32 / dsra32
    at.lo = t4.lo < 255U ? 1U : 0U;
    retired += 26U;
    if (at.lo == 0U) {
        t4.lo = 254U;
        retired += 1U;
    }
    // ---- 0x0026541c ----
    v1.lo = t4.lo << 8U;
    a0.lo = t4.lo << 16U;
    v1.lo = t4.lo | v1.lo;
    t5.lo = 255U;
    v1.lo = a0.lo | v1.lo;
    a0.lo = t5.lo - t4.lo;                                      // dsubu
    t4.lo = sx(low32(a0.lo));
    a0.lo = sx(0x47000000U);
    f1 = low32(t4.lo);                                          // mtc1
    f0 = low32(a0.lo);
    f1 = fpu::from_int(static_cast<std::int32_t>(f1));
    f2 = fpu::div(f0, f1);
    f1 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpFloatA));
    f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpFloatB));
    f2 = fpu::mul(f2, f1);
    f0 = fpu::add(f0, f2);
    f0 = static_cast<std::uint32_t>(fpu::to_int(f0));
    t4.lo = sx(f0);
    at.lo = t4.lo < 256U ? 1U : 0U;
    retired += 24U;
    if (at.lo == 0U) {
        t4.lo = t5.lo + 0U;                                     // daddu
        retired += 1U;
    }
    // ---- 0x00265480: unsigned convert of t4 ----
    a0.lo = sx(0x43000000U);
    f1 = low32(a0.lo);
    retired += 4U;
    if (static_cast<std::int64_t>(t4.lo) < 0) {                 // bltz
        t5.lo = sx(low32(t4.lo) >> 1U);
        a0.lo = t4.lo & 1U;
        t5.lo = t5.lo | a0.lo;
        f0 = low32(t5.lo);
        f0 = fpu::from_int(static_cast<std::int32_t>(f0));
        f0 = fpu::add(f0, f0);
        retired += 7U;
    } else {
        f0 = low32(t4.lo);
        f0 = fpu::from_int(static_cast<std::int32_t>(f0));
        retired += 3U;
    }
    // ---- 0x002654b8 ----
    f0 = fpu::div(f1, f0);
    f2 = fpu::mul(f2, f0);
    f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpFloatB));
    f0 = fpu::add(f0, f2);
    f0 = static_cast<std::uint32_t>(fpu::to_int(f0));
    t6.lo = sx(f0);
    at.lo = t6.lo < 256U ? 1U : 0U;
    retired += 12U;
    if (at.lo == 0U) {
        t6.lo = 255U;
        retired += 1U;
    }
    // ---- 0x002654ec ----
    a0.lo = sx(0x43000000U);
    f1 = low32(a0.lo);
    retired += 4U;
    if (static_cast<std::int64_t>(t6.lo) < 0) {
        t5.lo = sx(low32(t6.lo) >> 1U);
        a0.lo = t6.lo & 1U;
        t5.lo = t5.lo | a0.lo;
        f0 = low32(t5.lo);
        f0 = fpu::from_int(static_cast<std::int32_t>(f0));
        f0 = fpu::add(f0, f0);
        retired += 7U;
    } else {
        f0 = low32(t6.lo);
        f0 = fpu::from_int(static_cast<std::int32_t>(f0));
        retired += 3U;
    }
    // ---- 0x00265524 ----
    f0 = fpu::div(f1, f0);
    f1 = fpu::mul(f2, f0);
    f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpFloatB));
    f0 = fpu::add(f0, f1);
    f0 = static_cast<std::uint32_t>(fpu::to_int(f0));
    s1.lo = sx(f0);
    at.lo = s1.lo < 256U ? 1U : 0U;
    retired += 12U;
    if (at.lo == 0U) {
        s1.lo = 255U;
        retired += 1U;
    }

    // ---- 0x00265558: build the fixed 19 quadwords ----
    t1.lo = sx(low32(t1.lo) << 5U);
    a0.lo = t2.lo << 32U;
    a0.lo = a0.lo >> 32U;
    t1.lo = t1.lo << 32U;
    t1.lo = t1.lo >> 32U;
    a0.lo = a0.lo << 14U;
    a0.lo = t1.lo | a0.lo;
    s3.lo = t3.lo << 32U;
    t1.lo = t4.lo << 32U;
    s3.lo = s3.lo >> 32U;
    t1.lo = t1.lo >> 32U;
    t4.lo = sx(0xff000000U);
    t2.lo = t1.lo << 32U;
    s0.lo = t4.lo << 32U;
    t1.lo = t6.lo << 32U;
    t7.lo = sx(m.read_u32_le(gp + static_cast<std::uint32_t>(kGpWritePointer)));
    t1.lo = t1.lo >> 32U;
    t6.lo = 76U;
    t1.lo = t1.lo << 32U;
    t2.lo = t2.lo | 0xa4U;
    s6.lo = t1.lo | 0xa8U;
    fp.lo = 6U;
    t1.lo = sx(0x00020000U);
    t4.lo = t1.lo | s0.lo;
    t1.lo = sx(low32(t3.lo) + 1U);
    t3.lo = sx(low32(t3.lo) << 5U);
    t5.lo = t1.lo << 32U;
    s2 = pcpyld(Q{}, t4);
    s4.lo = t3.lo << 32U;
    t5.lo = t5.lo >> 32U;
    t1 = pcpyld(Q{}, s0);
    t4.lo = m.read_u32_le(entry_sp + 0U);                        // lwu 160($sp)
    t3.lo = s3.lo | s2.lo;
    s5.lo = t5.lo | s2.lo;
    s0 = pcpyld(t6, t3);
    t3.lo = m.read_u32_le(entry_sp + 8U);                        // lwu 168($sp)
    s2.lo = sx(0x01000000U);
    s7 = pcpyld(t6, s5);
    s3.lo = s3.lo | s2.lo;
    t5.lo = t5.lo | s2.lo;
    s2.lo = 78U;
    t6.lo = sx(0x10a30000U);
    t5 = pcpyld(s2, t5);
    q144 = t5;                                                  // sq $t5, 144($sp)
    t9 = pcpyld(s2, s3);
    t3.lo = t3.lo << 16U;
    t5.lo = t6.lo | 0x4000U;
    t3.lo = t4.lo | t3.lo;
    t6.lo = 1U;
    t1.lo = t3.lo | t1.lo;
    t4.lo = t5.lo << 32U;
    t3.lo = 17U;
    s3.lo = 77U;
    t4.lo = t3.lo | t4.lo;
    t5.lo = v1.lo << 32U;
    t3.lo = 14U;
    s4.lo = s4.lo >> 32U;
    t3 = pcpyld(t3, t4);
    sq(m, t7.lo, 0U, t3);
    t4.lo = 42U;
    t3.lo = 66U;
    sq(m, t7.lo, 16U, s0);
    t8 = pcpyld(t3, t2);
    sq(m, t7.lo, 32U, t9);
    t4 = pcpyld(t3, t4);
    t3.lo = sx(0x00030000U);
    sq(m, t7.lo, 48U, t4);
    t2.lo = 71U;
    t4.lo = t3.lo | 0x0003U;
    t2 = pcpyld(t2, t4);
    sq(m, t7.lo, 64U, t2);
    s2.lo = t3.lo | 0x1001U;
    t2 = pcpyld(t6, v1);
    t4.lo = 5U;
    sq(m, t7.lo, 80U, t2);
    t3.lo = sx(low32(a2.lo) << 4U);
    v1 = pcpyld(t4, t5);
    t2.lo = t3.lo << 32U;
    sq(m, t7.lo, 96U, v1);
    t2.lo = t2.lo >> 32U;
    v1.lo = t2.lo << 16U;
    s3 = pcpyld(s3, t1);
    v1.lo = v1.lo | 0x0200U;
    t1.lo = t5.lo | v1.lo;
    s5 = pcpyld(t4, t1);
    sq(m, t7.lo, 112U, s5);
    v1.lo = fp.lo << 32U;
    s5.lo = 0xa800U;
    sq(m, t7.lo, 128U, s3);
    s3.lo = s5.lo << 16U;
    s5.lo = s3.lo | v1.lo;
    s3.lo = s3.lo | 0x8000U;
    a0.lo = a0.lo | s5.lo;
    v1.lo = s3.lo | v1.lo;
    a0 = pcpyld(fp, a0);
    sq(m, t7.lo, 144U, a0);
    v1.lo = s4.lo | v1.lo;
    s3.lo = 7U;
    a0.lo = 20U;
    v1 = pcpyld(s3, v1);
    sq(m, t7.lo, 160U, v1);
    s3.lo = 97U;
    v1 = pcpyld(a0, t6);
    sq(m, t7.lo, 176U, v1);
    a0.lo = 21U;
    v1.lo = 72U;
    a0 = pcpyld(a0, s3);
    v1 = pcpyld(v1, s2);
    sq(m, t7.lo, 192U, a0);
    sq(m, t7.lo, 208U, v1);
    v1.lo = a3.lo << 32U;
    v1.lo = v1.lo >> 32U;
    a0.lo = v1.lo << 4U;
    v1.lo = sx(low32(a3.lo) + low32(a1.lo));
    a0.lo = a0.lo | 0x000aU;
    v1.lo = sx(low32(v1.lo) - 1U);
    s2.lo = v1.lo << 32U;
    v1.lo = sx(low32(a1.lo) >> 6U);
    s2.lo = s2.lo >> 32U;
    a1.lo = sx(low32(a3.lo) << 4U);
    s2.lo = s2.lo << 14U;
    a3.lo = t0.lo << 32U;
    a0.lo = a0.lo | s2.lo;
    a3.lo = a3.lo >> 32U;
    a1.lo = sx(low32(a1.lo) + 8U);
    s2.lo = a3.lo << 24U;
    a1.lo = a1.lo << 32U;
    a3.lo = sx(low32(t0.lo) + low32(a2.lo));
    a0.lo = s2.lo | a0.lo;
    a3.lo = sx(low32(a3.lo) - 1U);
    at.lo = (0U < v1.lo) ? 1U : 0U;
    s2.lo = a3.lo << 32U;
    a1.lo = a1.lo >> 32U;
    a3.lo = sx(low32(t0.lo) << 4U);
    s2.lo = s2.lo >> 32U;
    t0.lo = s2.lo << 34U;                                       // dsll32 $t0, $s2, 2
    a3.lo = sx(low32(a3.lo) + 8U);
    s3.lo = a3.lo << 32U;
    t0.lo = a0.lo | t0.lo;
    a0.lo = 8U;
    s3.lo = s3.lo >> 32U;
    a0 = pcpyld(a0, t0);
    sq(m, t7.lo, 224U, a0);
    t2.lo = s3.lo + t2.lo;                                      // daddu
    a0.lo = 9U;
    t0.lo = sx(low32(t7.lo) + 288U);
    a0 = pcpyld(a0, t4);
    sq(m, t7.lo, 240U, a0);
    a3.lo = 0U;
    a0.lo = 67U;
    sq(m, t7.lo, 256U, t8);
    a0 = pcpyld(a0, s6);
    sq(m, t7.lo, 272U, a0);
    a0.lo = 0U;                                                 // delay slot of the beq below
    retired += 155U;

    if (at.lo != 0U) {
        // ---- 0x002657c4: constants for the tile loop ----
        s2 = pcpyld(t1, t1);
        t1.lo = sx(low32(t3.lo) + 16U);
        a2.lo = sx(low32(a2.lo) << 3U);
        t1.lo = t1.lo << 32U;
        a2.lo = a2.lo << 32U;
        t4.lo = t2.lo << 16U;
        t1.lo = t1.lo >> 32U;
        t2.lo = t1.lo << 16U;
        a2.lo = a2.lo >> 32U;
        t1.lo = a2.lo << 16U;
        s1.lo = s1.lo << 32U;
        a2.lo = sx(0x10ab0000U);
        s1.lo = s1.lo >> 32U;
        t3.lo = a2.lo | 0x4000U;
        t7.lo = s3.lo << 16U;
        s3.lo = t3.lo << 32U;
        a2.lo = 2U;
        t3.lo = sx(0xe4000000U);
        a2.lo = a2.lo | s3.lo;
        t3.lo = t3.lo << 32U;
        s5.lo = s1.lo << 8U;
        s3.lo = t6.lo | t3.lo;
        fp.lo = 16U;
        t3.lo = 14U;
        t6.lo = sx(0x00530000U);
        s4 = pcpyld(t3, a2);
        t6.lo = t6.lo | 0x5310U;
        t3.lo = sx(0x53550000U);
        t6.lo = t6.lo << 32U;
        t3.lo = t3.lo | 0x3531U;
        t6.lo = t3.lo | t6.lo;
        t8.lo = 1040U;
        a2 = pcpyld(Q{}, s3);
        s3 = pcpyld(t6, a2);
        a2.lo = s1.lo << 16U;
        t3.lo = sx(0x00800000U);
        s1.lo = s1.lo | s5.lo;
        t6.lo = t3.lo | 0x8080U;
        a2.lo = a2.lo | s1.lo;
        t3.lo = t5.lo | 0x0400U;
        s1.lo = 854U;
        s1 = pcpyld(a2, s1);
        retired += 43U;
    }
    // ---- 0x0026590c: the tile loop (v1 tiles) ----
    if (at.lo != 0U) {
        while (true) {
            a2.lo = (a3.lo < v1.lo) ? 1U : 0U;
            retired += 3U;
            if (a2.lo == 0U) {
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            // 0x00265870
            a2.lo = a3.lo & 1U;
            sq(m, t0.lo, 0U, s4);                                   // delay slot of the beq
            if (a2.lo != 0U) {
                sq(m, t0.lo, 16U, s7);
                s5.lo = t8.lo + 0U;
                sq(m, t0.lo, 32U, t9);
                t0.lo = sx(low32(t0.lo) + 48U);
            } else {
                a2.lo = q144.lo;                                    // lq $a2, 144($sp)
                const Q a2_quad = q144;
                s5.lo = fp.lo + 0U;
                sq(m, t0.lo, 16U, s0);
                sq(m, t0.lo, 32U, a2_quad);
                t0.lo = sx(low32(t0.lo) + 48U);
                a2.hi = a2_quad.hi;
            }
            retired += 3U + 5U;
            // 0x002658a4
            a2.lo = a1.lo | t7.lo;
            sq(m, t0.lo, 0U, s3);
            a2 = pcpyld(a2, t6);
            sq(m, t0.lo, 16U, a2);
            s6.lo = s5.lo + 1024U;
            a2.lo = a1.lo + 512U;
            a3.lo = sx(low32(a3.lo) + 1U);
            a2.lo = a2.lo | t4.lo;
            a1.lo = a1.lo + 1024U;
            a2 = pcpyld(a2, t5);
            sq(m, t0.lo, 32U, a2);
            a2.lo = a1.lo | t7.lo;
            sq(m, t0.lo, 48U, s2);
            a2 = pcpyld(t3, a2);
            sq(m, t0.lo, 64U, a2);
            a2.lo = s5.lo | t2.lo;
            sq(m, t0.lo, 80U, s1);
            s5.lo = sx(0x00100000U);
            s5.lo = s6.lo | s5.lo;
            s6.lo = a0.lo | t1.lo;
            a2 = pcpyld(s6, a2);
            sq(m, t0.lo, 96U, a2);
            a0.lo = a0.lo + 512U;
            a2 = pcpyld(a0, s5);
            sq(m, t0.lo, 112U, a2);
            t0.lo = sx(low32(t0.lo) + 128U);
            retired += 26U;
        }
    }
    // ---- 0x00265918 ----
    m.write_u32_le(gp + static_cast<std::uint32_t>(kGpWritePointer), low32(t0.lo));
    retired += 12U;
    (void)s6;
    return retired;
}

template <class Context>
n3::Run build_gif_tile_packet(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired = build(call.memory(), call.cpu());
    if (retired > kCeiling) {
        return call.decline();
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002653b0, 1432U, 0x1145c886267c80e9ULL, "BuildGifTilePacket", kCeiling, build_gif_tile_packet,
         EngineReturn::Nothing)
