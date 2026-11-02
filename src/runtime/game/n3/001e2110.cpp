// 0x001e2110 RwCameraSetViewWindow(camera, const float view_window[2]): stores the view window (width, height) into
// the camera, derives the two reciprocals the projection code uses and, when the camera has a frame, tells the frame
// code to update its objects (0x001e3fd0). Returns the camera.
//
//   camera + 4    frame pointer (0 = none)
//   camera + 104  view window width, camera + 108 height (the 8 bytes at `view_window`, which need not be aligned:
//                 the original copies them with an ldl/ldr, sdl/sdr pair)
//   camera + 112  1.0 / width, camera + 116  1.0 / height (R5900 division: n3::fpu::div)

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001e3fd0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFrameOffset = 4U;
constexpr GuestAddress kViewWindowOffset = 104U;
constexpr GuestAddress kInverseOffset = 112U;
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kFrameBytes = 32U;
constexpr std::uint32_t kWithoutFrame = 23U;   // 0x001e2110-0x001e2154 (18) + 0x001e2160-0x001e2170 (5)
constexpr std::uint32_t kCallSetup = 2U;       // jal + its delay slot, before the callee's own count

template <class Context>
n3::Run rw_camera_set_view_window(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress camera = call.arg(0);

    m.write_u64_le(camera + kViewWindowOffset, m.read_u64_le(call.arg(1)));
    const std::uint32_t width = m.read_u32_le(camera + kViewWindowOffset);
    const std::uint32_t height = m.read_u32_le(camera + kViewWindowOffset + 4U);
    m.write_u32_le(camera + kInverseOffset, n3::fpu::div(kOneBits, width));
    m.write_u32_le(camera + kInverseOffset + 4U, n3::fpu::div(kOneBits, height));

    call.return_value(camera);
    const std::uint32_t frame = m.read_u32_le(camera + kFrameOffset);
    if (frame == 0U) {
        return call.finish(kWithoutFrame);
    }
    call.set_arg(0, frame);
    const n3::Run update = call.invoke([](auto& mem) { return n3::fn_001e3fd0(mem); }, kFrameBytes);
    if (update.declined) {
        return call.decline();
    }
    call.return_value(camera);
    return call.finish(kWithoutFrame + kCallSetup + update.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001e2110, 100U, 0xb0d3327de1012dc4ULL, "RwCameraSetViewWindow", 47U, rw_camera_set_view_window,
         EngineReturn::Integer)
