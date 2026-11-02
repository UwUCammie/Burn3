// 0x0014ca20 AdvanceRoadQuad (a0 = P, a road follower): keeps track of which quad of the road strip chain the
// follower's owner stands on.
//
//   P+4    owner object (the car; its road handle is at +6324 and its quad at +6328, its position at +48)
//   P+468  u32 current strip handle H (0x00522660 + 8 * strip + 44; u32[H + 4] is the strip record)
//   P+472  u16 current quad index (0xffff = none)
//   P+656  signed byte, slot state (cleared when the follower is re-seated)
//
// Strip record: +0 u32 corner table (4 u16 per quad), +8 u32 quad records (10 bytes each), +12 u16 quad count,
// +14 u8 closed flag. A quad record E holds two links to a neighbouring strip: E[4] = strip A, u16 E[6] = its quad,
// E[5] = strip B, u16 E[8] = its quad.
//
// Phases: (1) when the follower has no quad yet, seed it from the owner's own handle/quad, or pick one of the two
// linked strips (the side the owner's position lies on when both are open); (2) when SegmentRangeFlags finds
// something ahead, follow the links of the current quad (chain A, then chain B) and re-seat the follower on the
// first strip where it reports nothing; (3) classify the owner's position against the current quad and step the
// quad one forward or back, wrapping a closed strip or crossing to a linked strip at the end of an open one.
// Always returns 1.
//
// The listing never sets $s4 before the "no link found yet" tests at 0x14d030/0x14d15c: they read the caller's $s4
// (a callee-saved register, so its entry value), which decides whether a first failing link probe is retried
// through link B. It is read once from the CPU here. The four calls of the no-strip fallback 0x00148a80
// (0x14ccb0, 0x14ccd0, 0x14d10c, 0x14d238) are not native: the call is declined there.
// Retired counts below are the blocks of the listing (brief.py), delay slots included; callees add their own.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00148c20)
N3_CALLEE(00148ca0)
N3_CALLEE(00147e90)
N3_CALLEE(0014a560)
N3_CALLEE(00148440)
N3_CALLEE(0014d380)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 240U;
constexpr std::uint32_t kPosSlot = 224U;          // sq $v0,224($sp): the owner position, read by the classifier
constexpr std::uint32_t kStepSlot = 208U;         // a2 of StripQuadStepCross
constexpr GuestAddress kVertices = 0x0064d354U;   // u32 pointer to the 16-byte vertices
constexpr GuestAddress kStripTable = 0x00522660U; // handle(k) = kStripTable + 8 * k + 44
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kNoQuad = 0xffffU;
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint8_t kMaskX = 8U;

// P fields
constexpr GuestAddress kOwner = 4U;
constexpr GuestAddress kStripHandle = 468U;
constexpr GuestAddress kQuad = 472U;
constexpr GuestAddress kSlot = 656U;
// owner fields
constexpr GuestAddress kOwnerHandle = 6324U;
constexpr GuestAddress kOwnerQuad = 6328U;
constexpr GuestAddress kOwnerPos = 48U;

// Block counts (address of the block in the name).
constexpr std::uint32_t kC20 = 22U;      // entry up to the quad test
constexpr std::uint32_t kCA78 = 4U;      // owner handle test
constexpr std::uint32_t kCA88 = 6U;      // up to the ScanForFlaggedEntry call and its test
constexpr std::uint32_t kCAA0 = 7U;      // seeded from the owner
constexpr std::uint32_t kCABC = 24U;     // record read and both RouteOpenAhead calls
constexpr std::uint32_t kCC48 = 2U;      // ra set, test rb
constexpr std::uint32_t kCB1C = 2U;
constexpr std::uint32_t kCB24 = 2U;
constexpr std::uint32_t kCB2C = 2U;
constexpr std::uint32_t kCC74 = 2U;      // ra clear, test rb
constexpr std::uint32_t kCB34 = 51U;     // side test
constexpr std::uint32_t kChoose = 9U;    // set P+468/472 from a link, jump to 0x14ccd8
constexpr std::uint32_t kCCD8 = 1U;
constexpr std::uint32_t kCCDC = 7U;      // SegmentRangeFlags call and its test
constexpr std::uint32_t kCCF8 = 11U;     // chain A set-up
constexpr std::uint32_t kLoopHead = 6U;  // cd24 / cebc: RouteOpenAhead(.., 1) and its test
constexpr std::uint32_t kSpanTest = 6U;  // cd3c / ced4: span and bltz
constexpr std::uint32_t kSpanWrap = 9U;  // ce3c / cf78
constexpr std::uint32_t kSpanScan = 6U;  // cd54 / ceec: RouteOpenAhead(.., span) and its test
constexpr std::uint32_t kFlagsCall = 13U; // cd6c / cf04
constexpr std::uint32_t kNoFlags = 1U;   // cda0 / cf38
constexpr std::uint32_t kFlagsTest = 2U; // cda4 / cf3c
constexpr std::uint32_t kStepLink = 13U; // cdac / cf44
constexpr std::uint32_t kReseatA = 11U;  // ce60
constexpr std::uint32_t kReseatB = 12U;  // cf9c (continues at 0x14cde4)
constexpr std::uint32_t kAfterA = 2U;    // ce8c
constexpr std::uint32_t kChainB = 10U;   // ce94
constexpr std::uint32_t kCDE0 = 1U;
constexpr std::uint32_t kCDE4 = 6U;      // classifier call and test of bit 2
constexpr std::uint32_t kCDFC = 3U;      // test of bit 3
constexpr std::uint32_t kExit = 12U;     // ce34 (2) + epilogue d254 (10)
constexpr std::uint32_t kStartEdge = 3U; // ce08: quad == 0 test
constexpr std::uint32_t kStepBack = 3U;  // d248
constexpr std::uint32_t kStartStrip = 5U; // ce14
constexpr std::uint32_t kWrapBack = 3U;  // ce28
constexpr std::uint32_t kEndEdge = 7U;   // cfcc
constexpr std::uint32_t kStepOn = 3U;    // d11c
constexpr std::uint32_t kClosedTest = 3U; // cfe8
constexpr std::uint32_t kWrapStart = 2U; // cff4
constexpr std::uint32_t kLinkProbe = 13U; // cffc / d128: record read, RouteOpenAhead, test
constexpr std::uint32_t kLinkTaken = 10U; // d050 / d17c / d0b8 / d1e4
constexpr std::uint32_t kLinkRetry = 16U; // d078 / d1a4: second probe
constexpr std::uint32_t kLinkTest = 2U;  // d030, d038 / d15c, d164
constexpr std::uint32_t kReseatEnd = 4U; // d040 / d16c, then the epilogue
constexpr std::uint32_t kEpilogue = 10U; // d254

constexpr GuestAddress handle_of(const std::uint32_t strip) { return kStripTable + 8U * strip + 44U; }

struct QuadRecord {
    std::uint32_t strip_a;
    std::uint32_t quad_a;
    std::uint32_t strip_b;
    std::uint32_t quad_b;
};

template <class Memory>
QuadRecord read_record(const Memory& m, const GuestAddress strip, const std::uint32_t quad) {
    const GuestAddress e = m.read_u32_le(strip + 8U) + quad * 10U;
    return {m.read_u8(e + 4U), m.read_u16_le(e + 6U), m.read_u8(e + 5U), m.read_u16_le(e + 8U)};
}

template <class Context>
n3::Run advance_road_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress p = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const bool entry_s4 = cpu.read_gpr_u64(20U) != 0U;  // $s4 as the caller left it (see the header)
    std::uint32_t retired = 0U;

    // The callee natives, each with the original's argument registers; none may decline for the call to go on.
    const auto invoke_open = [&](const std::uint32_t strip, const std::uint32_t quad, const std::uint32_t span) {
        call.set_arg(0, strip);
        call.set_arg(1, quad);
        call.set_arg(2, span);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_00148ca0(mm); }, kFrameBytes);
        retired += r.retired;
        return r.declined ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{call.result()};
    };
    const auto invoke_flags = [&](const std::uint32_t handle, const std::uint32_t start, const std::uint32_t end) {
        call.set_arg(0, p);
        call.set_arg(1, handle);
        call.set_arg(2, start);
        call.set_arg(3, end);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_0014a560(mm); }, kFrameBytes);
        retired += r.retired;
        return r.declined ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{call.result()};
    };
    const auto invoke_init = [&]() {
        call.set_arg(0, p);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_0014d380(mm); }, kFrameBytes);
        retired += r.retired;
        return !r.declined;
    };
    // Puts the follower on quad `quad` of the strip behind `handle`.
    const auto set_seat = [&](const std::uint32_t handle, const std::uint32_t quad) {
        m.write_u32_le(p + kStripHandle, handle);
        m.write_u16_le(p + kQuad, static_cast<std::uint16_t>(quad));
    };
    // The re-seat of the chains: the seat, then the slot byte (a jal delay slot), then InitSlotDefaults.
    const auto reseat = [&](const std::uint32_t strip_id, const std::uint32_t quad) {
        set_seat(handle_of(strip_id), quad);
        m.write_u8(p + kSlot, 0U);
        return invoke_init();
    };

    // The owner's position goes to the frame, where the classifier reads it.
    const GuestAddress owner = m.read_u32_le(p + kOwner);
    const Gpr128 pos = n3::load_quad_any(m, m.read_u32_le(m.read_u32_le(owner + 9264U) + 516U) + kOwnerPos);
    n3::store_quad_any(m, frame + kPosSlot, pos);
    retired += kC20;

    if (m.read_u16_le(p + kQuad) == kNoQuad) {
        // Phase 1: seed the follower.
        const GuestAddress handle = m.read_u32_le(owner + kOwnerHandle);
        const std::uint32_t quad = m.read_u16_le(owner + kOwnerQuad);
        retired += kCA78;
        if (handle == 0U) {
            return call.decline();  // 0x14ccd0: no strip found (00148a80)
        }
        retired += kCA88;
        call.set_arg(0, m.read_u32_le(handle + 4U));
        call.set_arg(1, quad);
        call.set_arg(2, 1U);
        const n3::Run scan = call.invoke([](auto& mm) { return n3::fn_00148c20(mm); }, kFrameBytes);
        if (scan.declined) {
            return call.decline();
        }
        retired += scan.retired;
        if (call.result() != 0U) {
            set_seat(handle, quad);
            retired += kCAA0;
        } else {
            const QuadRecord e = read_record(m, m.read_u32_le(handle + 4U), quad);
            retired += kCABC;
            const auto ra_full = invoke_open(e.strip_a, e.quad_a, 1U);
            const auto rb_full = invoke_open(e.strip_b, e.quad_b, 1U);
            if (!ra_full || !rb_full) {
                return call.decline();
            }
            const bool ra = (*ra_full & 0xffU) != 0U;
            const bool rb = (*rb_full & 0xffU) != 0U;
            if (ra && !rb) {
                retired += kCC48 + kChoose;
                set_seat(handle_of(e.strip_a), e.quad_a);
            } else if (!ra && rb) {
                retired += kCB1C + kCC74 + kChoose;
                set_seat(handle_of(e.strip_b), e.quad_b);
            } else if (!ra && !rb) {
                return call.decline();  // 0x14cca0: no strip found (00148a80)
            } else {
                // Both links open: the side of the quad the owner's position is on, from the dot product of its
                // offset to the quad's centre with the quad's step-cross vector.
                retired += kCC48 + kCB1C + kCB24 + kCB2C + kCB34;
                const GuestAddress vertices = m.read_u32_le(kVertices);
                const GuestAddress corners = m.read_u32_le(m.read_u32_le(handle + 4U)) + (quad << 2U);
                const auto vertex = [&](const GuestAddress index_at) {
                    return n3::load_quad_any(m, vertices + (static_cast<std::uint32_t>(m.read_u16_le(corners + index_at)) << 4U));
                };
                cpu.write_vu0_vector(5U, Gpr128{{kQuarter, 0U, 0U, 0U}});
                cpu.write_vu0_vector(4U, n3::load_quad_any(m, owner + kOwnerPos));
                cpu.write_vu0_vector(2U, vertex(2U));
                cpu.write_vu0_vector(3U, vertex(0U));
                cpu.write_vu0_vector(1U, vertex(4U));
                ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, n3::kMaskXyz, false);
                ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);
                cpu.write_vu0_vector(2U, vertex(6U));
                ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, false);
                ee_cop2::vmul_broadcast(cpu, 1U, 1U, 5U, n3::kMaskXyz, 0U);   // vmulx.xyz vf1,vf1,vf5x
                ee_cop2::vadd_vector(cpu, 1U, 4U, 1U, n3::kMaskXyz, true);    // offset = position - centre
                const Gpr128 offset = cpu.read_vu0_vector(1U);
                call.set_arg(0, handle);
                call.set_arg(1, quad);
                call.set_arg(2, frame + kStepSlot);
                const n3::Run step = call.invoke([](auto& mm) { return n3::fn_00147e90(mm); }, kFrameBytes);
                if (step.declined) {
                    return call.decline();
                }
                retired += step.retired;
                cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame + kStepSlot));
                cpu.write_vu0_vector(3U, offset);
                ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);  // vf1.x = 1.0
                ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);
                ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
                const bool take_b = fpu::le(cpu.read_vu0_vector(1U).words[0], 0U);
                retired += kChoose;
                if (take_b) {
                    set_seat(handle_of(e.strip_b), e.quad_b);
                } else {
                    set_seat(handle_of(e.strip_a), e.quad_a);
                }
            }
        }
    }
    retired += kCCD8;

    // Phase 2: is something ahead on the current quad's route?
    const std::uint32_t seat_quad = m.read_u16_le(p + kQuad);
    retired += kCCDC;
    const auto ahead = invoke_flags(m.read_u32_le(p + kStripHandle), seat_quad, (seat_quad + 8U) & 0xffffU);
    if (!ahead) {
        return call.decline();
    }
    bool classify_directly = true;  // reached 0x14cde0 (true) or 0x14cde4 (false, after a chain B re-seat)
    if (*ahead != 0U) {
        retired += kCCF8;
        const QuadRecord first = read_record(m, m.read_u32_le(m.read_u32_le(p + kStripHandle) + 4U), m.read_u16_le(p + kQuad));
        std::uint32_t strip = first.strip_a;
        std::uint32_t quad = first.quad_a;
        bool reseated = false;
        // Chain A: strips linked through A while the route stays open.
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            retired += kLoopHead;
            const auto open = invoke_open(strip, quad, 1U);
            if (!open) {
                return call.decline();
            }
            if (*open == 0U) {
                break;
            }
            retired += kSpanTest;
            std::int32_t span = static_cast<std::int32_t>(((quad + 8U) & 0xffffU) - quad);
            if (span < 0) {
                retired += kSpanWrap;
                span = static_cast<std::int32_t>(static_cast<std::uint32_t>(span) +
                                                 m.read_u16_le(m.read_u32_le(handle_of(strip) + 4U) + 12U));
            }
            retired += kSpanScan;
            const auto through = invoke_open(strip, quad, static_cast<std::uint32_t>(span) & 0xffffU);
            if (!through) {
                return call.decline();
            }
            std::uint32_t flags = 4U;
            if (*through != 0U) {
                retired += kFlagsCall;
                const auto f = invoke_flags(handle_of(strip), quad, (quad + 8U) & 0xffffU);
                if (!f) {
                    return call.decline();
                }
                flags = *f;
            } else {
                retired += kNoFlags;
            }
            retired += kFlagsTest;
            if (flags == 0U) {
                retired += kReseatA;
                if (!reseat(strip, quad)) {
                    return call.decline();
                }
                reseated = true;
                break;
            }
            retired += kStepLink;
            const QuadRecord next = read_record(m, m.read_u32_le(handle_of(strip) + 4U), quad);
            quad = next.quad_a;
            strip = next.strip_a;
        }
        retired += kAfterA;
        if (!reseated) {
            // Chain B: the same walk through the B links of the (unchanged) current quad.
            retired += kChainB;
            const QuadRecord second =
                read_record(m, m.read_u32_le(m.read_u32_le(p + kStripHandle) + 4U), m.read_u16_le(p + kQuad));
            std::uint32_t strip_b = second.strip_b;
            std::uint32_t quad_b = second.quad_b;
            for (;;) {
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                retired += kLoopHead;
                const auto open = invoke_open(strip_b, quad_b, 1U);
                if (!open) {
                    return call.decline();
                }
                if (*open == 0U) {
                    break;
                }
                retired += kSpanTest;
                std::int32_t span = static_cast<std::int32_t>(((quad_b + 8U) & 0xffffU) - quad_b);
                if (span < 0) {
                    retired += kSpanWrap;
                    span = static_cast<std::int32_t>(static_cast<std::uint32_t>(span) +
                                                     m.read_u16_le(m.read_u32_le(handle_of(strip_b) + 4U) + 12U));
                }
                retired += kSpanScan;
                const auto through = invoke_open(strip_b, quad_b, static_cast<std::uint32_t>(span) & 0xffffU);
                if (!through) {
                    return call.decline();
                }
                std::uint32_t flags = 4U;
                if (*through != 0U) {
                    retired += kFlagsCall;
                    const auto f = invoke_flags(handle_of(strip_b), quad_b, (quad_b + 8U) & 0xffffU);
                    if (!f) {
                        return call.decline();
                    }
                    flags = *f;
                } else {
                    retired += kNoFlags;
                }
                retired += kFlagsTest;
                if (flags == 0U) {
                    retired += kReseatB;
                    if (!reseat(strip_b, quad_b)) {
                        return call.decline();
                    }
                    classify_directly = false;
                    break;
                }
                retired += kStepLink;
                const QuadRecord next = read_record(m, m.read_u32_le(handle_of(strip_b) + 4U), quad_b);
                quad_b = next.quad_b;
                strip_b = next.strip_b;
            }
        }
    } else {
        // f == 0: straight to the classification (0x14ccdc's taken branch).
    }
    if (classify_directly) {
        retired += kCDE0;
    }

    // Phase 3: classify the owner's position against the current quad.
    call.set_arg(0, m.read_u32_le(p + kStripHandle));
    call.set_arg(1, frame + kPosSlot);
    call.set_arg(2, m.read_u16_le(p + kQuad));
    const n3::Run cls_run = call.invoke([](auto& mm) { return n3::fn_00148440(mm); }, kFrameBytes);
    if (cls_run.declined) {
        return call.decline();
    }
    retired += kCDE4 + cls_run.retired;
    const std::uint32_t cls = call.result();
    call.return_value(1U);

    if ((cls & 4U) == 0U && (cls & 8U) == 0U) {
        retired += kCDFC + kExit;
        return call.finish(retired);
    }

    const std::uint32_t quad = m.read_u16_le(p + kQuad);
    const GuestAddress strip = m.read_u32_le(m.read_u32_le(p + kStripHandle) + 4U);
    const std::uint32_t count = m.read_u16_le(strip + 12U);
    const bool closed = m.read_u8(strip + 14U) != 0U;

    if ((cls & 4U) != 0U) {
        // Past the end edge.
        retired += kEndEdge;
        if (static_cast<std::int32_t>(quad) != static_cast<std::int32_t>(count) - 1) {
            m.write_u16_le(p + kQuad, static_cast<std::uint16_t>(quad + 1U));
            retired += kStepOn + kExit;
            return call.finish(retired);
        }
        retired += kClosedTest;
        if (closed) {
            m.write_u16_le(p + kQuad, 0U);
            retired += kWrapStart + kExit;
            return call.finish(retired);
        }
    } else {
        // Before the start edge.
        retired += kCDFC + kStartEdge;
        if (quad != 0U) {
            m.write_u16_le(p + kQuad, static_cast<std::uint16_t>(quad - 1U));
            retired += kStepBack + kExit;
            return call.finish(retired);
        }
        retired += kStartStrip;
        if (closed) {
            m.write_u16_le(p + kQuad, static_cast<std::uint16_t>(count - 1U));
            retired += kWrapBack + kExit;
            return call.finish(retired);
        }
    }

    // The follower is at the end of an open strip (quad 0 at the start, the last quad at the end): look for a
    // strip linked from this quad, A first and then B. The probe flag starts as the caller's $s4.
    const QuadRecord e = read_record(m, strip, quad);
    bool linked = entry_s4;
    retired += kLinkProbe;
    const auto first_open = invoke_open(e.strip_a, e.quad_a, 1U);
    if (!first_open) {
        return call.decline();
    }
    if (*first_open != 0U) {
        retired += kLinkTaken;
        linked = true;
        set_seat(handle_of(e.strip_a), e.quad_a);
    }
    retired += kLinkTest;
    if (!linked) {
        retired += kLinkRetry;
        const auto second_open = invoke_open(e.strip_b, e.quad_b, 1U);
        if (!second_open) {
            return call.decline();
        }
        if (*second_open != 0U) {
            retired += kLinkTaken;
            linked = true;
            set_seat(handle_of(e.strip_b), e.quad_b);
        }
    }
    retired += kLinkTest;
    if (!linked) {
        return call.decline();  // 0x14d10c / 0x14d238: no strip found (00148a80)
    }
    retired += kReseatEnd;
    if (!invoke_init()) {
        return call.decline();
    }
    retired += kEpilogue;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014ca20, 2140U, 0xbafb3591e8e2c6cdULL, "AdvanceRoadQuad", kCeiling, advance_road_quad,
         EngineReturn::Integer)
