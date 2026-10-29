// Adapters for world visibility: sphere and segment culling against the
// camera's frustum and its regions.

#include "takedown/engine/world/convex_volume.hpp"
#include "takedown/engine/world/frustum_culling.hpp"
#include "takedown/engine/world/sphere_culling.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <array>
#include <span>
#include <string_view>

namespace takedown::runtime {
namespace {

using engine::Visibility;
using guest::GuestAddress;

engine::TransposedPlanes load_transposed_planes(const guest::EeMemory& memory, const GuestAddress address) {
    return {load_vector4(memory, address), load_vector4(memory, address + 16U), load_vector4(memory, address + 32U),
            load_vector4(memory, address + 48U)};
}

NativeFunctionExecution build_region_volume(NativeFunctionContext& context);
NativeFunctionExecution classify_sphere_in_culling_frustum(NativeFunctionContext& context);
NativeFunctionExecution classify_segment_against_sphere(NativeFunctionContext& context);
NativeFunctionExecution classify_sphere_in_view(NativeFunctionContext& context);

constexpr std::array<EngineFunction, 4U> kFunctions{{
    {
        .address = 0x3D7F70U,
        .code_bytes = 256U,
        .code_fnv1a64 = 5967380052372145651ULL,
        .name = "BuildRegionVolume",
        .cost = 350U,
        .implementation = build_region_volume,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 0x2202D0U,
        .code_bytes = 372U,
        .code_fnv1a64 = 8660817538243510394ULL,
        .name = "ClassifySphereInCullingFrustum",
        .cost = 100U,
        .implementation = classify_sphere_in_culling_frustum,
        .returns = EngineReturn::Integer,
    },
    {
        .address = 0x2BF5F0U,
        .code_bytes = 436U,
        .code_fnv1a64 = 15189938016474793049ULL,
        .name = "ClassifySegmentAgainstSphere",
        .cost = 120U,
        .implementation = classify_segment_against_sphere,
        .returns = EngineReturn::Integer,
    },
    {
        .address = 0x2BF2E0U,
        .code_bytes = 168U,
        .code_fnv1a64 = 205499809837612982ULL,
        .name = "ClassifySphereInView",
        .cost = 120U,
        .implementation = classify_sphere_in_view,
        .returns = EngineReturn::Integer,
    },
}};


NativeFunctionExecution classify_segment_against_sphere(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::Vector4 sphere = load_vector4(memory, call.arg(1) & 0xFFFFFFF0U);
    const engine::Vector4 end = load_vector4(memory, (call.arg(0) & 0xFFFFFFF0U) + 16U);
    const engine::Vector4 start = load_vector4(memory, call.arg(0) & 0xFFFFFFF0U);
    const Visibility overlap = engine::segment_sphere_overlap(
        engine::Vector3{start.x, start.y, start.z}, engine::Vector3{end.x, end.y, end.z},
        engine::Vector3{sphere.x, sphere.y, sphere.z}, sphere.w);
    const std::uint64_t extended = static_cast<std::uint64_t>(overlap);
    call.return_value64(extended);
    return call.finish(70U);
}

NativeFunctionExecution build_region_volume(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    engine::ConvexVolume volume{};
    for (GuestAddress corner = 0U; corner != 8U; ++corner) {
        const GuestAddress source = call.arg(0) + corner * 16U;
        const GuestAddress destination = call.arg(1) + corner * 16U;
        copy_quadword(memory, destination, source);
        engine::Vector4 point{};
        point.x = load_float(memory, source);
        point.y = load_float(memory, source + 4U);
        point.z = load_float(memory, source + 8U);
        point.w = load_float(memory, destination + 12U);
        volume.corners[corner] = point;
    }
    std::array<engine::FaceCorners, 6> faces{};
    for (GuestAddress face = 0U; face != 6U; ++face) {
        const GuestAddress entry = (face + 1255236U) * 4U;
        faces[face] = engine::FaceCorners{
            static_cast<std::int8_t>(memory.read_u8(entry)),
            static_cast<std::int8_t>(memory.read_u8(entry + 1U)),
            static_cast<std::int8_t>(memory.read_u8(entry + 2U)),
        };
    }
    engine::build_face_planes(volume, faces);
    for (GuestAddress face = 0U; face != 6U; ++face) {
        store_vector4(memory, call.arg(1) + face * 16U + 128U, volume.planes[face]);
    }
    return call.finish(350U);
}

NativeFunctionExecution classify_sphere_in_culling_frustum(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::Vector4 sphere = load_vector4(memory, call.arg(0) & 0xFFFFFFF0U);
    engine::CullingFrustum frustum{};
    frustum.depth_planes[0] = load_vector4(memory, 0x67D5A0U);
    frustum.depth_planes[1] = load_vector4(memory, 0x67D5B0U);
    frustum.sides = load_transposed_planes(memory, 0x67D5C0U);
    frustum.inner_sides = load_transposed_planes(memory, 0x67D600U);
    const Visibility visibility =
        engine::classify_sphere(engine::Vector3{sphere.x, sphere.y, sphere.z}, sphere.w, frustum);
    const std::uint64_t extended = static_cast<std::uint64_t>(visibility);
    call.return_value64(extended);
    return call.finish(visibility == Visibility::Outside ? 40U : 110U);
}

NativeFunctionExecution classify_sphere_in_view(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::Vector4 sphere = load_vector4(memory, call.arg(0) & 0xFFFFFFF0U);
    std::array<engine::Vector4, 6> planes;
    for (GuestAddress index = 0U; index != 6U; ++index) {
        planes[index] = load_vector4(memory, call.arg(1) + index * 16U + 128U);
    }
    const Visibility result = engine::classify_sphere(engine::Vector3{sphere.x, sphere.y, sphere.z}, sphere.w,
                                                      std::span<const engine::Vector4>(planes));
    const std::uint64_t extended = static_cast<std::uint64_t>(result);
    call.return_value64(extended);
    return call.finish(80U);
}

} // namespace

[[nodiscard]] std::span<const EngineFunction> world_visibility_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
