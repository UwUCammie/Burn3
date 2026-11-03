// 0x0026af50 (a0 = world object O, 160 bytes; a1 = record R, 64 bytes; a2 = clip mode; a3 = clip argument, low byte):
// SubmitWorldObject. Unless a2 == 1, the VU0 microprogram at 0x143 (VCALLMS at 0x0026afac) culls the oriented box of R
// (corner R+48, edges R+0, R+16, R+32) against the six planes of frustum view O[153]. Outside returns 0 and stores
// nothing. Otherwise the call returns 1 after appending to the draw list D (the pointer at gp-23648): O's tag, O's first
// quadword, the Q queued quadwords (copied in batches of eight, then singles), a clip-change pair when the clip flags are
// positive, and a tail (a DMA call to R[28], or O+32). Then D[0] = 0, D[4] = the new write cursor, O[152] = the clip
// mode and O[155] = 1.
//
// Both runs launch the real microprogram (n3::run_vu0_micro_any), so the cull result and the VU0 state are the original's
// in a dry run too, and both perform every store. Destinations that overlap the draw list, O, R or this code decline
// (the original then runs), so the closed-form count and the bulk copy are taken from the values before the call.
// Q is at most kMaximumQueued so the copy stays bounded; a negative Q runs no copy at all.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kCodeAddress = 0x0026af50U;
constexpr std::uint32_t kCodeBytes = 612U;
constexpr GuestAddress kDrawListPointerOffset = 0U - 23648U;   // gp-relative: the draw list pointer D
constexpr GuestAddress kViewTable = 0x01e327d0U;
constexpr std::uint32_t kViewStride = 96U;
constexpr std::uint16_t kCullMicroEntry = 0x143U;
constexpr GuestAddress kCullVcallmsAddress = 0x0026afacU;
constexpr std::uint32_t kMaximumQueued = 4096U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSkipCullCost = 6U;            // a2 == 1 on entry: 0x0026af50-0x0026af64, then 0x0026afe4
constexpr std::uint32_t kCullCost = 33U;               // a2 != 1: the cull up to 0x0026afe4, or the whole outside exit
constexpr std::uint32_t kStreamCost = 49U;             // the stream blocks every call runs: 19 + 6 + 2 + 4 + 7 + 3 + 5 + 3
constexpr std::uint32_t kBatchCost = 33U;              // one batch of eight: its 27 instructions and the test after it (6)
constexpr std::uint32_t kSingleCost = 11U;             // one single quadword: its 7 instructions and the test after it (4)
constexpr std::uint32_t kClipPairCostCommand2 = 16U;   // clip change with a2 == 2: 3 + 13
constexpr std::uint32_t kClipPairCost = 21U;           // other clip modes: 3 + 3 + 2 + 13
constexpr std::uint32_t kTailCost = 1U;                // R[28] == 0: the lq of O+32
constexpr std::uint32_t kProgramTailCost = 8U;         // R[28] != 0: the DMA call quadword

struct GuestSpan {
    std::uint64_t begin{};
    std::uint64_t end{};
};

[[nodiscard]] GuestSpan span_of(const GuestAddress address, const std::uint64_t bytes) noexcept {
    return GuestSpan{address, static_cast<std::uint64_t>(address) + bytes};
}

[[nodiscard]] bool overlaps(const GuestSpan a, const GuestSpan b) noexcept {
    return a.begin < b.end && b.begin < a.end;
}

// Whether the range is guest memory. The dry run asks before it reads or writes; the live run was admitted by the dry
// run for the same call state, so it always answers yes.
template <class Memory>
[[nodiscard]] bool mapped([[maybe_unused]] const Memory& memory, [[maybe_unused]] const GuestAddress address,
                          [[maybe_unused]] const std::uint64_t length) {
    if constexpr (std::is_same_v<Memory, guest::EeMemory>) {
        return true;
    } else {
        return memory.contains(address, static_cast<std::size_t>(length));
    }
}


// The VU0 inputs of the cull: the record's box (corner R+48, edges R+0, R+16, R+32) and the six planes of a view.
template <class Memory>
void load_cull_inputs(guest::R5900State& cpu, Memory& m, const GuestAddress record, const GuestAddress view) {
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 48U));
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, record + 32U));
    cpu.write_vu0_vector(7U, n3::load_quad_any(m, record + 0U));
    cpu.write_vu0_vector(8U, n3::load_quad_any(m, record + 16U));
    cpu.write_vu0_vector(9U, n3::load_quad_any(m, view + 32U));
    cpu.write_vu0_vector(10U, n3::load_quad_any(m, view + 48U));
    cpu.write_vu0_vector(11U, n3::load_quad_any(m, view + 64U));
    cpu.write_vu0_vector(12U, n3::load_quad_any(m, view + 80U));
    cpu.write_vu0_vector(13U, n3::load_quad_any(m, view + 0U));
    cpu.write_vu0_vector(14U, n3::load_quad_any(m, view + 16U));
}

template <class Context>
n3::Run submit_world_object(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const GuestAddress object = call.arg(0U);
    const GuestAddress record = call.arg(1U);
    const std::uint64_t entry_clip = call.arg64(2U);
    std::uint64_t clip = entry_clip;
    const std::uint32_t clip_argument = call.arg(3U) & 0xffU;
    if (!mapped(m, object, 160U) || !mapped(m, record, 64U) || (object & 15U) != 0U || (record & 15U) != 0U) {
        return call.decline();
    }
    const GuestAddress list_pointer = cpu.read_gpr_u32(n3::kGlobalPointer) + kDrawListPointerOffset;
    if (!mapped(m, list_pointer, 4U)) {
        return call.decline();
    }
    const GuestAddress list = m.read_u32_le(list_pointer);
    if (!mapped(m, list, 16U) || (list & 15U) != 0U) {
        return call.decline();
    }
    const auto queued = static_cast<std::int32_t>(m.read_u32_le(list));

    if (entry_clip != 1U) {
        const auto view_index = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + 153U)));
        const GuestAddress view = kViewTable + kViewStride * static_cast<GuestAddress>(view_index);
        if (!mapped(m, view, kViewStride)) {
            return call.decline();
        }
        load_cull_inputs(cpu, m, record, view);
        cpu.set_pc(kCullVcallmsAddress);
        const auto launch = n3::run_vu0_micro_any(cpu, m, kCullMicroEntry);
        if (launch.status != guest::Vu0MicroRunStatus::Completed) {
            if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>) {
                return call.decline();
            }
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("SubmitWorldObject: VU0 microprogram 0x143 did not complete: " +
                                     std::string(guest::to_string(launch.status)));
        }
        // VI1 & 0xffff: 0 outside, 2 inside entirely (no clipping needed), anything else intersecting.
        const std::uint32_t visibility = ee_cop2::cfc2(cpu, 1U) & 0xffffU;
        if (visibility == 0U) {
            call.return_value(0U);
            return call.finish(kCullCost);
        }
        if (visibility == 2U) {
            clip = 1U;
        }
    }

    // A negative Q runs neither copy loop; Q - 7 must not wrap in the original's 32-bit compare.
    if (queued > static_cast<std::int32_t>(kMaximumQueued) || queued < INT32_MIN + 7) {
        return call.decline();
    }
    const std::uint32_t quads = queued > 0 ? static_cast<std::uint32_t>(queued) : 0U;
    const GuestAddress cursor_start = m.read_u32_le(list + 4U);
    const std::uint64_t written_bytes = 16U * (static_cast<std::uint64_t>(quads) + 5U);
    const std::uint64_t queue_bytes = 16U + 16U * static_cast<std::uint64_t>(quads);
    if (!mapped(m, cursor_start, written_bytes) || (cursor_start & 15U) != 0U || !mapped(m, list, queue_bytes)) {
        return call.decline();
    }
    const GuestSpan written = span_of(cursor_start, written_bytes);
    const GuestSpan queue = span_of(list, queue_bytes);
    const GuestSpan object_span = span_of(object, 160U);
    const GuestSpan record_span = span_of(record, 64U);
    const GuestSpan pointer_span = span_of(list_pointer, 4U);
    if (overlaps(written, queue) || overlaps(written, pointer_span) || overlaps(written, object_span) ||
        overlaps(written, record_span) || overlaps(written, span_of(kCodeAddress, kCodeBytes)) ||
        overlaps(queue, object_span) || overlaps(queue, record_span) || overlaps(queue, pointer_span) ||
        overlaps(object_span, record_span) || overlaps(object_span, pointer_span)) {
        return call.decline();
    }

    const auto old_clip = static_cast<std::int8_t>(m.read_u8(object + 152U));
    const std::uint32_t tag_address = m.read_u32_le(record + 12U);
    const std::uint32_t tail_program = m.read_u32_le(record + 28U);
    const std::uint64_t flags = static_cast<std::uint64_t>(static_cast<std::int64_t>(queued)) |
                                (clip & ~static_cast<std::uint64_t>(static_cast<std::int64_t>(old_clip)));
    const bool clip_change = static_cast<std::int64_t>(flags) > 0;

    // Batches of eight quadwords while i < Q - 7, then singles while i < Q: the closed form of those loops.
    const std::uint32_t batches = quads / 8U;
    const std::uint32_t singles = quads % 8U;
    std::uint32_t retired = entry_clip != 1U ? kCullCost : kSkipCullCost;
    retired += kStreamCost + batches * kBatchCost + singles * kSingleCost;
    if (clip_change) {
        retired += clip == 2U ? kClipPairCostCommand2 : kClipPairCost;
    }
    retired += tail_program == 0U ? kTailCost : kProgramTailCost;

    m.write_u8(object + 155U, 1U);
    n3::store_quad_any(m, cursor_start, Gpr128{{0x50000000U, tag_address, 0U, 0U}});
    n3::store_quad_any(m, cursor_start + 16U, n3::load_quad_any(m, object));
    GuestAddress cursor = cursor_start + 32U;
    for (std::uint32_t k = 0U; k < quads; ++k) {
        n3::store_quad_any(m, cursor, n3::load_quad_any(m, list + 16U + 16U * k));
        cursor += 16U;
    }
    m.write_u32_le(list, 0U);
    m.write_u8(object + 152U, static_cast<std::uint8_t>(clip));
    if (clip_change) {
        n3::store_quad_any(m, cursor, n3::load_quad_any(m, object + 16U));
        const std::uint64_t command = clip == 2U ? 2U : (clip == 3U ? 10U : 0U);
        const std::uint64_t pair = (flags << 32U) | clip_argument;
        n3::store_quad_any(m, cursor + 16U,
                           Gpr128{{static_cast<std::uint32_t>(command), static_cast<std::uint32_t>(command >> 32U),
                                   static_cast<std::uint32_t>(pair), static_cast<std::uint32_t>(pair >> 32U)}});
        cursor += 32U;
    }
    if (tail_program != 0U) {
        n3::store_quad_any(m, cursor, Gpr128{{0x50000000U, tail_program, 0x17000000U, 0U}});
    } else {
        n3::store_quad_any(m, cursor, n3::load_quad_any(m, object + 32U));
    }
    cursor += 16U;
    m.write_u32_le(list + 4U, cursor);
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0026af50, 612U, 0x37a8918c069be35dULL, "SubmitWorldObject", takedown::runtime::kCeiling,
         takedown::runtime::submit_world_object, EngineReturn::Integer)
