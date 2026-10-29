// Adapters for rendering: RenderWare camera set-up and the game's GS packet
// building.

#include "takedown/engine/camera/chase_camera.hpp"
#include "takedown/engine/math/axis_rotation.hpp"
#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/matrix_flags.hpp"
#include "takedown/engine/math/random.hpp"
#include "takedown/engine/math/trigonometry.hpp"
#include "takedown/engine/physics/box_contact.hpp"
#include "takedown/engine/world/camera_projection.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/world/lighting.hpp"
#include "takedown/engine/world/sun.hpp"
#include "takedown/engine/world/trail.hpp"
#include "takedown/engine/world/view_frustum.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_camera_fov.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>

namespace takedown::runtime {
namespace {

using guest::GuestAddress;

// RenderWare's RwCamera (3.x layout) and the RwFrame it hangs from.
struct CameraLayout {
    static constexpr GuestAddress kFrame = 4U;           // RwFrame*
    static constexpr GuestAddress kViewWindow = 104U;    // RwV2d
    static constexpr GuestAddress kViewOffset = 120U;    // RwV2d
    static constexpr GuestAddress kNearPlane = 128U;
    static constexpr GuestAddress kFarPlane = 132U;
    static constexpr GuestAddress kFrustumPlanes = 148U; // 6 RwFrustumPlane, 20 bytes each
    static constexpr GuestAddress kFrustumCorners = 292U;// 8 RwV3d
};
constexpr GuestAddress kFrameWorldMatrix = 80U;          // RwFrame::ltm (right, up, at, pos)
constexpr GuestAddress kFrustumPlaneBytes = 20U;

engine::Vector3 load3(const guest::EeMemory& memory, const GuestAddress address) {
    return {load_float(memory, address), load_float(memory, address + 4U), load_float(memory, address + 8U)};
}

void store3(guest::EeMemory& memory, const GuestAddress address, const engine::Vector3 value) {
    store_float(memory, address, value.x);
    store_float(memory, address + 4U, value.y);
    store_float(memory, address + 8U, value.z);
}

// BuildPerspectiveFrustum (original 0x001e0b48): a perspective RwCamera's
// frustum corners and planes from its frame's world matrix.
NativeFunctionExecution build_perspective_frustum(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress camera = call.arg(0);
    const GuestAddress matrix = memory.read_u32_le(camera + CameraLayout::kFrame) + kFrameWorldMatrix;
    engine::PerspectiveView view;
    view.right = load3(memory, matrix);
    view.up = load3(memory, matrix + 16U);
    view.at = load3(memory, matrix + 32U);
    view.position = load3(memory, matrix + 48U);
    view.window_x = load_float(memory, camera + CameraLayout::kViewWindow);
    view.window_y = load_float(memory, camera + CameraLayout::kViewWindow + 4U);
    view.offset_x = load_float(memory, camera + CameraLayout::kViewOffset);
    view.offset_y = load_float(memory, camera + CameraLayout::kViewOffset + 4U);
    view.near_distance = load_float(memory, camera + CameraLayout::kNearPlane);
    view.far_distance = load_float(memory, camera + CameraLayout::kFarPlane);

    const engine::ViewFrustum frustum = engine::build_view_frustum(view);
    for (int corner = 0; corner < 8; ++corner) {
        store3(memory, camera + CameraLayout::kFrustumCorners + static_cast<GuestAddress>(corner * 12), frustum.corners[corner]);
    }
    for (int plane = 0; plane < 6; ++plane) {
        const GuestAddress address = camera + CameraLayout::kFrustumPlanes + static_cast<GuestAddress>(plane) * kFrustumPlaneBytes;
        const engine::FrustumPlane& source = frustum.planes[plane];
        store3(memory, address, source.normal);
        store_float(memory, address + 12U, source.distance);
        for (GuestAddress axis = 0U; axis < 3U; ++axis) {
            memory.write_u8(address + 16U + axis, source.closest[axis]);
        }
    }
    return call.finish(760U);
}

// A view from a world matrix (right, up, at, position rows), a view window
// and offset (two floats each) and the near and far distances.
engine::PerspectiveView load_perspective_view(const guest::EeMemory& memory, const GuestAddress matrix,
                                              const GuestAddress window, const GuestAddress offset,
                                              const float near_distance, const float far_distance) {
    engine::PerspectiveView view;
    view.right = load3(memory, matrix);
    view.up = load3(memory, matrix + 16U);
    view.at = load3(memory, matrix + 32U);
    view.position = load3(memory, matrix + 48U);
    view.window_x = load_float(memory, window);
    view.window_y = load_float(memory, window + 4U);
    view.offset_x = load_float(memory, offset);
    view.offset_y = load_float(memory, offset + 4U);
    view.near_distance = near_distance;
    view.far_distance = far_distance;
    return view;
}

void store_side_planes(guest::EeMemory& memory, const GuestAddress address, const engine::SidePlanes& planes) {
    store_vector4(memory, address, {planes.x[0], planes.x[1], planes.x[2], planes.x[3]});
    store_vector4(memory, address + 16U, {planes.y[0], planes.y[1], planes.y[2], planes.y[3]});
    store_vector4(memory, address + 32U, {planes.z[0], planes.z[1], planes.z[2], planes.z[3]});
    store_vector4(memory, address + 48U,
                  {planes.distance[0], planes.distance[1], planes.distance[2], planes.distance[3]});
}

// The far and near planes (normal, distance), then the side planes.
void store_packed_frustum(guest::EeMemory& memory, const GuestAddress address, const engine::PackedFrustum& f) {
    store_vector4(memory, address, {f.far_normal.x, f.far_normal.y, f.far_normal.z, f.far_distance});
    store_vector4(memory, address + 16U, {f.near_normal.x, f.near_normal.y, f.near_normal.z, f.near_distance});
    store_side_planes(memory, address + 32U, f.sides);
}

// BuildPackedViewFrustum (original 0x00349980): the frustum of a view with
// world matrix a1 (right, up, at, position rows), view window a2 and offset
// a3 (two floats each), near f12 and far f13, written to a0 as the far and
// near planes (normal, distance) then four quadwords holding the side
// planes' x, y, z and distance components.
NativeFunctionExecution build_packed_view_frustum(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::PerspectiveView view =
        load_perspective_view(memory, call.arg(1), call.arg(2), call.arg(3), call.float_arg(0), call.float_arg(1));
    store_packed_frustum(memory, call.arg(0), engine::build_packed_frustum(view));
    return call.finish(320U);
}

// The game's limit of screen coordinates the GS rasteriser takes without
// clipping (its guard band), 2047.94 in the ELF.
constexpr GuestAddress kGuardBandLimit = 0x004e0a64U;  // gp - 31756

// BuildCullingFrustum (original 0x00334750): the culling frustum at a0
// (engine::Frustum's layout) from the RwCamera at a1: its first two planes
// (near, far) one per row, the other four transposed, one per lane. Normals
// pass through an add of 0 (so -0 becomes +0) and distances a multiply by 1,
// then the transposed ones an add of 0, like the original.
void write_culling_frustum(guest::EeMemory& memory, const GuestAddress frustum, const GuestAddress camera) {
    const GuestAddress planes = camera + CameraLayout::kFrustumPlanes;
    const auto plane = [&](const GuestAddress index) {
        const GuestAddress address = planes + index * kFrustumPlaneBytes;
        return engine::Vector4{0.0F + load_float(memory, address), 0.0F + load_float(memory, address + 4U),
                               0.0F + load_float(memory, address + 8U), load_float(memory, address + 12U)};
    };
    store_vector4(memory, frustum, plane(0U));
    store_vector4(memory, frustum + 16U, plane(1U));
    for (GuestAddress side = 0U; side < 4U; ++side) {
        const engine::Vector4 value = plane(2U + side);
        const float lanes[4] = {value.x, value.y, value.z, value.w};
        for (GuestAddress row = 0U; row < 4U; ++row) {
            store_float(memory, frustum + 32U + row * 16U + side * 4U, row == 3U ? 0.0F + lanes[row] : lanes[row]);
        }
    }
}

NativeFunctionExecution build_culling_frustum(NativeFunctionContext& context) {
    GuestCall call{context};
    write_culling_frustum(call.memory(), call.arg(0), call.arg(1));
    return call.finish(130U);
}

// The camera's planes the inner culling sides come from (an offset into the
// camera, plus 136).
constexpr GuestAddress kInnerCullingPlanesOffset = 0x004e285cU;

// BuildCullingFrustumWithInnerSides (original 0x00334ea0): BuildCullingFrustum,
// then at a0 + 96 the four planes from a1 + (the offset) + 136 (20 bytes
// apart) transposed, one per lane (normals plus 0, distances times 1).
void write_culling_frustum_with_inner_sides(guest::EeMemory& memory, const GuestAddress frustum,
                                           const GuestAddress camera) {
    write_culling_frustum(memory, frustum, camera);
    const GuestAddress planes = camera + memory.read_u32_le(kInnerCullingPlanesOffset) + 136U;
    for (GuestAddress side = 0U; side < 4U; ++side) {
        for (GuestAddress row = 0U; row < 4U; ++row) {
            const float value = load_float(memory, planes + side * 20U + row * 4U);
            store_float(memory, frustum + 96U + row * 16U + side * 4U, row == 3U ? value : 0.0F + value);
        }
    }
}

NativeFunctionExecution build_culling_frustum_with_inner_sides(NativeFunctionContext& context) {
    GuestCall call{context};
    write_culling_frustum_with_inner_sides(call.memory(), call.arg(0), call.arg(1));
    return call.finish(200U);
}

// BuildViewClipVolume (original 0x00334950): BuildPackedViewFrustum into a0
// (with the view window at a3 and offset at t0), then at +96 the side planes
// widened to the guard band of the screen size at a2 (two floats).
void write_view_clip_volume(guest::EeMemory& memory, const GuestAddress output, const GuestAddress matrix,
                            const GuestAddress screen, const GuestAddress window, const GuestAddress offset,
                            const float near_distance, const float far_distance) {
    const engine::PerspectiveView view =
        load_perspective_view(memory, matrix, window, offset, near_distance, far_distance);
    store_packed_frustum(memory, output, engine::build_packed_frustum(view));
    store_side_planes(memory, output + 96U,
                      engine::build_guard_band_planes(view, load_float(memory, screen), load_float(memory, screen + 4U),
                                                      load_float(memory, kGuardBandLimit)));
}

NativeFunctionExecution build_view_clip_volume(NativeFunctionContext& context) {
    GuestCall call{context};
    write_view_clip_volume(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3), call.arg(4),
                           call.float_arg(0), call.float_arg(1));
    return call.finish(660U);
}

// RenderWare frames and cameras. A frame's flags are byte 3; a frame whose
// hierarchy has changed puts its root (the pointer at +160) on the dirty list
// (head at 0x00670c8c, linked through the root's +8 and +12) once, until the
// next frame sync.
constexpr GuestAddress kRwFrameRoot = 160U;
constexpr GuestAddress kRwFrameFlags = 3U;
constexpr GuestAddress kRwFrameDirtyLink = 8U;
constexpr GuestAddress kRwFrameMatrix = 16U;  // the modelling matrix (an RwMatrix)
constexpr GuestAddress kRwDirtyFrames = 0x00670c8cU;
constexpr std::uint8_t kRwFrameHierarchyDirty = 0x03U;
constexpr std::uint8_t kRwFrameSubtreeDirty = 0x0cU;
constexpr GuestAddress kRwMatrixFlags = 12U;
constexpr std::uint32_t kRwMatrixClearedByUpdate = 0x00020003U;  // orthonormal, normal, identity
constexpr GuestAddress kRwObjectParent = 4U;      // an RwCamera's frame
constexpr GuestAddress kRwCameraViewWindow = 104U;  // two floats, then their inverses

// Returns the original's instruction count.
std::uint32_t rw_frame_update_objects(guest::EeMemory& memory, const GuestAddress frame) {
    const GuestAddress root = memory.read_u32_le(frame + kRwFrameRoot);
    const std::uint8_t root_flags = memory.read_u8(root + kRwFrameFlags);
    const bool newly_dirty = (root_flags & kRwFrameHierarchyDirty) == 0U;
    if (newly_dirty) {
        const GuestAddress link = root + kRwFrameDirtyLink;
        memory.write_u32_le(link + 4U, kRwDirtyFrames);
        memory.write_u32_le(link, memory.read_u32_le(kRwDirtyFrames));
        memory.write_u32_le(memory.read_u32_le(kRwDirtyFrames) + 4U, link);
        memory.write_u32_le(kRwDirtyFrames, link);
    }
    memory.write_u8(root + kRwFrameFlags, static_cast<std::uint8_t>(root_flags | kRwFrameHierarchyDirty));
    memory.write_u8(frame + kRwFrameFlags,
                    static_cast<std::uint8_t>(memory.read_u8(frame + kRwFrameFlags) | kRwFrameSubtreeDirty));
    return newly_dirty ? 22U : 13U;
}

std::uint32_t rw_camera_set_view_window(guest::EeMemory& memory, const GuestAddress camera,
                                        const GuestAddress window) {
    const GuestAddress view_window = camera + kRwCameraViewWindow;
    memory.write_u32_le(view_window, memory.read_u32_le(window));
    memory.write_u32_le(view_window + 4U, memory.read_u32_le(window + 4U));
    store_float(memory, view_window + 8U, 1.0F / load_float(memory, view_window));
    store_float(memory, view_window + 12U, 1.0F / load_float(memory, view_window + 4U));
    const GuestAddress frame = memory.read_u32_le(camera + kRwObjectParent);
    return 25U + (frame != 0U ? rw_frame_update_objects(memory, frame) : 0U);
}

// RwMatrixUpdate (original 0x001da860): clears the matrix a0's orthonormal
// and identity flags; returns the matrix.
NativeFunctionExecution rw_matrix_update(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress flags = call.arg(0) + kRwMatrixFlags;
    memory.write_u32_le(flags, memory.read_u32_le(flags) & ~kRwMatrixClearedByUpdate);
    call.return_value(call.arg(0));
    return call.finish(7U);
}

// RwFrameUpdateObjects (original 0x001e3fd0): marks frame a0 changed, putting
// its root on the dirty list; returns the frame.
NativeFunctionExecution rw_frame_update_objects(NativeFunctionContext& context) {
    GuestCall call{context};
    const std::uint32_t cost = rw_frame_update_objects(call.memory(), call.arg(0));
    call.return_value(call.arg(0));
    return call.finish(cost);
}

// RwCameraSetViewWindow (original 0x001e2110): camera a0 takes the view
// window at a1 (two floats); returns the camera.
NativeFunctionExecution rw_camera_set_view_window(NativeFunctionContext& context) {
    GuestCall call{context};
    const std::uint32_t cost = rw_camera_set_view_window(call.memory(), call.arg(0), call.arg(1));
    call.return_value(call.arg(0));
    return call.finish(cost);
}

// StoreQuadword (original 0x00130740): stores the 128-bit value in a1 at a0
// (a vector passed by value); returns a0.
NativeFunctionExecution store_quadword(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress destination = call.arg(0) & ~15U;
    const auto value = call.arg128(1);
    for (GuestAddress word = 0U; word < 4U; ++word) {
        memory.write_u32_le(destination + word * 4U, value.words[word]);
    }
    call.return_value(call.arg(0));
    return call.finish(3U);
}

void store_screen_projection(guest::EeMemory& memory, const GuestAddress at,
                             const engine::ScreenProjection& projection) {
    for (GuestAddress row = 0U; row < 4U; ++row) {
        const engine::Vector3 value = projection.rows[row];
        store_vector4(memory, at + row * 16U, {value.x, value.y, value.z, 0.0F});
    }
}

// The game's camera record (UpdateCameraFromOrientation's a0).
struct GameCameraLayout {
    static constexpr GuestAddress kProjection = 0U;    // engine::ScreenProjection rows
    static constexpr GuestAddress kWindowOffset = 64U;  // two floats
    static constexpr GuestAddress kViewWindow = 72U;    // two floats, then their inverses
    static constexpr GuestAddress kRwCamera = 88U;      // pointer
    static constexpr GuestAddress kSource = 104U;       // pointer
    static constexpr GuestAddress kFieldOfView = 108U;
    static constexpr GuestAddress kWindowScale = 112U;
    static constexpr GuestAddress kAspectRatio = 116U;
};
// What the camera follows: field of view at +4, a scaled orientation
// quaternion at +16 and the position at +32.
struct CameraSourceLayout {
    static constexpr GuestAddress kFieldOfView = 4U;
    static constexpr GuestAddress kOrientation = 16U;
    static constexpr GuestAddress kPosition = 32U;
};
constexpr GuestAddress kQuaternionScale = 0x004e07b8U;   // square root of two
constexpr GuestAddress kFieldOfViewToRadians = 0x004e06f0U;

// The strength of camera shake record `shake`, peaking at 1 on both of its
// phases (+28 and +32, each 0 to 2): scale * (1 - (b - 1)^2) * (1 - (a - 1)^2),
// in the game's rounding.
constexpr GuestAddress kCameraShakeScale = 0x004e0fc8U;
float camera_shake_strength(const guest::EeMemory& memory, const GuestAddress shake) {
    const auto bell = [](const float phase) {
        const float centred = engine::chop_sub(phase, 1.0F);
        return engine::chop_sub(1.0F, engine::chop_mul(centred, centred));
    };
    return engine::chop_mul(
        engine::chop_mul(load_float(memory, kCameraShakeScale), bell(load_float(memory, shake + 32U))),
        bell(load_float(memory, shake + 28U)));
}

// CameraShakeEnvelope (original 0x0030a990): camera_shake_strength of a0.
NativeFunctionExecution camera_shake_envelope(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_float(camera_shake_strength(call.memory(), call.arg(0)));
    return call.finish(14U);
}

// The radial blur record (UpdateRadialBlur's a0): where the blur centres on
// screen, how it falls off and how strong it is, and the point ahead of the
// camera it was last measured from.
struct RadialBlurLayout {
    static constexpr GuestAddress kAhead = 0U;  // 10 ahead of the camera
    static constexpr GuestAddress kCentreU = 16U;
    static constexpr GuestAddress kCentreV = 20U;
    static constexpr GuestAddress kFalloffU = 24U;
    static constexpr GuestAddress kFalloffV = 28U;
    static constexpr GuestAddress kStrength = 32U;
    static constexpr GuestAddress kTime = 36U;
};
constexpr GuestAddress kGameCamera = 0x00666270U;     // pointer to the game camera record
constexpr GuestAddress kCameraClock = 0x004f50a0U;
constexpr GuestAddress kCrashClock = 0x00522680U;
constexpr GuestAddress kPlayerViews = 0x00522660U;    // per player, 10192 bytes
constexpr GuestAddress kPlayerViewSize = 10192U;
constexpr GuestAddress kViewShake = 0x12bedcU;        // the player's camera shake record
constexpr GuestAddress kViewCrash = 0x12d180U;        // pointer; +13620 is when the crash began
constexpr GuestAddress kCrashStart = 13620U;
constexpr GuestAddress kShakeActive = 82U;            // u8
constexpr GuestAddress kPlayerFlags = 0x005179c0U;    // per player, 2992 bytes
constexpr GuestAddress kPlayerFlagsSize = 2992U;
constexpr std::uint32_t kFlagNoCrashWobble = 0x2000U;
constexpr std::uint32_t kFlagAlwaysBlur = 0x400U;
constexpr std::uint32_t kFlagShakeBlur = 0x4U | 0x20U;
constexpr GuestAddress kCrashWobbleRates = 0x004d1540U;    // four angular rates
constexpr GuestAddress kCrashWobbleWeights = 0x004d1550U;  // x, x, v, v
constexpr GuestAddress kBlurQuaternionScale = 0x004e0ed4U;  // square root of two
constexpr GuestAddress kCrashWobbleTime = 0x004e0f5cU;      // 0.3
constexpr GuestAddress kCrashWobbleRate = 0x004e0fc0U;      // 1 / 0.3
constexpr GuestAddress kCrashFalloffBase = 0x004e0e24U;     // 0.1
constexpr GuestAddress kCrashFalloffScale = 0x004e0ee4U;    // 0.9
constexpr GuestAddress kBlurFalloff = 0x004e0fc4U;          // 0.012
constexpr GuestAddress kTurnBlurScale = 0x004e0fccU;        // 1 / 49
constexpr GuestAddress kTurnBlurLeast = 0x004e0fd0U;        // 2 / 255
constexpr GuestAddress kTurnBlurNearest = 0x004e0c6cU;      // 0.01
constexpr GuestAddress kTurnBlurSpread = 0x004e0fd4U;       // 0.998
constexpr float kBlurAhead = 10.0F;

// UpdateRadialBlur (original 0x004256c0): the radial blur of player a1's
// view. Just after a crash it wobbles around the screen centre (four
// cosines); otherwise, when the player's flags or camera shake call for it,
// it centres where the point that was ahead of the camera last time now is
// on screen, as strong as the camera turned. Returns 1 when the record was
// set (even to nothing), 0 when there is no blur.
NativeFunctionExecution update_radial_blur(NativeFunctionContext& context) {
    using engine::chop_add;
    using engine::chop_mul;
    using engine::chop_sub;
    using B = RadialBlurLayout;
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress blur = call.arg(0);
    const GuestAddress player = call.arg(1);
    const GuestAddress view = kPlayerViews + player * kPlayerViewSize;
    const GuestAddress shake = view + kViewShake;
    const std::uint32_t flags = memory.read_u32_le(kPlayerFlags + player * kPlayerFlagsSize);
    const float now = load_float(memory, kCameraClock);
    const float then = load_float(memory, blur + B::kTime);
    const engine::Vector4 last_ahead = load_vector4(memory, blur + B::kAhead);

    const GuestAddress source = memory.read_u32_le(memory.read_u32_le(kGameCamera) + GameCameraLayout::kSource);
    const engine::Vector4 orientation = load_vector4(memory, source + CameraSourceLayout::kOrientation);
    const float scale = load_float(memory, kBlurQuaternionScale);
    const engine::Vector4 q{chop_mul(orientation.x, scale), chop_mul(orientation.y, scale),
                            chop_mul(orientation.z, scale), chop_mul(orientation.w, scale)};
    const float xx = chop_mul(q.x, q.x);
    const float xy = chop_mul(q.x, q.y);
    const float xz = chop_mul(q.x, q.z);
    const float yy = chop_mul(q.y, q.y);
    const float yz = chop_mul(q.y, q.z);
    const float zz = chop_mul(q.z, q.z);
    const float wx = chop_mul(q.w, q.x);
    const float wy = chop_mul(q.w, q.y);
    const float wz = chop_mul(q.w, q.z);
    const engine::Vector3 right{chop_sub(chop_sub(1.0F, yy), zz), chop_add(xy, wz), chop_sub(xz, wy)};
    const engine::Vector3 up{chop_sub(xy, wz), chop_sub(chop_sub(1.0F, xx), zz), chop_add(yz, wx)};
    const engine::Vector3 at{chop_add(xz, wy), chop_sub(yz, wx), chop_sub(chop_sub(1.0F, xx), yy)};
    const engine::Vector4 position = load_vector4(memory, source + CameraSourceLayout::kPosition);
    // The at row's fourth word is never set: it is what the stack held.
    const float at_w = load_float(memory, call.stack_pointer() - 1444U);
    store_vector4(memory, blur + B::kAhead,
                  {chop_add(position.x, chop_mul(at.x, kBlurAhead)), chop_add(position.y, chop_mul(at.y, kBlurAhead)),
                   chop_add(position.z, chop_mul(at.z, kBlurAhead)), at_w});
    store_float(memory, blur + B::kTime, now);
    store_float(memory, blur + B::kStrength, 0.0F);

    const auto finish = [&](const std::uint32_t result, const std::uint32_t cost) {
        call.return_value(result);
        return call.finish(cost);
    };
    const GuestAddress crash = memory.read_u32_le(view + kViewCrash);
    if (crash == 0U) {
        return finish(0U, 560U);
    }
    const float crash_start = load_float(memory, crash + kCrashStart);
    if (engine::game_less(0.0F, crash_start) && (flags & kFlagNoCrashWobble) == 0U) {
        const float since = chop_sub(load_float(memory, kCrashClock), crash_start);
        if (engine::game_less(since, load_float(memory, kCrashWobbleTime))) {
            float wave[4];
            for (GuestAddress lane = 0U; lane < 4U; ++lane) {
                const float angle = chop_mul(load_float(memory, kCrashWobbleRates + lane * 4U), since);
                wave[lane] = chop_mul(engine::wave_cosine_chopped(angle),
                                      load_float(memory, kCrashWobbleWeights + lane * 4U));
            }
            const float u = chop_add(wave[0], wave[1]);
            const float v = chop_add(wave[2], wave[3]);
            const float fade = chop_mul(load_float(memory, kCrashWobbleRate), since);
            const float strength = chop_sub(1.0F, chop_mul(fade, fade));
            const float offset = engine::game_sqrt(chop_add(chop_add(0.0F, chop_mul(v, v)), chop_mul(u, u)));
            const float spread = chop_mul(load_float(memory, kBlurFalloff),
                                          engine::chop_madd(chop_add(0.0F, load_float(memory, kCrashFalloffBase)),
                                                            load_float(memory, kCrashFalloffScale), strength));
            const float falloff = chop_sub(1.0F, engine::game_div(spread, chop_add(0.5F, offset)));
            store_float(memory, blur + B::kCentreU, chop_add(u, 0.5F));
            store_float(memory, blur + B::kCentreV, chop_add(v, 0.5F));
            store_float(memory, blur + B::kStrength, strength);
            store_float(memory, blur + B::kFalloffU, falloff);
            store_float(memory, blur + B::kFalloffV, falloff);
            return finish(1U, 723U);
        }
    }
    const bool shaking = memory.read_u8(shake + kShakeActive) != 0U;
    if ((flags & kFlagAlwaysBlur) == 0U) {
        if (!shaking || !engine::game_less(0.0F, camera_shake_strength(memory, shake))) {
            return finish(0U, shaking ? 611U : 590U);
        }
    }
    const float elapsed = chop_sub(now, then);
    if (!engine::game_less(0.0F, elapsed)) {
        return finish(1U, 640U);
    }
    // The last point ahead in the camera's own axes, less the 10 ahead it
    // started at: how far the view has swung, per second.
    const auto along = [&](const engine::Vector3 point, const float a, const float b, const float c) {
        return engine::chop_madd(engine::chop_madd(chop_mul(point.x, a), point.y, b), point.z, c);
    };
    const engine::Vector3 offset{chop_sub(0.0F, along(position.xyz(), right.x, right.y, right.z)),
                                 chop_sub(0.0F, along(position.xyz(), up.x, up.y, up.z)),
                                 chop_sub(0.0F, along(position.xyz(), at.x, at.y, at.z))};
    const auto local = [&](const float a, const float b, const float c, const float shift) {
        return chop_add(along(last_ahead.xyz(), a, b, c), shift);
    };
    const engine::Vector3 swing{local(right.x, right.y, right.z, offset.x), local(up.x, up.y, up.z, offset.y),
                                chop_sub(local(at.x, at.y, at.z, offset.z), kBlurAhead)};
    const float per_second = engine::game_div(1.0F, elapsed);
    engine::Vector3 rate{chop_mul(swing.x, per_second), chop_mul(swing.y, per_second), chop_mul(swing.z, per_second)};
    const float turned = engine::chop_sqrt(
        engine::chop_madd(engine::chop_madd(chop_mul(rate.x, rate.x), rate.y, rate.y), rate.z, rate.z));
    float strength = chop_mul(load_float(memory, kTurnBlurScale), chop_sub(turned, 1.0F));
    if (engine::game_less(strength, load_float(memory, kTurnBlurLeast))) {
        return finish(1U, 700U);
    }
    if (engine::game_less(1.0F, strength)) {
        strength = 1.0F;
    }
    if (engine::game_less(rate.z, 0.0F)) {
        rate = {chop_sub(0.0F, rate.x), chop_sub(0.0F, rate.y), chop_sub(0.0F, rate.z)};
    }
    const float spread = engine::game_div(load_float(memory, kTurnBlurSpread),
                                          engine::maximum(rate.z, load_float(memory, kTurnBlurNearest)));
    const float screen_x = chop_mul(rate.x, spread);
    const float screen_y = chop_mul(rate.y, spread);
    const float corner_u = engine::game_less(0.0F, screen_x) ? 0.0F : 1.0F;
    const float corner_v = engine::game_less(0.0F, screen_y) ? 0.0F : 1.0F;
    const float u = chop_add(screen_x, 0.5F);
    const float v = chop_add(screen_y, 0.5F);
    const float du = chop_sub(u, corner_u);
    const float dv = chop_sub(v, corner_v);
    const float reach = engine::game_sqrt(engine::chop_madd(chop_mul(dv, dv), du, du));
    const float falloff = chop_sub(
        1.0F, engine::game_div(chop_mul(load_float(memory, kBlurFalloff),
                                        engine::chop_madd(chop_add(0.0F, 0.5F), 0.5F, strength)),
                               reach));
    store_float(memory, blur + B::kCentreU, u);
    store_float(memory, blur + B::kCentreV, v);
    store_float(memory, blur + B::kFalloffU, falloff);
    store_float(memory, blur + B::kFalloffV, falloff);
    std::uint32_t cost = 750U;
    if (shaking) {
        cost += 40U;
        if ((flags & kFlagShakeBlur) != 0U) {
            strength = chop_mul(strength, camera_shake_strength(memory, shake));
            cost += 14U;
        }
    }
    store_float(memory, blur + B::kStrength, strength);
    return finish(1U, cost);
}

// UpdateCameraFromOrientation (original 0x00211ed0): sets the RenderWare
// camera's frame from the source's orientation and position, its view window
// from the source's field of view, and the camera's screen projection.
NativeFunctionExecution update_camera_from_orientation(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress camera = call.arg(0);
    const GuestAddress source = memory.read_u32_le(camera + GameCameraLayout::kSource);
    const float quaternion_scale = load_float(memory, kQuaternionScale);
    const engine::Vector4 orientation = load_vector4(memory, source + CameraSourceLayout::kOrientation);
    const engine::CameraBasis basis = engine::basis_from_scaled_quaternion(
        {orientation.x * quaternion_scale, orientation.y * quaternion_scale, orientation.z * quaternion_scale,
         orientation.w * quaternion_scale});
    const engine::Vector3 position = load_vector4(memory, source + CameraSourceLayout::kPosition).xyz();
    const GuestAddress rw_camera = memory.read_u32_le(camera + GameCameraLayout::kRwCamera);
    const GuestAddress frame = memory.read_u32_le(rw_camera + kRwObjectParent);
    const engine::Vector3 rows[4] = {basis.right, basis.up, basis.at, position};
    for (GuestAddress row = 0U; row < 4U; ++row) {
        store_float(memory, frame + kRwFrameMatrix + row * 16U, rows[row].x);
        store_float(memory, frame + kRwFrameMatrix + row * 16U + 4U, rows[row].y);
        store_float(memory, frame + kRwFrameMatrix + row * 16U + 8U, rows[row].z);
    }
    memory.write_u32_le(frame + kRwFrameMatrix + kRwMatrixFlags, 3U);
    rw_frame_update_objects(memory, frame);
    float field_of_view = load_float(memory, source + CameraSourceLayout::kFieldOfView);
    // The FIELD OF VIEW option (guest hook at 0x00212288, which this
    // function replaces): its FOV and its camera move, at the same point.
    if (Slus21050CameraFov* const fov = Slus21050CameraFov::native_instance(); fov != nullptr) {
        field_of_view = fov->on_native_camera(memory, frame, field_of_view);
    }
    const float half_angle = load_float(memory, kFieldOfViewToRadians) * (0.5F * field_of_view);
    const auto tangent = static_cast<float>(std::tan(static_cast<double>(half_angle)));
    const GuestAddress window = camera + GameCameraLayout::kViewWindow;
    store_float(memory, window, load_float(memory, camera + GameCameraLayout::kWindowScale) * tangent);
    store_float(memory, window + 4U, load_float(memory, window) / load_float(memory, camera + GameCameraLayout::kAspectRatio));
    store_float(memory, window + 8U, 1.0F / load_float(memory, window));
    store_float(memory, window + 12U, 1.0F / load_float(memory, window + 4U));
    rw_camera_set_view_window(memory, rw_camera, window);
    store_float(memory, camera + GameCameraLayout::kFieldOfView, field_of_view);
    store_screen_projection(memory, camera + GameCameraLayout::kProjection,
                            engine::screen_projection(basis, position, load_float(memory, window + 8U),
                                                      load_float(memory, window + 12U),
                                                      load_float(memory, camera + GameCameraLayout::kWindowOffset),
                                                      load_float(memory, camera + GameCameraLayout::kWindowOffset + 4U)));
    return call.finish(1100U);
}

// A render view (SetUpRenderView's a0): its camera matrix, screen
// projection (also with z repeated in the fourth lane, for the vector unit),
// screen scales and clip guards, the GS registers that select its frame
// and depth buffers, and its view window.
struct RenderViewLayout {
    static constexpr GuestAddress kCameraMatrix = 160U;  // right, up, at, position
    static constexpr GuestAddress kProjection = 224U;
    static constexpr GuestAddress kPackedProjection = 288U;
    static constexpr GuestAddress kScreenScale = 352U;
    static constexpr GuestAddress kScreenOffset = 368U;
    static constexpr GuestAddress kClipGuards = 384U;    // four vectors
    static constexpr GuestAddress kGsRegisters = 448U;   // FRAME, ZBUF, XYOFFSET, SCISSOR (data, address)
    static constexpr GuestAddress kViewWindow = 512U;    // two floats, then their inverses
    static constexpr GuestAddress kWindowOffset = 528U;  // two floats
    static constexpr GuestAddress kScreenSize = 536U;    // two floats, then two zero words
};
constexpr GuestAddress kDepthBias = 0x004e0a68U;
constexpr GuestAddress kDepthRange = 0x004e0a6cU;
constexpr GuestAddress kDepthNear = 0x004e0a70U;
constexpr GuestAddress kClipGuardBase = 0x004e0a64U;
constexpr GuestAddress kClipGuardX = 0x004e0a74U;
constexpr GuestAddress kClipGuardY = 0x004e0a78U;
constexpr GuestAddress kRenderNear = 0x004e091cU;
constexpr GuestAddress kScreenScaleW = 0x004e0a7cU;
constexpr GuestAddress kDefaultViewWindow = 0x004b9da0U;
constexpr GuestAddress kRenderFlags = 0x004e18d0U;  // u8; SetUpRenderView clears bit 3
constexpr GuestAddress kFrameBufferBase = 0x006662dcU;  // GS words, added to the view's base
constexpr GuestAddress kDepthBufferBase = 0x006662d8U;
constexpr float kRenderFar = 100.0F;
constexpr std::uint32_t kGsFrame1 = 0x4cU;
constexpr std::uint32_t kGsZbuf1 = 0x4eU;
constexpr std::uint32_t kGsXyOffset1 = 0x18U;
constexpr std::uint32_t kGsScissor1 = 0x40U;
constexpr std::uint32_t kZbufFormat = 0x01000000U;

void store_gs_register(guest::EeMemory& memory, const GuestAddress at, const std::uint64_t data,
                       const std::uint32_t address) {
    memory.write_u64_le(at, data);
    memory.write_u64_le(at + 8U, address);
}

// SetUpRenderView (original 0x00225440): sets up render view a0 for a
// t0 x t1 target at GS word t2 from the camera matrix at a1, with a unit view
// window when a3 is set (else the default one); returns the GS word after
// the target.
using CameraMatrix = std::array<engine::Vector4, 4>;  // right, up, at, position

std::uint32_t write_render_view(guest::EeMemory& memory, const GuestAddress view, const CameraMatrix& camera_matrix,
                                const bool unit_window, const std::int32_t width, const std::int32_t height,
                                const std::uint32_t base) {
    for (GuestAddress row = 0U; row < 4U; ++row) {
        store_vector4(memory, view + RenderViewLayout::kCameraMatrix + row * 16U, camera_matrix[row]);
    }
    const float depth_bias = load_float(memory, kDepthBias);
    const float depth_span = 16777215.0F + depth_bias;
    const float depth_scale = ((0.0F - depth_bias) - depth_span) / load_float(memory, kDepthRange);
    const float depth_offset =
        0.5F * ((0.0F + ((0.0F - depth_bias) + depth_span)) - depth_scale * load_float(memory, kDepthNear));
    const GuestAddress window = view + RenderViewLayout::kViewWindow;
    if (unit_window) {
        for (GuestAddress word = 0U; word < 16U; word += 4U) {
            store_float(memory, window + word, 1.0F);
        }
        store_float(memory, view + RenderViewLayout::kWindowOffset, 0.0F);
        store_float(memory, view + RenderViewLayout::kWindowOffset + 4U, 0.0F);
    } else {
        const float size = load_float(memory, kDefaultViewWindow);
        const float inverse = 1.0F / size;
        store_float(memory, window, size);
        store_float(memory, window + 4U, size);
        store_float(memory, window + 8U, inverse);
        store_float(memory, window + 12U, inverse);
        store_float(memory, view + RenderViewLayout::kWindowOffset, -(size - 1.0F));
        store_float(memory, view + RenderViewLayout::kWindowOffset + 4U, size - 1.0F);
    }
    const GuestAddress matrix = view + RenderViewLayout::kCameraMatrix;
    const engine::CameraBasis basis{load_vector4(memory, matrix).xyz(), load_vector4(memory, matrix + 16U).xyz(),
                                    load_vector4(memory, matrix + 32U).xyz()};
    const engine::ScreenProjection projection = engine::screen_projection(
        basis, load_vector4(memory, matrix + 48U).xyz(), load_float(memory, window + 8U),
        load_float(memory, window + 12U), load_float(memory, view + RenderViewLayout::kWindowOffset),
        load_float(memory, view + RenderViewLayout::kWindowOffset + 4U));
    store_screen_projection(memory, view + RenderViewLayout::kProjection, projection);
    for (GuestAddress row = 0U; row < 4U; ++row) {
        const engine::Vector3 value = projection.rows[row];
        store_vector4(memory, view + RenderViewLayout::kPackedProjection + row * 16U,
                      {value.x, value.y, value.z, value.z});
    }
    memory.write_u8(kRenderFlags, static_cast<std::uint8_t>(memory.read_u8(kRenderFlags) & 0xf7U));
    const std::int32_t half_width = width >> 1;
    const std::int32_t half_height = height >> 1;
    const float guard_base = load_float(memory, kClipGuardBase);
    const float guard_x = load_float(memory, kClipGuardX);
    const float guard_y = load_float(memory, kClipGuardY) * load_float(memory, kDepthNear);
    const float render_near = load_float(memory, kRenderNear);
    const auto guard = [&](const float extent, const float z, const float w) {
        return engine::Vector4{0.0F + extent, 0.0F + 1.0F / extent, 0.0F + z, w};
    };
    const GuestAddress guards = view + RenderViewLayout::kClipGuards;
    store_vector4(memory, guards, guard(static_cast<float>(half_width + 5), guard_x, kRenderFar));
    store_vector4(memory, guards + 16U, guard(static_cast<float>(half_height + 5), guard_y, render_near));
    store_vector4(memory, guards + 32U, guard(guard_base - static_cast<float>(half_width), guard_x, kRenderFar));
    store_vector4(memory, guards + 48U, guard(guard_base - static_cast<float>(half_height), guard_y, render_near));
    const std::uint32_t frame_page = (base + memory.read_u32_le(kFrameBufferBase)) >> 5U;
    const std::uint32_t depth_page = (base + memory.read_u32_le(kDepthBufferBase)) >> 5U;
    const auto pixels = static_cast<std::int32_t>(static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height));
    const GuestAddress registers = view + RenderViewLayout::kGsRegisters;
    store_gs_register(memory, registers, frame_page | (static_cast<std::uint32_t>(width / 64) << 16U), kGsFrame1);
    store_gs_register(memory, registers + 16U, depth_page | kZbufFormat, kGsZbuf1);
    store_gs_register(memory, registers + 32U,
                      (static_cast<std::uint64_t>(2048 - static_cast<std::int64_t>(half_width)) << 4U) |
                          (static_cast<std::uint64_t>(2048 - static_cast<std::int64_t>(half_height)) << 36U),
                      kGsXyOffset1);
    store_gs_register(memory, registers + 48U,
                      (static_cast<std::uint64_t>(static_cast<std::int64_t>(width - 1)) << 16U) |
                          (static_cast<std::uint64_t>(static_cast<std::int64_t>(height - 1)) << 48U),
                      kGsScissor1);
    store_vector4(memory, view + RenderViewLayout::kScreenScale,
                  {static_cast<float>(width), static_cast<float>(height), depth_scale,
                   load_float(memory, kScreenScaleW)});
    store_vector4(memory, view + RenderViewLayout::kScreenOffset, {2048.0F, 2048.0F, depth_offset, 100.0F});
    store_float(memory, view + RenderViewLayout::kScreenSize, static_cast<float>(width));
    store_float(memory, view + RenderViewLayout::kScreenSize + 4U, static_cast<float>(height));
    memory.write_u32_le(view + RenderViewLayout::kScreenSize + 8U, 0U);
    memory.write_u32_le(view + RenderViewLayout::kScreenSize + 12U, 0U);
    write_view_clip_volume(memory, view, matrix, view + RenderViewLayout::kScreenSize, window,
                           view + RenderViewLayout::kWindowOffset, render_near, kRenderFar);
    return base + static_cast<std::uint32_t>(pixels / 64);
}

// The original's instructions for one render view (with BuildViewClipVolume).
constexpr std::uint32_t kRenderViewCost = 1330U;

NativeFunctionExecution set_up_render_view(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress source = call.arg(1) & ~15U;
    const CameraMatrix camera_matrix{load_vector4(memory, source), load_vector4(memory, source + 16U),
                                     load_vector4(memory, source + 32U), load_vector4(memory, source + 48U)};
    call.return_value(write_render_view(memory, call.arg(0), camera_matrix, call.arg(3) != 0U,
                                        static_cast<std::int32_t>(call.arg(4)), static_cast<std::int32_t>(call.arg(5)),
                                        call.arg(6)));
    return call.finish(kRenderViewCost);
}

// The RwCamera fields a render view takes: the view matrix (four rows from
// +32), the projection (1 perspective, else parallel), the frame buffer
// raster (width, height and the offset it is drawn at), the clip planes'
// distances, and the view window, its inverse and offset (+104 to +124).
struct RwCameraLayout {
    static constexpr GuestAddress kProjection = 20U;
    static constexpr GuestAddress kViewMatrix = 32U;
    static constexpr GuestAddress kFrameBuffer = 96U;  // RwRaster pointer
    static constexpr GuestAddress kViewWindow = 104U;
    static constexpr GuestAddress kNearPlane = 128U;
    static constexpr GuestAddress kFarPlane = 132U;
};
struct RwRasterLayout {
    static constexpr GuestAddress kWidth = 12U;
    static constexpr GuestAddress kHeight = 16U;
    static constexpr GuestAddress kOffsetX = 28U;  // s16
    static constexpr GuestAddress kOffsetY = 30U;  // s16
};
constexpr std::uint32_t kRwPerspective = 1U;
constexpr GuestAddress kCurrentFrameRegister = 0x004e1838U;  // 64-bit GS register data
constexpr GuestAddress kCurrentDepthRegister = 0x004e1830U;
constexpr GuestAddress kCurrentOffsetRegister = 0x004e1848U;
constexpr GuestAddress kRwScreenScale = 0x0049af30U;  // four vectors
constexpr std::uint8_t kRenderParallel = 0x08U;

// SetUpRenderViewFromCamera (original 0x00225b30): sets up render view a0 for
// the RwCamera a1: its view matrix (also with z repeated in the fourth lane),
// clip guards with the depth terms of its projection, the current frame,
// depth and offset GS registers, a scissor for its frame buffer, its frame's
// matrix, the culling frustum (BuildCullingFrustumWithInnerSides) and its
// view window.
void write_render_view_from_camera(guest::EeMemory& memory, const GuestAddress view, const GuestAddress camera) {
    const GuestAddress view_matrix = camera + RwCameraLayout::kViewMatrix;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        const engine::Vector4 value = load_vector4(memory, view_matrix + row * 16U);
        store_vector4(memory, view + RenderViewLayout::kPackedProjection + row * 16U,
                      {value.x, value.y, value.z, value.z});
    }
    const float near_plane = load_float(memory, camera + RwCameraLayout::kNearPlane);
    const float far_plane = load_float(memory, camera + RwCameraLayout::kFarPlane);
    const float inverse_depth = 1.0F / (far_plane - near_plane);
    const GuestAddress raster = memory.read_u32_le(camera + RwCameraLayout::kFrameBuffer);
    const auto width = static_cast<std::int32_t>(memory.read_u32_le(raster + RwRasterLayout::kWidth));
    const auto height = static_cast<std::int32_t>(memory.read_u32_le(raster + RwRasterLayout::kHeight));
    const std::int32_t half_width = width >> 1;
    const std::int32_t half_height = height >> 1;
    const float guard_base = load_float(memory, kClipGuardBase);
    float depth_x;
    float depth_y;
    std::uint8_t flags = memory.read_u8(kRenderFlags);
    if (memory.read_u32_le(camera + RwCameraLayout::kProjection) == kRwPerspective) {
        depth_x = inverse_depth * ((-2.0F * far_plane) * near_plane);
        depth_y = inverse_depth * (far_plane + near_plane);
        flags = static_cast<std::uint8_t>(flags & ~kRenderParallel);
    } else {
        depth_x = -inverse_depth * (far_plane + near_plane);
        depth_y = 2.0F * inverse_depth;
        flags = static_cast<std::uint8_t>(flags | kRenderParallel);
    }
    memory.write_u8(kRenderFlags, flags);
    const auto guard = [&](const float extent, const float z, const float w) {
        return engine::Vector4{0.0F + extent, 0.0F + 1.0F / extent, 0.0F + z, w};
    };
    const GuestAddress guards = view + RenderViewLayout::kClipGuards;
    store_vector4(memory, guards + 32U, guard(guard_base - static_cast<float>(half_width), depth_x, far_plane));
    store_vector4(memory, guards + 48U, guard(guard_base - static_cast<float>(half_height), depth_y, near_plane));
    store_vector4(memory, guards, guard(static_cast<float>(half_width + 5), depth_x, far_plane));
    store_vector4(memory, guards + 16U, guard(static_cast<float>(half_height + 5), depth_y, near_plane));
    const GuestAddress registers = view + RenderViewLayout::kGsRegisters;
    store_gs_register(memory, registers, memory.read_u64_le(kCurrentFrameRegister), kGsFrame1);
    store_gs_register(memory, registers + 16U, memory.read_u64_le(kCurrentDepthRegister), kGsZbuf1);
    store_gs_register(memory, registers + 32U, memory.read_u64_le(kCurrentOffsetRegister), kGsXyOffset1);
    const auto offset_x = static_cast<std::int16_t>(memory.read_u16_le(raster + RwRasterLayout::kOffsetX));
    const auto offset_y = static_cast<std::int16_t>(memory.read_u16_le(raster + RwRasterLayout::kOffsetY));
    const auto signed_word = [](const std::uint32_t value) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
    };
    store_gs_register(
        memory, registers + 48U,
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(offset_x) & 0x7ffU)) |
            (signed_word(static_cast<std::uint32_t>(width) + static_cast<std::uint32_t>(offset_x) - 1U) << 16U) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(offset_y) & 0x7ffU) << 32U) |
            (signed_word(static_cast<std::uint32_t>(height) + static_cast<std::uint32_t>(offset_y) - 1U) << 48U),
        kGsScissor1);
    const GuestAddress screen = view + RenderViewLayout::kScreenSize;
    store_float(memory, screen, static_cast<float>(width));
    store_float(memory, screen + 4U, static_cast<float>(height));
    store_float(memory, screen + 8U, static_cast<float>(offset_x));
    store_float(memory, screen + 12U, static_cast<float>(offset_y));
    const engine::Vector4 scale_w = load_vector4(memory, kRwScreenScale);
    const engine::Vector4 scale = load_vector4(memory, kRwScreenScale + 16U);
    store_vector4(memory, view + RenderViewLayout::kScreenScale, {scale.x, scale.y, scale.z, scale_w.z});
    const engine::Vector4 offset = load_vector4(memory, kRwScreenScale + 32U);
    const engine::Vector4 extra = load_vector4(memory, kRwScreenScale + 48U);
    store_vector4(memory, view + RenderViewLayout::kScreenOffset,
                  {0.0F + (offset.x + extra.x), 0.0F + (offset.y + extra.y), offset.z, scale_w.w});
    const GuestAddress frame_matrix = memory.read_u32_le(camera + kRwObjectParent) + kRwFrameMatrix;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        const engine::Vector4 value = load_vector4(memory, frame_matrix + row * 16U);
        store_vector4(memory, view + RenderViewLayout::kCameraMatrix + row * 16U, {value.x, value.y, value.z, 0.0F});
    }
    for (GuestAddress row = 0U; row < 4U; ++row) {
        const engine::Vector4 value = load_vector4(memory, view_matrix + row * 16U);
        store_vector4(memory, view + RenderViewLayout::kProjection + row * 16U, {value.x, value.y, value.z, 0.0F});
    }
    write_culling_frustum_with_inner_sides(memory, view, camera);
    for (GuestAddress word = 0U; word < 24U; word += 4U) {
        memory.write_u32_le(view + RenderViewLayout::kViewWindow + word,
                            memory.read_u32_le(camera + RwCameraLayout::kViewWindow + word));
    }
}

constexpr std::uint32_t kRenderViewFromCameraCost = 552U;

NativeFunctionExecution set_up_render_view_from_camera(NativeFunctionContext& context) {
    GuestCall call{context};
    write_render_view_from_camera(call.memory(), call.arg(0), call.arg(1));
    return call.finish(kRenderViewFromCameraCost);
}

// The renderer's constants for the RwCamera being drawn (RenderWare's PS2
// device keeps them for the vector unit): the near and far planes four times
// over, the fog's start and scale, the frame buffer's size and depth scale,
// the depth shift, where its centre and corner are on the GS's drawing area,
// and per axis the clip guard bands with the depth terms of the projection.
struct CameraConstantsLayout {
    static constexpr GuestAddress kGuards = 0x0049aed0U;        // x, then y, then the same again: 16 bytes each
    static constexpr GuestAddress kNearPlanes = 0x0049af10U;    // four floats
    static constexpr GuestAddress kFarPlanes = 0x0049af20U;     // four floats
    static constexpr GuestAddress kFogScale = 0x0049af38U;
    static constexpr GuestAddress kFogEnd = 0x0049af3cU;
    static constexpr GuestAddress kScreenScale = 0x0049af40U;   // width, height, depth scale, 0
    static constexpr GuestAddress kScreenShift = 0x0049af50U;   // 0, 0, depth shift, 0
    static constexpr GuestAddress kScreenCentre = 0x0049af60U;  // x, y, 0, 0
    static constexpr GuestAddress kScreenCorner = 0x0049afe0U;  // x, y, 0, 0
};
struct RwCameraDepthLayout {
    static constexpr GuestAddress kFogPlane = 136U;
    static constexpr GuestAddress kDepthScale = 140U;
    static constexpr GuestAddress kDepthShift = 144U;
};
constexpr GuestAddress kRwRasterParent = 0U;  // the raster a sub-raster is part of
constexpr GuestAddress kFogUsesFarPlane = 0x004e28b0U;  // u32
constexpr GuestAddress kFogEndDistance = 0x004e28b4U;
constexpr GuestAddress kVideoMode = 0x004e1808U;        // pointer: width, height
constexpr GuestAddress kGuardBandBase = 0x004e0718U;
constexpr float kFogLevels = -255.0F;
constexpr std::int32_t kDrawingAreaCentre = 2048;
constexpr std::int32_t kGuardMargin = 5;

// SetUpCameraConstants (original 0x0020aef0): the constants for the RwCamera
// a0, in the game's rounding. Returns the original's instruction count.
std::uint32_t write_camera_constants(guest::EeMemory& memory, const GuestAddress camera) {
    const float near_plane = load_float(memory, camera + RwCameraLayout::kNearPlane);
    const float far_plane = load_float(memory, camera + RwCameraLayout::kFarPlane);
    for (GuestAddress lane = 0U; lane < 16U; lane += 4U) {
        store_float(memory, CameraConstantsLayout::kNearPlanes + lane, near_plane);
        store_float(memory, CameraConstantsLayout::kFarPlanes + lane, far_plane);
    }

    const GuestAddress raster = memory.read_u32_le(camera + RwCameraLayout::kFrameBuffer);
    const GuestAddress parent = memory.read_u32_le(raster + kRwRasterParent);
    const auto word = [&](const GuestAddress address) { return static_cast<std::int32_t>(memory.read_u32_le(address)); };
    const auto half = [](const std::int32_t value) { return value / 2; };  // toward zero, as the original
    const std::int32_t width = word(raster + RwRasterLayout::kWidth);
    const std::int32_t height = word(raster + RwRasterLayout::kHeight);
    store_vector4(memory, CameraConstantsLayout::kScreenScale,
                  {engine::chop_from_int(width), engine::chop_from_int(height),
                   load_float(memory, camera + RwCameraDepthLayout::kDepthScale), 0.0F});
    store_vector4(memory, CameraConstantsLayout::kScreenShift,
                  {0.0F, 0.0F, load_float(memory, camera + RwCameraDepthLayout::kDepthShift), 0.0F});
    const auto corner = [&](const GuestAddress offset, const GuestAddress parent_size) {
        const auto drawn_at = static_cast<std::int16_t>(memory.read_u16_le(raster + offset));
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(drawn_at) -
                                         static_cast<std::uint32_t>(half(word(parent + parent_size)) - kDrawingAreaCentre));
    };
    const std::int32_t corner_x = corner(RwRasterLayout::kOffsetX, RwRasterLayout::kWidth);
    const std::int32_t corner_y = corner(RwRasterLayout::kOffsetY, RwRasterLayout::kHeight);
    const auto wrapped_sum = [](const std::int32_t a, const std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    store_vector4(memory, CameraConstantsLayout::kScreenCentre,
                  {engine::chop_from_int(wrapped_sum(corner_x, half(width))),
                   engine::chop_from_int(wrapped_sum(corner_y, half(height))), 0.0F, 0.0F});
    store_vector4(memory, CameraConstantsLayout::kScreenCorner,
                  {engine::chop_from_int(corner_x), engine::chop_from_int(corner_y), 0.0F, 0.0F});

    std::uint32_t instructions = 81U;
    float fog_end;
    if (memory.read_u32_le(kFogUsesFarPlane) != 0U) {
        fog_end = far_plane;
        instructions += 11U;
    } else {
        fog_end = load_float(memory, kFogEndDistance);
        instructions += 9U;
    }
    store_float(memory, CameraConstantsLayout::kFogEnd, fog_end);
    store_float(memory, CameraConstantsLayout::kFogScale,
                engine::game_div(kFogLevels,
                                 engine::chop_sub(fog_end, load_float(memory, camera + RwCameraDepthLayout::kFogPlane))));

    // Guard bands: the base less half the screen and its inverse, then half
    // the screen plus a margin and its inverse; each with the planes.
    const GuestAddress mode = memory.read_u32_le(kVideoMode);
    const std::int32_t half_x = word(mode) >> 1;
    const std::int32_t half_y = word(mode + 4U) >> 1;
    const float guard_base = load_float(memory, kGuardBandBase);
    const float inner_x = engine::chop_sub(guard_base, engine::chop_from_int(half_x));
    const float inner_y = engine::chop_sub(guard_base, engine::chop_from_int(half_y));
    const float outer_x = engine::chop_from_int(half_x + kGuardMargin);
    const float outer_y = engine::chop_from_int(half_y + kGuardMargin);
    const float inverse_depth = engine::game_div(1.0F, engine::chop_sub(far_plane, near_plane));
    float depth_x;
    float depth_y;
    std::uint8_t flags = memory.read_u8(kRenderFlags);
    if (memory.read_u32_le(camera + RwCameraLayout::kProjection) == kRwPerspective) {
        depth_x = engine::chop_mul(engine::chop_mul(far_plane, engine::chop_mul(near_plane, -2.0F)), inverse_depth);
        depth_y = engine::chop_mul(engine::chop_add(far_plane, near_plane), inverse_depth);
        flags = static_cast<std::uint8_t>(flags & ~kRenderParallel);
        instructions += 56U + 19U;
    } else {
        depth_x = engine::chop_mul(engine::chop_add(far_plane, near_plane), -inverse_depth);
        depth_y = engine::chop_add(inverse_depth, inverse_depth);
        flags = static_cast<std::uint8_t>(flags | kRenderParallel);
        instructions += 56U + 14U;
    }
    memory.write_u8(kRenderFlags, flags);
    const auto guard = [&](const GuestAddress at, const float extent, const float depth, const float plane) {
        store_float(memory, at, extent);
        store_float(memory, at + 4U, engine::game_div(1.0F, extent));
        store_float(memory, at + 8U, depth);
        store_float(memory, at + 12U, plane);
    };
    guard(CameraConstantsLayout::kGuards, inner_x, depth_x, far_plane);
    guard(CameraConstantsLayout::kGuards + 16U, inner_y, depth_y, near_plane);
    guard(CameraConstantsLayout::kGuards + 32U, outer_x, depth_x, far_plane);
    guard(CameraConstantsLayout::kGuards + 48U, outer_y, depth_y, near_plane);
    return instructions;
}

NativeFunctionExecution set_up_camera_constants(NativeFunctionContext& context) {
    GuestCall call{context};
    return call.finish(write_camera_constants(call.memory(), call.arg(0)));
}

// A vehicle's reflection views: the main camera's view (at +1136), then
// views looking from the camera towards the vehicle's centre (its basis at
// +1072: right, up, forward, centre) turned by fixed rotations, one per face
// of the reflection map (from +1696, 560 bytes apart, each 2^n by 2^m with
// the exponents from +4532 and +4552).
struct ReflectionLayout {
    static constexpr GuestAddress kBasis = 1072U;
    static constexpr GuestAddress kCentre = 1120U;
    static constexpr GuestAddress kMainView = 1136U;
    static constexpr GuestAddress kFirstFace = 1696U;
    static constexpr GuestAddress kFaceBytes = 560U;
    static constexpr GuestAddress kWidthExponents = 4532U;   // s32 per face
    static constexpr GuestAddress kHeightExponents = 4552U;  // s32 per face
};
constexpr GuestAddress kVehicles = 0x0064d388U;     // pointer per vehicle index
constexpr GuestAddress kVehicleModel = 64U;        // pointer; bounds at +3712 (low) and +3728 (high)
constexpr GuestAddress kModelBoundsLow = 3712U;
constexpr GuestAddress kModelBoundsHigh = 3728U;
constexpr GuestAddress kMainCamera = 0x00670bd0U;  // RwCamera pointer
constexpr GuestAddress kReflectionMode = 0x0051ba88U;
constexpr std::uint32_t kReflectionCubeFaces = 0x0051a6a8U;  // the mode value for five square-on faces
// Settings of the reflection map (game variables).
constexpr GuestAddress kReflectionTilt = 0x004b9da8U;        // degrees
constexpr GuestAddress kReflectionForward = 0x004b9db0U;     // how far the faces sit ahead of the centre
constexpr GuestAddress kReflectionFirstAxis = 0x004b9dc0U;
constexpr GuestAddress kReflectionSecondAxis = 0x004b9dd0U;
constexpr GuestAddress kReflectionTurnAxis = 0x004b9de0U;
constexpr GuestAddress kDegreesToRadians = 0x004e06f0U;
constexpr GuestAddress kReflectionLift = 0x004e0a20U;       // 1/3
constexpr GuestAddress kReflectionRoll = 0x004e0a60U;       // pi/4
constexpr GuestAddress kReflectionTurn = 0x004e0a80U;       // 2 pi/3
constexpr GuestAddress kQuarterTurn = 0x004e0a84U;          // pi/2
constexpr GuestAddress kNegativeQuarterTurn = 0x004e0a88U;  // -pi/2

engine::Vector3 vu_normalized(const engine::Vector3 v) {
    const float length_squared = (v.x * v.x + v.y * v.y) + v.z * v.z;
    return v * (1.0F / std::sqrt(length_squared));
}

// The rotation by `angle` about the unit `axis` (Rodrigues' formula as the
// original builds it, with the game's fast sine and cosine).
engine::RotationRows axis_angle_rotation(const engine::Vector3 axis, const float angle) {
    const float turned = 0.0F + angle;
    const float sine = engine::wave_cosine(turned - engine::kHalfPi);
    const float versine = 0.0F + (1.0F - engine::wave_cosine(0.0F + turned));
    const engine::Vector3 squares{axis.x * axis.x, axis.y * axis.y, axis.z * axis.z};
    const engine::Vector3 kept = ((engine::Vector3{} - squares) + engine::Vector3{1.0F, 1.0F, 1.0F}) * versine;
    const float yz = (0.0F + axis.y * axis.z) * versine;
    const float zx = (0.0F + axis.z * axis.x) * versine;
    const float xy = (0.0F + axis.x * axis.y) * versine;
    const engine::Vector3 swing = axis * sine;
    engine::RotationRows rows;
    rows.rows[0] = {0.0F + (1.0F - kept.x), 0.0F + (xy + swing.z), 0.0F + (zx - swing.y)};
    rows.rows[1] = {0.0F + (xy - swing.z), 0.0F + (1.0F - kept.y), 0.0F + (yz + swing.x)};
    rows.rows[2] = {0.0F + (zx + swing.y), 0.0F + (yz - swing.x), 0.0F + (1.0F - kept.z)};
    return rows;
}

// rows times `by` (row vectors): each row a sum of `by`'s rows.
engine::RotationRows rotation_product(const engine::RotationRows& rows, const engine::RotationRows& by) {
    engine::RotationRows product;
    for (int row = 0; row < 3; ++row) {
        const engine::Vector3 r = rows.rows[row];
        product.rows[row] = (by.rows[0] * r.x + by.rows[1] * r.y) + by.rows[2] * r.z;
    }
    return product;
}

// UpdateReflectionViews (original 0x00223130): the reflection views of
// vehicle a1 (none for -1, which only sets the main camera's view) into the
// record at a0. Faces of the cube mode look straight along the basis and a
// quarter turn either way about two axes; the other mode has three faces a
// third of a turn apart, tilted, rolled and pushed forward.
NativeFunctionExecution update_reflection_views(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress record = call.arg(0);
    const auto vehicle = static_cast<std::int32_t>(call.arg(1));
    const GuestAddress camera = memory.read_u32_le(kMainCamera);
    engine::Vector4 centre{};
    if (vehicle != -1) {
        const GuestAddress matrix = memory.read_u32_le(kVehicles + static_cast<GuestAddress>(vehicle) * 4U);
        const GuestAddress model = memory.read_u32_le(matrix + kVehicleModel);
        const engine::Vector3 high = load_vector4(memory, model + kModelBoundsHigh).xyz();
        const engine::Vector3 low = load_vector4(memory, model + kModelBoundsLow).xyz();
        const engine::Vector3 local = ((high + engine::Vector3{}) - high * 0.5F) + low * 0.5F;
        const engine::Vector4 translation = load_vector4(memory, matrix + 48U);
        const engine::Vector3 world =
            ((load_vector4(memory, matrix).xyz() * local.x + load_vector4(memory, matrix + 16U).xyz() * local.y) +
             load_vector4(memory, matrix + 32U).xyz() * local.z) +
            translation.xyz();
        centre = {world.x, world.y, world.z, translation.w};
    }
    write_render_view_from_camera(memory, record + ReflectionLayout::kMainView, camera);
    std::uint32_t cost = kRenderViewFromCameraCost + 43U;
    if (vehicle == -1) {
        return call.finish(cost);
    }
    store_vector4(memory, record + ReflectionLayout::kCentre, centre);
    const GuestAddress frame_matrix = memory.read_u32_le(camera + kRwObjectParent) + kRwFrameMatrix;
    const engine::Vector3 camera_up = load_vector4(memory, frame_matrix + 16U).xyz();
    engine::Vector3 eye = load_vector4(memory, frame_matrix + 48U).xyz();
    const bool cube = memory.read_u32_le(kReflectionMode) == kReflectionCubeFaces;
    if (!cube) {
        const float height = engine::minimum(engine::maximum(3.0F * (eye.y - centre.y), 0.0F), 3.0F);
        eye.y = 0.0F + (eye.y + (height * load_float(memory, kReflectionLift)) * 3.0F);
    }
    const engine::Vector3 forward = vu_normalized(eye - centre.xyz());
    const engine::Vector3 right = vu_normalized(engine::cross(camera_up, forward));
    const engine::Vector3 up = vu_normalized(engine::cross(forward, right));
    const GuestAddress basis = record + ReflectionLayout::kBasis;
    store_vector4(memory, basis, {right.x, right.y, right.z, centre.w});
    store_vector4(memory, basis + 16U, {up.x, up.y, up.z, centre.w});
    store_vector4(memory, basis + 32U, {forward.x, forward.y, forward.z, centre.w});
    // The face's rows are the turn applied to the basis; the cube's faces sit
    // at the centre, the others `push` along their view direction.
    const auto face_matrix = [&](const engine::RotationRows& turn, const bool pushed, const float push) {
        CameraMatrix rows;
        for (int row = 0; row < 3; ++row) {
            const engine::Vector3 t = turn.rows[row];
            const engine::Vector3 value = (right * t.x + up * t.y) + forward * t.z;
            rows[static_cast<std::size_t>(row)] = {value.x, value.y, value.z, centre.w};
        }
        const engine::Vector3 origin = ((right * 0.0F + up * 0.0F) + forward * 0.0F) + centre.xyz();
        const engine::Vector3 placed =
            pushed ? origin + engine::Vector3{rows[2].x, rows[2].y, rows[2].z} * push : origin;
        rows[3] = {placed.x, placed.y, placed.z, centre.w};
        return rows;
    };
    std::uint32_t base = 0U;
    const auto face = [&](const std::uint32_t index, const CameraMatrix& rows) {
        const auto size = [&](const GuestAddress exponents) {
            return static_cast<std::int32_t>(1U << (memory.read_u32_le(record + exponents + index * 4U) & 31U));
        };
        base = write_render_view(memory, record + ReflectionLayout::kFirstFace + index * ReflectionLayout::kFaceBytes,
                                 rows, cube, size(ReflectionLayout::kWidthExponents),
                                 size(ReflectionLayout::kHeightExponents), base);
        cost += kRenderViewCost + 403U;
    };
    if (cube) {
        const engine::Vector3 first_axis = vu_normalized(load_vector4(memory, kReflectionFirstAxis).xyz());
        const engine::Vector3 second_axis = vu_normalized(load_vector4(memory, kReflectionSecondAxis).xyz());
        const float quarter = load_float(memory, kQuarterTurn);
        const float negative_quarter = load_float(memory, kNegativeQuarterTurn);
        CameraMatrix straight;
        for (int row = 0; row < 3; ++row) {
            const engine::Vector4 value = load_vector4(memory, basis + static_cast<GuestAddress>(row) * 16U);
            straight[static_cast<std::size_t>(row)] = value;
        }
        straight[3] = centre;
        face(0U, straight);
        face(1U, face_matrix(axis_angle_rotation(first_axis, quarter), false, 0.0F));
        face(2U, face_matrix(axis_angle_rotation(first_axis, negative_quarter), false, 0.0F));
        face(3U, face_matrix(axis_angle_rotation(second_axis, quarter), false, 0.0F));
        face(4U, face_matrix(axis_angle_rotation(second_axis, negative_quarter), false, 0.0F));
    } else {
        const engine::Vector3 first_axis = vu_normalized(load_vector4(memory, kReflectionFirstAxis).xyz());
        const engine::RotationRows tilt = axis_angle_rotation(
            first_axis, load_float(memory, kDegreesToRadians) * load_float(memory, kReflectionTilt));
        const engine::RotationRows roll =
            axis_angle_rotation(vu_normalized(tilt.rows[2]), load_float(memory, kReflectionRoll));
        const engine::RotationRows turn = axis_angle_rotation(
            vu_normalized(load_vector4(memory, kReflectionTurnAxis).xyz()), load_float(memory, kReflectionTurn));
        const float push = load_float(memory, kReflectionForward);
        engine::RotationRows rotation = rotation_product(tilt, roll);
        for (std::uint32_t index = 0U; index < 3U; ++index) {
            if (index != 0U) {
                rotation = rotation_product(rotation, turn);
            }
            face(index, face_matrix(rotation, true, push));
        }
    }
    return call.finish(cost);
}

// A directional light: parameters through the pointer at +228, colours at
// +48 and +64, direction at +80.
struct LightLayout {
    static constexpr GuestAddress kParameters = 228U;
    static constexpr GuestAddress kFirstColour = 48U;
    static constexpr GuestAddress kSecondColour = 64U;
    static constexpr GuestAddress kDirection = 80U;
};
struct LightParametersLayout {
    static constexpr GuestAddress kFirstIntensity = 80U;
    static constexpr GuestAddress kSecondIntensity = 84U;
    static constexpr GuestAddress kVerticalScale = 88U;
    static constexpr GuestAddress kBright = 92U;  // u8: colours may exceed 1
};
constexpr GuestAddress kBrightLightLimit = 0x004e0ff4U;  // gp - 30332, 1.99 in the ELF

// SetDirectionalLight (original 0x00254d50): the light a0 takes colour a1
// (scaled by both intensities) and direction a2.
NativeFunctionExecution set_directional_light(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress light = call.arg(0);
    const GuestAddress parameters = memory.read_u32_le(light + LightLayout::kParameters);
    const engine::Vector4 colour = load_vector4(memory, call.arg(1) & ~15U);
    const engine::Vector4 direction = load_vector4(memory, call.arg(2) & ~15U);
    const float limit = memory.read_u8(parameters + LightParametersLayout::kBright) != 0U
                            ? load_float(memory, kBrightLightLimit)
                            : 1.0F;
    store_vector4(memory, light + LightLayout::kFirstColour,
                  engine::light_colour(colour, load_float(memory, parameters + LightParametersLayout::kFirstIntensity), limit));
    store_vector4(memory, light + LightLayout::kSecondColour,
                  engine::light_colour(colour, load_float(memory, parameters + LightParametersLayout::kSecondIntensity), limit));
    const engine::Vector3 unit =
        engine::light_direction(direction.xyz(), load_float(memory, parameters + LightParametersLayout::kVerticalScale));
    store_float(memory, light + LightLayout::kDirection, unit.x);
    store_float(memory, light + LightLayout::kDirection + 4U, unit.y);
    store_float(memory, light + LightLayout::kDirection + 8U, unit.z);
    return call.finish(110U);
}

// The game's packet under construction: a count of quadwords, then the
// quadwords from +16.
constexpr GuestAddress kPacketPointer = 0x004e2a10U;  // gp - 23648
constexpr GuestAddress kCurrentTargetIndex = 0x00667050U;
// Per render target (560 bytes each): ten quadwords of VU1 data at +288, then
// the FRAME, ZBUF and two further GS register writes at +448.
constexpr GuestAddress kRenderTargets = 0x00666330U;
constexpr GuestAddress kRenderTargetBytes = 560U;

struct Quadword {
    std::uint64_t low{};
    std::uint64_t high{};
};

class PacketWriter {
  public:
    explicit PacketWriter(guest::EeMemory& memory)
        : memory_(memory), packet_(memory.read_u32_le(kPacketPointer)) {}

    void clear() { memory_.write_u32_le(packet_, 0U); }

    void append(const Quadword value) {
        const std::uint32_t count = memory_.read_u32_le(packet_);
        memory_.write_u32_le(packet_, count + 1U);
        const GuestAddress address = packet_ + 16U + count * 16U;
        memory_.write_u64_le(address, value.low);
        memory_.write_u64_le(address + 8U, value.high);
    }

    [[nodiscard]] Quadword read(const GuestAddress address) const {
        return {memory_.read_u64_le(address), memory_.read_u64_le(address + 8U)};
    }

  private:
    guest::EeMemory& memory_;
    GuestAddress packet_;
};

// SelectRenderTarget (original 0x0026baa0): records the render target index
// (s8 at a0+153) and starts a new packet with its VU1 constants (a DMA cnt
// tag, STCYCL and UNPACK of ten quadwords) and an A+D GIF packet setting
// FRAME_1 and ZBUF_1 (the target's values ORed with the object's at +160 and
// +176) and two more registers.
NativeFunctionExecution select_render_target(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);
    const auto index = static_cast<std::int8_t>(memory.read_u8(object + 153U));
    memory.write_u32_le(kCurrentTargetIndex, static_cast<std::uint32_t>(static_cast<std::int32_t>(index)));
    const GuestAddress target =
        kRenderTargets + static_cast<GuestAddress>(static_cast<std::int32_t>(index) * static_cast<std::int32_t>(kRenderTargetBytes));

    PacketWriter packet{memory};
    packet.clear();
    packet.append({0x000000001000000aULL, 0x6c0a03f601000404ULL});
    for (GuestAddress quadword = 0U; quadword < 10U; ++quadword) {
        packet.append(packet.read(target + 288U + quadword * 16U));
    }
    packet.append({0x0000000010000005ULL, 0x5000000500000000ULL});
    packet.append({0x1000000000008004ULL, 0x000000000000000eULL});
    constexpr std::uint64_t kFrame1 = 0x4cU;
    constexpr std::uint64_t kZbuf1 = 0x4eU;
    packet.append({packet.read(target + 448U).low | memory.read_u64_le(object + 160U), kFrame1});
    packet.append({packet.read(target + 464U).low | memory.read_u64_le(object + 176U), kZbuf1});
    packet.append(packet.read(target + 480U));
    packet.append(packet.read(target + 496U));
    return call.finish(110U);
}

// Render target fields the lens flare reads.
struct RenderTargetViewLayout {
    static constexpr GuestAddress kEye = 208U;            // the main target's camera position
    static constexpr GuestAddress kView = 224U;           // world to view: 3 rows, translation
    static constexpr GuestAddress kScreenX = 480U;        // u16
    static constexpr GuestAddress kScreenY = 484U;        // u16
    static constexpr GuestAddress kProjectionScale = 512U;
    static constexpr GuestAddress kFlareScale = 520U;
    static constexpr GuestAddress kProjectionAspect = 524U;
    static constexpr GuestAddress kWidth = 536U;
    static constexpr GuestAddress kHeight = 540U;
    static constexpr GuestAddress kCentreX = 544U;
    static constexpr GuestAddress kCentreY = 548U;
};
// Targets after the first take the eye from here.
constexpr GuestAddress kSecondaryEye = 0x00666320U;
// The lens flare's state.
struct SunFlareLayout {
    static constexpr GuestAddress kBase = 0x004e2970U;
    static constexpr GuestAddress kProjectedX = kBase + 0U;    // view x / z
    static constexpr GuestAddress kProjectedY = kBase + 4U;
    static constexpr GuestAddress kWidth = kBase + 8U;         // float
    static constexpr GuestAddress kHeight = kBase + 12U;
    static constexpr GuestAddress kCentreX = kBase + 16U;
    static constexpr GuestAddress kCentreY = kBase + 20U;
    static constexpr GuestAddress kWidthPixels = kBase + 24U;  // s32
    static constexpr GuestAddress kHeightPixels = kBase + 28U;
    static constexpr GuestAddress kScreenX = kBase + 32U;      // s32
    static constexpr GuestAddress kScreenY = kBase + 36U;
    static constexpr GuestAddress kCentreScreenX = kBase + 40U;  // screen + 16 * centre
    static constexpr GuestAddress kCentreScreenY = kBase + 44U;
    static constexpr GuestAddress kProjection = kBase + 48U;
    static constexpr GuestAddress kFlareScale = kBase + 52U;
    static constexpr GuestAddress kFlareSize = kBase + 56U;    // f12 * scale
    static constexpr GuestAddress kFlareParameter = kBase + 60U;  // f13
    static constexpr GuestAddress kHidden = kBase + 64U;       // u8
};
constexpr GuestAddress kSunDirection = 0x01d9b940U;
constexpr GuestAddress kSunColour = 0x01d9b960U;             // colour * 128
constexpr GuestAddress kSunColourPremultiplied = 0x01d9b970U; // colour * alpha * 128
constexpr GuestAddress kSunNearLimit = 0x004e0c6cU;          // 0.01

// ProjectSun (original 0x0024d110): the sun of direction a0 and colour a1
// in the current render target; f12 and f13 size the flare.
NativeFunctionExecution project_sun(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::Vector4 direction = load_vector4(memory, call.arg(0) & ~15U);
    const engine::Vector4 colour = load_vector4(memory, call.arg(1) & ~15U);
    const auto index = static_cast<std::int32_t>(memory.read_u32_le(kCurrentTargetIndex));
    const GuestAddress target = kRenderTargets + static_cast<GuestAddress>(index * static_cast<std::int32_t>(kRenderTargetBytes));
    const engine::Vector3 eye = load3(memory, index > 0 ? kSecondaryEye : target + RenderTargetViewLayout::kEye);
    engine::Transform view;
    for (int row = 0; row < 3; ++row) {
        view.basis[row] = load3(memory, target + RenderTargetViewLayout::kView + static_cast<GuestAddress>(row * 16));
    }
    view.translation = load3(memory, target + RenderTargetViewLayout::kView + 48U);

    const engine::SunProjection sun = engine::project_sun(direction.xyz(), eye, view);
    store_vector4(memory, kSunDirection, {sun.direction.x, sun.direction.y, sun.direction.z, direction.w});
    if (sun.view_point.z <= load_float(memory, kSunNearLimit)) {
        memory.write_u8(SunFlareLayout::kHidden, 1U);
        return call.finish(60U);
    }
    memory.write_u8(SunFlareLayout::kHidden, 0U);
    const auto field = [&](const GuestAddress offset) { return load_float(memory, target + offset); };
    const auto screen_x = static_cast<std::int32_t>(memory.read_u16_le(target + RenderTargetViewLayout::kScreenX));
    const auto screen_y = static_cast<std::int32_t>(memory.read_u16_le(target + RenderTargetViewLayout::kScreenY));
    const float centre_x = field(RenderTargetViewLayout::kCentreX);
    const float centre_y = field(RenderTargetViewLayout::kCentreY);
    const std::int32_t width = engine::truncate_to_int(field(RenderTargetViewLayout::kWidth));
    const std::int32_t height = engine::truncate_to_int(field(RenderTargetViewLayout::kHeight));
    const float inverse_depth = 1.0F / sun.view_point.z;
    const float flare_scale = field(RenderTargetViewLayout::kFlareScale);

    memory.write_u32_le(SunFlareLayout::kScreenX, static_cast<std::uint32_t>(screen_x));
    memory.write_u32_le(SunFlareLayout::kScreenY, static_cast<std::uint32_t>(screen_y));
    store_float(memory, SunFlareLayout::kCentreX, centre_x);
    store_float(memory, SunFlareLayout::kCentreY, centre_y);
    store_vector4(memory, kSunColour, {colour.x * 128.0F, colour.y * 128.0F, colour.z * 128.0F, colour.w * 128.0F});
    store_vector4(memory, kSunColourPremultiplied,
                  {(colour.x * colour.w) * 128.0F, (colour.y * colour.w) * 128.0F, (colour.z * colour.w) * 128.0F,
                   (colour.w * colour.w) * 128.0F});
    memory.write_u32_le(SunFlareLayout::kCentreScreenX,
                        static_cast<std::uint32_t>(screen_x + engine::truncate_to_int(16.0F * centre_x)));
    memory.write_u32_le(SunFlareLayout::kCentreScreenY,
                        static_cast<std::uint32_t>(screen_y + engine::truncate_to_int(16.0F * centre_y)));
    store_float(memory, SunFlareLayout::kProjectedY, sun.view_point.y * inverse_depth);
    store_float(memory, SunFlareLayout::kProjectedX, sun.view_point.x * inverse_depth);
    const auto width_float = static_cast<float>(width);
    const auto height_float = static_cast<float>(height);
    store_float(memory, SunFlareLayout::kWidth, width_float);
    store_float(memory, SunFlareLayout::kHeight, height_float);
    memory.write_u32_le(SunFlareLayout::kWidthPixels, static_cast<std::uint32_t>(width));
    memory.write_u32_le(SunFlareLayout::kHeightPixels, static_cast<std::uint32_t>(height));
    store_float(memory, SunFlareLayout::kFlareParameter, call.float_arg(1));
    store_float(memory, SunFlareLayout::kProjection,
                field(RenderTargetViewLayout::kProjectionScale) *
                    (field(RenderTargetViewLayout::kProjectionAspect) * (height_float / width_float)));
    store_float(memory, SunFlareLayout::kFlareScale, flare_scale);
    store_float(memory, SunFlareLayout::kFlareSize, call.float_arg(0) * flare_scale);
    return call.finish(110U);
}

// The 2D packet ring the sprite strips go into (shared with the other 2D
// packet builders).
struct PacketRing {
    static constexpr GuestAddress kWrite = 0x004e28d8U;        // pointer
    static constexpr GuestAddress kOpenTag = 0x004e28dcU;      // pointer to the open GIF tag
    static constexpr GuestAddress kOpenKind = 0x004e28e0U;     // s32
    static constexpr GuestAddress kBase = 0x004e1950U;         // pointer
    static constexpr std::int32_t kQuadwords = 511;
};
constexpr GuestAddress kScreenOffsetX = 0x004e2908U;  // s32, 1/16 pixels
constexpr GuestAddress kScreenOffsetY = 0x004e290cU;
constexpr GuestAddress kVertexDepth = 0x004e2900U;    // the XYZ2 z word
constexpr GuestAddress kColourScale = 0x004b9bbcU;    // the 2D colour scale (w of a vector)
// A sprite strip opens with a 48-byte header: a GIF tag, the TEX settings
// and the strip's own tag (whose loop count grows as strips join it).
constexpr GuestAddress kStripTagTemplate = 0x0067cda0U;
constexpr GuestAddress kStripSettingsTemplate = 0x0067cdf8U;
constexpr GuestAddress kStripOpenTagTemplate = 0x0067ce80U;
constexpr std::int32_t kSpriteStripKind = 7;
constexpr std::uint64_t kQOne = 0x3f80000000000000ULL;  // Q = 1 in RGBAQ

std::int32_t sprite_strip_quadwords(const std::int32_t vertices) {
    const std::int32_t words = vertices * 3 + 1;
    return (words < 0 ? words + 1 : words) >> 1;
}

std::int32_t packet_ring_space(const guest::EeMemory& memory) {
    const GuestAddress used = memory.read_u32_le(PacketRing::kWrite) + 8U - memory.read_u32_le(PacketRing::kBase);
    return PacketRing::kQuadwords - static_cast<std::int32_t>(used >> 4U);
}

// Strips that would overflow the ring need the ring flushed first, which the
// original does by calling the (not yet native) ring flush: those calls run
// the original.
std::uint32_t sprite_strip_cost(const NativeFunctionPrepareContext& context) {
    const auto vertices = static_cast<std::int32_t>(context.cpu.read_gpr_u32(5U));
    if (packet_ring_space(context.memory) < sprite_strip_quadwords(vertices) + 6) {
        return 0U;
    }
    return 110U + 85U * static_cast<std::uint32_t>(vertices > 0 ? vertices : 0);
}

// A sign-extended 32-bit field shifted into place, as the original packs it.
std::uint64_t packed_field(const std::int32_t value, const unsigned shift) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) << shift;
}

// AppendSpriteStrip (original 0x002187b0): appends a1 textured vertices
// (screen positions a2, texture coordinates a3, RGBA colours t0, each
// offset by the origin at a0) to the 2D packet, joining the open sprite strip
// with two repeated vertices or opening a new one.
NativeFunctionExecution append_sprite_strip(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress origin = call.arg(0);
    const auto count = call.arg_signed(1);
    GuestAddress positions = call.arg(2);
    GuestAddress texture = call.arg(3);
    GuestAddress colours = call.arg(4);
    const auto rounded = [](const float value) {
        return engine::truncate_to_int(value + std::copysign(0.5F, value));
    };
    const std::int32_t screen_x = static_cast<std::int32_t>(memory.read_u32_le(kScreenOffsetX)) +
                                  rounded(16.0F * load_float(memory, origin));
    const std::int32_t screen_y = static_cast<std::int32_t>(memory.read_u32_le(kScreenOffsetY)) +
                                  rounded(16.0F * load_float(memory, origin + 4U));
    const float colour_scale = load_float(memory, kColourScale);
    const std::uint64_t depth = static_cast<std::uint64_t>(memory.read_u32_le(kVertexDepth)) << 32U;

    GuestAddress write = memory.read_u32_le(PacketRing::kWrite);
    const auto put = [&](const std::uint64_t value) {
        memory.write_u64_le(write, value);
        write += 8U;
    };
    const auto put_vertex = [&]() {
        put(static_cast<std::uint64_t>(memory.read_u32_le(texture)) |
            (static_cast<std::uint64_t>(memory.read_u32_le(texture + 4U)) << 32U));
        std::uint64_t rgbaq = kQOne;
        for (unsigned channel = 0U; channel < 4U; ++channel) {
            rgbaq |= packed_field(
                engine::truncate_to_int(load_float(memory, colours + channel * 4U) * colour_scale), channel * 8U);
        }
        put(rgbaq);
        put(depth | packed_field(screen_x + engine::truncate_to_int(16.0F * load_float(memory, positions)), 0U) |
            packed_field(screen_y + engine::truncate_to_int(16.0F * load_float(memory, positions + 4U)), 16U));
        positions += 8U;
        texture += 8U;
        colours += 16U;
    };

    std::int32_t remaining = count;
    if (static_cast<std::int32_t>(memory.read_u32_le(PacketRing::kOpenKind)) == kSpriteStripKind) {
        // Join: repeat the strip's last vertex and this strip's first.
        const GuestAddress tag = memory.read_u32_le(PacketRing::kOpenTag);
        memory.write_u32_le(tag, memory.read_u32_le(tag) + static_cast<std::uint32_t>(count + 2));
        for (int word = 0; word < 3; ++word) {
            put(memory.read_u64_le(write - 24U));
        }
        put_vertex();
        for (int word = 0; word < 3; ++word) {
            put(memory.read_u64_le(write - 24U));
        }
        --remaining;
    } else {
        if ((write & 15U) != 0U) {
            write += 8U;
        }
        memory.write_u32_le(PacketRing::kOpenKind, static_cast<std::uint32_t>(kSpriteStripKind));
        copy_quadword(memory, write, kStripTagTemplate);
        memory.write_u64_le(write + 16U, memory.read_u64_le(kStripSettingsTemplate));
        copy_quadword(memory, write + 32U, kStripOpenTagTemplate);
        memory.write_u32_le(PacketRing::kOpenTag, write + 32U);
        write += 48U;
        const GuestAddress tag = write - 16U;
        memory.write_u32_le(tag, memory.read_u32_le(tag) + static_cast<std::uint32_t>(count));
    }
    for (; remaining > 0; --remaining) {
        put_vertex();
    }
    memory.write_u32_le(PacketRing::kWrite, write);
    return call.finish(0U);  // reported from sprite_strip_cost
}

// An effect's state (through the owner's pointer at +256), filled from its
// emitter (a1) when it starts.
struct EffectLayout {
    static constexpr GuestAddress kState = 256U;       // in the owner: pointer
    static constexpr GuestAddress kOwnerPosition = 224U;
    static constexpr GuestAddress kExtent = 0U;        // half size x3, depth scale
    static constexpr GuestAddress kRates = 16U;        // 0, 0, 0, rate
    static constexpr GuestAddress kSize = 32U;         // size x3, 128 * scale
    static constexpr GuestAddress kSpread = 48U;
    static constexpr GuestAddress kCounter = 60U;      // u32
    static constexpr GuestAddress kAxes = 784U;        // three emitter vectors
    static constexpr GuestAddress kOffset = 832U;      // emitter position - owner position
    static constexpr GuestAddress kTimers = 920U;      // two u32
};
struct EmitterLayout {
    static constexpr GuestAddress kPosition = 48U;
    static constexpr GuestAddress kAxes = 92U;         // three 12-byte vectors
};
constexpr GuestAddress kEffectRateScale = 0x004e0f38U;   // 19.2
constexpr GuestAddress kEffectSizeScale = 0x004e0f3cU;   // 128.1
constexpr GuestAddress kEffectDepthScale = 0x004e0f40U;  // 38.4
// The emitter axes' fourth lanes take stale stack words in the original.
constexpr std::array kEffectUnusedWords{
    UnusedWord{EffectLayout::kState, EffectLayout::kAxes + 12U},
    UnusedWord{EffectLayout::kState, EffectLayout::kAxes + 28U},
    UnusedWord{EffectLayout::kState, EffectLayout::kAxes + 44U},
};

// StartEffect (original 0x00220040): the effect state of the owner at a0
// from the emitter at a1, scaled by f12 (and its depth by f13).
NativeFunctionExecution start_effect(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress owner = call.arg(0);
    const GuestAddress emitter = call.arg(1);
    const float scale = call.float_arg(0);
    const float depth = call.float_arg(1);
    const GuestAddress state = memory.read_u32_le(owner + EffectLayout::kState);
    memory.write_u32_le(state + EffectLayout::kCounter, 0U);
    memory.write_u32_le(state + EffectLayout::kTimers, 0U);
    memory.write_u32_le(state + EffectLayout::kTimers + 4U, 0U);
    const engine::Vector4 owner_position = load_vector4(memory, owner + EffectLayout::kOwnerPosition);
    const engine::Vector3 offset = load3(memory, emitter + EmitterLayout::kPosition) - owner_position.xyz();
    store_vector4(memory, state + EffectLayout::kOffset, {offset.x, offset.y, offset.z, owner_position.w});
    for (GuestAddress axis = 0U; axis < 3U; ++axis) {
        store3(memory, state + EffectLayout::kAxes + axis * 16U, load3(memory, emitter + EmitterLayout::kAxes + axis * 12U));
    }
    const float size_scale = load_float(memory, kEffectSizeScale);
    store_float(memory, state + EffectLayout::kSpread, size_scale * scale);
    store_vector4(memory, state + EffectLayout::kRates, {0.0F, 0.0F, 0.0F, load_float(memory, kEffectRateScale) * scale});
    const float size = size_scale * depth;
    const float half = 0.5F * size;
    store_vector4(memory, state + EffectLayout::kExtent, {half, half, half, load_float(memory, kEffectDepthScale) * scale});
    store_vector4(memory, state + EffectLayout::kSize, {size, size, size, 128.0F * scale});
    return call.finish(75U);
}

// A list of 32-byte particles: write and end pointers.
struct ParticleListLayout {
    static constexpr GuestAddress kWrite = 44U;
    static constexpr GuestAddress kEnd = 48U;
};

// AppendFacingParticle (original 0x00256fd0): when the vectors at a1 and a2
// face the same way (positive dot product), appends to the list at a0 the
// quadword at a3 with 400 in its fourth lane, then the vector at t0 scaled
// by 64 times that dot product (fourth lane f12).
NativeFunctionExecution append_facing_particle(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress list = call.arg(0);
    const float facing = engine::dot(load_vector4(memory, call.arg(2) & ~15U).xyz(),
                                     load_vector4(memory, call.arg(1) & ~15U).xyz());
    if (facing <= 0.0F) {
        return call.finish(14U);
    }
    const GuestAddress write = memory.read_u32_le(list + ParticleListLayout::kWrite);
    if (write == memory.read_u32_le(list + ParticleListLayout::kEnd)) {
        return call.finish(20U);
    }
    const engine::Vector4 direction = load_vector4(memory, call.arg(4) & ~15U);
    copy_quadword(memory, write, call.arg(3) & ~15U);
    store_float(memory, write + 12U, 400.0F);
    const engine::Vector3 velocity = (direction.xyz() * facing) * 64.0F;
    store_vector4(memory, write + 16U, {velocity.x, velocity.y, velocity.z, call.float_arg(0)});
    memory.write_u32_le(list + ParticleListLayout::kWrite, write + 32U);
    return call.finish(32U);
}

// RenderWare's RwMatrix flags and the default optimise tolerances (in the
// RenderWare instance, through its pointer).
constexpr std::uint32_t kMatrixNormal = 0x1U;
constexpr std::uint32_t kMatrixOrthogonal = 0x2U;
constexpr std::uint32_t kMatrixIdentity = 0x20000U;
constexpr GuestAddress kMatrixFlags = 12U;
constexpr GuestAddress kRenderWareInstance = 0x004e2760U;      // pointer
constexpr GuestAddress kDefaultMatrixTolerance = 0x00670bdcU;  // added to it

// RwMatrixOptimize (original 0x001d9990): sets the matrix at a0's normal,
// orthogonal and identity flags by the tolerances at a1 (the default ones
// when a1 is null); returns the matrix.
NativeFunctionExecution rw_matrix_optimize(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress matrix = call.arg(0);
    GuestAddress tolerances = call.arg(1);
    if (tolerances == 0U) {
        tolerances = memory.read_u32_le(kRenderWareInstance) + kDefaultMatrixTolerance;
    }
    engine::Transform transform;
    for (GuestAddress row = 0U; row < 3U; ++row) {
        transform.basis[row] = load3(memory, matrix + row * 16U);
    }
    transform.translation = load3(memory, matrix + 48U);
    const engine::MatrixShape shape =
        engine::matrix_shape(transform, {load_float(memory, tolerances), load_float(memory, tolerances + 4U),
                                         load_float(memory, tolerances + 8U)});
    std::uint32_t flags = memory.read_u32_le(matrix + kMatrixFlags);
    flags = shape.normal ? flags | kMatrixNormal : flags & ~kMatrixNormal;
    flags = shape.orthogonal ? flags | kMatrixOrthogonal : flags & ~kMatrixOrthogonal;
    flags = shape.identity ? flags | kMatrixIdentity : flags & ~kMatrixIdentity;
    memory.write_u32_le(matrix + kMatrixFlags, flags);
    call.return_value(matrix);
    return call.finish(shape.normal && shape.orthogonal ? 125U : 101U);
}

// The frustum side planes come from cross products of long, nearly parallel
// edges (corners at track coordinates), so a normal's components near zero
// keep only a few bits; the original rounds toward zero and the native code
// to nearest, which moves such components by up to 3e-6 (an angle of 3e-6
// radians).
constexpr double kFrustumAbsoluteTolerance = 1e-5;

// The sun's projected position is view x / z, with view x (about 5 in a
// capture) summed from terms around 5000 at track coordinates. A few units in
// the last place of those terms (1.5e-3 there) remain even when the native
// code rounds toward zero like the PS2, whose multiplier truncates slightly
// differently; projected that is 7.6e-5, about 0.02 pixel.
constexpr double kSunProjectionAbsoluteTolerance = 1e-4;

// A trail of points (two per effect, at +232 and +276): rings written from
// the top index down, each point's position (16 bytes), packed direction
// (8 bytes) and colour (4 bytes) in its own array, written through the
// uncached mirror so the renderer sees them at once.
struct TrailLayout {
    static constexpr GuestAddress kPositions = 8U;   // pointer
    static constexpr GuestAddress kDirections = 12U;  // pointer
    static constexpr GuestAddress kColours = 16U;     // pointer
    static constexpr GuestAddress kTopIndex = 20U;    // u32
    static constexpr GuestAddress kNextIndex = 24U;   // u32, counts down to 0, then back to the top
    static constexpr GuestAddress kSegmentAges = 28U;  // pointer; one float per 60 points
    static constexpr GuestAddress kMinimumAge = 36U;   // float
};
constexpr GuestAddress kFirstTrail = 232U;
constexpr GuestAddress kSecondTrail = 276U;
constexpr GuestAddress kTrailOwnerAge = 124U;  // float in the effect
constexpr GuestAddress kTrailDirectionScale = 0x004bbf70U;  // z scales the direction, w the fourth lane
constexpr GuestAddress kTrailColourScale = 0x004bbf80U;
constexpr GuestAddress kUncachedAccelerated = 0x30000000U;
constexpr GuestAddress kUncached = 0x20000000U;
constexpr std::uint32_t kTrailPointsPerSegment = 60U;

// AppendTrailPoint (original 0x00254a90): appends to trail t0 (the second
// when nonzero) of effect a0 the position at a1 with age f12, the direction
// at a2 and the colour (f14 + 2 f13, f14, f16, f15), with f17 the
// direction's fourth lane; returns 1. Unless a3 is set, a point is only
// added once the effect's age is at least the trail's minimum past the last
// point's (returns 0 otherwise).
// A trail point's values: its age, the colour terms (f13 to f16 of the
// original) and the direction's fourth lane.
struct TrailPoint {
    engine::Vector3 position;
    engine::Vector3 direction;
    float age{};
    float red_boost{};
    float base{};
    float blue{};
    float alpha{};
    float fourth{};
};

// Returns whether the point was added (the original's instruction count is
// 96, or 22 when the spacing check rejects it).
bool append_trail_point(guest::EeMemory& memory, const GuestAddress effect, const bool second_trail,
                        const bool skip_spacing, const TrailPoint& point) {
    const GuestAddress trail = effect + (second_trail ? kSecondTrail : kFirstTrail);
    const std::uint32_t index = memory.read_u32_le(trail + TrailLayout::kNextIndex);
    const GuestAddress position = memory.read_u32_le(trail + TrailLayout::kPositions) + index * 16U;
    if (!skip_spacing) {
        const float since = load_float(memory, effect + kTrailOwnerAge) -
                            load_float(memory, (position | kUncached) + 12U);
        if (since < load_float(memory, trail + TrailLayout::kMinimumAge)) {
            return false;
        }
    }
    const float age = point.age;
    const engine::Vector4 scale = load_vector4(memory, kTrailDirectionScale);
    const engine::Vector3 direction = point.direction * scale.z;
    const auto packed = engine::packed_trail_direction({direction.x, direction.y, direction.z,
                                                        scale.w * point.fourth});
    std::uint64_t packed_bits = 0U;
    for (std::size_t lane = 0U; lane < packed.size(); ++lane) {
        packed_bits |= static_cast<std::uint64_t>(static_cast<std::uint16_t>(packed[lane])) << (16U * lane);
    }
    memory.write_u64_le((memory.read_u32_le(trail + TrailLayout::kDirections) + index * 8U) | kUncachedAccelerated,
                        packed_bits);
    const engine::Vector4 colour_scale = load_vector4(memory, kTrailColourScale);
    const float brightness = point.base + (point.red_boost + point.red_boost);
    memory.write_u32_le((memory.read_u32_le(trail + TrailLayout::kColours) + index * 4U) | kUncachedAccelerated,
                        engine::packed_trail_colour({brightness * colour_scale.x, point.base * colour_scale.y,
                                                     point.blue * colour_scale.z, point.alpha * colour_scale.w}));
    const engine::Vector3 at = point.position;
    store_vector4(memory, position | kUncachedAccelerated, {0.0F + at.x, 0.0F + at.y, 0.0F + at.z, 0.0F + age});
    store_float(memory, memory.read_u32_le(trail + TrailLayout::kSegmentAges) + (index / kTrailPointsPerSegment) * 4U,
                age);
    const std::uint32_t next = index == 0U ? memory.read_u32_le(trail + TrailLayout::kTopIndex) : index;
    memory.write_u32_le(trail + TrailLayout::kNextIndex, next - 1U);
    return true;
}

constexpr std::uint32_t kTrailPointCost = 96U;
constexpr std::uint32_t kTrailPointRejectedCost = 22U;

NativeFunctionExecution append_trail_point(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    TrailPoint point;
    point.position = load_vector4(memory, call.arg(1) & ~15U).xyz();
    point.direction = load_vector4(memory, call.arg(2) & ~15U).xyz();
    point.age = call.float_arg(0);
    point.red_boost = call.float_arg(1);
    point.base = call.float_arg(2);
    point.alpha = call.float_arg(3);
    point.blue = call.float_arg(4);
    point.fourth = call.float_arg(5);
    const bool added = append_trail_point(memory, call.arg(0), call.arg(4) != 0U, call.arg(3) != 0U, point);
    call.return_value(added ? 1U : 0U);
    return call.finish(added ? kTrailPointCost : kTrailPointRejectedCost);
}

// A tyre mark: segments of up to 16 points, each point's position with its
// time in the fourth lane (16 bytes from the pointer at +8) and its sideways
// direction as three signed bytes (from that pointer + 256).
struct TyreMarkSegmentLayout {
    static constexpr GuestAddress kLastTime = 0U;   // float, -1 while unused
    static constexpr GuestAddress kKind = 4U;       // pointer to the kind's 80-byte entry
    static constexpr GuestAddress kPoints = 8U;     // pointer
    static constexpr GuestAddress kCount = 16U;     // s8
    static constexpr GuestAddress kOwner = 17U;     // s8, the mark kind
    static constexpr GuestAddress kFlags = 18U;     // u8
};
constexpr GuestAddress kTyreMarkSides = 256U;       // after the points
constexpr std::int32_t kTyreMarkSegmentPoints = 16;
constexpr std::uint8_t kTyreMarkContinued = 0x02U;  // starts with the previous segment's last point
constexpr GuestAddress kGameTime = 0x00522680U;
constexpr GuestAddress kGameTimeStep = 0x0052267cU;
constexpr GuestAddress kTyreMarkLift = 0x004c1cd0U;     // added to every point
constexpr GuestAddress kTyreMarkStraightness = 0x004e1304U;  // cos of the bend that starts a new point
constexpr float kTyreMarkSideScale = 127.5F;
constexpr float kTyreMarkMinimumStep = 0x1p-32F;  // squared distance

void store_tyre_mark_side(guest::EeMemory& memory, const GuestAddress points, const std::int32_t index,
                          const engine::Vector3 side) {
    const GuestAddress at = points + kTyreMarkSides + static_cast<GuestAddress>(index * 3);
    memory.write_u8(at, static_cast<std::uint8_t>(engine::truncate_word(engine::chop_mul(side.x, kTyreMarkSideScale))));
    memory.write_u8(at + 1U, static_cast<std::uint8_t>(engine::truncate_word(engine::chop_mul(side.y, kTyreMarkSideScale))));
    memory.write_u8(at + 2U, static_cast<std::uint8_t>(engine::truncate_word(engine::chop_mul(side.z, kTyreMarkSideScale))));
}

engine::Vector3 chopped_difference(const engine::Vector3 a, const engine::Vector3 b) {
    return {engine::chop_sub(a.x, b.x), engine::chop_sub(a.y, b.y), engine::chop_sub(a.z, b.z)};
}
float chopped_length_squared(const engine::Vector3 v) {
    return engine::chop_add(engine::chop_add(engine::chop_mul(v.x, v.x), engine::chop_mul(v.y, v.y)),
                            engine::chop_mul(v.z, v.z));
}
float chopped_dot(const engine::Vector3 a, const engine::Vector3 b) {
    return engine::chop_add(engine::chop_add(engine::chop_mul(a.x, b.x), engine::chop_mul(a.y, b.y)),
                            engine::chop_mul(a.z, b.z));
}

// AppendTyreMarkPoint (original 0x002bcf60): adds the position at a1 (lifted,
// then moved by the matrix at a3 when there is one) to the tyre mark segment
// a0, with its sideways direction across the normal at a2. A point that keeps
// the mark straight replaces the last one instead; one too close to it is
// dropped (returns 0, else 1).
struct TyreMarkAppend {
    bool added{};
    std::uint32_t cost{};
};

// `stack` is the original's stack pointer on entry: a segment's first point
// takes its side from 64 bytes below it, which the original leaves unset,
// so this reads the same (leftover) bytes.
TyreMarkAppend append_tyre_mark_point(guest::EeMemory& memory, const GuestAddress segment,
                                      const GuestAddress position, const GuestAddress normal_at,
                                      const GuestAddress matrix_at, const GuestAddress stack) {
    using engine::chop_add;
    using engine::chop_mul;
    const float time = load_float(memory, kGameTime);
    const engine::Vector3 lift = load_vector4(memory, kTyreMarkLift).xyz();
    const engine::Vector3 given = load_vector4(memory, position & ~15U).xyz();
    engine::Vector3 point{chop_add(given.x, lift.x), chop_add(given.y, lift.y), chop_add(given.z, lift.z)};
    if (matrix_at != 0U) {
        const GuestAddress matrix = matrix_at & ~15U;
        const engine::Vector3 rows[4] = {load_vector4(memory, matrix).xyz(), load_vector4(memory, matrix + 16U).xyz(),
                                         load_vector4(memory, matrix + 32U).xyz(),
                                         load_vector4(memory, matrix + 48U).xyz()};
        const auto lane = [&](float engine::Vector3::*component) {
            float sum = chop_mul(rows[0].*component, point.x);
            sum = engine::chop_madd(sum, rows[1].*component, point.y);
            sum = engine::chop_madd(sum, rows[2].*component, point.z);
            return chop_add(sum, rows[3].*component);
        };
        point = {lane(&engine::Vector3::x), lane(&engine::Vector3::y), lane(&engine::Vector3::z)};
    }
    const GuestAddress points = memory.read_u32_le(segment + TyreMarkSegmentLayout::kPoints);
    const auto count = static_cast<std::int8_t>(memory.read_u8(segment + TyreMarkSegmentLayout::kCount));
    const auto point_at = [&](const std::int32_t index) {
        return load_vector4(memory, points + static_cast<GuestAddress>(index * 16)).xyz();
    };
    engine::Vector3 side = load_vector4(memory, stack - 64U).xyz();
    std::int32_t index = count;
    std::uint32_t cost = 69U;
    if (count != 0) {
        const engine::Vector3 last = point_at(count - 1);
        const engine::Vector3 step = chopped_difference(point, last);
        const float length_squared = chopped_length_squared(step);
        if (length_squared < kTyreMarkMinimumStep) {
            return {false, 42U};
        }
        const float inverse = engine::chop_reciprocal_sqrt(length_squared);
        const engine::Vector3 direction{chop_mul(step.x, inverse), chop_mul(step.y, inverse), chop_mul(step.z, inverse)};
        const engine::Vector3 normal = load_vector4(memory, normal_at & ~15U).xyz();
        side = {engine::chop_sub(chop_mul(direction.y, normal.z), chop_mul(normal.y, direction.z)),
                engine::chop_sub(chop_mul(direction.z, normal.x), chop_mul(normal.z, direction.x)),
                engine::chop_sub(chop_mul(direction.x, normal.y), chop_mul(normal.x, direction.y))};
        cost += 45U;
        if (count == 1 && (memory.read_u8(segment + TyreMarkSegmentLayout::kFlags) & kTyreMarkContinued) == 0U) {
            // The second point gives the first its side too.
            store_tyre_mark_side(memory, points, 0, side);
            cost += 20U;
        } else {
            // A point continuing straight on from the last two moves the
            // last one instead of adding another.
            const engine::Vector3 before = chopped_difference(last, point_at(count - 2));
            const float distance = engine::chop_sqrt(chopped_length_squared(before));
            cost += 30U;
            if (!(chopped_dot(direction, before) <= chop_mul(load_float(memory, kTyreMarkStraightness), distance))) {
                index = count - 1;
            }
        }
    }
    const GuestAddress slot = points + static_cast<GuestAddress>(index * 16);
    store_tyre_mark_side(memory, points, index, side);
    const float w = load_float(memory, slot + 12U);
    store_vector4(memory, slot, {chop_add(0.0F, point.x), chop_add(0.0F, point.y), chop_add(0.0F, point.z), w});
    store_float(memory, slot + 12U, chop_add(engine::chop_sub(1.0F, 1.0F), time));
    if (index == count) {
        memory.write_u8(segment + TyreMarkSegmentLayout::kCount, static_cast<std::uint8_t>(count + 1));
    }
    store_float(memory, segment + TyreMarkSegmentLayout::kLastTime, time);
    return {true, cost};
}

NativeFunctionExecution append_tyre_mark_point(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto appended = append_tyre_mark_point(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3),
                                                 call.stack_pointer());
    call.return_value(appended.added ? 1U : 0U);
    return call.finish(appended.cost);
}

// A wheel's tyre mark state: the segment it is drawing (+16) and when it last
// added a point (+20, -1 for never).
struct TyreMarkTrackLayout {
    static constexpr GuestAddress kSegment = 16U;
    static constexpr GuestAddress kLastTime = 20U;
};
// The tyre mark system: a pool of free segments, and each kind's list of
// segments in use (520 bytes per kind).
struct TyreMarkSystemLayout {
    static constexpr GuestAddress kFreeSegments = 0x13a00U;  // pointer to segment pointers
    static constexpr GuestAddress kFreeCount = 0x13a04U;
    static constexpr GuestAddress kKindSegments = 0x13c0cU;  // segment pointers, per kind
    static constexpr GuestAddress kKindCount = 0x13e0cU;
    static constexpr GuestAddress kKindBytes = 520U;
};
constexpr GuestAddress kTyreMarkKinds = 0x0049fff0U;
constexpr GuestAddress kTyreMarkKindBytes = 80U;

// AddTyreMark (original 0x002bd320): adds the position at a2 (normal at a3,
// matrix t1) to wheel a1's tyre mark of kind t0 in the system a0. A wheel
// keeps its segment while it marks every frame and the segment has room;
// otherwise it takes a new one from the pool (continuing from the full
// segment's last point when the mark is unbroken).
// `stack` is the original's stack pointer on entry (see
// append_tyre_mark_point). Returns the original's instruction count.
std::uint32_t add_tyre_mark(guest::EeMemory& memory, const GuestAddress system, const GuestAddress track,
                            const GuestAddress position, const GuestAddress normal, const std::int32_t kind,
                            const GuestAddress matrix, const GuestAddress stack) {
    const float time = load_float(memory, kGameTime);
    const auto recent = [&] {
        const float last = load_float(memory, track + TyreMarkTrackLayout::kLastTime);
        return time <= engine::chop_madd(engine::chop_add(0.0F, last), 1.5F, load_float(memory, kGameTimeStep));
    };
    std::uint32_t cost = 43U;
    const auto count_of = [&](const GuestAddress segment) {
        return static_cast<std::int8_t>(memory.read_u8(segment + TyreMarkSegmentLayout::kCount));
    };
    bool keep = false;
    if (load_float(memory, track + TyreMarkTrackLayout::kLastTime) != -1.0F && recent()) {
        const GuestAddress segment = memory.read_u32_le(track + TyreMarkTrackLayout::kSegment);
        keep = static_cast<std::int8_t>(memory.read_u8(segment + TyreMarkSegmentLayout::kOwner)) == kind &&
               count_of(segment) != kTyreMarkSegmentPoints;
    }
    if (!keep) {
        const std::uint32_t free_count = memory.read_u32_le(system + TyreMarkSystemLayout::kFreeCount);
        if (free_count == 0U) {
            return cost + 10U;
        }
        memory.write_u32_le(system + TyreMarkSystemLayout::kFreeCount, free_count - 1U);
        const GuestAddress segment = memory.read_u32_le(
            memory.read_u32_le(system + TyreMarkSystemLayout::kFreeSegments) + (free_count - 1U) * 4U);
        store_float(memory, segment + TyreMarkSegmentLayout::kLastTime, -1.0F);
        memory.write_u32_le(segment + TyreMarkSegmentLayout::kKind,
                            kTyreMarkKinds + static_cast<GuestAddress>(kind) * kTyreMarkKindBytes);
        memory.write_u8(segment + TyreMarkSegmentLayout::kFlags, 0U);
        memory.write_u8(segment + TyreMarkSegmentLayout::kCount, 0U);
        const GuestAddress lists = system + static_cast<GuestAddress>(kind) * TyreMarkSystemLayout::kKindBytes;
        const std::uint32_t used = memory.read_u32_le(lists + TyreMarkSystemLayout::kKindCount);
        memory.write_u32_le(lists + TyreMarkSystemLayout::kKindCount, used + 1U);
        memory.write_u32_le(lists + used * 4U + TyreMarkSystemLayout::kKindSegments, segment);
        memory.write_u8(segment + TyreMarkSegmentLayout::kOwner, static_cast<std::uint8_t>(kind));
        cost += 32U;
        const GuestAddress previous = memory.read_u32_le(track + TyreMarkTrackLayout::kSegment);
        if (previous != 0U && recent() && count_of(previous) == kTyreMarkSegmentPoints) {
            const GuestAddress from = memory.read_u32_le(previous + TyreMarkSegmentLayout::kPoints);
            const GuestAddress to = memory.read_u32_le(segment + TyreMarkSegmentLayout::kPoints);
            const std::int32_t last = kTyreMarkSegmentPoints - 1;
            const std::int8_t at = count_of(segment);
            copy_quadword(memory, to + static_cast<GuestAddress>(at * 16),
                          from + static_cast<GuestAddress>(last * 16));
            for (GuestAddress byte = 0U; byte < 3U; ++byte) {
                memory.write_u8(to + kTyreMarkSides + static_cast<GuestAddress>(at * 3) + byte,
                                memory.read_u8(from + kTyreMarkSides + static_cast<GuestAddress>(last * 3) + byte));
            }
            memory.write_u8(segment + TyreMarkSegmentLayout::kCount, static_cast<std::uint8_t>(at + 1));
            store_float(memory, segment + TyreMarkSegmentLayout::kLastTime,
                        load_float(memory, from + static_cast<GuestAddress>(last * 16) + 12U));
            memory.write_u8(segment + TyreMarkSegmentLayout::kFlags,
                            static_cast<std::uint8_t>(memory.read_u8(segment + TyreMarkSegmentLayout::kFlags) |
                                                      kTyreMarkContinued));
            cost += 30U;
        }
        memory.write_u32_le(track + TyreMarkTrackLayout::kSegment, segment);
    }
    store_float(memory, track + TyreMarkTrackLayout::kLastTime, time);
    const auto appended = append_tyre_mark_point(memory, memory.read_u32_le(track + TyreMarkTrackLayout::kSegment),
                                                 position, normal, matrix, stack - 80U);
    return cost + appended.cost;
}

NativeFunctionExecution add_tyre_mark(NativeFunctionContext& context) {
    GuestCall call{context};
    return call.finish(add_tyre_mark(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3),
                                     static_cast<std::int32_t>(call.arg(4)), call.arg(5), call.stack_pointer()));
}

// A kind of trail emission (56-byte entries from 0x004bc040): the effect it
// feeds, its rate, and the ranges particles are spread over.
struct EmissionLayout {
    static constexpr GuestAddress kEffect = 0U;         // index of the 336-byte effect
    static constexpr GuestAddress kRate = 4U;
    static constexpr GuestAddress kOffset = 8U;         // half width of the start offset
    static constexpr GuestAddress kSpread = 12U;        // scales the spread vector
    static constexpr GuestAddress kSideways = 16U;      // direction half range across
    static constexpr GuestAddress kUpward = 20U;        // direction range up
    static constexpr GuestAddress kSizeY = 24U;         // fourth lane half range
    static constexpr GuestAddress kGravity = 28U;
    static constexpr GuestAddress kSizeZLow = 32U;
    static constexpr GuestAddress kSizeZHigh = 36U;
    static constexpr GuestAddress kSizeXLow = 40U;
    static constexpr GuestAddress kSizeXHigh = 44U;
    static constexpr GuestAddress kFirstTrailRate = 48U;
    static constexpr GuestAddress kSecondTrailRate = 52U;
};
constexpr GuestAddress kEmissions = 0x004bc040U;
constexpr GuestAddress kEmissionBytes = 56U;
constexpr GuestAddress kEffectsBase = 368U;
constexpr GuestAddress kEffectBytes = 336U;
constexpr GuestAddress kTrailRate = 32U;  // in each trail
// The effect system's random words, clock and gravity scale.
struct EffectSystemLayout {
    static constexpr GuestAddress kRandom = 0x1f828U;
    static constexpr GuestAddress kTime = 0x1f848U;
    static constexpr GuestAddress kStep = 0x1f84cU;
    static constexpr GuestAddress kGravityScale = 0x1f850U;
};
constexpr GuestAddress kEmissionJitter = 0x01d9d190U;  // four floats, rotated a lane per use
constexpr GuestAddress kEmissionJitterGrid = 0x004bbfb0U;
constexpr GuestAddress kEmissionSizeLimit = 0x004e1170U;
constexpr GuestAddress kEmissionCarryScale = 0x004e12ccU;
constexpr GuestAddress kEmissionColour = 0x004e19c8U;  // four floats the colour range comes from
constexpr std::int32_t kMostParticlesPerPass = 8;
constexpr float kWordScale = 0x1p-32F;

// EmitTrailParticles (original 0x0025f0c0): spreads new particles of
// emission kind t0 along the segment a1 to a2 of the effect system a0, into
// the kind's effect's first trail, and first into its second trail when the
// stacked flag is set. How many follows the kind's rate scaled by f12 and f13
// (the fraction carried in the byte at t2); a2-a1 spacing, sizes, colours
// (shaded by the two floats at t1, and f14 for sizes), directions and start
// offsets are random within the kind's ranges. The random terms keep only
// rounding residues, so this runs in the game's own rounding
// (engine/math/chopped.hpp) to place the same particles.
struct TrailEmission {
    GuestAddress system{};
    engine::Vector4 start;
    engine::Vector4 end;
    engine::Vector4 spread;
    std::uint32_t kind{};
    GuestAddress shade{};
    GuestAddress carry{};
    bool skip_spacing{};
    bool both_trails{};
    GuestAddress flag{};  // the caller's flag byte, cleared after the second trail (0 for none)
    float intensity{};
    float rate_scale{};
    float size_scale{};
};

// Returns the original's instruction count; `vector_random` is VU0 R.
std::uint32_t emit_trail_particles(guest::EeMemory& memory, const TrailEmission& emission,
                                   std::uint32_t& vector_random_bits) {
    using engine::chop_add;
    using engine::chop_madd;
    using engine::chop_mul;
    using engine::chop_sub;
    const GuestAddress system = emission.system;
    const engine::Vector4 start = emission.start;
    const engine::Vector4 end = emission.end;
    const engine::Vector4 spread = emission.spread;
    const GuestAddress kind = kEmissions + emission.kind * kEmissionBytes;
    const GuestAddress shade = emission.shade;
    const GuestAddress carry = emission.carry;
    const bool skip_spacing = emission.skip_spacing;
    const float intensity = emission.intensity;
    const float rate_scale = emission.rate_scale;
    const float size_scale = emission.size_scale;
    const auto field = [&](const GuestAddress offset) { return load_float(memory, kind + offset); };
    const GuestAddress effect =
        system + kEffectsBase + memory.read_u32_le(kind + EmissionLayout::kEffect) * kEffectBytes;
    const GuestAddress random_at = system + EffectSystemLayout::kRandom;
    engine::EffectRandom random{memory.read_u32_le(random_at), memory.read_u32_le(random_at + 4U)};
    const auto unit_random = [&] { return chop_mul(kWordScale, engine::chop_from_unsigned(random.step())); };
    // The original's instructions: 137 to the early return, 121 more to set up,
    // 103 a pass, 42 a particle, 92 more to emit one plus AppendTrailPoint.
    std::uint32_t cost = 137U;

    const float rate = chop_mul(field(EmissionLayout::kRate), chop_mul(intensity, rate_scale));
    const float carried = chop_mul(0.00390625F, engine::chop_from_int(memory.read_u8(carry)));
    const float wobble = chop_madd(chop_add(0.0F, 0.875F), 0.25F, unit_random());
    const float amount = chop_mul(rate, wobble);
    std::int32_t first_count;
    std::int32_t second_count = 0;
    float fraction;
    if (emission.both_trails) {
        const float first = chop_mul(chop_mul(amount, field(EmissionLayout::kFirstTrailRate)),
                                     load_float(memory, effect + kFirstTrail + kTrailRate));
        const float second = chop_mul(chop_mul(amount, field(EmissionLayout::kSecondTrailRate)),
                                      load_float(memory, effect + kSecondTrail + kTrailRate));
        const float first_total = chop_add(first, carried);
        first_count = engine::truncate_word(first_total);
        const float second_total = chop_add(second, chop_sub(first_total, engine::chop_from_int(first_count)));
        second_count = engine::truncate_word(second_total);
        fraction = chop_sub(second_total, engine::chop_from_int(second_count));
    } else {
        const float total = chop_add(chop_mul(amount, load_float(memory, effect + kFirstTrail + kTrailRate)), carried);
        first_count = engine::truncate_word(total);
        fraction = chop_sub(total, engine::chop_from_int(first_count));
    }
    memory.write_u8(carry, static_cast<std::uint8_t>(
                               engine::truncate_word(chop_mul(load_float(memory, kEmissionCarryScale), fraction))));
    if (static_cast<std::uint32_t>(first_count) + static_cast<std::uint32_t>(second_count) == 0U) {
        memory.write_u32_le(random_at, random.first);
        memory.write_u32_le(random_at + 4U, random.second);
        return cost;
    }
    // Counts become unsigned words, as the original's packed minimum leaves them.
    const auto passes_first = static_cast<std::int64_t>(static_cast<std::uint32_t>(
        first_count < kMostParticlesPerPass ? first_count : kMostParticlesPerPass));
    const auto passes_second = static_cast<std::int64_t>(static_cast<std::uint32_t>(
        second_count < kMostParticlesPerPass ? second_count : kMostParticlesPerPass));

    cost += 121U;
    const float spread_scale = field(EmissionLayout::kSpread);
    const float gravity =
        chop_mul(field(EmissionLayout::kGravity),
                 chop_mul(intensity, load_float(memory, system + EffectSystemLayout::kGravityScale)));
    const engine::Vector3 quarter_spread{chop_mul(chop_mul(chop_mul(spread.x, spread_scale), 0.5F), 0.5F),
                                         chop_mul(chop_mul(chop_mul(spread.y, spread_scale), 0.5F), 0.5F),
                                         chop_mul(chop_mul(chop_mul(spread.z, spread_scale), 0.5F), 0.5F)};
    const engine::Vector3 fall{0.0F, chop_add(0.0F, gravity), 0.0F};
    engine::VectorRandom vector_random;
    vector_random.seed(unit_random());

    const float size_x_high = chop_mul(size_scale, field(EmissionLayout::kSizeXHigh));
    const float size_x_low = chop_mul(size_scale, field(EmissionLayout::kSizeXLow));
    const float size_y = field(EmissionLayout::kSizeY);
    const engine::Vector3 size_base{size_x_high, size_y, field(EmissionLayout::kSizeZHigh)};
    const engine::Vector3 size_range{chop_sub(size_x_high, size_x_low), chop_sub(size_y, -size_y),
                                     chop_sub(field(EmissionLayout::kSizeZHigh), field(EmissionLayout::kSizeZLow))};
    const float sideways = field(EmissionLayout::kSideways);
    const engine::Vector3 direction_high{chop_add(sideways, quarter_spread.x),
                                         chop_add(field(EmissionLayout::kUpward), quarter_spread.y),
                                         chop_add(sideways, quarter_spread.z)};
    const engine::Vector3 direction_low{chop_add(-sideways, quarter_spread.x), chop_add(0.0F, quarter_spread.y),
                                        chop_add(-sideways, quarter_spread.z)};
    const engine::Vector3 direction_range{chop_sub(direction_high.x, direction_low.x),
                                          chop_sub(direction_high.y, direction_low.y),
                                          chop_sub(direction_high.z, direction_low.z)};
    const float offset = field(EmissionLayout::kOffset);
    const float offset_range = chop_sub(offset, chop_sub(0.0F, offset));
    const auto colour_term = [&](const GuestAddress index) { return load_float(memory, kEmissionColour + index * 4U); };
    const float red_low = chop_mul(load_float(memory, shade), colour_term(0U));
    const float red_high = chop_add(colour_term(1U), red_low);
    const float green_high = chop_mul(load_float(memory, shade + 4U), chop_add(colour_term(2U), colour_term(3U)));
    const float green_low = chop_mul(load_float(memory, shade + 4U), colour_term(2U));
    const engine::Vector4 colour_base{chop_sub(chop_add(red_low, red_low), red_high),
                                      chop_sub(chop_add(green_low, green_low), green_high), -1.0F, -1.0F};
    const engine::Vector4 colour_range{chop_sub(red_high, red_low), chop_sub(green_high, green_low), 1.0F, 1.0F};
    const float size_limit = load_float(memory, kEmissionSizeLimit);
    const engine::Vector4 grid = load_vector4(memory, kEmissionJitterGrid);
    engine::Vector4 jitter = load_vector4(memory, kEmissionJitter);
    const auto rotate_jitter = [&] { jitter = {jitter.y, jitter.z, jitter.w, jitter.x}; };
    // The rounding residue of jitter * random on the grid, one lane at a time.
    const auto residue = [&](const float scale, const float lane_random, const float lane_grid) {
        const float scaled = chop_mul(scale, lane_random);
        return chop_sub(chop_sub(chop_add(scaled, lane_grid), lane_grid), scaled);
    };
    const auto jittered = [&]() {
        const float x = vector_random.next();
        const float y = vector_random.next();
        const float z = vector_random.next();
        return engine::Vector3{residue(jitter.x, x, grid.x), residue(jitter.y, y, grid.y),
                               residue(jitter.z, z, grid.z)};
    };
    const auto lerp = [&](const engine::Vector3 base, const engine::Vector3 range, const engine::Vector3 weight) {
        return engine::Vector3{chop_madd(chop_add(base.x, 0.0F), range.x, weight.x),
                               chop_madd(chop_add(base.y, 0.0F), range.y, weight.y),
                               chop_madd(chop_add(base.z, 0.0F), range.z, weight.z)};
    };

    bool second_trail = emission.both_trails;
    for (;;) {
        std::int64_t remaining = second_trail ? passes_second : passes_first;
        if (remaining > 0) {
            random.step();
            const float inverse = 1.0F / engine::chop_from_int(static_cast<std::int32_t>(remaining));
            const engine::Vector3 step{chop_mul(chop_sub(end.x, start.x), inverse),
                                       chop_mul(chop_sub(end.y, start.y), inverse),
                                       chop_mul(chop_sub(end.z, start.z), inverse)};
            const float along = chop_mul(kWordScale, engine::chop_from_unsigned(random.first));
            const float time_step = load_float(memory, system + EffectSystemLayout::kStep);
            const float age_step = chop_mul(time_step, inverse);
            float age = chop_sub(load_float(memory, system + EffectSystemLayout::kTime), time_step);
            engine::Vector3 at{chop_add(start.x, chop_mul(step.x, along)), chop_add(start.y, chop_mul(step.y, along)),
                               chop_add(start.z, chop_mul(step.z, along))};
            cost += 103U;
            for (;;) {
                const engine::Vector3 size = lerp(size_base, size_range, jittered());
                rotate_jitter();
                store_vector4(memory, kEmissionJitter, jitter);
                cost += 42U;
                if (!(size.x <= size_limit)) {
                    float lanes[4];
                    for (float& lane : lanes) {
                        lane = vector_random.next();
                    }
                    const engine::Vector4 colour{chop_madd(chop_add(colour_base.x, 0.0F), colour_range.x, lanes[0]),
                                                 chop_madd(chop_add(colour_base.y, 0.0F), colour_range.y, lanes[1]),
                                                 chop_madd(chop_add(colour_base.z, 0.0F), colour_range.z, lanes[2]),
                                                 chop_madd(chop_add(colour_base.w, 0.0F), colour_range.w, lanes[3])};
                    const engine::Vector3 shift = lerp({offset, offset, offset},
                                                       {offset_range, offset_range, offset_range}, jittered());
                    rotate_jitter();
                    const engine::Vector3 heading = lerp(direction_high, direction_range, jittered());
                    rotate_jitter();
                    store_vector4(memory, kEmissionJitter, jitter);
                    const auto lane3 = [&](const float a, const float b, const float c, const float d, const float e) {
                        return chop_sub(chop_add(chop_add(a, chop_mul(b, colour.z)), chop_mul(c, colour.w)),
                                        chop_mul(d, e));
                    };
                    TrailPoint point;
                    point.direction = {lane3(heading.x, quarter_spread.x, fall.x, shift.x, 0.5F),
                                       lane3(heading.y, quarter_spread.y, fall.y, shift.y, 0.5F),
                                       lane3(heading.z, quarter_spread.z, fall.z, shift.z, 0.5F)};
                    point.base = colour.y;
                    point.red_boost = colour.x;
                    if (colour.y < 1.0F) {
                        if (!(chop_add(colour.y, colour.x) <= 1.0F)) {
                            point.red_boost = chop_sub(1.0F, colour.y);
                        }
                    } else {
                        point.base = 1.0F;
                        point.red_boost = 0.0F;
                    }
                    point.position = {chop_add(at.x, shift.x), chop_add(at.y, shift.y), chop_add(at.z, shift.z)};
                    point.age = age;
                    point.alpha = size.z;
                    point.blue = size.x;
                    point.fourth = size.y;
                    const bool added = append_trail_point(memory, effect, second_trail, skip_spacing, point);
                    cost += 92U + (added ? kTrailPointCost : kTrailPointRejectedCost);
                    if (!added) {
                        break;
                    }
                }
                age = chop_add(age, age_step);
                at = {chop_add(at.x, step.x), chop_add(at.y, step.y), chop_add(at.z, step.z)};
                if (--remaining == 0) {
                    break;
                }
            }
        }
        if (!second_trail) {
            break;
        }
        second_trail = false;
        if (emission.flag != 0U) {
            memory.write_u8(emission.flag, 0U);
        }
    }
    memory.write_u32_le(random_at, random.first);
    memory.write_u32_le(random_at + 4U, random.second);
    vector_random_bits = vector_random.bits;
    return cost;
}

NativeFunctionExecution emit_trail_particles(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    TrailEmission emission;
    emission.system = call.arg(0);
    emission.start = load_vector4(memory, call.arg(1) & ~15U);
    emission.end = load_vector4(memory, call.arg(2) & ~15U);
    emission.spread = load_vector4(memory, call.arg(3) & ~15U);
    emission.kind = call.arg(4);
    emission.shade = call.arg(5);
    emission.carry = call.arg(6);
    emission.skip_spacing = call.arg(7) != 0U;
    emission.flag = call.stack_pointer();  // the first stacked argument
    emission.both_trails = memory.read_u8(emission.flag) != 0U;
    emission.intensity = call.float_arg(0);
    emission.rate_scale = call.float_arg(1);
    emission.size_scale = call.float_arg(2);
    std::uint32_t vector_random = call.vector_random();
    const std::uint32_t cost = emit_trail_particles(memory, emission, vector_random);
    call.set_vector_random(vector_random);
    return call.finish(cost);
}

// A wheel's surface effect state (192 bytes per wheel from the vehicle's
// +2080).
struct WheelEffectsLayout {
    static constexpr GuestAddress kPosition = 0U;
    static constexpr GuestAddress kContact = 16U;
    static constexpr GuestAddress kNormal = 32U;
    static constexpr GuestAddress kPrevious = 48U;
    static constexpr GuestAddress kSlip = 80U;
    static constexpr GuestAddress kSlipScale = 92U;
    static constexpr GuestAddress kMarkState = 120U;  // u32: 0 to 6 select the mark (see below)
    static constexpr GuestAddress kTrack = 128U;      // the tyre mark track
    static constexpr GuestAddress kFades = 160U;      // u8 x3: dust, smoke and spray levels
    static constexpr GuestAddress kCarries = 164U;    // u8 x4: emission carries
    static constexpr GuestAddress kSurface = 176U;    // u8
    static constexpr GuestAddress kTouching = 179U;   // u8
};
constexpr GuestAddress kVehicleWheels = 2080U;
constexpr GuestAddress kWheelBytes = 192U;
constexpr GuestAddress kVehiclePlayer = 533U;    // s8, 1 for the player's car
constexpr GuestAddress kVehicleBody = 3264U;     // pointer to the body matrix
constexpr GuestAddress kTyreMarkSystem = 9728U;  // in the effect system
constexpr GuestAddress kSurfaceEffects = 0x004bc650U;  // per surface: three levels, then the emission kind (s8)
constexpr GuestAddress kPlayerSettings = 0x005179c0U;  // 2992 bytes per player; bit 5 drops front wheel marks
constexpr GuestAddress kPlayerSettingsBytes = 2992U;
constexpr GuestAddress kFadeToEmission = 0x004e1010U;
constexpr GuestAddress kMinimumEffectSpeed = 0x004e1308U;
constexpr GuestAddress kRearWheelPush = 0x004e0e00U;
constexpr std::int32_t kNoSurfaceEmission = 26;
constexpr float kMaximumWheelStep = 100.0F;
constexpr float kFadeRate = 510.0F;  // levels a second

// UpdateWheelSurfaceEffects (original 0x002603f0): wheel a2 of vehicle a1
// leaves tyre marks for its surface and mark state, fades its dust, smoke
// and spray levels towards the surface's (byte levels at +160), and emits
// trail particles for each along its path this frame (moved by the matrix
// at a3 when there is one). t0 shades the particles and t1 is passed to the
// emitter as its two-trail flag; t2 picks the player whose settings may
// turn front wheel marks off.
NativeFunctionExecution update_wheel_surface_effects(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    using engine::chop_add;
    using engine::chop_mul;
    using engine::chop_sub;
    const GuestAddress system = call.arg(0);
    const GuestAddress vehicle = call.arg(1);
    const std::uint32_t wheel = call.arg(2);
    const GuestAddress matrix = call.arg(3);
    const GuestAddress shade = call.arg(4);
    const std::uint64_t both_trails_word = call.arg64(5);
    const bool both_trails = (both_trails_word & 0xffU) != 0U;
    const GuestAddress stack = call.stack_pointer() - 400U;
    const GuestAddress state = vehicle + kVehicleWheels + wheel * kWheelBytes;
    const bool player = static_cast<std::int8_t>(memory.read_u8(vehicle + kVehiclePlayer)) == 1;
    const std::uint8_t touching = memory.read_u8(state + WheelEffectsLayout::kTouching);
    const std::uint8_t surface = memory.read_u8(state + WheelEffectsLayout::kSurface);
    const GuestAddress surface_effects = kSurfaceEffects + surface * 16U;
    std::uint32_t vector_random = call.vector_random();
    std::uint32_t cost = 130U;
    const auto mark = [&](const std::int32_t kind) {
        cost += 10U + add_tyre_mark(memory, system + kTyreMarkSystem, state + WheelEffectsLayout::kTrack,
                                    state + WheelEffectsLayout::kContact, state + WheelEffectsLayout::kNormal, kind,
                                    matrix, stack);
    };

    bool marking = touching != 0U;
    if (marking && player) {
        const auto player_index = static_cast<std::int32_t>(static_cast<std::int8_t>(call.arg(6) & 0xffU));
        const GuestAddress settings =
            kPlayerSettings + static_cast<GuestAddress>(player_index * static_cast<std::int32_t>(kPlayerSettingsBytes));
        if ((memory.read_u32_le(settings) & 0x20U) != 0U && wheel < 2U) {
            marking = false;
        }
    }
    if (marking) {
        if (surface == 4U || surface == 14U) {
            mark(2);
            marking = false;
        } else if (surface == 11U) {
            mark(3);
            marking = false;
        }
    }
    float scale = 0.0F;
    if (touching != 0U) {
        const std::uint32_t mark_state = memory.read_u32_le(state + WheelEffectsLayout::kMarkState);
        switch (mark_state) {
        case 1U:
            scale = 1.0F;
            break;
        case 2U:
        case 3U:
        case 5U:
        case 6U:
            scale = 1.0F;
            if (marking) {
                mark(1);
            }
            break;
        case 4U:
            if (marking) {
                mark(1);
            }
            break;
        default:
            break;
        }
    }
    std::int32_t emission_kind = kNoSurfaceEmission;
    float targets[3] = {0.0F, 0.0F, 0.0F};
    if (touching != 0U) {
        if (wheel >= 2U) {
            emission_kind = static_cast<std::int8_t>(memory.read_u8(surface_effects + 12U));
        }
        targets[0] = chop_mul(load_float(memory, surface_effects), scale);
        targets[1] = load_float(memory, surface_effects + 4U);
        targets[2] = load_float(memory, surface_effects + 8U);
    }
    for (GuestAddress level = 0U; level < 3U; ++level) {
        const float step = load_float(memory, system + EffectSystemLayout::kStep);
        const float current = engine::chop_from_int(memory.read_u8(state + WheelEffectsLayout::kFades + level));
        const float target = chop_mul(targets[level], 255.0F);
        const float moved = target <= current
                                ? engine::maximum(chop_sub(chop_add(0.0F, current), chop_mul(kFadeRate, step)), target)
                                : engine::minimum(engine::chop_madd(chop_add(0.0F, current), kFadeRate, step), target);
        memory.write_u8(state + WheelEffectsLayout::kFades + level,
                        static_cast<std::uint8_t>(engine::truncate_word(chop_add(0.5F, moved))));
    }
    const auto fade = [&](const GuestAddress level) {
        return memory.read_u8(state + WheelEffectsLayout::kFades + level);
    };
    cost += 70U;
    if (emission_kind == kNoSurfaceEmission && (fade(0U) | fade(1U) | fade(2U)) == 0U) {
        call.set_vector_random(vector_random);
        return call.finish(cost);
    }

    cost += 85U;
    const engine::Vector3 position = load_vector4(memory, state + WheelEffectsLayout::kPosition).xyz();
    const engine::Vector3 previous = load_vector4(memory, state + WheelEffectsLayout::kPrevious).xyz();
    engine::Vector3 moved = chopped_difference(position, previous);
    const float rate = load_float(memory, system + EffectSystemLayout::kGravityScale);
    engine::Vector3 velocity{chop_mul(moved.x, rate), chop_mul(moved.y, rate), chop_mul(moved.z, rate)};
    float speed = engine::chop_sqrt(chopped_length_squared(moved));
    if (!(speed <= kMaximumWheelStep)) {
        moved = {};
        velocity = {};
        speed = 0.0F;
    }
    float strength = 1.0F;
    if (wheel >= 2U) {
        const float slip = chop_mul(load_float(memory, state + WheelEffectsLayout::kSlip),
                                    load_float(memory, state + WheelEffectsLayout::kSlipScale));
        const float push = load_float(memory, kRearWheelPush);
        const engine::Vector3 up = load_vector4(memory, memory.read_u32_le(vehicle + kVehicleBody) + 32U).xyz();
        velocity = {chop_sub(velocity.x, chop_mul(chop_mul(up.x, slip), push)),
                    chop_sub(velocity.y, chop_mul(chop_mul(up.y, slip), push)),
                    chop_sub(velocity.z, chop_mul(chop_mul(up.z, slip), push))};
        const float step = load_float(memory, system + EffectSystemLayout::kStep);
        if (memory.read_u32_le(state + WheelEffectsLayout::kMarkState) == 1U) {
            speed = chop_mul(push, chop_mul(slip, step));
            const float back = -step;
            moved = {chop_mul(chop_mul(velocity.x, back), push), chop_mul(chop_mul(velocity.y, back), push),
                     chop_mul(chop_mul(velocity.z, back), push)};
        }
        const float travelled = chop_mul(engine::chop_sqrt(chopped_length_squared(velocity)), step);
        if (!(travelled <= speed)) {
            strength = travelled <= chop_add(speed, speed) ? travelled / speed : 2.0F;
        }
        cost += 40U;
    }
    if (speed < load_float(memory, kMinimumEffectSpeed)) {
        call.set_vector_random(vector_random);
        return call.finish(cost);
    }
    const engine::Vector3 contact = load_vector4(memory, state + WheelEffectsLayout::kContact).xyz();
    engine::Vector3 end{chop_add(contact.x, 0.0F), chop_add(contact.y, 0.125F), chop_add(contact.z, 0.0F)};
    engine::Vector3 start = chopped_difference(end, moved);
    if (matrix != 0U) {
        const GuestAddress rows = matrix & ~15U;
        const engine::Vector3 r[4] = {load_vector4(memory, rows).xyz(), load_vector4(memory, rows + 16U).xyz(),
                                      load_vector4(memory, rows + 32U).xyz(), load_vector4(memory, rows + 48U).xyz()};
        const auto transform = [&](const engine::Vector3 v) {
            const auto lane = [&](float engine::Vector3::*component) {
                float sum = chop_mul(r[0].*component, v.x);
                sum = engine::chop_madd(sum, r[1].*component, v.y);
                sum = engine::chop_madd(sum, r[2].*component, v.z);
                return chop_add(sum, r[3].*component);
            };
            return engine::Vector3{lane(&engine::Vector3::x), lane(&engine::Vector3::y), lane(&engine::Vector3::z)};
        };
        start = transform(start);
        end = transform(end);
    }
    TrailEmission emission;
    emission.system = system;
    emission.start = {start.x, start.y, start.z, 0.0F};
    emission.end = {end.x, end.y, end.z, 0.0F};
    emission.spread = {velocity.x, velocity.y, velocity.z, 0.0F};
    emission.shade = shade;
    emission.intensity = speed;
    const auto emit = [&](const std::uint32_t kind, const GuestAddress carry, const bool skip_spacing,
                          const bool both, const float rate_scale, const float size_scale) {
        emission.kind = kind;
        emission.carry = state + WheelEffectsLayout::kCarries + carry;
        emission.skip_spacing = skip_spacing;
        emission.both_trails = both;
        emission.rate_scale = rate_scale;
        emission.size_scale = size_scale;
        cost += 15U + emit_trail_particles(memory, emission, vector_random);
    };
    const auto level_terms = [&](const GuestAddress level, float& rate_scale, float& size_scale) {
        const float amount = chop_mul(load_float(memory, kFadeToEmission), engine::chop_from_int(fade(level)));
        size_scale = engine::minimum(chop_add(amount, amount), 1.0F);
        rate_scale = chop_mul(engine::chop_madd(chop_add(0.0F, 0.75F), 0.25F, amount), strength);
    };
    float rate_scale;
    float size_scale;
    if (fade(0U) != 0U) {
        level_terms(0U, rate_scale, size_scale);
        emit(0U, 0U, both_trails_word != 0U && player, both_trails, rate_scale, size_scale);
        if (player) {
            emit(1U, 0U, true, false, 1.0F, size_scale);
        }
    }
    if (fade(1U) != 0U) {
        level_terms(1U, rate_scale, size_scale);
        emit(3U, 1U, false, both_trails, rate_scale, size_scale);
    }
    if (fade(2U) != 0U) {
        level_terms(2U, rate_scale, size_scale);
        emit(4U, 2U, false, both_trails, rate_scale, size_scale);
    }
    if (emission_kind != kNoSurfaceEmission) {
        emit(static_cast<std::uint32_t>(emission_kind), 3U, false, both_trails, strength, 1.0F);
    }
    call.set_vector_random(vector_random);
    return call.finish(cost);
}

// The fourth words of the screen projection rows: register or stack
// contents in the original.
constexpr std::array kCameraProjectionUnusedWords{
    UnusedWord{-1, 12U},
    UnusedWord{-1, 28U},
    UnusedWord{-1, 44U},
    UnusedWord{-1, 60U},
};
// The depth offset is half the difference of two values near 2^24 that
// cancel: the original's sums, rounded toward zero, land one unit lower, so
// it is -0.5 where rounding to nearest gives 0 (of a 16.7 million depth
// range).
constexpr std::array kRenderViewLooseWords{LooseWord{376U, 0.5F}};
constexpr std::array kRenderViewUnusedWords{
    UnusedWord{-1, 236U},
    UnusedWord{-1, 252U},
    UnusedWord{-1, 268U},
    UnusedWord{-1, 284U},
};

// The race chase camera (the update in the camera mode table at 0x004de288).
// a0 is the camera controller, a1 the view it updates, f12 the frame time.
struct ChaseCameraLayout {
    static constexpr GuestAddress kSettings = 8U;    // pointer, ChaseCameraSettingsLayout
    static constexpr GuestAddress kYaw = 24U;
    static constexpr GuestAddress kPitch = 28U;
    static constexpr GuestAddress kPlayer = 32U;     // s32
    static constexpr GuestAddress kTracksYaw = 36U;  // u8
    static constexpr GuestAddress kReversed = 38U;   // u8
};
struct ChaseCameraSettingsLayout {
    static constexpr GuestAddress kOffset = 0U;
    static constexpr GuestAddress kFollowPoint = 16U;
    static constexpr GuestAddress kPitchResponse = 32U;
    static constexpr GuestAddress kYawResponse = 36U;
    static constexpr GuestAddress kRoll = 48U;
};
struct CameraViewLayout {
    static constexpr GuestAddress kFov = 4U;
    static constexpr GuestAddress kOrientation = 16U;  // quaternion
    static constexpr GuestAddress kPosition = 32U;
};
constexpr GuestAddress kPlayerCars = 0x0055efecU;   // per player, 48 bytes apart: the car pointer first
constexpr GuestAddress kPlayerRecordPointers = 0x0064d388U;  // per player
constexpr GuestAddress kPlayerRecordZoom = 0x118cU + 16U;
constexpr GuestAddress kCarVelocityLanes = 176U;    // w is the speed
constexpr GuestAddress kCarTransform = 516U;        // pointer
constexpr GuestAddress kCameraSpeedScale = 0x004e0d6cU;      // 2.2369 (metres per second to mph)
constexpr GuestAddress kCameraSpeedFactor = 0x004e0ef4U;     // 0.02
constexpr GuestAddress kCameraStepsPerSecond = 0x004e1c88U;  // 60
constexpr GuestAddress kCameraNarrowFov = 0x004e1c90U;       // 90
constexpr GuestAddress kCameraWideFov = 0x004e1c8cU;         // 110
constexpr GuestAddress kCameraPullBack = 0x004e1c94U;        // 2.3
constexpr GuestAddress kCameraSlowSpeed = 0x004e1c98U;
constexpr GuestAddress kCameraResponseExponent = 0x004e0e60U;  // 1.2
constexpr GuestAddress kCameraResponsePower = 0x004e2910U;     // u8
constexpr GuestAddress kCameraPitchAxis = 0x004c4260U;
constexpr GuestAddress kCameraYawAxis = 0x004c4270U;
constexpr GuestAddress kCameraDegreesPerRadian = 0x004e069cU;
constexpr GuestAddress kCameraRadiansPerDegree = 0x004e0d60U;
// The stack word (below the caller's stack pointer) whose fold the original
// leaves in the position's fourth lane.
constexpr GuestAddress kChaseCameraStaleLaneBelowEntry = 9476U;

// Set from the frame pacing options; read on whichever thread runs the game.
std::atomic<bool> chase_camera_real_time_easing{false};

engine::ChaseCameraTuning chase_camera_tuning(const guest::EeMemory& memory) {
    return {load_float(memory, kCameraSpeedScale),     load_float(memory, kCameraSpeedFactor),
            load_float(memory, kCameraStepsPerSecond), load_float(memory, kCameraDegreesPerRadian),
            load_float(memory, kCameraRadiansPerDegree), load_vector4(memory, kCameraPitchAxis).xyz(),
            load_vector4(memory, kCameraYawAxis).xyz(), load_float(memory, kCameraNarrowFov),
            load_float(memory, kCameraWideFov),        load_float(memory, kCameraPullBack),
            memory.read_u8(kCameraResponsePower) != 0U, load_float(memory, kCameraResponseExponent)};
}

GuestAddress chase_camera_car(const guest::EeMemory& memory, const GuestAddress controller) {
    const auto player = memory.read_u32_le(controller + ChaseCameraLayout::kPlayer);
    return memory.read_u32_le(kPlayerCars + player * 48U);
}

// The original's instruction count for one update, path by path (its own
// instructions, the zoom curve helper's 8, and the C library natives it
// calls: asinf, acosf and tanf at 60 each, powf at 120).
std::uint32_t chase_camera_instructions(const engine::ChaseCameraView& view, const engine::ChaseCameraState& state,
                                        const bool response_power) {
    std::uint32_t count = 1190U + 8U + 3U * 60U;
    count += 7U * static_cast<std::uint32_t>(view.response_steps);
    count += view.speed_factor_capped ? 1U : 0U;
    count += state.reversed ? 9U : 0U;
    count += view.yaw_swing_flipped ? 4U : 0U;
    count += state.tracks_yaw ? 4U : 0U;
    count += response_power ? 9U + 2U * 120U : 0U;
    switch (view.orientation_case) {
    case 0:
        count += 39U;
        break;
    case 1:
        count += 40U;
        break;
    case 2:
        count += view.second_diagonal_tested ? 36U : 33U;
        break;
    default:
        break;
    }
    return count;
}

struct ChaseCameraCall {
    engine::ChaseCameraState state;
    engine::ChaseCameraView view;
    std::uint32_t instructions{};
};

// One update of the chase camera of controller a0 by `frame_seconds`, read
// from guest memory without writing it; `stack_pointer` is the caller's.
ChaseCameraCall evaluate_chase_camera(const guest::EeMemory& memory, const GuestAddress controller,
                                      const float frame_seconds, const GuestAddress stack_pointer) {
    const auto player = memory.read_u32_le(controller + ChaseCameraLayout::kPlayer);
    const GuestAddress car = chase_camera_car(memory, controller);
    const GuestAddress transform = memory.read_u32_le(car + kCarTransform);
    const GuestAddress settings_address = memory.read_u32_le(controller + ChaseCameraLayout::kSettings);
    ChaseCameraCall call;
    call.state = {load_float(memory, controller + ChaseCameraLayout::kYaw),
                  load_float(memory, controller + ChaseCameraLayout::kPitch),
                  memory.read_u8(controller + ChaseCameraLayout::kTracksYaw) != 0U,
                  memory.read_u8(controller + ChaseCameraLayout::kReversed) != 0U};
    const engine::Vector4 offset = load_vector4(memory, settings_address + ChaseCameraSettingsLayout::kOffset);
    const engine::ChaseCameraSettings settings{
        offset.xyz(), offset.w, load_vector4(memory, settings_address + ChaseCameraSettingsLayout::kFollowPoint).xyz(),
        load_float(memory, settings_address + ChaseCameraSettingsLayout::kPitchResponse),
        load_float(memory, settings_address + ChaseCameraSettingsLayout::kYawResponse),
        load_float(memory, settings_address + ChaseCameraSettingsLayout::kRoll)};
    engine::ChaseCameraInput input;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        input.car_rows[row] = load_vector4(memory, transform + row * 16U);
    }
    input.car_speed = load_float(memory, car + kCarVelocityLanes + 12U);
    input.frame_seconds = frame_seconds;
    input.real_time_easing = chase_camera_real_time_easing.load(std::memory_order_relaxed);
    input.zoom = load_float(memory, memory.read_u32_le(kPlayerRecordPointers + player * 4U) + kPlayerRecordZoom);
    const float stale = engine::game_operand(load_float(memory, stack_pointer - kChaseCameraStaleLaneBelowEntry));
    const float folded = engine::wave_fold_chopped(stale);
    input.position_lane = engine::chop_mul(folded, folded);
    const engine::ChaseCameraTuning tuning = chase_camera_tuning(memory);
    call.view = engine::update_chase_camera(call.state, settings, tuning, input);
    call.instructions = chase_camera_instructions(call.view, call.state, tuning.response_power);
    return call;
}

// The call's cost, or 0 below the slow speed: there the original goes on to
// a low speed adjustment (0x002f6f60) that is not native yet, so those calls
// run as original code.
std::uint32_t chase_camera_cost(const NativeFunctionPrepareContext& context) {
    constexpr unsigned kStackPointer = 29U;
    constexpr unsigned kFrameSeconds = 12U;
    const GuestAddress controller = context.cpu.read_gpr_u32(4U);
    const GuestAddress car = chase_camera_car(context.memory, controller);
    if (engine::chase_camera_is_slow(load_float(context.memory, car + kCarVelocityLanes + 12U),
                                     chase_camera_tuning(context.memory),
                                     load_float(context.memory, kCameraSlowSpeed))) {
        return 0U;
    }
    return evaluate_chase_camera(context.memory, controller,
                                 std::bit_cast<float>(context.cpu.read_fpr_bits(kFrameSeconds)),
                                 context.cpu.read_gpr_u32(kStackPointer))
        .instructions;
}

NativeFunctionExecution update_chase_camera(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress controller = call.arg(0);
    const GuestAddress view_address = call.arg(1);
    const ChaseCameraCall result = evaluate_chase_camera(memory, controller, call.float_arg(0), call.stack_pointer());
    store_float(memory, controller + ChaseCameraLayout::kPitch, result.state.pitch_degrees);
    if (result.state.tracks_yaw) {
        store_float(memory, controller + ChaseCameraLayout::kYaw, result.state.yaw_degrees);
    }
    store_float(memory, view_address + CameraViewLayout::kFov, result.view.fov_degrees);
    store_vector4(memory, view_address + CameraViewLayout::kOrientation, result.view.orientation);
    store_vector4(memory, view_address + CameraViewLayout::kPosition, result.view.position);
    return call.finish(0U);  // reported from chase_camera_cost
}

// The fourth words of the camera and projection rows: stack contents in the
// original.
constexpr std::array kRenderViewFromCameraUnusedWords{
    UnusedWord{-1, 172U}, UnusedWord{-1, 188U}, UnusedWord{-1, 204U}, UnusedWord{-1, 220U},
    UnusedWord{-1, 236U}, UnusedWord{-1, 252U}, UnusedWord{-1, 268U}, UnusedWord{-1, 284U},
};

// The fourth words of every view's camera and projection rows (register
// and stack contents in the original).
constexpr std::array kReflectionUnusedWords{
    UnusedWord{-1, 1308U}, UnusedWord{-1, 1324U}, UnusedWord{-1, 1340U}, UnusedWord{-1, 1356U}, UnusedWord{-1, 1372U},
    UnusedWord{-1, 1388U}, UnusedWord{-1, 1404U}, UnusedWord{-1, 1420U}, UnusedWord{-1, 1868U}, UnusedWord{-1, 1884U},
    UnusedWord{-1, 1900U}, UnusedWord{-1, 1916U}, UnusedWord{-1, 1932U}, UnusedWord{-1, 1948U}, UnusedWord{-1, 1964U},
    UnusedWord{-1, 1980U}, UnusedWord{-1, 2428U}, UnusedWord{-1, 2444U}, UnusedWord{-1, 2460U}, UnusedWord{-1, 2476U},
    UnusedWord{-1, 2492U}, UnusedWord{-1, 2508U}, UnusedWord{-1, 2524U}, UnusedWord{-1, 2540U}, UnusedWord{-1, 2988U},
    UnusedWord{-1, 3004U}, UnusedWord{-1, 3020U}, UnusedWord{-1, 3036U}, UnusedWord{-1, 3052U}, UnusedWord{-1, 3068U},
    UnusedWord{-1, 3084U}, UnusedWord{-1, 3100U}, UnusedWord{-1, 3548U}, UnusedWord{-1, 3564U}, UnusedWord{-1, 3580U},
    UnusedWord{-1, 3596U}, UnusedWord{-1, 3612U}, UnusedWord{-1, 3628U}, UnusedWord{-1, 3644U}, UnusedWord{-1, 3660U},
    UnusedWord{-1, 4108U}, UnusedWord{-1, 4124U}, UnusedWord{-1, 4140U}, UnusedWord{-1, 4156U}, UnusedWord{-1, 4172U},
    UnusedWord{-1, 4188U}, UnusedWord{-1, 4204U}, UnusedWord{-1, 4220U},
};

// The faces look from the camera to the vehicle's centre, a few metres
// apart at track coordinates of hundreds of metres, so that difference keeps
// the rounding of the coordinates and every face inherits it: up to 6.1e-4
// relative, and 7e-5 on unit vector components (0.004 degrees), between
// rounding to nearest and toward zero. The captures match exactly when
// rounding toward zero, apart from each face's depth offset (see
// kRenderViewLooseWords).
constexpr double kReflectionTolerance = 1e-3;
constexpr double kReflectionAbsoluteTolerance = 1e-4;
// Per face: the depth offset, and the clip planes' distances (a normal times
// a position hundreds of metres out, so a normal 1e-4 off moves them by up to
// 0.4; measured 0.38).
constexpr std::array kReflectionLooseWords{
    LooseWord{2072U, 0.5F}, LooseWord{1708U, 1.0F}, LooseWord{1724U, 1.0F}, LooseWord{1776U, 1.0F},
    LooseWord{1780U, 1.0F}, LooseWord{1784U, 1.0F}, LooseWord{1788U, 1.0F}, LooseWord{1840U, 1.0F},
    LooseWord{1844U, 1.0F}, LooseWord{1848U, 1.0F}, LooseWord{1852U, 1.0F}, LooseWord{2632U, 0.5F},
    LooseWord{2268U, 1.0F}, LooseWord{2284U, 1.0F}, LooseWord{2336U, 1.0F}, LooseWord{2340U, 1.0F},
    LooseWord{2344U, 1.0F}, LooseWord{2348U, 1.0F}, LooseWord{2400U, 1.0F}, LooseWord{2404U, 1.0F},
    LooseWord{2408U, 1.0F}, LooseWord{2412U, 1.0F}, LooseWord{3192U, 0.5F}, LooseWord{2828U, 1.0F},
    LooseWord{2844U, 1.0F}, LooseWord{2896U, 1.0F}, LooseWord{2900U, 1.0F}, LooseWord{2904U, 1.0F},
    LooseWord{2908U, 1.0F}, LooseWord{2960U, 1.0F}, LooseWord{2964U, 1.0F}, LooseWord{2968U, 1.0F},
    LooseWord{2972U, 1.0F}, LooseWord{3752U, 0.5F}, LooseWord{3388U, 1.0F}, LooseWord{3404U, 1.0F},
    LooseWord{3456U, 1.0F}, LooseWord{3460U, 1.0F}, LooseWord{3464U, 1.0F}, LooseWord{3468U, 1.0F},
    LooseWord{3520U, 1.0F}, LooseWord{3524U, 1.0F}, LooseWord{3528U, 1.0F}, LooseWord{3532U, 1.0F},
    LooseWord{4312U, 0.5F}, LooseWord{3948U, 1.0F}, LooseWord{3964U, 1.0F}, LooseWord{4016U, 1.0F},
    LooseWord{4020U, 1.0F}, LooseWord{4024U, 1.0F}, LooseWord{4028U, 1.0F}, LooseWord{4080U, 1.0F},
    LooseWord{4084U, 1.0F}, LooseWord{4088U, 1.0F}, LooseWord{4092U, 1.0F},
};

// Box contact (original 0x002f0640 and 0x002f2e70, with the supporting-point
// routines at 0x002f2a00 and 0x002f2cd0 and the polygon clipper at
// 0x002bedc0 that they call).
constexpr GuestAddress kBoxEdgeOrder = 0x004c3fe8U;      // three signed bytes
constexpr GuestAddress kBoxWorldAxes = 0x004c3ff0U;      // three vectors
constexpr GuestAddress kMeshBoundsStart = 0x004c4020U;   // two vectors
constexpr std::int32_t kGpAxisEpsilon = -29672;
constexpr std::int32_t kGpFaceLow = -29664;
constexpr std::int32_t kGpFaceHigh = -29660;
constexpr std::int32_t kGpDepthStart = -31324;
constexpr std::int32_t kGpDistanceFloor = -32740;

float load_gp_float(const guest::R5900State& cpu, const guest::EeMemory& memory, const std::int32_t offset) {
    return load_float(memory, cpu.read_gpr_u32(28U) + static_cast<GuestAddress>(offset));
}

engine::Vector4 load_quad4(const guest::EeMemory& memory, const GuestAddress address) {
    return load_vector4(memory, address);
}

// TestBoxPair(first, second, frame, depth_out, point_out, normal_out): the
// boxes at a0 and a1 (a1 in world space, a0 in the frame a2); a non-null
// point_out asks for the contact.
NativeFunctionExecution test_box_pair(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress point_out = call.arg(4U);
    engine::BoxPair pair;
    pair.first_upper = load_quad4(memory, call.arg(0U));
    pair.first_lower = load_quad4(memory, call.arg(0U) + 16U);
    pair.second_upper = load_quad4(memory, call.arg(1U));
    pair.second_lower = load_quad4(memory, call.arg(1U) + 16U);
    for (GuestAddress row = 0U; row < 4U; ++row) {
        pair.frame[row] = load_quad4(memory, call.arg(2U) + row * 16U);
    }
    engine::BoxContactConstants constants;
    constants.axis_epsilon = load_gp_float(call.cpu(), memory, kGpAxisEpsilon);
    constants.face_low = load_gp_float(call.cpu(), memory, kGpFaceLow);
    constants.face_high = load_gp_float(call.cpu(), memory, kGpFaceHigh);
    constants.depth_start = load_gp_float(call.cpu(), memory, kGpDepthStart);
    for (GuestAddress axis = 0U; axis < 3U; ++axis) {
        constants.world_axes[axis] = load_quad4(memory, kBoxWorldAxes + axis * 16U);
        constants.edge_order[axis] = static_cast<std::int8_t>(memory.read_u8(kBoxEdgeOrder + axis));
    }
    // The w lanes the original leaves holding what its stack frame (5776
    // bytes below the caller's stack pointer) held and what the second box's
    // corner register held.
    engine::BoxContactStaleLanes stale;
    constexpr std::array<GuestAddress, 9> kEdgeNormalFrameOffsets{3760U, 3600U, 3440U, 3280U, 3120U,
                                                                  2960U, 2800U, 2640U, 2480U};
    const GuestAddress frame_base = call.stack_pointer() - 5776U;
    for (std::size_t index = 0U; index < kEdgeNormalFrameOffsets.size(); ++index) {
        stale.edge_normal_w[index] = load_float(memory, frame_base + kEdgeNormalFrameOffsets[index] + 12U);
    }
    stale.edge_point_w = pair.second_upper.w;
    const engine::BoxContact result = engine::test_box_pair(pair, point_out != 0U, constants, stale);
    const bool reported = result.hit && (point_out == 0U || result.has_contact);
    if (reported && point_out != 0U) {
        store_float(memory, call.arg(3U), result.depth);
        store_vector4(memory, point_out, result.point);
        store_vector4(memory, call.arg(5U), result.normal);
    }
    call.return_value(reported ? 1U : 0U);
    return call.finish(105U + 115U * result.axes_tested + (result.has_contact ? 1100U : 0U));
}

// TestMeshAgainstBox(box, to_world, to_box, mesh, point_out, normal_out,
// id_out, minimum_out, maximum_out, push_out).
NativeFunctionExecution test_mesh_against_box(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress stack = call.stack_pointer();
    const GuestAddress box = call.arg(0U);
    const GuestAddress to_world_address = call.arg(1U);
    const GuestAddress to_box_address = call.arg(2U);
    const GuestAddress mesh = call.arg(3U);
    const GuestAddress point_out = call.arg(4U);
    const GuestAddress normal_out = call.arg(5U);
    const GuestAddress id_out = call.arg(6U);
    const GuestAddress minimum_out = call.arg(7U);
    const GuestAddress maximum_out = memory.read_u32_le(stack);
    const GuestAddress push_out = memory.read_u32_le(stack + 8U);

    // The sums start from the vector unit register the caller left in VU0 vf1.
    const float leftover_w = std::bit_cast<float>(call.cpu().read_vu0_vector(1U).words[3]);
    store_vector4(memory, push_out, engine::Vector4{0.0F, 0.0F, 0.0F, leftover_w});

    const GuestAddress polygons = memory.read_u32_le(mesh + 4U);
    const GuestAddress ids = memory.read_u32_le(mesh + 8U);
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(mesh));
    std::array<engine::Vector4, 4> to_box;
    std::array<engine::Vector4, 4> to_world;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        to_box[row] = load_quad4(memory, to_box_address + row * 16U);
        to_world[row] = load_quad4(memory, to_world_address + row * 16U);
    }
    engine::MeshBoxConstants constants;
    constants.distance_floor = load_gp_float(call.cpu(), memory, kGpDistanceFloor);
    constants.minimum_start = load_quad4(memory, kMeshBoundsStart);
    constants.maximum_start = load_quad4(memory, kMeshBoundsStart + 16U);
    constants.normal_epsilon = std::bit_cast<float>(0x2f800000U);

    const auto polygon = [&](const std::size_t index) {
        const GuestAddress at = polygons + static_cast<GuestAddress>(index) * 64U;
        engine::MeshPolygon result;
        for (GuestAddress corner = 0U; corner < 3U; ++corner) {
            result.vertices[corner] = load_quad4(memory, at + corner * 16U);
        }
        result.plane = load_quad4(memory, at + 48U);
        result.id = memory.read_u16_le(ids + static_cast<GuestAddress>(index) * 2U);
        return result;
    };
    const engine::MeshBoxResult result = engine::test_mesh_against_box(
        count > 0 ? static_cast<std::size_t>(count) : 0U, polygon, to_box, to_world, load_quad4(memory, box),
        load_quad4(memory, box + 16U), leftover_w, constants);
    if (!result.hit) {
        call.return_value(0U);
        return call.finish(60U + 220U * static_cast<std::uint32_t>(std::max(count, 0)));
    }
    memory.write_u16_le(id_out, result.last_id);
    if (minimum_out != 0U) {
        store_vector4(memory, minimum_out, result.bounds_minimum);
    }
    if (maximum_out != 0U) {
        store_vector4(memory, maximum_out, result.bounds_maximum);
    }
    if (result.has_contact) {
        store_vector4(memory, normal_out, result.normal);
        store_vector4(memory, point_out, result.point);
        store_vector4(memory, push_out, result.push);
    }
    call.return_value(result.has_contact ? 1U : 0U);
    return call.finish(300U + 220U * static_cast<std::uint32_t>(std::max(count, 0)));
}

constexpr std::array kFunctions{
    EngineFunction{0x002e3440U, 5620U, 0x5a7736ea1c89b7f2ULL, "UpdateChaseCamera", 4000U, update_chase_camera,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, chase_camera_cost},
    EngineFunction{0x00220040U, 300U, 0x8b995d6ac13dac78ULL, "StartEffect", 120U, start_effect, EngineReturn::Nothing,
                   kEffectUnusedWords},
    EngineFunction{0x001da860U, 28U, 0x7e0274a2d0e383c7ULL, "RwMatrixUpdate", 10U, rw_matrix_update},
    EngineFunction{0x001e3fd0U, 92U, 0xd35c79a0a975e657ULL, "RwFrameUpdateObjects", 30U, rw_frame_update_objects},
    EngineFunction{0x001e2110U, 100U, 0xb0d3327de1012dc4ULL, "RwCameraSetViewWindow", 80U, rw_camera_set_view_window},
    EngineFunction{0x00130740U, 12U, 0x3d757b104cfb3aa0ULL, "StoreQuadword", 5U, store_quadword},
    EngineFunction{0x0030a990U, 56U, 0x9efc881f239a996fULL, "CameraShakeEnvelope", 20U, camera_shake_envelope,
                   EngineReturn::Float},
    EngineFunction{0x004256c0U, 2416U, 0x897f2c0dc09abbb8ULL, "UpdateRadialBlur", 1000U, update_radial_blur},
    EngineFunction{0x00211ed0U, 1704U, 0x5e4b492a8fd1c6a2ULL, "UpdateCameraFromOrientation", 1500U,
                   update_camera_from_orientation, EngineReturn::Nothing, kCameraProjectionUnusedWords},
    EngineFunction{0x00225440U, 1772U, 0x8cd4deb8c7d01e52ULL, "SetUpRenderView", 2500U, set_up_render_view,
                   EngineReturn::Integer, kRenderViewUnusedWords, 1e-4, 1e-6, kRenderViewLooseWords},
    EngineFunction{0x00225b30U, 1432U, 0x9620ff4ebb185ee4ULL, "SetUpRenderViewFromCamera", 800U,
                   set_up_render_view_from_camera, EngineReturn::Nothing, kRenderViewFromCameraUnusedWords},
    EngineFunction{0x0020aef0U, 760U, 0xfe80c21509183706ULL, "SetUpCameraConstants", 175U, set_up_camera_constants,
                   EngineReturn::Nothing},
    EngineFunction{0x00223130U, 8836U, 0x0a385eed0680a99bULL, "UpdateReflectionViews", 12000U, update_reflection_views,
                   EngineReturn::Nothing, kReflectionUnusedWords, kReflectionTolerance, kReflectionAbsoluteTolerance,
                   kReflectionLooseWords},
    EngineFunction{0x0025f0c0U, 2180U, 0x1353b62b6b01a86aULL, "EmitTrailParticles", 20000U, emit_trail_particles,
                   EngineReturn::Nothing},
    EngineFunction{0x002bcf60U, 960U, 0xcec0d3206e649771ULL, "AppendTyreMarkPoint", 400U, append_tyre_mark_point},
    EngineFunction{0x002bd320U, 576U, 0x33b0350282a2d6aeULL, "AddTyreMark", 600U, add_tyre_mark, EngineReturn::Nothing},
    EngineFunction{0x002603f0U, 2492U, 0x4ccec9ef1ab3ce0eULL, "UpdateWheelSurfaceEffects", 60000U,
                   update_wheel_surface_effects, EngineReturn::Nothing},
    EngineFunction{0x00254a90U, 500U, 0xdf69c48ba7c76e17ULL, "AppendTrailPoint", 120U, append_trail_point},
    EngineFunction{0x00256fd0U, 176U, 0x0cf04d6d33f29877ULL, "AppendFacingParticle", 50U, append_facing_particle,
                   EngineReturn::Nothing},
    EngineFunction{0x00334ea0U, 392U, 0x3ebd8cfcb53f2a33ULL, "BuildCullingFrustumWithInnerSides", 260U,
                   build_culling_frustum_with_inner_sides, EngineReturn::Nothing},
    EngineFunction{0x001d9990U, 544U, 0x7a1addf45fedaceeULL, "RwMatrixOptimize", 150U, rw_matrix_optimize},
    EngineFunction{0x00334750U, 504U, 0xd32b15a187410a37ULL, "BuildCullingFrustum", 200U, build_culling_frustum,
                   EngineReturn::Nothing},
    EngineFunction{0x00334950U, 1348U, 0xd451ab22cb60ca82ULL, "BuildViewClipVolume", 900U, build_view_clip_volume,
                   EngineReturn::Nothing, {}, 1e-4, kFrustumAbsoluteTolerance},
    EngineFunction{0x00254d50U, 448U, 0x4c6761ce3a138f7bULL, "SetDirectionalLight", 200U, set_directional_light,
                   EngineReturn::Nothing},
    EngineFunction{0x00349980U, 1128U, 0x37745a2a1bd2225fULL, "BuildPackedViewFrustum", 400U, build_packed_view_frustum,
                   EngineReturn::Nothing, {}, 1e-4, kFrustumAbsoluteTolerance},
    EngineFunction{0x001e0b48U, 2244U, 0xd63dc8e489f4a946ULL, "BuildPerspectiveFrustum", 900U, build_perspective_frustum,
                   EngineReturn::Nothing},
    EngineFunction{0x0026baa0U, 448U, 0x5bea5ed6bce28130ULL, "SelectRenderTarget", 200U, select_render_target,
                   EngineReturn::Nothing},
    EngineFunction{0x0024d110U, 592U, 0x09c54075b328e293ULL, "ProjectSun", 200U, project_sun, EngineReturn::Nothing,
                   {}, 1e-4, kSunProjectionAbsoluteTolerance},
    EngineFunction{0x002187b0U, 1356U, 0x6c197529d2999fcbULL, "AppendSpriteStrip", 12000U, append_sprite_strip,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, sprite_strip_cost},
    EngineFunction{0x002f0640U, 10288U, 0x2a18626732110246ULL, "TestBoxPair", 3200U, test_box_pair,
                   EngineReturn::Integer},
    EngineFunction{0x002f2e70U, 1616U, 0x73b94a6ab9ec5163ULL, "TestMeshAgainstBox", 12000U, test_mesh_against_box,
                   EngineReturn::Integer},
};

} // namespace

std::span<const EngineFunction> render_engine_functions() { return kFunctions; }

void set_slus_21050_chase_camera_real_time_easing(const bool enabled) noexcept {
    chase_camera_real_time_easing.store(enabled, std::memory_order_relaxed);
}

} // namespace takedown::runtime
