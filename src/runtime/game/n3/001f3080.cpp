// 0x001f3080 ReservePacketBuffer (a0 = request flags F, a1 = qword count N): makes room in the display-list packet
// buffer for N + 4 quadwords (one more when F asks for an open DMA chain and none is open yet) and keeps the chain
// bookkeeping consistent. v0 = 1 reserved, 0 no room. All state lives in gp-relative globals:
//
//   STATE  gp-24132  0 = no buffer yet; else bit 31 = a DMA chain packet is open, low 31 bits must be 0x1101
//   TAG    gp-24136  address of the open chain's pending DMA tag quadword (0 = none)
//   POS    gp-24208  write position in the buffer
//   BASE   gp-24212  start of the segment not yet sent to the DMA channel
//   CUR    gp-24240  start of the current segment (0 = none yet)
//   LIMIT  gp-24160  end of the buffer; LAST gp-24156 end of the reservation (written here); LIMIT2 gp-24176 the
//                    hard limit of the retry
//   HDR    gp-24148 / HDR_END gp-24152  16-byte buffer-header block (HDR[0] = 127, HDR[1] = next header)
//   gp-24144  start of the first packet written at buffer set-up
//
// Native paths (all plain RAM reads and writes): the fits-and-nothing-to-change exit, closing an open chain (writing
// its count tag), opening a new one, buffer set-up when STATE is 0 and the "no room" return of 0. Everything that
// needs the original declines before any store: a wrong STATE, a request that does not fit (jal 0x001f1f50), the DMA
// channel kicks (device registers 0x1000d000.., two copies of the same block) and the retry call (jal 0x001f2408).
//
// The tests on a0 are 64-bit (lui sign-extends the 0x80000000 mask), so the flags are kept as uint64_t.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kBelowState = 24132U;
constexpr std::uint32_t kBelowTag = 24136U;
constexpr std::uint32_t kBelowPos = 24208U;
constexpr std::uint32_t kBelowBase = 24212U;
constexpr std::uint32_t kBelowCur = 24240U;
constexpr std::uint32_t kBelowLimit = 24160U;
constexpr std::uint32_t kBelowLast = 24156U;
constexpr std::uint32_t kBelowLimit2 = 24176U;
constexpr std::uint32_t kBelowHdrEnd = 24152U;
constexpr std::uint32_t kBelowHdr = 24148U;
constexpr std::uint32_t kBelowFirstPacket = 24144U;

constexpr std::uint32_t kChainOpen = 0x80000000U;
constexpr std::uint32_t kStateReady = 0x1101U;
constexpr std::uint32_t kStateMask = 0x7fffffffU;
constexpr std::uint64_t kHighBits = 0xffffffff80000000ULL;   // lui $x, 0x8000 (sign-extended)
constexpr std::uint32_t kFixedQwords = 4U;
constexpr std::uint32_t kMaxSegmentBytes = 8192U;
constexpr std::uint32_t kUncached = 0x20000000U;
constexpr std::uint32_t kTagCountId = 0x10000000U;
constexpr std::uint32_t kTagCountEnd = 0x50000000U;
constexpr std::uint32_t kHeaderMarker = 127U;
constexpr std::uint32_t kHeaderGap = 128U;
constexpr std::uint32_t kHeaderUsed = 120U;
constexpr std::uint32_t kFailSlack = 192U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions per block (brief.py, delay slots included).
constexpr std::uint32_t kEntry = 7U + 9U;              // 0x3080-0x30bc
constexpr std::uint32_t kStateAndRoom = 6U + 10U;      // 0x30c0, 0x30d8
constexpr std::uint32_t kSegmentTest = 4U;             // 0x3100
constexpr std::uint32_t kSegmentSkip = 4U;             // 0x3110 with nothing to send
constexpr std::uint32_t kFitted = 11U;                 // 0x3254
constexpr std::uint32_t kReturn = 5U;                  // 0x35b0
constexpr std::uint32_t kChainTest = 5U;               // 0x3280
constexpr std::uint32_t kCloseEmpty = 7U + 2U + 6U;    // 0x3294, 0x32e0, 0x32e8
constexpr std::uint32_t kCloseTag = 7U + 11U + 6U;     // 0x3294, 0x32b0, 0x32e8
constexpr std::uint32_t kOpenTest = 7U;                // 0x3300
constexpr std::uint32_t kOpenNoSegment = 2U;           // 0x331c with CUR == 0
constexpr std::uint32_t kOpenSegment = 2U + 3U;        // 0x331c, 0x3324
constexpr std::uint32_t kOpenWrite = 2U + 9U;          // 0x3330, 0x3468
constexpr std::uint32_t kOk = 2U;                      // 0x3578
constexpr std::uint32_t kInitHeaderKept = 6U;          // 0x34a8, header still has room
constexpr std::uint32_t kInitHeaderNew = 6U + 10U + 8U;
constexpr std::uint32_t kInitHeaderFail = 6U + 10U + 1U;
constexpr std::uint32_t kInitEmpty = 3U;               // 0x350c
constexpr std::uint32_t kInitPlace = 9U + 8U;          // 0x3518 (not taken), 0x3540
constexpr std::uint32_t kInitChain = 6U;               // 0x3560
constexpr std::uint32_t kInitTooBig = 10U;             // 0x3518 with the bnel taken
constexpr std::uint32_t kFailEntry = 1U;               // 0x3580
constexpr std::uint32_t kFail = 7U;                    // 0x3584

struct Globals {
    GuestAddress state, tag, pos, base, cur, limit, last, limit2, hdr_end, hdr, first_packet;
};

constexpr Globals globals_at(const GuestAddress gp) {
    return {gp - kBelowState, gp - kBelowTag,   gp - kBelowPos,    gp - kBelowBase, gp - kBelowCur,
            gp - kBelowLimit, gp - kBelowLast,  gp - kBelowLimit2, gp - kBelowHdrEnd, gp - kBelowHdr,
            gp - kBelowFirstPacket};
}

// Set-up of the buffer (STATE == 0): finds or makes a header block, places the first packet and opens the state.
// `qwords` is the request including the fixed part and the chain tag (s0 of the original).
template <class Context>
n3::Run set_up_buffer(Context& context, const Globals& g, const std::uint64_t flags, const std::uint32_t qwords) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t hdr = m.read_u32_le(g.hdr);
    const std::uint32_t hdr_end = m.read_u32_le(g.hdr_end);
    const std::uint32_t limit = m.read_u32_le(g.limit);
    std::uint32_t start = m.read_u32_le(g.last);   // where the first packet may begin (0 = no header room)
    std::uint32_t retired = kEntry;
    bool new_header = false;
    std::uint32_t header_next = 0U;

    if (hdr < hdr_end - 8U) {
        retired += kInitHeaderKept;
    } else {
        header_next = (start + 63U) & ~63U;
        const std::uint32_t after = header_next + kHeaderGap;
        if (after < limit) {
            new_header = true;
            start = after;
            retired += kInitHeaderNew;
        } else {
            start = 0U;
            retired += kInitHeaderFail;
        }
    }

    // Any non-zero flag word (not just bit 31) asks for one more quadword here.
    const bool any_flag = flags != 0U;
    const std::uint32_t placed_qwords = qwords + (any_flag ? 1U : 0U);
    retired += kInitEmpty;

    std::uint32_t place = 0U;
    std::uint32_t end = 0U;
    bool reserved = false;
    if (start == 0U) {
        retired += kFailEntry;
    } else {
        place = (start + 127U) & ~127U;
        end = place + (placed_qwords << 4);
        if (limit < end) {
            retired += kInitTooBig;
        } else {
            reserved = true;
            retired += kInitPlace;
        }
    }

    if (!reserved) {
        // No room: past the hard limit the call reports 0; otherwise the original grows the buffer (jal 0x001f2408).
        if (!(m.read_u32_le(g.limit2) < (qwords << 4) + kFailSlack)) {
            return call.decline();
        }
        if (new_header) {
            m.write_u32_le(hdr + 4U, header_next);
            m.write_u32_le(g.hdr, header_next);
            m.write_u32_le(g.hdr_end, header_next + kHeaderUsed);
            m.write_u32_le(hdr, kHeaderMarker);
            m.write_u32_le(g.last, start);
        }
        call.return_value(0U);
        return call.finish(retired + kFail + kReturn);
    }

    if (new_header) {
        m.write_u32_le(hdr + 4U, header_next);
        m.write_u32_le(g.hdr, header_next);
        m.write_u32_le(g.hdr_end, header_next + kHeaderUsed);
        m.write_u32_le(hdr, kHeaderMarker);
    }
    m.write_u32_le(g.last, end);
    m.write_u32_le(g.cur, m.read_u32_le(g.base));
    m.write_u32_le(g.first_packet, place);
    if (flags == 0U) {
        m.write_u32_le(g.state, kStateReady);
        m.write_u32_le(g.pos, place);
    } else {
        m.write_u32_le(g.state, kChainOpen | kStateReady);
        m.write_u32_le(g.pos, place + 16U);
        m.write_u32_le(g.tag, place);
        retired += kInitChain;
    }
    call.return_value(1U);
    return call.finish(retired + kOk + kReturn);
}

template <class Context>
n3::Run reserve_packet_buffer(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const Globals g = globals_at(call.cpu().read_gpr_u32(n3::kGlobalPointer));
    const std::uint64_t flags = call.arg64(0);
    const bool wants_chain = (flags & kHighBits) != 0U;
    const std::uint32_t state = m.read_u32_le(g.state);

    // One more quadword for the DMA tag when a chain is asked for and none is open.
    const bool needs_tag = wants_chain && (state & kChainOpen) == 0U;
    const std::uint32_t qwords = call.arg(1) + kFixedQwords + (needs_tag ? 1U : 0U);
    if (state == 0U) {
        return set_up_buffer(context, g, flags, qwords);
    }
    if ((state & kStateMask) != kStateReady) {
        return call.decline();   // jal 0x001f1f50
    }

    const std::uint32_t pos = m.read_u32_le(g.pos);
    const std::uint32_t base = m.read_u32_le(g.base);
    const std::uint32_t cur = m.read_u32_le(g.cur);
    const std::uint32_t used = cur - base;
    const std::uint32_t bytes = qwords << 4;
    if (!(pos + used + bytes < m.read_u32_le(g.limit))) {
        return call.decline();   // jal 0x001f1f50
    }
    std::uint32_t retired = kEntry + kStateAndRoom + kSegmentTest;
    if (!(used + bytes < kMaxSegmentBytes + 1U)) {
        // A full segment still holds data: the original starts the DMA transfer (device registers).
        if (cur != 0U && used != 0U) {
            return call.decline();
        }
        retired += kSegmentSkip;
    }
    retired += kFitted;

    const std::uint32_t last = pos + bytes + cur - base;
    const std::int64_t state64 = static_cast<std::int64_t>(static_cast<std::int32_t>(state));
    const std::uint64_t state_high = static_cast<std::uint64_t>(state64) & kHighBits;
    if (state_high == flags) {
        m.write_u32_le(g.last, last);
        call.return_value(1U);
        return call.finish(retired + kReturn);
    }
    retired += kChainTest;

    // Close the open chain when the request does not want one.
    const bool close = (static_cast<std::uint64_t>(state64) & ~flags & kHighBits) != 0U;
    const std::uint32_t state_after_close = close ? (state & kStateMask) : state;
    // Open a new one when it does and none is open now.
    const bool open = wants_chain && (state_after_close & kChainOpen) == 0U;
    retired += kOpenTest;
    if (open) {
        const std::uint32_t segment = cur != 0U ? used : 0U;
        if (segment != 0U) {
            return call.decline();   // second DMA kick
        }
        retired += (cur != 0U ? kOpenSegment : kOpenNoSegment) + kOpenWrite;
    }
    std::uint32_t tag = 0U;
    std::uint32_t cnt = 0U;
    if (close) {
        tag = m.read_u32_le(g.tag);
        cnt = ((pos - tag + used) >> 4) - 1U;
        retired += cnt == 0U ? kCloseEmpty : kCloseTag;
    }
    retired += kOk + kReturn;

    m.write_u32_le(g.last, last);
    std::uint32_t pos_now = pos;
    if (close) {
        if (cnt == 0U) {
            pos_now = pos - 16U;
            m.write_u32_le(g.pos, pos_now);
        } else {
            // The chain's count tag, written through the uncached alias: QWC with ID/END in lanes 0 and 3.
            n3::store_quad_any(m, tag | kUncached, guest::Gpr128{{cnt | kTagCountId, 0U, 0U, cnt | kTagCountEnd}});
        }
        m.write_u32_le(g.tag, 0U);
        m.write_u32_le(g.state, state_after_close);
    }
    if (open) {
        m.write_u32_le(g.tag, pos_now);
        m.write_u32_le(g.pos, pos_now + 16U);
        m.write_u32_le(g.state, state_after_close | kChainOpen);
    }
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001f3080, 1348U, 0xad3943e27d465245ULL, "ReservePacketBuffer", kCeiling, reserve_packet_buffer,
         EngineReturn::Integer)
