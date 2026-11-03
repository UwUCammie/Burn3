// 0x0021f790 DrawSkinnedModel (a0 = draw context O, a1 = word kept in the work area, a2 = 4x4 model matrix (four
// quadwords), a3 = model descriptor D, t0 = part mask M (low 16 bits), t1 = word whose low half goes to the work area,
// t2 = T bone matrices of 64 bytes, t3 = T (signed), f12 = float kept in the work area, stack byte = VI3 of the
// first microprogram). It submits one skinned model:
//
//  1. With R = *(O + 256) (the scratchpad work area), runs VU0 microprogram 0xfb (VCALLMS at 0x0021f81c) on the model
//     matrix, three camera quadwords at R + 784, four at R + 144, two of O and one at R + 832. R + 60 becomes 13 and
//     the T bone matrices are copied behind the staged uniforms at R + 272.
//  2. R + 920 = T; R + 60 grows by 4 * T (the staged quadword count); an UNPACK chain header goes to R + 64 and the
//     microprogram's twelve output vectors to R + 80 .. R + 256.
//  3. For each part of D (16-byte records: +0 geometry chain, +4 colour pointer, +8 half mask bits, +10/+11/+13 bytes)
//     whose mask bits match M: runs microprogram 0x16a (VCALLMS at 0x0021f994), reserves R + 60 + 12 quadwords of the
//     packet buffer (ReservePacketBuffer, 0x001f3080) and appends the packet at the buffer's current segment: a DMA
//     call tag to the part's chain, the staged uniforms (first part only, R + 60 becomes 0 afterwards), three state
//     quadwords and seven A+D / VIF quadwords built from the microprogram's results.
//
// A dry run runs both microprograms too (n3::run_vu0_micro_any), so it performs every store and register write of
// the live run. The work area words the packet re-reads (R + 912 .. 944) are kept in memory because the packet copies
// whole quadwords that mix them with stale words.

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
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Fields of the draw context, the work area R and the descriptors.
constexpr GuestAddress kContextWork = 256U;           // O: pointer to R
constexpr GuestAddress kContextCameraA = 192U;        // O + 176 + 16
constexpr GuestAddress kContextCameraB = 224U;        // O + 176 + 48
constexpr GuestAddress kContextSlots = 32U;           // quadword (O + 16 * slot) + 32

constexpr GuestAddress kWorkUniforms = 0U;            // four quadwords given to microprogram 0x16a
constexpr GuestAddress kWorkFloat = 52U;
constexpr GuestAddress kWorkHalf = 56U;
constexpr GuestAddress kWorkQwords = 60U;             // staged quadword count
constexpr GuestAddress kWorkStage = 64U;              // header, microprogram 0xfb outputs, bone matrices
constexpr GuestAddress kWorkBones = 272U;             // R + 64 + 13 * 16
constexpr GuestAddress kWorkCameraSlow = 784U;
constexpr GuestAddress kWorkStageCamera = 144U;       // R + 64 + 80
constexpr GuestAddress kWorkModelWord = 832U;
constexpr GuestAddress kWorkSkinFloat = 844U;
constexpr GuestAddress kWorkStateB = 848U;            // VF6..VF9 outputs
constexpr GuestAddress kWorkStateLast = 896U;
constexpr GuestAddress kWorkCaller = 912U;            // a1, VI7 (916), T (920): one quadword
constexpr GuestAddress kWorkVi7 = 916U;
constexpr GuestAddress kWorkBoneCount = 920U;
constexpr GuestAddress kWorkColourWord = 932U;
constexpr GuestAddress kWorkColourQuad = 928U;
constexpr GuestAddress kWorkVi8 = 944U;
constexpr GuestAddress kWorkTag = 960U;
constexpr GuestAddress kWorkStateX = 976U;
constexpr GuestAddress kWorkStateY = 992U;
constexpr GuestAddress kWorkDefaultColour = 1008U;

constexpr GuestAddress kCurrentSegmentBelow = 24240U; // gp - 24240: start of the open segment

constexpr std::uint32_t kBoneCountStart = 13U;
constexpr std::uint32_t kHeaderFlag = 0x10000000U;
constexpr std::uint32_t kHeaderUnpackWord = 0x01000404U;   // STCYCL 4,4 pair
constexpr std::uint32_t kHeaderUnpackTag = 0x6c0003c2U;    // UNPACK into VU0 data memory
constexpr std::uint32_t kCallTag = 0x50000000U;            // DMA call tag
constexpr std::uint32_t kAdMarker = 67U;                   // upper half of the A+D quadword that carries VI8
constexpr std::uint32_t kReserveExtra = 12U;
constexpr std::uint16_t kMicroPrepare = 0xfbU;
constexpr std::uint16_t kMicroPart = 0x16aU;
constexpr GuestAddress kPrepareVcallms = 0x0021f81cU;
constexpr GuestAddress kPartVcallms = 0x0021f994U;
constexpr std::uint32_t kFrameBytes = 112U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions per block (brief.py, delay slots included).
constexpr std::uint32_t kPrepare = 45U + 3U;           // 0x21f790-0x21f840, first bone test
constexpr std::uint32_t kBonePass = 11U + 3U;          // 0x21f844-0x21f86c + test
constexpr std::uint32_t kStageSetup = 50U;             // 0x21f87c-0x21f940
constexpr std::uint32_t kPartTest = 4U;                // 0x21fb70
constexpr std::uint32_t kPartSkipped = 8U + 1U;        // 0x21f944, 0x21fb6c
constexpr std::uint32_t kPartHead = 8U + 33U;          // 0x21f944, 0x21f964 (jal + delay slot included)
constexpr std::uint32_t kCopySetup = 4U;               // 0x21f9e8
constexpr std::uint32_t kGroupTest = 3U;
constexpr std::uint32_t kGroupPass = 14U;
constexpr std::uint32_t kSingleSetup = 2U;             // 0x21fa3c
constexpr std::uint32_t kSingleTest = 2U;
constexpr std::uint32_t kSinglePass = 6U;
constexpr std::uint32_t kStageCleared = 2U;            // 0x21fa64
constexpr std::uint32_t kPacketBody = 54U;             // 0x21fa6c
constexpr std::uint32_t kColourSet = 5U;               // 0x21fb44
constexpr std::uint32_t kColourDefault = 2U;           // 0x21fb58
constexpr std::uint32_t kPacketEnd = 3U + 1U;          // 0x21fb60, 0x21fb6c
constexpr std::uint32_t kReturn = 9U;                  // 0x21fb80

struct Part {
    GuestAddress chain;      // +0
    GuestAddress colour;     // +4
    std::uint32_t mask;      // +8 (half)
    std::uint32_t vi6;       // +13
    std::uint32_t vi9;       // +10
    std::uint32_t slot;      // +11
};

template <class Memory>
Part read_part(const Memory& m, const GuestAddress part) {
    return Part{m.read_u32_le(part), m.read_u32_le(part + 4U), m.read_u16_le(part + 8U), m.read_u8(part + 13U),
                m.read_u8(part + 10U), m.read_u8(part + 11U)};
}

template <class Context>
n3::Run draw_skinned_model(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    constexpr bool kLive = std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>;

    const GuestAddress object = call.arg(0U);
    const std::uint32_t caller_word = call.arg(1U);
    const GuestAddress matrix = call.arg(2U);
    const GuestAddress descriptor = call.arg(3U);
    const std::uint32_t part_mask = call.arg(4U) & 0xffffU;
    const std::uint32_t half_word = call.arg(5U) & 0xffffU;
    GuestAddress bone_source = call.arg(6U);
    const std::int32_t bone_total = static_cast<std::int32_t>(call.arg(7U));
    const std::uint32_t f12 = cpu.read_fpr_bits(12U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress work = m.read_u32_le(object + kContextWork);

    auto run_micro = [&](const GuestAddress vcallms, const std::uint16_t entry) {
        cpu.set_pc(vcallms);
        const auto launch = n3::run_vu0_micro_any(cpu, m, entry);
        if (launch.status == guest::Vu0MicroRunStatus::Completed) return true;
        if constexpr (kLive) {
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("DrawSkinnedModel: VU0 microprogram " + std::to_string(entry) +
                                     " did not complete: " + std::string(guest::to_string(launch.status)));
        }
        return false;
    };

    // 1. Uniform preparation on the VU0.
    m.write_u32_le(work + kWorkSkinFloat, m.read_u32_le(descriptor + 8U));
    for (unsigned i = 0U; i < 4U; ++i) cpu.write_vu0_vector(1U + i, n3::load_quad_any(m, matrix + 16U * i));
    for (unsigned i = 0U; i < 3U; ++i) {
        cpu.write_vu0_vector(5U + i, n3::load_quad_any(m, work + kWorkCameraSlow + 16U * i));
    }
    for (unsigned i = 0U; i < 4U; ++i) {
        cpu.write_vu0_vector(8U + i, n3::load_quad_any(m, work + kWorkStageCamera + 16U * i));
    }
    cpu.write_vu0_vector(12U, n3::load_quad_any(m, object + kContextCameraA));
    cpu.write_vu0_vector(13U, n3::load_quad_any(m, object + kContextCameraB));
    cpu.write_vu0_vector(14U, n3::load_quad_any(m, work + kWorkModelWord));
    ee_cop2::ctc2(cpu, 3U, m.read_u8(call.stack_pointer()));
    if (!run_micro(kPrepareVcallms, kMicroPrepare)) return call.decline();
    m.write_u32_le(work + kWorkQwords, kBoneCountStart);

    std::uint32_t retired = kPrepare;
    {
        GuestAddress destination = work + kWorkBones;
        for (std::int32_t bone = 0; bone < bone_total; ++bone) {
            if (retired > kCeiling) return call.finish(retired);
            const Gpr128 row0 = n3::load_quad_any(m, bone_source);
            const Gpr128 row1 = n3::load_quad_any(m, bone_source + 16U);
            const Gpr128 row2 = n3::load_quad_any(m, bone_source + 32U);
            const Gpr128 row3 = n3::load_quad_any(m, bone_source + 48U);
            n3::store_quad_any(m, destination, row0);
            n3::store_quad_any(m, destination + 16U, row1);
            n3::store_quad_any(m, destination + 32U, row2);
            n3::store_quad_any(m, destination + 48U, row3);
            bone_source += 64U;
            destination += 64U;
            retired += kBonePass;
        }
    }

    // 2. The chain header and the microprogram's outputs.
    m.write_u32_le(work + kWorkBoneCount, static_cast<std::uint32_t>(bone_total));
    const std::uint32_t staged = m.read_u32_le(work + kWorkQwords) + (static_cast<std::uint32_t>(bone_total) << 2U);
    m.write_u32_le(work + kWorkQwords, staged);
    {
        const std::uint32_t count = staged - 1U;
        const std::uint32_t head = count | kHeaderFlag;
        n3::store_quad_any(m, work + kWorkStage,
                           Gpr128{{head, (head & 0x80000000U) != 0U ? 0xffffffffU : 0U, kHeaderUnpackWord,
                                   kHeaderUnpackTag | (count << 16U)}});
    }
    m.write_u32_le(work + kWorkFloat, f12);
    m.write_u32_le(work + kWorkHalf, half_word);
    m.write_u32_le(work + kWorkCaller, caller_word);
    ee_cop2::ctc2(cpu, 4U, 0U);
    {
        // VF5..VF8, VF1..VF4, VF14, VF9..VF11 in the order the listing stores them.
        constexpr unsigned kOutputs[12] = {5U, 6U, 7U, 8U, 1U, 2U, 3U, 4U, 14U, 9U, 10U, 11U};
        for (unsigned i = 0U; i < 12U; ++i) {
            n3::store_quad_any(m, work + kWorkStage + 16U + 16U * i, cpu.read_vu0_vector(kOutputs[i]));
        }
    }
    retired += kStageSetup;

    // 3. One packet per matching part.
    for (std::int32_t part_index = 0;; ++part_index) {
        retired += kPartTest;
        if (part_index >= static_cast<std::int8_t>(m.read_u8(descriptor))) break;
        if (retired > kCeiling) return call.finish(retired);
        const GuestAddress part_address = m.read_u32_le(descriptor + 4U) + (static_cast<std::uint32_t>(part_index) << 4U);
        const Part part = read_part(m, part_address);
        if ((part_mask & part.mask) == 0U) {
            retired += kPartSkipped;
            continue;
        }

        ee_cop2::ctc2(cpu, 5U, part.slot);
        ee_cop2::ctc2(cpu, 4U, part.mask);
        ee_cop2::ctc2(cpu, 6U, part.vi6);
        ee_cop2::ctc2(cpu, 9U, part.vi9);
        for (unsigned i = 0U; i < 4U; ++i) {
            cpu.write_vu0_vector(1U + i, n3::load_quad_any(m, work + kWorkUniforms + 16U * i));
        }
        if (!run_micro(kPartVcallms, kMicroPart)) return call.decline();

        std::uint32_t pending = m.read_u32_le(work + kWorkQwords);
        call.set_arg64(0U, 0U);
        call.set_arg(1U, pending + kReserveExtra);
        const n3::Run reserve = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
        if (reserve.declined) return call.decline();
        retired += kPartHead + reserve.retired;

        GuestAddress out = m.read_u32_le(gp - kCurrentSegmentBelow);
        // The DMA call tag: ID in the first word, the part's chain address in the second (the tag's ADDR half).
        n3::store_quad_any(m, out, Gpr128{{kCallTag, part.chain, 0U, 0U}});
        n3::store_quad_any(m, out + 16U, n3::load_quad_any(m, work + kWorkTag));
        out += 32U;

        pending = m.read_u32_le(work + kWorkQwords);
        if (pending != 0U) {
            const std::int32_t total = static_cast<std::int32_t>(pending);
            const std::int32_t group_limit = static_cast<std::int32_t>(pending - 3U);
            GuestAddress source = work + kWorkStage;
            std::int32_t copied = 0;
            retired += kCopySetup + kGroupTest;
            while (copied < group_limit) {
                if (retired > kCeiling) return call.finish(retired);
                const Gpr128 q0 = n3::load_quad_any(m, source);
                const Gpr128 q1 = n3::load_quad_any(m, source + 16U);
                const Gpr128 q2 = n3::load_quad_any(m, source + 32U);
                const Gpr128 q3 = n3::load_quad_any(m, source + 48U);
                n3::store_quad_any(m, out, q0);
                n3::store_quad_any(m, out + 16U, q1);
                n3::store_quad_any(m, out + 32U, q2);
                n3::store_quad_any(m, out + 48U, q3);
                copied += 4;
                source += 64U;
                out += 64U;
                retired += kGroupPass + kGroupTest;
            }
            retired += kSingleSetup + kSingleTest;
            while (copied < total) {
                if (retired > kCeiling) return call.finish(retired);
                n3::store_quad_any(m, out, n3::load_quad_any(m, source));
                copied += 1;
                source += 16U;
                out += 16U;
                retired += kSinglePass + kSingleTest;
            }
            m.write_u32_le(work + kWorkQwords, 0U);
            retired += kStageCleared;
        }

        // Three state quadwords: the work area's two and the context slot's.
        n3::store_quad_any(m, out, n3::load_quad_any(m, work + kWorkStateX));
        n3::store_quad_any(m, out + 16U, n3::load_quad_any(m, work + kWorkStateY));
        n3::store_quad_any(m, out + 32U, n3::load_quad_any(m, object + (part.slot << 4U) + kContextSlots));
        out += 48U;

        // The microprogram's VF6..VF9 and VI7/VI8 go through the work area into seven more quadwords.
        for (unsigned i = 0U; i < 4U; ++i) {
            n3::store_quad_any(m, work + kWorkStateB + 16U * i, cpu.read_vu0_vector(6U + i));
        }
        m.write_u32_le(work + kWorkVi7, ee_cop2::cfc2(cpu, 7U));
        m.write_u64_le(work + kWorkVi8, ee_cop2::cfc2(cpu, 8U));

        n3::store_quad_any(m, out, n3::load_quad_any(m, work + kWorkStateLast));
        n3::store_quad_any(m, out + 16U, Gpr128{{m.read_u32_le(work + kWorkVi8), m.read_u32_le(work + kWorkVi8 + 4U),
                                                kAdMarker, 0U}});
        n3::store_quad_any(m, out + 32U, n3::load_quad_any(m, work + kWorkStateB));
        n3::store_quad_any(m, out + 48U, n3::load_quad_any(m, work + kWorkStateB + 16U));
        n3::store_quad_any(m, out + 64U, n3::load_quad_any(m, work + kWorkStateB + 32U));
        n3::store_quad_any(m, out + 80U, n3::load_quad_any(m, work + kWorkCaller));
        retired += kPacketBody;
        if (part.colour != 0U) {
            m.write_u32_le(work + kWorkColourWord, part.colour);
            n3::store_quad_any(m, out + 96U, n3::load_quad_any(m, work + kWorkColourQuad));
            retired += kColourSet;
        } else {
            n3::store_quad_any(m, out + 96U, n3::load_quad_any(m, work + kWorkDefaultColour));
            retired += kColourDefault;
        }
        m.write_u32_le(gp - kCurrentSegmentBelow, out + 112U);
        retired += kPacketEnd;
    }
    return call.finish(retired + kReturn);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(0021f790, 1044U, 0x26768db0f66d7f5dULL, "DrawSkinnedModel", takedown::runtime::kCeiling,
                   takedown::runtime::draw_skinned_model, EngineReturn::Nothing)
