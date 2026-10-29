#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace takedown::presentation {
enum class GsRendererWorkerCommandKind : std::uint8_t;
} // namespace takedown::presentation

namespace takedown::runtime {

struct Slus21050FemainTerminalProducerDiagnosticSnapshot;
struct Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult;
class Slus21050FemainFrameGraphicsSourceProbe;
class Slus21050FemainRetailConsumeScalarWriteObserver;

// A passive, bounded observation surface for the unresolved SLUS-210.50
// FEMain terminal graphics producer. It records the evidence which exists at
// the current seams (retail scalar stores, VIF1 source walks, PATH GIF words,
// decoded primary commands, and worker dispatch), but it never classifies a
// frame, creates a witness, or calls an owner/ledger. In particular, a packet
// with a clean EOP is still only a packet record here.
//
// The object is deliberately opt-in. Disabled objects return immediately from
// all callbacks and do not retain records. Enabled objects use only fixed
// arrays on callback paths; report file I/O happens in write_report().
class Slus21050FemainTerminalProducerDiagnostic final
    : public guest::GsRawGifObserver,
      public guest::GsCommandObserver,
      public guest::Vif1DmacChainObserver,
      public guest::Vif1Path2SourceMarkerObserver,
      public guest::EeMemoryWriteObserver {
  public:
    static constexpr std::size_t kMaximumPackets = 4096U;
    static constexpr std::size_t kMaximumChains = 1024U;
    static constexpr std::size_t kMaximumChainTags = 16384U;
    static constexpr std::size_t kMaximumMarkerCandidates = 4096U;
    static constexpr std::size_t kMaximumMarkerResolutions = 4096U;
    static constexpr std::size_t kMaximumMarkerEvents = 4096U;
    static constexpr std::size_t kMaximumScalarWrites = 8192U;
    // A 5M-step garage window retained 149,096 decoded primary callbacks while
    // reaching hundreds of retail scalar writes. Keep measured diagnostic-only
    // headroom for that discovery window without weakening the fail-closed
    // dropped-record gate.
    static constexpr std::size_t kMaximumPrimaryCommands = 196608U;
    static constexpr std::size_t kMaximumWorkerDispatches = 65536U;
    static constexpr std::size_t kMaximumInvalidations = 4096U;
    static constexpr std::size_t kMaximumPendingPrimaryCommands = 65536U;
    static constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

    enum class LifecycleReason : std::uint8_t {
        GifResetOrObserverChange,
        GsReset,
        MarkerCancelled,
        MemoryCleared,
        CaptureDisabledOrGap,
        ExplicitNote,
    };

    enum class SnapshotSealFailure : std::uint8_t {
        None,
        Disabled,
        GuestWorkScopeNotStarted,
        GuestWorkScopeIncomplete,
        CollectionOpen,
        ContextUnbound,
        ContextInvalid,
        PacketOpen,
        ChainOpen,
        DiscardedPacketPrefix,
        DiscardedChainPrefix,
        PendingMarker,
        PendingPrimary,
        EarlyWorkerDispatch,
        WorkerFenceMissing,
        WorkerHookMissing,
        WorkerMappingInvalid,
        DroppedRecords,
        InvalidationsPresent,
        ObservationEpochInvalid,
        CounterReordered,
        RecordEpochMismatch,
        SnapshotAllocationFailure,
    };

    enum class MarkerEventKind : std::uint8_t {
        Reached,
        Cancelled,
        MemoryCleared,
    };

    enum class PrimaryCommandKind : std::uint8_t {
        GifTagBegin,
        GsStateReset,
        RegisterWrite,
        ImageTransfer,
    };

    enum class ScalarField : std::uint8_t {
        Unknown,
        BufferIndex,
        TransfersActive,
        PendingEnd,
        QueueRead,
        QueueEnd,
        Occupancy,
        OccupancyLimit,
    };

    struct Context {
        // These values are caller-supplied labels only. The diagnostic never
        // treats them as proof of ownership or frame identity.
        std::uint64_t runtime_epoch{};
        std::uint64_t worker_epoch{};
        std::uint64_t player_session{};
        std::uint64_t owner_generation{};
        std::uint64_t frame_id{};
        std::uint64_t ledger_identity{};
        std::uint64_t ledger_session{};
    };

    struct PacketRecord {
        std::uint64_t ordinal{};
        std::uint64_t observation_epoch{};
        guest::GifPath path{};
        guest::GsQuadword first_quadword{};
        std::uint64_t raw_first_event{};
        std::uint64_t raw_last_event{};
        // One diagnostic-local ordering domain shared by raw GIF, decoded
        // primary, packet EOP, and retained scalar-write observations.
        std::uint64_t raw_first_timeline_event{};
        std::uint64_t raw_last_timeline_event{};
        std::uint64_t raw_quadwords{};
        std::uint64_t eop_event{};
        std::uint64_t eop_timeline_event{};
        std::uint64_t gif_tag_low{};
        std::uint64_t gif_tag_high{};
        std::uint32_t loop_count{};
        std::uint32_t register_count{};
        std::uint32_t primitive{};
        std::uint32_t format{};
        bool eop_in_tag{};
        bool pre_in_tag{};
        bool complete{};
        bool primary_command_stream_available{};
        bool invalidated{};
        bool marker_bound{};
        bool marker_quadword_match{};
        // Diagnostic-local raw event at which the bound marker's exact
        // quadword was observed. This is not the source probe's private raw
        // ordinal and must not be compared across those domains.
        std::uint64_t marker_raw_event{};
        std::uint64_t source_event_domain_identity{};
        std::uint64_t marker_shared_event{};
        std::uint64_t marker_raw_shared_event{};
        std::uint64_t eop_shared_event{};
        std::uint64_t marker_generation{};
        std::uint32_t marker_source{};
        std::uint32_t marker_chain_root{};
        std::uint32_t marker_tag_location{};
        std::uint32_t marker_tag_ordinal{};
        std::uint32_t marker_payload_word_ordinal{};
        std::uint64_t expected_payload_quadwords{};
        std::uint64_t primary_first_ordinal{};
        std::uint64_t primary_last_ordinal{};
        std::uint64_t primary_command_count{};
        std::uint64_t worker_first_serial{};
        std::uint64_t worker_last_serial{};
        std::uint64_t worker_dispatch_count{};
        bool worker_serial_range_contiguous{true};
    };

    struct ChainRecord {
        std::uint64_t sequence{};
        std::uint64_t observation_epoch{};
        std::uint32_t chain_root{};
        guest::GuestAddress start_pc{};
        guest::DmacSourceStop stop{guest::DmacSourceStop::TagOutOfRange};
        std::uint64_t tags_processed{};
        std::uint64_t payload_quadwords{};
        std::uint64_t first_tag_record{};
        std::uint64_t last_tag_record{};
        bool accepted{};
        bool complete{};
    };

    struct ChainTagRecord {
        std::uint64_t sequence{};
        std::uint64_t observation_epoch{};
        std::uint32_t chain_root{};
        std::uint32_t ordinal{};
        guest::DmacSourceTag tag{};
        guest::GuestAddress last_writer_pc{};
        bool last_writer_pc_known{};
    };

    struct MarkerCandidateRecord {
        std::uint64_t observation_epoch{};
        guest::Vif1DmacPreSnapshotObservation observation{};
        guest::Vif1DmacPreSnapshotCandidate candidate{};
    };

    struct MarkerResolutionRecord {
        std::uint64_t observation_epoch{};
        guest::Vif1DmacPreSnapshotResolvedMarker marker{};
        bool accepted{};
    };

    struct MarkerEventRecord {
        std::uint64_t observation_epoch{};
        std::uint64_t source_event_domain_identity{};
        std::uint64_t source_event_ordinal{};
        MarkerEventKind kind{MarkerEventKind::Reached};
        std::uint64_t generation{};
        std::uint32_t source{};
        std::uint32_t chain_root{};
        std::uint32_t source_tag_location{};
        std::uint32_t source_tag_ordinal{};
        std::uint32_t source_payload_word_ordinal{};
        guest::Vif1Path2SourceMarkerCancelReason cancel_reason{
            guest::Vif1Path2SourceMarkerCancelReason::ExplicitlyCancelled};
        guest::GsQuadword quadword{};
        bool has_quadword{};
    };

    struct ScalarWriteRecord {
        std::uint64_t observation_epoch{};
        std::uint64_t timeline_event{};
        std::uint64_t raw_events_observed{};
        std::uint64_t eop_events_observed{};
        std::uint64_t primary_events_observed{};
        ScalarField field{ScalarField::Unknown};
        guest::EeMemoryWriteEvent event{};
    };

    struct PrimaryCommandRecord {
        std::uint64_t ordinal{};
        std::uint64_t observation_epoch{};
        std::uint64_t timeline_event{};
        PrimaryCommandKind kind{PrimaryCommandKind::RegisterWrite};
        guest::GifPath path{};
        std::size_t packet_record{kNoIndex};
        std::uint64_t guest_sequence{};
        // Deterministic FNV-1a64 over the complete decoded command value.
        // IMAGE commands include their pixel bytes. This is retained only in
        // the immutable diagnostic snapshot for later exact-stream
        // correlation; the stable text-report schema remains unchanged.
        std::uint64_t command_digest{};
        std::uint64_t worker_serial{};
        bool worker_dispatched{};
    };

    struct WorkerDispatchRecord {
        std::uint64_t observation_epoch{};
        presentation::GsRendererWorkerCommandKind kind{};
        std::uint64_t serial{};
        std::thread::id thread{};
    };

    struct InvalidationRecord {
        std::uint64_t observation_epoch{};
        LifecycleReason reason{LifecycleReason::ExplicitNote};
        std::size_t active_packet_count{};
        std::thread::id thread{};
    };

    // One mutex-linearized counter view for correlation at an exact guest
    // instruction boundary. Individual accessors remain available for simple
    // reports, but combining them cannot establish a shared event instant.
    struct CounterSnapshot {
        std::uint64_t observation_epoch{};
        std::uint64_t timeline_events{};
        std::uint64_t raw_quadwords{};
        std::uint64_t packet_eop_events{};
        std::uint64_t primary_commands{};
        std::uint64_t worker_dispatches{};

        bool operator==(const CounterSnapshot&) const = default;
    };

    explicit Slus21050FemainTerminalProducerDiagnostic(
        bool enabled = false, bool worker_dispatch_expected = true) noexcept;
    // Defined out of line so the multiple-inheritance destructor thunks are
    // emitted in exactly one translation unit; an inline default here produces
    // duplicate COMDAT thunks that MinGW's LTO plugin rejects.
    ~Slus21050FemainTerminalProducerDiagnostic() override;

    Slus21050FemainTerminalProducerDiagnostic(
        const Slus21050FemainTerminalProducerDiagnostic&) = delete;
    Slus21050FemainTerminalProducerDiagnostic& operator=(
        const Slus21050FemainTerminalProducerDiagnostic&) = delete;
    Slus21050FemainTerminalProducerDiagnostic(
        Slus21050FemainTerminalProducerDiagnostic&&) = delete;
    Slus21050FemainTerminalProducerDiagnostic& operator=(
        Slus21050FemainTerminalProducerDiagnostic&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool terminal_authority_available() const noexcept { return false; }
    [[nodiscard]] std::thread::id observation_thread() const noexcept {
        return observation_thread_;
    }

    void set_context(const Context& context, bool bound) noexcept;
    [[nodiscard]] Context context() const noexcept;
    [[nodiscard]] bool context_bound() const noexcept;

    // Forwarding is explicit so an existing renderer, semantic trace, or
    // marker owner keeps receiving the same callbacks. The diagnostic never
    // forwards to itself and never takes ownership of the target.
    void set_raw_forward_observer(guest::GsRawGifObserver* observer) noexcept;
    void set_primary_forward_observer(guest::GsCommandObserver* observer) noexcept;
    void set_chain_forward_observer(guest::Vif1DmacChainObserver* observer) noexcept;
    void set_marker_forward_observer(guest::Vif1Path2SourceMarkerObserver* observer) noexcept;
    // Optional candidate-only sink. It receives the diagnostic's shared event
    // identity without occupying either generic raw or singleton marker slot.
    // Attach/detach only at a quiescent callback boundary; the probe must
    // outlive the attachment and must not also occupy the generic raw slot.
    void set_source_probe_forward(Slus21050FemainFrameGraphicsSourceProbe* probe) noexcept;
    [[nodiscard]] std::uint64_t source_event_domain_identity() const noexcept {
        return source_event_domain_identity_;
    }

    // The scalar tap has one EeMemory slot. Install only when it is vacant so
    // a pre-existing owner cannot be displaced; the caller must detach before
    // either object is destroyed. A false return is recorded as unavailable.
    [[nodiscard]] bool attach_scalar_write_observer(guest::EeMemory& memory) noexcept;
    [[nodiscard]] bool detach_scalar_write_observer() noexcept;

    // Tee relevant scalar writes to the bounded retail-consume adapter while
    // this diagnostic retains EeMemory's one observer slot. The target is
    // borrowed and must be detached at a quiescent callback boundary.
    void set_retail_consume_scalar_write_observer(
        Slus21050FemainRetailConsumeScalarWriteObserver* observer) noexcept;

    // Called by the optional GsRendererWorker dispatch hook. This records
    // worker serial/thread evidence and FIFO-matches only the primary command
    // kinds previously observed by this object. A match remains observational;
    // it does not certify execution as a frame terminal.
    void on_renderer_dispatch(presentation::GsRendererWorkerCommandKind kind,
                              std::uint64_t serial) noexcept;

    void note_lifecycle(LifecycleReason reason) noexcept;

    // Declares that the caller requested the optional guest-work scope. The
    // reset below marks A only after the shared measurement window accepts it;
    // completion is marked separately at the exact accepted C boundary.
    void request_guest_work_scope() noexcept;
    // An in-process worker fence may be supplied so its host-only synchronize
    // command is removed after proving every pre-C dispatch has completed.
    // The serialized caller must wait for synchronize() and stop guest-side
    // callbacks before this exact C-boundary call.
    void complete_guest_work_scope(std::uint64_t worker_fence_serial = 0U) noexcept;

    // Discard all startup observations and begin a fresh bounded collection.
    // The caller must invoke this at a quiescent guest-work boundary. The
    // forwarding observers, context, and EeMemory scalar attachment remain
    // installed so the tee continues without a lifecycle gap.
    void reset_for_guest_work() noexcept;

    [[nodiscard]] bool write_report(const std::filesystem::path& path) const;

    [[nodiscard]] std::span<const PacketRecord> packets() const noexcept;
    [[nodiscard]] std::span<const ChainRecord> chains() const noexcept;
    [[nodiscard]] std::span<const ChainTagRecord> chain_tags() const noexcept;
    [[nodiscard]] std::span<const MarkerCandidateRecord> marker_candidates() const noexcept;
    [[nodiscard]] std::span<const MarkerResolutionRecord> marker_resolutions() const noexcept;
    [[nodiscard]] std::span<const MarkerEventRecord> marker_events() const noexcept;
    [[nodiscard]] std::span<const ScalarWriteRecord> scalar_writes() const noexcept;
    [[nodiscard]] std::span<const PrimaryCommandRecord> primary_commands() const noexcept;
    [[nodiscard]] std::span<const WorkerDispatchRecord> worker_dispatches() const noexcept;
    [[nodiscard]] std::span<const InvalidationRecord> invalidations() const noexcept;

    [[nodiscard]] std::uint64_t observation_epoch() const noexcept;
    [[nodiscard]] std::uint64_t raw_quadwords_observed() const noexcept;
    [[nodiscard]] std::uint64_t primary_commands_observed() const noexcept;
    [[nodiscard]] std::uint64_t worker_dispatches_observed() const noexcept;
    [[nodiscard]] CounterSnapshot counter_snapshot() const noexcept;
    [[nodiscard]] bool worker_mapping_valid() const noexcept;
    [[nodiscard]] bool scalar_write_observer_unavailable() const noexcept;
    [[nodiscard]] std::uint64_t dropped_records() const noexcept;

    // Copy the closed guest-work collection while holding the diagnostic's
    // callback mutex. The returned value owns every retained record and may
    // outlive this diagnostic. This never creates terminal authority.
    [[nodiscard]] Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult
    seal_immutable_snapshot() const noexcept;

    // guest::GsRawGifObserver
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override { return true; }
    void on_gs_gif_quadword(guest::GifPath path,
                            const guest::GsQuadword& quadword) override;
    void on_gs_gif_packet_eop(guest::GifPath path,
                              bool primary_command_stream_available) noexcept override;
    void on_gs_gif_reset() noexcept override;

    // guest::GsCommandObserver
    void on_gs_gif_tag_begin(guest::GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const guest::GsRegisterWrite& write) override;
    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override;
    void on_gs_image_transfer(guest::GsImageTransfer&& transfer) override;

    // guest::Vif1DmacChainObserver
    [[nodiscard]] bool on_vif1_dmac_chain_begin(
        const guest::Vif1DmacChainObservation& observation) noexcept override;
    void on_vif1_dmac_chain_tag(
        const guest::Vif1DmacTagObservation& observation) noexcept override;
    void on_vif1_dmac_chain_end(
        const guest::Vif1DmacChainResultObservation& observation) noexcept override;

    // guest::Vif1Path2SourceMarkerObserver. Candidate callbacks are forwarded
    // byte-for-byte; this object never offers a candidate of its own.
    [[nodiscard]] bool has_vif1_dmac_pre_snapshot_candidates() const noexcept override;
    [[nodiscard]] std::size_t on_vif1_dmac_pre_snapshot_candidates(
        const guest::Vif1DmacPreSnapshotObservation& observation,
        std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept override;
    void on_vif1_dmac_pre_snapshot_markers_resolved(
        std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
        std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept override;
    void on_vif1_path2_source_marker_memory_cleared() noexcept override;
    void on_vif1_path2_source_marker_reached(
        const guest::Vif1Path2SourceMarkerReached& marker) noexcept override;
    void on_vif1_path2_source_marker_cancelled(
        const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept override;

    // guest::EeMemoryWriteObserver
    void on_ee_memory_write(const guest::EeMemoryWriteEvent& event) noexcept override;

  private:
    [[nodiscard]] bool recording_enabled() const noexcept {
        return enabled_ && !collection_closed_.load(std::memory_order_relaxed);
    }

    struct PendingPrimary {
        PrimaryCommandKind kind{PrimaryCommandKind::RegisterWrite};
        std::uint64_t ordinal{};
        std::size_t primary_record{kNoIndex};
        std::size_t packet_record{kNoIndex};
        bool retain_worker_dispatch{true};
    };

    [[nodiscard]] std::size_t active_packet_for(guest::GifPath path) const noexcept;
    [[nodiscard]] std::size_t ensure_packet(guest::GifPath path,
                                             const guest::GsQuadword& quadword,
                                             std::uint64_t raw_event,
                                             std::uint64_t timeline_event);
    void record_primary(PrimaryCommandKind kind, guest::GifPath path,
                        std::uint64_t guest_sequence, std::uint64_t command_digest);
    void record_primary_without_path(PrimaryCommandKind kind,
                                     std::uint64_t command_digest);
    [[nodiscard]] std::uint64_t next_timeline_event_locked() noexcept;
    void enqueue_or_match_primary_locked(const PendingPrimary& pending) noexcept;
    void associate_worker_serial_locked(const PendingPrimary& pending,
                                        std::uint64_t serial) noexcept;
    void discard_worker_dispatch_record_locked(std::size_t record) noexcept;
    void invalidate_locked(LifecycleReason reason) noexcept;
    void bind_pending_marker_locked(std::size_t packet_record) noexcept;
    void record_marker_event_locked(const MarkerEventRecord& event) noexcept;
    [[nodiscard]] static ScalarField scalar_field_for(guest::GuestAddress address) noexcept;
    [[nodiscard]] static bool is_retail_scalar_address(guest::GuestAddress address) noexcept;
    [[nodiscard]] static bool is_primary_kind(
        presentation::GsRendererWorkerCommandKind kind) noexcept;
    [[nodiscard]] static PrimaryCommandKind primary_kind_for(
        presentation::GsRendererWorkerCommandKind kind) noexcept;

    bool enabled_{};
    // External parallel-GS runs have decoded primary callbacks but no native
    // renderer worker. Do not fill a correlation FIFO that can never drain.
    bool worker_dispatch_expected_{true};
    std::atomic<bool> collection_closed_{false};
    std::thread::id observation_thread_{};
    // Every callback uses this short critical section to serialize its
    // fixed-capacity record update. Contention waits for the bounded update
    // instead of dropping worker or lifecycle evidence.
    mutable std::mutex mutex_{};
    std::atomic<std::uint64_t> dropped_records_{0U};

    Context context_{};
    bool context_bound_{};
    std::atomic<bool> worker_mapping_valid_{true};
    std::atomic<bool> worker_dispatch_hook_seen_{false};
    bool scalar_write_observer_unavailable_{};
    bool scalar_write_observer_attached_{};
    guest::EeMemory* scalar_memory_{};

    guest::GsRawGifObserver* raw_forward_{};
    guest::GsCommandObserver* primary_forward_{};
    guest::Vif1DmacChainObserver* chain_forward_{};
    guest::Vif1Path2SourceMarkerObserver* marker_forward_{};
    Slus21050FemainFrameGraphicsSourceProbe* source_probe_forward_{};
    Slus21050FemainRetailConsumeScalarWriteObserver* retail_consume_scalar_forward_{};

    std::uint64_t observation_epoch_{1U};
    std::uint64_t source_event_domain_identity_{};
    std::uint64_t source_event_ordinal_{};
    std::uint64_t timeline_event_ordinal_{};
    std::uint64_t raw_event_ordinal_{};
    std::uint64_t eop_event_ordinal_{};
    std::uint64_t packet_ordinal_{};
    std::uint64_t chain_sequence_{};
    std::uint64_t primary_ordinal_{};
    std::uint64_t worker_dispatch_count_{};
    std::uint64_t timeline_scope_start_{};
    std::uint64_t raw_scope_start_{};
    std::uint64_t eop_scope_start_{};
    std::uint64_t primary_scope_start_{};
    std::uint64_t highest_marker_generation_observed_{};
    std::uint64_t marker_generation_floor_{};
    std::size_t active_chain_record_{kNoIndex};
    std::array<std::size_t, 3U> active_packets_{kNoIndex, kNoIndex, kNoIndex};
    // Parser activity is independent of bounded record retention. These bits
    // remain authoritative even after a record array fills and its index is
    // unavailable.
    std::array<bool, 3U> packet_open_{};
    bool chain_open_{};
    // A guest-work boundary can fall between the tag and EOP of a packet
    // submitted before A. Keep consuming that raw prefix until its EOP so a
    // payload quadword cannot be mistaken for a new tag in the fresh window.
    std::array<bool, 3U> discard_packet_prefix_after_reset_{};
    bool discard_chain_prefix_after_reset_{};
    bool guest_work_scope_requested_{};
    bool guest_work_scope_started_{};
    bool guest_work_scope_completed_{};
    bool guest_work_scope_worker_fence_completed_{};
    std::uint64_t guest_work_scope_worker_fence_serial_{};
    std::size_t pending_marker_event_{kNoIndex};

    std::array<PacketRecord, kMaximumPackets> packets_{};
    std::size_t packet_count_{};
    std::array<ChainRecord, kMaximumChains> chains_{};
    std::size_t chain_count_{};
    std::array<ChainTagRecord, kMaximumChainTags> chain_tags_{};
    std::size_t chain_tag_count_{};
    std::array<MarkerCandidateRecord, kMaximumMarkerCandidates> marker_candidates_{};
    std::size_t marker_candidate_count_{};
    std::array<MarkerResolutionRecord, kMaximumMarkerResolutions> marker_resolutions_{};
    std::size_t marker_resolution_count_{};
    std::array<MarkerEventRecord, kMaximumMarkerEvents> marker_events_{};
    std::size_t marker_event_count_{};
    std::array<ScalarWriteRecord, kMaximumScalarWrites> scalar_writes_{};
    std::size_t scalar_write_count_{};
    std::array<PrimaryCommandRecord, kMaximumPrimaryCommands> primary_commands_{};
    std::size_t primary_command_count_{};
    std::array<WorkerDispatchRecord, kMaximumWorkerDispatches> worker_dispatches_{};
    std::size_t worker_dispatch_record_count_{};
    std::array<InvalidationRecord, kMaximumInvalidations> invalidations_{};
    std::size_t invalidation_count_{};

    std::array<PendingPrimary, kMaximumPendingPrimaryCommands> pending_primary_{};
    std::size_t pending_primary_head_{};
    std::size_t pending_primary_count_{};
    // The worker can execute a command after the primary observer enqueues it
    // but before GsCapture reaches its diagnostic observer slot. Retain that
    // short-lived prefix so the serial mapping remains FIFO without requiring
    // the renderer or GsCapture to block on this diagnostic.
    struct EarlyWorkerDispatch {
        presentation::GsRendererWorkerCommandKind kind{};
        std::uint64_t serial{};
        std::size_t record{kNoIndex};
    };
    std::array<EarlyWorkerDispatch, kMaximumPendingPrimaryCommands>
        early_worker_dispatches_{};
    std::size_t early_worker_dispatch_head_{};
    std::size_t early_worker_dispatch_count_{};
};

// A value-only copy of one fully closed diagnostic collection. The vectors are
// owned so a later consumer cannot race the live fixed-capacity arrays.
struct Slus21050FemainTerminalProducerDiagnosticSnapshot {
    using Diagnostic = Slus21050FemainTerminalProducerDiagnostic;

    // These remain caller-supplied labels, copied for correlation only. A
    // future builder must derive authority from live serialized participants.
    Diagnostic::Context context{};
    bool context_bound{};
    std::thread::id observation_thread{};
    std::uint64_t observation_epoch{};
    std::uint64_t timeline_events_observed{};
    std::uint64_t raw_quadwords_observed{};
    std::uint64_t packet_eop_events_observed{};
    std::uint64_t primary_commands_observed{};
    std::uint64_t timeline_events_in_scope{};
    std::uint64_t raw_quadwords_in_scope{};
    std::uint64_t packet_eop_events_in_scope{};
    std::uint64_t primary_commands_in_scope{};
    std::uint64_t worker_dispatches_observed{};
    bool worker_dispatch_expected{};
    bool worker_mapping_valid{};
    bool guest_work_scope_requested{};
    bool guest_work_scope_started{};
    bool guest_work_scope_completed{};
    bool guest_work_scope_worker_fence_completed{};
    std::uint64_t guest_work_scope_worker_fence_serial{};
    bool collection_closed{};
    std::uint64_t dropped_records{};

    std::vector<Diagnostic::PacketRecord> packets{};
    std::vector<Diagnostic::ChainRecord> chains{};
    std::vector<Diagnostic::ChainTagRecord> chain_tags{};
    std::vector<Diagnostic::MarkerCandidateRecord> marker_candidates{};
    std::vector<Diagnostic::MarkerResolutionRecord> marker_resolutions{};
    std::vector<Diagnostic::MarkerEventRecord> marker_events{};
    std::vector<Diagnostic::ScalarWriteRecord> scalar_writes{};
    std::vector<Diagnostic::PrimaryCommandRecord> primary_commands{};
    std::vector<Diagnostic::WorkerDispatchRecord> worker_dispatches{};
    std::vector<Diagnostic::InvalidationRecord> invalidations{};
};

struct Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult {
    using Failure = Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure;

    Failure failure{Failure::CollectionOpen};
    std::optional<Slus21050FemainTerminalProducerDiagnosticSnapshot> snapshot{};

    [[nodiscard]] bool sealed() const noexcept {
        return failure == Failure::None && snapshot.has_value();
    }
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure failure) noexcept;

} // namespace takedown::runtime
