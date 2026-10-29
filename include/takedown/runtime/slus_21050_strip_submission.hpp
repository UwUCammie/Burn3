#pragma once

#include "takedown/runtime/slus_21050_strip_geometry.hpp"
#include "takedown/guest/vu1_micro_interpreter.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace takedown::runtime::slus_21050 {
// Owned by one EeMemory. Reference images are immutable local fixture images;
// failed matches are cached too. Escaped mutable MicroMem views force a fresh
// comparison on every attempt. Dynamic vertex/material admission is never cached.
class NativeGeometryProgramCache {
public:
    [[nodiscard]] bool matches(std::size_t family, std::span<const std::byte> micro,
        std::span<const std::byte> reference, std::optional<std::uint64_t> generation);
private:
    struct Entry {
        const std::byte* memory{};
        const std::byte* reference{};
        std::size_t size{};
        std::optional<std::uint64_t> generation;
        bool matched{};
    };
    std::array<Entry, 5> entries_{};
};
struct NativeGeometryAdmission {
    NativeGeometryProgramCache* cache{};
    std::optional<std::uint64_t> generation;
};
// Transitional, opt-in live renderer bridge. A local geometry fixture supplies
// the exact admitted program image; no retail bytes are embedded in this code.
// Declines before any live mutation. The caller retains reference execution
// for compatibility state, suppresses only its new packets, and submits these
// owned native packets in the same job order. This is not a performance mode.
[[nodiscard]] std::optional<std::vector<StripSubmissionPacket>>
prepare_native_strip_submission(std::span<const std::byte> micro,
    std::span<const std::byte> data, const guest::Vu1State&, std::uint16_t entry,
    std::uint32_t top, std::uint32_t count, NativeGeometryAdmission = {});

// Successful preparations by family (specular, supplied alpha, two-sided RGBA,
// two-sided specular, environment), reported only
// on the owning EE thread after execution has stopped.
[[nodiscard]] std::array<std::uint64_t, 5> native_strip_prepared_jobs();

// Owned results for the transitional live boundary. Prepare once, finish any
// older borrowed-memory producer, then commit these selected compatibility
// writes and publish packets. Preparation never mutates the shared inputs.
struct NativeStripCompletion {
    StripTransform transform;
    StripProjection projection;
    StripClipSummary clip;
    std::vector<TransformedStripPosition> transformed;
    std::vector<std::uint32_t> alpha;
    std::uint32_t top{};
    std::uint16_t view_selector{};
    std::uint16_t continuation{};
    bool reload_view{};
    bool supplied_alpha{};
};
struct PreparedNativeStripJob {
    std::vector<StripSubmissionPacket> packets;
    NativeStripCompletion completion;
};
[[nodiscard]] std::optional<PreparedNativeStripJob> prepare_native_strip_job(
    std::span<const std::byte> micro, std::span<const std::byte> data,
    const guest::Vu1State&, std::uint16_t entry, std::uint32_t top, std::uint32_t count,
    NativeGeometryAdmission = {});
void commit_native_strip_completion(const NativeStripCompletion&,
    std::span<std::byte> data, guest::Vu1State&);

// Transitional completion for the recovered in-place vertex data, view cache
// and continuation (including the launch owner's MSCNT target). Computes these writes from original job inputs, without
// executing VU instructions. Does not own producer timing, output scratch space,
// buffer swaps or other families' register dependencies; not a live replacement
// by itself. Declines unsupported input before any mutation.
[[nodiscard]] bool complete_native_strip_inputs(std::span<const std::byte> micro,
    std::span<std::byte> data, guest::Vu1State&, std::uint16_t entry,
    std::uint32_t top, std::uint32_t count);
// Bounded environment-material adapter. This is separate from strip completion:
// environment meshes own a paired output buffer and unpack normals/texture data.
// Preparation is read-only; commit must follow completion of older borrowed
// producers. Other families' scratch-register consumers still need live proof.
struct EnvironmentMemoryWrite {
    std::uint16_t quadword{};
    std::uint8_t lanes{};
    std::array<std::uint32_t, 4> words{};
};
struct NativeEnvironmentCompletion {
    std::vector<EnvironmentMemoryWrite> writes;
    std::array<std::array<float, 4>, 4> view{};
    std::array<float, 3> screen_offset{};
    std::uint16_t previous_buffer{}, output_buffer{}, padded_count{};
    bool reload_view{}, reflection{};
};
struct PreparedNativeEnvironmentJob {
    std::vector<StripSubmissionPacket> packets;
    NativeEnvironmentCompletion completion;
};
[[nodiscard]] std::optional<PreparedNativeEnvironmentJob> prepare_native_environment_job(
    std::span<const std::byte> micro, std::span<const std::byte> data,
    const guest::Vu1State&, std::uint16_t entry, std::uint32_t top, std::uint32_t count,
    NativeGeometryAdmission = {});
void commit_native_environment_completion(const NativeEnvironmentCompletion&,
    std::span<std::byte> data, guest::Vu1State&);

} // namespace takedown::runtime::slus_21050
