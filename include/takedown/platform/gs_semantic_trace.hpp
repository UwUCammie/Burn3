#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_primitive_assembler.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <optional>

namespace takedown::platform {

// A semantic trace is deliberately bounded independently of the GS capture's
// in-memory histories. With selected_vblank unset it records from process
// boot. With a selected VBlank it still tracks all preceding GS state, but
// writes only the interval beginning at that modeled VBlank and ending at the
// next one. retain_last_nonempty_completed_vblank instead keeps one bounded
// temporary interval and replaces it when a completed interval observes at
// least one TAG/REG/IMAGE/KICK/RESET event. Idle VBlanks therefore cannot
// evict the most recent useful interval. On close it writes that interval and
// discards the current, possibly partial one.
// This makes a late frontend fault reproducible without a huge log or advance
// knowledge of its VBlank index.
struct GsSemanticTraceConfig {
    std::uint64_t maximum_events{100'000U};
    std::optional<std::uint64_t> selected_vblank{};
    bool retain_last_nonempty_completed_vblank{};
};

struct GsSemanticTraceStatistics {
    std::uint64_t events_written{};
    std::uint64_t events_omitted{};
    std::uint64_t events_filtered{};
    std::uint64_t tags_observed{};
    std::uint64_t register_writes_observed{};
    std::uint64_t image_transfers_observed{};
    std::uint64_t draw_kicks_observed{};
    std::uint64_t resets_observed{};
    std::uint64_t current_vblank{};
    std::optional<std::uint64_t> retained_nonempty_vblank{};
    bool event_limit_reached{};
};

enum class GsSemanticTraceCheckpointStatus : std::uint8_t {
    Written,
    NoRetainedFrame,
};

// Compact deterministic text at the exact post-arbitration GsCapture seam.
// Raw observation supplies GIFtag framing; decoded observation supplies the
// canonical register/IMAGE command order seen by the renderer. The class is
// both observers so one instance can correlate those two views.
class GsSemanticTraceWriter final : public guest::GsRawGifObserver,
                                    public guest::GsCommandObserver,
                                    public guest::Vu0MicroCallObserver,
                                    public guest::Vu1XgkickObserver,
                                    public guest::Vif1DmacChainObserver {
  public:
    explicit GsSemanticTraceWriter(const std::filesystem::path& output_path,
                                   GsSemanticTraceConfig config = {});
    ~GsSemanticTraceWriter() override;

    GsSemanticTraceWriter(const GsSemanticTraceWriter&) = delete;
    GsSemanticTraceWriter& operator=(const GsSemanticTraceWriter&) = delete;
    GsSemanticTraceWriter(GsSemanticTraceWriter&&) = delete;
    GsSemanticTraceWriter& operator=(GsSemanticTraceWriter&&) = delete;

    void on_gs_gif_quadword(guest::GifPath path, const guest::GsQuadword& quadword) override;
    void on_gs_gif_reset() noexcept override;
    void on_gs_gif_tag_begin(guest::GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const guest::GsRegisterWrite& write) override;
    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override;
    [[nodiscard]] bool on_vu0_micro_call_begin(
        const guest::Vu0MicroCallObservation& observation,
        const guest::R5900State& state, const guest::EeMemory& memory) noexcept override;
    void on_vu0_micro_call_end(const guest::Vu0MicroCallObservation& observation,
                               const guest::R5900State& state,
                               const guest::Vu0MicroRunResult& result) noexcept override;
    [[nodiscard]] bool
    on_vu1_xgkick_begin(const guest::Vu1XgkickObservation& observation) noexcept override;
    void on_vu1_xgkick_quadword(const guest::GsQuadword& quadword) noexcept override;
    void on_vu1_xgkick_end(bool completed) noexcept override;
    [[nodiscard]] bool on_vif1_dmac_chain_begin(
        const guest::Vif1DmacChainObservation& observation) noexcept override;
    void on_vif1_dmac_chain_tag(
        const guest::Vif1DmacTagObservation& observation) noexcept override;
    void on_vif1_dmac_chain_end(
        const guest::Vif1DmacChainResultObservation& observation) noexcept override;

    // Called after the runner raises a modeled VBlank start. VBlank zero is
    // the boot-to-first-VBlank interval; note_vblank(1) begins the next one.
    void note_vblank(std::uint64_t vblank_index);

    [[nodiscard]] const GsSemanticTraceStatistics& statistics() const noexcept;

    // Writes a self-contained copy of the newest nonempty completed VBlank
    // retained by `last` mode without closing this writer or consuming the
    // retained interval. The destination is exclusively created, just like
    // the primary trace. NoRetainedFrame leaves the destination untouched.
    // Calling this for a writer not configured for `last` mode is an error.
    [[nodiscard]] GsSemanticTraceCheckpointStatus
    checkpoint_last_completed_vblank(const std::filesystem::path& output_path);

    void flush();
    void close();

  private:
    struct RawPathState {
        std::uint64_t payload_quadwords_remaining{};
    };

    struct PendingPath1Producer {
        std::uint64_t sequence{};
        std::uint64_t quadwords_remaining{};
    };

    [[nodiscard]] bool begin_event();
    [[nodiscard]] bool begin_semantic_event();
    [[nodiscard]] std::FILE* event_output() const noexcept;
    [[nodiscard]] std::uint64_t event_number() const noexcept;
    [[nodiscard]] bool selected_interval(std::uint64_t vblank_index) const noexcept;
    void begin_last_interval(std::uint64_t vblank_index);
    void complete_last_interval();
    void discard_current_last_interval() noexcept;
    void copy_completed_last_interval();
    void close_temporary_files() noexcept;
    void write_header();
    void write_footer();
    void require_open() const;
    void check_write(int result) const;
    void reset_drawing_state() noexcept;
    [[nodiscard]] std::uint64_t
    associate_path1_quadword(guest::GifPath path) noexcept;
    [[nodiscard]] std::optional<guest::GsPrimitiveKickSnapshot>
    forward_register_write(const guest::GsRegisterWrite& write) noexcept;
    void write_draw_kick(const guest::GsRegisterWrite& write,
                         const guest::GsPrimitiveKickSnapshot& kick);

    std::FILE* output_{};
    std::filesystem::path output_path_;
    GsSemanticTraceConfig config_{};
    GsSemanticTraceStatistics statistics_{};
    std::array<RawPathState, 3> raw_paths_{};
    std::deque<PendingPath1Producer> pending_path1_producers_{};
    guest::GsPrimitiveAssembler assembler_{};
    std::uint64_t bitbltbuf_{};
    std::uint64_t trxpos_{};
    std::uint64_t trxreg_{};
    std::uint64_t trxdir_{};
    std::uint32_t packed_q_bits_{0x3f800000U};
    std::FILE* current_interval_output_{};
    std::FILE* completed_interval_output_{};
    std::uint64_t interval_events_written_{};
    std::uint64_t interval_events_omitted_{};
    std::uint64_t interval_semantic_events_observed_{};
    std::uint64_t completed_events_written_{};
    std::uint64_t completed_events_omitted_{};
    std::uint64_t completed_vblank_{};
    std::uint64_t xgkick_sequence_{};
    std::uint64_t active_xgkick_sequence_{};
    std::uint64_t active_xgkick_hash_{};
    std::uint64_t active_xgkick_quadwords_{};
    std::uint64_t active_xgkick_path1_quadwords_observed_{};
    std::uint64_t current_path1_xgkick_sequence_{};
    guest::GsQuadword active_xgkick_first_quadword_{};
    bool active_xgkick_has_quadword_{};
    std::uint64_t vu0_call_sequence_{};
    std::uint64_t active_vu0_call_sequence_{};
    std::uint64_t vif1_chain_sequence_{};
    std::uint64_t active_vif1_chain_sequence_{};
    bool interval_limit_reached_{};
    bool completed_limit_reached_{};
    bool have_completed_interval_{};
    bool active_{};
    bool footer_written_{};
};

} // namespace takedown::platform
