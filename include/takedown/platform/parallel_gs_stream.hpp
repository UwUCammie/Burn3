#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_privileged_display.hpp"
#include "takedown/platform/gs_stream_transport.hpp"
#include "takedown/platform/parallel_gs_host_presentation.hpp"
#include "takedown/platform/parallel_gs_host_ui.hpp"
#include "takedown/render/native_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace takedown::platform {

// One already-arbitrated GIF quadword. The raw paraLLEl-GS stream keeps the
// hardware path identity but has no timestamp or packet-framing metadata.
struct ParallelGsStreamQuadword {
    guest::GifPath path{};
    guest::GsQuadword quadword{};

    bool operator==(const ParallelGsStreamQuadword&) const = default;
};

// paraLLEl-GS consumes Transfer payload sizes as uint32 byte counts. Keep
// packets modest for live inspection while retaining GIF submission order.
inline constexpr std::size_t kParallelGsTransferChunkQuadwords = 4096U;

// SetGsCrt is handled by the no-BIOS kernel model, so its timing registers do
// not arrive as ordinary EE MMIO stores. Carry the small mode fact separately
// when producing an external GS stream; it is presentation metadata and never
// changes guest-visible state.
struct ParallelGsCrtcHint {
    bool configured{};
    std::int16_t interlace{};
    std::int16_t mode{};
    std::int16_t field_mode{};

    bool operator==(const ParallelGsCrtcHint&) const = default;
};

enum class ParallelGsStreamOpenMode : std::uint8_t {
    // Create a new regular capture file and refuse every existing path.
    ExclusiveFile,
    // Open an existing POSIX FIFO, or create a local Windows named pipe. This is the live Vulkan-compute bridge used
    // by parallel-gs-stream; regular files and other node types are rejected
    // so selecting live mode can never truncate user data.
    ExistingFifo,
};

// Aggregate host-only accounting for the lossless stream path. The encoder
// (emulation thread) keeps plain counters and never reads a clock; the output
// and mirror counters are maintained by the writer thread that delivers the
// stream, so output_write_nanoseconds is writer-thread time, not emulation
// thread time. The emulation thread's only possible stall is
// producer_wait_nanoseconds: time spent waiting for a free buffer segment
// because the renderer fell more than the buffered backlog behind.
// FIFO writes use nonblocking write(2), so fifo_backpressure_nanoseconds is
// time spent explicitly waiting for writable pipe capacity and
// output_write_nanoseconds excludes that wait.
// Windows uses blocking lossless pipe writes; their backpressure is included
// in output_write_nanoseconds and the POSIX fifo_backpressure counters stay zero.
// encode_nanoseconds is no longer sampled (it is always zero): encoding is a
// memcpy into the segment and timing it cost more than the copy.
struct ParallelGsStreamStatistics {
    std::uint64_t native_models_encoded{};
    std::uint64_t native_triangles_encoded{};
    std::uint64_t native_texture_definitions_encoded{};
    std::uint64_t guest_quadwords_submitted{};
    std::uint64_t transfer_packets_encoded{};
    std::uint64_t transfer_quadwords_encoded{};
    std::uint64_t vblank_boundaries_encoded{};
    std::uint64_t encoded_bytes{};
    std::uint64_t payload_copy_bytes{};
    std::uint64_t encode_nanoseconds{};
    std::uint64_t mirror_write_operations{};
    std::uint64_t mirror_bytes_written{};
    std::uint64_t mirror_write_nanoseconds{};
    std::uint64_t mirror_flush_operations{};
    std::uint64_t mirror_flush_nanoseconds{};
    std::uint64_t output_write_operations{};
    std::uint64_t output_bytes_written{};
    std::uint64_t output_write_nanoseconds{};
    std::uint64_t output_flush_operations{};
    std::uint64_t output_flush_nanoseconds{};
    std::uint64_t fifo_backpressure_waits{};
    std::uint64_t fifo_backpressure_nanoseconds{};
    std::uint64_t segments_published{};
    std::uint64_t oversize_segments{};
    std::uint64_t producer_wait_operations{};
    std::uint64_t producer_wait_nanoseconds{};
};

// Writes the headerless packet stream consumed by parallel-gs-stream's
// GSDumpParser::open_raw(). This writer must be attached at a fresh GS reset
// boundary. Before the first guest quadword it emits one self-contained A+D
// Transfer which makes the hardware PRMODECONT.AC=1 reset default explicit;
// every subsequent GIF quadword is the original, ordered guest value. The
// destination is created exclusively: an existing file is never truncated or
// replaced. Live FIFO mode may additionally mirror the exact encoded byte
// stream to an exclusively created regular file. The mirror is written from
// the same packet buffer, before the corresponding FIFO write, so it cannot
// perturb packet framing or order.
//
// The raw grammar has no GIF reset record. A GIF_CTRL.RST observed while every
// used path has completed an EOP tag is semantically empty for the downstream
// path parsers and is therefore omitted. A reset which would discard a partial
// or non-EOP packet remains an error at the next reporting boundary.
//
// Packet encoding is little-endian and byte packed:
//   Transfer:      u8(0), u8(path), u32(payload bytes), 16-byte GIF QWs
//   PrivRegisters: u8(3), one 0x2000-byte GS privileged-register snapshot
//   Vsync:         u8(1), u8(phase)
//   NativeModel:   u8(4), u32(payload bytes), native_model_protocol v1 payload
//   HostPresentation: u8(5), u32(payload bytes), see
//                  takedown/platform/parallel_gs_host_presentation.hpp
//
// Adjacent quadwords on one path are coalesced up to
// kParallelGsTransferChunkQuadwords. A path change and every VBlank preserve
// the ordering boundary by flushing the pending Transfer first.
//
// Transport: packets are encoded directly into large reusable segments owned
// by a GsStreamTransport (see gs_stream_transport.hpp). The emulation thread
// never performs the output syscall; a dedicated writer thread delivers the
// segments in order. A segment is handed over when it fills and at every
// VBlank; the emulation thread blocks only if the bounded segment pool is
// exhausted (renderer backpressure). Output errors surface from the next
// producer call. flush() and close() wait until everything is delivered.
class ParallelGsOutputSink;

class ParallelGsRawStreamWriter final : public guest::GsRawGifObserver {
  public:
    explicit ParallelGsRawStreamWriter(
        const std::filesystem::path& output_path,
        ParallelGsStreamOpenMode open_mode = ParallelGsStreamOpenMode::ExclusiveFile,
        std::optional<std::filesystem::path> live_capture_path = std::nullopt);
    ~ParallelGsRawStreamWriter() override;

    ParallelGsRawStreamWriter(const ParallelGsRawStreamWriter&) = delete;
    ParallelGsRawStreamWriter& operator=(const ParallelGsRawStreamWriter&) = delete;
    ParallelGsRawStreamWriter(ParallelGsRawStreamWriter&&) = delete;
    ParallelGsRawStreamWriter& operator=(ParallelGsRawStreamWriter&&) = delete;

    void write_gif_quadword(guest::GifPath path, const guest::GsQuadword& quadword);
    void write_gif_quadwords(std::span<const ParallelGsStreamQuadword> quadwords);
    void on_gs_gif_quadword(guest::GifPath path, const guest::GsQuadword& quadword) override;
    void on_gs_gif_quadwords(guest::GifPath path,
                             std::span<const guest::GsQuadword> quadwords) override;
    void on_gs_gif_reset() noexcept override;

    // Publishes a complete game model at its ordered execution boundary. This
    // must not be called merely because an EE producer has prepared a queue.
    // Rejects malformed data and partial compatibility packets before writing;
    // IO errors after publication starts are fatal, never permission to replay
    // the model through a compatibility fallback. Sequences start at one.
    void write_native_model(const TakedownNative::ModelPublication& model);

    // NS-3: appends one native command record (u8(8), u32(payload bytes), payload; see native_cmd_protocol.hpp). Like the other
    // non-Transfer records it ends the open Transfer, so the GIF quadwords around it keep their order.
    void write_native_cmd(std::span<const std::uint8_t> payload);

    // Emits PrivRegisters immediately followed by Vsync. phase is the GS
    // field phase accepted by paraLLEl-GS and must be 0 or 1. The current
    // observer tracks the seven display-relevant registers; all other bytes
    // in paraLLEl-GS's 0x2000-byte privileged snapshot are written as zero.
    void write_vblank(std::uint8_t phase, const guest::GsPrivilegedDisplayState& display_state,
                      ParallelGsCrtcHint crt_hint = {});

    // Latches the host presentation state (PC options and the options menu
    // overlay). It is written once, ahead of the next VBlank, and only when
    // it differs from what the renderer already has; the renderer starts
    // from a default-constructed state, so a session that never changes it
    // writes nothing. The record is host-only: it is left out of the digest
    // log and of the live capture mirror, which describe the guest stream.
    void set_host_presentation(const ParallelGsHostPresentation& presentation);
    // Latches the host UI layer (parallel_gs_host_ui.hpp: the MODS page's
    // icons and details). Written ahead of the next VBlank when it changed;
    // host-only like the presentation record.
    void set_host_ui(const ParallelGsHostUi& ui);
    // Writes a Panini warp record now, after everything submitted so far
    // (parallel_gs_host_presentation.hpp). Host-only like the presentation:
    // it stays out of the digest and is mirrored only when asked for.
    void write_panini_warp(std::uint16_t tangent, std::uint8_t strength, std::uint16_t extent,
                           bool widescreen);

    // flush() waits until every submitted packet has been delivered to the
    // output. close() additionally closes it, reports final write or close
    // errors and is idempotent. The destructor makes a best-effort close for
    // normal RAII use, but callers that need error reporting call close().
    void flush();
    void close();

    // Starts a text log with one line per VBlank: its ordinal, the encoded
    // bytes so far and a running digest of them. Two runs that send the
    // renderer the same stream produce the same log, and the first line
    // that differs names the frame where they diverge, without keeping a
    // multi-gigabyte capture. The file must not exist yet. Call it before the
    // first guest quadword; it drains the transport so the digest covers the
    // stream from that point.
    void record_digest(const std::filesystem::path& path);

    // A snapshot: output/mirror counters belong to the writer thread.
    [[nodiscard]] ParallelGsStreamStatistics statistics() const;

  private:
    struct GifPathFraming {
        std::uint32_t payload_quadwords_remaining{};
        bool completing_tag_eop{};
        bool at_eop_boundary{true};
    };

    void require_open() const;
    void throw_if_unsupported_reset() const;
    void append_quadword_slow(guest::GifPath path, const guest::GsQuadword& quadword);
    void write_reset_defaults(std::size_t path_index);
    void begin_transfer(std::size_t path_index);
    void track_gif_quadword(std::size_t path_index, const guest::GsQuadword& quadword) noexcept;
    [[nodiscard]] bool gif_paths_at_eop_boundaries() const noexcept;
    void flush_transfer();
    // Appends one whole packet (a digest/mirror "piece") to the open segment.
    void append_packet(const void* data, std::size_t size, std::uint32_t piece_flags);
    void ensure_segment_space(std::size_t bytes);
    void publish_segment();
    void write_pending_host_presentation();
    void write_pending_host_ui();

    std::unique_ptr<GsStreamTransport> transport_{};
    ParallelGsOutputSink* sink_{};  // owned by transport_
    GsStreamSegment* segment_{};    // current producer-side segment (not yet published)
    // The open Transfer packet, written in place in segment_. A zero-length
    // run (cursor_ == limit_) or no open transfer sends the next quadword to
    // append_quadword_slow(), which is also where a poisoned stream throws.
    std::uint8_t* header_{};
    std::uint8_t* cursor_{};
    std::uint8_t* limit_{};
    int open_path_{-1};
    bool open_transfer_is_reset_{};
    bool closed_{};
    bool track_pieces_{};
    bool digest_attached_{};
    std::array<GifPathFraming, 3U> gif_path_framing_{};
    ParallelGsStreamStatistics statistics_{};
    bool reset_defaults_emitted_{};
    bool submitted_quadword_{};
    bool unsupported_reset_seen_{};
    std::uint64_t native_model_sequence_{};
    ParallelGsHostPresentation sent_host_presentation_{};
    std::optional<ParallelGsHostPresentation> pending_host_presentation_{};
    ParallelGsHostUi sent_host_ui_{};
    std::optional<ParallelGsHostUi> pending_host_ui_{};
};

// FSR 3.1 upscaling for the next host presentation record (version 4 when on).
// Process-wide rather than a writer member: the writer's layout is shared
// with release objects. The writer's set_host_presentation() reads it, so set
// it before handing the writer the presentation it goes with.
void set_parallel_gs_upscaling(const ParallelGsUpscaling& upscaling) noexcept;

// Convenience for a one-VBlank capture. It has the same exclusive-create
// behavior as ParallelGsRawStreamWriter.
void write_parallel_gs_raw_stream(const std::filesystem::path& output_path,
                                  std::span<const ParallelGsStreamQuadword> quadwords,
                                  std::uint8_t phase,
                                  const guest::GsPrivilegedDisplayState& display_state,
                                  ParallelGsCrtcHint crt_hint = {});

} // namespace takedown::platform
