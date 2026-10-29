#pragma once

#include "takedown/guest/dmac_source_chain.hpp"
#include "takedown/guest/ee_aot_memory_view.hpp"
#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_packet_queue.hpp"
#include "takedown/guest/reusable_fifo.hpp"
#include "takedown/runtime/slus_21050_strip_submission.hpp"
#include "takedown/guest/gs_privileged_display.hpp"
#include "takedown/guest/ipu_mpeg2_decoder.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/guest/vu1_micro_interpreter.hpp"
#include "takedown/runtime/graphics_async.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::runtime {
class EeGraphicsServiceDiagnostics;
struct Vu1WorkerAccessWindow;
struct Vu1WorkerJournal;
enum class Vu1WorkerConsumeReason : std::uint8_t;
}

namespace takedown::guest {

class Vu0MicroCallObserver;
#if defined(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS) && TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS
class Vif1PureUnpackCensus;
#endif

inline constexpr std::size_t kEeMainRamSize = 32U * 1024U * 1024U;
inline constexpr std::size_t kEeScratchpadSize = 16U * 1024U;
inline constexpr std::size_t kVu0MemorySize = 4U * 1024U;
// The EE exposes each 4 KiB VU0 memory through a mirrored 16 KiB window.
// Keep these architectural bases public so VIF0, the micro interpreter, and
// diagnostics cannot silently disagree about which window is code or data.
inline constexpr GuestAddress kVu0MicroMemoryStart = 0x11000000U;
inline constexpr GuestAddress kVu0DataMemoryStart = 0x11004000U;
// VU1 carries 16 KiB of instruction memory (MicroMem1) and 16 KiB of data
// memory (VUMem1), four times the VU0 on-chip sizes.
inline constexpr std::size_t kVu1MemorySize = 16U * 1024U;

// Canonical storage domains reported by the opt-in scalar EE write observer.
// Main-RAM aliases and the modeled TLB aliases are collapsed into MainRam;
// MMIO and the GS privileged window remain distinct because their register
// offsets have different architectural meanings.
enum class EeMemoryWriteDomain : std::uint8_t {
    MainRam,
    Scratchpad,
    Vu0Micro,
    Vu0Data,
    Vu1Micro,
    Vu1Data,
    EeMmio,
    GsPrivileged,
};

[[nodiscard]] std::string_view to_string(EeMemoryWriteDomain domain) noexcept;

inline constexpr std::size_t kEeMemoryWritePayloadBytes = 16U;

// One successful logical scalar write. `width` is the number of valid bytes
// in each payload; payload bytes are always little-endian and the remaining
// tails are zero. Side-effect-free storage domains also preserve their bytes
// from immediately before the write and a bit per changed byte, allowing a
// bounded journal to reconstruct overlapping writes and the net memory delta.
// MMIO writes deliberately mark the previous payload unavailable rather than
// invoking a potentially stateful device read. `original_address` is retained
// before alias normalization so a trace can distinguish the guest pointer
// used by the instruction from the canonical domain/offset used for analysis.
struct EeMemoryWriteEvent {
    std::uint64_t guest_step{};
    std::array<std::uint8_t, kEeMemoryWritePayloadBytes> payload{};
    std::array<std::uint8_t, kEeMemoryWritePayloadBytes> previous_payload{};
    GuestAddress original_address{};
    std::uint32_t offset{};
    GuestAddress pc{};
    std::uint16_t changed_byte_mask{};
    EeMemoryWriteDomain domain{EeMemoryWriteDomain::MainRam};
    std::uint8_t width{};
    bool previous_payload_available{};
};

// Synchronous, non-owning scalar-write tap. The callback is invoked only
// after the complete public scalar write succeeds. Callbacks are noexcept and
// non-reentrant: writes issued from a callback are intentionally suppressed.
class EeMemoryWriteObserver {
  public:
    virtual ~EeMemoryWriteObserver() = default;
    virtual void on_ee_memory_write(const EeMemoryWriteEvent& event) noexcept = 0;
};

// Cumulative, behavior-neutral evidence of guest use of the EE's IPU path.
// The readiness model does not decode MPEG yet, so these counters deliberately
// distinguish command/DMA traffic from actual decoded-picture production.
struct EeIpuActivity {
    std::uint64_t control_writes{};
    std::uint64_t reset_writes{};
    std::uint64_t command_writes{};
    std::array<std::uint64_t, 16> command_opcode_counts{};
    std::uint64_t input_fifo_quadwords{};
    std::uint64_t from_ipu_dma_starts{};
    std::uint64_t from_ipu_dma_completions{};
    std::uint64_t from_ipu_quadwords_requested{};
    std::uint64_t to_ipu_dma_starts{};
    std::uint64_t to_ipu_dma_completions{};
    std::uint64_t to_ipu_quadwords_requested{};
    // Command-0x7 macroblock-row decode: rows produced by the decoder seam
    // (decoded) vs rows whose pixels were actually written into guest RAM at
    // the from-IP DMA destination (placed). A placed row is real decoded
    // output the GS presentation path can upload; a decoded-but-unplaced row
    // means the guest issued 0x7 without a from-IP transfer setup.
    std::uint64_t macroblock_rows_decoded{};
    std::uint64_t macroblock_rows_placed{};
    std::uint32_t last_command{};
};

// Cumulative, behavior-neutral evidence of guest use of the VIF1->VU1 path.
// VU1 is the geometry unit behind the world-map globe and garage scenes; until
// a micro interpreter reaches completion these counters show how far the
// retail VIF1 chains actually drive the unit.
struct EeVu1Activity {
    struct FaultSignature {
        std::uint64_t count{};
        std::uint32_t entry{};
        std::uint32_t pc{};
        std::uint32_t upper_word{};
        std::uint32_t lower_word{};
        Vu1RunStatus status{Vu1RunStatus::Completed};
        Vu1Slot slot{Vu1Slot::None};
    };

    std::uint64_t vif1_mpg_uploads{};
    std::uint64_t vif1_micro_words_written{};
    // VIF1 UNPACK (0x60-0x7F) expands compressed world-map/garage vertex data
    // into VU Mem1. The command count and the datum words written give the
    // evidence that the retail compressed streams actually landed in VUMem1.
    std::uint64_t vif1_unpack_commands{};
    std::uint64_t vif1_unpack_vectors_written{};
    std::uint64_t vif1_vumem_words_written{};
    std::uint64_t micro_runs_started{};
    std::uint64_t native_strip_submission_jobs{};
    std::uint64_t native_strip_execution_jobs{};
    // Launches drawn natively instead (Vu1LaunchReplacement).
    std::uint64_t native_replaced_launches{};
    std::uint64_t native_strip_submission_packets{};
    std::uint64_t native_strip_submission_quadwords{};
    std::uint64_t native_strip_scratch_reset_jobs{};
    std::uint64_t micro_runs_completed{};
    std::uint64_t micro_run_faults{};
    std::uint64_t micro_pairs_committed{};
    std::uint64_t micro_cycles_elapsed{};
    std::uint64_t micro_pipeline_stall_cycles{};
    std::uint64_t xgkick_packets{};
    std::uint64_t xgkick_packets_completed{};
    std::uint64_t xgkick_quadwords{};
    std::uint64_t xgkick_faults{};
    std::uint64_t xgkick_oversized_tags{};
    std::uint64_t xgkick_oversized_tags_present_at_submit{};
    std::uint64_t xgkick_start_tags_changed_before_transfer{};
    std::uint32_t xgkick_first_vector{};
    // First fault retains the earliest unsupported boundary; last fault shows
    // where repeated later launches currently end. A retail frame can launch
    // several different microprograms, so both positions are useful.
    std::uint32_t first_fault_entry{};
    std::uint32_t first_fault_pc{};
    std::uint32_t first_fault_upper_word{};
    std::uint32_t first_fault_lower_word{};
    Vu1RunStatus first_fault_status{Vu1RunStatus::Completed};
    Vu1Slot first_fault_slot{Vu1Slot::None};
    // The last fault's stop position, for the boundary report.
    std::uint32_t last_fault_pc{};
    std::uint32_t last_fault_upper_word{};
    std::uint32_t last_fault_lower_word{};
    // Keep a bounded frequency table of exact stop signatures. This turns a
    // long retail replay into one useful instruction-coverage report instead
    // of requiring a full rerun for every distinct microprogram boundary.
    static constexpr std::size_t kFaultSignatureCapacity = 16U;
    std::array<FaultSignature, kFaultSignatureCapacity> fault_signatures{};
    std::uint32_t fault_signature_count{};
    std::uint64_t fault_signatures_dropped{};
    // The first MSCAL/MSCALF entry point the retail VIF1 chain requested,
    // captured before any micro run executes, so we can see where VU1
    // geometry programs are supposed to start even when the microcode is
    // absent or faults immediately.
    std::uint32_t first_mscal_entry{};
    bool saw_mscal{};
    // Diagnostics for why VU1 stays idle (what the retail VIF1 driver actually
    // wrote and how the chain loader reacted) -- filled in by the VIF1 DMAC
    // path in ee_memory.cpp.
    std::uint64_t vif1_chcr_start_writes{};
    std::uint64_t vif1_chcr_non_chain_entries{};
    std::uint64_t vif1_chain_tags_processed{};
    std::uint64_t vif1_chain_words_submitted{};
    std::uint64_t vif1_chain_completions{};
    std::uint64_t vif1_chain_faults{};
    std::array<std::uint64_t, 8> vif1_chain_tag_id_counts{};
    // The first source-chain failure. Reason values are kept stable for local
    // diagnostics: 1 tag OOB, 2 payload OOB, 3 address overflow, 4 tag limit,
    // 5 QWC limit, 6 CALL stack overflow, 7 reserved, 8 cursor loop.
    std::uint32_t vif1_first_chain_fault_reason{};
    std::uint32_t vif1_first_chain_fault_tadr{};
    // VIF1 chain stop-reason census, indexed by DmacSourceStop, so every walk
    // stop (normal end and fault alike) is visible per reason. The shared
    // walker accepts every tag id 0..7, so unlike the pre-shared-walker census
    // there is no "unsupported tag id" stop; REFE (id 0) surfaces as the
    // walker's Refe stop instead.
    std::array<std::uint32_t, static_cast<std::size_t>(DmacSourceStop::CursorLoop) + 1U>
        vif1_chain_stop_counts{};
    // Raw dump of the first 12 tag quadwords (w0..w3) of the first three
    // distinct chain-start TADRs: the exact retail tag stream shape.
    static constexpr std::size_t kVif1ChainDumpChains = 3U;
    static constexpr std::size_t kVif1ChainDumpTags = 12U;
    struct Vif1ChainTagRecord {
        std::uint32_t tadr{};
        std::uint32_t w0{};
        std::uint32_t w1{};
        std::uint32_t w2{};
        std::uint32_t w3{};
    };
    struct Vif1ChainDump {
        std::uint32_t start_tadr{};
        std::uint16_t tag_count{};
        std::uint8_t break_stop{};
    };
    std::array<Vif1ChainDump, kVif1ChainDumpChains> vif1_chain_dumps{};
    std::uint32_t vif1_chain_dump_count{};
    std::array<std::array<Vif1ChainTagRecord, kVif1ChainDumpTags>, kVif1ChainDumpChains>
        vif1_chain_dump_tags{};
    std::uint64_t vif1_unsupported_commands{};
    // The first word the VIF1 command decoder could not dispatch (the exact
    // retail instruction that gates every chain before any MPG upload).
    std::uint32_t vif1_first_unsupported_word{};
    // Bit histogram of every VIF1 command byte (0..0x7f) the decoder saw, used
    // to enumerate the exact retail instruction set before implementing it.
    std::uint64_t vif1_command_bits[2]{};
    // First few TADR values the VIF1 chain walker was started with, to identify
    // which chains the game kicks and whether any carry MPG.
    static constexpr std::size_t kVif1TadrHistorySize = 8U;
    std::array<std::uint32_t, kVif1TadrHistorySize> vif1_first_tadrs{};
    std::uint32_t vif1_tadr_history_count{};
    // Direct EE writes to VU1 micro memory (0x11008000): the alternative
    // microcode upload path when VIF1 MPG is absent.
    std::uint64_t ee_vu1_micro_writes{};
    std::uint32_t ee_vu1_micro_first_addr{};
    // Every write the game makes to VIF1 TADR (0x10009030) and MADR (0x10009010),
    // captured with the EE PC, to identify which chain roots the DMA scheduler
    // actually enqueues.
    static constexpr std::size_t kVif1RegWriteLogSize = 16U;
    struct Vif1RegWriteEntry {
        std::uint32_t value{};
        std::uint32_t pc{};
    };
    std::array<Vif1RegWriteEntry, kVif1RegWriteLogSize> vif1_tadr_write_log{};
    std::uint32_t vif1_tadr_write_count{};
    std::array<Vif1RegWriteEntry, kVif1RegWriteLogSize> vif1_madr_write_log{};
    std::uint32_t vif1_madr_write_count{};
    // Direct EE writes to the VIF1 FIFO are a separate input path from DMAC
    // source chains. Preserve a bounded PC/value history while counting all
    // words so a missing upload cannot hide outside the DMA-only telemetry.
    std::uint64_t vif1_fifo_words_submitted{};
    std::array<Vif1RegWriteEntry, kVif1RegWriteLogSize> vif1_fifo_write_log{};
    std::uint32_t vif1_fifo_write_log_count{};

    // Source provenance for GIF IMAGE data carried by VIF1 DIRECT/PATH2.
    // Main-RAM addresses are physical and bit 31 marks scratchpad, matching
    // DMAtag ADDR's SPR encoding.  Only all-zero IMAGE spans are retained:
    // this keeps the report bounded while identifying the exact producer
    // ranges behind Burnout's missing frontend texture tiles.
    struct Vif1ZeroImageSourceSpan {
        std::uint32_t first_source{};
        std::uint32_t last_source{};
        std::uint32_t chain_tadr{};
        std::uint64_t quadwords{};
    };
    static constexpr std::size_t kVif1ZeroImageSourceSpanCapacity = 32U;
    std::uint64_t vif1_path2_image_quadwords{};
    std::uint64_t vif1_path2_zero_image_quadwords{};
    std::uint64_t vif1_path2_unknown_image_sources{};
    std::array<Vif1ZeroImageSourceSpan, kVif1ZeroImageSourceSpanCapacity>
        vif1_zero_image_source_spans{};
    std::uint32_t vif1_zero_image_source_span_count{};
    std::uint64_t vif1_zero_image_source_spans_dropped{};
};

// Exact provenance at one VU1 XGKICK issue.  The VIF command source is the
// guest address of the MSCAL/MSCALF word whose synchronous microprogram run
// reached XGKICK; the containing DMAtag identifies the dynamic source-chain
// packet which referenced that program.  Direct FIFO launches legitimately
// have no source address.
struct Vu1XgkickObservation {
    std::uint16_t vector_address{};
    std::uint16_t micro_entry{};
    std::uint16_t micro_resolved_entry{};
    Vu1ProgramHash micro_program_hash{};
    std::uint64_t micro_run_sequence{};
    std::uint32_t vif_command_source{};
    std::uint32_t vif_chain_root{};
    std::uint32_t vif_tag_location{};
    GsQuadword initial_tag{};
    bool micro_program_hash_known{};
    bool vif_command_source_known{};
    bool vif_chain_root_known{};
    bool vif_tag_location_known{};
};

// Opt-in producer provenance for one VIF1 source-chain snapshot.  Unlike the
// XGKICK observer, this boundary is reached even when the retail renderer
// culls an object before VU1 emits any geometry.  `last_writer_pc` identifies
// the EE store which most recently built the tag quadword; scratchpad DMA
// copies carry the originating writer through to their destination.
struct Vif1DmacChainObservation {
    std::uint32_t chain_root{};
    GuestAddress start_pc{};
};

struct Vif1DmacTagObservation {
    std::uint32_t chain_root{};
    std::uint32_t ordinal{};
    DmacSourceTag tag{};
    GuestAddress last_writer_pc{};
    bool last_writer_pc_known{};
};

struct Vif1DmacChainResultObservation {
    std::uint32_t chain_root{};
    DmacSourceStop stop{DmacSourceStop::TagOutOfRange};
    std::size_t tags_processed{};
    std::size_t payload_quadwords{};
};

class Vif1DmacChainObserver {
  public:
    virtual ~Vif1DmacChainObserver() = default;
    [[nodiscard]] virtual bool
    on_vif1_dmac_chain_begin(const Vif1DmacChainObservation& observation) noexcept = 0;
    virtual void
    on_vif1_dmac_chain_tag(const Vif1DmacTagObservation& observation) noexcept = 0;
    virtual void on_vif1_dmac_chain_end(
        const Vif1DmacChainResultObservation& observation) noexcept = 0;
};

// A verified handoff point for a native replacement of one VIF1 DIRECT/PATH2
// GIF quadword.  The marker is intentionally transport-only: game-specific
// code owns the transaction associated with `generation`, while EeMemory
// proves that the requested source bytes reached the selected PATH2 FIFO
// position before notifying it.
//
// `source` is the exact byte address carried by VIF1 provenance. Bit 31
// selects scratchpad and the low bits are retained because a DIRECT GIFtag
// can begin in a non-zero word lane of a source quadword. `chain_root` is
// the normalized VIF1 TADR that started the source walk.
// `source_tag_location`, `source_tag_ordinal`, and
// `source_payload_word_ordinal` identify one dynamic source occurrence: a
// CALL/RET chain can revisit one tag address, and a scratchpad payload can
// wrap to the same byte address more than once. The two ordinal fields are
// zero-based: tag ordinal counts dynamic visits from the chain root and
// payload-word ordinal counts 32-bit words within that tag's payload. Both
// address values are physical DMAC addresses, not EE virtual pointers.
struct Vif1Path2SourceMarkerRequest {
    std::uint32_t source{};
    std::uint32_t chain_root{};
    std::uint32_t source_tag_location{};
    std::uint32_t source_tag_ordinal{};
    std::uint32_t source_payload_word_ordinal{};
    GsQuadword expected_quadword{};
};

enum class Vif1Path2SourceMarkerArmStatus : std::uint8_t {
    Armed,
    ObserverUnavailable,
    GsCaptureDisabled,
    SourceUnaligned,
    SourceOutOfRange,
    SourceContentsMismatch,
    MutableMemoryViewEscaped,
    TimedSnapshotAlreadyStarted,
    DuplicateSource,
    CapacityExhausted,
    GenerationExhausted,
};

struct Vif1Path2SourceMarkerArmResult {
    Vif1Path2SourceMarkerArmStatus status{Vif1Path2SourceMarkerArmStatus::ObserverUnavailable};
    // A nonzero, never-reused marker identity for the lifetime of this
    // EeMemory.  The native transaction registry uses it as its key.
    std::uint64_t generation{};

    [[nodiscard]] bool armed() const noexcept {
        return status == Vif1Path2SourceMarkerArmStatus::Armed;
    }
};

// A bounded, opt-in request to locate one native producer's QW in the VIF1
// source snapshot being taken by a timed CHCR start. `source` is a canonical
// EE guest address: main-RAM aliases are accepted and scratchpad uses the
// 0x70000000 guest window. EeMemory converts it to DMAC provenance only after
// the observer returns, so game-specific code never depends on the DMAC's
// private bit-31 scratchpad encoding. `token` is copied without interpretation
// and lets the observer associate a successful marker with its own claim.
inline constexpr std::size_t kVif1DmacPreSnapshotCandidateCapacity = 64U;

struct Vif1DmacPreSnapshotObservation {
    std::uint32_t chain_root{};
    GuestAddress start_pc{};
};

struct Vif1DmacPreSnapshotCandidate {
    std::uint64_t token;
    GuestAddress source;
    GsQuadword expected_quadword;
};

// Emitted only after the existing source walk found exactly one occurrence,
// its copied bytes matched, and EeMemory installed a marker already bound to
// that immutable timed snapshot.
struct Vif1DmacPreSnapshotResolvedMarker {
    std::uint64_t token{};
    std::uint64_t generation{};
    // The exact caller-facing GuestAddress originally offered for this token.
    // `request.source` is the canonical DMAC address used by the snapshot;
    // retaining both lets game-specific owners validate identity without
    // duplicating EeMemory's RAM-alias and scratchpad translation rules.
    GuestAddress candidate_source{};
    Vif1Path2SourceMarkerRequest request{};
};

enum class Vif1DmacPreSnapshotMarkerDecision : std::uint8_t {
    Reject,
    Accept,
};

enum class Vif1Path2SourceMarkerCancelReason : std::uint8_t {
    ExplicitlyCancelled,
    SourceOverwritten,
    MutableMemoryView,
    SourceContentsMismatch,
    GsCaptureDisabled,
    GsReset,
    GifReset,
    VifReset,
    Vif1DmacCancelled,
    Vif1DmacFault,
    Vif1DmacCompletedWithoutMatch,
    ObserverDetached,
    MemoryCleared,
};

struct Vif1Path2SourceMarkerReached {
    std::uint64_t generation{};
    std::uint32_t source{};
    std::uint32_t chain_root{};
    std::uint32_t source_tag_location{};
    std::uint32_t source_tag_ordinal{};
    std::uint32_t source_payload_word_ordinal{};
    GsQuadword quadword{};
};

struct Vif1Path2SourceMarkerCancelled {
    std::uint64_t generation{};
    std::uint32_t source{};
    std::uint32_t chain_root{};
    std::uint32_t source_tag_location{};
    std::uint32_t source_tag_ordinal{};
    std::uint32_t source_payload_word_ordinal{};
    Vif1Path2SourceMarkerCancelReason reason{
        Vif1Path2SourceMarkerCancelReason::ExplicitlyCancelled};
};

class EeMemory;

// Called synchronously before every VU1 microprogram launch (MSCAL, MSCALF or
// MSCNT) while an observer is registered. Game-specific code uses it to run a
// microprogram natively (src/runtime/slus_21050_native_cars.cpp): returning a
// replacement makes EeMemory apply it instead of running the program.
// Implementations must not change EeMemory state.
struct Vu1Launch {
    // VU1 program and data memory and registers as the launch sees them.
    std::span<const std::byte> micro;
    std::span<const std::byte> data;
    const Vu1State& state;
    std::uint16_t entry{0U};
    // VIF1 TOP and ITOP (what XTOP and XITOP read).
    std::uint16_t top{0U};
    std::uint16_t itop{0U};
    // Where the launch's vertex data came from: the address of the command
    // word of the last UNPACK since the previous launch (or of its DMA tag
    // when the word's own address is not known) when it lies in the
    // registered source range, else 0.
    std::uint32_t source{0U};
};

// The program's effects that later launches and the GS depend on, when it
// runs natively.
struct Vu1LaunchReplacement {
    // A complete GIF packet (ending in EOP) sent on PATH1, or empty.
    std::vector<std::array<std::uint32_t, 4>> packet;
    // Data memory quadword writes, applied in order (address wraps at 1024),
    // then the 32-bit writes: word index (quadword * 4 + lane), value.
    std::vector<std::pair<std::uint16_t, std::array<std::uint32_t, 4>>> quadword_writes;
    std::vector<std::pair<std::uint16_t, std::uint32_t>> data_writes;
    // Integer register writes: register, value.
    std::vector<std::pair<std::uint8_t, std::uint16_t>> integer_writes;
    // Vector register writes: register, lanes x to w.
    std::vector<std::pair<std::uint8_t, std::array<std::uint32_t, 4>>> vector_writes;
    // The CLIP flag register after the program, when it changed it.
    std::optional<std::uint32_t> clip_flags;
    // Where an MSCNT continues afterwards (the program's end address).
    std::uint16_t continuation{0U};
};

class Vu1LaunchObserver {
  public:
    virtual ~Vu1LaunchObserver() = default;
    virtual std::optional<Vu1LaunchReplacement> before_vu1_launch(const EeMemory& memory,
                                                                  const Vu1Launch& launch) = 0;
    // After a launch the observer did not replace has run (`launch` shows
    // VU1 memory and registers as the program left them).
    virtual void after_vu1_launch(const EeMemory&, const Vu1Launch&) {}
};

// Callbacks occur synchronously. Reached is emitted at the actual arbitrated
// PATH2 position immediately before GsCapture receives the marked word;
// cancelled is emitted once when the marker can no longer be delivered.
// Callbacks are non-reentrant: they must not call EeMemory methods, mutate an
// observer registration, or start/cancel another marker. Implementations must
// be noexcept so a host replacement cannot perturb guest scheduling.
class Vif1Path2SourceMarkerObserver {
  public:
    virtual ~Vif1Path2SourceMarkerObserver() = default;
    // Called before EeMemory allocates or initializes its bounded resolver.
    // Existing marker observers default to preserving the candidate callback;
    // registries that know they have no published work can decline it cheaply.
    [[nodiscard]] virtual bool
    has_vif1_dmac_pre_snapshot_candidates() const noexcept {
        return true;
    }
    // Called after CHCR state, the source cursor, and walk options are stable,
    // but before timed capture becomes active and before its first source
    // read. Return the number of initialized leading entries in `candidates`;
    // values above the fixed span size are clamped. The default keeps the
    // ordinary runtime and existing marker observers completely passive.
    [[nodiscard]] virtual std::size_t on_vif1_dmac_pre_snapshot_candidates(
        const Vif1DmacPreSnapshotObservation& observation,
        std::span<Vif1DmacPreSnapshotCandidate> candidates) noexcept {
        static_cast<void>(observation);
        static_cast<void>(candidates);
        return 0U;
    }
    // Every marker in this batch exists and is snapshot-bound during the one
    // callback. EeMemory invokes it only when the full uniquely resolved set
    // can be provisioned. Marker identities are immutable; the same-size
    // decision span starts entirely Reject, and only exact Accept entries are
    // retained. Rejected markers are removed synchronously without a
    // cancellation callback because ownership never transferred. Every
    // exposed generation remains consumed and is never reused.
    virtual void on_vif1_dmac_pre_snapshot_markers_resolved(
        std::span<const Vif1DmacPreSnapshotResolvedMarker> markers,
        std::span<Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept {
        static_cast<void>(markers);
        static_cast<void>(decisions);
    }
    // EeMemory retains its observer across clear(), but every packet address
    // and mutable-ownership claim becomes invalid. This notification follows
    // cancellation of any armed markers so an attached owner can discard its
    // remaining reserved and published state without touching EeMemory.
    virtual void on_vif1_path2_source_marker_memory_cleared() noexcept {}
    virtual void
    on_vif1_path2_source_marker_reached(const Vif1Path2SourceMarkerReached& marker) noexcept = 0;
    virtual void on_vif1_path2_source_marker_cancelled(
        const Vif1Path2SourceMarkerCancelled& marker) noexcept = 0;
};

// PCSX2's normal microVU path delays XGKICK by one issued microinstruction,
// then copies the complete GIF packet out of VUMem in one operation.  Its
// XGKickHack path instead exposes the hardware-timed live producer, where one
// quadword becomes eligible every two VU cycles and later VUMem stores can
// still change unread data.  Burnout 3 has no XGKickHack game fix, so the
// delayed whole-packet model is the architectural default for this runtime.
enum class Vu1XgkickMode : std::uint8_t {
    DelayedSnapshot,
    TimedLive,
};

// Optional, synchronous diagnostic tap at the PATH1 producer boundary.  A
// false begin result declines the packet and avoids every per-QW callback.
// Accepted packets receive the exact live VUMem words in producer order,
// followed by one completion result.  Callbacks are noexcept so diagnostics
// can never alter guest control flow.
class Vu1XgkickObserver {
  public:
    virtual ~Vu1XgkickObserver() = default;
    [[nodiscard]] virtual bool
    on_vu1_xgkick_begin(const Vu1XgkickObservation& observation) noexcept = 0;
    virtual void on_vu1_xgkick_quadword(const GsQuadword& quadword) noexcept = 0;
    virtual void on_vu1_xgkick_end(bool completed) noexcept = 0;
};

// One immutable-code observation for a physical 4 KiB EE main-RAM page.
// The fields are deliberately private: this is evidence issued by EeMemory,
// not an authority a caller can manufacture from an address and counter.
// The default value is inert, while issued values remain trivially copyable
// so a native admission token can retain a fixed array without allocation.
class EeMainRamCodePageToken {
  public:
    constexpr EeMainRamCodePageToken() noexcept = default;
    [[nodiscard]] constexpr bool operator==(
        const EeMainRamCodePageToken&) const noexcept = default;

  private:
    friend class EeMemory;
    std::uint64_t memory_domain_id_{};
    std::uint64_t generation_{};
    std::uint32_t physical_page_{};
};

enum class EeMainRamCodePageCaptureStatus : std::uint8_t {
    Captured,
    AddressOutOfRange,
    MutableViewEscaped,
    GenerationExhausted,
    MemoryDomainUnavailable,
};

struct EeMainRamCodePageCapture {
    EeMainRamCodePageCaptureStatus status{
        EeMainRamCodePageCaptureStatus::MemoryDomainUnavailable};
    EeMainRamCodePageToken token{};
};

// Opaque evidence for one exact, non-empty main-RAM byte range. The normalized
// physical offset, length, memory domain, and global mutation generation are
// private so callers cannot manufacture a token that names an arbitrary range
// or replay an old generation. The token remains trivially copyable for fixed
// native admission records.
class EeMainRamMutationToken {
  public:
    constexpr EeMainRamMutationToken() noexcept = default;
    [[nodiscard]] constexpr bool operator==(
        const EeMainRamMutationToken&) const noexcept = default;

  private:
    friend class EeMemory;
    std::uint64_t memory_domain_id_{};
    std::uint32_t physical_offset_{};
    std::uint32_t length_{};
    std::uint64_t generation_{};
};

enum class EeMainRamMutationCaptureStatus : std::uint8_t {
    Captured,
    ZeroLength,
    AddressOutOfRange,
    MutableViewEscaped,
    GenerationExhausted,
    MemoryDomainUnavailable,
};

struct EeMainRamMutationCapture {
    EeMainRamMutationCaptureStatus status{
        EeMainRamMutationCaptureStatus::MemoryDomainUnavailable};
    EeMainRamMutationToken token{};
};

// Read-only classification of the next graphics-service boundary. The result
// is intentionally a small value type: callers on the guest hot path can
// inspect it without constructing an optional or allocating storage.
enum class EeGraphicsServiceDeadlineKind : std::uint8_t {
    Immediate,
    InternalDeadline,
    ExternalWake,
    Unknown,
};

inline constexpr std::size_t kEeGraphicsServiceDeadlineKindCount = 4U;

[[nodiscard]] constexpr std::size_t ee_graphics_service_deadline_kind_index(
    const EeGraphicsServiceDeadlineKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kEeGraphicsServiceDeadlineKindCount
               ? index
               : static_cast<std::size_t>(EeGraphicsServiceDeadlineKind::Unknown);
}

struct EeGraphicsServiceDeadline {
    EeGraphicsServiceDeadlineKind kind{EeGraphicsServiceDeadlineKind::Unknown};
    // Meaningful for InternalDeadline. Immediate is represented by zero;
    // ExternalWake and Unknown do not manufacture a timed distance.
    std::uint64_t cycles_until_event{};

    [[nodiscard]] constexpr bool has_internal_deadline() const noexcept {
        return kind == EeGraphicsServiceDeadlineKind::InternalDeadline;
    }
};

class EeMemory {
  public:
    explicit EeMemory(std::size_t size = kEeMainRamSize);
    // Out of line: owns the opaque Stage 2 access-window tracker. The move
    // special members are kept (defaulted out of line) because callers move
    // EeMemory containers around; copying stays deleted via the unique_ptr.
    ~EeMemory();
    EeMemory(EeMemory&&) noexcept;
    EeMemory& operator=(EeMemory&&) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    // Persistent storage for native services whose lifetime spans retail RAM
    // overlays. This is not PS2 main RAM and is never exposed to DMA or AOT
    // main-RAM fast paths. Only the exact virtual range is mapped.
    static constexpr GuestAddress kNativeHeapBase = 0x06000000U;
    void ensure_native_heap(std::size_t size);
    [[nodiscard]] bool is_native_heap_range(GuestAddress address, std::size_t length) const noexcept;
    [[nodiscard]] bool contains(GuestAddress address, std::size_t length) const noexcept;

    // Native translations may only collapse many guest stores into one host
    // operation when the complete range resolves to ordinary EE RAM. These
    // helpers deliberately account for KSEG and the modeled default-TLB
    // aliases; `contains` alone also admits scheduler-visible MMIO and local
    // memories.
    [[nodiscard]] bool is_main_ram_range(GuestAddress address,
                                         std::size_t length) const noexcept;
    [[nodiscard]] bool main_ram_ranges_overlap(GuestAddress first_address,
                                                std::size_t first_length,
                                                GuestAddress second_address,
                                                std::size_t second_length) const noexcept;

    // Capture enrolls the normalized physical page in mutation tracking and
    // returns its current generation. Validation is an allocation-free exact
    // domain/page/generation check. Every later main-RAM write invalidates an
    // issued token even when the bytes do not change; a mutable bytes()/view()
    // escape permanently makes each covered page ineligible. These methods
    // are serialized with ordinary EE memory access by the caller.
    [[nodiscard]] EeMainRamCodePageCapture
    capture_main_ram_code_page(GuestAddress address) const noexcept;
    [[nodiscard]] bool validate_main_ram_code_page(
        const EeMainRamCodePageToken& token) const noexcept;

    // Capture and validate one exact physical main-RAM range. Capture is
    // side-effect free: it never enrolls or refreshes state. Every successful
    // tracked RAM write, including same-value writes and clear(), advances the
    // non-reusable global generation, so unrelated writes conservatively
    // invalidate outstanding ranges. Mutable views permanently retire every
    // covered page; const views do not affect eligibility or generation.
    [[nodiscard]] EeMainRamMutationCapture
    capture_main_ram_mutation(GuestAddress address, std::size_t length) const noexcept;
    [[nodiscard]] bool validate_main_ram_mutation(
        const EeMainRamMutationToken& token, GuestAddress address,
        std::size_t length) const noexcept;

    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::span<std::byte> view(GuestAddress address, std::size_t length);
    [[nodiscard]] std::span<const std::byte> view(GuestAddress address, std::size_t length) const;
    // Tracked bulk initialization/copy paths. The complete range is validated
    // before any byte changes, and one common writer record covers the whole
    // operation. These are the safe alternatives to an escaping mutable span
    // for ELF segment contents and zero-filled BSS tails.
    void write_main_ram_bytes(GuestAddress address, std::span<const std::byte> source);
    void fill_main_ram_bytes(GuestAddress address, std::size_t length, std::byte value);
    // Replaces one complete VU1 MicroMem image without exposing a mutable span
    // which could outlive cache invalidation. Local replay/seeding tools use
    // this path so later launches can retain the normal generation fast path.
    // The operation does not manufacture EE/VIF write activity.
    void replace_vu1_micro_memory(std::span<const std::byte> image);

    // The ordinary case (a direct or cached main-RAM address) is inline; every
    // other region, alias and fault is the out-of-line *_slow body.
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (is_direct_or_cached_alias(address)) [[likely]] {
            const auto offset = static_cast<std::size_t>(physical_address(address));
            if (offset < data_.size()) [[likely]] {
                return std::to_integer<std::uint8_t>(data_[offset]);
            }
        }
        return read_u8_slow(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(GuestAddress address) const;
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if (is_direct_or_cached_alias(address)) [[likely]] {
            const auto offset = static_cast<std::size_t>(physical_address(address));
            if (offset <= data_.size() && 4U <= data_.size() - offset) [[likely]] {
                std::uint32_t value;
                std::memcpy(&value, data_.data() + offset, sizeof(value));
                return value;
            }
        }
        return read_u32_le_slow(address);
    }
    [[nodiscard]] std::uint64_t read_u64_le(GuestAddress address) const;
    // The word read_u32_le() returns for a plain stored VIF1 register
    // (VIF1_TOP, VIF1_ITOP) without the general address decode: a VU1 launch
    // that executes XTOP/XITOP reads one per instruction. With a graphics
    // window open the general path is kept, as it observes the bus.
    [[nodiscard]] std::uint32_t read_stored_vif1_register(GuestAddress address) const;
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (ee_memory_write_observer_.pointer == nullptr ||
            scalar_write_transaction_depth_ != 0U) [[likely]] {
            if (is_direct_or_cached_alias(address)) [[likely]] {
                const auto offset = static_cast<std::size_t>(physical_address(address));
                if (offset < data_.size() && offset < kEeMainRamSize) [[likely]] {
                    data_[offset] = static_cast<std::byte>(value);
                    static_cast<void>(ee_aot_record_main_ram_store(offset, 1U));
                    return;
                }
            }
            write_u8_impl(address, value);
            return;
        }
        write_u8_observed(address, value);
    }
    void write_u16_le(GuestAddress address, std::uint16_t value);
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (ee_memory_write_observer_.pointer == nullptr ||
            scalar_write_transaction_depth_ != 0U) [[likely]] {
            // An aligned word of direct or cached main RAM: the store and
            // the bookkeeping record_main_ram_writer does, with its common
            // case (nothing watched) inline.
            if (is_direct_or_cached_alias(address) && (address & 3U) == 0U) [[likely]] {
                const auto offset = static_cast<std::size_t>(physical_address(address));
                if (offset <= data_.size() && 4U <= data_.size() - offset &&
                    offset <= kEeMainRamSize - 4U) [[likely]] {
                    std::memcpy(data_.data() + offset, &value, sizeof(value));
                    static_cast<void>(ee_aot_record_main_ram_store(offset, 4U));
                    return;
                }
            }
            write_u32_le_impl(address, value);
            return;
        }
        write_u32_le_observed(address, value);
    }
    void write_u64_le(GuestAddress address, std::uint64_t value);
    void write_u128_le(GuestAddress address, const std::array<std::uint32_t, 4>& words);

    // Opt-in observer for the five public scalar write entry points. The
    // pointer is non-owning and null by default; bulk setup/copy, DMA, and
    // internal device mutations never use this boundary.
    void set_ee_memory_write_observer(EeMemoryWriteObserver* observer) noexcept;
    [[nodiscard]] EeMemoryWriteObserver* ee_memory_write_observer() const noexcept {
        return ee_memory_write_observer_.pointer;
    }
    [[nodiscard]] std::uint32_t gs_interrupt_mask() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> take_completed_dmac_channel() noexcept;
    // Counts every queued completion's dispatch delay down by `polls`
    // scheduler polls, as that many take_completed_dmac_channel() calls would
    // (without taking any): for a native call that stood for that many
    // instructions without polling.
    void advance_dmac_completion_delays(std::uint64_t polls) noexcept;
    [[nodiscard]] const std::array<std::uint64_t, 10>& dmac_completion_counts() const noexcept;
    [[nodiscard]] std::size_t queued_dmac_completion_count() const noexcept;
    // Selects the opt-in internal GIF_STAT load shortcut. The canonical MMIO
    // bytes remain the sole status state and public guest reads are unchanged.
    void set_fast_gif_status_read_enabled(bool enabled) noexcept;
    void set_gif_blocked_arbiter_cache_enabled(bool enabled) noexcept;
    [[nodiscard]] std::uint64_t gif_blocked_arbiter_cache_skips() const noexcept {
        return gif_blocked_arbiter_cache_skips_;
    }
    // The full boot runner advances GIF DMA beside EE execution instead of
    // draining a snapshotted source chain inside the CHCR store. The PS2 GIF
    // bus runs at half the EE clock, so two supplied EE cycles make one PATH3
    // quadword eligible. Offline capture/replay keeps the original immediate
    // mode unless it explicitly opts in.
    void set_gif_dmac_timing_enabled(bool enabled);
    void advance_gif_dmac(std::uint64_t ee_cycles);
    [[nodiscard]] bool gif_dmac_in_flight() const noexcept;
    [[nodiscard]] bool gif_dmac_can_progress() const noexcept;
    // VIF1 source DMA is likewise incremental in the boot runner. Each source
    // tag becomes visible only at its scheduled event, and DIRECT/DIRECTHL
    // stops before its first data word until PATH2 can enter the GIF.
    void set_vif1_dmac_timing_enabled(bool enabled);
    void advance_vif1_dmac(std::uint64_t ee_cycles);
    // Advances the private VIF1/VU1/GIF clock until neither producer can make
    // progress without the EE, or maximum_cycles have elapsed on that clock.
    // Cycle-by-cycle VIF-then-GIF order is retained wherever the two interact;
    // only proven single-owner waits (VU1 completion, PATH1 readiness) are
    // crossed in one step. Returns the device cycles consumed.
    std::uint64_t run_graphics_ahead(std::uint64_t maximum_cycles);

    // ---- Asynchronous graphics window (runtime/graphics_async.hpp) ----
    //
    // With the mode On, run_graphics_ahead() hands the whole VIF1 -> UNPACK ->
    // VU1 -> XGKICK -> GIF -> GS-stream run to a worker thread and returns at
    // once, opening a "window". While it is open the emulation thread keeps
    // executing guest code but must not look at device state: every bus
    // access, accessor or scheduler query that could observe it either closes
    // the window (waits for the worker, then applies the EE cycles that passed
    // as the service calls the main loop would have made) or is answered from
    // state that cannot differ (see the gfx_* helpers). A closed window leaves
    // exactly the state the synchronous run-ahead would have produced when it
    // opened, so the GS stream and every guest-visible register are
    // identical; only the moment the DMA completion becomes visible to the
    // guest moves, to the first of "a fence" and "latency_cycles of EE time
    // after the window opened". Measure runs synchronously and only records
    // what an On run would have overlapped. An EeMemory with an open window
    // must not be moved.
    void configure_graphics_async(takedown::runtime::GraphicsAsyncMode mode,
                                  std::uint64_t latency_cycles = 1'500'000U,
                                  std::uint64_t minimum_quadwords = 64U);
    [[nodiscard]] takedown::runtime::GraphicsAsyncMode graphics_async_mode() const noexcept {
        return gfx_mode_;
    }
    [[nodiscard]] bool graphics_async_window_open() const noexcept { return gfx_window_; }
    // Closes an open window. Rethrows an exception the worker raised.
    void graphics_async_fence(takedown::runtime::GraphicsAsyncFence reason =
                                  takedown::runtime::GraphicsAsyncFence::Explicit);
    // Runs `action` after all graphics work already submitted: immediately
    // when no window is open, otherwise queued behind the window's job on the
    // worker so GS stream order is preserved without closing the window.
    void run_in_graphics_order(std::function<void()> action);
    // The same for a frame boundary (the VBlank stream marker), with
    // back-pressure: the emulation thread waits until fewer than
    // `frames_in_flight` (at least 1) earlier frame boundaries are queued on
    // the worker.
    // Without it a decoupled guest could run any number of frames ahead of
    // the device model.
    void run_frame_in_graphics_order(std::function<void()> action, std::uint32_t frames_in_flight);
    // Closes any window first.
    [[nodiscard]] takedown::runtime::GraphicsAsyncStatistics graphics_async_statistics();
    // Exact host shortcuts on the timed GIF path, both enabled by default.
    // The PATH3 batch lets run_graphics_ahead() move a streaming PATH3 source
    // through a span in which VIF1 provably has no event, instead of stepping
    // VIF1 and GIF one cycle at a time. The idle pump gate lets VU1 execution
    // stop polling an arbiter whose next pump would change nothing. Neither
    // changes the GS stream, register state or completion timing.
    void set_gif_path3_batch_enabled(bool enabled) noexcept {
        gif_path3_batch_enabled_ = enabled;
    }
    void set_gif_idle_pump_gate_enabled(bool enabled) noexcept {
        gif_idle_pump_gate_enabled_ = enabled;
    }
    [[nodiscard]] std::uint64_t gif_path3_batched_quadwords() const noexcept {
        return gif_path3_batched_quadwords_;
    }
    [[nodiscard]] std::uint64_t gif_path3_batched_spans() const noexcept {
        return gif_path3_batched_spans_;
    }
    [[nodiscard]] std::uint64_t gif_idle_pump_gate_hits() const noexcept {
        return gif_idle_pump_gate_hits_;
    }
    // True when a non-timing pump_gif_arbiter() call would leave every piece
    // of GIF, DMAC and capture state exactly as it is now.
    [[nodiscard]] bool gif_arbiter_pump_is_noop() const noexcept;
    // Read-only, allocation-free view of the next graphics boundary. The
    // first supported internal boundary is VU1 idle completion or an earlier
    // queued PATH1 publication. Direct/Flush/FlushAll waits and nontrivial
    // GIF activity remain Unknown until a later, narrower proof covers them.
    // Only an InternalDeadline result is retained. Keeping the epoch probe and
    // absolute-deadline materialization here lets the join-pump caller return
    // a valid hit without making an out-of-line query call; misses use the
    // out-of-line recompute helper below.
    [[nodiscard]] EeGraphicsServiceDeadline
    graphics_service_deadline() const noexcept {
        if (gfx_ee_view()) [[unlikely]] {
            return gfx_window_deadline();
        }
        if (graphics_service_deadline_epoch_ != 0U &&
            graphics_service_deadline_cache_valid_ &&
            graphics_service_deadline_cache_kind_ ==
                EeGraphicsServiceDeadlineKind::InternalDeadline &&
            graphics_service_deadline_cache_epoch_ == graphics_service_deadline_epoch_) {
            const auto entry_kind = graphics_service_deadline_cache_kind_;
            const bool expired =
                graphics_service_deadline_cache_absolute_deadline_ <= vif1_ee_cycle_;
            const auto materialized_kind = expired
                                               ? EeGraphicsServiceDeadlineKind::Immediate
                                               : EeGraphicsServiceDeadlineKind::InternalDeadline;
            if (graphics_service_deadline_cache_counters_enabled_) {
                ++graphics_service_deadline_cache_hits_;
                ++graphics_service_deadline_cache_entry_hits_by_kind_[
                    ee_graphics_service_deadline_kind_index(entry_kind)];
                ++graphics_service_deadline_cache_hits_by_kind_[
                    ee_graphics_service_deadline_kind_index(materialized_kind)];
            }
            return EeGraphicsServiceDeadline{
                .kind = materialized_kind,
                .cycles_until_event =
                    expired
                        ? 0U
                        : graphics_service_deadline_cache_absolute_deadline_ - vif1_ee_cycle_,
            };
        }
        return recompute_graphics_service_deadline_and_publish();
    }
    // Admission-only fast path for the default-off join-pump pilot. Producer
    // transitions publish one absolute, owner-thread deadline; this query
    // only materializes its relative distance. It intentionally returns only
    // a future InternalDeadline which can admit a span (more than one cycle);
    // every other classifier result, including an expired or one-cycle
    // boundary, is a fail-fast miss. Keep the typed graphics_service_deadline()
    // query above as the authoritative public classifier for diagnostics and
    // callers which need its result kinds.
    [[gnu::always_inline]] [[nodiscard]] std::optional<std::uint64_t>
    next_internal_graphics_deadline_for_join_pump() const noexcept {
        if (gfx_ee_view()) [[unlikely]] {
            const auto deadline = gfx_window_deadline();
            return deadline.cycles_until_event > 1U
                       ? std::optional<std::uint64_t>{deadline.cycles_until_event}
                       : std::nullopt;
        }
        if (!join_pump_owned_deadline_valid_ || !vif1_dmac_timing_enabled_ ||
            timed_vu1_execution_active_ ||
            join_pump_owned_deadline_ <= vif1_ee_cycle_) {
            return std::nullopt;
        }
        const auto cycles_until_event = join_pump_owned_deadline_ - vif1_ee_cycle_;
        return cycles_until_event > 1U
                   ? std::optional<std::uint64_t>{cycles_until_event}
                   : std::nullopt;
    }
    // Test-only escape hatch for comparing the cached query with an explicit
    // recomputation. An InternalDeadline recomputation is published into the
    // same cache; other typed results remain uncached by policy.
    [[nodiscard]] EeGraphicsServiceDeadline
    graphics_service_deadline_forced_recompute_for_testing() const noexcept;
    [[nodiscard]] std::uint32_t graphics_idle_diagnostic_state() const noexcept;
    void enable_graphics_service_deadline_cache_counters_for_testing(
        bool enabled) noexcept;
    [[nodiscard]] std::uint64_t graphics_service_deadline_cache_hits() const noexcept {
        return graphics_service_deadline_cache_hits_;
    }
    [[nodiscard]] std::uint64_t graphics_service_deadline_cache_recomputes() const noexcept {
        return graphics_service_deadline_cache_recomputes_;
    }
    // Diagnostic-only breakdowns use the stable enum order above. Return-kind
    // hits are classified by the materialized result, so an expired internal
    // entry is reported as Immediate while entry hits remain InternalDeadline.
    [[nodiscard]] std::uint64_t graphics_service_deadline_cache_hits(
        EeGraphicsServiceDeadlineKind kind) const noexcept {
        return graphics_service_deadline_cache_hits_by_kind_[
            ee_graphics_service_deadline_kind_index(kind)];
    }
    [[nodiscard]] std::uint64_t graphics_service_deadline_cache_entry_hits(
        EeGraphicsServiceDeadlineKind kind) const noexcept {
        return graphics_service_deadline_cache_entry_hits_by_kind_[
            ee_graphics_service_deadline_kind_index(kind)];
    }
    [[nodiscard]] std::uint64_t graphics_service_deadline_cache_recomputes(
        EeGraphicsServiceDeadlineKind kind) const noexcept {
        return graphics_service_deadline_cache_recomputes_by_kind_[
            ee_graphics_service_deadline_kind_index(kind)];
    }
    [[nodiscard]] const std::array<std::uint64_t,
                                   kEeGraphicsServiceDeadlineKindCount>&
    graphics_service_deadline_cache_hits_by_kind() const noexcept {
        return graphics_service_deadline_cache_hits_by_kind_;
    }
    [[nodiscard]] const std::array<std::uint64_t,
                                   kEeGraphicsServiceDeadlineKindCount>&
    graphics_service_deadline_cache_entry_hits_by_kind() const noexcept {
        return graphics_service_deadline_cache_entry_hits_by_kind_;
    }
    [[nodiscard]] const std::array<std::uint64_t,
                                   kEeGraphicsServiceDeadlineKindCount>&
    graphics_service_deadline_cache_recomputes_by_kind() const noexcept {
        return graphics_service_deadline_cache_recomputes_by_kind_;
    }
    [[nodiscard]] bool vif1_dmac_in_flight() const noexcept;
    [[nodiscard]] bool vif1_dmac_can_progress() const noexcept;

    // ---- Scalar burst support (the boot runner's inner interpreter loop) ----
    //
    // Monotonic count of EE bus accesses that can change device state. It moves
    // on every write of MMIO, GS privileged registers, VU0/VU1 micro and data
    // memory (the complete non-RAM half of the write_u8/write_u32_le/
    // write_u128_le implementations, including a faulting access), on every
    // read of VU0/VU1 memory or a faulting read, and when an ordinary RAM or
    // scratchpad store cancels a VIF1 PATH2 source marker. Reads of MMIO and
    // GS privileged registers do not move it: they return stored bytes (or
    // the DMAC/INTC status words) and change nothing, so a burst that polls a
    // status register can keep going. Plain main-RAM and scratchpad accesses
    // never touch it, so their fast paths carry no extra work. An unchanged
    // value across one interpreted instruction therefore proves that the
    // instruction started no transfer, raised or acknowledged no interrupt,
    // changed no interrupt mask and ran no device or observer callback.
    [[nodiscard]] std::uint64_t hardware_activity_epoch() const noexcept {
        return hardware_activity_epoch_;
    }
    // Accesses by the VU0 micro interpreter to VU0's own MicroMem and data
    // memory (their first 4 KiB mapping). A microprogram runs synchronously
    // inside VCALLMS, and VU0 memory changes otherwise only through device
    // writes and DMA kicks that are themselves device activity, so these are
    // not counted as device activity: a VCALLMS leaves the scheduler state
    // the scalar burst depends on untouched. The bytes are those
    // read_u32_le/write_u32_le would access; a store with a write observer
    // installed, and every other address, take the ordinary path.
    [[nodiscard]] std::uint32_t read_vu0_local_u32(const GuestAddress address) const {
        if (const auto* local = vu0_local_bytes(address, 4U); local != nullptr) [[likely]] {
            std::uint32_t value;
            std::memcpy(&value, local, sizeof(value));
            return value;
        }
        return read_u32_le(address);
    }
    [[nodiscard]] std::uint16_t read_vu0_local_u16(const GuestAddress address) const {
        if (const auto* local = vu0_local_bytes(address, 2U); local != nullptr) [[likely]] {
            std::uint16_t value;
            std::memcpy(&value, local, sizeof(value));
            return value;
        }
        return read_u16_le(address);
    }
    // VU0 MicroMem as it stands, and a counter that advances whenever any
    // path may have changed it (EE or DMA writes, a mutable view, reset).
    [[nodiscard]] std::span<const std::byte, kVu0MemorySize> vu0_micro_image() const noexcept {
        return std::span<const std::byte, kVu0MemorySize>{vu0_micro_};
    }
    [[nodiscard]] std::uint64_t vu0_micro_generation() const noexcept { return vu0_micro_generation_; }
    // VU0 data memory for compiled microprograms (TAKEDOWN_LOCAL_VU0_AOT_MODULE),
    // which read and write it directly while no write observer is installed.
    [[nodiscard]] std::byte* vu0_data_local_bytes() noexcept { return vu0_data_.data(); }
    void write_vu0_local_u32(const GuestAddress address, const std::uint32_t value) {
        if (ee_memory_write_observer_.pointer == nullptr) [[likely]] {
            if (const auto* local = vu0_local_bytes(address, 4U); local != nullptr) [[likely]] {
                std::memcpy(const_cast<std::byte*>(local), &value, sizeof(value));
                return;
            }
        }
        write_u32_le(address, value);
    }
    void write_vu0_local_u16(const GuestAddress address, const std::uint16_t value) {
        if (ee_memory_write_observer_.pointer == nullptr) [[likely]] {
            if (const auto* local = vu0_local_bytes(address, 2U); local != nullptr) [[likely]] {
                std::memcpy(const_cast<std::byte*>(local), &value, sizeof(value));
                return;
            }
        }
        write_u16_le(address, value);
    }
    // True when advance_vif1_dmac() and advance_gif_dmac() can do nothing but
    // move the device clock: no VIF1 source DMA, no VU1 PATH1 packet waiting
    // for its release cycle, no PATH3 source DMA, and no diagnostics recorder
    // counting the calls. Both *_can_progress() queries are false in this
    // state. It can only end through one of the accesses counted above (or
    // through device/kernel work the outer runner performs itself).
    [[nodiscard]] bool graphics_dmacs_idle() const noexcept {
        // An open asynchronous window services the device on its worker; for
        // the emulation thread every service call is a deferred clock tick.
        if (gfx_ee_view()) [[unlikely]] {
            return true;
        }
        return graphics_service_diagnostics_ == nullptr && !pending_vif1_dmac_.has_value() &&
               pending_vu1_path1_packets_.empty() && !pending_gif_dmac_.has_value();
    }
    // Exact equivalents of advance_vif1_dmac() / advance_gif_dmac() while
    // graphics_dmacs_idle() holds; calling them in any other state is a logic
    // error. With no pending VIF1 source the full routine only saturating-adds
    // to the device clock and scans the (empty) PATH1 release queue; with no
    // pending PATH3 source it only clears the banked bus credit.
    void advance_idle_vif1_dmac(const std::uint64_t ee_cycles) noexcept {
        if (!vif1_dmac_timing_enabled_ || ee_cycles == 0U) {
            return;
        }
        if (gfx_window_) [[unlikely]] {
            if (gfx_note_service(ee_cycles, true)) {
                return;
            }
        }
        const auto target = vif1_ee_cycle_ + ee_cycles;
        vif1_ee_cycle_ = target < vif1_ee_cycle_ ? ~std::uint64_t{0U} : target;
    }
    void advance_idle_gif_dmac(const std::uint64_t ee_cycles) noexcept {
        if (!gif_dmac_timing_enabled_ || ee_cycles == 0U) {
            return;
        }
        if (gfx_window_) [[unlikely]] {
            if (gfx_note_service(ee_cycles, false)) {
                return;
            }
        }
        gif_dmac_cycle_credit_ = 0U;
    }
    // The private VIF1/VU1/GIF device clock and banked PATH3 credit. Exposed
    // read-only so tests can pin the idle equivalents above to the full service
    // routines.
    [[nodiscard]] std::uint64_t graphics_device_cycle() const noexcept {
        gfx_sync_accessor(0xacce5507U);
        return vif1_ee_cycle_;
    }
    [[nodiscard]] std::uint64_t gif_dmac_cycle_credit() const noexcept {
        gfx_sync_accessor(0xacce5508U);
        return gif_dmac_cycle_credit_;
    }

    // Opt-in exact-window diagnostics. The pointer is non-owning and remains
    // null for every ordinary run; it records no guest-visible state.
    void set_graphics_service_diagnostics(
        takedown::runtime::EeGraphicsServiceDiagnostics* diagnostics) noexcept;
    [[nodiscard]] const EeIpuActivity& ipu_activity() const noexcept;
    [[nodiscard]] const EeVu1Activity& vu1_activity() const noexcept;
#if defined(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS) && TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS
    // Diagnostic-only named A/C observations; no guest state is changed.
    void observe_vif1_pure_unpack_boundary(bool start, std::uint64_t steps,
                                           std::uint64_t cycles) noexcept;
#endif
    [[nodiscard]] Vu1ProgramCache& vu1_program_cache() noexcept;
    [[nodiscard]] const Vu1ProgramCache& vu1_program_cache() const noexcept;
    // VIF1 UNPACK (0x60-0x7F) expansion. `command_byte` is the command's low
    // byte (0x60..0x7F); its 0x0f bits select the S/V2/V3/V4 + bit width. The
    // `unpack_count` input vector sources are packed sequentially in
    // `packed_bytes` (little-endian); the returned lane words hold each output
    // 128-bit vector as four consecutive 32-bit lanes in X,Y,Z,W byte order.
    // `zero_extension` selects zero- (true) vs sign-extension (false) for the
    // 16/8-bit formats. Returns 4*`unpack_count` lane words, or throws
    // std::invalid_argument for an undefined UNPACK type or a short source.
    [[nodiscard]] static std::vector<std::uint32_t>
    expand_vif1_unpack(std::uint32_t command_byte, std::uint32_t unpack_count,
                       bool zero_extension, std::span<const std::uint8_t> packed_bytes);
    // expand_vif1_unpack() into a caller-owned buffer (cleared first), so the
    // UNPACK hot path reuses one allocation.
    static void expand_vif1_unpack_into(std::vector<std::uint32_t>& lanes,
                                        std::uint32_t command_byte, std::uint32_t unpack_count,
                                        bool zero_extension,
                                        std::span<const std::uint8_t> packed_bytes);
    // Optional host-side MPEG-2 decoder behind the clean IpuMpeg2Decoder
    // interface. The IPU feeds it the elementary-stream bytes it pulls from
    // guest RAM through the to-IPU DMA paths; nothing in the guest-visible
    // IPU model depends on a decoder being installed.
    void set_ipu_mpeg2_decoder(std::unique_ptr<IpuMpeg2Decoder> decoder) noexcept;
    [[nodiscard]] IpuMpeg2Decoder* ipu_mpeg2_decoder() noexcept;
    [[nodiscard]] const IpuMpeg2Decoder* ipu_mpeg2_decoder() const noexcept;
    void set_gs_capture_enabled(bool enabled) noexcept;
    [[nodiscard]] bool gs_capture_enabled() const noexcept;
    [[nodiscard]] GsCapture& gs_capture() noexcept;
    [[nodiscard]] const GsCapture& gs_capture() const noexcept;
    // Latches one VU1 XGKICK address (a VUMem vector index). In the default
    // DelayedSnapshot mode the complete packet is copied after the following
    // issued microinstruction. TimedLive retains PCSX2's opt-in XGKickHack
    // behavior for diagnostics and games which explicitly require it.
    [[nodiscard]] bool submit_vu1_xgkick(std::uint16_t vector_address);
    // Retires the instruction/cycles following XGKICK. DelayedSnapshot copies
    // the complete packet on the first nonzero call; TimedLive adds VU cycles
    // to its one-quadword-per-two-cycles producer clock. VU dependency stalls
    // call this only in TimedLive mode because PCSX2 performs its default
    // delayed copy after the following instruction has executed.
    // A testable, inline service gate shared by the interpreter today and
    // future compiled VU executors. False means a nonzero VU retirement has
    // no XGKICK or resident GIF work to advance.
    [[nodiscard]] bool vu1_xgkick_nonzero_advance_needed() const noexcept {
        return vu1_xgkick_.active || gif_active_path_.has_value() ||
               !vu1_xgkick_.queued_quadwords.empty() ||
               !queued_gif_path2_.empty() || pending_gif_dmac_.has_value();
    }
    // Synchronous execution can stop polling a proven external wait. Keep
    // the conservative resident-work gate above for worker admission: paused
    // data is still shared work even though another VU instruction cannot
    // unblock it. Producer publication and GIF control writes invalidate the
    // wait through the ordinary arbiter entry. An active XGKICK must still
    // snapshot/produce its bytes while the GIF consumer is paused.
    [[nodiscard]] bool vu1_xgkick_execution_advance_needed() const noexcept {
        if (vu1_xgkick_.active) return true;
        if (gif_blocked_arbiter_cache_enabled_ && gif_arbiter_waits_for_external_wake_)
            return false;
        if (!vu1_xgkick_nonzero_advance_needed()) return false;
        // A VU launch runs synchronously: only its own XGKICK can change GIF
        // state, and that re-arms the caller's gate. While no kick is active
        // every further retirement is a plain arbiter pump, so once that pump
        // is a proven no-op (for example PATH3 short of cycle credit, or PATH1
        // queued behind an open PATH3 packet) the rest of the launch is too.
        if (gif_idle_pump_gate_enabled_ && gif_arbiter_pump_is_noop()) {
            ++gif_idle_pump_gate_hits_;
            return false;
        }
        return true;
    }
    void advance_vu1_xgkick(std::uint32_t cycles = 1U) {
        // Resident work without a proven wait still gets its scheduling turn,
        // including PATH3's deliberate yield at a packet boundary. Zero-cycle
        // calls remain explicit arbiter flushes regardless of the cached wait.
        if (cycles != 0U && !vu1_xgkick_execution_advance_needed()) {
            return;
        }
        advance_vu1_xgkick_slow(cycles);
    }
    // VU program termination and FLUSHA copy the remaining packet to PATH1's
    // producer FIFO regardless of cycle credit. False denotes the bounded
    // malformed-stream safeguard.
    [[nodiscard]] bool flush_vu1_xgkick();
    [[nodiscard]] bool vu1_xgkick_active() const noexcept;
    void set_vu1_xgkick_mode(Vu1XgkickMode mode);
    [[nodiscard]] Vu1XgkickMode vu1_xgkick_mode() const noexcept;
    void set_vu1_xgkick_observer(Vu1XgkickObserver* observer) noexcept;
    void set_vu0_micro_call_observer(Vu0MicroCallObserver* observer) noexcept {
        vu0_micro_call_observer_ = observer;
    }
    [[nodiscard]] Vu0MicroCallObserver* vu0_micro_call_observer() const noexcept {
        return vu0_micro_call_observer_;
    }
    // Attaching this diagnostic allocates last-writer provenance tables and
    // enables the corresponding write tracking.  The ordinary runtime keeps
    // both the storage and write-side work disabled.
    void set_vif1_dmac_chain_observer(Vif1DmacChainObserver* observer);
    // Enables verified native handoffs at a post-arbitration PATH2 boundary.
    // Detaching an observer synchronously cancels every armed or queued
    // marker through the old observer, so a transaction registry cannot keep
    // a stale native request alive.
    void set_vif1_path2_source_marker_observer(
        Vif1Path2SourceMarkerObserver* observer) noexcept;
    // Registers the VU1 launch observer and the DMA source range it watches
    // ([source_begin, source_end)); nullptr detaches it.
    void set_vu1_launch_observer(Vu1LaunchObserver* observer, std::uint32_t source_begin,
                                 std::uint32_t source_end) noexcept {
        vu1_launch_observer_ = observer;
        vu1_launch_source_begin_ = source_begin;
        vu1_launch_source_end_ = source_end;
        vu1_launch_source_tag_ = 0U;
    }
    // Detaches only when `expected` still owns the registration. This lets a
    // scoped native registry clean up without disconnecting a replacement
    // observer installed later in the same EeMemory lifetime.
    [[nodiscard]] bool detach_vif1_path2_source_marker_observer(
        Vif1Path2SourceMarkerObserver* expected) noexcept;
    // Replacing the singleton observer is lossless only after every marker
    // owned by the current observer has retired and no pre-snapshot callback
    // is in flight.
    [[nodiscard]] bool vif1_path2_source_markers_quiescent() const noexcept {
        return vif1_path2_source_marker_count_ == 0U &&
               !vif1_timed_snapshot_capture_active_;
    }
    [[nodiscard]] Vif1Path2SourceMarkerArmResult
    arm_vif1_path2_source_marker(const Vif1Path2SourceMarkerRequest& request);
    // Lets a caller roll back a prepared native transaction before its marked
    // QW reaches the selected PATH2 position. It never removes a marker
    // already delivered at PATH2.
    [[nodiscard]] bool cancel_vif1_path2_source_marker(std::uint64_t generation) noexcept;
    [[nodiscard]] const GsPrivilegedDisplayObserver& gs_privileged_display() const noexcept;
    // Kernel HLE calls need to expose their logical post-call mask state
    // without applying D_STAT's guest-facing clear/toggle write semantics.
    void set_dmac_status(std::uint32_t value) noexcept;
    void set_intc_status(std::uint32_t value) noexcept;
    void set_intc_mask(std::uint32_t value) noexcept;
    void set_current_ee_pc(GuestAddress pc) noexcept { current_ee_pc_ = pc; }
    [[nodiscard]] GuestAddress current_ee_pc() const noexcept { return current_ee_pc_; }
    void set_current_guest_step(std::uint64_t step) noexcept {
        current_guest_step_ = step;
    }
    [[nodiscard]] std::uint64_t current_guest_step() const noexcept {
        return current_guest_step_;
    }

    // Generated guest code (src/guest/ee_aot.cpp) reads and writes main RAM
    // and the scratchpad directly. A direct store must be followed by the
    // same record call, with the same offset and length, that the ordinary
    // write path makes for it; while ee_aot_store_needs_slow_path() holds a
    // store may notify an observer and must go through the ordinary path.
    [[nodiscard]] std::byte* ee_aot_main_ram() noexcept { return data_.data(); }
    [[nodiscard]] std::size_t ee_aot_main_ram_size() const noexcept { return data_.size(); }
    [[nodiscard]] std::byte* ee_aot_scratchpad() noexcept { return scratchpad_.data(); }
    [[nodiscard]] bool ee_aot_store_needs_slow_path() const noexcept {
        return vif1_path2_source_marker_count_ != 0U ||
               ee_memory_write_observer_.pointer != nullptr;
    }
    // `length` is 1 or 4 and the store does not cross a word. When all the
    // ordinary path would do is advance the mutation generation (no watched
    // code page, compiled word, polled range, PATH2 marker, write watch or
    // VIF1 writer log is involved), that is done inline. Returns whether the
    // ordinary bookkeeping ran (only it can advance an epoch).
    bool ee_aot_record_main_ram_store(const std::size_t offset, const std::size_t length) noexcept {
        const auto generation = *ee_aot_view_.mutation_generation;
        const auto word = (offset - ee_aot_view_.code_begin) / 4U;
        if (generation - 1U < ~std::uint64_t{0U} - 1U && !*ee_aot_mutation_generation_exhausted_ &&
            ee_aot_view_.page_states[offset / kEeMainRamCodePageSize] != kEeAotWatchedPageState &&
            (offset < ee_aot_view_.code_begin || offset >= ee_aot_view_.code_end ||
             ((ee_aot_code_words_[word / 64U] >> (word % 64U)) & 1U) == 0U) &&
            !(offset < ee_aot_view_.watch[0].second && offset + length > ee_aot_view_.watch[0].first) &&
            !(offset < ee_aot_view_.watch[1].second && offset + length > ee_aot_view_.watch[1].first) &&
            vif1_path2_source_marker_count_ == 0U && !ram_write_watch_active_ &&
            (vif1_dmac_chain_observer_ == nullptr || vif1_main_ram_last_writer_pc_.empty())) [[likely]] {
            *ee_aot_view_.mutation_generation = generation + 1U;
            return false;
        }
        record_main_ram_writer(offset, length);
        return true;
    }
    // The conditions of the main-RAM fast path that do not depend on the
    // address: no write observer or PATH2 marker, no write watch or VIF1 writer
    // log in use, and the mutation generation not exhausted. Only a store that
    // takes the ordinary route, an interpreted instruction or a device access
    // can change them, so compiled code asks once per run and again after
    // anything of that kind (see ee_aot_plain_main_ram_store).
    [[nodiscard]] bool ee_aot_stores_plain() const noexcept {
        return vif1_path2_source_marker_count_ == 0U && ee_memory_write_observer_.pointer == nullptr &&
               !ram_write_watch_active_ &&
               (vif1_dmac_chain_observer_ == nullptr || vif1_main_ram_last_writer_pc_.empty()) &&
               !*ee_aot_mutation_generation_exhausted_;
    }
    // The fast path of a direct main-RAM store of `length` bytes (1, 2, 4, 8 or
    // 16, aligned to its size) at `offset`, given ee_aot_stores_plain(): every
    // record call the ordinary path makes for it (one per byte below a word,
    // else one per word) would only advance the mutation generation, which is
    // then advanced by that many. True means exactly that was done and the
    // caller writes the bytes; false leaves everything untouched and the
    // caller takes the ordinary route (ee_aot_store_needs_slow_path, then one
    // ee_aot_record_main_ram_store per record).
    [[gnu::always_inline]] bool ee_aot_plain_main_ram_store(const std::size_t offset,
                                                            const std::size_t length) noexcept {
        return takedown::guest::ee_aot_plain_main_ram_store(ee_aot_view_, offset, length);
    }
    // The view generated code reads on its store fast path. The pointers to
    // live state are set here, not once, because the memory can be moved.
    [[nodiscard]] const EeAotMemoryView& ee_aot_memory_view() noexcept {
        ee_aot_view_.vif1_marker_count = &vif1_path2_source_marker_count_;
        ee_aot_view_.write_observer_slot = &ee_memory_write_observer_.pointer;
        ee_aot_view_.chain_observer_slot = &vif1_dmac_chain_observer_;
        return ee_aot_view_;
    }
    // Whether a scratchpad store needs anything beyond the bytes: provenance
    // for a VIF1 chain observer (a live PATH2 marker already forces the
    // ordinary path, see ee_aot_store_needs_slow_path).
    [[nodiscard]] bool ee_aot_scratchpad_store_records() const noexcept {
        return vif1_dmac_chain_observer_ != nullptr || vif1_path2_source_marker_count_ != 0U;
    }
    // Whether a load of `length` bytes at `address` reads only EE MMIO or GS
    // privileged register storage. Such a read changes no state (see
    // hardware_activity_epoch), so compiled code may perform it with the
    // ordinary read_u8/read_u16_le/read_u32_le and keep running.
    [[nodiscard]] bool ee_aot_register_read_is_pure(const GuestAddress address,
                                                    const std::size_t length) const noexcept {
        return is_mmio_range(address, length);
    }
    // A word store to one of the SPR DMA channels' MADR, QWC or SADR (both
    // channels), which only latch the masked value: done exactly as
    // write_u32_le does it, but not counted as device activity because it
    // starts nothing. False, with nothing written, for any other address.
    // The caller has excluded a write observer (ee_aot_store_needs_slow_path).
    [[nodiscard]] bool ee_aot_store_plain_register(GuestAddress address, std::uint32_t value) noexcept;
    void ee_aot_record_scratchpad_store(const std::size_t offset,
                                        const std::size_t length) noexcept {
        record_scratchpad_writer(offset, length);
    }
    // Main-RAM words holding compiled guest code: bit i of `words` stands
    // for the word at physical `begin` + 4i. Every store that overlaps one
    // advances ee_aot_code_write_epoch(); data between compiled functions
    // (the executable is one writable segment) does not.
    void set_ee_aot_code_words(const std::size_t begin, std::vector<std::uint64_t> words) noexcept {
        ee_aot_view_.code_begin = begin;
        ee_aot_view_.code_end = begin + words.size() * 64U * 4U;
        ee_aot_code_words_ = std::move(words);
        ee_aot_view_.code_words = ee_aot_code_words_.data();
    }
    [[nodiscard]] std::uint64_t ee_aot_code_write_epoch() const noexcept {
        return ee_aot_code_write_epoch_;
    }
    // Physical main-RAM ranges the caller polls between instructions; every
    // store that overlaps one advances ee_aot_watch_epoch().
    void set_ee_aot_watch_range(const std::size_t index, const std::size_t begin,
                                const std::size_t end) noexcept {
        if (index < kEeAotWatchRanges) {
            ee_aot_view_.watch[index] = {begin, end};
        }
    }
    [[nodiscard]] std::uint64_t ee_aot_watch_epoch() const noexcept { return ee_aot_watch_epoch_; }

    // TAKEDOWN_RAM_WRITE_WATCH (or legacy RAM_WRITE_WATCH)=
    // <hex-start>-<hex-end>[@<decimal-step>]: names the guest code writing one
    // main-RAM range after an optional guest-step boundary (bounded stderr
    // log; diagnostic only).
    void configure_ram_write_watch() noexcept {
        const char* v = std::getenv("TAKEDOWN_RAM_WRITE_WATCH");
        if (v == nullptr) {
            v = std::getenv("RAM_WRITE_WATCH");
        }
        if (v != nullptr) {
            const auto dash = std::strchr(v, '-');
            if (dash != nullptr) {
                char* start_end{};
                char* range_end{};
                const auto start = std::strtoull(v, &start_end, 16);
                const auto end = std::strtoull(dash + 1, &range_end, 16);
                if (start_end == dash && range_end != dash + 1 &&
                    (*range_end == '\0' || *range_end == '@') && end > start) {
                    ram_write_watch_start_ = static_cast<std::size_t>(start);
                    ram_write_watch_end_ = static_cast<std::size_t>(end);
                    if (*range_end == '@') {
                        char* step_end{};
                        const auto step = std::strtoull(range_end + 1, &step_end, 10);
                        if (step_end == range_end + 1 || *step_end != '\0') {
                            return;
                        }
                        ram_write_watch_after_step_ = step;
                    }
                    ram_write_watch_logged_ = 96U;
                    ram_write_watch_active_ = true;
                }
            }
        }
    }
    // True once configure_ram_write_watch() armed the diagnostic store log.
    [[nodiscard]] bool ram_write_watch_active() const noexcept { return ram_write_watch_active_; }
    [[nodiscard]] std::uint32_t dmac_status() const noexcept;
    [[nodiscard]] std::uint32_t intc_status() const noexcept;
    [[nodiscard]] std::uint32_t intc_mask() const noexcept;
    [[nodiscard]] std::uint32_t pending_intc_causes() const noexcept;
    void raise_intc_cause(std::uint32_t cause) noexcept;

    void clear() noexcept;

    // Stage 2 VU1 access-window tracker (docs/STATIC_RECOMPILATION.md
    // "VU1 worker Stage 2, milestone 2"). When recording is enabled, every
    // guest CPU bus read/write resolving into VU1 MicroMem or VUMem appends
    // one half-open window over [0x11008000, 0x11010000); adjacent accesses
    // merge. take_vu1_observed_windows() drains the accumulation at each VU1
    // launch so the deferral predicate observes exactly one launch interval.
    // VIF1 UNPACK/MPG traffic and the micro interpreter touch the VU1 buffers
    // directly and are deliberately not recorded — they are the launch
    // mechanism, not an observation of it.
    void set_vu1_observed_window_recording(bool enabled) noexcept;
    [[nodiscard]] std::vector<takedown::runtime::Vu1WorkerAccessWindow>
    take_vu1_observed_windows();

    // Stage 2 milestone 3 consume fences (docs/STATIC_RECOMPILATION.md
    // "VU1 worker Stage 2, milestone 3"). vu1_worker_consume_fence() joins a
    // pending worker launch and applies its journal to live state (VUMem word
    // deltas, byte-exact PATH1 packets, pending-producer restore, run-result
    // bookkeeping); it is inert unless a consume-mode controller has a launch
    // in flight. The observation variant exists because the guest bus read
    // paths are const — a pending consume can only exist on an EeMemory that
    // already took a non-const launch path, so the cast below never mutates a
    // genuinely const object.
    void vu1_worker_consume_fence(takedown::runtime::Vu1WorkerConsumeReason reason);
    // The pending-journal half, out of line: its journal needs a stack frame
    // of tens of kilobytes, which no bus access that merely checks may pay.
    [[gnu::noinline]] void vu1_worker_consume_fence_pending(
        takedown::runtime::Vu1WorkerConsumeReason reason);
    void vu1_worker_consume_fence_at_observation() const;

    // The latched-but-unsnapshotted DelayedSnapshot producer (empty when no
    // kick is open). The VU1 worker execution service captures this after a
    // detached run so consume can restore the equivalent live producer.
    struct Vu1PendingXgkick {
        bool active{};
        std::uint16_t cursor{};
        GsQuadword submitted_first_tag{};
    };
    [[nodiscard]] Vu1PendingXgkick vu1_pending_xgkick() const noexcept;

  private:
    enum class Vif1StreamPhase : std::uint8_t {
        Command,
        MpgPayload,
        DirectPayload,
        RegisterPayload,
        UnpackPayload,
    };

    enum class Vif1RegisterPayload : std::uint8_t {
        None,
        Mask,
        Row,
        Column,
    };

    struct Vif1StreamState {
        Vif1StreamPhase phase{Vif1StreamPhase::Command};
        std::uint32_t remaining_words{};
        std::uint32_t micro_word{};
        GsQuadword direct_quadword{};
        std::array<std::uint32_t, 4> direct_quadword_sources{};
        std::size_t direct_lane{};
        bool direct_quadword_sources_known{};
        std::uint32_t direct_chain_tadr{};
        std::uint32_t direct_chain_root{};
        // A source address can recur later in the same chain. Retain the
        // DMAtag location as part of a native handoff identity, and refuse a
        // marker when one GIF QW crosses a source-tag boundary.
        std::uint32_t direct_source_tag_location{};
        std::uint32_t direct_source_tag_ordinal{};
        std::uint32_t direct_source_payload_word_ordinal{};
        // DIRECT may interrupt an intermittent PATH3 IMAGE slice; DIRECTHL
        // deliberately may not. Keep the command type attached to every raw
        // PATH2 quadword until the shared GIF arbiter selects it.
        bool direct_can_preempt_path3{};
        // CanDoPath2 is sampled once per DMAtag payload. A DIRECT which spans
        // source tags must re-enter VGW at the next tag boundary.
        bool direct_transfer_admitted{};
        std::uint32_t unpack_pending_bytes{};
        std::uint32_t unpack_destination{};
        std::uint32_t unpack_vector_count{};
        std::uint32_t unpack_source_vector_count{};
        std::uint8_t unpack_type{};
        // V3's otherwise-indeterminate W lane depends on where its VIFcode
        // began inside the incoming quadword. Values are one through four,
        // matching PCSX2's start_aligned convention.
        std::uint8_t unpack_start_alignment{1U};
        // Every accepted word advances this lane within the current external
        // VIF transfer.  The decoder phase persists across transfers, but the
        // source-QW alignment does not: PCSX2 seeds start_aligned from each
        // VIF1transfer() span independently (TTE and the following DMA payload
        // are separate spans).
        std::uint8_t word_lane{};
        bool unpack_zero_extension{};
        bool unpack_masked{};
        std::vector<std::uint8_t> unpack_rx{};
        Vif1RegisterPayload register_payload{Vif1RegisterPayload::None};
        std::uint32_t register_payload_index{};
        // Persistent VIF1 register state. UNPACK consumes CYCLE, MODE, MASK,
        // ROW/COL and TOPS; microprogram launches advance the TOP/TOPS and
        // ITOP/ITOPS double-buffer state.
        std::uint32_t cycle{};
        std::uint32_t offset{};
        std::uint32_t base{};
        std::uint32_t top{};
        std::uint32_t tops{};
        std::uint32_t itop{};
        std::uint32_t itops{};
        std::uint32_t mode{};
        std::uint32_t mask{};
        std::array<std::uint32_t, 4> row{};
        std::array<std::uint32_t, 4> column{};
        bool double_buffer{};
    };

    struct PendingDmacCompletion {
        std::uint32_t channel{};
        std::uint32_t dispatch_delay{};
    };

    struct PendingGifDmacTransfer {
        std::uint32_t chcr{};
        bool normal_mode{};
        std::vector<GsQuadword> payload{};
        std::vector<std::size_t> tag_payload_ends{};
        std::size_t next_quadword{};
        std::size_t next_tag_boundary{};
        // Decoupled graphics: the emulation thread already published this
        // transfer's final DMAC registers, D_STAT bit and completion.
        bool ee_published{};
    };

    struct PendingVif1DmacTag {
        DmacSourceTag tag{};
        // This tag's payload snapshot: a run of the owning transfer's
        // payload_pool (see PendingVif1DmacTransfer::payload_of).
        std::uint32_t payload_begin{};
        std::uint32_t payload_count{};
        // Source address of the first payload quadword; quadword n came from
        // payload_source + 16n (see vif1_payload_source).
        std::uint32_t payload_source{};
        DmacSourceCursor cursor_after{};
        bool cursor_after_valid{};
    };

    enum class Vif1DmacWait : std::uint8_t {
        None,
        Direct,
        Vu1Idle,
        Flush,
        FlushAll,
    };

    enum class Vif1DmacPhase : std::uint8_t {
        Startup,
        TagTransfer,
        Payload,
        Retire,
    };

    struct PendingVif1DmacTransfer {
        std::uint32_t chcr{};
        std::uint32_t initial_chain_tadr{};
        // A timed DMA snapshots source bytes at CHCR. Marker identities bound
        // to this nonzero epoch can later follow that immutable snapshot even
        // if the guest reuses the source ring before DIRECT reaches GIF.
        std::uint64_t source_snapshot_generation{};
        std::vector<PendingVif1DmacTag> tags{};
        // Every tag's payload quadwords, back to back. One buffer for the
        // whole chain (recycled between transfers) instead of one heap block
        // per tag.
        std::vector<GsQuadword> payload_pool{};
        [[nodiscard]] std::span<const GsQuadword>
        payload_of(const PendingVif1DmacTag& tag) const noexcept {
            return {payload_pool.data() + tag.payload_begin, tag.payload_count};
        }
        DmacSourceResult result{};
        std::size_t tag_index{};
        std::size_t tag_word_index{};
        std::size_t payload_quadword_index{};
        std::size_t payload_lane{};
        Vif1DmacPhase phase{Vif1DmacPhase::Startup};
        std::uint64_t cycles_until_event{4U};
        Vif1DmacWait wait{Vif1DmacWait::None};
        // See PendingGifDmacTransfer::ee_published.
        bool ee_published{};
    };

    enum class GifPacketPhase : std::uint8_t {
        Tag,
        Packed,
        Reglist,
        Image,
    };

    struct GifPacketTracker {
        GifPacketPhase phase{GifPacketPhase::Tag};
        std::uint32_t remaining_payload_quadwords{};
        std::uint8_t image_quadwords_in_slice{};
        bool tag_ends_packet{};
        // EOP, rather than a temporary producer/DMA gap, closes a GIF
        // packet.  Keep this separate from phase: a non-EOP tag can finish
        // with phase == Tag while the path is still in the same packet.
        bool packet_open{};
        bool intermittent_boundary_open{};
    };

    struct GifPacketProgress {
        bool packet_eop{};
        bool intermittent_image_slice{};
    };

    struct Vif1Path2SourceMarker {
        std::uint64_t generation{};
        // Zero until an immediate DMA or a timed CHCR snapshot owns this
        // marker. A nonzero value must equal the active timed DMA snapshot.
        std::uint64_t source_snapshot_generation{};
        std::uint32_t source{};
        std::uint32_t chain_root{};
        std::uint32_t source_tag_location{};
        std::uint32_t source_tag_ordinal{};
        std::uint32_t source_payload_word_ordinal{};
        GsQuadword expected_quadword{};
    };

    struct QueuedGifQuadword {
        GsQuadword value{};
        std::uint32_t source{};
        std::uint32_t chain_tadr{};
        std::uint32_t source_tag_location{};
        bool source_known{};
        // Set only after source address, chain root, and all four bytes of a
        // DIRECT GIF word match an armed native handoff.  The marker travels
        // with the QW until the shared arbiter selects PATH2.
        std::optional<Vif1Path2SourceMarker> source_marker{};
        // Meaningful only on PATH2. DIRECT is true and DIRECTHL is false.
        bool can_preempt_path3{};
        // VIF retains a DIRECT/DIRECTHL QW when it reaches the GIF while an
        // unsliceable PATH3 packet owns APATH; it is not already resident in
        // PATH2's GIF FIFO.  A PATH3 source-DMA completion can clear APATH
        // without scheduling that stalled VIF producer. Keep such QWs behind
        // the packet until packet EOP or explicit source cancellation makes
        // VIF eligible again.
        bool wait_for_path3_packet_eop{};
    };

    struct Vu1XgkickState {
        std::uint16_t cursor{};
        std::uint32_t tag_quadwords_remaining{};
        std::uint32_t packet_quadwords{};
        GsQuadword submitted_first_tag{};
        GsPacketQueue queued_quadwords{};
        std::uint64_t cycle_credit{};
        std::size_t forced_quadwords{};
        bool tag_ends_packet{};
        bool submitted_first_tag_oversized{};
        bool native_output_replaced{};
        bool force_flush{};
        bool active{};
        bool observed{};

        // A default state with the PATH1 FIFO left exactly as it is. Cheaper
        // than assigning a fresh Vu1XgkickState, whose FIFO allocates.
        void reset_producer_keeping_fifo() noexcept {
            cursor = 0U;
            tag_quadwords_remaining = 0U;
            packet_quadwords = 0U;
            submitted_first_tag = {};
            cycle_credit = 0U;
            forced_quadwords = 0U;
            tag_ends_packet = false;
            submitted_first_tag_oversized = false;
            native_output_replaced = false;
            force_flush = false;
            active = false;
            observed = false;
        }
    };

    struct PendingVu1Path1Packet {
        std::vector<GsQuadword> quadwords{};
        std::uint64_t ready_ee_cycle{};
    };

    [[nodiscard]] static bool is_direct_or_cached_alias(const GuestAddress address) noexcept {
        const auto segment = address & 0xe0000000U;
        return segment == 0U || segment == 0x80000000U || segment == 0xa0000000U;
    }
    [[nodiscard]] static GuestAddress physical_address(const GuestAddress address) noexcept {
        return address & 0x1fffffffU;
    }
    [[nodiscard]] std::uint8_t read_u8_slow(GuestAddress address) const;
    [[nodiscard]] std::uint32_t read_u32_le_slow(GuestAddress address) const;
    [[nodiscard]] std::optional<std::size_t> main_ram_offset(GuestAddress address,
                                                             std::size_t length) const noexcept;
    [[nodiscard]] static std::optional<std::size_t> scratchpad_offset(GuestAddress address,
                                                                      std::size_t length) noexcept;
    [[nodiscard]] static std::optional<std::size_t> vu0_micro_offset(GuestAddress address,
                                                                     std::size_t length) noexcept;
    [[nodiscard]] static std::optional<std::size_t> vu0_data_offset(GuestAddress address,
                                                                    std::size_t length) noexcept;
    [[nodiscard]] static std::optional<std::size_t> vu1_micro_offset(GuestAddress address,
                                                                     std::size_t length) noexcept;
    [[nodiscard]] static std::optional<std::size_t> vu1_data_offset(GuestAddress address,
                                                                    std::size_t length) noexcept;
    [[nodiscard]] bool is_mmio_range(GuestAddress address, std::size_t length) const noexcept;
    void reset_gs_registers() noexcept;
    void write_gs_csr(std::uint32_t value);
    void write_gs_imr(std::uint32_t value) noexcept;
    void write_gif_control(std::uint32_t value);
    void write_gif_mode(std::uint32_t value);
    void write_vif1_fifo_word(GuestAddress address, std::uint32_t value);
    void submit_vif1_word(std::uint32_t value);
    [[nodiscard]] bool submit_timed_vif1_word(std::uint32_t value);
    [[nodiscard]] bool submit_timed_vif1_direct_quadword(PendingVif1DmacTransfer& pending,
                                                         const PendingVif1DmacTag& captured,
                                                         const GsQuadword& quadword,
                                                         std::uint32_t quadword_source);
    void set_vif1_vgw(bool waiting);
    [[nodiscard]] bool gif_path2_can_accept(bool can_preempt_path3) const noexcept;
    void set_vif1_path3_mask(bool masked);
    void write_vif1_micro_word(std::uint32_t word_index, std::uint32_t value) noexcept;
    void advance_vu1_xgkick_slow(std::uint32_t cycles);
    [[nodiscard]] bool transfer_vu1_xgkick(std::uint32_t cycles, bool flush);
    [[nodiscard]] bool snapshot_vu1_xgkick();
    void begin_timed_vu1_execution() noexcept;
    void advance_timed_vu1_execution(std::uint64_t cycles) noexcept;
    // The reference interpreter already owns an exact absolute VU cycle
    // count.  Publish that count only at an XGKICK/termination boundary,
    // where PATH1 readiness can observe it, instead of incrementing shared
    // EeMemory state after every committed pair.
    void synchronize_timed_vu1_execution(std::uint64_t elapsed_cycles) noexcept;
    void finish_timed_vu1_execution(std::uint64_t elapsed_cycles) noexcept;
    [[nodiscard]] std::uint64_t vu1_path1_packet_ready_cycle() const noexcept;
    void enqueue_vu1_path1_packet(std::vector<GsQuadword> packet);
    void enqueue_vu1_path1_quadword(const GsQuadword& word);
    void release_ready_vu1_path1_packets() {
        if (!pending_vu1_path1_packets_.empty() &&
            pending_vu1_path1_packets_.front().ready_ee_cycle <= vif1_ee_cycle_) {
            release_vu1_path1_packets_due();
        }
    }
    void release_vu1_path1_packets_due();
    [[nodiscard]] bool gif_path1_ready() const noexcept;
    void finish_vu1_xgkick_observation(bool completed) noexcept;
    [[nodiscard]] bool gif_path3_selection_blocked() const noexcept;
    [[nodiscard]] static GifPacketProgress
    advance_gif_packet(GifPacketTracker& tracker, const GsQuadword& quadword) noexcept;
    // Advances over a run of quadwords, stopping after the first packet end.
    // `taken` receives how many were consumed; the result is the progress of
    // the last one.
    [[nodiscard]] static GifPacketProgress
    advance_gif_packet_run(GifPacketTracker& tracker, std::span<const GsQuadword> quadwords,
                           std::size_t& taken) noexcept;
    void enqueue_gif_path2_quadword(const GsQuadword& quadword,
                                    bool can_preempt_path3, std::uint32_t source = 0U,
                                    std::uint32_t chain_tadr = 0U,
                                    std::uint32_t source_tag_location = 0U,
                                    bool source_known = false,
                                    std::optional<Vif1Path2SourceMarker> source_marker =
                                        std::nullopt);
    [[nodiscard]] static std::optional<std::uint32_t>
    canonical_vif1_path2_source(std::uint32_t source) noexcept;
    [[nodiscard]] static std::optional<std::uint32_t>
    canonical_vif1_chain_root(std::uint32_t chain_root) noexcept;
    [[nodiscard]] std::optional<GsQuadword>
    read_vif1_path2_source_quadword(std::uint32_t source) const noexcept;
    [[nodiscard]] std::optional<Vif1Path2SourceMarker>
    take_vif1_path2_source_marker(std::uint32_t source, std::uint32_t chain_root,
                                  std::uint32_t source_tag_location,
                                  std::uint32_t source_tag_ordinal,
                                  std::uint32_t source_payload_word_ordinal,
                                  const GsQuadword& quadword) noexcept;
    void bind_vif1_path2_markers_to_timed_snapshot(
        PendingVif1DmacTransfer& pending) noexcept;
    void cancel_vif1_path2_source_marker(
        const Vif1Path2SourceMarker& marker,
        Vif1Path2SourceMarkerCancelReason reason) noexcept;
    void cancel_all_vif1_path2_source_markers(
        Vif1Path2SourceMarkerCancelReason reason) noexcept;
    void cancel_active_vif1_path2_source_markers(
        Vif1Path2SourceMarkerCancelReason reason) noexcept;
    void cancel_queued_vif1_path2_source_markers(
        Vif1Path2SourceMarkerCancelReason reason) noexcept;
    void cancel_vif1_path2_source_markers_for_chain(
        std::uint32_t chain_root, Vif1Path2SourceMarkerCancelReason reason,
        bool cancel_queued) noexcept;
    void invalidate_vif1_path2_markers_for_main_ram_write(
        std::size_t offset, std::size_t length,
        Vif1Path2SourceMarkerCancelReason reason =
            Vif1Path2SourceMarkerCancelReason::SourceOverwritten) noexcept;
    void invalidate_vif1_path2_markers_for_scratchpad_write(
        std::size_t offset, std::size_t length,
        Vif1Path2SourceMarkerCancelReason reason =
            Vif1Path2SourceMarkerCancelReason::SourceOverwritten) noexcept;
    void pump_gif_arbiter(bool timing_advance = false);
    // GIF_STAT with FQC/APATH/OPH derived from the current arbiter state.
    [[nodiscard]] std::uint32_t gif_arbiter_status(std::uint32_t status) const noexcept;
    void update_gif_arbiter_status() noexcept;
    std::uint64_t try_batch_path3_run_ahead(std::uint64_t maximum_cycles);
    [[nodiscard]] std::uint64_t path3_quadwords_before_boundary(
        std::uint64_t limit) const noexcept;
    void reset_gif_arbiter() noexcept;
    void synchronize_gs_capture_packet_boundaries() noexcept;
    void invalidate_graphics_service_deadline_cache() noexcept;
    void publish_join_pump_owned_deadline() noexcept;
    [[nodiscard]] EeGraphicsServiceDeadline
    compute_graphics_service_deadline() const noexcept;
    [[gnu::noinline]] [[nodiscard]] EeGraphicsServiceDeadline
    recompute_graphics_service_deadline_and_publish() const noexcept;
    void release_gif_path3_source() noexcept;
    void complete_pending_gif_dmac();
    // With capture_only the snapshot is moved there instead of becoming
    // pending_vif1_dmac_ (decoupled graphics; no marker observer may be live).
    [[nodiscard]] bool start_timed_vif1_dmac(std::uint32_t value,
                                             PendingVif1DmacTransfer* capture_only = nullptr);
    // Decoupled graphics (GraphicsAsyncMode::Decoupled).
    [[nodiscard]] bool gfx_decoupled_eligible() const noexcept;
    [[nodiscard]] bool gfx_decoupled_kick_vif1(std::uint32_t value);
    [[nodiscard]] bool gfx_decoupled_kick_gif(std::uint32_t value);
    void gfx_decoupled_hand_over(std::function<void()> install);
    // Decoupled: waits out the worker and runs all handed-over graphics work
    // to quiescence on the emulation thread.
    void gfx_decoupled_drain();
    void gfx_install_vif1(PendingVif1DmacTransfer&& pending);
    void gfx_install_gif(PendingGifDmacTransfer&& pending);
    void gfx_start_queued_vif1();
    void gfx_start_queued_gif();
    [[nodiscard]] bool transfer_pending_vif1_dmac_tag();
    void recycle_vif1_pending_storage(PendingVif1DmacTransfer& pending) noexcept;
    void release_pending_vif1_dmac() noexcept;
    void release_pending_gif_dmac() noexcept;
    [[nodiscard]] bool vif1_dmac_wait_satisfied(Vif1DmacWait wait) const noexcept;
    void complete_pending_vif1_dmac();
    void fault_pending_vif1_dmac();
    void write_ipu_control(std::uint32_t value) noexcept;
    void write_ipu_command(std::uint32_t value) noexcept;
    void decode_ipu_macroblock_row() noexcept;
    void write_ipu_input_fifo_quadword(const std::array<std::uint32_t, 4>& words) noexcept;
    void enqueue_ipu_input_fifo_quadword(const std::array<std::uint32_t, 4>& words,
                                         bool retain_newest_when_full) noexcept;
    void update_ipu_bitstream_registers() noexcept;
    void advance_ipu_bitstream(std::uint32_t bit_count) noexcept;
    [[nodiscard]] bool ipu_bitstream_has(std::size_t bit_count) const noexcept;
    [[nodiscard]] std::uint32_t
    peek_ipu_bitstream_u32(std::size_t additional_bit_offset = 0U) const noexcept;
    void try_complete_ipu_command() noexcept;
    void pump_to_ipu_source_chain() noexcept;
    void complete_to_ipu_dma() noexcept;
    void fail_to_ipu_dma_bus_error() noexcept;
    void write_ipu_dmac_address(GuestAddress address, std::uint32_t value) noexcept;
    void write_ipu_dmac_qwc(GuestAddress address, std::uint32_t value) noexcept;
    void write_ipu_dmac_chcr(GuestAddress address, std::uint32_t value);
    void update_ipu_input_fifo_count() noexcept;
    void capture_to_ipu_decoder_bytes(std::size_t ram_offset, std::size_t byte_count) noexcept;
    void write_dmac_status(std::uint32_t value) noexcept;
    void write_intc_status(std::uint32_t value) noexcept;
    void write_intc_mask(std::uint32_t value) noexcept;
    void write_from_spr_dmac_chcr(std::uint32_t value);
    void write_to_spr_dmac_chcr(std::uint32_t value);
    void load_vif0_dmac_payload(std::uint32_t value);
    [[nodiscard]] bool load_vif1_dmac_payload(std::uint32_t value);
    // VIF1 chain-shape census helpers shared by the immediate and timed walk
    // paths (read-only telemetry; see EeVu1Activity::vif1_chain_stop_counts).
    [[nodiscard]] std::size_t open_vif1_chain_dump_slot(std::uint32_t start_tadr);
    void record_vif1_chain_dump_tag(std::size_t slot, const DmacSourceTag& tag);
    void close_vif1_chain_dump_slot(std::size_t slot, DmacSourceStop stop);
    // One DMAC source quadword (tag or payload): a direct host read for main
    // RAM, the general address mapper otherwise. nullopt when unmapped.
    [[nodiscard]] std::optional<DmacQuadword> read_dmac_source_quadword(
        DmacSourceAddress source) const;
    void capture_graphics_dmac_payload(GuestAddress channel_address, std::uint32_t value,
                                       std::vector<GsQuadword>* deferred_payload = nullptr,
                                       std::vector<std::size_t>* deferred_tag_payload_ends = nullptr);
    void advance_gif_normal_dmac_registers(std::uint32_t quadwords) noexcept;
    void write_completed_command_dmac_chcr(GuestAddress channel_address, std::uint32_t value);

    [[nodiscard]] std::uint32_t read_gif_status_word() const noexcept;
    // read_u32_le() of an EE MMIO register that is plain byte storage (the
    // VIF1 and DMAC channel registers), without the address classification.
    // Counts as one hardware-activity read exactly as read_u32_le() does.
    [[nodiscard]] std::uint32_t read_plain_mmio_u32(GuestAddress address) const noexcept;
    [[nodiscard]] std::uint8_t read_mmio_byte(GuestAddress physical_address) const noexcept;
    [[nodiscard]] std::uint8_t
    read_guest_mmio_byte(GuestAddress physical_address) const noexcept;
    void write_mmio_byte(GuestAddress physical_address, std::uint8_t value) noexcept;
    void store_mmio_u32(GuestAddress physical_address, std::uint32_t value) noexcept;
    void poison_main_ram_code_pages(std::size_t offset, std::size_t length) noexcept;
    void record_main_ram_code_page_write(std::size_t offset,
                                         std::size_t length) noexcept;
    void record_main_ram_mutation(std::size_t offset, std::size_t length) noexcept;
    void record_main_ram_writer(std::size_t offset, std::size_t length) noexcept;
    void record_scratchpad_writer(const std::size_t offset, const std::size_t length) noexcept {
        // Both consumers (PATH2 source markers and the VIF1 chain observer's
        // writer log) are idle in ordinary play.
        if (vif1_path2_source_marker_count_ == 0U && vif1_dmac_chain_observer_ == nullptr) [[likely]] {
            return;
        }
        record_scratchpad_writer_slow(offset, length);
    }
    void record_scratchpad_writer_slow(std::size_t offset, std::size_t length) noexcept;
    [[gnu::noinline]] void write_u8_observed(GuestAddress address, std::uint8_t value);
    [[gnu::noinline]] void write_u16_le_observed(GuestAddress address,
                                                 std::uint16_t value);
    [[gnu::noinline]] void write_u32_le_observed(GuestAddress address,
                                                 std::uint32_t value);
    [[gnu::noinline]] void write_u64_le_observed(GuestAddress address,
                                                 std::uint64_t value);
    [[gnu::noinline]] void write_u128_le_observed(
        GuestAddress address, const std::array<std::uint32_t, 4>& words);
    void write_u8_impl(GuestAddress address, std::uint8_t value);
    void write_u16_le_impl(GuestAddress address, std::uint16_t value);
    void write_u32_le_impl(GuestAddress address, std::uint32_t value);
    void write_u64_le_impl(GuestAddress address, std::uint64_t value);
    void write_u128_le_impl(GuestAddress address,
                            const std::array<std::uint32_t, 4>& words);
    void prepare_scalar_write_preimage(GuestAddress address, std::size_t width);
    [[nodiscard]] bool make_scalar_write_event(
        GuestAddress address, std::size_t width,
        std::span<const std::uint8_t> payload,
        EeMemoryWriteEvent& event) const noexcept;
    class ScalarWriteTransaction;
    struct MemoryWriteObserverSlot {
        EeMemoryWriteObserver* pointer{};

        MemoryWriteObserverSlot() = default;
        MemoryWriteObserverSlot(MemoryWriteObserverSlot&& other) noexcept
            : pointer(other.pointer) {
            other.pointer = nullptr;
        }
        MemoryWriteObserverSlot& operator=(MemoryWriteObserverSlot&& other) noexcept {
            if (this != &other) {
                pointer = other.pointer;
                other.pointer = nullptr;
            }
            return *this;
        }
        MemoryWriteObserverSlot(const MemoryWriteObserverSlot&) = delete;
        MemoryWriteObserverSlot& operator=(const MemoryWriteObserverSlot&) = delete;
    };
    [[nodiscard]] std::optional<GuestAddress>
    vif1_last_writer_pc(DmacSourceAddress address) const noexcept;
    void copy_main_ram_provenance_to_scratchpad(std::size_t destination,
                                                DmacSourceAddress source) noexcept;
    void copy_scratchpad_provenance_to_main_ram(std::size_t destination,
                                                std::size_t source) noexcept;
    void set_main_ram_code_page_generation_for_testing(
        GuestAddress address, std::uint64_t generation) noexcept;
    void set_main_ram_mutation_generation_for_testing(
        std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t main_ram_mutation_generation_for_testing() const noexcept {
        return *ee_aot_view_.mutation_generation;
    }

    struct MainRamCodePageTracker;
    std::vector<std::byte> data_;
    std::vector<std::byte> native_heap_;
    std::unique_ptr<MainRamCodePageTracker> main_ram_code_page_tracker_;
    std::array<std::byte, kEeScratchpadSize> scratchpad_{};
    std::array<std::byte, kVu0MemorySize> vu0_micro_{};
    [[nodiscard]] static std::uint64_t next_vu0_micro_generation() noexcept;
    std::uint64_t vu0_micro_generation_{next_vu0_micro_generation()};
    std::array<std::byte, kVu0MemorySize> vu0_data_{};
    std::array<std::byte, kVu1MemorySize> vu1_micro_{};
    std::array<std::byte, kVu1MemorySize> vu1_data_{};
    // Hardware is deliberately separate from main RAM. Both modeled physical
    // windows are dense and small enough to index directly; untouched bytes
    // remain zero. This avoids a hash lookup for every byte of a polled 32-bit
    // register while retaining the exact persistent byte-addressable state.
    std::array<std::uint8_t, 0x20000U> ee_mmio_bytes_{};
    std::array<std::uint8_t, 0x2000U> gs_privileged_bytes_{};
    // The IPU decoder is still a future subsystem, but its eight-QW input
    // FIFO is real hardware state needed by movie initialization and DMA.
    // Keeping this bounded prevents headless video transfers from retaining
    // an entire compressed movie while preserving the observable FIFO tail.
    std::deque<std::array<std::uint32_t, 4>> ipu_input_fifo_{};
    // BCLR/FDEC/VDEC address the compressed stream MSB-first. BP is relative
    // to the first QW retained above; complete QWs are retired on demand and
    // let an active to-IPU source chain refill the eight-QW hardware FIFO.
    std::uint32_t ipu_bitstream_offset_{};
    std::optional<std::uint32_t> pending_ipu_command_{};
    bool to_ipu_end_after_current_tag_{};
    bool pumping_to_ipu_source_chain_{};
    std::deque<PendingDmacCompletion> completed_dmac_channels_{};
    std::optional<PendingGifDmacTransfer> pending_gif_dmac_{};
    std::optional<PendingVif1DmacTransfer> pending_vif1_dmac_{};
    // Decoupled graphics: chains the guest kicked (and saw complete) while the
    // previous one was still in the device model. Device-owned, like the two
    // pending transfers above.
    std::deque<PendingVif1DmacTransfer> gfx_queued_vif1_{};
    std::deque<PendingGifDmacTransfer> gfx_queued_gif_{};
    // Capacity retained from the previous chain snapshot (see
    // recycle_vif1_pending_storage).
    std::vector<PendingVif1DmacTag> vif1_spare_tags_{};
    std::vector<GsQuadword> vif1_spare_payload_{};
    std::vector<std::uint32_t> vif1_unpack_lanes_{};
#if defined(TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS) && TAKEDOWN_ENABLE_VIF1_PURE_UNPACK_CENSUS
    std::unique_ptr<Vif1PureUnpackCensus> vif1_pure_unpack_census_{};
#endif
    std::vector<GsQuadword> gif_spare_payload_{};
    std::vector<std::size_t> gif_spare_tag_ends_{};
    // PATH1 is a live VUMem reader, PATH2 is an incremental producer, and
    // PATH3 is backed by the snapshotted DMAC payload above. The active path
    // retains its parser when its source is temporarily empty. PATH3 releases
    // APATH at each completed source DMA, so a higher path may run during the
    // producer gap; a later PATH3 DMA resumes the retained parser. Under IMT,
    // PATH1 and DIRECT (but not DIRECTHL) may also take over after each eight
    // IMAGE quadwords while a PATH3 source is active.
    ReusableFifo<QueuedGifQuadword> queued_gif_path2_{};
    // Native handoffs are rare and must never turn a malformed or runaway
    // guest packet stream into unbounded host storage. This tracks every live
    // marker, including one carried by queued PATH2, rather than only the
    // fixed active table.
    static constexpr std::size_t kVif1Path2SourceMarkerCapacity = 64U;
    std::array<std::optional<Vif1Path2SourceMarker>,
               kVif1Path2SourceMarkerCapacity>
        vif1_path2_source_markers_{};
    std::size_t vif1_path2_source_marker_count_{};
    // The store fast path's view of this memory (ee_aot_memory_view.hpp). The
    // code words live in ee_aot_code_words_, which the view points into.
    EeAotMemoryView ee_aot_view_{};
    std::vector<std::uint64_t> ee_aot_code_words_;
    [[nodiscard]] bool ee_aot_code_written(std::size_t offset, std::size_t length) const noexcept;
    std::uint64_t ee_aot_code_write_epoch_{};
    std::uint64_t ee_aot_watch_epoch_{};
    // The code-page tracker's mutation generation and page states, for the
    // inline store bookkeeping above (MainRamCodePageTracker::PageState).
    const bool* ee_aot_mutation_generation_exhausted_{};
    std::uint64_t next_vif1_path2_source_marker_generation_{1U};
    std::uint64_t next_vif1_source_snapshot_generation_{1U};
    std::array<GifPacketTracker, 3> gif_packet_trackers_{};
    // True only when capture observed the current canonical packet from its
    // first GIFtag. Capture may be disabled while the arbiter keeps running,
    // so this state is deliberately separate from the scheduling tracker.
    std::array<bool, 3> gif_capture_packet_observed_from_start_{};
    std::optional<GifPath> gif_active_path_{};
    // True only while an IMT IMAGE packet has yielded its PATH3 bus slot.
    // DIRECTHL cannot steal that suspended slot; PATH3 resumes ahead of it.
    bool gif_path3_interrupted_{};
    bool pumping_gif_arbiter_{};
    bool fast_gif_status_read_enabled_{};
    bool gif_dmac_timing_enabled_{};
    std::uint64_t gif_dmac_cycle_credit_{};
    // A timed advance cannot make a paused or PATH3-masked arbiter eligible.
    // The guest mutation which can reopen it always pumps the arbiter through
    // the ordinary path. Cache that exact stable wait so one-cycle scheduler
    // service does not rescan the unchanged GIF path inventory.
    bool gif_blocked_arbiter_cache_enabled_{};
    bool gif_arbiter_waits_for_external_wake_{};
    std::uint64_t gif_blocked_arbiter_cache_skips_{};
    bool gif_path3_batch_enabled_{true};
    bool gif_idle_pump_gate_enabled_{true};
    std::uint64_t gif_path3_batched_quadwords_{};
    std::uint64_t gif_path3_batched_spans_{};
    mutable std::uint64_t gif_idle_pump_gate_hits_{};
    // A zero epoch is permanently cache-disabled after counter exhaustion.
    // The cache stores an absolute EE-cycle deadline so advancing the shared
    // clock can materialize the exact relative result without rescanning the
    // VIF/GIF/VU1 state.
    std::uint64_t graphics_service_deadline_epoch_{1U};
    mutable std::uint64_t graphics_service_deadline_cache_epoch_{};
    mutable std::uint64_t graphics_service_deadline_cache_absolute_deadline_{};
    mutable EeGraphicsServiceDeadlineKind graphics_service_deadline_cache_kind_{
        EeGraphicsServiceDeadlineKind::Unknown};
    mutable bool graphics_service_deadline_cache_valid_{};
    bool graphics_service_deadline_cache_counters_enabled_{};
    mutable std::uint64_t graphics_service_deadline_cache_hits_{};
    mutable std::uint64_t graphics_service_deadline_cache_recomputes_{};
    mutable std::array<std::uint64_t, kEeGraphicsServiceDeadlineKindCount>
        graphics_service_deadline_cache_hits_by_kind_{};
    mutable std::array<std::uint64_t, kEeGraphicsServiceDeadlineKindCount>
        graphics_service_deadline_cache_entry_hits_by_kind_{};
    mutable std::array<std::uint64_t, kEeGraphicsServiceDeadlineKindCount>
        graphics_service_deadline_cache_recomputes_by_kind_{};
    // This is a producer-owned absolute event, published only by the EE/GIF
    // owner after a compound transition. The join-pump admission query reads
    // it without rescanning graphics state; invalidation is fail-closed.
    std::uint64_t join_pump_owned_deadline_{};
    bool join_pump_owned_deadline_valid_{};
    // Set only inside run_graphics_ahead(): advance_vif1_dmac() returns at the
    // first cycle boundary where one of its own events has made PATH3 runnable,
    // leaving the device clock at that boundary instead of the requested target.
    bool vif1_yield_when_gif_runnable_{};
    takedown::runtime::EeGraphicsServiceDiagnostics* graphics_service_diagnostics_{};
    bool vif1_dmac_timing_enabled_{};
    // True only while an already-admitted timed DIRECT word is being decoded.
    // Such a word has passed CanDoPath2 and must not acquire the legacy
    // synchronous-source wait latch when PATH3 is between source slices.
    bool vif1_dmac_direct_admitted_{};
    std::array<std::uint64_t, 10> dmac_completion_counts_{};
    // These three registers are polled at guest-instruction frequency by the
    // kernel event bridge. Keep a packed mirror alongside the byte-addressable
    // MMIO map so that a no-interrupt fast path does not perform eight hash
    // lookups per instruction.
    std::uint32_t dmac_status_{};
    std::uint32_t intc_status_{};
    std::uint32_t intc_mask_{};
    EeIpuActivity ipu_activity_{};
    EeVu1Activity vu1_activity_{};
    Vif1StreamState vif1_stream_{};
    std::optional<std::uint32_t> vif1_current_word_source_{};
    std::uint32_t vif1_current_chain_tadr_{};
    std::uint32_t vif1_current_chain_root_{};
    std::uint32_t vif1_current_tag_location_{};
    std::uint32_t vif1_current_tag_ordinal_{};
    std::uint32_t vif1_current_payload_word_ordinal_{};
    bool vif1_current_chain_root_known_{};
    bool vif1_current_tag_location_known_{};
    bool vif1_current_source_occurrence_known_{};
    // A timed chain is fully snapshotted before its first transfer event.
    // Keep this provisional epoch visible while observer callbacks run during
    // that capture, so a late arm cannot authenticate an earlier snapshot.
    bool vif1_timed_snapshot_capture_active_{};
    std::uint32_t vif1_timed_snapshot_capture_chain_root_{};
    // Raw mutable spans cannot report later stores. An observer installation
    // establishes a controlled epoch; a new mutable span invalidates it for
    // the relevant source region until the observer is re-established.
    std::uint64_t main_ram_mutable_view_epoch_{};
    std::uint64_t scratchpad_mutable_view_epoch_{};
    std::uint64_t vif1_path2_marker_main_ram_view_epoch_{};
    std::uint64_t vif1_path2_marker_scratchpad_view_epoch_{};
    bool vif1_path2_marker_memory_epoch_established_{};
    // A pre-snapshot offer remains valid only while both registrations retain
    // the exact epoch observed before the source walk. Epoch zero is a
    // permanently exhausted state, so wraparound can never revive an older
    // observer or GS-capture ownership interval.
    std::uint64_t vif1_path2_marker_observer_epoch_{1U};
    std::uint64_t gs_capture_epoch_{1U};
    std::uint64_t vif1_pre_snapshot_transaction_epoch_{1U};
    // The candidate resolver is large enough to hold sixty-four complete
    // batch entries. Allocate it only when the opt-in observer first offers
    // work, then reuse it outside start_timed_vif1_dmac's ordinary stack
    // frame. Nested source walks decline the optional offer rather than
    // aliasing an outer walk's scratch state.
    struct Vif1DmacPreSnapshotResolverState;
    std::unique_ptr<Vif1DmacPreSnapshotResolverState> vif1_dmac_pre_snapshot_resolver_{};
    bool vif1_dmac_pre_snapshot_resolver_in_use_{};
    Vu1XgkickState vu1_xgkick_{};
    std::deque<PendingVu1Path1Packet> pending_vu1_path1_packets_{};
    std::size_t pending_vu1_path1_quadwords_{};
    Vu1XgkickMode vu1_xgkick_mode_{Vu1XgkickMode::DelayedSnapshot};
    Vu1XgkickObserver* vu1_xgkick_observer_{};
    bool native_strip_suppress_reference_packets_{};
    takedown::runtime::slus_21050::NativeGeometryProgramCache native_geometry_program_cache_;
    Vu0MicroCallObserver* vu0_micro_call_observer_{};
    Vif1DmacChainObserver* vif1_dmac_chain_observer_{};
    Vif1Path2SourceMarkerObserver* vif1_path2_source_marker_observer_{};
    Vu1LaunchObserver* vu1_launch_observer_{};
    std::uint32_t vu1_launch_source_begin_{};
    std::uint32_t vu1_launch_source_end_{};
    std::uint32_t vu1_launch_source_tag_{};
    // One PC per 16-byte DMAtag/quadword. Allocated only for the opt-in
    // producer trace; zero denotes no observed guest writer.
    std::vector<GuestAddress> vif1_main_ram_last_writer_pc_{};
    std::vector<GuestAddress> vif1_scratchpad_last_writer_pc_{};
    std::uint16_t vu1_current_micro_entry_{};
    std::uint16_t vu1_current_micro_resolved_entry_{};
    Vu1ProgramHash vu1_current_micro_program_hash_{};
    bool vu1_current_micro_program_hash_known_{};
    std::uint64_t vu1_micro_run_sequence_{};
    // run_vu1_micro() currently computes a complete VU launch in one host
    // call. These clocks retain the hardware-visible producer time so a late
    // XGKICK cannot overtake following VIF1 DIRECT traffic merely because the
    // host finished the microprogram synchronously.
    std::uint64_t vif1_ee_cycle_{};
    std::uint64_t vu1_launch_ee_cycle_{};
    std::uint64_t vu1_execution_cycles_{};
    std::uint64_t vu1_busy_until_ee_cycle_{};
    bool timed_vu1_execution_active_{};
    Vu1ProgramCache vu1_program_cache_{};
    Vu1State vu1_state_{};
    GuestAddress current_ee_pc_{};
    std::uint64_t current_guest_step_{};
    // See hardware_activity_epoch(). Mutable because guest bus reads are const
    // yet a VU1 read can apply a pending worker journal.
    mutable std::uint64_t hardware_activity_epoch_{};
    void note_hardware_activity() const noexcept { ++hardware_activity_epoch_; }
    [[nodiscard]] const std::byte* vu0_local_bytes(const GuestAddress address,
                                                   const std::size_t length) const noexcept {
        if (address >= kVu0MicroMemoryStart && address - kVu0MicroMemoryStart <= kVu0MemorySize - length) {
            return vu0_micro_.data() + (address - kVu0MicroMemoryStart);
        }
        if (address >= kVu0DataMemoryStart && address - kVu0DataMemoryStart <= kVu0MemorySize - length) {
            return vu0_data_.data() + (address - kVu0DataMemoryStart);
        }
        return nullptr;
    }
    MemoryWriteObserverSlot ee_memory_write_observer_{};
    std::uint32_t scalar_write_transaction_depth_{};
    std::size_t ram_write_watch_start_{};
    std::size_t ram_write_watch_end_{};
    std::uint64_t ram_write_watch_after_step_{};
    std::size_t ram_write_watch_logged_{};
    bool ram_write_watch_active_{false};
    std::unique_ptr<IpuMpeg2Decoder> ipu_mpeg2_decoder_{};
    // Command-0x7 macroblock-row decode state. A decoded picture is streamed
    // out one 16-pixel macroblock row per command; ipu_active_frame_ holds the
    // current picture and ipu_next_macroblock_row_ the next row to emit. The
    // from-IP DMA destination is captured when the guest starts channel three
    // (the retail driver programs FROM_MADR/FROM_QWC/FROM_CHCR before every
    // command-0x7) so the decoded row lands in guest RAM at the exact address
    // the driver's presentation loop expects, even though this HLE completes
    // the channel's control flow at start.
    std::optional<IpuDecodedFrame> ipu_active_frame_{};
    std::size_t ipu_next_macroblock_row_{};
    std::optional<GuestAddress> ipu_from_ipu_destination_{};
    std::optional<std::size_t> ipu_from_ipu_capacity_{};
    GsCapture gs_capture_{};
    GsPrivilegedDisplayObserver gs_privileged_display_{};
    bool gs_capture_enabled_{};
    // Stage 2 access-window tracker. Heap-allocated on first recorded window
    // so the flag-off default stays allocation-free; defined in ee_memory.cpp.
    // Mutable: guest reads are const but still observed windows.
    struct Vu1ObservedWindowTracker;
    mutable std::unique_ptr<Vu1ObservedWindowTracker> vu1_observed_windows_;
    mutable bool vu1_observed_window_recording_{};
    void record_vu1_observed_window(bool write, GuestAddress address,
                                    std::size_t length) const;
    // Milestone 3 consume: journal application and the run-result accounting
    // shared by the synchronous launch tail and the consume path (defined in
    // ee_memory.cpp; types completed there).
    void apply_vu1_worker_journal(const takedown::runtime::Vu1WorkerJournal& journal);
    void record_vu1_run_result(std::uint16_t entry, const Vu1RunResult& result);

    // ---- Asynchronous graphics window (see configure_graphics_async) ----
    //
    // Fields below are written only by the emulation thread, and only while
    // the worker is idle (at open, before the job is posted; at close, after
    // it was awaited). The worker reads gfx_window_real_ and the saved status.
    using GfxFence = takedown::runtime::GraphicsAsyncFence;
    takedown::runtime::GraphicsAsyncMode gfx_mode_{takedown::runtime::GraphicsAsyncMode::Off};
    bool gfx_window_{};        // a window (real or measured) is open
    bool gfx_window_real_{};   // the worker owns the device model
    std::uint64_t gfx_latency_cycles_{1'500'000U};
    std::uint64_t gfx_minimum_quadwords_{64U};
    // Measure only: fence kinds (bit per GfxFence) that leave a measured
    // window open, to estimate the overlap a design without them could reach.
    std::uint32_t gfx_measure_ignored_fences_{};
    bool gfx_decoupled_{};
    // Decoupled: a GIF transfer was kicked and the guest has not read GIF_STAT
    // since. Burnout kicks PATH3 and spins until GIF_STAT.FQC is nonzero (the
    // transfer has started) before it kicks VIF1; by the time the emulation
    // thread can look, the worker may already have drained the FIFO, so that
    // first read reports the started transfer. Emulation thread only.
    mutable bool gfx_gif_start_unobserved_{};
    // EE cycles that elapsed through each service entry point since the
    // window opened. In a real window these are the cycles still owed to the
    // device model; in a measured window they only age the window.
    std::uint64_t gfx_age_vif1_{};
    std::uint64_t gfx_age_gif_{};
    std::size_t gfx_window_queued_completions_{};
    std::uint32_t gfx_window_dmac_status_{};
    std::exception_ptr gfx_deferred_error_{};
    struct GfxAsyncImpl;
    std::unique_ptr<GfxAsyncImpl> gfx_async_{};

    // The window-aware EE-visible answers. All are valid only for the
    // emulation thread; the worker (and the device model it runs) uses the
    // underlying members directly.
    [[nodiscard]] EeGraphicsServiceDeadline gfx_window_deadline() const noexcept {
        const auto age = std::max(gfx_age_vif1_, gfx_age_gif_);
        return EeGraphicsServiceDeadline{
            .kind = EeGraphicsServiceDeadlineKind::InternalDeadline,
            .cycles_until_event = gfx_latency_cycles_ > age ? gfx_latency_cycles_ - age : 1U,
        };
    }
    // True for the emulation thread while the worker owns the device model;
    // the worker itself always sees the real state.
    [[nodiscard]] bool gfx_ee_view() const noexcept {
        return gfx_window_real_ && !takedown::runtime::on_graphics_async_worker_thread();
    }
    [[nodiscard]] std::uint32_t gfx_dmac_status_for_reader() const noexcept {
        // Decoupled: D_STAT belongs to the emulation thread throughout.
        return gfx_window_real_ && !gfx_decoupled_ &&
                       !takedown::runtime::on_graphics_async_worker_thread()
                   ? gfx_window_dmac_status_
                   : dmac_status_;
    }
    // Waits out an open window unless called from the worker itself.
    void gfx_sync(const GfxFence reason) const noexcept {
        if (gfx_window_) [[unlikely]] {
            gfx_sync_slow(reason);
        }
    }
    [[gnu::noinline]] void gfx_sync_slow(GfxFence reason) const noexcept;
    // An accessor fence that names its accessor in the fence histogram
    // (tags 0xacce5501.. instead of a guest PC).
    void gfx_sync_accessor(const GuestAddress tag) const noexcept {
        if (gfx_window_) [[unlikely]] {
            gfx_sync_accessor_slow(tag);
        }
    }
    [[gnu::noinline]] void gfx_sync_accessor_slow(GuestAddress tag) const noexcept;
    // Classifies an EE bus access against the device ranges and closes the
    // window if it could observe or change graphics state.
    [[gnu::noinline]] void gfx_observe_bus(GuestAddress address, bool write) const noexcept;
    // Accounts EE cycles passing through a service entry point. Returns true
    // when the call was fully absorbed (real window) and must not reach the
    // device model.
    [[gnu::noinline]] bool gfx_note_service(std::uint64_t cycles, bool vif1) noexcept;
    void gfx_close_window(GfxFence reason, GuestAddress address = 0U) noexcept;
    [[nodiscard]] std::uint64_t run_graphics_ahead_sync(std::uint64_t maximum_cycles);
    void advance_vif1_dmac_sync(std::uint64_t ee_cycles);
    void advance_gif_dmac_sync(std::uint64_t ee_cycles);
    friend struct GfxAsyncImpl;

    friend Vu1RunResult run_vu1_micro(Vu1State&, EeMemory&, std::uint16_t,
                                      Vu1RunOptions);
    friend class EeMemoryCodePageTrackerTestAccess;
    friend class EeMemoryMainRamMutationTrackerTestAccess;
};

} // namespace takedown::guest
