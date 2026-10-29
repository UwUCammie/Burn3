#include "takedown/runtime/slus_21050_femain_terminal_producer_diagnostic.hpp"

#include "takedown/presentation/gs_renderer_worker.hpp"

#include <fstream>
#include <functional>
#include <iomanip>
#include <string_view>
#include "takedown/runtime/slus_21050_femain_graphics_source_probe.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_observation_adapter.hpp"

namespace takedown::runtime {
namespace {

// Counters saturate instead of wrapping so an ordering comparison stays valid.
void saturating_increment(unsigned long long& value) noexcept {
    if (value != std::numeric_limits<unsigned long long>::max()) {
        ++value;
    }
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kInvalidDigest = std::numeric_limits<std::uint64_t>::max();

void mix_word(unsigned long long& hash, const std::uint64_t value) noexcept {
    hash = (hash ^ value) * kFnvPrime;
}

template <typename Integer>
void mix_integer(unsigned long long& hash, const Integer value) noexcept {
    for (std::size_t shift = 0U; shift != sizeof(Integer) * 8U; shift += 8U) {
        mix_word(hash, (value >> shift) & 255U);
    }
}

void mix_domain(unsigned long long& hash, const std::string_view domain) noexcept {
    for (const char byte : domain) {
        mix_word(hash, static_cast<std::uint8_t>(byte));
    }
}

[[nodiscard]] std::uint64_t finish(const std::uint64_t hash) noexcept {
    return hash != 0U ? hash : kInvalidDigest;
}

[[nodiscard]] std::uint64_t image_transfer_digest(const guest::GsImageTransfer& transfer) noexcept {
    auto hash = kFnvOffsetBasis;
    mix_domain(hash, "slus-21050-femain-primary-image-transfer-v1");
    mix_integer(hash, transfer.sequence);
    mix_word(hash, static_cast<std::uint64_t>(transfer.path));
    mix_integer(hash, transfer.destination_base_pointer);
    mix_integer(hash, transfer.destination_buffer_width);
    mix_integer(hash, transfer.destination_psm);
    mix_integer(hash, transfer.destination_x);
    mix_integer(hash, transfer.destination_y);
    mix_integer(hash, transfer.width);
    mix_integer(hash, transfer.height);
    mix_integer(hash, transfer.pixels.size());
    for (const unsigned char byte : transfer.pixels) {
        mix_word(hash, byte);
    }
    return finish(hash);
}

[[nodiscard]] std::uint64_t register_write_digest(const guest::GsRegisterWrite& write) noexcept {
    auto hash = kFnvOffsetBasis;
    mix_domain(hash, "slus-21050-femain-primary-register-write-v1");
    mix_integer(hash, write.sequence);
    mix_word(hash, static_cast<std::uint64_t>(write.path));
    mix_word(hash, static_cast<std::uint64_t>(write.source));
    mix_word(hash, write.address);
    mix_integer(hash, write.value);
    mix_integer(hash, write.packed_high);
    return finish(hash);
}

} // namespace

namespace {

[[nodiscard]] std::string_view stop_name(const guest::DmacSourceStop stop) noexcept {
    switch (stop) {
    case guest::DmacSourceStop::Refe:
        return "refe";
    case guest::DmacSourceStop::End:
        return "end";
    case guest::DmacSourceStop::RetEmpty:
        return "ret-empty";
    case guest::DmacSourceStop::TieIrq:
        return "tie-irq";
    case guest::DmacSourceStop::TagOutOfRange:
        return "tag-out-of-range";
    case guest::DmacSourceStop::PayloadOutOfRange:
        return "payload-out-of-range";
    case guest::DmacSourceStop::AddressOverflow:
        return "address-overflow";
    case guest::DmacSourceStop::TagLimit:
        return "tag-limit";
    case guest::DmacSourceStop::QuadwordLimit:
        return "quadword-limit";
    case guest::DmacSourceStop::CallStackOverflow:
        return "call-stack-overflow";
    case guest::DmacSourceStop::CursorLoop:
        return "cursor-loop";
    }
    return "unknown";
}

using Diagnostic = Slus21050FemainTerminalProducerDiagnostic;

[[nodiscard]] std::string_view marker_event_kind_name(const Diagnostic::MarkerEventKind kind) noexcept {
    switch (kind) {
    case Diagnostic::MarkerEventKind::Reached:
        return "reached";
    case Diagnostic::MarkerEventKind::Cancelled:
        return "cancelled";
    case Diagnostic::MarkerEventKind::MemoryCleared:
        return "memory-cleared";
    }
    return "unknown";
}

[[nodiscard]] std::string_view scalar_field_name(const Diagnostic::ScalarField field) noexcept {
    switch (field) {
    case Diagnostic::ScalarField::Unknown:
        return "unknown";
    case Diagnostic::ScalarField::BufferIndex:
        return "buffer-index";
    case Diagnostic::ScalarField::TransfersActive:
        return "transfers-active";
    case Diagnostic::ScalarField::PendingEnd:
        return "pending-end";
    case Diagnostic::ScalarField::QueueRead:
        return "queue-read";
    case Diagnostic::ScalarField::QueueEnd:
        return "queue-end";
    case Diagnostic::ScalarField::Occupancy:
        return "occupancy";
    case Diagnostic::ScalarField::OccupancyLimit:
        return "occupancy-limit";
    }
    return "unknown";
}

[[nodiscard]] std::string_view primary_kind_name(const Diagnostic::PrimaryCommandKind kind) noexcept {
    switch (kind) {
    case Diagnostic::PrimaryCommandKind::GifTagBegin:
        return "gif-tag-begin";
    case Diagnostic::PrimaryCommandKind::GsStateReset:
        return "gs-state-reset";
    case Diagnostic::PrimaryCommandKind::RegisterWrite:
        return "register-write";
    case Diagnostic::PrimaryCommandKind::ImageTransfer:
        return "image-transfer";
    }
    return "unknown";
}

[[nodiscard]] std::string_view worker_command_kind_name(
    const presentation::GsRendererWorkerCommandKind kind) noexcept {
    using Kind = presentation::GsRendererWorkerCommandKind;
    switch (kind) {
    case Kind::RegisterWrite:
        return "register-write";
    case Kind::ImageTransfer:
        return "image-transfer";
    case Kind::GifTagBegin:
        return "gif-tag-begin";
    case Kind::GsStateReset:
        return "gs-state-reset";
    case Kind::Reset:
        return "reset";
    case Kind::AcquireTextureResidency:
        return "acquire-texture-residency";
    case Kind::ReleaseTextureResidency:
        return "release-texture-residency";
    case Kind::NativeUiTextDraw:
        return "native-ui-text-draw";
    case Kind::Synchronize:
        return "synchronize";
    case Kind::FrameCompletion:
        return "frame-completion";
    }
    return "unknown";
}

[[nodiscard]] std::string_view lifecycle_reason_name(
    const Diagnostic::LifecycleReason reason) noexcept {
    switch (reason) {
    case Diagnostic::LifecycleReason::GifResetOrObserverChange:
        return "gif-reset-or-observer-change";
    case Diagnostic::LifecycleReason::GsReset:
        return "csr-reset";
    case Diagnostic::LifecycleReason::MarkerCancelled:
        return "marker-cancelled";
    case Diagnostic::LifecycleReason::MemoryCleared:
        return "memory-cleared";
    case Diagnostic::LifecycleReason::CaptureDisabledOrGap:
        return "capture-disabled-or-gap";
    case Diagnostic::LifecycleReason::ExplicitNote:
        return "explicit-note";
    }
    return "unknown";
}

} // namespace

void Slus21050FemainTerminalProducerDiagnostic::on_gs_gif_quadword(
    const guest::GifPath path, const guest::GsQuadword& quadword) {
    if (!recording_enabled()) {
        if (raw_forward_ != nullptr) {
            raw_forward_->on_gs_gif_quadword(path, quadword);
        }
        return;
    }
    Slus21050FemainSourceDiagnosticEventIdentity shared_event{};
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (source_probe_forward_ != nullptr) {
            if (source_event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
            } else {
                shared_event = Slus21050FemainSourceDiagnosticEventIdentity{
                    source_event_domain_identity_, observation_epoch_, ++source_event_ordinal_,
                    Slus21050FemainSourceDiagnosticEventKind::RawSubmission};
            }
        }
        const std::uint64_t timeline_event = next_timeline_event_locked();
        saturating_increment(raw_event_ordinal_);
        const auto path_index = static_cast<std::size_t>(path);
        if (path_index >= discard_packet_prefix_after_reset_.size() ||
            !discard_packet_prefix_after_reset_[path_index]) {
            const auto packet_record =
                ensure_packet(path, quadword, raw_event_ordinal_, timeline_event);
            if (packet_record != kNoIndex && packet_record < packet_count_) {
                PacketRecord& packet = packets_[packet_record];
                if (packet.raw_first_event != raw_event_ordinal_) {
                    saturating_increment(packet.raw_quadwords);
                }
                packet.raw_last_event = raw_event_ordinal_;
                packet.raw_last_timeline_event = timeline_event;
                if (!packet.marker_bound && path == guest::GifPath::Path2 &&
                    pending_marker_event_ != kNoIndex &&
                    pending_marker_event_ < marker_event_count_) {
                    const MarkerEventRecord& event = marker_events_[pending_marker_event_];
                    if (event.kind == MarkerEventKind::Reached) {
                        packet.marker_bound = true;
                        packet.marker_quadword_match = event.quadword == quadword;
                        packet.marker_raw_event = raw_event_ordinal_;
                        packet.source_event_domain_identity = event.source_event_domain_identity;
                        packet.marker_shared_event = event.source_event_ordinal;
                        packet.marker_raw_shared_event = source_event_ordinal_;
                        packet.marker_generation = event.generation;
                        packet.marker_source = event.source;
                        packet.marker_chain_root = event.chain_root;
                        packet.marker_tag_location = event.source_tag_location;
                        packet.marker_tag_ordinal = event.source_tag_ordinal;
                        packet.marker_payload_word_ordinal = event.source_payload_word_ordinal;
                        pending_marker_event_ = kNoIndex;
                    }
                }
            }
        }
    }
    if (shared_event.valid() && source_probe_forward_ != nullptr) {
        static_cast<void>(
            source_probe_forward_->observe_diagnostic_raw_quadword(path, quadword, shared_event));
    }
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_quadword(path, quadword);
    }
}

namespace {

// Each enabled diagnostic gets a process-unique, non-zero event-domain label so
// its shared event ordinals are never compared with another probe's domain.
[[nodiscard]] std::uint64_t allocate_source_event_domain_identity() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    const std::uint64_t identity = next.fetch_add(1U, std::memory_order_relaxed);
    return identity != 0U ? identity : std::numeric_limits<std::uint64_t>::max();
}

} // namespace

Slus21050FemainTerminalProducerDiagnostic::Slus21050FemainTerminalProducerDiagnostic(
    const bool enabled, const bool worker_dispatch_expected) noexcept
    : enabled_(enabled),
      worker_dispatch_expected_(worker_dispatch_expected),
      observation_thread_(std::this_thread::get_id()),
      source_event_domain_identity_(enabled ? allocate_source_event_domain_identity() : 0U) {}

Slus21050FemainTerminalProducerDiagnostic::~Slus21050FemainTerminalProducerDiagnostic() = default;

std::string_view to_string(
    const Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure failure) noexcept {
    using Failure = Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure;
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::Disabled:
        return "disabled";
    case Failure::GuestWorkScopeNotStarted:
        return "guest-work-scope-not-started";
    case Failure::GuestWorkScopeIncomplete:
        return "guest-work-scope-incomplete";
    case Failure::CollectionOpen:
        return "collection-open";
    case Failure::ContextUnbound:
        return "context-unbound";
    case Failure::ContextInvalid:
        return "context-invalid";
    case Failure::PacketOpen:
        return "packet-open";
    case Failure::ChainOpen:
        return "chain-open";
    case Failure::DiscardedPacketPrefix:
        return "discarded-packet-prefix";
    case Failure::DiscardedChainPrefix:
        return "discarded-chain-prefix";
    case Failure::PendingMarker:
        return "pending-marker";
    case Failure::PendingPrimary:
        return "pending-primary";
    case Failure::EarlyWorkerDispatch:
        return "early-worker-dispatch";
    case Failure::WorkerFenceMissing:
        return "worker-fence-missing";
    case Failure::WorkerHookMissing:
        return "worker-hook-missing";
    case Failure::WorkerMappingInvalid:
        return "worker-mapping-invalid";
    case Failure::DroppedRecords:
        return "dropped-records";
    case Failure::InvalidationsPresent:
        return "invalidations-present";
    case Failure::ObservationEpochInvalid:
        return "observation-epoch-invalid";
    case Failure::CounterReordered:
        return "counter-reordered";
    case Failure::RecordEpochMismatch:
        return "record-epoch-mismatch";
    case Failure::SnapshotAllocationFailure:
        return "snapshot-allocation-failure";
    }
    return "unknown";
}

void Slus21050FemainTerminalProducerDiagnostic::set_context(const Context& context,
                                                            const bool bound) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    context_ = context;
    context_bound_ = bound;
}

Slus21050FemainTerminalProducerDiagnostic::Context
Slus21050FemainTerminalProducerDiagnostic::context() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return context_;
}

bool Slus21050FemainTerminalProducerDiagnostic::context_bound() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return context_bound_;
}

void Slus21050FemainTerminalProducerDiagnostic::set_raw_forward_observer(
    guest::GsRawGifObserver* const observer) noexcept {
    if (observer != static_cast<guest::GsRawGifObserver*>(this)) {
        raw_forward_ = observer;
    }
}

void Slus21050FemainTerminalProducerDiagnostic::set_primary_forward_observer(
    guest::GsCommandObserver* const observer) noexcept {
    if (observer != static_cast<guest::GsCommandObserver*>(this)) {
        primary_forward_ = observer;
    }
}

void Slus21050FemainTerminalProducerDiagnostic::set_chain_forward_observer(
    guest::Vif1DmacChainObserver* const observer) noexcept {
    if (observer != static_cast<guest::Vif1DmacChainObserver*>(this)) {
        chain_forward_ = observer;
    }
}

void Slus21050FemainTerminalProducerDiagnostic::set_marker_forward_observer(
    guest::Vif1Path2SourceMarkerObserver* const observer) noexcept {
    if (observer != static_cast<guest::Vif1Path2SourceMarkerObserver*>(this)) {
        marker_forward_ = observer;
    }
}

void Slus21050FemainTerminalProducerDiagnostic::set_source_probe_forward(
    Slus21050FemainFrameGraphicsSourceProbe* const probe) noexcept {
    source_probe_forward_ = probe;
}

bool Slus21050FemainTerminalProducerDiagnostic::attach_scalar_write_observer(
    guest::EeMemory& memory) noexcept {
    if (!enabled_) {
        return false;
    }
    if (memory.ee_memory_write_observer() != nullptr) {
        const std::lock_guard<std::mutex> lock(mutex_);
        scalar_write_observer_unavailable_ = true;
        return false;
    }
    memory.set_ee_memory_write_observer(this);
    const std::lock_guard<std::mutex> lock(mutex_);
    scalar_memory_ = &memory;
    scalar_write_observer_attached_ = true;
    return true;
}

bool Slus21050FemainTerminalProducerDiagnostic::detach_scalar_write_observer() noexcept {
    guest::EeMemory* memory = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        memory = scalar_memory_;
    }
    if (memory == nullptr) {
        return false;
    }
    if (memory->ee_memory_write_observer() != this) {
        const std::lock_guard<std::mutex> lock(mutex_);
        scalar_memory_ = nullptr;
        scalar_write_observer_attached_ = false;
        return false;
    }
    memory->set_ee_memory_write_observer(nullptr);
    const std::lock_guard<std::mutex> lock(mutex_);
    scalar_memory_ = nullptr;
    scalar_write_observer_attached_ = false;
    return true;
}

void Slus21050FemainTerminalProducerDiagnostic::set_retail_consume_scalar_write_observer(
    Slus21050FemainRetailConsumeScalarWriteObserver* const observer) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    retail_consume_scalar_forward_ = observer;
}

void Slus21050FemainTerminalProducerDiagnostic::note_lifecycle(
    const LifecycleReason reason) noexcept {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        invalidate_locked(reason);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::request_guest_work_scope() noexcept {
    if (!enabled_) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    guest_work_scope_requested_ = true;
}

void Slus21050FemainTerminalProducerDiagnostic::complete_guest_work_scope(
    const std::uint64_t worker_fence_serial) noexcept {
    if (!enabled_) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (guest_work_scope_completed_) {
        return;
    }
    if (guest_work_scope_requested_ && guest_work_scope_started_) {
        guest_work_scope_worker_fence_completed_ = !worker_dispatch_expected_;
        guest_work_scope_worker_fence_serial_ = 0U;
        if (worker_fence_serial != 0U) {
            std::size_t index = worker_dispatch_record_count_;
            std::size_t fence_record = kNoIndex;
            while (index != 0U) {
                const auto candidate = index - 1U;
                if (worker_dispatches_[candidate].serial == worker_fence_serial) {
                    if (worker_dispatches_[candidate].kind ==
                        presentation::GsRendererWorkerCommandKind::Synchronize) {
                        fence_record = candidate;
                        break;
                    }
                }
                index = candidate;
            }
            if (fence_record != kNoIndex) {
                discard_worker_dispatch_record_locked(fence_record);
                guest_work_scope_worker_fence_completed_ = true;
                guest_work_scope_worker_fence_serial_ = worker_fence_serial;
            }
        }
        guest_work_scope_completed_ = true;
        collection_closed_.store(true, std::memory_order_relaxed);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::reset_for_guest_work() noexcept {
    if (!enabled_) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    collection_closed_.store(false, std::memory_order_relaxed);
    saturating_increment(observation_epoch_);
    for (std::size_t index = 0U; index < discard_packet_prefix_after_reset_.size(); ++index) {
        discard_packet_prefix_after_reset_[index] =
            discard_packet_prefix_after_reset_[index] || packet_open_[index];
    }
    discard_chain_prefix_after_reset_ = discard_chain_prefix_after_reset_ || chain_open_;
    guest_work_scope_requested_ = true;
    guest_work_scope_started_ = true;
    guest_work_scope_completed_ = false;
    guest_work_scope_worker_fence_completed_ = false;
    guest_work_scope_worker_fence_serial_ = 0U;
    raw_scope_start_ = raw_event_ordinal_;
    eop_scope_start_ = eop_event_ordinal_;
    primary_scope_start_ = primary_ordinal_;
    timeline_scope_start_ = timeline_event_ordinal_;
    marker_generation_floor_ = highest_marker_generation_observed_;
    worker_dispatch_count_ = 0U;
    active_chain_record_ = kNoIndex;
    active_packets_.fill(kNoIndex);
    pending_marker_event_ = kNoIndex;
    packet_count_ = 0U;
    chain_count_ = 0U;
    chain_tag_count_ = 0U;
    marker_candidate_count_ = 0U;
    marker_resolution_count_ = 0U;
    marker_event_count_ = 0U;
    scalar_write_count_ = 0U;
    primary_command_count_ = 0U;
    worker_dispatch_record_count_ = 0U;
    invalidation_count_ = 0U;
    pending_primary_head_ = 0U;
    pending_primary_count_ = 0U;
    early_worker_dispatch_head_ = 0U;
    early_worker_dispatch_count_ = 0U;
    packets_.fill(PacketRecord{});
    chains_.fill(ChainRecord{});
    chain_tags_.fill(ChainTagRecord{});
    marker_candidates_.fill(MarkerCandidateRecord{});
    marker_resolutions_.fill(MarkerResolutionRecord{});
    marker_events_.fill(MarkerEventRecord{});
    scalar_writes_.fill(ScalarWriteRecord{});
    primary_commands_.fill(PrimaryCommandRecord{});
    worker_dispatches_.fill(WorkerDispatchRecord{});
    invalidations_.fill(InvalidationRecord{});
    pending_primary_.fill(PendingPrimary{});
    early_worker_dispatches_.fill(EarlyWorkerDispatch{});
    dropped_records_.store(0U, std::memory_order_relaxed);
    worker_mapping_valid_.store(true, std::memory_order_relaxed);
    worker_dispatch_hook_seen_.store(false, std::memory_order_relaxed);
}

std::size_t Slus21050FemainTerminalProducerDiagnostic::active_packet_for(
    const guest::GifPath path) const noexcept {
    const auto index = static_cast<std::size_t>(path);
    return index < active_packets_.size() ? active_packets_[index] : kNoIndex;
}

namespace {

// Number of payload quadwords a GIF tag promises for its FLG format.
[[nodiscard]] std::uint64_t expected_payload_quadwords(const std::uint32_t format,
                                                       const std::uint32_t loop_count,
                                                       const std::uint32_t register_count) noexcept {
    const auto values = static_cast<std::uint64_t>(loop_count) * register_count;
    switch (format) {
    case 0U:
        return values;
    case 1U:
        return (values + 1U) / 2U;
    case 2U:
    case 3U:
        return loop_count;
    default:
        return 0U;
    }
}

} // namespace

std::size_t Slus21050FemainTerminalProducerDiagnostic::ensure_packet(
    const guest::GifPath path, const guest::GsQuadword& quadword, const std::uint64_t raw_event,
    const std::uint64_t timeline_event) {
    const auto path_index = static_cast<std::size_t>(path);
    if (path_index >= packet_open_.size()) {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        return kNoIndex;
    }
    if (packet_open_[path_index]) {
        return active_packets_[path_index];
    }
    packet_open_[path_index] = true;
    if (packet_count_ >= kMaximumPackets) {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        return kNoIndex;
    }
    const std::size_t record_index = packet_count_++;
    PacketRecord& packet = packets_[record_index];
    packet = PacketRecord{};
    saturating_increment(packet_ordinal_);
    packet.ordinal = packet_ordinal_;
    packet.observation_epoch = observation_epoch_;
    packet.path = path;
    packet.first_quadword = quadword;
    packet.raw_first_event = raw_event;
    packet.raw_last_event = raw_event;
    packet.raw_first_timeline_event = timeline_event;
    packet.raw_last_timeline_event = timeline_event;
    packet.raw_quadwords = 1U;
    packet.gif_tag_low = static_cast<std::uint64_t>(quadword[0]) |
                         (static_cast<std::uint64_t>(quadword[1]) << 32U);
    packet.gif_tag_high = static_cast<std::uint64_t>(quadword[2]) |
                          (static_cast<std::uint64_t>(quadword[3]) << 32U);
    packet.loop_count = quadword[0] & 0x7FFFU;
    packet.eop_in_tag = (packet.gif_tag_low & 0x8000U) != 0U;
    packet.pre_in_tag = (packet.gif_tag_low & (1ULL << 46U)) != 0U;
    packet.primitive = static_cast<std::uint32_t>(packet.gif_tag_low >> 47U) & 0x7FFU;
    packet.format = static_cast<std::uint32_t>(packet.gif_tag_low >> 58U) & 0x3U;
    packet.register_count = static_cast<std::uint32_t>(packet.gif_tag_low >> 60U);
    if (packet.register_count == 0U) {
        packet.register_count = 16U;
    }
    packet.expected_payload_quadwords =
        expected_payload_quadwords(packet.format, packet.loop_count, packet.register_count);
    active_packets_[path_index] = record_index;
    bind_pending_marker_locked(record_index);
    return record_index;
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::next_timeline_event_locked() noexcept {
    if (timeline_event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    return ++timeline_event_ordinal_;
}

void Slus21050FemainTerminalProducerDiagnostic::bind_pending_marker_locked(
    const std::size_t packet_record) noexcept {
    if (pending_marker_event_ == kNoIndex || pending_marker_event_ >= marker_event_count_ ||
        packet_record >= packet_count_) {
        return;
    }
    const MarkerEventRecord& event = marker_events_[pending_marker_event_];
    if (event.kind != MarkerEventKind::Reached) {
        return;
    }
    if (packets_[packet_record].path != guest::GifPath::Path2) {
        return;
    }
    PacketRecord& packet = packets_[packet_record];
    packet.marker_bound = true;
    packet.marker_quadword_match = packet.first_quadword == event.quadword;
    packet.marker_raw_event = packet.raw_first_event;
    packet.source_event_domain_identity = event.source_event_domain_identity;
    packet.marker_shared_event = event.source_event_ordinal;
    packet.marker_raw_shared_event = source_event_ordinal_;
    packet.marker_generation = event.generation;
    packet.marker_source = event.source;
    packet.marker_chain_root = event.chain_root;
    packet.marker_tag_location = event.source_tag_location;
    packet.marker_tag_ordinal = event.source_tag_ordinal;
    packet.marker_payload_word_ordinal = event.source_payload_word_ordinal;
    pending_marker_event_ = kNoIndex;
}

void Slus21050FemainTerminalProducerDiagnostic::associate_worker_serial_locked(
    const PendingPrimary& pending, const std::uint64_t serial) noexcept {
    if (pending.primary_record != kNoIndex && pending.primary_record < primary_command_count_) {
        primary_commands_[pending.primary_record].worker_serial = serial;
        primary_commands_[pending.primary_record].worker_dispatched = true;
    }
    if (pending.packet_record != kNoIndex && pending.packet_record < packet_count_) {
        PacketRecord& packet = packets_[pending.packet_record];
        if (packet.worker_dispatch_count == 0U) {
            packet.worker_first_serial = serial;
        } else if (packet.worker_last_serial + 1U != serial) {
            packet.worker_serial_range_contiguous = false;
        }
        packet.worker_last_serial = serial;
        saturating_increment(packet.worker_dispatch_count);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::discard_worker_dispatch_record_locked(
    const std::size_t record) noexcept {
    if (worker_dispatch_count_ != 0U) {
        --worker_dispatch_count_;
    }
    if (worker_dispatch_count_ == 0U) {
        worker_dispatch_hook_seen_.store(false, std::memory_order_relaxed);
    }
    if (record == kNoIndex || record >= worker_dispatch_record_count_) {
        return;
    }
    for (auto index = record + 1U; index < worker_dispatch_record_count_; ++index) {
        worker_dispatches_[index - 1U] = worker_dispatches_[index];
    }
    --worker_dispatch_record_count_;
    worker_dispatches_[worker_dispatch_record_count_] = WorkerDispatchRecord{};
    for (std::size_t offset = 0U; offset < early_worker_dispatch_count_; ++offset) {
        const auto slot = (early_worker_dispatch_head_ + offset) % kMaximumPendingPrimaryCommands;
        EarlyWorkerDispatch& early = early_worker_dispatches_[slot];
        if (early.record != kNoIndex && record < early.record) {
            --early.record;
        }
    }
}

void Slus21050FemainTerminalProducerDiagnostic::enqueue_or_match_primary_locked(
    const PendingPrimary& pending) noexcept {
    if (!worker_dispatch_expected_) {
        return;
    }
    if (!worker_mapping_valid_.load(std::memory_order_relaxed)) {
        return;
    }
    if (early_worker_dispatch_count_ != 0U) {
        // The worker already ran this command; match it against the oldest
        // early dispatch instead of queueing a pending entry.
        const EarlyWorkerDispatch early = early_worker_dispatches_[early_worker_dispatch_head_];
        early_worker_dispatch_head_ =
            (early_worker_dispatch_head_ + 1U) % kMaximumPendingPrimaryCommands;
        --early_worker_dispatch_count_;
        if (primary_kind_for(early.kind) != pending.kind) {
            worker_mapping_valid_.store(false, std::memory_order_relaxed);
            return;
        }
        if (pending.retain_worker_dispatch) {
            associate_worker_serial_locked(pending, early.serial);
        } else {
            discard_worker_dispatch_record_locked(early.record);
        }
        return;
    }
    if (pending_primary_count_ >= kMaximumPendingPrimaryCommands) {
        worker_mapping_valid_.store(false, std::memory_order_relaxed);
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto slot = (pending_primary_head_ + pending_primary_count_) % kMaximumPendingPrimaryCommands;
    pending_primary_[slot] = pending;
    ++pending_primary_count_;
}

void Slus21050FemainTerminalProducerDiagnostic::record_primary(
    const PrimaryCommandKind kind, const guest::GifPath path, const std::uint64_t guest_sequence,
    const std::uint64_t command_digest) {
    const std::uint64_t timeline_event = next_timeline_event_locked();
    const auto path_index = static_cast<std::size_t>(path);
    if (path_index < discard_packet_prefix_after_reset_.size() &&
        discard_packet_prefix_after_reset_[path_index]) {
        // The packet began before the guest-work boundary: keep the worker FIFO
        // aligned but retain no record for it.
        enqueue_or_match_primary_locked(
            PendingPrimary{kind, primary_ordinal_, kNoIndex, kNoIndex, false});
        return;
    }
    saturating_increment(primary_ordinal_);
    const std::size_t packet_record = active_packet_for(path);
    std::size_t primary_record = kNoIndex;
    if (primary_command_count_ < kMaximumPrimaryCommands) {
        primary_record = primary_command_count_++;
        primary_commands_[primary_record] =
            PrimaryCommandRecord{primary_ordinal_, observation_epoch_, timeline_event, kind, path,
                                 packet_record,    guest_sequence,     command_digest, 0U,   false};
    } else {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    }
    if (packet_record != kNoIndex && packet_record < packet_count_) {
        PacketRecord& packet = packets_[packet_record];
        if (packet.primary_command_count == 0U) {
            packet.primary_first_ordinal = primary_ordinal_;
        }
        packet.primary_last_ordinal = primary_ordinal_;
        saturating_increment(packet.primary_command_count);
    }
    enqueue_or_match_primary_locked(
        PendingPrimary{kind, primary_ordinal_, primary_record, packet_record, true});
}

void Slus21050FemainTerminalProducerDiagnostic::record_primary_without_path(
    const PrimaryCommandKind kind, const std::uint64_t command_digest) {
    const std::uint64_t timeline_event = next_timeline_event_locked();
    saturating_increment(primary_ordinal_);
    std::size_t primary_record = kNoIndex;
    if (primary_command_count_ < kMaximumPrimaryCommands) {
        primary_record = primary_command_count_++;
        primary_commands_[primary_record] =
            PrimaryCommandRecord{primary_ordinal_, observation_epoch_, timeline_event, kind,
                                 guest::GifPath{}, kNoIndex,           0U,             command_digest,
                                 0U,               false};
    } else {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    }
    enqueue_or_match_primary_locked(
        PendingPrimary{kind, primary_ordinal_, primary_record, kNoIndex, true});
}

void Slus21050FemainTerminalProducerDiagnostic::invalidate_locked(
    const LifecycleReason reason) noexcept {
    saturating_increment(observation_epoch_);
    std::size_t active_count = 0U;
    for (auto& packet_record : active_packets_) {
        if (packet_record == kNoIndex || packet_record >= packet_count_) {
            packet_record = kNoIndex;
            continue;
        }
        packets_[packet_record].invalidated = true;
        ++active_count;
        packet_record = kNoIndex;
    }
    pending_marker_event_ = kNoIndex;
    if (reason == LifecycleReason::GifResetOrObserverChange || reason == LifecycleReason::GsReset ||
        reason == LifecycleReason::MemoryCleared || reason == LifecycleReason::CaptureDisabledOrGap) {
        packet_open_.fill(false);
        discard_packet_prefix_after_reset_.fill(false);
    }
    if (invalidation_count_ < kMaximumInvalidations) {
        invalidations_[invalidation_count_++] = InvalidationRecord{
            observation_epoch_, reason, active_count, std::this_thread::get_id()};
    } else {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::record_marker_event_locked(
    const MarkerEventRecord& event) noexcept {
    if (marker_event_count_ >= kMaximumMarkerEvents) {
        dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    marker_events_[marker_event_count_++] = event;
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_gif_packet_eop(
    const guest::GifPath path, const bool primary_command_stream_available) noexcept {
    Slus21050FemainSourceDiagnosticEventIdentity shared_event{};
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t timeline_event = next_timeline_event_locked();
        if (source_probe_forward_ != nullptr) {
            if (source_event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
            } else {
                shared_event = Slus21050FemainSourceDiagnosticEventIdentity{
                    source_event_domain_identity_, observation_epoch_, ++source_event_ordinal_,
                    Slus21050FemainSourceDiagnosticEventKind::PacketCompletion};
            }
        }
        saturating_increment(eop_event_ordinal_);
        const auto path_index = static_cast<std::size_t>(path);
        if (path_index < discard_packet_prefix_after_reset_.size() &&
            discard_packet_prefix_after_reset_[path_index]) {
            // The EOP closes a packet that began before the guest-work boundary.
            discard_packet_prefix_after_reset_[path_index] = false;
            if (path == guest::GifPath::Path2) {
                pending_marker_event_ = kNoIndex;
            }
        } else {
            const std::size_t packet_record = active_packet_for(path);
            if (packet_record != kNoIndex && packet_record < packet_count_) {
                PacketRecord& packet = packets_[packet_record];
                packet.eop_event = eop_event_ordinal_;
                packet.eop_timeline_event = timeline_event;
                packet.complete = true;
                packet.primary_command_stream_available = primary_command_stream_available;
                if (shared_event.valid()) {
                    packet.eop_shared_event = shared_event.ordinal;
                }
            }
        }
        if (path_index < packet_open_.size()) {
            packet_open_[path_index] = false;
            active_packets_[path_index] = kNoIndex;
        }
    }
    if (shared_event.valid() && source_probe_forward_ != nullptr) {
        static_cast<void>(source_probe_forward_->observe_diagnostic_packet_eop(
            path, primary_command_stream_available, shared_event));
    }
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_gif_reset() noexcept {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        invalidate_locked(LifecycleReason::GifResetOrObserverChange);
    }
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_reset();
    }
    if (source_probe_forward_ != nullptr) {
        source_probe_forward_->on_gs_gif_reset();
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_gif_tag_begin(const guest::GifPath path) {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto hash = kFnvOffsetBasis;
        mix_domain(hash, "slus-21050-femain-primary-gif-tag-begin-v1");
        mix_word(hash, static_cast<std::uint64_t>(path));
        record_primary(PrimaryCommandKind::GifTagBegin, path, 0U, finish(hash));
    }
    if (primary_forward_ != nullptr) {
        primary_forward_->on_gs_gif_tag_begin(path);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_reset() {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        invalidate_locked(LifecycleReason::GsReset);
        auto hash = kFnvOffsetBasis;
        mix_domain(hash, "slus-21050-femain-primary-gs-reset-v1");
        record_primary_without_path(PrimaryCommandKind::GsStateReset, finish(hash));
    }
    if (primary_forward_ != nullptr) {
        primary_forward_->on_gs_reset();
    }
    if (source_probe_forward_ != nullptr) {
        source_probe_forward_->on_gs_gif_reset();
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_register_write(
    const guest::GsRegisterWrite& write) {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        record_primary(PrimaryCommandKind::RegisterWrite, write.path, write.sequence,
                       register_write_digest(write));
    }
    if (primary_forward_ != nullptr) {
        primary_forward_->on_gs_register_write(write);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_image_transfer(
    const guest::GsImageTransfer& transfer) {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        record_primary(PrimaryCommandKind::ImageTransfer, transfer.path, transfer.sequence,
                       image_transfer_digest(transfer));
    }
    if (primary_forward_ != nullptr) {
        primary_forward_->on_gs_image_transfer(transfer);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_gs_image_transfer(
    guest::GsImageTransfer&& transfer) {
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        record_primary(PrimaryCommandKind::ImageTransfer, transfer.path, transfer.sequence,
                       image_transfer_digest(transfer));
    }
    if (primary_forward_ != nullptr) {
        primary_forward_->on_gs_image_transfer(std::move(transfer));
    }
}

bool Slus21050FemainTerminalProducerDiagnostic::on_vif1_dmac_chain_begin(
    const guest::Vif1DmacChainObservation& observation) noexcept {
    bool accepted = true;
    if (chain_forward_ != nullptr) {
        accepted = chain_forward_->on_vif1_dmac_chain_begin(observation);
    }
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        discard_chain_prefix_after_reset_ = false;
        chain_open_ = true;
        saturating_increment(chain_sequence_);
        active_chain_record_ = kNoIndex;
        if (chain_count_ < kMaximumChains) {
            const std::size_t record = chain_count_++;
            active_chain_record_ = record;
            chains_[record] = ChainRecord{chain_sequence_,
                                          observation_epoch_,
                                          observation.chain_root,
                                          observation.start_pc,
                                          guest::DmacSourceStop::TagOutOfRange,
                                          0U,
                                          0U,
                                          0U,
                                          0U,
                                          accepted,
                                          false};
        } else {
            dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    return accepted;
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_dmac_chain_tag(
    const guest::Vif1DmacTagObservation& observation) noexcept {
    if (chain_forward_ != nullptr) {
        chain_forward_->on_vif1_dmac_chain_tag(observation);
    }
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        // RECOVERY: the original dump carries no early-return predictor note on this exit;
        // the branch probabilities and control flow are otherwise the same.
        if (discard_chain_prefix_after_reset_) {
            return;
        }
        {
            if (chain_tag_count_ < kMaximumChainTags) {
                const std::size_t tag_record = chain_tag_count_++;
                chain_tags_[tag_record] = ChainTagRecord{active_chain_record_ != kNoIndex &&
                                                                 active_chain_record_ < chain_count_
                                                             ? chains_[active_chain_record_].sequence
                                                             : 0U,
                                                         observation_epoch_,
                                                         observation.chain_root,
                                                         observation.ordinal,
                                                         observation.tag,
                                                         observation.last_writer_pc,
                                                         observation.last_writer_pc_known};
                if (active_chain_record_ != kNoIndex && active_chain_record_ < chain_count_) {
                    ChainRecord& chain = chains_[active_chain_record_];
                    if (chain.tags_processed == 0U) {
                        chain.first_tag_record = tag_record;
                    }
                    chain.last_tag_record = tag_record;
                    saturating_increment(chain.tags_processed);
                }
            } else {
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_dmac_chain_end(
    const guest::Vif1DmacChainResultObservation& observation) noexcept {
    if (chain_forward_ != nullptr) {
        chain_forward_->on_vif1_dmac_chain_end(observation);
    }
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        // RECOVERY: see on_vif1_dmac_chain_tag; same predictor-note-only difference.
        if (discard_chain_prefix_after_reset_) {
            // This end closes a chain that began before the guest-work boundary.
            discard_chain_prefix_after_reset_ = false;
            chain_open_ = false;
            return;
        }
        if (active_chain_record_ != kNoIndex && active_chain_record_ < chain_count_) {
            ChainRecord& chain = chains_[active_chain_record_];
            chain.stop = observation.stop;
            chain.tags_processed = observation.tags_processed;
            chain.payload_quadwords = observation.payload_quadwords;
            chain.complete = true;
        }
        active_chain_record_ = kNoIndex;
        chain_open_ = false;
    }
}

bool Slus21050FemainTerminalProducerDiagnostic::has_vif1_dmac_pre_snapshot_candidates() const noexcept {
    return marker_forward_ != nullptr && marker_forward_->has_vif1_dmac_pre_snapshot_candidates();
}

std::size_t Slus21050FemainTerminalProducerDiagnostic::on_vif1_dmac_pre_snapshot_candidates(
    const guest::Vif1DmacPreSnapshotObservation& observation,
    const std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept {
    const auto offered = marker_forward_ != nullptr
                             ? marker_forward_->on_vif1_dmac_pre_snapshot_candidates(observation,
                                                                                    candidates)
                             : 0U;
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t count = std::min(offered, candidates.size());
        for (std::size_t index = 0U; index < count; ++index) {
            if (marker_candidate_count_ >= kMaximumMarkerCandidates) {
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
                break;
            }
            marker_candidates_[marker_candidate_count_++] =
                MarkerCandidateRecord{observation_epoch_, observation, candidates[index]};
        }
        return offered;
    }
    return offered;
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_dmac_pre_snapshot_markers_resolved(
    const std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
    const std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept {
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_dmac_pre_snapshot_markers_resolved(markers, decisions);
    }
    if (recording_enabled()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t count = std::min(markers.size(), decisions.size());
        for (std::size_t index = 0U; index < count; ++index) {
            highest_marker_generation_observed_ =
                std::max(highest_marker_generation_observed_, markers[index].generation);
            if (marker_resolution_count_ >= kMaximumMarkerResolutions) {
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
                break;
            }
            marker_resolutions_[marker_resolution_count_++] = MarkerResolutionRecord{
                observation_epoch_, markers[index],
                decisions[index] == guest::Vif1DmacPreSnapshotMarkerDecision::Accept};
        }
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_path2_source_marker_memory_cleared() noexcept {
    if (!recording_enabled()) {
        if (marker_forward_ != nullptr) {
            marker_forward_->on_vif1_path2_source_marker_memory_cleared();
        }
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        invalidate_locked(LifecycleReason::MemoryCleared);
        record_marker_event_locked(MarkerEventRecord{.observation_epoch = observation_epoch_,
                                                     .kind = MarkerEventKind::MemoryCleared});
    }
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_memory_cleared();
    }
    if (source_probe_forward_ != nullptr) {
        source_probe_forward_->on_gs_gif_reset();
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_path2_source_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    if (!recording_enabled()) {
        if (marker_forward_ != nullptr) {
            marker_forward_->on_vif1_path2_source_marker_reached(marker);
        }
        return;
    }
    Slus21050FemainSourceDiagnosticEventIdentity shared_event{};
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        highest_marker_generation_observed_ =
            std::max(highest_marker_generation_observed_, marker.generation);
        // RECOVERY: the original basic-block layout of this three-way branch could
        // not be reproduced; the behaviour is the same.
        if (guest_work_scope_started_ && marker.generation <= marker_generation_floor_) {
            // The marker predates the current guest-work window.
            pending_marker_event_ = kNoIndex;
        } else if (marker_event_count_ < kMaximumMarkerEvents) {
            if (source_probe_forward_ != nullptr) {
                if (source_event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
                    dropped_records_.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    shared_event = Slus21050FemainSourceDiagnosticEventIdentity{
                        source_event_domain_identity_, observation_epoch_, ++source_event_ordinal_,
                        Slus21050FemainSourceDiagnosticEventKind::MarkerReached};
                }
            }
            const std::size_t event_record = marker_event_count_++;
            marker_events_[event_record] = MarkerEventRecord{
                .observation_epoch = observation_epoch_,
                .source_event_domain_identity = shared_event.domain_identity,
                .source_event_ordinal = shared_event.ordinal,
                .kind = MarkerEventKind::Reached,
                .generation = marker.generation,
                .source = marker.source,
                .chain_root = marker.chain_root,
                .source_tag_location = marker.source_tag_location,
                .source_tag_ordinal = marker.source_tag_ordinal,
                .source_payload_word_ordinal = marker.source_payload_word_ordinal,
                .quadword = marker.quadword,
                .has_quadword = true};
            pending_marker_event_ = event_record;
        } else {
            dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_reached(marker);
    }
    if (shared_event.valid() && source_probe_forward_ != nullptr) {
        static_cast<void>(
            source_probe_forward_->observe_diagnostic_marker_reached(marker, shared_event));
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_vif1_path2_source_marker_cancelled(
    const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    if (!recording_enabled()) {
        if (marker_forward_ != nullptr) {
            marker_forward_->on_vif1_path2_source_marker_cancelled(marker);
        }
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        highest_marker_generation_observed_ =
            std::max(highest_marker_generation_observed_, marker.generation);
        if (guest_work_scope_started_ && marker.generation <= marker_generation_floor_) {
            // The cancellation belongs to a marker from before this guest-work window.
        } else {
            invalidate_locked(LifecycleReason::MarkerCancelled);
            record_marker_event_locked(
                MarkerEventRecord{.observation_epoch = observation_epoch_,
                                  .kind = MarkerEventKind::Cancelled,
                                  .generation = marker.generation,
                                  .source = marker.source,
                                  .chain_root = marker.chain_root,
                                  .source_tag_location = marker.source_tag_location,
                                  .source_tag_ordinal = marker.source_tag_ordinal,
                                  .source_payload_word_ordinal = marker.source_payload_word_ordinal,
                                  .cancel_reason = marker.reason});
        }
    }
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_cancelled(marker);
    }
    if (source_probe_forward_ != nullptr) {
        static_cast<void>(source_probe_forward_->observe_vif1_path2_source_marker_cancelled(marker));
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_ee_memory_write(
    const guest::EeMemoryWriteEvent& event) noexcept {
    if (!recording_enabled()) {
        return;
    }
    if (!is_retail_scalar_address(event.original_address) &&
        !is_retail_scalar_address(event.offset)) {
        return;
    }
    Slus21050FemainRetailConsumeScalarWriteObserver* retail_forward = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t timeline_event = next_timeline_event_locked();
        if (scalar_write_count_ < kMaximumScalarWrites) {
            scalar_writes_[scalar_write_count_++] =
                ScalarWriteRecord{observation_epoch_, timeline_event, raw_event_ordinal_,
                                  eop_event_ordinal_, primary_ordinal_,
                                  scalar_field_for(event.offset), event};
        } else {
            dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        }
        retail_forward = retail_consume_scalar_forward_;
    }
    if (retail_forward != nullptr) {
        retail_forward->on_retail_consume_scalar_write(event);
    }
}

void Slus21050FemainTerminalProducerDiagnostic::on_renderer_dispatch(
    const presentation::GsRendererWorkerCommandKind kind, const std::uint64_t serial) noexcept {
    if (!recording_enabled()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    PendingPrimary matched_pending{};
    std::size_t dispatch_record = kNoIndex;
    bool suppress_record = false;
    bool consumed_pending = false;
    bool has_matched_pending = false;
    if (is_primary_kind(kind) && pending_primary_count_ != 0U) {
        matched_pending = pending_primary_[pending_primary_head_];
        pending_primary_head_ = (pending_primary_head_ + 1U) % kMaximumPendingPrimaryCommands;
        --pending_primary_count_;
        consumed_pending = true;
        if (primary_kind_for(kind) != matched_pending.kind) {
            worker_mapping_valid_.store(false, std::memory_order_relaxed);
        } else {
            has_matched_pending = true;
            suppress_record = !matched_pending.retain_worker_dispatch;
        }
    }
    if (!suppress_record) {
        worker_dispatch_hook_seen_.store(true, std::memory_order_relaxed);
        saturating_increment(worker_dispatch_count_);
        if (worker_dispatch_record_count_ < kMaximumWorkerDispatches) {
            dispatch_record = worker_dispatch_record_count_++;
            worker_dispatches_[dispatch_record] = WorkerDispatchRecord{
                observation_epoch_, kind, serial, std::this_thread::get_id()};
        } else {
            dropped_records_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    if (is_primary_kind(kind)) {
        if (has_matched_pending) {
            associate_worker_serial_locked(matched_pending, serial);
        } else if (!(suppress_record || consumed_pending)) {
            // The worker ran this command before the primary observer queued it.
            if (early_worker_dispatch_count_ < kMaximumPendingPrimaryCommands) {
                const auto slot = (early_worker_dispatch_count_ + early_worker_dispatch_head_) %
                                  kMaximumPendingPrimaryCommands;
                early_worker_dispatches_[slot] = EarlyWorkerDispatch{kind, serial, dispatch_record};
                ++early_worker_dispatch_count_;
            } else {
                worker_mapping_valid_.store(false, std::memory_order_relaxed);
                dropped_records_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
}

Slus21050FemainTerminalProducerDiagnostic::ScalarField
Slus21050FemainTerminalProducerDiagnostic::scalar_field_for(
    const guest::GuestAddress address) noexcept {
    switch (address & 0x1FFFFFFFU) {
    case 0x004E2804U:
        return ScalarField::BufferIndex;
    case 0x004E2810U:
        return ScalarField::Occupancy;
    case 0x004E2814U:
        return ScalarField::OccupancyLimit;
    case 0x004E281CU:
        return ScalarField::PendingEnd;
    case 0x004E2830U:
        return ScalarField::TransfersActive;
    case 0x004E2838U:
        return ScalarField::QueueRead;
    case 0x004E283CU:
        return ScalarField::QueueEnd;
    default:
        return ScalarField::Unknown;
    }
}

bool Slus21050FemainTerminalProducerDiagnostic::is_retail_scalar_address(
    const guest::GuestAddress address) noexcept {
    return (address & 0x1FFFFFFFU) - 0x004E2800U <= 0x40U;
}

bool Slus21050FemainTerminalProducerDiagnostic::is_primary_kind(
    const presentation::GsRendererWorkerCommandKind kind) noexcept {
    return kind <= presentation::GsRendererWorkerCommandKind::GsStateReset;
}

Slus21050FemainTerminalProducerDiagnostic::PrimaryCommandKind
Slus21050FemainTerminalProducerDiagnostic::primary_kind_for(
    const presentation::GsRendererWorkerCommandKind kind) noexcept {
    switch (kind) {
    case presentation::GsRendererWorkerCommandKind::GifTagBegin:
        return PrimaryCommandKind::GifTagBegin;
    case presentation::GsRendererWorkerCommandKind::GsStateReset:
        return PrimaryCommandKind::GsStateReset;
    case presentation::GsRendererWorkerCommandKind::ImageTransfer:
        return PrimaryCommandKind::ImageTransfer;
    default:
        return PrimaryCommandKind::RegisterWrite;
    }
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::PacketRecord>
Slus21050FemainTerminalProducerDiagnostic::packets() const noexcept {
    return {packets_.data(), packet_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::ChainRecord>
Slus21050FemainTerminalProducerDiagnostic::chains() const noexcept {
    return {chains_.data(), chain_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::ChainTagRecord>
Slus21050FemainTerminalProducerDiagnostic::chain_tags() const noexcept {
    return {chain_tags_.data(), chain_tag_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::MarkerCandidateRecord>
Slus21050FemainTerminalProducerDiagnostic::marker_candidates() const noexcept {
    return {marker_candidates_.data(), marker_candidate_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::MarkerResolutionRecord>
Slus21050FemainTerminalProducerDiagnostic::marker_resolutions() const noexcept {
    return {marker_resolutions_.data(), marker_resolution_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::MarkerEventRecord>
Slus21050FemainTerminalProducerDiagnostic::marker_events() const noexcept {
    return {marker_events_.data(), marker_event_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::ScalarWriteRecord>
Slus21050FemainTerminalProducerDiagnostic::scalar_writes() const noexcept {
    return {scalar_writes_.data(), scalar_write_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::PrimaryCommandRecord>
Slus21050FemainTerminalProducerDiagnostic::primary_commands() const noexcept {
    return {primary_commands_.data(), primary_command_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::WorkerDispatchRecord>
Slus21050FemainTerminalProducerDiagnostic::worker_dispatches() const noexcept {
    return {worker_dispatches_.data(), worker_dispatch_record_count_};
}

std::span<const Slus21050FemainTerminalProducerDiagnostic::InvalidationRecord>
Slus21050FemainTerminalProducerDiagnostic::invalidations() const noexcept {
    return {invalidations_.data(), invalidation_count_};
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::observation_epoch() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return observation_epoch_;
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::raw_quadwords_observed() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return raw_event_ordinal_;
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::primary_commands_observed() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return primary_ordinal_;
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::worker_dispatches_observed() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return worker_dispatch_count_;
}

Slus21050FemainTerminalProducerDiagnostic::CounterSnapshot
Slus21050FemainTerminalProducerDiagnostic::counter_snapshot() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    CounterSnapshot snapshot;
    snapshot.observation_epoch = observation_epoch_;
    snapshot.timeline_events = timeline_event_ordinal_;
    snapshot.raw_quadwords = raw_event_ordinal_;
    snapshot.packet_eop_events = eop_event_ordinal_;
    snapshot.primary_commands = primary_ordinal_;
    snapshot.worker_dispatches = worker_dispatch_count_;
    return snapshot;
}

bool Slus21050FemainTerminalProducerDiagnostic::worker_mapping_valid() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return worker_dispatch_hook_seen_.load(std::memory_order_relaxed) &&
           worker_mapping_valid_.load(std::memory_order_relaxed) &&
           pending_primary_count_ == 0U && early_worker_dispatch_count_ == 0U;
}

bool Slus21050FemainTerminalProducerDiagnostic::scalar_write_observer_unavailable() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return scalar_write_observer_unavailable_;
}

std::uint64_t Slus21050FemainTerminalProducerDiagnostic::dropped_records() const noexcept {
    return dropped_records_.load(std::memory_order_relaxed);
}

Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult
Slus21050FemainTerminalProducerDiagnostic::seal_immutable_snapshot() const noexcept {
    using Failure = SnapshotSealFailure;
    using Result = Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return Result{Failure::Disabled, std::nullopt};
    }
    if (!guest_work_scope_requested_ || !guest_work_scope_started_) {
        return Result{Failure::GuestWorkScopeNotStarted, std::nullopt};
    }
    if (!guest_work_scope_completed_) {
        return Result{Failure::GuestWorkScopeIncomplete, std::nullopt};
    }
    if (!collection_closed_.load(std::memory_order_relaxed)) {
        return Result{Failure::CollectionOpen, std::nullopt};
    }
    if (!context_bound_) {
        return Result{Failure::ContextUnbound, std::nullopt};
    }
    if (context_.runtime_epoch == 0U || context_.worker_epoch == 0U ||
        context_.player_session == 0U || context_.owner_generation == 0U ||
        context_.frame_id == 0U || context_.ledger_identity == 0U ||
        context_.ledger_session == 0U) {
        return Result{Failure::ContextInvalid, std::nullopt};
    }
    if (observation_epoch_ == 0U) {
        return Result{Failure::ObservationEpochInvalid, std::nullopt};
    }
    if (timeline_event_ordinal_ < timeline_scope_start_ || raw_event_ordinal_ < raw_scope_start_ ||
        eop_event_ordinal_ < eop_scope_start_ || primary_ordinal_ < primary_scope_start_) {
        return Result{Failure::CounterReordered, std::nullopt};
    }
    for (const bool open : packet_open_) {
        if (open) {
            return Result{Failure::PacketOpen, std::nullopt};
        }
    }
    for (const unsigned long long active : active_packets_) {
        if (active != kNoIndex) {
            return Result{Failure::PacketOpen, std::nullopt};
        }
    }
    if (chain_open_ || active_chain_record_ != kNoIndex) {
        return Result{Failure::ChainOpen, std::nullopt};
    }
    for (const bool discarded : discard_packet_prefix_after_reset_) {
        if (discarded) {
            return Result{Failure::DiscardedPacketPrefix, std::nullopt};
        }
    }
    if (discard_chain_prefix_after_reset_) {
        return Result{Failure::DiscardedChainPrefix, std::nullopt};
    }
    if (pending_marker_event_ != kNoIndex) {
        return Result{Failure::PendingMarker, std::nullopt};
    }
    if (pending_primary_count_ != 0U) {
        return Result{Failure::PendingPrimary, std::nullopt};
    }
    if (early_worker_dispatch_count_ != 0U) {
        return Result{Failure::EarlyWorkerDispatch, std::nullopt};
    }
    if (worker_dispatch_expected_ &&
        (!guest_work_scope_worker_fence_completed_ || guest_work_scope_worker_fence_serial_ == 0U)) {
        return Result{Failure::WorkerFenceMissing, std::nullopt};
    }
    const bool hook_seen = worker_dispatch_hook_seen_.load(std::memory_order_relaxed);
    const bool mapping_valid = hook_seen && worker_mapping_valid_.load(std::memory_order_relaxed) &&
                               pending_primary_count_ == 0U && early_worker_dispatch_count_ == 0U;
    if (worker_dispatch_expected_ && !hook_seen) {
        return Result{Failure::WorkerHookMissing, std::nullopt};
    }
    if (worker_dispatch_expected_ && !mapping_valid) {
        return Result{Failure::WorkerMappingInvalid, std::nullopt};
    }
    if (dropped_records_.load(std::memory_order_relaxed) != 0U) {
        return Result{Failure::DroppedRecords, std::nullopt};
    }
    if (invalidation_count_ != 0U) {
        return Result{Failure::InvalidationsPresent, std::nullopt};
    }
    const auto all_in_epoch = [this](const auto records) {
        for (const auto& record : records) {
            if (record.observation_epoch != observation_epoch_) {
                return false;
            }
        }
        return true;
    };
    if (!(all_in_epoch(std::span(packets_).first(packet_count_)) &&
          all_in_epoch(std::span(chains_).first(chain_count_)) &&
          all_in_epoch(std::span(chain_tags_).first(chain_tag_count_)) &&
          all_in_epoch(std::span(marker_candidates_).first(marker_candidate_count_)) &&
          all_in_epoch(std::span(marker_resolutions_).first(marker_resolution_count_)) &&
          all_in_epoch(std::span(marker_events_).first(marker_event_count_)) &&
          all_in_epoch(std::span(scalar_writes_).first(scalar_write_count_)) &&
          all_in_epoch(std::span(primary_commands_).first(primary_command_count_)) &&
          all_in_epoch(std::span(worker_dispatches_).first(worker_dispatch_record_count_)) &&
          all_in_epoch(std::span(invalidations_).first(invalidation_count_)))) {
        return Result{Failure::RecordEpochMismatch, std::nullopt};
    }
    for (const auto& packet : std::span(packets_).first(packet_count_)) {
        if (!packet.complete || packet.invalidated) {
            return Result{Failure::PacketOpen, std::nullopt};
        }
    }
    for (const auto& chain : std::span(chains_).first(chain_count_)) {
        if (!chain.complete) {
            return Result{Failure::ChainOpen, std::nullopt};
        }
    }

    try {
        Slus21050FemainTerminalProducerDiagnosticSnapshot snapshot{};
        snapshot.context = context_;
        snapshot.context_bound = context_bound_;
        snapshot.observation_thread = observation_thread_;
        snapshot.observation_epoch = observation_epoch_;
        snapshot.timeline_events_observed = timeline_event_ordinal_;
        snapshot.raw_quadwords_observed = raw_event_ordinal_;
        snapshot.packet_eop_events_observed = eop_event_ordinal_;
        snapshot.primary_commands_observed = primary_ordinal_;
        snapshot.timeline_events_in_scope = timeline_event_ordinal_ - timeline_scope_start_;
        snapshot.raw_quadwords_in_scope = raw_event_ordinal_ - raw_scope_start_;
        snapshot.packet_eop_events_in_scope = eop_event_ordinal_ - eop_scope_start_;
        snapshot.primary_commands_in_scope = primary_ordinal_ - primary_scope_start_;
        snapshot.worker_dispatches_observed = worker_dispatch_count_;
        snapshot.worker_dispatch_expected = worker_dispatch_expected_;
        snapshot.worker_mapping_valid = mapping_valid;
        snapshot.guest_work_scope_requested = guest_work_scope_requested_;
        snapshot.guest_work_scope_started = guest_work_scope_started_;
        snapshot.guest_work_scope_completed = guest_work_scope_completed_;
        snapshot.guest_work_scope_worker_fence_completed = guest_work_scope_worker_fence_completed_;
        snapshot.guest_work_scope_worker_fence_serial = guest_work_scope_worker_fence_serial_;
        snapshot.collection_closed = collection_closed_.load(std::memory_order_relaxed);
        snapshot.dropped_records = dropped_records_.load(std::memory_order_relaxed);
        snapshot.packets.assign(packets_.begin(), packets_.begin() + packet_count_);
        snapshot.chains.assign(chains_.begin(), chains_.begin() + chain_count_);
        snapshot.chain_tags.assign(chain_tags_.begin(), chain_tags_.begin() + chain_tag_count_);
        snapshot.marker_candidates.assign(marker_candidates_.begin(),
                                          marker_candidates_.begin() + marker_candidate_count_);
        snapshot.marker_resolutions.assign(marker_resolutions_.begin(),
                                           marker_resolutions_.begin() + marker_resolution_count_);
        snapshot.marker_events.assign(marker_events_.begin(),
                                      marker_events_.begin() + marker_event_count_);
        snapshot.scalar_writes.assign(scalar_writes_.begin(),
                                      scalar_writes_.begin() + scalar_write_count_);
        snapshot.primary_commands.assign(primary_commands_.begin(),
                                         primary_commands_.begin() + primary_command_count_);
        snapshot.worker_dispatches.assign(worker_dispatches_.begin(),
                                          worker_dispatches_.begin() + worker_dispatch_record_count_);
        snapshot.invalidations.assign(invalidations_.begin(),
                                      invalidations_.begin() + invalidation_count_);
        return Result{Failure::None, std::move(snapshot)};
    } catch (...) {
        return Result{Failure::SnapshotAllocationFailure, std::nullopt};
    }
}

bool Slus21050FemainTerminalProducerDiagnostic::write_report(
    const std::filesystem::path& path) const {
    std::ofstream output(path);
    if (!output) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string_view record_scope = guest_work_scope_completed_ ? "guest-work-a-to-c"
                                          : guest_work_scope_started_
                                              ? "guest-work-a-to-termination"
                                              : "full-run";
    const unsigned long long raw_in_scope = raw_event_ordinal_ >= raw_scope_start_ ? raw_event_ordinal_ - raw_scope_start_ : 0U;
    const unsigned long long eop_in_scope = eop_event_ordinal_ >= eop_scope_start_ ? eop_event_ordinal_ - eop_scope_start_ : 0U;
    const unsigned long long primary_in_scope = primary_ordinal_ >= primary_scope_start_ ? primary_ordinal_ - primary_scope_start_ : 0U;
    const unsigned long long timeline_in_scope = timeline_event_ordinal_ >= timeline_scope_start_ ? timeline_event_ordinal_ - timeline_scope_start_ : 0U;
    output << "schema=2\n"
           << "enabled=" << (enabled_ ? 1 : 0) << "\n"
           << "record_scope=" << record_scope << "\n"
           << "guest_work_scope.requested=" << (guest_work_scope_requested_ ? 1 : 0) << "\n"
           << "guest_work_scope.started=" << (guest_work_scope_started_ ? 1 : 0) << "\n"
           << "guest_work_scope.completed=" << (guest_work_scope_completed_ ? 1 : 0) << "\n"
           << "guest_work_scope.collection_closed="
           << (collection_closed_.load(std::memory_order_relaxed) ? 1 : 0) << "\n"
           << "terminal_authority=none\n"
           << "terminal_witness_emitted=0\n"
           << "terminal_producer_attachment=missing\n"
           << "retail_consume_attachment=missing\n"
           << "retail_consume_terminal_authority=none\n"
           << "terminal_producer_guest_pc=unknown\n"
           << "terminal_producer_proxy_rejected=next-eop-vblank\n"
           << "retail_join_function=FE_JoinDrainPump\n"
           << "retail_join_guest_pc=0x001f2408\n"
           << "retail_join_region_guest_pc=0x001f24e0\n"
           << "observation_thread_token=0x" << std::hex
           << std::hash<std::thread::id>{}(observation_thread_) << std::dec << "\n"
           << "context_bound=" << (context_bound_ ? 1 : 0) << "\n"
           << "context.runtime_epoch=" << context_.runtime_epoch << "\n"
           << "context.worker_epoch=" << context_.worker_epoch << "\n"
           << "context.player_session=" << context_.player_session << "\n"
           << "context.owner_generation=" << context_.owner_generation << "\n"
           << "context.frame_id=" << context_.frame_id << "\n"
           << "context.ledger_identity=" << context_.ledger_identity << "\n"
           << "context.ledger_session=" << context_.ledger_session << "\n"
           << "observation_epoch=" << observation_epoch_ << "\n"
           << "monotonic_counter_scope=diagnostic-lifetime\n"
           << "timeline_events=" << timeline_event_ordinal_ << "\n"
           << "timeline_events_in_record_scope=" << timeline_in_scope << "\n"
           << "raw_quadwords=" << raw_event_ordinal_ << "\n"
           << "packet_eop_events=" << eop_event_ordinal_ << "\n"
           << "raw_quadwords_in_record_scope=" << raw_in_scope << "\n"
           << "packet_eop_events_in_record_scope=" << eop_in_scope << "\n"
           << "packets_retained=" << packet_count_ << "\n"
           << "primary_commands=" << primary_ordinal_ << "\n"
           << "primary_commands_in_record_scope=" << primary_in_scope << "\n"
           << "primary_commands_retained=" << primary_command_count_ << "\n"
           << "worker_dispatches=" << worker_dispatch_count_ << "\n"
           << "worker_dispatches_retained=" << worker_dispatch_record_count_ << "\n"
           << "worker_mapping_valid="
           << (worker_dispatch_hook_seen_.load(std::memory_order_relaxed) &&
                       worker_mapping_valid_.load(std::memory_order_relaxed) &&
                       pending_primary_count_ == 0U && early_worker_dispatch_count_ == 0U
                   ? 1
                   : 0)
           << "\n"
           << "scalar_writes_retained=" << scalar_write_count_ << "\n"
           << "scalar_write_observer_unavailable=" << (scalar_write_observer_unavailable_ ? 1 : 0)
           << "\n"
           << "dropped_records=" << dropped_records_.load(std::memory_order_relaxed) << "\n"
           << "attachment.raw_forward=" << (raw_forward_ != nullptr ? 1 : 0) << "\n"
           << "attachment.primary_forward=" << (primary_forward_ != nullptr ? 1 : 0) << "\n"
           << "attachment.chain_forward=" << (chain_forward_ != nullptr ? 1 : 0) << "\n"
           << "attachment.marker_forward=" << (marker_forward_ != nullptr ? 1 : 0) << "\n"
           << "attachment.scalar_write=" << (scalar_write_observer_attached_ ? 1 : 0) << "\n"
           << "missing_attachment.reason=retail terminal graphics producer callback is not exposed; "
              "records below are observational only\n";
    for (const auto& packet : std::span(packets_).first(packet_count_)) {
        output << "packet ordinal=" << packet.ordinal << " epoch=" << packet.observation_epoch
               << " path=" << static_cast<unsigned>(packet.path) + 1U
               << " raw=" << packet.raw_first_event << '-' << packet.raw_last_event
               << " raw-timeline=" << packet.raw_first_timeline_event << '-'
               << packet.raw_last_timeline_event << " qwords=" << packet.raw_quadwords
               << " eop-event=" << packet.eop_event
               << " eop-timeline=" << packet.eop_timeline_event << " tag-low=0x" << std::hex
               << packet.gif_tag_low << " tag-high=0x" << packet.gif_tag_high << std::dec
               << " loop=" << packet.loop_count << " nreg=" << packet.register_count
               << " flg=" << packet.format << " prim=" << packet.primitive
               << " expected-payload=" << packet.expected_payload_quadwords
               << " eop-tag=" << (packet.eop_in_tag ? 1 : 0)
               << " complete=" << (packet.complete ? 1 : 0)
               << " primary=" << packet.primary_first_ordinal << '-' << packet.primary_last_ordinal
               << " primary-count=" << packet.primary_command_count
               << " primary-available=" << (packet.primary_command_stream_available ? 1 : 0)
               << " worker=" << packet.worker_first_serial << '-' << packet.worker_last_serial
               << " worker-count=" << packet.worker_dispatch_count
               << " worker-contiguous=" << (packet.worker_serial_range_contiguous ? 1 : 0)
               << " marker-bound=" << (packet.marker_bound ? 1 : 0)
               << " marker-match=" << (packet.marker_quadword_match ? 1 : 0)
               << " invalidated=" << (packet.invalidated ? 1 : 0) << "\n";
    }
    for (const auto& chain : std::span(chains_).first(chain_count_)) {
        output << "chain sequence=" << chain.sequence << " epoch=" << chain.observation_epoch
               << " root=0x" << std::hex << chain.chain_root << " start-pc=0x" << chain.start_pc
               << std::dec << " accepted=" << (chain.accepted ? 1 : 0)
               << " complete=" << (chain.complete ? 1 : 0) << " stop=" << stop_name(chain.stop)
               << " tags=" << chain.tags_processed << " payload-qwords=" << chain.payload_quadwords
               << " tag-records=" << chain.first_tag_record << '-' << chain.last_tag_record << "\n";
    }
    for (const auto& tag : std::span(chain_tags_).first(chain_tag_count_)) {
        output << "chain-tag sequence=" << tag.sequence << " epoch=" << tag.observation_epoch
               << " root=0x" << std::hex << tag.chain_root << " location=0x"
               << tag.tag.location.value << " payload=0x" << tag.tag.payload.value
               << " writer-pc=0x" << tag.last_writer_pc << std::dec << " ordinal=" << tag.ordinal
               << " id=" << tag.tag.id << " qwc=" << tag.tag.qwc
               << " writer-pc-known=" << (tag.last_writer_pc_known ? 1 : 0) << " words=0x"
               << std::hex << tag.tag.words[0] << ',' << tag.tag.words[1] << ','
               << tag.tag.words[2] << ',' << tag.tag.words[3] << std::dec << "\n";
    }
    for (const auto& candidate : std::span(marker_candidates_).first(marker_candidate_count_)) {
        output << "marker-candidate epoch=" << candidate.observation_epoch << " root=0x"
               << std::hex << candidate.observation.chain_root << " start-pc=0x"
               << candidate.observation.start_pc << " token=" << std::dec
               << candidate.candidate.token << " source=0x" << std::hex
               << candidate.candidate.source << std::dec << "\n";
    }
    for (const auto& resolution : std::span(marker_resolutions_).first(marker_resolution_count_)) {
        output << "marker-resolution epoch=" << resolution.observation_epoch
               << " token=" << resolution.marker.token
               << " generation=" << resolution.marker.generation
               << " accepted=" << (resolution.accepted ? 1 : 0) << " source=0x" << std::hex
               << resolution.marker.request.source << " chain-root=0x"
               << resolution.marker.request.chain_root << std::dec << "\n";
    }
    for (const auto& event : std::span(marker_events_).first(marker_event_count_)) {
        output << "marker-event epoch=" << event.observation_epoch
               << " kind=" << marker_event_kind_name(event.kind)
               << " generation=" << event.generation << " source=0x" << std::hex << event.source
               << " chain-root=0x" << event.chain_root << " tag-location=0x"
               << event.source_tag_location << std::dec
               << " tag-ordinal=" << event.source_tag_ordinal
               << " payload-word-ordinal=" << event.source_payload_word_ordinal
               << " cancel-reason=" << static_cast<unsigned>(event.cancel_reason)
               << " quadword=" << (event.has_quadword ? 1 : 0) << "\n";
    }
    for (const auto& write : std::span(scalar_writes_).first(scalar_write_count_)) {
        output << "scalar-write epoch=" << write.observation_epoch
               << " field=" << scalar_field_name(write.field) << " timeline=" << write.timeline_event
               << " raw-observed=" << write.raw_events_observed
               << " eop-observed=" << write.eop_events_observed
               << " primary-observed=" << write.primary_events_observed
               << " guest-step=" << write.event.guest_step << " pc=0x" << std::hex << write.event.pc
               << " original=0x" << write.event.original_address << " offset=0x" << write.event.offset
               << std::dec << " width=" << static_cast<unsigned>(write.event.width)
               << " changed-mask=0x" << std::hex << static_cast<unsigned long>(write.event.changed_byte_mask)
               << std::dec << " previous-available=" << (write.event.previous_payload_available ? 1 : 0)
               << " payload=" << std::hex << std::setfill('0');
        const std::size_t byte_count = std::min<std::size_t>(write.event.width, 16U);
        for (std::size_t index = 0U; byte_count > index; ++index) {
            output << std::setw(2) << static_cast<unsigned>(write.event.payload[index]);
        }
        output << " previous=" << std::hex;
        if (write.event.previous_payload_available) {
            output << std::setfill('0');
            for (std::size_t index = 0U; byte_count > index; ++index) {
                output << std::setw(2) << static_cast<unsigned>(write.event.previous_payload[index]);
            }
        } else {
            output << "unavailable";
        }
        output << std::setfill(' ') << std::dec << "\n";
    }
    for (const auto& command : std::span(primary_commands_).first(primary_command_count_)) {
        output << "primary ordinal=" << command.ordinal << " epoch=" << command.observation_epoch
               << " timeline=" << command.timeline_event
               << " kind=" << primary_kind_name(command.kind)
               << " path=" << static_cast<unsigned>(command.path) + 1U
               << " packet-record=" << command.packet_record
               << " guest-sequence=" << command.guest_sequence
               << " worker-serial=" << command.worker_serial
               << " worker-dispatched=" << (command.worker_dispatched ? 1 : 0) << "\n";
    }
    for (const auto& dispatch : std::span(worker_dispatches_).first(worker_dispatch_record_count_)) {
        output << "worker-dispatch epoch=" << dispatch.observation_epoch
               << " kind=" << worker_command_kind_name(dispatch.kind)
               << " serial=" << dispatch.serial << " thread-token=0x" << std::hex
               << std::hash<std::thread::id>{}(dispatch.thread) << std::dec << "\n";
    }
    for (const auto& invalidation : std::span(invalidations_).first(invalidation_count_)) {
        output << "invalidation epoch=" << invalidation.observation_epoch
               << " reason=" << lifecycle_reason_name(invalidation.reason)
               << " active-packets=" << invalidation.active_packet_count << " thread-token=0x"
               << std::hex << std::hash<std::thread::id>{}(invalidation.thread) << std::dec
               << "\n";
    }
    output.flush();
    return static_cast<bool>(output);
}

} // namespace takedown::runtime
