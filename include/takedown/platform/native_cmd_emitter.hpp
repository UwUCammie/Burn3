#pragma once

// NS-3: the game side of the native command stream (see native_cmd_protocol.hpp).
//
// One object plays three roles on the graphics device model's thread (the thread that runs VU1 launches and arbitrates the GIF,
// whichever of the emulation thread or the async graphics worker owns the device model; the two are never concurrent):
//   * Vu1LaunchObserver: chained in front of the native-cars observer. Before each launch it identifies the VU1 program, decodes
//     the vertex group / instance block / car batch into a cached mesh, and queues what a strip of that launch must look like.
//   * GsRawGifObserver: it is the *primary* raw observer in front of ParallelGsRawStreamWriter. It parses PATH1 tags, holds back
//     each complete-able strip tag, checks it against the queued launches (cars: exact hash of the packet native cars built;
//     world and props: the strip's vertices against the projected input mesh), writes the definitions and the DRAW record, and then
//     either keeps the strip (shadow mode) or replaces it by a token (token mode). Everything else is forwarded unchanged.
//   * NativeCmdStreamHooks: the writer calls it for ordering with its own non-GIF records and at vblanks.
// It never changes guest state: it only reads VU1 memory in the observer calls, and the stream it feeds is host-only, so guest
// timing and every guest-visible byte are identical to a run without it (verified by the legacy digest, see REPORT).

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"
#include "takedown/platform/native_cmd_decode.hpp"
#include "takedown/platform/native_cmd_hooks.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"
#include "takedown/platform/parallel_gs_stream.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace takedown::platform {

struct NativeCmdConfig {
    int mode{0};                 // TAKEDOWN_NATIVE_CMD: 0 off (default), 1 shadow (records + legacy strips), 2 tokens replace strips
    bool elide{false};           // TAKEDOWN_NATIVE_CMD_ELIDE=1: tokens carry no payload quadwords (transport saving, kFeatElided)
    bool echo{false};            // TAKEDOWN_NATIVE_CMD_ECHO=1: ECHO commands carry the replaced strips (verification only)
    bool force{false};           // TAKEDOWN_NATIVE_CMD_FORCE=1: do not require the renderer's capability event
    bool v2{true};               // TAKEDOWN_NATIVE_CMD_V2=0: the first emitter (no survivor masks, same-packet clipper strips tokenized as extras)
    bool timing{false};          // TAKEDOWN_NATIVE_CMD_TIME=1: measure the emitter's own time
    std::uint32_t window{128};    // TAKEDOWN_NATIVE_CMD_WINDOW: queue entries tried per strip
    std::uint32_t mesh_ttl{900}; // frames a mesh may stay unused before MESH_FREE
    std::uint32_t reset_every{0}; // TAKEDOWN_NATIVE_CMD_RESET_EVERY=N: RESET_ALL every N frames (tests the resend path)
    std::uint64_t mesh_budget{256ULL << 20U};
    std::uint32_t log_every{600}; // frames between [NATIVECMD] statistics lines (0 = never)
};
[[nodiscard]] NativeCmdConfig native_cmd_config_from_environment();

// True when the renderer behind `pipe_path` announced protocol `version` (Windows event
// "Local\takedown-gs-ncap-<base>-v<version>", see native_cmd_protocol.hpp).
[[nodiscard]] bool native_cmd_renderer_supports(const std::filesystem::path& pipe_path, std::uint32_t version);

class NativeCmdEmitter final : public guest::Vu1LaunchObserver, public guest::GsRawGifObserver, public NativeCmdStreamHooks {
  public:
    struct Stats {
        std::uint64_t launches{}, launches_world{}, launches_prop{}, launches_car{}, pending_dropped{}, v2_left{}, v2_unmapped{}, masked{};
        std::uint64_t strips_seen{}, strips_held{}, strips_left{}, strips_partial_flush{};
        std::uint64_t matched_world{}, matched_prop{}, matched_car{}, matched_extra{}, skipped_entries{};
        std::uint64_t tokens{}, draws{}, mesh_defs{}, mesh_frees{}, view_defs{}, car_const_defs{}, resets{};
        std::uint64_t def_bytes{}, draw_bytes{}, replaced_qw{}, probe_mismatch{}, records{};
        std::uint64_t ns_launch{}, ns_stream{};
        std::uint64_t cyc[24]{}; // TAKEDOWN_NATIVE_CMD_TIME: rdtsc cycles per section (see Section in the .cpp)
    };

    NativeCmdEmitter(ParallelGsRawStreamWriter& writer, guest::Vu1LaunchObserver* inner, NativeCmdConfig config);
    ~NativeCmdEmitter() override;
    NativeCmdEmitter(const NativeCmdEmitter&) = delete;
    NativeCmdEmitter& operator=(const NativeCmdEmitter&) = delete;

    // Vu1LaunchObserver
    std::optional<guest::Vu1LaunchReplacement> before_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) override;
    void after_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) override;
    // GsRawGifObserver
    void on_gs_gif_quadword(guest::GifPath path, const guest::GsQuadword& quadword) override;
    void on_gs_gif_quadwords(guest::GifPath path, std::span<const guest::GsQuadword> quadwords) override;
    void on_gs_gif_reset() noexcept override;
    // NativeCmdStreamHooks
    void flush_held() override;
    void on_vblank() override;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void log_stats(const char* tag) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Stats stats_{};
};

} // namespace takedown::platform
