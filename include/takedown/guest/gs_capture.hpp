#pragma once

#include "takedown/guest/gs_replay_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace takedown::guest {

using GsQuadword = std::array<std::uint32_t, 4>;

enum class GifPath : std::uint8_t {
    Path1 = 0U,
    Path2 = 1U,
    Path3 = 2U,
};

// GIF packets can carry GS register writes through the packed A+D form, a
// packed direct register descriptor, a REGLIST descriptor, or the PRIM field
// embedded in a GIFtag. Keep the transport visible so later rendering work
// never confuses a 4-bit GIF descriptor with a full A+D register address.
enum class GsRegisterWriteSource : std::uint8_t {
    GifTagPrim,
    Packed,
    Reglist,
    Ad,
};

enum class GsCaptureDiagnosticCode : std::uint8_t {
    UnsupportedGifFormat,
    UnsupportedPixelStorageMode,
    InvalidImageTransfer,
    UnsupportedVifCommand,
    IncompleteVifDirect,
    DmaChainOutOfRange,
    UnsupportedDmaMode,
    UnsupportedDmaTag,
    DmaChainLimit,
    IncompleteGifPacket,
    CaptureLimit,
};

[[nodiscard]] const char* to_string(GsCaptureDiagnosticCode code) noexcept;

struct GsCaptureDiagnostic {
    GsCaptureDiagnosticCode code{};
    std::uint32_t detail{};
    std::uint32_t address{};

    bool operator==(const GsCaptureDiagnostic&) const = default;
};

struct GsRegisterWrite {
    std::uint64_t sequence{};
    GifPath path{};
    GsRegisterWriteSource source{};
    std::uint8_t address{};
    std::uint64_t value{};
    // Direct PACKED descriptors carry a full 128-bit payload. Keeping the
    // upper half is essential for XYZ Z/ADC and several other packed layouts.
    // It is zero for GIFtag PRIM, REGLIST, and A+D writes.
    std::uint64_t packed_high{};

    bool operator==(const GsRegisterWrite&) const = default;
};

// Counters remain useful even when the bounded detailed trace reaches its
// storage limit. They are observational only and never influence GIF/DMAC
// completion behavior.
struct GsCaptureActivity {
    std::uint64_t gif_tag_prim_writes{};
    std::uint64_t packed_register_writes{};
    std::uint64_t reglist_register_writes{};
    std::uint64_t ad_register_writes{};
    std::uint64_t omitted_register_writes{};
    // Valid IMAGE transfers are queued before the bounded/de-duplicated PPM
    // history receives them. A host diagnostic surface drains this stream so
    // repeated writes are observable without making capture memory unbounded.
    std::uint64_t queued_image_transfers{};
    std::uint64_t omitted_queued_image_transfers{};
};

struct GsImageTransfer {
    std::uint64_t sequence{};
    GifPath path{};
    std::uint32_t destination_base_pointer{};
    std::uint32_t destination_buffer_width{};
    std::uint32_t destination_psm{};
    std::uint32_t destination_x{};
    std::uint32_t destination_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels{};

    bool operator==(const GsImageTransfer&) const = default;
};

// A synchronous, non-owning bridge from the GIF decoder to a renderer. The
// bounded diagnostic histories below remain useful for reports, while this
// observer sees every decoded command in submission order and can maintain a
// complete live GS state without depending on those history limits.
class GsCommandObserver {
  public:
    virtual ~GsCommandObserver() = default;
    // GIF's PACKED Q latch is reset to 1.0 at the start of every non-empty
    // GIFtag.  Keep that boundary in the command stream: deriving it from
    // register writes is incorrect for tags whose first descriptor is not ST.
    virtual void on_gs_gif_tag_begin(GifPath path) { static_cast<void>(path); }
    // CSR.RESET clears the GS drawing environment and in-flight path state,
    // but it does not make local memory a freshly zeroed host allocation.
    // Keep this as an ordered command so asynchronous observers reset at the
    // same point as the decoded GIF stream.
    virtual void on_gs_reset() {}
    virtual void on_gs_register_write(const GsRegisterWrite& write) = 0;
    virtual void on_gs_image_transfer(const GsImageTransfer& transfer) = 0;
    // Ownership-aware observers may consume a completed transfer without
    // copying its potentially large pixel vector. Existing observers retain
    // their const-reference behavior through this default bridge.
    virtual void on_gs_image_transfer(GsImageTransfer&& transfer) {
        on_gs_image_transfer(static_cast<const GsImageTransfer&>(transfer));
    }
};

// Synchronous observation of the already-arbitrated GIF FIFO before packet
// decoding. Differential traces and alternate GS backends need the original
// 128-bit words, path identity, and packet framing; reconstructed register
// writes are not sufficient to diagnose VIF/VU/GIF ordering faults.
class GsRawGifObserver {
  public:
    virtual ~GsRawGifObserver() = default;
    virtual void on_gs_gif_quadword(GifPath path, const GsQuadword& quadword) = 0;
    // A run of consecutive quadwords on one path, equivalent to calling
    // on_gs_gif_quadword for each in order. Observers which append to a
    // buffer override this to copy the run at once.
    virtual void on_gs_gif_quadwords(GifPath path, std::span<const GsQuadword> quadwords) {
        for (const auto& quadword : quadwords) {
            on_gs_gif_quadword(path, quadword);
        }
    }
    // Queried once when the observer is attached. Existing raw stream writers
    // keep the EOP path completely inactive and pay no per-packet callback.
    [[nodiscard]] virtual bool observes_gs_gif_packet_eop() const noexcept {
        return false;
    }
    // Delivered from EeMemory's authoritative GIF arbiter after a complete
    // packet reaches EOP. primary_command_stream_available is true only when
    // capture observed the complete packet while raw passthrough stayed off
    // and the same primary command stream remained continuously available.
    // It reports delivery continuity, not semantic GS acceptance.
    // Dispatch uses an attachment snapshot. Callbacks must not destroy an
    // attached observer or mutate observer registrations before dispatch
    // returns.
    virtual void on_gs_gif_packet_eop(GifPath path,
                                      bool primary_command_stream_available) noexcept {
        static_cast<void>(path);
        static_cast<void>(primary_command_stream_available);
    }
    // GIF_CTRL.RST can discard a partial packet without submitting another
    // quadword. Raw stream decoders need this boundary to keep their three
    // path-local tag cursors aligned; observers which only serialize words
    // can ignore it.
    virtual void on_gs_gif_reset() noexcept {}
};

class GsCapture {
  public:
    GsCapture() = default;
    ~GsCapture() = default;
    GsCapture(const GsCapture&) = delete;
    GsCapture& operator=(const GsCapture&) = delete;
    GsCapture(GsCapture&&) noexcept = default;
    GsCapture& operator=(GsCapture&&) noexcept = default;

    void begin_vif1_dma() noexcept;
    void submit_vif1_word(std::uint32_t word);
    void submit_vif1_quadword(const GsQuadword& quadword);
    void end_vif1_dma();
    void submit_gif_quadword(GifPath path, const GsQuadword& quadword) {
        if (raw_only_observer_ != nullptr) [[likely]] {
            gif_packet_observation_open_[static_cast<std::size_t>(path)] = true;
            raw_only_observer_->on_gs_gif_quadword(path, quadword);
            return;
        }
        submit_gif_quadword_general(path, quadword);
    }
    // Equivalent to submit_gif_quadword for each element, in order.
    void submit_gif_quadwords(GifPath path, std::span<const GsQuadword> quadwords) {
        if (raw_only_observer_ != nullptr) [[likely]] {
            if (!quadwords.empty()) {
                gif_packet_observation_open_[static_cast<std::size_t>(path)] = true;
                raw_only_observer_->on_gs_gif_quadwords(path, quadwords);
            }
            return;
        }
        for (const auto& quadword : quadwords) {
            submit_gif_quadword_general(path, quadword);
        }
    }

    void record_diagnostic(GsCaptureDiagnosticCode code, std::uint32_t detail = 0U,
                           std::uint32_t address = 0U);

    [[nodiscard]] std::span<const GsRegisterWrite> register_writes() const noexcept;
    [[nodiscard]] std::span<const GsImageTransfer> image_transfers() const noexcept;
    [[nodiscard]] std::span<const GsCaptureDiagnostic> diagnostics() const noexcept;
    [[nodiscard]] const GsCaptureActivity& activity() const noexcept;
    // Observer ownership may change without invalidating packet continuity
    // only while no authoritative GIF packet is open.
    [[nodiscard]] bool gif_observation_quiescent() const noexcept;
    // Borrowed-observer replacement is a serialized, quiescent operation.
    // These accessors let scoped bridges reject occupied slots and restore
    // only registrations they actually own.
    [[nodiscard]] GsCommandObserver* command_observer() const noexcept {
        return command_observer_;
    }
    [[nodiscard]] GsRawGifObserver* terminal_raw_gif_observer() const noexcept {
        return terminal_raw_gif_observer_;
    }
    // True when nothing but the primary raw GIF observer (the GS stream
    // writer, a digest) is attached: no decoded-command, terminal or
    // diagnostic consumer whose thread affinity the capture would have to
    // honor. The asynchronous graphics window requires this.
    [[nodiscard]] bool only_primary_raw_observer() const noexcept {
        return command_observer_ == nullptr && terminal_command_observer_ == nullptr &&
               terminal_raw_gif_observer_ == nullptr && diagnostic_command_observer_ == nullptr &&
               diagnostic_raw_gif_observer_ == nullptr;
    }
    [[nodiscard]] std::weak_ptr<const bool> lifetime_token() const noexcept {
        return lifetime_token_;
    }
    [[nodiscard]] std::vector<GsImageTransfer> take_completed_image_transfers();

    void set_command_observer(GsCommandObserver* observer) noexcept;
    void set_raw_gif_observer(GsRawGifObserver* observer) noexcept;

    // A production-only synchronous tap for the future FEMain terminal
    // producer. It is deliberately separate from the bounded diagnostic
    // observers and remains detached by default. Supplying an observer here
    // does not grant terminal authority; it only exposes the same validated
    // command/FIFO boundaries without changing the primary observer's role.
    void set_terminal_command_observer(GsCommandObserver* observer) noexcept;
    void set_terminal_raw_gif_observer(GsRawGifObserver* observer) noexcept;

    // A second, observational tap at the same post-arbitration boundary. It
    // lets bounded semantic diagnostics coexist with the renderer and with an
    // exact raw-stream backend. Supplying the primary observer here as well is
    // harmless: duplicate delivery is suppressed by pointer identity.
    void set_diagnostic_command_observer(GsCommandObserver* observer) noexcept;
    void set_diagnostic_raw_gif_observer(GsRawGifObserver* observer) noexcept;

    // Deliver the exact post-arbitration PATH words to raw observers without
    // reconstructing GS registers, IMAGE payloads, or command callbacks. This
    // is a host-performance mode for a raw external GS backend and must be
    // selected only when no decoded command/history consumer is configured.
    // It never changes GIF/DMAC scheduling in EeMemory.
    void set_raw_passthrough_only(bool enabled) noexcept;

    // Detailed IMAGE history serves capture/export diagnostics, but a live
    // renderer already receives every validated transfer synchronously through
    // GsCommandObserver. Disabling history avoids both the bounded duplicate
    // comparison and the drainable transfer queue without affecting observer
    // delivery. Existing behavior remains the default.
    void set_image_history_enabled(bool enabled) noexcept;

    // GIF_CTRL.RST discards all three GIF path cursors and any partial IMAGE
    // payload while retaining already-latched GS registers and completed
    // diagnostic history. CSR.RESET additionally clears those GS transfer
    // latches and emits one ordered renderer reset callback.
    void reset_gif_paths() noexcept;
    void reset_gs_state();

    void clear() noexcept;

    // Clears diagnostic/payload history and restores only the transport
    // latches needed by future, newly submitted packets. Active serialized
    // GIF paths and partial transfers are rejected without changing capture.
    [[nodiscard]] GsReplayStateRestoreStatus
    restore_idle_state(const GsReplayStateSeed& seed) noexcept;

  private:
    enum class GifPayloadKind : std::uint8_t {
        Tag,
        Packed,
        Reglist,
        Image,
    };

    // PATH1 (VU1 XGKICK), PATH2 (VIF DIRECT), and PATH3 (GIF DMA) may be
    // scheduled between each other. Their GIFtag payload cursors must remain
    // independent; only the actual GS register state is shared.
    struct GifPayloadState {
        GifPayloadKind kind{GifPayloadKind::Tag};
        std::uint64_t register_descriptors{};
        std::uint32_t payload_quadwords_remaining{};
        std::uint32_t register_count{};
        std::uint32_t register_index{};
        std::uint32_t register_values_remaining{};
        // IMAGE payload framing remains path-local, but the transfer itself is
        // GS-global. A generation prevents payload from a tag which was
        // interrupted by a later TRXDIR on another path from entering the new
        // transfer.
        std::uint64_t image_transfer_generation{};
    };

    struct HostToLocalTransferState {
        GsImageTransfer image{};
        std::size_t expected_bytes{};
        std::uint64_t generation{};
        bool active{};
    };

    void submit_gif_quadword_general(GifPath path, const GsQuadword& quadword);
    // Recomputes raw_only_observer_ after any observer or mode change.
    void refresh_raw_only_observer() noexcept;
    void begin_gif_tag(GifPath path, const GsQuadword& quadword);
    void consume_packed_quadword(GifPath path, const GsQuadword& quadword);
    void consume_reglist_quadword(GifPath path, const GsQuadword& quadword);
    void consume_image_quadword(GifPath path, const GsQuadword& quadword);
    void record_register_write(GifPath path, GsRegisterWriteSource source, std::uint8_t address,
                               std::uint64_t value, std::uint64_t packed_high = 0U);
    void apply_direct_register_write(GifPath path, GsRegisterWriteSource source,
                                     std::uint8_t address, std::uint64_t value,
                                     std::uint64_t packed_high = 0U);
    void apply_ad_write(GifPath path, const GsQuadword& quadword);
    void begin_host_to_local_transfer();
    void interrupt_host_to_local_transfer(bool diagnose_incomplete);
    void finish_image_transfer(GifPath path);
    void queue_completed_image_transfer(GsImageTransfer&& image);
    void notify_gif_packet_eop(GifPath path, bool packet_observed_from_start) noexcept;
    void synchronize_gif_packet_start(GifPath path) noexcept;
    void invalidate_open_gif_packet_continuity() noexcept;
    void reset_gif_payload(GifPayloadState& state) noexcept;
    [[nodiscard]] GifPayloadState& gif_payload_state(GifPath path) noexcept;

    std::array<std::uint32_t, 4> vif_direct_quadword_{};
    std::uint32_t vif_direct_words_remaining_{};
    std::size_t vif_direct_lane_{};
    bool vif_dma_active_{};
    bool vif_dma_desynchronized_{};

    std::array<GifPayloadState, 3> gif_payloads_{};
    // EeMemory owns authoritative GIF framing. These capture-local bits bind
    // one EOP notification to an uninterrupted raw and primary-command
    // observation session; configuration changes cannot authenticate a
    // packet prefix delivered under a different session.
    std::array<bool, 3> gif_packet_observation_open_{};
    std::array<bool, 3> gif_packet_primary_command_continuous_{};
    std::array<bool, 3> gif_packet_start_synchronized_{true, true, true};
    HostToLocalTransferState host_to_local_transfer_{};
    std::uint64_t next_image_transfer_generation_{1U};

    std::uint64_t gs_bitbltbuf_{};
    std::uint64_t gs_trxpos_{};
    std::uint64_t gs_trxreg_{};
    std::uint64_t gs_trxdir_{};
    std::uint64_t next_sequence_{1U};
    std::vector<GsRegisterWrite> register_writes_{};
    std::vector<GsImageTransfer> image_transfers_{};
    std::deque<GsImageTransfer> completed_image_transfers_{};
    std::size_t queued_image_bytes_{};
    std::vector<GsCaptureDiagnostic> diagnostics_{};
    GsCaptureActivity activity_{};
    GsCommandObserver* command_observer_{};
    GsRawGifObserver* raw_gif_observer_{};
    GsRawGifObserver* raw_gif_packet_eop_observer_{};
    GsCommandObserver* terminal_command_observer_{};
    GsRawGifObserver* terminal_raw_gif_observer_{};
    GsRawGifObserver* terminal_raw_gif_packet_eop_observer_{};
    GsCommandObserver* diagnostic_command_observer_{};
    GsRawGifObserver* diagnostic_raw_gif_observer_{};
    GsRawGifObserver* diagnostic_raw_gif_packet_eop_observer_{};
    // Non-null exactly when the only consumer of a GIF quadword is one raw
    // observer: raw passthrough is on, no observer tracks packet EOP and no
    // other raw observer is attached. submit_gif_quadword then reduces to one
    // flag store and one call (see the general path in gs_capture.cpp).
    GsRawGifObserver* raw_only_observer_{};
    bool gif_packet_eop_tracking_active_{};
    bool image_history_enabled_{true};
    bool raw_passthrough_only_{};
    std::shared_ptr<const bool> lifetime_token_{std::make_shared<const bool>(true)};

    friend class EeMemory;
};

[[nodiscard]] std::vector<std::uint8_t> encode_gs_image_ppm(const GsImageTransfer& image);
// Converts one complete, currently supported host-to-local transfer into tightly packed RGBA8.
// PSMCT32 preserves guest RGBA, PSMCT24 supplies opaque host alpha, and
// PSMCT16/PSMCT16S expand RGB5 plus their alpha bit. Indexed PSMT8/PSMT4
// transfers deliberately remain unsupported here: resolving them needs the
// GS CLUT, so enrichment is left to the rasterizer with its real registers.
[[nodiscard]] std::vector<std::uint8_t> decode_gs_image_rgba(const GsImageTransfer& image);
// PPM export accepts every supported transfer: PSMCT32/24/16/16S become a P6
// RGB image, while PSMT8/PSMT4 become a P5 index map (raw palette indices)
// so --capture-gs never aborts on font-atlas or sprite uploads.
void write_gs_image_ppm(const GsImageTransfer& image, const std::filesystem::path& output_path);

} // namespace takedown::guest
