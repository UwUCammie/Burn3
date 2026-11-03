// 0x00272ea0 (a0 = matrix M of three quadwords, a2 = descriptor {s32 count @+0, u32 points @+4} of 16-byte input points,
// a3 = output buffer, t0 = optional quadword to receive the last point's scaled result, 0 = none): BuildClippedPolygonVu0.
// The three VU0 microprograms turn a polygon into a clipped triangle/point list:
//
//  * 0x152 loads the matrix (VF1..VF3 = M);
//  * 0x15b runs once per input point (VF1 = the point). VI6 (sign-extended 16 bits) is the number of vertices of the
//    clipped polygon so far, VF2 the point's transformed position. A result below 3 means the polygon is gone: the
//    function returns 0 at once, and the outputs written so far stay as they are;
//  * if t0 is given, the last point's position VF2 is scaled by Q (xyz only) and stored there;
//  * 0x1b5 emits the first triangle (VF1..VF3 -> out +0, +16, +32), 0x1ba the following ones (three quadwords each, one
//    per three vertices) and 0x1bf the remaining single vertices (one quadword each). The result is the vertex count.
//
// Like the other VU0 launchers (DrawSlotVu0Launch 0x0026a7e0, LaunchVu0Program14A 0x00274af0) it first checks the word
// at gp-22808: when it is not 0x004787d0 (the first call of a run) it stores the tag, starts DMA channel 9 (registers
// 0x10008020 / 0x10008030 / 0x10008000) and waits for the channel with WordTableLookup10(0) and
// ReadBusyFlagOrWait(word, 1) until the latter returns 0. A dry run never sees the busy bit clear, so it counts above
// the ceiling and the original runs that call.
//
// With a point count of 0 or less the loop never runs and the vertex count and the scaled quadword come from the
// caller's s4 / s5 registers, as in the original (both are callee-saved, so they are read here and never changed).
// A dry run runs the microprograms too (n3::run_vu0_micro_any), so a caller's dry run sees the real results.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

N3_CALLEE(00103d20)
N3_CALLEE(001042f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kStartedTag = 0x004787d0U;       // lui 0x48, addiu -30768
constexpr GuestAddress kStartedFlagOffset = 0U - 22808U; // gp-relative
constexpr GuestAddress kDmaCount = 0x10008020U;
constexpr GuestAddress kDmaTag = 0x10008030U;
constexpr GuestAddress kDmaControl = 0x10008000U;
constexpr std::uint32_t kDmaStart = 325U;
constexpr std::uint32_t kFrameBytes = 128U;
constexpr unsigned kS4 = 20U;
constexpr unsigned kS5 = 21U;

constexpr std::uint16_t kMicroLoadMatrix = 0x152U;
constexpr std::uint16_t kMicroTransformPoint = 0x15bU;
constexpr std::uint16_t kMicroFirstTriangle = 0x1b5U;
constexpr std::uint16_t kMicroNextTriangle = 0x1baU;
constexpr std::uint16_t kMicroSingleVertex = 0x1bfU;
// The addresses of the VCALLMS instructions (the launches set the PC there, as the interpreter would have).
constexpr GuestAddress kVcallmsLoadMatrix = 0x00272f4cU;
constexpr GuestAddress kVcallmsTransformPoint = 0x00272f74U;
constexpr GuestAddress kVcallmsFirstTriangle = 0x00272fc8U;
constexpr GuestAddress kVcallmsNextTriangle = 0x00272ff4U;
constexpr GuestAddress kVcallmsSingleVertex = 0x00273028U;

constexpr std::uint8_t kMaskXyz = 14U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 16U;        // 0x00272ea0-0x00272edc
constexpr std::uint32_t kStartCost = 13U;        // 0x00272ee0-0x00272f18 on the one-time path (2 + 2 + 7 + 2)
constexpr std::uint32_t kWaitPassCost = 8U;      // 0x00272f20-0x00272f3c, without the callees
constexpr std::uint32_t kSetupCost = 9U;         // 0x00272f40-0x00272f60: matrix, launch, descriptor
constexpr std::uint32_t kLoopTestCost = 3U;      // slt, bne, nop (all three loops and the point loop)
constexpr std::uint32_t kPointCost = 12U;        // 0x00272f64-0x00272f90
constexpr std::uint32_t kPointNextCost = 1U;     // 0x00272f9c
constexpr std::uint32_t kPointGoneCost = 2U;     // 0x00272f94-0x00272f98, then the epilogue
constexpr std::uint32_t kScaleTestCost = 2U;     // 0x00272fac-0x00272fb0
constexpr std::uint32_t kScaleCost = 5U;         // 0x00272fb4-0x00272fc4
constexpr std::uint32_t kFirstTriangleCost = 11U;// 0x00272fc8-0x00272ff0
constexpr std::uint32_t kNextTriangleCost = 7U;  // 0x00272ff4-0x0027300c
constexpr std::uint32_t kBeforeSinglesCost = 3U; // 0x0027301c-0x00273024
constexpr std::uint32_t kSingleVertexCost = 5U;  // 0x00273028-0x00273038
constexpr std::uint32_t kReturnCost = 9U;        // 0x00273048-0x00273068

// The one-time DMA wait can spin forever, and loops over a stray vertex count are long: a run above this is left to
// the original (the dry run returns 0).
constexpr std::uint32_t kCeiling = 20000U;

// lqc2 / lq / sq / sqc2 fault unless the address is 16-byte aligned and inside guest memory; the original then takes
// the fault, so the native declines (the dry run runs first, so the live call never gets here).
template <class Memory>
[[nodiscard]] bool quad_ok(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

// A VU0 microprogram launch. The interpreter stops with a VU0 execution fault at a VCALLMS that does not complete; a
// live native cannot return that status.
template <class Context, class Memory>
[[nodiscard]] bool launch(Context& cpu_owner, Memory& m, const GuestAddress vcallms, const std::uint16_t entry) {
    auto& cpu = cpu_owner.cpu();
    cpu.set_pc(vcallms);
    const auto result = n3::run_vu0_micro_any(cpu, m, entry);
    if (result.status == guest::Vu0MicroRunStatus::Completed) {
        return true;
    }
    if constexpr (std::is_same_v<std::remove_cvref_t<Memory>, guest::EeMemory>) {
        throw std::runtime_error("BuildClippedPolygonVu0: VU0 microprogram " + std::to_string(entry) +
                                 " did not complete: " + std::string(guest::to_string(result.status)));
    } else {
        return false;
    }
}

template <class Context>
n3::Run build_clipped_polygon_vu0(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress matrix = call.arg(0U);
    const GuestAddress descriptor = call.arg(2U);
    const GuestAddress out = call.arg(3U);
    const bool has_scaled_out = call.arg64(4U) != 0U;
    const GuestAddress scaled_out = call.arg(4U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // The one-time DMA start and wait.
    std::uint32_t retired = kEntryCost;
    if (m.read_u32_le(gp + kStartedFlagOffset) != kStartedTag) {
        m.write_u32_le(gp + kStartedFlagOffset, kStartedTag);
        m.write_u32_le(kDmaCount, 0U);
        m.write_u32_le(kDmaTag, kStartedTag);
        m.write_u32_le(kDmaControl, kDmaStart);
        retired += kStartCost;
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            call.set_arg64(0U, 0U);
            const n3::Run lookup = call.invoke([](auto& machine) { return n3::fn_00103d20(machine); }, kFrameBytes);
            if (lookup.declined) {
                return call.decline();
            }
            call.set_arg64(0U, call.result64());
            call.set_arg(1U, 1U);
            call.set_arg64(2U, 0U);
            const n3::Run busy = call.invoke([](auto& machine) { return n3::fn_001042f0(machine); }, kFrameBytes);
            if (busy.declined) {
                return call.decline();
            }
            retired += kWaitPassCost + lookup.retired + busy.retired;
            if (call.result64() == 0U) {
                break;
            }
        }
    }

    // The matrix.
    if (!quad_ok(m, matrix + 0U) || !quad_ok(m, matrix + 16U) || !quad_ok(m, matrix + 32U)) {
        return call.decline();
    }
    for (unsigned row = 0U; row < 3U; ++row) {
        cpu.write_vu0_vector(1U + row, n3::load_quad_any(m, matrix + 16U * row));
    }
    if (!launch(call, m, kVcallmsLoadMatrix, kMicroLoadMatrix)) {
        return call.decline();
    }
    const std::int32_t count = static_cast<std::int32_t>(m.read_u32_le(descriptor + 0U));
    const GuestAddress points = m.read_u32_le(descriptor + 4U);
    retired += kSetupCost;

    // Without a point the vertex count and the scaled quadword are the caller's s4 / s5.
    std::int32_t vertices = static_cast<std::int16_t>(cpu.read_gpr_u32(kS4) & 0xffffU);
    Gpr128 last_position = cpu.read_gpr(kS5);

    // Every point goes through the transform; a vertex count below 3 drops the whole polygon.
    for (std::int32_t index = 0; index < count; ++index) {
        retired += kLoopTestCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress point = points + (static_cast<std::uint32_t>(index) << 4U);
        if (!quad_ok(m, point)) {
            return call.decline();
        }
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, point));
        if (!launch(call, m, kVcallmsTransformPoint, kMicroTransformPoint)) {
            return call.decline();
        }
        vertices = static_cast<std::int16_t>(ee_cop2::cfc2(cpu, 6U) & 0xffffU);
        last_position = cpu.read_vu0_vector(2U);
        retired += kPointCost;
        if (vertices < 3) {
            call.return_value(0U);
            return call.finish(retired + kPointGoneCost + kReturnCost);
        }
        retired += kPointNextCost;
    }
    retired += kLoopTestCost;

    // The last position scaled by Q (xyz), if the caller wants it.
    retired += kScaleTestCost;
    if (has_scaled_out) {
        if (!quad_ok(m, scaled_out)) {
            return call.decline();
        }
        cpu.write_vu0_vector(4U, last_position);
        ee_cop2::vmul_control(cpu, 4U, 4U, kMaskXyz, ee_cop2::kControlQ);  // vwaitq; vmulq.xyz vf4,vf4,q
        n3::store_quad_any(m, scaled_out, cpu.read_vu0_vector(4U));
        retired += kScaleCost;
    }

    // The first triangle.
    if (!launch(call, m, kVcallmsFirstTriangle, kMicroFirstTriangle)) {
        return call.decline();
    }
    if (!quad_ok(m, out + 0U) || !quad_ok(m, out + 16U) || !quad_ok(m, out + 32U)) {
        return call.decline();
    }
    for (unsigned row = 0U; row < 3U; ++row) {
        n3::store_quad_any(m, out + 16U * row, cpu.read_vu0_vector(1U + row));
    }
    retired += kFirstTriangleCost;

    // The remaining triangles, three vertices apiece, then single vertices.
    GuestAddress cursor = out + 48U;
    std::int32_t done = 3;
    while (retired += kLoopTestCost, done < vertices - 2) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (!launch(call, m, kVcallmsNextTriangle, kMicroNextTriangle)) {
            return call.decline();
        }
        if (!quad_ok(m, cursor + 0U) || !quad_ok(m, cursor + 16U) || !quad_ok(m, cursor + 32U)) {
            return call.decline();
        }
        for (unsigned row = 0U; row < 3U; ++row) {
            n3::store_quad_any(m, cursor + 16U * row, cpu.read_vu0_vector(1U + row));
        }
        cursor += 48U;
        done += 3;
        retired += kNextTriangleCost;
    }
    retired += kBeforeSinglesCost;
    while (retired += kLoopTestCost, done < vertices) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (!launch(call, m, kVcallmsSingleVertex, kMicroSingleVertex)) {
            return call.decline();
        }
        if (!quad_ok(m, cursor)) {
            return call.decline();
        }
        n3::store_quad_any(m, cursor, cpu.read_vu0_vector(1U));
        cursor += 16U;
        done += 1;
        retired += kSingleVertexCost;
    }

    call.return_value(static_cast<std::uint32_t>(vertices));
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(00272ea0, 460U, 0x4cc188773667fa64ULL, "BuildClippedPolygonVu0", takedown::runtime::kCeiling,
         takedown::runtime::build_clipped_polygon_vu0, EngineReturn::Integer)
