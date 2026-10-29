#include "takedown/runtime/vu1_worker.hpp"

#include "takedown/platform/host_wait_stats.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>

namespace takedown::runtime {

namespace {

// Process-wide hook state (see vu1_worker.hpp).
Vu1WorkerDifferentialController* g_vu1_worker_differential_controller = nullptr;
bool g_vu1_worker_async_stage2_enabled = false;
bool g_vu1_worker_consume_enabled = false;

// Appends one change record per 32-bit word that differs between two VUMem
// images.
void copy_vumem_words(const std::byte* previous, const std::byte* current,
                      std::vector<Vu1WorkerVumemChange>& changes) {
    for (std::size_t word = 0; word != kVu1WorkerMemorySize / 4U; ++word) {
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        __builtin_memcpy(&before, previous + word * 4U, sizeof(before));
        __builtin_memcpy(&after, current + word * 4U, sizeof(after));
        if (before != after) {
            changes.push_back(Vu1WorkerVumemChange{static_cast<std::uint32_t>(word), after});
        }
    }
}

// Two FNV-1a style lanes that start from different offset bases.
void hash_micro_memory(const std::span<const std::byte> bytes, guest::Vu1ProgramHash* const hash) {
    std::uint64_t low = 14695981039346656037ULL;
    std::uint64_t high = 11400714819323198485ULL;
    for (const std::byte byte : bytes) {
        const auto value = static_cast<unsigned long long>(byte);
        low ^= value;
        low *= 1099511628211ULL;
        high ^= value;
        high *= 1099511628211ULL;
    }
    hash->low = low;
    hash->high = high;
}

// Mirrors the launch snapshot into the private EeMemory: MicroMem, VUMem and
// the VIF1 TOP/ITOP registers.
void install_launch(guest::EeMemory& memory, const Vu1WorkerLaunchJob& job) {
    const auto micro = memory.view(kVu1WorkerMicroBase, kVu1WorkerMemorySize);
    __builtin_memcpy(micro.data(), job.micro_memory.data(), kVu1WorkerMemorySize);
    const auto data = memory.view(kVu1WorkerDataBase, kVu1WorkerMemorySize);
    __builtin_memcpy(data.data(), job.vu_mem.data(), kVu1WorkerMemorySize);
    memory.vu1_program_cache().notify_micro_memory_write();
    memory.write_u32_le(kVu1WorkerVif1Top, job.vif1_top);
    memory.write_u32_le(kVu1WorkerVif1Itop, job.vif1_itop);
}

} // namespace

// Records VUMem checkpoints and XGKICK packets into the journal of the launch
// currently executing on the private EeMemory.
struct Vu1WorkerExecutionService::PacketCapture final : guest::Vu1XgkickObserver {
    Vu1WorkerJournal* journal{};
    const std::byte* live_vumem{};
    std::vector<std::byte> last_checkpoint;
    std::uint64_t next_sequence{1U};

    [[nodiscard]] bool ready() const noexcept {
        return journal != nullptr && live_vumem != nullptr &&
               last_checkpoint.size() == kVu1WorkerMemorySize;
    }
    bool on_vu1_xgkick_begin(const guest::Vu1XgkickObservation& observation) noexcept override;
    void on_vu1_xgkick_quadword(const guest::GsQuadword& quadword) noexcept override;
    void on_vu1_xgkick_end(bool completed) noexcept override;
};

bool Vu1WorkerExecutionService::PacketCapture::on_vu1_xgkick_begin(
    const guest::Vu1XgkickObservation& observation) noexcept {
    if (!ready()) {
        return false;
    }
    Vu1WorkerJournalEvent event{};
    event.sequence = next_sequence++;
    copy_vumem_words(last_checkpoint.data(), live_vumem, event.vumem_changes);
    __builtin_memcpy(last_checkpoint.data(), live_vumem, kVu1WorkerMemorySize);
    journal->events.push_back(std::move(event));
    Vu1WorkerJournalEvent packet{};
    packet.kind = Vu1WorkerJournalEvent::Kind::XgkickPacket;
    packet.sequence = next_sequence++;
    packet.xgkick_vector = observation.vector_address;
    journal->events.push_back(std::move(packet));
    return true;
}

void Vu1WorkerExecutionService::PacketCapture::on_vu1_xgkick_quadword(
    const guest::GsQuadword& quadword) noexcept {
    if (journal == nullptr || journal->events.empty()) {
        return;
    }
    Vu1WorkerJournalEvent& event = journal->events.back();
    if (event.kind != Vu1WorkerJournalEvent::Kind::XgkickPacket) {
        return;
    }
    event.packet_quadwords.push_back(quadword);
}

void Vu1WorkerExecutionService::PacketCapture::on_vu1_xgkick_end(const bool completed) noexcept {
    if (completed || journal == nullptr || journal->events.empty()) {
        return;
    }
    Vu1WorkerJournalEvent& event = journal->events.back();
    if (event.kind == Vu1WorkerJournalEvent::Kind::XgkickPacket) {
        event.cancelled = true;
    }
}

Vu1WorkerExecutionService::Vu1WorkerExecutionService()
    : capture_(std::make_unique<PacketCapture>()) {
    capture_->journal = nullptr;
    memory_.set_vu1_xgkick_observer(capture_.get());
    memory_.set_gif_dmac_timing_enabled(false);
    memory_.set_vif1_dmac_timing_enabled(false);
    memory_.set_vu1_observed_window_recording(false);
    memory_.vu1_program_cache().set_aot_enabled(false);
}

Vu1WorkerExecutionService::~Vu1WorkerExecutionService() = default;

std::uint64_t Vu1WorkerExecutionService::launches_executed() const noexcept {
    return launches_executed_;
}

Vu1WorkerJournal Vu1WorkerExecutionService::execute(Vu1WorkerLaunchJob job) {
    Vu1WorkerJournal journal{};
    journal.job = std::move(job);

    hash_micro_memory(journal.job.micro_memory, &journal.job.program_hash);

    const auto activity_before = memory_.vu1_activity();

    install_launch(memory_, journal.job);
    memory_.vu1_program_cache().set_reference_tier_override(journal.job.reference_tier);
    memory_.set_vu1_xgkick_mode(journal.job.xgkick_mode);
    memory_.set_vif1_dmac_timing_enabled(journal.job.timed_vu1_execution_enabled);

    capture_->journal = &journal;
    capture_->next_sequence = 1U;
    journal.initial_vu_mem = journal.job.vu_mem;
    capture_->live_vumem = memory_.view(kVu1WorkerDataBase, kVu1WorkerMemorySize).data();
    capture_->last_checkpoint.assign(journal.job.vu_mem.begin(), journal.job.vu_mem.end());

    const auto started = std::chrono::steady_clock::now();
    journal.result = guest::run_vu1_micro(journal.job.vu1_state, memory_, journal.job.entry,
                                          journal.job.options);
    const auto finished = std::chrono::steady_clock::now();
    journal.executed_tier = memory_.vu1_program_cache().last_launch_tier();
    journal.profile_delta.host_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    journal.profile_delta.completed_runs =
        journal.result.status == guest::Vu1RunStatus::Completed ? 1U : 0U;
    journal.profile_delta.committed_pairs = journal.result.committed_pairs;
    journal.profile_delta.elapsed_cycles = journal.result.elapsed_cycles;
    journal.profile_delta.pipeline_stall_cycles = journal.result.pipeline_stall_cycles;

    // Final checkpoint: every word changed since the last XGKICK boundary.
    const std::byte* live = capture_->live_vumem;
    if (std::memcmp(capture_->last_checkpoint.data(), live, kVu1WorkerMemorySize) != 0) {
        Vu1WorkerJournalEvent event{};
        event.sequence = capture_->next_sequence++;
        copy_vumem_words(capture_->last_checkpoint.data(), live, event.vumem_changes);
        journal.events.push_back(std::move(event));
    }
    __builtin_memcpy(journal.job.vu_mem.data(), live, kVu1WorkerMemorySize);

    const auto pending = memory_.vu1_pending_xgkick();
    journal.pending_producer.active = pending.active;
    journal.pending_producer.vector_address = pending.cursor;
    journal.pending_producer.submitted_first_tag = pending.submitted_first_tag;

    const auto& activity_after = memory_.vu1_activity();
    Vu1WorkerXgkickAccounting& accounting = journal.xgkick_accounting;
    accounting.packets = activity_after.xgkick_packets - activity_before.xgkick_packets;
    accounting.packets_completed =
        activity_after.xgkick_packets_completed - activity_before.xgkick_packets_completed;
    accounting.quadwords = activity_after.xgkick_quadwords - activity_before.xgkick_quadwords;
    accounting.faults = activity_after.xgkick_faults - activity_before.xgkick_faults;
    accounting.oversized_tags =
        activity_after.xgkick_oversized_tags - activity_before.xgkick_oversized_tags;
    accounting.oversized_tags_present_at_submit =
        activity_after.xgkick_oversized_tags_present_at_submit -
        activity_before.xgkick_oversized_tags_present_at_submit;
    accounting.start_tags_changed_before_transfer =
        activity_after.xgkick_start_tags_changed_before_transfer -
        activity_before.xgkick_start_tags_changed_before_transfer;
    if (accounting.packets != 0U && activity_before.xgkick_packets == 0U) {
        accounting.first_vector_valid = true;
        accounting.first_vector = static_cast<std::uint16_t>(activity_after.xgkick_first_vector);
    }

    capture_->journal = nullptr;
    capture_->live_vumem = nullptr;
    capture_->last_checkpoint.clear();
    if (pending.active) {
        static_cast<void>(memory_.flush_vu1_xgkick());
    }
    ++launches_executed_;
    return journal;
}

Vu1WorkerReplayResult replay_vu1_worker_journal(const Vu1WorkerJournal& journal) {
    Vu1WorkerReplayResult replay{};
    replay.vu1_state = journal.job.vu1_state;
    replay.vu_mem = journal.initial_vu_mem;
    for (const Vu1WorkerJournalEvent& event : journal.events) {
        switch (event.kind) {
        case Vu1WorkerJournalEvent::Kind::VumemCheckpoint:
            for (const Vu1WorkerVumemChange& change : event.vumem_changes) {
                const auto offset = static_cast<unsigned long long>(change.word_offset) * 4U;
                __builtin_memcpy(replay.vu_mem.data() + offset, &change.value, sizeof(change.value));
            }
            break;
        case Vu1WorkerJournalEvent::Kind::XgkickPacket:
            replay.packet_quadwords.insert(replay.packet_quadwords.end(),
                                           event.packet_quadwords.begin(),
                                           event.packet_quadwords.end());
            break;
        }
    }
    return replay;
}

Vu1WorkerComparisonVerdict compare_vu1_worker_journal_to_reference(
    const Vu1WorkerJournal& journal, const guest::Vu1RunResult& reference_result,
    const guest::Vu1State& reference_state, const std::span<const std::byte> reference_vu_mem,
    std::string& mismatch_reason) {
    Vu1WorkerComparisonVerdict verdict{};
    if (journal.result.status != reference_result.status ||
        journal.result.committed_pairs != reference_result.committed_pairs ||
        journal.result.elapsed_cycles != reference_result.elapsed_cycles ||
        journal.result.pipeline_stall_cycles != reference_result.pipeline_stall_cycles) {
        verdict.result_matches = false;
        mismatch_reason = "run result (status/pairs/cycles/stalls)";
    }
    if (std::memcmp(&journal.job.vu1_state, &reference_state, sizeof(guest::Vu1State)) != 0) {
        verdict.state_matches = false;
        mismatch_reason = "final Vu1State (vectors/flags/control)";
    }
    if (reference_vu_mem.size() != kVu1WorkerMemorySize ||
        std::memcmp(journal.job.vu_mem.data(), reference_vu_mem.data(), kVu1WorkerMemorySize) != 0) {
        verdict.vumem_matches = false;
        mismatch_reason = "final VUMem bytes";
    }
    return verdict;
}

// Stage 2 milestone 3: capacity-1 pipeline with one launch job in flight. The
// worker thread owns a private execution service; the emulation thread only
// waits at the consume fence.
struct Vu1WorkerDifferentialController::ConsumePipeline {
    std::mutex mutex_;
    std::condition_variable job_available_;
    std::condition_variable journal_ready_cv_;
    std::condition_variable job_submitted_;
    Vu1WorkerLaunchJob job_;
    Vu1WorkerJournal journal_;
    bool has_job_{};
    bool journal_ready_{};
    bool stopping_{};
    std::unique_ptr<Vu1WorkerExecutionService> service_;
    std::thread worker_;

    ConsumePipeline() {
        service_ = std::make_unique<Vu1WorkerExecutionService>();
        worker_ = std::thread([this] { drain(); });
    }
    ConsumePipeline(const ConsumePipeline&) = delete;
    ConsumePipeline& operator=(const ConsumePipeline&) = delete;
    ~ConsumePipeline() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        job_available_.notify_all();
        journal_ready_cv_.notify_all();
        job_submitted_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Hands one job to the worker; blocks while the previous job or its
    // journal is still unclaimed.
    void submit(Vu1WorkerLaunchJob job) {
        std::unique_lock<std::mutex> lock(mutex_);
        job_submitted_.wait(lock, [this] { return (!has_job_ && !journal_ready_) || stopping_; });
        job_ = std::move(job);
        has_job_ = true;
        job_available_.notify_one();
    }

    // Joins the in-flight job and claims its journal.
    Vu1WorkerJournal take_journal(bool& still_running, std::uint64_t& wait_nanoseconds) {
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        if (!journal_ready_) {
            still_running = true;
            const platform::ScopedHostWait host_wait(platform::HostWaitSite::Vu1ConsumeJoin);
            journal_ready_cv_.wait(lock, [this] { return journal_ready_ || stopping_; });
        }
        wait_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        Vu1WorkerJournal journal = std::move(journal_);
        journal_ready_ = false;
        has_job_ = false;
        job_submitted_.notify_one();
        return journal;
    }

    void drain();
};

// Stage 2 milestone 2: bounded queue drained by one worker thread that compares
// each admitted launch against its synchronous reference.
struct Vu1WorkerDifferentialController::DeferredComparison {
    struct Item {
        Vu1WorkerLaunchJob job{};
        guest::Vu1RunResult reference_result{};
        guest::Vu1State reference_state{};
        std::vector<std::byte> reference_vu_mem;
    };
    static constexpr std::size_t kQueueCapacity = 16U;

    std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable space_available_;
    std::deque<Item> queue_;
    bool stopping_{};
    std::unique_ptr<Vu1WorkerExecutionService> service_;
    std::thread worker_;

    DeferredComparison() {
        service_ = std::make_unique<Vu1WorkerExecutionService>();
        worker_ = std::thread([this] { drain(); });
    }
    DeferredComparison(const DeferredComparison&) = delete;
    DeferredComparison& operator=(const DeferredComparison&) = delete;
    ~DeferredComparison() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        work_available_.notify_all();
        space_available_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Queues one comparison; blocks the emulation thread while the queue is
    // full.
    void enqueue(Item&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.size() >= kQueueCapacity && !stopping_) {
            const platform::ScopedHostWait host_wait(platform::HostWaitSite::Vu1DeferredQueueFull);
            space_available_.wait(lock,
                                  [this] { return queue_.size() < kQueueCapacity || stopping_; });
        }
        queue_.push_back(std::move(item));
        work_available_.notify_one();
    }

    static void verify_against_reference(const Vu1WorkerJournal& journal, const Item& item) {
        std::string reason;
        const Vu1WorkerComparisonVerdict verdict = compare_vu1_worker_journal_to_reference(
            journal, item.reference_result, item.reference_state, item.reference_vu_mem, reason);
        if (!verdict.all_match()) {
            std::cerr << "vu1-worker-async-stage2 HARD ERROR: deferred VU1 launch mismatched the "
                         "synchronous reference ("
                      << reason << "); the async result is never consumed by the guest, aborting\n";
            std::cerr.flush();
            std::abort();
        }
    }

    void drain();
};

Vu1WorkerDifferentialController::Vu1WorkerDifferentialController() = default;

Vu1WorkerDifferentialController::~Vu1WorkerDifferentialController() {
    join_deferred_worker();
}

void Vu1WorkerDifferentialController::set_enabled(const bool enabled) noexcept {
    enabled_ = enabled;
}

bool Vu1WorkerDifferentialController::enabled() const noexcept {
    return enabled_;
}

void Vu1WorkerDifferentialController::set_deferred(const bool enabled) noexcept {
    deferred_ = enabled;
}

bool Vu1WorkerDifferentialController::deferred() const noexcept {
    return deferred_;
}

void Vu1WorkerDifferentialController::set_consume(const bool enabled) noexcept {
    consume_ = enabled;
}

bool Vu1WorkerDifferentialController::consume() const noexcept {
    return consume_;
}

void Vu1WorkerDifferentialController::set_consume_audit_period(
    const std::uint64_t period) noexcept {
    consume_audit_period_ = period;
}

bool Vu1WorkerDifferentialController::observes_launches() const noexcept {
    return enabled_;
}

void Vu1WorkerDifferentialController::DeferredComparison::drain() {
    while (true) {
        Item item{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_available_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        space_available_.notify_one();
        const Vu1WorkerJournal journal = service_->execute(item.job);
        verify_against_reference(journal, item);
    }
}

void Vu1WorkerDifferentialController::join_deferred_worker() noexcept {
    if (pending_consume_ && !pending_audit_) {
        std::cerr << "vu1-worker-consume HARD ERROR: join requested with a pending unconsumed "
                     "launch; fence it before joining\n";
        std::cerr.flush();
        std::abort();
    }
    pending_consume_ = false;
    pending_audit_ = false;
    deferred_comparison_.reset();
    consume_pipeline_.reset();
}

void Vu1WorkerDifferentialController::ConsumePipeline::drain() {
    while (true) {
        Vu1WorkerLaunchJob job{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            job_available_.wait(lock, [this] { return has_job_ || stopping_; });
            if (!has_job_) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            job = std::move(job_);
            has_job_ = false;
        }
        job_submitted_.notify_one();
        Vu1WorkerJournal result = service_->execute(job);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            journal_ = std::move(result);
            journal_ready_ = true;
        }
        journal_ready_cv_.notify_one();
    }
}

void Vu1WorkerDifferentialController::defer_comparison(
    Vu1WorkerLaunchJob job, const guest::Vu1RunResult& reference_result,
    const guest::Vu1State& reference_state, const std::span<const std::byte> reference_vu_mem) {
    if (!deferred_comparison_) {
        deferred_comparison_ = std::make_unique<DeferredComparison>();
    }
    DeferredComparison::Item item{};
    item.job = std::move(job);
    item.reference_result = reference_result;
    item.reference_state = reference_state;
    item.reference_vu_mem.assign(reference_vu_mem.begin(), reference_vu_mem.end());
    deferred_comparison_->enqueue(std::move(item));
}

Vu1WorkerAdmission
Vu1WorkerDifferentialController::before_launch(const Vu1WorkerLaunchContext& context) {
    ++statistics_.launches_observed;
    pending_valid_ = false;
    pending_deferred_ = false;
    if (context.xgkick_gate_live) {
        ++statistics_.launches_skipped_gate_live;
        return Vu1WorkerAdmission::RunSynchronously;
    }
    if (context.vu_mem.size() != kVu1WorkerMemorySize ||
        context.micro_memory.size() != kVu1WorkerMemorySize) {
        return Vu1WorkerAdmission::RunSynchronously;
    }
    pending_.vu1_state = context.state;
    __builtin_memcpy(pending_.vu_mem.data(), context.vu_mem.data(), kVu1WorkerMemorySize);
    __builtin_memcpy(pending_.micro_memory.data(), context.micro_memory.data(),
                     kVu1WorkerMemorySize);
    hash_micro_memory(pending_.micro_memory, &pending_.program_hash);
    pending_.vif1_top = context.vif1_top;
    pending_.vif1_itop = context.vif1_itop;
    pending_.entry = context.entry;
    pending_.options = guest::Vu1RunOptions{};
    pending_.timed_vu1_execution_enabled = context.timed_vu1_execution_enabled;
    pending_.xgkick_mode = context.xgkick_mode;
    pending_valid_ = true;

    const bool consume_admission = consume_ && g_vu1_worker_consume_enabled;
    if (consume_admission || deferred_) {
        if (consume_admission) {
            const bool deferrable =
                !context.live_observers_present &&
                is_deferrable_vu1_launch(pending_, context.xgkick_gate_live,
                                         context.observed_windows);
            if (!deferrable) {
                ++statistics_.launches_deferred_denied;
                return Vu1WorkerAdmission::RunSynchronously;
            }
            if (!consume_pipeline_) {
                consume_pipeline_ = std::make_unique<ConsumePipeline>();
            }
            ++statistics_.launches_deferred_admitted;
            consume_thread_ = std::this_thread::get_id();
            consume_pipeline_->submit(pending_);
            pending_consume_ = true;
            pending_consume_started_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (consume_audit_period_ != 0U &&
                statistics_.launches_deferred_admitted % consume_audit_period_ == 0U) {
                pending_audit_ = true;
                audit_sync_run_active_ = true;
                ++statistics_.consume_audited_launches;
                return Vu1WorkerAdmission::DeferWithLiveReference;
            }
            return Vu1WorkerAdmission::DeferToWorker;
        }
        if (g_vu1_worker_async_stage2_enabled) {
            if (is_deferrable_vu1_launch(pending_, context.xgkick_gate_live,
                                         context.observed_windows)) {
                pending_deferred_ = true;
                ++statistics_.launches_deferred_admitted;
            } else {
                ++statistics_.launches_deferred_denied;
            }
        }
    }
    return Vu1WorkerAdmission::RunSynchronously;
}

void Vu1WorkerDifferentialController::after_launch(
    const guest::Vu1RunResult& reference_result, const guest::Vu1State& reference_state,
    std::span<const std::byte> reference_vu_mem, const guest::Vu1ReferenceTier reference_tier) {
    if (!pending_valid_) {
        return;
    }
    pending_valid_ = false;
    pending_.reference_tier = reference_tier;
    if (reference_vu_mem.size() != kVu1WorkerMemorySize) {
        pending_deferred_ = false;
        return;
    }
    if (pending_audit_) {
        audit_reference_result_ = reference_result;
        audit_reference_state_ = reference_state;
        audit_reference_vu_mem_.assign(reference_vu_mem.begin(), reference_vu_mem.end());
        return;
    }
    ++statistics_.launches_compared;
    const auto log_mismatch = [this](const std::string_view reason) {
        if (statistics_.reported_mismatch_logs < 8U) {
            std::cerr << "vu1-worker-differential mismatch: " << reason << '\n';
            ++statistics_.reported_mismatch_logs;
        }
    };
    if (pending_deferred_) {
        pending_deferred_ = false;
        defer_comparison(pending_, reference_result, reference_state, reference_vu_mem);
        return;
    }
    const Vu1WorkerJournal journal = service_.execute(pending_);
    std::string mismatch_reason;
    const Vu1WorkerComparisonVerdict verdict = compare_vu1_worker_journal_to_reference(
        journal, reference_result, reference_state, reference_vu_mem, mismatch_reason);
    if (!verdict.result_matches) {
        ++statistics_.result_mismatches;
        log_mismatch(mismatch_reason);
    }
    if (!verdict.state_matches) {
        ++statistics_.state_mismatches;
        log_mismatch(mismatch_reason);
    }
    if (!verdict.vumem_matches) {
        ++statistics_.vumem_mismatches;
        log_mismatch(mismatch_reason);
    }
}

void Vu1WorkerDifferentialController::end_audit_sync_run() noexcept {
    audit_sync_run_active_ = false;
}

std::optional<Vu1WorkerJournal>
Vu1WorkerDifferentialController::consume_pending(const Vu1WorkerConsumeReason reason) {
    if (!pending_consume_) {
        return std::nullopt;
    }
    if (audit_sync_run_active_) {
        return std::nullopt;
    }
    if (std::this_thread::get_id() != consume_thread_) {
        return std::nullopt;
    }
    pending_consume_ = false;
    switch (reason) {
    case Vu1WorkerConsumeReason::NextLaunch:
        ++statistics_.consume_fences_next_launch;
        break;
    case Vu1WorkerConsumeReason::Flush:
        ++statistics_.consume_fences_flush;
        break;
    case Vu1WorkerConsumeReason::UnpackCompletion:
        ++statistics_.consume_fences_unpack;
        break;
    case Vu1WorkerConsumeReason::EeBusAccess:
        ++statistics_.consume_fences_ee_access;
        break;
    case Vu1WorkerConsumeReason::Sync:
        ++statistics_.consume_fences_sync;
        break;
    }
    bool still_running = false;
    std::uint64_t wait_nanoseconds = 0;
    Vu1WorkerJournal journal = consume_pipeline_->take_journal(still_running, wait_nanoseconds);
    if (still_running) {
        ++statistics_.consume_fences_worker_still_running;
        statistics_.consume_join_wait_nanoseconds += wait_nanoseconds;
    }
    const unsigned long long finished_ns = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    statistics_.consume_overlap_nanoseconds += finished_ns - pending_consume_started_ns;
    if (pending_audit_) {
        pending_audit_ = false;
        std::string mismatch_reason;
        const Vu1WorkerComparisonVerdict verdict = compare_vu1_worker_journal_to_reference(
            journal, audit_reference_result_, audit_reference_state_, audit_reference_vu_mem_,
            mismatch_reason);
        if (!verdict.all_match()) {
            std::string detail;
            if (!verdict.vumem_matches && audit_reference_vu_mem_.size() == kVu1WorkerMemorySize) {
                for (std::size_t word = 0; word < kVu1WorkerMemorySize / 4U; ++word) {
                    std::uint32_t worker_value = 0;
                    std::uint32_t reference_value = 0;
                    __builtin_memcpy(&worker_value, journal.job.vu_mem.data() + word * 4U, 4U);
                    __builtin_memcpy(&reference_value, audit_reference_vu_mem_.data() + word * 4U,
                                     4U);
                    if (worker_value != reference_value) {
                        detail += " first-diff word " + std::to_string(word) + " (offset 0x" +
                                 std::to_string(word * 4U) + "): worker=0x" +
                                 std::to_string(worker_value) + " reference=0x" +
                                 std::to_string(reference_value);
                        break;
                    }
                }
            }
            std::cerr << "vu1-worker-consume HARD ERROR: audited VU1 launch mismatched its "
                         "synchronous reference ("
                      << mismatch_reason << ");" << detail
                      << "; the worker journal is not byte-faithful, aborting\n";
            std::cerr.flush();
            std::abort();
        }
        audit_reference_vu_mem_.clear();
        audit_reference_vu_mem_.shrink_to_fit();
        return std::nullopt;
    }
    return journal;
}

const Vu1WorkerDifferentialController::Statistics&
Vu1WorkerDifferentialController::statistics() const noexcept {
    return statistics_;
}

void Vu1WorkerDifferentialController::report(std::ostream& stream) const {
    stream << "vu1-worker-differential: observed=" << statistics_.launches_observed
           << " compared=" << statistics_.launches_compared
           << " skipped_gate_live=" << statistics_.launches_skipped_gate_live
           << " result_mismatches=" << statistics_.result_mismatches
           << " state_mismatches=" << statistics_.state_mismatches
           << " vumem_mismatches=" << statistics_.vumem_mismatches
           << " deferred_admitted=" << statistics_.launches_deferred_admitted
           << " deferred_denied=" << statistics_.launches_deferred_denied;
    if (consume_) {
        stream << " consume_fences(next_launch=" << statistics_.consume_fences_next_launch
               << ",flush=" << statistics_.consume_fences_flush
               << ",unpack=" << statistics_.consume_fences_unpack
               << ",ee_access=" << statistics_.consume_fences_ee_access
               << ",sync=" << statistics_.consume_fences_sync
               << ") fences_worker_still_running="
               << statistics_.consume_fences_worker_still_running
               << " join_wait_ns=" << statistics_.consume_join_wait_nanoseconds
               << " overlap_ns=" << statistics_.consume_overlap_nanoseconds
               << " audited=" << statistics_.consume_audited_launches;
    }
    stream << '\n';
}

Vu1WorkerDifferentialController* vu1_worker_differential_controller() noexcept {
    return g_vu1_worker_differential_controller;
}

void set_vu1_worker_differential_controller(
    Vu1WorkerDifferentialController* const controller) noexcept {
    g_vu1_worker_differential_controller = controller;
}

bool vu1_worker_async_stage2_enabled() noexcept {
    return g_vu1_worker_async_stage2_enabled;
}

void set_vu1_worker_async_stage2_enabled(const bool enabled) noexcept {
    g_vu1_worker_async_stage2_enabled = enabled;
}

bool vu1_worker_consume_enabled() noexcept {
    return g_vu1_worker_consume_enabled;
}

void set_vu1_worker_consume_enabled(const bool enabled) noexcept {
    g_vu1_worker_consume_enabled = enabled;
}

} // namespace takedown::runtime
