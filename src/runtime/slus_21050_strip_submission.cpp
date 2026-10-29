#include "takedown/runtime/slus_21050_strip_submission.hpp"
#include "takedown/guest/vu1_simd_arithmetic.hpp"
#include "takedown/runtime/vu1_geometry_capture.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace takedown::runtime::slus_21050 {
namespace {
using ProgramImage = std::array<std::byte, kVu1WorkerMemorySize>;
struct StripProgram {
    const char* fixture;
    guest::Vu1ProgramHash hash;
    std::uint16_t continuation;
    std::uint16_t view_selector;
    std::size_t owned_bytes;
    bool supplied_alpha;
    bool cull_back_faces;
    bool direct_entry;
};
constexpr std::array<StripProgram, 4> programs{{
    {"TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE", {0x1063595b1645a43fULL, 0x9428d0bef321cc6cULL}, 802U, 364U, kVu1WorkerMemorySize, false, true, false},
    {"TAKEDOWN_NATIVE_STRIP_RGBA_CASE", {0xc05334e06f50cc66ULL, 0x20aaaef238613b1dULL}, 681U, 243U, 683U * 8U, true, true, true},
    {"TAKEDOWN_NATIVE_STRIP_TWO_SIDED_CASE", {0xf5aab27a48d0c755ULL, 0xac626ba32f744ae7ULL}, 611U, 178U, 613U * 8U, true, false, true},
    {"TAKEDOWN_NATIVE_STRIP_TWO_SIDED_SPECULAR_CASE", {0x0dc53ffb6f968c44ULL, 0x9ba28d69290baf33ULL}, 732U, 299U, 734U * 8U, false, false, false}}};
const std::array<std::optional<ProgramImage>, programs.size()>& reference_images() {
    static const auto images = [] {
        std::array<std::optional<ProgramImage>, programs.size()> result;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto* path = std::getenv(programs[i].fixture);
            if (!path || !*path) { continue; }
            std::ifstream source(path, std::ios::binary);
            const auto data = read_vu1_geometry_case(source);
            guest::Vu1ProgramCache validation;
            if (!validation.register_compiled_program({
                .hash = programs[i].hash, .entry = programs[i].continuation, .resolved_entry = 4U,
                .micro_memory = data.input.micro_memory})) {
                throw std::runtime_error("native strip submission fixture has an unsupported program image");
            }
            result[i] = data.input.micro_memory;
        }
        return result;
    }();
    return images;
}
// Preparation and reporting are owned by the single EE execution thread.
std::array<std::uint64_t, 5> prepared_jobs{};
struct StripInputs {
    StripTransform transform;
    StripProjection projection;
    std::optional<StripSpecularLight> light;
    std::vector<StripMeshVertex> vertices;
    bool textured{};
    bool reload_view{};
    bool supplied_alpha{};
    std::size_t family{};
};

std::optional<StripInputs> decode_strip_inputs(
    const std::span<const std::byte> micro, const std::span<const std::byte> data,
    const guest::Vu1State& state, const std::uint16_t entry,
    const std::uint32_t top, const std::uint32_t count, const bool padded,
    const NativeGeometryAdmission admission) {
    if (data.size() != kVu1WorkerMemorySize || micro.size() != kVu1WorkerMemorySize) { return std::nullopt; }
    const auto& images = reference_images();
    std::size_t family = programs.size();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry != programs[i].continuation && !(programs[i].direct_entry && entry == 4U)) { continue; }
        // Owned program ranges include continuation and delay slot. Unreachable
        // bytes after them are stale uploads, not inputs to these operations.
        if (images[i]) {
            const std::span<const std::byte> reference{images[i]->data(), programs[i].owned_bytes};
            const bool same = admission.cache != nullptr
                ? admission.cache->matches(i, micro, reference, admission.generation)
                : std::memcmp(micro.data(), reference.data(), programs[i].owned_bytes) == 0;
            if (same) {
                family = i; break;
            }
        }
    }
    if (family == programs.size()) { return std::nullopt; }
    const bool supplied_alpha = programs[family].supplied_alpha;
    const auto continuation = programs[family].continuation;
    if (!((entry == continuation && state.read_integer(15U) == 4U) || (programs[family].direct_entry && entry == 4U)) ||
        (top != 0U && top != (supplied_alpha ? 240U : 272U)) || count < 3U ||
        count > (supplied_alpha ? 80U : 68U)) { return std::nullopt; }
    const auto word = [&](std::size_t q, unsigned lane) {
        std::uint32_t result{};
        for (unsigned i = 0; i < 4U; ++i) {
            result |= std::to_integer<std::uint32_t>(data[q * 16U + lane * 4U + i]) << (i * 8U);
        }
        return result;
    };
    const auto vector = [&](std::size_t q) {
        std::array<float, 4> result{};
        for (unsigned i = 0; i < 4U; ++i) { result[i] = std::bit_cast<float>(word(q, i)); }
        return result;
    };
    const auto material = word(1006U, 1U);
    // The recovered two-sided specular operation has no optional prefix pass.
    // Its six captured jobs all take the zero prefix selector at q1005.x.
    if (family == 3U && word(1005U, 0U) != 0U) { return std::nullopt; }
    if (word(1013U, 0U) != 10U || (!supplied_alpha && word(1013U, 2U) > 1U) || word(1005U, 3U) != 0U ||
        (material & ~0x00080000U) != 0x30264000U || word(1006U, 2U) != 0x412U) { return std::nullopt; }
    const guest::vu1_detail::NativeFloatArithmeticScope float_scope{false};
    const auto viewport = vector(1018U), offset = vector(1019U);
    const auto horizontal = vector(1020U), vertical = vector(1021U);
    auto transform = prepare_strip_transform(
        {vector(1014U), vector(1015U), vector(1016U), vector(1017U)},
        {viewport[0], viewport[1]}, {horizontal[1], vertical[1], vertical[2]}, horizontal[2]);
    transform.cull_back_faces = programs[family].cull_back_faces;
    // When the game reuses its cached view, do not substitute uploaded constants
    // which have not yet become that view. Z is temporary projection workspace.
    if (word(1013U, 3U) == 0U) {
        if (word(1013U, 1U) != programs[family].view_selector) { return std::nullopt; }
        for (unsigned column = 0; column < 4U; ++column) {
            const auto& prepared = column == 3U ? transform.translation : transform.columns[column];
            for (const unsigned lane : {0U, 1U, 3U}) {
                if (std::bit_cast<std::uint32_t>(prepared[lane]) != state.read_vector(28U + column).words[lane]) {
                    return std::nullopt;
                }
            }
        }
    }
    StripProjection projection{vertical[3], horizontal[3], {horizontal[1], vertical[1]},
        {offset[0], offset[1], offset[2]}, viewport[2], viewport[3], offset[3]};
    if (!(projection.near_distance > 0 && projection.far_distance >= projection.near_distance &&
        projection.clip_scale[0] > 0 && projection.clip_scale[1] > 0)) { return std::nullopt; }
    std::optional<StripSpecularLight> light;
    if (!supplied_alpha && word(1013U, 2U) == 0U) {
        const auto direction = vector(1007U), camera = vector(1008U);
        light = StripSpecularLight{{camera[0], camera[1], camera[2]},
            {direction[0], direction[1], direction[2]}, direction[3], camera[3], vector(1009U)[0]};
    }
    const auto padding_mask = supplied_alpha ? 1U : 3U;
    std::vector<StripMeshVertex> vertices(padded ? ((count + padding_mask) & ~padding_mask) : count);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        auto& vertex = vertices[i]; const auto q = top + i * 3U;
        for (unsigned lane = 0; lane < 3U; ++lane) {
            vertex.geometry.position[lane] = std::bit_cast<float>(word(q, lane));
            vertex.colour[lane] = static_cast<float>(std::bit_cast<std::int32_t>(word(q + 2U, lane)));
            if (light) { vertex.normal[lane] = std::bit_cast<std::int32_t>(word(
                top + 204U + (i / 4U) * 4U + lane, static_cast<unsigned>(i % 4U))); }
        }
        vertex.geometry.flags = static_cast<std::uint16_t>(word(q, 3U));
        if (supplied_alpha) { vertex.colour[3] = static_cast<float>(std::bit_cast<std::int32_t>(word(q + 2U, 3U))); }
        for (unsigned lane = 0; lane < 2U; ++lane) {
            vertex.texture[lane] = static_cast<float>(std::bit_cast<std::int32_t>(word(q + 1U, lane))) / 4096.0F;
        }
        if (light) { vertex.intensity = std::bit_cast<std::int32_t>(word(
            top + 204U + (i / 4U) * 4U + 3U, static_cast<unsigned>(i % 4U))); }
    }
    return StripInputs{transform, projection, light, std::move(vertices),
        (material & 0x00080000U) != 0U, word(1013U, 3U) != 0U, supplied_alpha, family};
}
}

std::optional<std::vector<StripSubmissionPacket>> prepare_native_strip_submission(
    const std::span<const std::byte> micro, const std::span<const std::byte> data,
    const guest::Vu1State& state, const std::uint16_t entry,
    const std::uint32_t top, const std::uint32_t count, const NativeGeometryAdmission admission) {
    const auto input = decode_strip_inputs(micro, data, state, entry, top, count, false, admission);
    if (!input) { return std::nullopt; }
    auto packets = pack_strip_submission(draw_strip_mesh(input->transform, input->projection,
        input->light, input->vertices), input->textured);
    ++prepared_jobs[input->family];
    return packets;
}

std::array<std::uint64_t, 5> native_strip_prepared_jobs() { return prepared_jobs; }

namespace {
NativeStripCompletion prepare_completion(const StripInputs& input,
    const std::uint32_t top, const std::uint32_t count) {
    NativeStripCompletion result;
    result.transform = input.transform;
    result.projection = input.projection;
    result.top = top;
    result.view_selector = programs[input.family].view_selector;
    result.continuation = programs[input.family].continuation;
    result.reload_view = input.reload_view;
    result.supplied_alpha = input.supplied_alpha;
    const auto transform_count = (count + 1U) & ~1U;
    // Admission bounds all four families to at most 80 padded vertices.
    std::array<StripPosition, 80> position_storage;
    const auto positions = std::span{position_storage}.first(transform_count);
    result.transformed.resize(transform_count);
    for (std::size_t i = 0; i < positions.size(); ++i) { positions[i] = input.vertices[i].geometry; }
    static_cast<void>(transform_strip_geometry(input.transform, positions, result.transformed));
    // Padding is a compatibility output, not an additional visible vertex.
    for (std::size_t i = 0; i < count; ++i) {
        result.clip.common_planes &= result.transformed[i].clip_planes;
        result.clip.any_planes |= result.transformed[i].clip_planes;
    }
    if (!input.supplied_alpha) { result.alpha.resize(input.vertices.size()); }
    if (input.light) {
        std::array<StripSpecularVertex, 80> lighting_storage;
        const auto lighting = std::span{lighting_storage}.first(result.alpha.size());
        for (std::size_t i = 0; i < lighting.size(); ++i) {
            const auto& v = input.vertices[i];
            lighting[i] = {v.geometry.position, v.normal, v.intensity};
        }
        light_strip_specular(*input.light, lighting, result.alpha);
    }
    return result;
}
} // namespace

std::optional<PreparedNativeStripJob> prepare_native_strip_job(
    const std::span<const std::byte> micro, const std::span<const std::byte> data,
    const guest::Vu1State& state, const std::uint16_t entry,
    const std::uint32_t top, const std::uint32_t count, const NativeGeometryAdmission admission) {
    const auto input = decode_strip_inputs(micro, data, state, entry, top, count, true, admission);
    if (!input) { return std::nullopt; }
    PreparedNativeStripJob job;
    const guest::vu1_detail::NativeFloatArithmeticScope float_scope{false};
    job.completion = prepare_completion(*input, top, count);
    const auto transformed = std::span{job.completion.transformed}.first(count);
    if (!strip_mesh_outside_projection(input->projection, transformed, job.completion.clip)) {
        std::vector<StripSurfaceVertex> surfaces(count);
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            surfaces[i] = {transformed[i], input->vertices[i].texture, input->vertices[i].colour};
            if (input->light) { surfaces[i].colour[3] = static_cast<float>(job.completion.alpha[i]); }
        }
        job.packets = pack_strip_submission(project_strip_geometry(input->projection, surfaces), input->textured);
    }
    ++prepared_jobs[input->family];
    return job;
}

void commit_native_strip_completion(const NativeStripCompletion& input,
    const std::span<std::byte> data, guest::Vu1State& state) {
    if (data.size() != kVu1WorkerMemorySize) {
        throw std::invalid_argument("native strip completion memory has the wrong size");
    }
    const auto top = input.top;
    const auto& transformed = input.transformed;
    const auto& alpha = input.alpha;
    // Allocate and calculate everything before committing shared state.
    const auto write = [&](std::size_t q, unsigned lane, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4U; ++byte) {
            data[q * 16U + lane * 4U + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 255U);
        }
    };
    for (std::size_t i = 0; i < transformed.size(); ++i) {
        for (unsigned lane = 0; lane < 4U; ++lane) {
            write(top + i * 3U, lane, std::bit_cast<std::uint32_t>(transformed[i].position[lane]));
        }
        write(top + i * 3U + 1U, 3U, transformed[i].flags);
    }
    if (!input.supplied_alpha) {
        for (std::size_t i = 0; i < alpha.size(); ++i) { write(top + i * 3U + 2U, 3U, alpha[i]); }
    }
    if (input.reload_view) {
        for (unsigned column = 0; column < 4U; ++column) {
            const auto& source = column == 3U ? input.transform.translation : input.transform.columns[column];
            for (const unsigned lane : {0U, 1U, 3U}) {
                state.vectors[28U + column].words[lane] = std::bit_cast<std::uint32_t>(source[lane]);
            }
        }
        for (unsigned lane = 0; lane < 3U; ++lane) {
            state.vectors[2U].words[lane] = std::bit_cast<std::uint32_t>(input.projection.screen_offset[lane]);
        }
        state.vectors[2U].words[3U] = 0U;
        state.vectors[31U].words[2U] = std::bit_cast<std::uint32_t>(input.projection.depth_numerator);
        write(1013U, 3U, 0U);
        write(1013U, 1U, input.view_selector);
    }
    for (unsigned column = 0; column < 3U; ++column) { state.vectors[28U + column].words[2U] = 0U; }
    state.write_integer(15U, 4U);
    // The launch owner resolves MSCNT from TPC before it calls either path.
    // This is an external continuation contract, not disposable job scratch.
    state.write_control(26U, input.continuation);
}

bool complete_native_strip_inputs(const std::span<const std::byte> micro,
    const std::span<std::byte> data, guest::Vu1State& state, const std::uint16_t entry,
    const std::uint32_t top, const std::uint32_t count) {
    const auto input = decode_strip_inputs(micro, data, state, entry, top, count, true, {});
    if (!input) { return false; }
    commit_native_strip_completion(prepare_completion(*input, top, count), data, state);
    return true;
}
bool NativeGeometryProgramCache::matches(const std::size_t family, const std::span<const std::byte> micro,
    const std::span<const std::byte> reference, const std::optional<std::uint64_t> generation) {
    if (family > 4U || reference.empty() || micro.size() < reference.size()) { return false; }
    Entry& entry = entries_[family];
    if (generation.has_value() && entry.generation.has_value() && *std::as_const(entry.generation) == *generation &&
        entry.memory == micro.data() && entry.reference == reference.data() && entry.size == reference.size()) {
        return entry.matched;
    }
    const bool matched = std::memcmp(micro.data(), reference.data(), reference.size()) == 0;
    entry = Entry{micro.data(), reference.data(), reference.size(), generation, matched};
    return matched;
}

std::optional<PreparedNativeEnvironmentJob> prepare_native_environment_job(
    const std::span<const std::byte> micro, const std::span<const std::byte> data,
    const guest::Vu1State& state, const std::uint16_t entry, const std::uint32_t top, const std::uint32_t count,
    const NativeGeometryAdmission admission) {
    // Family 4 is selected by the integer registers the environment program leaves behind.
    const bool family = (top == 156U && state.read_integer(13U) == 316U && state.read_integer(14U) == 641U) ||
        (top == 0U && state.read_integer(13U) == 641U && state.read_integer(14U) == 316U);
    if (micro.size() != kVu1WorkerMemorySize || data.size() != kVu1WorkerMemorySize || entry != 668U ||
        state.read_integer(15U) != 4U || !family || count <= 2U || count > 52U) {
        return std::nullopt;
    }
    // Environment material fixture: the admitted program image for family 4.
    static const std::optional<ProgramImage> image = [] {
        std::optional<ProgramImage> result;
        const auto* path = std::getenv("TAKEDOWN_NATIVE_ENVIRONMENT_CASE");
        if (!path || !*path) { return result; }
        std::ifstream source(path, std::ios::binary);
        const auto fixture = read_vu1_geometry_case(source);
        guest::Vu1ProgramCache validation;
        if (!validation.register_compiled_program({
            .hash = {0x4dc0598d1c12bc3bULL, 0x1bbf166050f071a4ULL}, .entry = 668U, .resolved_entry = 4U,
            .micro_memory = fixture.input.micro_memory})) {
            throw std::runtime_error("native environment fixture has an unsupported program image");
        }
        result = fixture.input.micro_memory;
        return result;
    }();
    if (!image) { return std::nullopt; }
    const std::span<const std::byte> reference{image->data(), 16384U};
    const bool same = admission.cache != nullptr
        ? admission.cache->matches(4U, micro, reference, admission.generation)
        : std::memcmp(micro.data(), reference.data(), reference.size()) == 0;
    if (!same) { return std::nullopt; }

    // Quadword reads (four little-endian words) and float vectors over the input block.
    const auto word = [&data](const std::size_t q) {
        std::array<std::uint32_t, 4> result{};
        for (unsigned lane = 0; lane < 4U; ++lane) {
            for (unsigned byte = 0; byte < 4U; ++byte) {
                result[lane] |= std::to_integer<std::uint32_t>(data[q * 16U + lane * 4U + byte]) << (byte * 8U);
            }
        }
        return result;
    };
    const auto vector = [&word](const std::size_t q) {
        const auto words = word(q);
        std::array<float, 4> result{};
        for (unsigned lane = 0; lane < 4U; ++lane) { result[lane] = std::bit_cast<float>(words[lane]); }
        return result;
    };

    // Validation words: quadword 1013 (status), 965 (reload), 1006 (packet header).
    const auto status = word(1013U);
    if (status[0] != 2U || (status[1] != 0U && status[1] != 2U) || status[2] != 0U || status[3] != 0U) {
        return std::nullopt;
    }
    if (word(965U)[3] > 1U) { return std::nullopt; }
    const auto header = word(1006U);
    if ((header[0] != 0U && header[0] != 3U) || header[1] != 268435456U || header[2] != 14U || header[3] != 0U) {
        return std::nullopt;
    }
    const bool reflection = status[1] == 0U;
    const bool reload_view = word(965U)[3] == 0U;
    const guest::vu1_detail::NativeFloatArithmeticScope float_scope{false};
    if (header[0] != 0U) {
        constexpr std::array<std::uint32_t, 3> registers{6U, 66U, 67U};
        for (std::size_t i = 0; i < registers.size(); ++i) {
            const auto row = word(1007U + i);
            if (row[2] != registers[i] || row[3] != 0U) { return std::nullopt; }
        }
    }

    PreparedNativeEnvironmentJob job;
    job.completion.previous_buffer = state.read_integer(13U);
    job.completion.output_buffer = state.read_integer(14U);
    job.completion.padded_count = static_cast<std::uint16_t>((count + 3U) & ~3U);
    job.completion.reload_view = reload_view;
    job.completion.reflection = reflection;
    const std::size_t padded = job.completion.padded_count;
    const auto offset = vector(1019U);
    job.completion.screen_offset = {offset[0], offset[1], offset[2]};

    EnvironmentProjection projection;
    if (reload_view) {
        std::array<std::array<float, 3>, 4> world{};
        for (std::size_t column = 0; column < 4U; ++column) {
            const auto values = vector(962U + column);
            world[column] = {values[0], values[1], values[2]};
        }
        std::array<std::array<float, 4>, 4> view{};
        for (std::size_t column = 0; column < 4U; ++column) { view[column] = vector(1014U + column); }
        const auto viewport = vector(1018U);
        projection.columns = prepare_environment_view(world, view, {viewport[0], viewport[1], viewport[2]});
    } else {
        if (state.read_integer(12U) != 2U) { return std::nullopt; }
        for (std::size_t lane = 0; lane < 3U; ++lane) {
            if (state.vectors[2].words[lane] != word(1019U)[lane]) { return std::nullopt; }
        }
        for (std::size_t column = 0; column < 4U; ++column) {
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                projection.columns[column][lane] = std::bit_cast<float>(state.vectors[28U + column].words[lane]);
            }
        }
    }
    job.completion.view = projection.columns;

    const auto horizontal = vector(1022U);
    const auto vertical = vector(1023U);
    projection.clip_scale = {horizontal[1], vertical[1]};
    projection.clip_restore = {horizontal[0], vertical[0]};
    projection.clip_depth_scale = vertical[2];
    projection.clip_depth_bias = horizontal[2];
    projection.screen_offset = job.completion.screen_offset;
    projection.reflection_pass = job.completion.reflection;

    EnvironmentMeshLight light;
    for (std::size_t column = 0; column < 4U; ++column) { light.quadratic[column] = vector(966U + column); }
    const auto direction = vector(970U);
    light.view_direction = {direction[0], direction[1], direction[2]};
    light.low_colour = vector(1010U);
    light.high_colour = vector(1011U);

    std::optional<EnvironmentReflectionView> reflection_view;
    if (reflection) {
        EnvironmentReflectionView view_value{};
        for (std::size_t column = 0; column < 3U; ++column) {
            const auto values = vector(971U + column);
            view_value.columns[column] = {values[0], values[1], values[2]};
            view_value.camera[column] = values[3];
        }
        reflection_view = view_value;
    }
    const std::array<std::uint32_t, 4> reflection_colour = word(1012U);

    std::array<EnvironmentMeshVertex, 52> vertex_storage{};
    for (std::size_t i = 0; i < padded; ++i) {
        auto& vertex = vertex_storage[i];
        const std::size_t q = top + i * 3U;
        const auto geometry = word(q);
        const auto normal = word(q + 2U);
        for (unsigned lane = 0; lane < 3U; ++lane) {
            vertex.geometry.position[lane] = std::bit_cast<float>(geometry[lane]);
            vertex.normal[lane] = std::bit_cast<float>(normal[lane]) - direction[3];
        }
        vertex.geometry.flags = static_cast<std::uint16_t>(geometry[3]);
        const std::size_t texture_q = top + (i & 1U) * 6U + (i >> 2U) * 12U + 1U;
        const auto texture = word(texture_q);
        for (unsigned lane = 0; lane < 2U; ++lane) {
            vertex.texture[lane] =
                static_cast<float>(static_cast<std::int32_t>(texture[(i & 2U) + lane])) * 2.44140625e-4F;
        }
    }
    PreparedEnvironmentMesh mesh = prepare_environment_mesh(projection, light, reflection_view, reflection_colour,
        std::span<const EnvironmentMeshVertex>{vertex_storage.data(), count});

    auto& writes = job.completion.writes;
    writes.reserve(mesh.packets.front().quadwords.size() + (padded * 7U) / 4U + 7U);

    std::array<std::array<float, 2>, 52> reflected{};
    if (reflection_view) {
        std::copy(mesh.reflection_texture.begin(), mesh.reflection_texture.end(), reflected.begin());
        std::array<EnvironmentReflectionVertex, 52> padding_storage{};
        for (std::size_t i = count; i < padded; ++i) {
            const auto& vertex = vertex_storage[i];
            padding_storage[i - count] = {vertex.geometry.position, vertex.normal};
        }
        project_environment_reflections(*reflection_view,
            std::span<const EnvironmentReflectionVertex>{padding_storage.data(), padded - count},
            std::span<std::array<float, 2>>{reflected.data() + count, padded - count});
    }

    for (std::size_t group = 0; group < padded; group += 4U) {
        for (std::size_t axis = 0; axis < 3U; ++axis) {
            std::array<std::uint32_t, 4> values{};
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                values[lane] = std::bit_cast<std::uint32_t>(vertex_storage[group + lane].normal[axis]);
            }
            writes.push_back(EnvironmentMemoryWrite{
                .quadword = static_cast<std::uint16_t>(top + group * 3U + axis * 3U + 2U),
                .lanes = 15U,
                .words = values});
        }
    }
    for (std::size_t i = 0; i < padded; ++i) {
        const auto& vertex = vertex_storage[i];
        writes.push_back(EnvironmentMemoryWrite{
            .quadword = static_cast<std::uint16_t>(top + i * 3U + 1U),
            .lanes = static_cast<std::uint8_t>(reflection ? 15U : 3U),
            .words = {std::bit_cast<std::uint32_t>(vertex.texture[0]), std::bit_cast<std::uint32_t>(vertex.texture[1]),
                std::bit_cast<std::uint32_t>(reflected[i][0]), std::bit_cast<std::uint32_t>(reflected[i][1])}});
    }
    for (std::size_t j = 0; j < mesh.packets.front().quadwords.size(); ++j) {
        writes.push_back(EnvironmentMemoryWrite{
            .quadword = static_cast<std::uint16_t>(job.completion.output_buffer + j),
            .lanes = 15U,
            .words = mesh.packets.front().quadwords[j]});
    }
    writes.push_back(EnvironmentMemoryWrite{
        .quadword = job.completion.previous_buffer,
        .lanes = 14U,
        .words = {0U, 808337408U, 1042U, 1065353216U}});
    if (header[0] != 0U) {
        auto& quadwords = mesh.packets.front().quadwords;
        quadwords.insert(quadwords.begin(), 4U, std::array<std::uint32_t, 4>{});
        for (std::size_t i = 0; i < 4U; ++i) {
            const auto values = word(1006U + i);
            quadwords[i] = values;
            writes.push_back(EnvironmentMemoryWrite{
                .quadword = static_cast<std::uint16_t>(job.completion.output_buffer + i - 4U),
                .lanes = 15U,
                .words = values});
        }
        writes.push_back(EnvironmentMemoryWrite{.quadword = 1006U, .lanes = 1U, .words = {}});
    }
    if (reload_view) {
        writes.push_back(EnvironmentMemoryWrite{.quadword = 965U, .lanes = 8U, .words = {0U, 0U, 0U, 1U}});
    }

    job.packets = std::move(mesh.packets);
    ++prepared_jobs[4];
    return job;
}

void commit_native_environment_completion(const NativeEnvironmentCompletion& input,
    const std::span<std::byte> data, guest::Vu1State& state) {
    if (data.size() != 16384U) {
        throw std::invalid_argument("native environment completion memory is out of bounds");
    }
    for (const EnvironmentMemoryWrite& write : input.writes) {
        if (write.quadword > 1023U || write.lanes > 15U) {
            throw std::invalid_argument("native environment completion memory is out of bounds");
        }
    }
    for (const EnvironmentMemoryWrite& write : input.writes) {
        for (unsigned lane = 0; lane < 4U; ++lane) {
            if (((write.lanes >> lane) & 1U) == 0U) { continue; }
            for (unsigned byte = 0; byte < 4U; ++byte) {
                const auto offset = (static_cast<std::size_t>(write.quadword) * 4U + lane) * 4U + byte;
                data[offset] = static_cast<std::byte>((write.words[lane] >> (byte * 8U)) & 255U);
            }
        }
    }
    if (input.reload_view) {
        for (unsigned column = 0; column < 4U; ++column) {
            for (unsigned lane = 0; lane < 4U; ++lane) {
                state.vectors[28U + column].words[lane] = std::bit_cast<std::uint32_t>(input.view[column][lane]);
            }
        }
        for (unsigned lane = 0; lane < 3U; ++lane) {
            state.vectors[2U].words[lane] = std::bit_cast<std::uint32_t>(input.screen_offset[lane]);
        }
        state.vectors[2U].words[3U] = 0U;
    }
    if (input.reflection) { state.vectors[2U].words[3U] = 1056964608U; }
    state.write_control(11U, static_cast<std::uint16_t>(input.padded_count * 2U));
    state.write_control(12U, 2U);
    state.write_control(13U, input.output_buffer);
    state.write_control(14U, input.previous_buffer);
    state.write_control(15U, 4U);
    state.write_control(26U, 668U);
}

} // namespace takedown::runtime::slus_21050
