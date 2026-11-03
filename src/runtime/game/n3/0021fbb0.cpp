// 0x0021fbb0 (a0 = draw object S, a1 = value for the work record, a2 = four quadwords (the model matrix), a3 = entry
// list L, t0 = entry mask, t1 = low 16 bits stored in the work record, t2 = VI3 of the preparation microprogram,
// f12 = float stored in the work record): DrawEntryList. S+256 points at a work record W (a scratch block of about
// 1 KiB that the microprograms and the packet builder share), L holds a signed byte count at +0, the address of an
// array of 16-byte entries at +4 and a float at +8.
//
// 1. Preparation: L's float goes to W+844, the matrix and the record's transform blocks are loaded into VF1..VF14 and
//    microprogram 0xfb (VCALLMS at 0x0021fc3c) runs once. Then W+920 is cleared, the pending queue length W+60 is set to
//    13 and its first quadword (a DMA call tag plus a VIF word, W+64) is built from the length; the arguments go to
//    W+52 / W+56 / W+912, and the registers microprogram 0xfb left behind (VF5..VF14) are written to W+80..W+256.
// 2. For every entry whose halfword at +8 matches the mask: microprogram 0x16a runs on the matrix at W+0..48 with the
//    entry's bytes (+11 slot, +13, +10) and halfword in VI5 / VI6 / VI9 / VI4, then WordTableLookup-style reservation
//    0x001f3080 (a0 = 0, a1 = queue length + 12; its result is ignored) makes room and the entry's packet is appended at
//    the display list cursor (word gp-24240): a DMA call tag to the entry's chain (+0), the template quadword W+960,
//    the pending queue (W+64, queue length quadwords, only the first time: the length is then cleared), W+976, W+992,
//    the slot's quadword S+32+16*slot, six quadwords built from the microprogram's results (VF6..VF9 stored to W+848,
//    VI7 to W+916, VI8 to W+944) and a last quadword (the entry's +4 word stored at W+932 and W+928, or W+1008).
//
// Every read of the work pointer and of the list header happens where the listing does it, so the order against the
// cursor stores is the original's. A dry run runs both microprograms too (n3::run_vu0_micro_any on a snapshot of VU0
// data memory) and the callee in its own dry run, so every store and VU0 register write is the live run's.
//
// Retired counts: 83 for the preparation, 4 per loop test, 9 for a skipped entry, 8 + 33 + callee + queue + 54 +
// (5 or 2) + 3 + 1 for a drawn one (queue = 13 + 17 * (n / 4) + 8 * (n % 4) for n > 0, 13 for n < 0, 0 for n = 0), 9 to
// leave. The function's own frame saves sit below the entry $sp and are not reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 112U;

// Draw object S.
constexpr GuestAddress kWorkPointer = 256U;
constexpr GuestAddress kObjectBlockA = 176U + 16U;   // VF12 of microprogram 0xfb
constexpr GuestAddress kObjectBlockB = 176U + 48U;   // VF13
constexpr GuestAddress kSlotTable = 32U;             // 16-byte slot quadwords

// Work record W.
constexpr GuestAddress kQueueQuads = 60U;
constexpr GuestAddress kQueue = 64U;
constexpr GuestAddress kFloatA = 52U;
constexpr GuestAddress kFlagsLow = 56U;
constexpr GuestAddress kBlockE = 784U;               // VF5..VF7
constexpr GuestAddress kBlockF = 144U;               // VF8..VF11
constexpr GuestAddress kBlockG = 832U;               // VF14
constexpr GuestAddress kListFloat = 844U;
constexpr GuestAddress kResults = 848U;              // VF6..VF9 of microprogram 0x16a
constexpr GuestAddress kLastResult = 896U;
constexpr GuestAddress kArgument = 912U;
constexpr GuestAddress kVi7Word = 916U;
constexpr GuestAddress kClearedWord = 920U;
constexpr GuestAddress kEntryWord = 928U;
constexpr GuestAddress kEntryWordValue = 932U;
constexpr GuestAddress kVi8Double = 944U;
constexpr GuestAddress kTemplateA = 960U;
constexpr GuestAddress kTemplateB = 976U;
constexpr GuestAddress kTemplateC = 992U;
constexpr GuestAddress kTemplateD = 1008U;

constexpr GuestAddress kCursorOffset = 0U - 24240U;  // gp-relative: the display list cursor

constexpr std::uint16_t kPrepareEntry = 0xfbU;
constexpr GuestAddress kPrepareVcallms = 0x0021fc3cU;
constexpr std::uint16_t kEntryProgram = 0x16aU;
constexpr GuestAddress kEntryVcallms = 0x0021fd4cU;

constexpr std::uint32_t kPrepareCost = 83U;
constexpr std::uint32_t kTestCost = 4U;
constexpr std::uint32_t kSkipCost = 9U;
constexpr std::uint32_t kSelectCost = 8U;            // 0x0021fcfc-0x0021fd18
constexpr std::uint32_t kLaunchCost = 33U;           // 0x0021fd1c-0x0021fd9c, the callee excluded
constexpr std::uint32_t kQueueStartCost = 4U;
constexpr std::uint32_t kQuadsTestCost = 3U;
constexpr std::uint32_t kQuadsPassCost = 14U;
constexpr std::uint32_t kSinglesStartCost = 2U;
constexpr std::uint32_t kSinglesTestCost = 2U;
constexpr std::uint32_t kSinglePassCost = 6U;
constexpr std::uint32_t kQueueEndCost = 2U;
constexpr std::uint32_t kPacketCost = 54U;
constexpr std::uint32_t kWithWordCost = 5U;
constexpr std::uint32_t kWithoutWordCost = 2U;
constexpr std::uint32_t kEndCost = 3U + 1U;          // cursor store, then the loop increment
constexpr std::uint32_t kReturnCost = 9U;

constexpr std::uint32_t kCallTag = 0x50000000U;
constexpr std::uint32_t kResultsDoubleHigh = 67U;

template <class Memory>
void copy_quad(Memory& m, const GuestAddress destination, const GuestAddress source) {
    n3::store_quad_any(m, destination, n3::load_quad_any(m, source));
}

template <class Context>
n3::Run draw_entry_list(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    constexpr bool kLive = std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>;

    const GuestAddress object = call.arg(0U);
    const std::uint32_t value = call.arg(1U);
    const GuestAddress matrix = call.arg(2U);
    const GuestAddress list = call.arg(3U);
    const std::uint32_t mask = call.arg(4U) & 0xffffU;
    const std::uint32_t low_flags = call.arg(5U) & 0xffffU;
    const std::uint32_t prepare_vi3 = call.arg(6U) & 0xffU;
    const std::uint32_t float_a = cpu.read_fpr_bits(12U);
    const GuestAddress cursor_slot = cpu.read_gpr_u32(n3::kGlobalPointer) + kCursorOffset;

    const auto work = [&]() { return m.read_u32_le(object + kWorkPointer); };
    const auto launch = [&](const std::uint16_t entry, const GuestAddress vcallms) {
        cpu.set_pc(vcallms);
        const auto result = n3::run_vu0_micro_any(cpu, m, entry);
        if (result.status != guest::Vu0MicroRunStatus::Completed) {
            if constexpr (!kLive) return false;
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("DrawEntryList: VU0 microprogram did not complete: " +
                                     std::string(guest::to_string(result.status)));
        }
        return true;
    };

    std::uint32_t retired = kPrepareCost;

    // Preparation: the transform blocks go into VF1..VF14 and microprogram 0xfb runs.
    {
        m.write_u32_le(work() + kListFloat, m.read_u32_le(list + 8U));
        const GuestAddress w = work();
        ee_cop2::ctc2(cpu, 3U, prepare_vi3);
        for (unsigned i = 0U; i < 4U; ++i) {
            cpu.write_vu0_vector(1U + i, n3::load_quad_any(m, matrix + 16U * i));
        }
        for (unsigned i = 0U; i < 3U; ++i) {
            cpu.write_vu0_vector(5U + i, n3::load_quad_any(m, w + kBlockE + 16U * i));
        }
        for (unsigned i = 0U; i < 4U; ++i) {
            cpu.write_vu0_vector(8U + i, n3::load_quad_any(m, w + kBlockF + 16U * i));
        }
        cpu.write_vu0_vector(12U, n3::load_quad_any(m, object + kObjectBlockA));
        cpu.write_vu0_vector(13U, n3::load_quad_any(m, object + kObjectBlockB));
        cpu.write_vu0_vector(14U, n3::load_quad_any(m, w + kBlockG));
        if (!launch(kPrepareEntry, kPrepareVcallms)) {
            return call.decline();
        }
        m.write_u32_le(w + kClearedWord, 0U);
    }

    // The pending queue starts as 13 quadwords whose first is a DMA tag built from the length.
    {
        m.write_u32_le(work() + kQueueQuads, 13U);
        const GuestAddress w = work();
        const std::uint32_t last = m.read_u32_le(w + kQueueQuads) - 1U;
        const std::uint32_t head = last | 0x10000000U;
        n3::store_quad_any(m, w + kQueue,
                           Gpr128{{head, (head & 0x80000000U) != 0U ? 0xffffffffU : 0U, 0x01000404U,
                                   (last << 16U) | 0x6c0003c2U}});
        m.write_u32_le(work() + kFloatA, float_a);
        m.write_u32_le(work() + kFlagsLow, low_flags);
        m.write_u32_le(work() + kArgument, value);
        const GuestAddress base = work() + kQueue;
        ee_cop2::ctc2(cpu, 4U, 0U);
        for (unsigned i = 0U; i < 4U; ++i) {
            n3::store_quad_any(m, base + 16U + 16U * i, cpu.read_vu0_vector(5U + i));      // VF5..VF8
            n3::store_quad_any(m, base + 80U + 16U * i, cpu.read_vu0_vector(1U + i));      // VF1..VF4
        }
        n3::store_quad_any(m, base + 144U, cpu.read_vu0_vector(14U));
        for (unsigned i = 0U; i < 3U; ++i) {
            n3::store_quad_any(m, base + 160U + 16U * i, cpu.read_vu0_vector(9U + i));     // VF9..VF11
        }
    }

    for (std::int32_t index = 0;; ++index) {
        retired += kTestCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (!(index < static_cast<std::int8_t>(m.read_u8(list)))) {
            break;
        }
        const GuestAddress entry = m.read_u32_le(list + 4U) + 16U * static_cast<std::uint32_t>(index);
        const std::uint32_t entry_flags = m.read_u16_le(entry + 8U);
        if ((mask & entry_flags) == 0U) {
            retired += kSkipCost;
            continue;
        }
        retired += kSelectCost + kLaunchCost;

        // Microprogram 0x16a on the work record's matrix.
        const std::uint32_t slot = m.read_u8(entry + 11U);
        const GuestAddress w = work();
        ee_cop2::ctc2(cpu, 5U, slot);
        ee_cop2::ctc2(cpu, 4U, entry_flags);
        ee_cop2::ctc2(cpu, 6U, m.read_u8(entry + 13U));
        ee_cop2::ctc2(cpu, 9U, m.read_u8(entry + 10U));
        for (unsigned i = 0U; i < 4U; ++i) {
            cpu.write_vu0_vector(1U + i, n3::load_quad_any(m, w + 16U * i));
        }
        if (!launch(kEntryProgram, kEntryVcallms)) {
            return call.decline();
        }

        // Make room in the display list.
        const std::uint32_t pending = m.read_u32_le(w + kQueueQuads);
        call.set_arg64(0U, 0U);
        call.set_arg(1U, pending + 12U);
        const n3::Run reserve = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
        if (reserve.declined) {
            return call.decline();
        }
        retired += reserve.retired;

        // The DMA call tag to the entry's chain and the template quadword.
        const GuestAddress cursor = m.read_u32_le(cursor_slot);
        const std::uint32_t chain = m.read_u32_le(entry);
        n3::store_quad_any(m, cursor, Gpr128{{kCallTag, chain, 0U, 0U}});
        copy_quad(m, cursor + 16U, work() + kTemplateA);

        // The pending queue goes out once, then the queue is empty.
        GuestAddress out = cursor + 32U;
        const std::int32_t queued = static_cast<std::int32_t>(m.read_u32_le(work() + kQueueQuads));
        if (queued != 0) {
            GuestAddress in = work() + kQueue;
            std::int32_t copied = 0;
            const std::int32_t batch_limit = static_cast<std::int32_t>(static_cast<std::uint32_t>(queued) - 3U);
            retired += kQueueStartCost;
            for (;;) {
                retired += kQuadsTestCost;
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                if (!(copied < batch_limit)) {
                    break;
                }
                retired += kQuadsPassCost;
                const Gpr128 q0 = n3::load_quad_any(m, in);
                const Gpr128 q1 = n3::load_quad_any(m, in + 16U);
                const Gpr128 q2 = n3::load_quad_any(m, in + 32U);
                const Gpr128 q3 = n3::load_quad_any(m, in + 48U);
                n3::store_quad_any(m, out, q0);
                n3::store_quad_any(m, out + 16U, q1);
                n3::store_quad_any(m, out + 32U, q2);
                n3::store_quad_any(m, out + 48U, q3);
                copied += 4;
                in += 64U;
                out += 64U;
            }
            retired += kSinglesStartCost;
            for (;;) {
                retired += kSinglesTestCost;
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                if (!(copied < queued)) {
                    break;
                }
                retired += kSinglePassCost;
                copy_quad(m, out, in);
                ++copied;
                in += 16U;
                out += 16U;
            }
            retired += kQueueEndCost;
            m.write_u32_le(work() + kQueueQuads, 0U);
        }

        // The entry's own packet quadwords.
        retired += kPacketCost;
        copy_quad(m, out, work() + kTemplateB);
        copy_quad(m, out + 16U, work() + kTemplateC);
        copy_quad(m, out + 32U, object + kSlotTable + 16U * slot);

        const std::uint32_t vi7 = ee_cop2::cfc2(cpu, 7U);
        const std::uint64_t vi8 = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(ee_cop2::cfc2(cpu, 8U))));
        const GuestAddress results = work() + kResults;
        for (unsigned i = 0U; i < 4U; ++i) {
            n3::store_quad_any(m, results + 16U * i, cpu.read_vu0_vector(6U + i));         // VF6..VF9
        }
        m.write_u32_le(work() + kVi7Word, vi7);
        m.write_u64_le(work() + kVi8Double, vi8);

        const GuestAddress tail = out + 48U;
        copy_quad(m, tail, work() + kLastResult);
        {
            const std::uint64_t doubled = m.read_u64_le(work() + kVi8Double);
            n3::store_quad_any(m, tail + 16U,
                               Gpr128{{static_cast<std::uint32_t>(doubled), static_cast<std::uint32_t>(doubled >> 32U),
                                       kResultsDoubleHigh, 0U}});
        }
        copy_quad(m, tail + 32U, work() + kResults);
        copy_quad(m, tail + 48U, work() + kResults + 16U);
        copy_quad(m, tail + 64U, work() + kResults + 32U);
        copy_quad(m, tail + 80U, work() + kArgument);

        const std::uint32_t entry_word = m.read_u32_le(entry + 4U);
        if (entry_word != 0U) {
            retired += kWithWordCost;
            m.write_u32_le(work() + kEntryWordValue, entry_word);
            copy_quad(m, tail + 96U, work() + kEntryWord);
        } else {
            retired += kWithoutWordCost;
            copy_quad(m, tail + 96U, work() + kTemplateD);
        }
        m.write_u32_le(cursor_slot, tail + 112U);
        retired += kEndCost;
    }

    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021fbb0, 940U, 0xb208faad62dc2e0aULL, "DrawEntryList", takedown::runtime::kCeiling,
         takedown::runtime::draw_entry_list, EngineReturn::Nothing)
