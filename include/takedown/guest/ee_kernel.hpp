#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/platform/host_file_reader.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::save {
class SaveCardDevice;
} // namespace takedown::save

namespace takedown::content {
class ModOverlay;
} // namespace takedown::content

namespace takedown::guest {

enum class EeKernelCallStatus : std::uint8_t {
    Handled,
    Blocked,
    Unsupported,
    InvalidArguments,
};

struct EeKernelCallResult {
    EeKernelCallStatus status{EeKernelCallStatus::Unsupported};
    std::uint32_t number{};
    std::string_view name{"unknown"};
};

struct EeKernelStartupState {
    bool main_thread_initialized{};
    bool heap_initialized{};
    GuestAddress global_pointer{};
    GuestAddress stack_base{};
    std::uint32_t stack_size{};
    GuestAddress initial_stack_pointer{};
    GuestAddress argument_block{};
    GuestAddress root_function{};
    GuestAddress heap_start{};
    GuestAddress heap_end{};
};

struct EeGsCrtState {
    bool configured{};
    std::int16_t interlace{};
    std::int16_t mode{};
    std::int16_t field_mode{};
};

// SetVSyncFlag installs the two user-memory locations that the EE kernel
// updates when it services VBLANK-start.  The registration is replaced by a
// later call, matching the BIOS' single global pair rather than inventing a
// queue of frame waiters.
struct EeVsyncFlagState {
    bool registered{};
    GuestAddress flag_address{};
    GuestAddress csr_address{};
    std::uint64_t registration_count{};
    std::uint64_t notification_count{};

    bool operator==(const EeVsyncFlagState&) const = default;
};

struct EeSemaphoreState {
    std::int32_t id{};
    std::int32_t count{};
    std::int32_t max_count{};
    std::int32_t wait_threads{};
    std::uint32_t attributes{};
    std::uint32_t option{};
};

struct EeSyscallHandlerState {
    std::uint32_t number{};
    GuestAddress handler{};

    bool operator==(const EeSyscallHandlerState&) const = default;
};

struct EeCacheMaintenanceEvent {
    std::uint32_t operation{};
};

struct EeIntcHandlerState {
    std::int32_t id{};
    std::uint32_t cause{};
    GuestAddress handler{};
    GuestAddress argument{};
    GuestAddress global_pointer{};

    bool operator==(const EeIntcHandlerState&) const = default;
};

struct EeDmacHandlerState {
    std::int32_t id{};
    std::uint32_t channel{};
    GuestAddress handler{};
    GuestAddress argument{};
    GuestAddress global_pointer{};

    bool operator==(const EeDmacHandlerState&) const = default;
};

struct EeAlarmState {
    std::int32_t id{};
    std::uint16_t requested_hsync_ticks{};
    GuestAddress handler{};
    GuestAddress common{};
    GuestAddress global_pointer{};
    std::uint64_t deadline_ee_cycles{};

    bool operator==(const EeAlarmState&) const = default;
};

struct EeSifRegisterState {
    std::uint32_t number{};
    std::uint32_t value{};

    bool operator==(const EeSifRegisterState&) const = default;
};

struct EeSifDmaTransferState {
    std::int32_t id{};
    GuestAddress source{};
    std::uint32_t destination{};
    std::uint32_t size{};
    std::uint32_t attributes{};
    bool completed{};
    // Guest EE cycles at sceSifSetDma submission. Retail SIF DMAs complete
    // asynchronously, AFTER the submitting code's queue-time bookkeeping;
    // the kernel ages transfers (docs/archive/AUDIO_HISTORY.md increment 18) instead of
    // collapsing completion into the submission instant.
    std::uint64_t submitted_ee_cycles{};

    bool operator==(const EeSifDmaTransferState&) const = default;
};

// Cumulative RenderWare Audio command traffic observed at the ordinary SIF
// command endpoint. Keeping every opcode visible is important while the
// no-IOP runner grows protocol support: an unhandled command must not look
// like a successfully initialized audio service.
struct EeRwaCommandRecord {
    std::uint64_t sequence{};
    std::int32_t sif_dma_id{};
    GuestAddress packet_address{};
    GuestAddress syscall_pc{};
    GuestAddress syscall_ra{};
    // Recovered only under the exact retail sceSifSetDma wrapper signature;
    // zero means that the stack did not prove the caller.
    GuestAddress sender_callsite{};
    std::array<std::uint32_t, 12> packet_words{};

    bool operator==(const EeRwaCommandRecord&) const = default;
};

struct EeRwaActivity {
    std::uint64_t command_packets{};
    std::array<std::uint64_t, 256> opcode_counts{};
    std::array<std::array<std::uint32_t, 12>, 256> last_packet_words{};
    std::uint32_t last_opcode{};
    // The most recent opcode-0x46 SPU2 transfer descriptor (five words as
    // delivered by the EE): window offset, SPU2 destination, IOP source,
    // chunk byte count, total window byte count. Keeping it observable lets
    // diagnostics correlate executed transfers with resident guest bytes.
    std::array<std::uint32_t, 5> last_spu_descriptor{};
    // Ring of the most recent 0x46 descriptors — one entry per command,
    // regardless of materialization: sequence followed by the five
    // descriptor words. The bank-vs-stream question (mode/src/dst
    // families) is settled from this in one run instead of a single
    // "last" snapshot.
    static constexpr std::size_t kSpuDescriptorHistory = 128U;
    std::array<std::array<std::uint32_t, 6>, kSpuDescriptorHistory>
        spu_descriptor_history{};
    std::size_t spu_descriptor_history_count{};
    // Bank-loader node probe (docs/archive/AUDIO_HISTORY.md increment 18): the fe.awd bank
    // pump (0x2ab500) is dispatched through a runtime method table; loader
    // nodes in the EE heap carry {0x2ab880, 0x2ab4f0, 0x2ab500, 0x2ab790}
    // at node+0x28..0x34. Snapshots are appended on state CHANGE while a
    // node exists (the silence diagnosis branches on existence + the FSM
    // state at *(node+104) it sticks at).
    struct BankProbe {
        std::uint64_t at_command{};
        std::uint32_t node_address{};
        std::uint32_t bank_state{};
        std::uint32_t loader_ptr{};
        std::uint32_t flags{};
        std::uint32_t records{};
        std::uint32_t chunk_count{};
        std::uint32_t chunk_size{};
        std::uint32_t resume{};
        std::uint32_t spu_base{};
        std::uint32_t window_ptr{};
        std::uint32_t window_log2{};
        std::uint32_t window_base{};
        std::uint32_t window_stride{};
        std::uint32_t in_flight{};
    };
    static constexpr std::size_t kBankProbeHistory = 64U;
    std::array<BankProbe, kBankProbeHistory> bank_probe_history{};
    std::size_t bank_probe_history_count{};
    std::uint32_t bank_probe_scans{};
    // All RWA.IRX VoiceTrans requests observed through the three retail seams:
    // opcode 0x05's resident-IOP transfer, opcode 0x07's staged bank upload,
    // and opcode 0x46's explicit descriptor.
    std::uint64_t spu_transfer_count{};
    // A transfer is materialized only when every byte at the descriptor's
    // exact IOP source was established by a real EE -> IOP upload or stream
    // read. No filename/read-order source substitution is permitted.
    std::uint64_t spu_transfer_materialized{};
    std::uint64_t spu_transfer_source_missing{};
    // Arming-gate probe (increment 16): across all opcode-0x14 commits,
    // how many carried key-on-pending slots, the max pending in one
    // commit, and the raw state histogram over every walked slot —
    // distinguishes "the game never arms" from "the walk misreads".
    std::uint64_t voice_pending_commits{};
    std::uint32_t voice_pending_max{};
    std::array<std::uint32_t, 8> voice_state_histogram{};
    std::uint64_t spu_transfer_bounds_rejected{};
    std::uint64_t spu_transfer_bytes{};
    std::uint32_t last_spu_iop_source{};
    std::uint32_t last_spu_destination{};
    std::uint32_t last_spu_size{};
    std::uint64_t last_spu_fnv1a64{};
    // Opcode 0x05 is a bounded libsd VoiceTrans from resident physical IOP
    // bytes (+0x18) to a raw SPU2 byte address (+0x1c). These command-level
    // counters distinguish its lifecycle from the shared per-chunk counters
    // above; rejected means the HLE withheld unproven/out-of-range audio but
    // still completed the retail command protocol.
    std::uint64_t op05_voice_transfer_completed{};
    std::uint64_t op05_voice_transfer_rejected{};
    std::uint64_t op05_voice_transfer_bytes{};
    std::uint32_t last_op05_iop_source{};
    std::uint32_t last_op05_spu_destination{};
    std::uint32_t last_op05_size{};
    std::uint64_t last_op05_fnv1a64{};
    // Opcode 0x07 is the bank VoiceTrans (docs/archive/AUDIO_HISTORY.md increment 18,
    // corrected final addendum): one SIF DMA ID stages the guest-patched
    // ADPCM payload into RWA.IRX's shared IOP staging buffer, then RWA.IRX
    // 0x7c18 VoiceTranses it from staging to the raw SPU2 destination and
    // answers with a CID-1 type-0x0f record. Counters mirror op-0x05's
    // lifecycle block so the SFX-bank success criteria (op-0x07s fire,
    // bytes land) are readable from the report alone.
    std::uint64_t op07_staging_transfer_completed{};
    std::uint64_t op07_staging_transfer_rejected{};
    std::uint64_t op07_staging_transfer_bytes{};
    std::uint64_t op07_staging_responses_enqueued{};
    std::uint64_t op07_staging_responses_dropped{};
    std::uint32_t last_op07_staging_address{};
    std::uint32_t last_op07_spu_destination{};
    std::uint32_t last_op07_chunk_size{};
    std::uint32_t last_op07_remaining_size{};
    std::uint64_t last_op07_fnv1a64{};
    std::vector<EeRwaCommandRecord> recent_commands{};
    std::vector<EeRwaCommandRecord> recent_op05_voice_transfers{};
    std::vector<EeRwaCommandRecord> recent_op07_staging_transfers{};
    std::uint64_t voice_snapshots_returned{};
    std::uint64_t voice_snapshot_return_bytes{};
    GuestAddress last_voice_snapshot_destination{};
    // Opcode-0x14 completions whose executed-retail response arrived after
    // the packet (field-paced handlers): applied late, and still awaited at
    // end of run.
    std::uint64_t voice_snapshots_deferred_applied{};
    std::uint64_t voice_snapshots_deferred_pending{};
};

// Cumulative host-backed GTFSCDVD activity. These records are diagnostic only:
// they make successful disc streaming distinguishable from a guest merely
// reaching the RPC interface.
struct EeGtfsActivity {
    std::uint64_t open_requests{};
    std::uint64_t successful_opens{};
    std::uint64_t read_requests{};
    std::uint64_t successful_reads{};
    std::uint64_t sectors_requested{};
    std::uint64_t host_bytes_read{};
    std::uint64_t zero_padding_bytes{};
    std::uint64_t seek_requests{};
    std::uint64_t successful_seeks{};
    std::uint64_t close_requests{};
    std::uint64_t successful_closes{};
};

struct EeGtfsOpenRecord {
    std::string guest_path{};
    std::filesystem::path host_path{};
    std::uint32_t handle{0xffffffffU};
    std::uint32_t file_size{};
    std::uint64_t read_requests{};
    std::uint64_t successful_reads{};
    std::uint64_t sectors_requested{};
    std::uint64_t host_bytes_read{};
    std::uint64_t zero_padding_bytes{};
    std::uint64_t seek_requests{};
    std::uint64_t successful_seeks{};
    bool opened{};
    bool closed{};
};

// Read-only RWA execution observers. They mirror already accepted traffic;
// EeKernelHle remains the sole owner of guest-visible completion/timing.
using RwaCommandPacketObserver = std::function<void(std::span<const std::byte>)>;
using RwaIopUploadObserver =
    std::function<void(std::uint32_t destination, std::span<const std::byte>)>;
struct EeRwaShadowResponse {
    std::uint32_t opcode{};
    GuestAddress extra_destination{};
    std::vector<std::byte> extra_data{};
};
using RwaShadowResponseProvider =
    std::function<std::optional<EeRwaShadowResponse>(std::uint32_t inbound_opcode)>;
// Native opcode-0x11 device initialization hook. This is deliberately
// independent of the executed-IOP observers: evidence-mode shadow binding
// may replace those observers, while the native initializer must be installed
// only for the no-shadow route. It runs at the validated command boundary
// before EeKernelHle publishes RWA's 0x1a completion; false withholds that
// success response rather than claiming an uninitialized device is ready.
using RwaInitializeObserver = std::function<bool()>;

// Audio diagnostics: per-request journal for the virtual RWA stream
// path (opcode 0x32). Observer-only; guest-visible behavior is unchanged.
struct EeRwaStreamJournalEvent {
    enum class Phase : std::uint8_t { Request, Dropped, Completed };
    // Public names for the retail stream-object states carried in `state`.
    // Completion detail codes are operation-local (close and seek both use
    // detail 1), so consumers must identify lifecycle events by this state.
    enum class StreamState : std::uint32_t {
        Close = 1U,
        Read = 2U,
        SectorRead = 3U,
        Seek = 4U,
    };

    std::uint64_t sequence{};      // rwa_activity_.command_packets
    std::uint32_t slot{};
    std::uint32_t state{};         // stream-object state word
    std::uint32_t flags{};
    std::uint32_t seek{};
    std::uint32_t byte_count{};
    std::uint32_t file_offset{};   // read start offset (Completed/Dropped>=5)
    std::uint32_t destination{};   // stream-object destination (EE or IOP address)
    std::string path{};
    Phase phase{Phase::Request};
    std::uint32_t detail{};        // drop/complete reason code

    [[nodiscard]] constexpr bool has_state(const StreamState expected) const noexcept {
        return state == static_cast<std::uint32_t>(expected);
    }
};
using RwaStreamJournalObserver = std::function<void(const EeRwaStreamJournalEvent&)>;

enum class EeThreadStatus : std::uint32_t {
    Running = 0x01U,
    Ready = 0x02U,
    Waiting = 0x04U,
    Suspended = 0x08U,
    Dormant = 0x10U,
};

struct EeThreadState {
    std::int32_t id{};
    EeThreadStatus status{EeThreadStatus::Dormant};
    GuestAddress entry_point{};
    GuestAddress stack{};
    std::uint32_t stack_size{};
    GuestAddress global_pointer{};
    std::int32_t initial_priority{};
    std::int32_t current_priority{};
    std::uint32_t attributes{};
    GuestAddress option{};
    GuestAddress argument{};
};

enum class EeThreadDispatchResult : std::uint8_t {
    Continued,
    Switched,
    NoRunnable,
};

struct EeThreadRuntimeSnapshot {
    std::int32_t id{};
    GuestAddress program_counter{};
    GuestAddress stack_pointer{};
    GuestAddress return_address{};
    // Matches the TSW_* values exposed by ReferThreadStatus: none, sleep,
    // or semaphore for the scheduler subset currently modeled.
    std::uint32_t wait_type{};
    std::int32_t wait_id{};
    std::uint32_t wakeup_count{};
    bool context_saved{};
};

// Small, explicit high-level model of the PS2 BIOS calls needed by the
// executable's CRT. Unsupported calls remain visible service boundaries.
class EeKernelHle {
  public:
    // Identifies the physical HLE callback currently executing. A callback
    // kind is part of the kernel-owned route, so consumers can bind
    // callback-scoped diagnostics to DMAC dispatches without treating every
    // active callback as a DMAC callback.
    enum class GuestCallbackKind : std::uint8_t {
        None,
        SifCommand,
        Dmac,
        Intc,
        Alarm,
    };

    // Retail libkernel locates the real BIOS table at this KSEG0 address. The
    // HLE profile presents the same ABI without requiring a copyrighted BIOS.
    static constexpr GuestAddress kCompatibleSyscallTableBase = 0x80011f80U;
    static constexpr GuestAddress kGuestCallbackReturnAddress = 0xfffffff0U;

    explicit EeKernelHle(std::filesystem::path disc_root = {});

    // Data-mod file overrides (docs/MODDING.md). Every guest file open
    // (GTFS/CDVD open and the RWA stream open) resolves through the overlay
    // first, and the file's size and sector count come from the file that
    // was resolved. Not owned; null or an empty overlay means retail files
    // only. Set before guest execution starts.
    void set_mod_overlay(const takedown::content::ModOverlay* overlay) noexcept {
        mod_overlay_ = overlay;
    }

    [[nodiscard]] EeKernelCallResult handle_system_call(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] const EeKernelStartupState& startup_state() const noexcept;
    [[nodiscard]] const EeGsCrtState& gs_crt_state() const noexcept;
    [[nodiscard]] const EeVsyncFlagState& vsync_flag_state() const noexcept;
    [[nodiscard]] std::span<const EeSemaphoreState> semaphores() const noexcept;
    [[nodiscard]] std::span<const EeSyscallHandlerState> syscall_handlers() const noexcept;
    [[nodiscard]] std::span<const EeCacheMaintenanceEvent>
    cache_maintenance_events() const noexcept;
    [[nodiscard]] std::span<const EeIntcHandlerState> intc_handlers() const noexcept;
    [[nodiscard]] std::span<const EeDmacHandlerState> dmac_handlers() const noexcept;
    [[nodiscard]] std::span<const EeAlarmState> alarms() const noexcept;
    void set_ee_cycle_count(std::uint64_t cycles) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_alarm_deadline() const noexcept;
    [[nodiscard]] std::span<const EeSifRegisterState> sif_registers() const noexcept;
    [[nodiscard]] std::span<const EeSifDmaTransferState> sif_dma_transfers() const noexcept;
    [[nodiscard]] const EeRwaActivity& rwa_activity() const noexcept;
    [[nodiscard]] const EeGtfsActivity& gtfs_activity() const noexcept;
    [[nodiscard]] std::span<const EeGtfsOpenRecord> gtfs_open_records() const noexcept;
    void service_sif_dma_completions(EeMemory& memory);
    void set_rwa_stream_journal(RwaStreamJournalObserver observer) noexcept;
    // Fired once per materialized retail SPU transfer: opcode 0x05's
    // resident-IOP path, opcode 0x07's staged path, and opcode 0x46's explicit
    // descriptor path. The event contains only the operation's exact source,
    // destination, size and bytes; read history is not transfer authority.
    struct EeRwaSpuTransferMaterialized {
        std::uint32_t iop_source{};
        std::uint32_t spu_destination{};
        std::uint32_t byte_count{};
        // The exact bytes materialized into SPU2 RAM (the retail VoiceTrans
        // payload) so the app layer can bridge them into the modelled SPU2.
        std::vector<std::uint8_t> data;
    };
    using SpuTransferMaterializedObserver =
        std::function<void(const EeRwaSpuTransferMaterialized&)>;
    void set_spu_transfer_materialized(SpuTransferMaterializedObserver observer) noexcept;
    // Retail RWA opcode 0x14 commits a 0x680 snapshot. Its first 48 records
    // are 0x20-byte voice slots: +0 state, +4 SSA, +8 LSAX, +0xc source
    // rate, +0x10 packed volumes, +0x14 returned position, +0x18 requested
    // position, +0x1c flags. The voice number comes from the record index:
    // slots 0..23 are core 0, slots 24..47 are core 1.
    enum class EeRwaVoiceAction : std::uint8_t {
        KeyOff,
        KeyOn,
        SetPitch,
        SetLoopAddress,
        SetVolumeLeft,
        SetVolumeRight,
    };
    struct EeRwaVoicePlay {
        EeRwaVoiceAction action{EeRwaVoiceAction::KeyOn};
        std::uint8_t core{};
        std::uint8_t voice{};
        std::uint16_t pitch{};
        std::uint32_t start_address{};
        std::uint32_t loop_address{};
        std::uint16_t volume_left{};
        std::uint16_t volume_right{};
        std::uint16_t adsr1{};
        std::uint16_t adsr2{};
    };
    using RwaVoicePlayObserver = std::function<void(const EeRwaVoicePlay&)>;
    void set_rwa_voice_play(RwaVoicePlayObserver observer) noexcept;
    struct EeRwaVoiceStatus {
        std::uint16_t envelope{};
        // Raw libsd byte address returned by SD_VA_NAX.
        std::uint32_t current_address{};
    };
    using RwaVoiceStatusObserver =
        std::function<EeRwaVoiceStatus(std::uint8_t core, std::uint8_t voice)>;
    void set_rwa_voice_status(RwaVoiceStatusObserver observer) noexcept;
    struct EeRwaVoiceVolumeStatus {
        std::int32_t left{};
        std::int32_t right{};
    };
    using RwaVoiceVolumeStatusObserver =
        std::function<EeRwaVoiceVolumeStatus(std::uint8_t core, std::uint8_t voice)>;
    void set_rwa_voice_volume_status(RwaVoiceVolumeStatusObserver observer) noexcept;
    // Native equivalent of one retail RWA.IRX 0x1720 -> 0x3358 worker pass.
    // The app schedules this from the SPU2 sample clock, independently of
    // opcode 0x14 command traffic and presentation rate.
    void advance_rwa_voice_volume_ramps();
    // Diagnostic: called per fired play with the effective SSA after RWA's
    // requested-position transform (checks modelled SPU2 RAM residency).
    void set_rwa_voice_play_probe(std::function<void(std::uint32_t)> probe) noexcept;
    // Presents a save card to libmc2 (the MC2_S1 page device behind the
    // DBCMAN 0x8000131c service). Without one the game finds no card, as on
    // a console with an empty slot. Replays and measurements leave it unset
    // so the guest timeline stays identical.
    void set_save_card_device(std::shared_ptr<save::SaveCardDevice> device) noexcept {
        save_card_device_ = std::move(device);
    }
    // Plugs the save card in or out: DBCMAN publishes the change as the
    // status word of every live libmc2 socket in the EE work area, and
    // sockets libmc2 creates later start in that state. The card is only
    // reported when a device is set as well.
    void set_save_card_present(EeMemory& memory, bool present);
    [[nodiscard]] bool save_card_present() const noexcept {
        return save_card_present_;
    }
    // Sets the absolute GTFSCDVD vtable address once the executed retail
    // module is loaded (see gtfs_cdvd_interface_address_).
    void set_gtfs_cdvd_interface_address(std::uint32_t address) noexcept {
        gtfs_cdvd_interface_address_ = address;
    }
    // Absolute IOP address of executed RWA.IRX's static work area (module
    // offset 0xe100). Retail replies to opcode 0x15 with this value and the
    // game derives its stream-object DMA destinations from it — the live
    // savestate shows the retail work-area pointer 0x73230 in the stream
    // packets and the objects at work_area - 0x2f60. Default is the
    // historical module-relative placeholder.
    void set_rwa_iop_work_area(std::uint32_t address) noexcept {
        rwa_iop_work_area_ = address;
    }
    void set_rwa_execution_observers(RwaCommandPacketObserver packet,
                                     RwaIopUploadObserver upload,
                                     RwaShadowResponseProvider response = {}) noexcept;
    void set_rwa_initialize_observer(RwaInitializeObserver observer) noexcept;
    // Applies an executed-retail RWA response that arrived AFTER its inbound
    // packet was completed (field-paced handlers park under the guest master
    // clock and finish in later clock windows). Only opcode 0x14 is
    // content-routed today: a packet that found no synchronous shadow
    // response recorded a pending snapshot instead of silently completing
    // nothing, and this delivers it — writing the shadow's bytes and queueing
    // the retail command callback at the guest time the response actually
    // arrived. Returns false when nothing is pending or the response is
    // invalid (the record is then dropped, as the packet-time path would).
    [[nodiscard]] bool apply_deferred_rwa_shadow_response(const EeRwaShadowResponse& response,
                                                          EeMemory& memory);
    [[nodiscard]] std::span<const EeThreadState> threads() const noexcept;
    [[nodiscard]] std::vector<EeThreadRuntimeSnapshot>
    thread_runtime_snapshots(const R5900State& live_cpu) const;
    [[nodiscard]] std::int32_t current_thread_id() const noexcept;
    [[nodiscard]] std::uint32_t osd_config_param() const noexcept;
    [[nodiscard]] bool sif0_dma_chain_enabled() const noexcept;
    [[nodiscard]] std::uint32_t enabled_dmac_channels() const noexcept;
    [[nodiscard]] const std::array<std::uint64_t, 10>& dmac_callback_counts() const noexcept;
    [[nodiscard]] std::size_t pending_dmac_callback_count() const noexcept;
    [[nodiscard]] std::uint32_t pending_dmac_callback_mask() const noexcept;
    // Pure eligibility for already-latched callbacks only. This does not poll
    // the memory-side completion/countdown queue; callers must guard it too.
    [[nodiscard]] bool pending_dmac_callback_can_dispatch(const R5900State& cpu) const noexcept;
    [[nodiscard]] bool sif_dma_completion_pending() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    next_sif_dma_completion_deadline() const noexcept;
    // Conservative batch barrier: an equal-priority ready peer may become
    // runnable at a time-slice deadline, while a higher-priority one may preempt.
    [[nodiscard]] bool has_competing_ready_thread() const noexcept;
    // Earliest kernel clock value (set_ee_cycle_count) at which
    // dispatch_guest_threads() stops returning Continued, assuming no other
    // kernel state changes first; nullopt when no such time exists. Zero means
    // the very next call acts (a switch, or NoRunnable). Thread and ready-queue
    // state change only inside system calls, callback entry/exit and the
    // dispatchers, so between those points the equal-priority time slice is
    // the scheduler's only time-driven input. Pure: unlike the dispatcher it
    // does not pop stale ready-queue entries.
    [[nodiscard]] std::optional<std::uint64_t>
    next_thread_time_slice_deadline() const noexcept;
    // Pure mirror of dispatch_pending_intc_callback(): true exactly when that
    // call would enter a handler chain now. Its inputs are the callback-active
    // state, COP0 Status, INTC_STAT & INTC_MASK and the handler registry.
    [[nodiscard]] bool pending_intc_callback_can_dispatch(const R5900State& cpu,
                                                          const EeMemory& memory) const noexcept;
    [[nodiscard]] std::size_t pending_sif_command_callback_count() const noexcept;
    [[nodiscard]] bool sif_command_callback_poll_can_progress() const noexcept;
    [[nodiscard]] bool dispatch_pending_dmac_callback(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] bool dispatch_pending_intc_callback(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] bool dispatch_pending_alarm_callback(R5900State& cpu);
    [[nodiscard]] bool dispatch_pending_sif_command_callback(R5900State& cpu);
    [[nodiscard]] bool finish_guest_callback(R5900State& cpu) noexcept;
    [[nodiscard]] bool guest_callback_active() const noexcept;
    [[nodiscard]] GuestCallbackKind guest_callback_kind() const noexcept;
    // Stable for one physical kernel callback, including a suspended/resumed
    // SIF callback. Zero means normal thread execution.
    [[nodiscard]] std::uint64_t guest_callback_identity() const noexcept;
    // Monotonic count of callback activations. A suspended SIF callback keeps
    // its identity when resumed but advances this generation again, allowing
    // sparse observers to detect a callback that began and ended between two
    // ordinary-thread boundaries.
    [[nodiscard]] std::uint64_t guest_callback_activation_generation() const noexcept;
    [[nodiscard]] EeThreadDispatchResult dispatch_guest_threads(R5900State& cpu);
    [[nodiscard]] bool finish_guest_thread(R5900State& cpu) noexcept;

  private:
    void note_guest_callback_activation() noexcept;
    void begin_new_guest_callback_identity() noexcept;
    [[nodiscard]] EeKernelCallResult set_gs_crt(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult set_vsync_flag(R5900State& cpu, EeMemory& memory);
    void notify_vsync_flag(EeMemory& memory);
    [[nodiscard]] EeKernelCallResult setup_thread(R5900State& cpu, const EeMemory& memory);
    [[nodiscard]] EeKernelCallResult setup_heap(R5900State& cpu, const EeMemory& memory);
    [[nodiscard]] EeKernelCallResult end_of_heap(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult create_semaphore(R5900State& cpu, const EeMemory& memory);
    [[nodiscard]] EeKernelCallResult signal_semaphore_call(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult wait_semaphore(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult poll_semaphore(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult create_thread(R5900State& cpu, const EeMemory& memory);
    [[nodiscard]] EeKernelCallResult delete_thread(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult start_thread(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult exit_thread(R5900State& cpu, bool delete_record);
    [[nodiscard]] EeKernelCallResult terminate_thread(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult get_thread_id(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult refer_thread_status(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult change_thread_priority(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult sleep_thread(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult wakeup_thread(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult delete_semaphore(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult set_osd_config_param(R5900State& cpu, const EeMemory& memory);
    [[nodiscard]] EeKernelCallResult get_osd_config_param(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult get_osd_config_param2(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult add_intc_handler(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult remove_intc_handler(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult enable_intc(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult add_dmac_handler(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult remove_dmac_handler(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult enable_dmac(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult disable_dmac(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult set_alarm(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult release_alarm(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult flush_cache(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult put_gs_interrupt_mask(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult stop_sif_dma(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult set_sif_dma_chain(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult set_sif_register(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult get_sif_register(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult deci2_call(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult set_sif_dma(R5900State& cpu, EeMemory& memory);
    [[nodiscard]] EeKernelCallResult sif_dma_stat(R5900State& cpu, EeMemory& memory);
    void complete_sif_dma_batch(EeMemory& memory, std::int32_t id);
    void complete_virtual_rwa_staging_transfer(EeMemory& memory, std::int32_t id);
    void complete_virtual_rwa_op05_voice_transfer(EeMemory& memory, std::int32_t id);
    bool enqueue_rwa_sif_completion(
        EeMemory& memory, const std::array<std::uint8_t, 0x30U>& source_record,
        std::uint32_t type, std::uint32_t t0, std::uint32_t extra_source,
        std::uint32_t extra_destination, std::uint32_t remaining_size,
        std::uint32_t a2, std::uint32_t a3, bool terminal);
    [[nodiscard]] EeKernelCallResult set_syscall(R5900State& cpu);
    [[nodiscard]] EeKernelCallResult find_address(R5900State& cpu) const;
    [[nodiscard]] EeKernelCallResult copy_words(R5900State& cpu, EeMemory& memory) const;
    [[nodiscard]] EeKernelCallResult get_entry_address(R5900State& cpu,
                                                       const EeMemory& memory) const;
    [[nodiscard]] std::optional<std::int32_t> lowest_free_semaphore_id() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> lowest_free_intc_handler_id() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> lowest_free_dmac_handler_id() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> lowest_free_alarm_id() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> lowest_free_thread_id() const noexcept;
    [[nodiscard]] std::optional<std::size_t> thread_index(std::int32_t id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> semaphore_index(std::int32_t id) const noexcept;
    void enqueue_ready_thread(std::size_t index, bool front = false);
    void remove_ready_thread(std::size_t index) noexcept;
    void refresh_highest_ready_priority() noexcept;
    [[nodiscard]] std::optional<std::size_t> take_next_ready_thread() noexcept;
    void remove_thread_wait(std::size_t index) noexcept;
    void make_thread_dormant(std::size_t index) noexcept;
    static void copy_cop0_state(const R5900State& source, R5900State& destination);
    void enter_next_intc_handler(R5900State& cpu);
    [[nodiscard]] std::optional<GuestAddress>
    registered_syscall_handler(std::uint32_t number) const noexcept;
    [[nodiscard]] bool signal_semaphore(std::int32_t id);
    void complete_virtual_sif_rpc_init(const EeSifDmaTransferState& transfer, EeMemory& memory);
    void complete_virtual_rpc_bind(const EeSifDmaTransferState& transfer, EeMemory& memory);
    void complete_virtual_network_config_lifecycle_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory);
    void complete_virtual_b3_route_init_call(const EeSifDmaTransferState& transfer,
                                             EeMemory& memory);
    void complete_virtual_b3_route_routing_call(const EeSifDmaTransferState& transfer,
                                                EeMemory& memory);
    void complete_virtual_gtfs_cdvd_init_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    void complete_virtual_gtfs_cdvd_open_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    void complete_virtual_gtfs_cdvd_close_call(const EeSifDmaTransferState& transfer,
                                               EeMemory& memory);
    void complete_virtual_gtfs_cdvd_read_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    void complete_virtual_gtfs_cdvd_seek_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    void complete_virtual_cdvd_init_call(const EeSifDmaTransferState& transfer, EeMemory& memory);
    void complete_virtual_cdvd_scmd_call(const EeSifDmaTransferState& transfer, EeMemory& memory);
    void complete_virtual_cdvd_disk_ready_call(const EeSifDmaTransferState& transfer,
                                               EeMemory& memory);
    void complete_virtual_file_io_version_call(const EeSifDmaTransferState& transfer,
                                               EeMemory& memory);
    void complete_virtual_iop_heap_call(const EeSifDmaTransferState& transfer, EeMemory& memory);
    void complete_virtual_load_file_version_call(const EeSifDmaTransferState& transfer,
                                                 EeMemory& memory);
    void complete_virtual_load_file_module_call(const EeSifDmaTransferState& transfer,
                                                EeMemory& memory);
    void complete_virtual_dbc_man_version_call(const EeSifDmaTransferState& transfer,
                                               EeMemory& memory);
    void complete_virtual_dbc_man_set_work_address_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory);
    void complete_virtual_dbc_man_create_socket_call(const EeSifDmaTransferState& transfer,
                                                     EeMemory& memory);
    void complete_virtual_dbc_man_delete_socket_call(const EeSifDmaTransferState& transfer,
                                                     EeMemory& memory);
    void complete_virtual_dbc_man_get_dependency_call(const EeSifDmaTransferState& transfer,
                                                      EeMemory& memory);
    void complete_virtual_dbc_service_1c_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    // Writes one socket's DBCMAN status word for the save card's presence
    // (libmc2 sockets only).
    void publish_save_card_socket_status(EeMemory& memory, std::size_t socket);
    // Serves one MC2_S1 page operation from save_card_device_; returns the
    // response result word and fills the response bytes from +0x0c.
    [[nodiscard]] std::uint32_t serve_save_card_operation(std::uint32_t operation,
                                                          GuestAddress request,
                                                          GuestAddress read_data,
                                                          std::span<std::byte> response_data,
                                                          EeMemory& memory);
    void complete_virtual_lg_dev_version_call(const EeSifDmaTransferState& transfer,
                                              EeMemory& memory);
    void complete_virtual_lg_dev_enumerate_call(const EeSifDmaTransferState& transfer,
                                                EeMemory& memory);
    void complete_virtual_lg_dev_no_device_call(const EeSifDmaTransferState& transfer,
                                                EeMemory& memory);
    void complete_virtual_file_io_open_call(const EeSifDmaTransferState& transfer,
                                            EeMemory& memory);
    void complete_virtual_file_io_read_call(const EeSifDmaTransferState& transfer,
                                            EeMemory& memory);
    void complete_virtual_file_io_close_call(const EeSifDmaTransferState& transfer,
                                             EeMemory& memory);
    void complete_virtual_rwa_command(const EeSifDmaTransferState& transfer, EeMemory& memory,
                                      const R5900State& cpu);
    void probe_bank_loader_node(EeMemory& memory);
    [[nodiscard]] std::uint32_t sif_register_value(std::uint32_t number) const noexcept;
    void set_sif_register_value(std::uint32_t number, std::uint32_t value);
    void refresh_next_alarm_deadline() noexcept;

    EeKernelStartupState startup_{};
    EeGsCrtState gs_crt_{};
    EeVsyncFlagState vsync_flag_{};
    std::filesystem::path disc_root_{};
    const takedown::content::ModOverlay* mod_overlay_{nullptr};
    std::vector<EeSemaphoreState> semaphores_{};
    std::vector<EeSyscallHandlerState> syscall_handlers_{};
    std::vector<EeCacheMaintenanceEvent> cache_maintenance_events_{};
    std::vector<EeIntcHandlerState> intc_handlers_{};
    std::vector<EeDmacHandlerState> dmac_handlers_{};
    std::vector<EeAlarmState> alarms_{};
    std::optional<std::uint64_t> next_alarm_deadline_{};
    std::uint64_t ee_cycle_count_{};
    std::vector<EeSifRegisterState> sif_registers_{};
    std::vector<EeSifDmaTransferState> sif_dma_transfers_{};
    // The batches (one sceSifSetDma call, consecutive entries of
    // sif_dma_transfers_ sharing an id) not completed yet, oldest first. Ids
    // only grow, so an id that is not listed here was completed or never
    // existed; lookups never scan the whole transfer history.
    struct IncompleteSifDmaBatch {
        std::int32_t id{};
        std::size_t first{};
        std::size_t end{};
    };
    std::vector<IncompleteSifDmaBatch> incomplete_sif_dma_batches_{};
    struct PendingSifDmaBatch {
        std::int32_t id{};
        std::uint64_t deadline_ee_cycles{};
    };
    std::deque<PendingSifDmaBatch> pending_sif_dma_batches_{};
    struct PendingRwaStagingTransfer {
        std::int32_t sif_dma_id{};
        std::uint32_t staging_destination{};
        std::uint32_t spu_destination{};
        std::uint32_t ee_source{};
        std::uint32_t size{};
        std::uint32_t remaining_size{};
        std::array<std::uint8_t, 0x30U> response_record{};
        std::vector<std::uint8_t> bytes{};
    };
    std::vector<PendingRwaStagingTransfer> pending_rwa_staging_transfers_{};
    struct PendingRwaOp05VoiceTransfer {
        std::int32_t sif_dma_id{};
        std::uint32_t iop_source{};
        std::uint32_t spu_destination{};
        std::uint32_t size{};
        std::uint32_t t0{};
        std::uint32_t a2{};
        std::uint32_t a3{};
        std::array<std::uint8_t, 0x30U> response_record{};
    };
    std::vector<PendingRwaOp05VoiceTransfer> pending_rwa_op05_voice_transfers_{};
    EeRwaActivity rwa_activity_{};
    std::optional<EeSifDmaTransferState> pending_file_io_payload_{};
    std::optional<EeSifDmaTransferState> pending_iop_heap_payload_{};
    std::optional<EeSifDmaTransferState> pending_load_file_payload_{};
    std::optional<EeSifDmaTransferState> pending_b3_route_payload_{};
    // Absolute IOP address of GTFSCDVD's storage vtable, published through
    // the "STG" RPC cmd-1 reply and forwarded by the game in RWA command
    // 0x30's packet word +0x24 (executed retail RWA stores it verbatim into
    // its BSS 0xe0f0 — the live PCSX2 savestate shows 0x81f30 = the retail
    // GTFSCDVD base 0x78c30 + 0x9300). Default is the historical
    // module-relative placeholder used while the module was unmodeled.
    std::uint32_t gtfs_cdvd_interface_address_{0x00009300U};
    std::uint32_t rwa_iop_work_area_{0x0000e100U};
    // EE return buffer published in opcode 0x15 word +0x24. RWA.IRX stores
    // this pointer and uses it as opcode 0x14's outbound extra destination;
    // it is not derivable from the later snapshot source packet.
    std::optional<GuestAddress> rwa_voice_snapshot_destination_{};
    // Sequence for the game-internal stream-interface completion ring.
    std::uint32_t rwa_stream_completion_sequence_{};
    std::optional<EeSifDmaTransferState> pending_gtfs_cdvd_payload_{};
    std::optional<EeSifDmaTransferState> pending_network_config_payload_{};
    std::optional<EeSifDmaTransferState> pending_dbc_service_1c_payload_{};
    std::shared_ptr<save::SaveCardDevice> save_card_device_{};
    EeGtfsActivity gtfs_activity_{};
    std::vector<EeGtfsOpenRecord> gtfs_open_records_{};
    RwaCommandPacketObserver rwa_command_packet_observer_{};
    RwaIopUploadObserver rwa_iop_upload_observer_{};
    RwaShadowResponseProvider rwa_shadow_response_provider_{};
    RwaInitializeObserver rwa_initialize_observer_{};
    RwaStreamJournalObserver rwa_stream_journal_{};
    GuestAddress virtual_b3_route_target_{};
    // Raw words of the most recent B3ROUTE routing transaction (command 2):
    // payload+0x04 channel-mask word, payload+0x08 parameter word, payload+0x0c
    // destination word. Recorded for the future distortion-router audio work;
    // only the RPC completion itself is an EE-observable contract.
    std::uint32_t virtual_b3_route_routing_word0_{};
    std::uint32_t virtual_b3_route_routing_word1_{};
    std::uint32_t virtual_b3_route_routing_word2_{};
    struct VirtualGtfsFile {
        std::filesystem::path path{};
        std::uint32_t size{};
        std::uint32_t sector{};
        std::optional<std::size_t> activity_index{};
        bool open{};
    };
    // The retail init request creates one primary RPC file context. Its
    // separate three-entry pool belongs to the RWA streaming vtable.
    std::array<VirtualGtfsFile, 1> virtual_gtfs_files_{};
    // Retained host handles for guest CDVD and RWA stream reads. Shared so the
    // kernel stays copyable; created on first use.
    std::shared_ptr<takedown::platform::HostFileReader> host_file_reader_;
    struct VirtualRwaFile {
        std::string guest_path{};
        std::filesystem::path path{};
        std::uint32_t size{};
        std::uint64_t offset{};
        std::optional<std::size_t> activity_index{};
        bool open{};
    };
    std::array<VirtualRwaFile, 3> virtual_rwa_files_{};
    struct VirtualIopHeapBlock {
        std::uint32_t address{};
        std::vector<std::uint8_t> bytes{};
    };
    std::vector<VirtualIopHeapBlock> virtual_iop_heap_blocks_{};
    // Physical IOP RAM validity is distinct from allocator ownership: retail
    // RWA opcode 0x05 drives an IOP DMA to absolute RAM addresses, including
    // module/work regions not returned by the EE-side IOPHEAP client.
    std::vector<std::uint8_t> virtual_iop_ram_{};
    std::vector<std::uint8_t> virtual_iop_ram_valid_{};
    SpuTransferMaterializedObserver spu_transfer_materialized_observer_{};
    RwaVoicePlayObserver rwa_voice_play_observer_{};
    RwaVoiceStatusObserver rwa_voice_status_observer_{};
    RwaVoiceVolumeStatusObserver rwa_voice_volume_status_observer_{};
    std::function<void(std::uint32_t)> rwa_voice_play_probe_{};
    // RWA's per-voice runtime caches the last submitted source rate. The
    // module constructor loads 0x5622 at 0x2e50 and stores it here at
    // 0x2e88; opcode 0x14 writes pitch only when this cache changes (and
    // never for state 5).
    std::array<std::uint32_t, 48> mixer_slot_rates_{};
    struct RwaVoiceVolumeTarget {
        struct Channel {
            std::int32_t desired_target{};
            std::int32_t cached_magnitude{};
            bool desired_negative{};
            bool phase_negative{};
            bool in_progress{};
        };
        Channel left{};
        Channel right{};
        // RWA record +0x24 bit 0 forces both local targets to zero. The
        // supported retail module leaves it clear for all 48 BSS records.
        bool force_zero{};
        bool active{};
    };
    std::array<RwaVoiceVolumeTarget, 48> mixer_slot_volume_targets_{};
    // Arming-gate probe: over the session, how many 0x14 commits carried
    // key-on-pending slots, the max seen in one commit, and the raw state
    // histogram across every walked slot (counts BEFORE any filtering —
    // distinguishes "the game never arms" from "we misread the slots").
    std::uint64_t voice_pending_commits_{};
    std::uint32_t voice_pending_max_{};
    std::array<std::uint32_t, 8> voice_state_histogram_{};
    // The two SPU2 cores expose one 2 MiB address space. This is deliberately
    // only a RAM image, not a mixer: RWA.IRX's KON/KOFF and voice SSA calls
    // occur inside the unexecuted IOP module and therefore are not observable
    // at the current EE-only HLE boundary.
    std::vector<std::uint8_t> virtual_spu2_ram_{};
    std::int32_t next_virtual_iop_module_id_{1};
    struct VirtualDbcSocket {
        std::array<GuestAddress, 2> response_buffers{};
        std::uint32_t next_sequence{1U};
        std::size_t next_response_buffer{};
        std::uint32_t descriptor_type{};
        bool active{};
    };
    // Whether the save card is plugged in: published as the connected status
    // of every libmc2 socket (see set_save_card_present()).
    bool save_card_present_{};
    // DBCMAN's socket table has exactly sixteen reusable slots. Each socket
    // retains the two EE response buffers registered by sceDbcCreateSocket.
    std::array<VirtualDbcSocket, 16> virtual_dbc_sockets_{};
    // EE work area registered by sceDbcSetWorkAddr. DBCMAN publishes one
    // status word per socket there; libdbc treats 1 as "device connected".
    GuestAddress dbc_work_address_{};
    struct Deci2SessionState {
        std::int32_t socket{};
        std::uint16_t protocol{};
        GuestAddress option{};
        GuestAddress handler{};
        GuestAddress user_area{};
        bool locked{};
    };
    std::vector<Deci2SessionState> deci2_sessions_{};
    std::int32_t next_deci2_socket_id_{1};
    std::vector<EeThreadState> threads_{};
    // Guest cycles at the last thread switch: retail's threadman rotates
    // equal-priority threads on a time slice; the kernel ages the running
    // thread the same way before allowing an equal-priority preemption
    // (docs/archive/AUDIO_HISTORY.md increment 18 — without it, cooperative-only scheduling
    // starves the audio worker threads forever).
    std::uint64_t last_thread_switch_ee_cycles_{};
    enum class ThreadWaitReason : std::uint8_t {
        None,
        Sleep,
        Semaphore,
    };
    struct ThreadRuntimeState {
        std::optional<R5900State> cpu{};
        ThreadWaitReason wait_reason{ThreadWaitReason::None};
        std::int32_t wait_id{};
        std::uint32_t wakeup_count{};
    };
    std::vector<ThreadRuntimeState> thread_runtime_{};
    std::array<std::deque<std::int32_t>, 128> ready_thread_queues_{};
    // Numerically lower EE priorities run first. Keep the first non-empty
    // queue cached so the per-instruction scheduler poll does not walk all 128
    // priority buckets while their membership is unchanged.
    std::optional<std::size_t> highest_ready_priority_{};
    std::vector<std::deque<std::int32_t>> semaphore_waiters_{};
    std::int32_t current_thread_id_{};
    struct PendingSifCommandCallback {
        GuestAddress handler{};
        GuestAddress context{};
        std::uint32_t command{};
        std::uint32_t a2{};
        std::uint32_t a3{};
        std::uint32_t t0{};
        GuestAddress global_pointer{};
        std::uint32_t dispatch_delay{};
    };
    std::vector<PendingSifCommandCallback> pending_sif_command_callbacks_{};
    // Save-card page-device answers wait out the card's own latency (see
    // kSaveCardCommandEeCycles) before their end function is queued; libmc2
    // depends on its requests taking a few frames, as on a real card.
    struct DelayedSifCommandCallback {
        std::uint64_t ready_ee_cycles{};
        PendingSifCommandCallback callback{};
    };
    std::deque<DelayedSifCommandCallback> delayed_save_card_callbacks_{};
    // ConfigParam from the EE BIOS is a packed 32-bit OSD setting word. Use
    // the conventional OSD2/English/RGB/4:3 default until a host settings
    // layer owns persistent console preferences.
    std::uint32_t osd_config_param_{0x00012010U};
    // sceSifSetDChain enables SIF0's IOP-to-EE DMA chain. The reference
    // interpreter records the externally visible enable state; actual SIF
    // transfer scheduling belongs to the later IOP/DMA model.
    bool sif0_dma_chain_enabled_{};
    std::uint32_t enabled_dmac_channels_{};
    // A DMAC source remains latched if it completes before its mask and guest
    // handler are ready. Keep ordered occurrences because a callback can
    // synchronously start the next channel while it is executing.
    std::vector<std::uint32_t> pending_dmac_channels_{};
    std::uint32_t pending_dmac_channel_mask_{};
    std::array<std::uint64_t, 10> dmac_callback_counts_{};
    struct ActiveIntcDispatch {
        std::uint32_t cause{};
        GuestAddress interrupted_pc{};
        std::vector<EeIntcHandlerState> handlers{};
        std::size_t next_handler{};
    };
    struct SuspendedSifCommandCallback {
        R5900State continuation_cpu{};
        std::int32_t semaphore_id{};
        std::uint64_t callback_identity{};
        bool ready{};
    };
    std::optional<ActiveIntcDispatch> active_intc_dispatch_{};
    std::optional<R5900State> interrupted_cpu_state_{};
    std::optional<SuspendedSifCommandCallback> suspended_sif_command_callback_{};
    GuestCallbackKind active_guest_callback_kind_{GuestCallbackKind::None};
    std::uint64_t active_guest_callback_identity_{};
    std::uint64_t next_guest_callback_identity_{1U};
    std::uint64_t guest_callback_activation_generation_{};
    bool virtual_romver_open_{};
    std::size_t virtual_romver_offset_{};
    std::int32_t next_sif_dma_id_{1};
};

[[nodiscard]] std::string_view to_string(EeKernelCallStatus status) noexcept;
[[nodiscard]] std::string_view ee_kernel_call_name(std::uint32_t number) noexcept;

} // namespace takedown::guest
