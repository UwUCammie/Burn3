#pragma once

// Renders the modelled SPU2 machine to a host output bus (docs/AUDIO.md §8
// increment 3).
//
// The machine is stepped on the emulation thread, paced by the guest EE
// cycle counter (6144 EE cycles per 48 kHz sample — the SPU2 sample clock is
// a fixed division of the PS2 clock, so guest-time pacing needs no wall
// clock and carries no catch-up debt across host pauses: a paused guest is
// a paused SPU2, which is the hardware semantic). Stepped samples are
// submitted to a dedicated 48 kHz HostAudioSink bus and, when a dump path is
// configured, appended to a 48 kHz stereo s16 WAV — the seed of the
// audio-diff harness: the same route can be rendered here and in PCSX2 and
// compared byte-wise or by ear.
//
// Write ordering (increment 6): guest register/DMA writes enter through
// Spu2GuestLink, whose optional write barrier calls drain_through() at the
// write's guest cycle BEFORE mutating the machine. Everything a pump_due at
// that exact cycle would render (every sample whose start cycle is <= the
// write cycle) is rendered first, so the write affects only samples that
// begin after it — the identical stream a finely pumped renderer would
// produce, no matter how far periodic pump_due calls lag behind (large
// guest-cycle jumps leave ordered debt; the barrier retires it at exactly
// the right sample).
//
// Deterministic: identical guest scripts produce identical sample streams;
// fnv1a64() pins that in tests and differential captures.

#include "takedown/audio/guest_clocks.hpp"
#include "takedown/audio/host_audio_sink.hpp"
#include "takedown/audio/spu2_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace takedown::audio {

class Spu2Renderer {
  public:
    // SPU2 nominal sample rate.
    static constexpr std::uint32_t kSampleRate = guest_clocks::kSpu2OutputHz;
    // EE cycles per output sample (294.912 MHz / 48 kHz).
    static constexpr std::uint64_t kEeCyclesPerSample =
        guest_clocks::kEeCyclesPerSpu2Sample;
    // Read-only guest-clock cursor through an exact EE cycle. This includes
    // every sample whose start cycle is <= guest_cycle, independent of any
    // bounded rendering debt retained by pump_due().
    [[nodiscard]] static constexpr std::uint64_t
    guest_sample_cursor_through(const std::uint64_t guest_cycle) noexcept {
        return guest_cycle / kEeCyclesPerSample + 1U;
    }
    // Upper bound on samples stepped in one pump. A large guest-cycle jump
    // leaves the remaining samples as ordered debt for later pump_due calls
    // (or a drain_through barrier at the next guest write); no model sample
    // is skipped or replaced with wall-clock catch-up.
    static constexpr std::uint32_t kMaxSamplesPerPump = 256;

    explicit Spu2Renderer(Spu2System& system);

    Spu2Renderer(const Spu2Renderer&) = delete;
    Spu2Renderer& operator=(const Spu2Renderer&) = delete;
    ~Spu2Renderer();

    // Bus ownership: the renderer opens and owns its 48 kHz stereo bus.
    [[nodiscard]] HostAudioSink& sink() noexcept { return sink_; }

    // Optional host-transport backpressure seam. Called after every bounded
    // sink submission, including batches emitted by an unbounded guest-write
    // drain, so pacing cannot be deferred until the outer guest loop resumes.
    void set_submit_observer(std::function<void()> observer) {
        submit_observer_ = std::move(observer);
    }

    // Enables a WAV dump; the file is finalized in the destructor. The
    // directory must exist. Returns false when the file cannot be opened.
    [[nodiscard]] bool start_wav_dump(const std::filesystem::path& path);

    // Steps up to kMaxSamplesPerPump samples due at guest cycle `guest_cycle`.
    // Any excess remains due and is drained in order by later calls. Cheap
    // when nothing is due (one comparison).
    void pump_due(std::uint64_t guest_cycle);

    // Renders every sample due at `guest_cycle` (start cycle <= guest_cycle,
    // the pump_due predicate), with no per-call cap. The guest-write barrier
    // contract: a register/DMA write at guest cycle X may only affect
    // samples beginning after X, so all due debt is retired before the
    // write is applied. Cheap when nothing is due.
    void drain_through(std::uint64_t guest_cycle);

    [[nodiscard]] std::uint64_t samples_rendered() const noexcept { return samples_rendered_; }
    // Start cycle of the next sample to render: pump_due(c) and
    // drain_through(c) are no-ops exactly while c < next_sample_cycle().
    [[nodiscard]] std::uint64_t next_sample_cycle() const noexcept { return next_sample_cycle_; }
    [[nodiscard]] std::uint64_t non_silent_frames() const noexcept { return non_silent_frames_; }
    // FNV-1a64 over every rendered sample pair (little-endian s16 bytes) —
    // the determinism anchor for tests and routes.
    [[nodiscard]] std::uint64_t fnv1a64() const noexcept { return fnv1a64_; }

  private:
    void finish_sample(Spu2System::Stereo sample);
    void render_samples(std::uint64_t count);
    void submit_pending_frames();
    void write_wav_header();
    void finalize_wav_dump();

    static constexpr std::size_t kSubmitBatchFrames = 256;

    Spu2System& system_;
    HostAudioSink sink_;
    std::function<void()> submit_observer_{};
    std::uint64_t next_sample_cycle_{0};
    std::uint64_t samples_rendered_{0};
    std::uint64_t non_silent_frames_{0};
    std::uint64_t fnv1a64_{0xcbf29ce484222325ULL};
    std::vector<std::int16_t> pending_frames_{};
    std::array<Spu2System::Stereo, Spu2System::kMaxBlockSamples> block_{};
    std::optional<std::filesystem::path> wav_path_{};
    std::FILE* wav_file_{nullptr};
    std::uint64_t wav_data_bytes_{0};
};

} // namespace takedown::audio
