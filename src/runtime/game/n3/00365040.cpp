// 0x00365040 AppendVertexBatch(src, N): appends a display-list packet that uploads N vertices to VU1 memory at
// address 0, three quadwords per vertex, then the VU1 kick (ITOP N, MSCNT). a0 = src: N records of 32 bytes (position
// xyzw floats, s and t floats, the colour bytes, a pad word). a1 = N, used as a 32-bit count; N <= 0 still makes one
// pass. The packet is written at CUR, the cursor that ReservePacketBuffer makes room for, and CUR moves past it last.
// FLAG_A (gp-22604) selects the layout: non-zero converts s and t to 4.12 fixed point (vftoi12.xy, which leaves VF1
// holding the last converted quadword) and keeps the colour bytes widened; zero leaves the second quadword of each
// vertex unwritten and writes the colour doubled and clamped. FLAG_B (gp-22600) is copied into the trailer.
//
// Packet at c = CUR: c+0 DMA tag (3N+1 quadwords follow), c+16 STMOD / STCYCL 3,3 / UNPACK V4-32 of 3N quadwords at
// VU1 address 0, c+32+48i the three quadwords of vertex i. The trailer at a = c+32+48N' (N' = max(N, 1)) holds an empty
// DMA tag with FLUSHE, a 2-quadword DMA tag with STCYCL 4,4 and UNPACK V4-32 at 0x3f4, the GIF tag (PRIM from FLAG_A,
// three registers ST RGBAQ XYZF2), FLAG_B, and the tag with ITOP N and MSCNT. CUR is then a+80.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;

// $gp-relative globals: the address is $gp - offset.
constexpr std::uint32_t kCursorBelowGp = 24240U;        // CUR: write cursor into the packet buffer
constexpr std::uint32_t kTexturedBelowGp = 22604U;      // FLAG_A: non-zero = textured
constexpr std::uint32_t kTrailerFlagBelowGp = 22600U;   // FLAG_B: the trailer's GIF data

constexpr std::uint32_t kReserveFrameBytes = 64U;       // the original's `addiu $sp, $sp, -64`
constexpr std::uint32_t kReserveFirstArg = 0U;          // a0 of ReservePacketBuffer: always 0 here
// a1 of ReservePacketBuffer is 3N + 7: the 3N vertex quadwords, the header's 2 and the trailer's 5.
constexpr std::uint32_t kReserveExtraQwords = 7U;

// Source vertex: the position quadword, the s and t floats (the quadword's x and y lanes), then the colour bytes
// (the z lane of that quadword) and a pad word.
constexpr GuestAddress kVertexStride = 32U;
constexpr GuestAddress kPositionOffset = 0U;
constexpr GuestAddress kStOffset = 16U;
constexpr GuestAddress kColourOffset = 24U;

// Packet: the DMA tag and the header quadword, then three quadwords per vertex.
constexpr GuestAddress kFirstGroupOffset = 32U;
constexpr GuestAddress kGroupStride = 48U;
constexpr GuestAddress kTrailerSpan = 80U;             // the trailer and the CUR advance past it

// Retired original instructions from the listing, delay slots included. The reserve call's count is added to the
// prefix. Textured: 88 + 14 N'; untextured: 91 + 13 N' (N' passes).
constexpr std::uint32_t kPrefixCost = 38U;             // entry to the FLAG_A test, the jal and its delay slot included
constexpr std::uint32_t kTexturedPassCost = 14U;       // 0x003650e0-0x00365114
constexpr std::uint32_t kTexturedTailCost = 2U + 2U + 46U;  // 0x003650d8-0x003650dc, 0x00365118-0x0036511c, the trailer
constexpr std::uint32_t kPlainPassCost = 13U;          // 0x00365138-0x00365168
constexpr std::uint32_t kPlainTailCost = 6U + 1U + 46U;    // 0x00365120-0x00365134, 0x0036516c, the trailer

constexpr std::uint32_t kCeiling = 20000U;

// The four colour bytes of a vertex as four words (pextlb and pextlh with zero).
Gpr128 colour_words(const std::uint32_t colour) {
    return Gpr128{{colour & 0xffU, (colour >> 8U) & 0xffU, (colour >> 16U) & 0xffU, colour >> 24U}};
}

// The untextured colour: each word doubled (paddw), then a signed minimum with {255, 255, 255, 128} (pminw).
Gpr128 doubled_colour(const std::uint32_t colour) {
    const Gpr128 bytes = colour_words(colour);
    const auto clamp = [](const std::uint32_t word, const std::int32_t limit) {
        return static_cast<std::uint32_t>(std::min(static_cast<std::int32_t>(word + word), limit));
    };
    return Gpr128{{clamp(bytes.words[0], 0xff), clamp(bytes.words[1], 0xff), clamp(bytes.words[2], 0xff),
                   clamp(bytes.words[3], 0x80)}};
}

// The DMA tag (qwc 3N+1, sign-extended to 64 bits as the original's dsra32 leaves it) and the STMOD / STCYCL /
// UNPACK header quadword.
template <class Memory>
void write_header(Memory& m, const GuestAddress packet, const std::uint32_t qwords) {
    const std::uint32_t tag = (qwords + 1U) | 0x10000000U;
    const std::uint32_t tag_high = (tag & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    n3::store_quad_any(m, packet, Gpr128{{tag, tag_high, 0U, 0U}});
    n3::store_quad_any(m, packet + 16U, Gpr128{{0U, 0x05000000U, 0x01000303U, (qwords << 16U) | 0x6c008000U}});
}

// A textured vertex (14 retired): the position quadword, the ST quadword with its x and y lanes converted to 4.12 fixed
// point (vftoi12.xy), and the colour bytes widened. Returns the converted ST quadword, which VF1 holds afterwards.
template <class Memory>
Gpr128 textured_vertex(Memory& m, const GuestAddress vertex, const GuestAddress group) {
    const std::uint32_t colour = m.read_u32_le(vertex + kColourOffset);
    const Gpr128 st = n3::load_quad_any(m, vertex + kStOffset);
    const Gpr128 position = n3::load_quad_any(m, vertex + kPositionOffset);
    Gpr128 converted = st;
    converted.words[0] = guest::convert_vu0_macro_float_to_integer_bits(st.words[0], 12U);
    converted.words[1] = guest::convert_vu0_macro_float_to_integer_bits(st.words[1], 12U);
    n3::store_quad_any(m, group, position);
    n3::store_quad_any(m, group + 16U, converted);
    n3::store_quad_any(m, group + 32U, colour_words(colour));
    return converted;
}

// An untextured vertex (13 retired): the position quadword and the doubled, clamped colour. The second quadword of
// the group is left alone.
template <class Memory>
void plain_vertex(Memory& m, const GuestAddress vertex, const GuestAddress group) {
    const std::uint32_t colour = m.read_u32_le(vertex + kColourOffset);
    const Gpr128 position = n3::load_quad_any(m, vertex + kPositionOffset);
    n3::store_quad_any(m, group, position);
    n3::store_quad_any(m, group + 32U, doubled_colour(colour));
}

// The packet's end: an empty DMA tag with FLUSHE, the two-quadword UNPACK tail at VU1 address 0x3f4, the GIF tag
// (PRIM from FLAG_A, NREG 3), FLAG_B, and the VU1 kick (ITOP N, MSCNT).
template <class Memory>
void write_trailer(Memory& m, const GuestAddress trailer, const std::uint32_t flag_a, const std::uint32_t flag_b,
                   const std::uint32_t count) {
    const std::uint32_t prim = (((flag_a << 4U) | 0x4cU) << 15U) | 0x30004000U;
    n3::store_quad_any(m, trailer, Gpr128{{0x10000000U, 0U, 0x10000000U, 0U}});
    n3::store_quad_any(m, trailer + 16U, Gpr128{{0x10000002U, 0U, 0x01000404U, 0x6c0203f4U}});
    n3::store_quad_any(m, trailer + 32U, Gpr128{{0U, prim, 0x412U, 0U}});
    n3::store_quad_any(m, trailer + 48U, Gpr128{{0U, flag_b, 0U, 0U}});
    n3::store_quad_any(m, trailer + 64U, Gpr128{{0x10000000U, 0U, 0x04000000U | count, 0x17000000U}});
}

template <class Context>
n3::Run append_vertex_batch(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress source = call.arg(0);
    const std::uint32_t count = call.arg(1);
    const std::uint32_t qwords = 3U * count;

    call.set_arg(0, kReserveFirstArg);
    call.set_arg(1, qwords + kReserveExtraQwords);
    const n3::Run reserved = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kReserveFrameBytes);
    if (reserved.declined) {
        return call.decline();
    }

    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress packet = m.read_u32_le(gp - kCursorBelowGp);
    const bool textured = m.read_u8(gp - kTexturedBelowGp) != 0U;
    write_header(m, packet, qwords);

    const std::uint32_t before_loop = kPrefixCost + reserved.retired;
    const std::uint32_t pass_cost = textured ? kTexturedPassCost : kPlainPassCost;
    GuestAddress vertex = source;
    GuestAddress group = packet + kFirstGroupOffset;
    std::uint32_t passes = 0U;
    std::int32_t remaining = static_cast<std::int32_t>(count);
    Gpr128 last_st{};
    do {
        const std::uint32_t retired = before_loop + pass_cost * passes;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (textured) {
            last_st = textured_vertex(m, vertex, group);
        } else {
            plain_vertex(m, vertex, group);
        }
        vertex += kVertexStride;
        group += kGroupStride;
        ++passes;
        remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(remaining) - 1U);
    } while (remaining > 0);

    if (textured) {
        call.cpu().write_vu0_vector(1U, last_st);
    }
    const GuestAddress trailer = group;
    const std::uint32_t flag_a = m.read_u8(gp - kTexturedBelowGp);
    const std::uint32_t flag_b = m.read_u8(gp - kTrailerFlagBelowGp);
    write_trailer(m, trailer, flag_a, flag_b, count);
    m.write_u32_le(gp - kCursorBelowGp, trailer + kTrailerSpan);

    const std::uint32_t tail_cost = textured ? kTexturedTailCost : kPlainTailCost;
    return call.finish(before_loop + pass_cost * passes + tail_cost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00365040, 488U, 0xed141175eb4b22d1ULL, "AppendVertexBatch", kCeiling, append_vertex_batch,
         EngineReturn::Nothing)
