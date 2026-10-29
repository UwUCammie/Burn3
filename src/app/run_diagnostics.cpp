#include "takedown/platform/process.hpp"
#include "takedown/runtime/slus_21050_model_live.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>

namespace takedown::app {

namespace {

// Wraps a counter that is always present in the snapshot.
const std::optional<std::uint64_t> present(const std::uint64_t value) noexcept {
    return value;
}

} // namespace

// The original GuestWorkHostSnapshot header is absent from the recovered tree.
// Field offsets below are the ones the original printer reads; unrecovered
// counters between them are kept as explicitly reserved storage.
struct GuestWorkHostSnapshot {
    platform::HostCpuSnapshot cpu{};
    std::uint64_t audio_wait_ns{};
    std::uint64_t gs_write_ns{};
    std::array<std::uint8_t, 8> reserved_64{};
    std::array<std::uint64_t, 5> prepared_by_family{};
    std::uint64_t submission_jobs{};
    std::uint64_t execution_jobs{};
    std::uint64_t packets{};
    std::uint64_t quadwords{};
    std::uint64_t admitted{};
    std::uint64_t published{};
    std::array<std::uint8_t, 104> reserved_160{};
    std::uint64_t triangles{};
    std::array<std::uint8_t, 64> reserved_272{};
    std::uint64_t preparations{};
    std::uint64_t reuses{};
    std::array<std::uint8_t, 24> reserved_352{};
    std::uint64_t worker_jobs{};
    std::uint64_t caller_jobs{};
    std::array<std::uint8_t, 8> reserved_392{};
    std::uint64_t vu1_aot_pairs{};
    std::uint64_t launches{};
    std::uint64_t completed{};
};

__attribute__((noinline, cold)) void print_guest_work_host_statistics(const GuestWorkHostSnapshot& a,
                                                                      const GuestWorkHostSnapshot& b,
                                                                      bool model_enabled) {
    const auto print_delta = [](const std::optional<std::uint64_t>& first,
                                const std::optional<std::uint64_t>& last) {
        if (first && last && *last >= *first) {
            std::cout << (*last - *first);
        } else {
            std::cout << "unavailable";
        }
    };
    std::cout << "  Guest-work host: thread-cpu-ns=";
    print_delta(a.cpu.thread_nanoseconds, b.cpu.thread_nanoseconds);
    std::cout << " process-cpu-ns=";
    print_delta(a.cpu.process_nanoseconds, b.cpu.process_nanoseconds);
    std::cout << " thread-cycles=";
    print_delta(a.cpu.thread_cycles, b.cpu.thread_cycles);
    std::cout << " audio-wait-ns=";
    print_delta(present(a.audio_wait_ns),
                present(b.audio_wait_ns));
    std::cout << " gs-write-ns=";
    print_delta(present(a.gs_write_ns),
                present(b.gs_write_ns));
    std::cout << '\n';
    std::cout << "  VU1 AOT window: pairs=";
    print_delta(present(a.vu1_aot_pairs),
                present(b.vu1_aot_pairs));
    std::cout << " launches=";
    print_delta(present(a.launches), present(b.launches));
    std::cout << " completed=";
    print_delta(present(a.completed), present(b.completed));
    std::cout << '\n';
    std::cout << "  Guest-work native: execution-jobs=";
    print_delta(present(a.execution_jobs),
                present(b.execution_jobs));
    std::cout << " submission-jobs=";
    print_delta(present(a.submission_jobs),
                present(b.submission_jobs));
    std::cout << " packets=";
    print_delta(present(a.packets), present(b.packets));
    std::cout << " quadwords=";
    print_delta(present(a.quadwords), present(b.quadwords));
    const std::array<const char*, 5> native_family_names{
        "specular", "supplied-alpha", "two-sided", "two-sided-specular", "environment"};
    for (std::size_t family = 0U; family != 5U; ++family) {
        std::cout << " prepared-" << native_family_names[family] << '=';
        print_delta(present(a.prepared_by_family[family]),
                    present(b.prepared_by_family[family]));
    }
    std::cout << '\n';
    if (model_enabled) {
        std::cout << "  Guest-work model: admitted=";
        print_delta(present(a.admitted), present(b.admitted));
        std::cout << " published=";
        print_delta(present(a.published), present(b.published));
        std::cout << " triangles=";
        print_delta(present(a.triangles), present(b.triangles));
        std::cout << " preparations=";
        print_delta(present(a.preparations),
                    present(b.preparations));
        std::cout << " reuses=";
        print_delta(present(a.reuses), present(b.reuses));
        std::cout << " worker-jobs=";
        print_delta(present(a.worker_jobs),
                    present(b.worker_jobs));
        std::cout << " caller-jobs=";
        print_delta(present(a.caller_jobs),
                    present(b.caller_jobs));
        std::cout << '\n';
    }
}

__attribute__((noinline, cold)) void
print_native_model_statistics(const runtime::slus_21050::ModelLive& native_model_live) {
    const auto live = native_model_live.statistics();
    const auto owner = native_model_live.producer_statistics();
    std::cout << "  native model parent: admitted=" << live.admitted << " published=" << live.published
              << " pending=" << live.pending << " resolved=" << live.resolved
              << " cancelled=" << live.cancelled << " source-uploads=" << live.source_uploads
              << " source-reuses=" << live.source_reuses
              << " declined-allocator=" << live.declined_allocator
              << " declined-source=" << live.declined_source
              << " declined-geometry=" << live.declined_geometry << " triangles=" << owner.triangles
              << " definitions=" << owner.texture_definitions << '\n';
    const auto geometry = native_model_live.geometry_statistics();
    const auto reuse = native_model_live.geometry_reuse_statistics();
    std::cout << "  native model geometry cache: enabled=" << reuse.enabled
              << " preparations=" << reuse.preparations << " reuses=" << reuse.reuses << '\n';
    const auto assets = native_model_live.asset_statistics();
    std::cout << "  native model assets: topology-imports=" << assets.topology_imports
              << " topology-reuses=" << assets.topology_reuses << " mesh-imports=" << assets.mesh_imports
              << " mesh-reuses=" << assets.mesh_reuses << " texture-imports=" << assets.texture_imports
              << " texture-reuses=" << assets.texture_reuses << " live-bytes=" << assets.live_bytes
              << " resident-bytes=" << assets.resident_bytes
              << " budget-declines=" << assets.budget_declines << '\n';
    if (geometry.worker_threads != 0U) {
        std::cout << "  native model geometry workers: threads=" << geometry.worker_threads
                  << " batches=" << geometry.batches << " jobs=" << geometry.jobs
                  << " worker-jobs=" << geometry.worker_jobs << " caller-jobs=" << geometry.caller_jobs
                  << " failed-batches=" << geometry.failed_batches << '\n';
    }
}

} // namespace takedown::app
