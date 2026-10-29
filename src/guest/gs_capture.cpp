#include "takedown/guest/gs_capture.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace takedown::guest {

namespace {

constexpr std::size_t kMaxImageHistory = 16U;
constexpr std::size_t kMaxQueuedImageBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaxTransferBytes = 4U * 1024U * 1024U;

// Two consecutive 32-bit lanes of a quadword as one little-endian 64-bit value.
template <std::size_t Lane>
[[nodiscard]] std::uint64_t quadword_u64(const GsQuadword& quadword) noexcept {
    return static_cast<std::uint64_t>(quadword[Lane]) |
           (static_cast<std::uint64_t>(quadword[Lane + 1U]) << 32U);
}

// REGLIST packs two 64-bit register values into each quadword.
[[nodiscard]] std::uint64_t reglist_quadword_count(const std::uint64_t register_values) noexcept {
    return (register_values + 1U) / 2U;
}

// Widens one RGB5 channel of a PSMCT16 texel to 8 bits.
[[nodiscard]] std::uint8_t expand_rgb5(const std::uint16_t channel) noexcept {
    return static_cast<std::uint8_t>((channel << 3U) | (channel >> 2U));
}

// Byte size of a complete host-to-local transfer, or nothing when the
// geometry, the pixel format or the size is not something capture supports.
[[nodiscard]] std::optional<std::uint64_t> transfer_byte_count(const std::uint32_t width,
                                                               const std::uint32_t height,
                                                               const std::uint32_t psm) noexcept {
    if (width == 0U || height == 0U) {
        return std::nullopt;
    }
    const auto pixel_count = static_cast<std::uint64_t>(width) * height;
    std::uint64_t count = pixel_count;
    switch (psm) {
    case 0U: // PSMCT32
        if (pixel_count > std::numeric_limits<std::uint64_t>::max() / 4U) {
            return std::nullopt;
        }
        count *= 4U;
        break;
    case 1U: // PSMCT24
        if (pixel_count > std::numeric_limits<std::uint64_t>::max() / 3U) {
            return std::nullopt;
        }
        count *= 3U;
        break;
    case 2U:  // PSMCT16
    case 10U: // PSMCT16S
        if (pixel_count > std::numeric_limits<std::uint64_t>::max() / 2U) {
            return std::nullopt;
        }
        count *= 2U;
        break;
    case 19U: // PSMT8
        break;
    case 20U: // PSMT4
        count = (count + 1U) / 2U;
        break;
    default:
        return std::nullopt;
    }
    if (count > kMaxTransferBytes) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(count);
}

} // namespace

const char* to_string(const GsCaptureDiagnosticCode code) noexcept {
    switch (code) {
    case GsCaptureDiagnosticCode::UnsupportedGifFormat:
        return "unsupported GIF format";
    case GsCaptureDiagnosticCode::UnsupportedPixelStorageMode:
        return "unsupported GS pixel storage mode";
    case GsCaptureDiagnosticCode::InvalidImageTransfer:
        return "invalid image transfer";
    case GsCaptureDiagnosticCode::UnsupportedVifCommand:
        return "unsupported VIF command";
    case GsCaptureDiagnosticCode::IncompleteVifDirect:
        return "incomplete VIF DIRECT";
    case GsCaptureDiagnosticCode::DmaChainOutOfRange:
        return "DMA chain out of range";
    case GsCaptureDiagnosticCode::UnsupportedDmaMode:
        return "unsupported DMA mode";
    case GsCaptureDiagnosticCode::UnsupportedDmaTag:
        return "unsupported DMA tag";
    case GsCaptureDiagnosticCode::DmaChainLimit:
        return "DMA chain limit";
    case GsCaptureDiagnosticCode::IncompleteGifPacket:
        return "incomplete GIF packet";
    case GsCaptureDiagnosticCode::CaptureLimit:
        return "capture limit";
    }
    return "unknown GS capture diagnostic";
}

void GsCapture::begin_vif1_dma() noexcept {
    vif_dma_active_ = true;
    vif_dma_desynchronized_ = false;
}

void GsCapture::submit_vif1_word(const std::uint32_t word) {
    if (!vif_dma_active_ || vif_dma_desynchronized_) {
        return;
    }
    if (vif_direct_words_remaining_ != 0U) {
        vif_direct_quadword_[vif_direct_lane_] = word;
        ++vif_direct_lane_;
        --vif_direct_words_remaining_;
        if (vif_direct_lane_ == 4U) {
            submit_gif_quadword(GifPath::Path2, vif_direct_quadword_);
            vif_direct_lane_ = 0U;
        }
        return;
    }

    const auto command = (word >> 24U) & 0x7FU;
    switch (command) {
    case 0x00U:
    case 0x01U:
    case 0x02U:
    case 0x03U:
    case 0x04U:
    case 0x05U:
    case 0x06U:
    case 0x07U:
    case 0x10U:
    case 0x11U:
    case 0x13U:
        return;
    case 0x50U:
    case 0x51U: {
        auto quadword_count = word & 0xFFFFU;
        if (quadword_count == 0U) {
            quadword_count = 0x10000U;
        }
        vif_direct_words_remaining_ = quadword_count * 4U;
        vif_direct_lane_ = 0U;
        return;
    }
    default:
        record_diagnostic(GsCaptureDiagnosticCode::UnsupportedVifCommand, command, word);
        vif_dma_desynchronized_ = true;
        return;
    }
}

void GsCapture::submit_vif1_quadword(const GsQuadword& quadword) {
    for (const auto word : quadword) {
        submit_vif1_word(word);
    }
}

void GsCapture::end_vif1_dma() {
    if (vif_direct_words_remaining_ != 0U || vif_direct_lane_ != 0U) {
        record_diagnostic(GsCaptureDiagnosticCode::IncompleteVifDirect,
                          vif_direct_words_remaining_);
    }
    vif_direct_words_remaining_ = 0U;
    vif_direct_lane_ = 0U;
    vif_dma_active_ = false;
    vif_dma_desynchronized_ = false;
}

void GsCapture::submit_gif_quadword_general(const GifPath path, const GsQuadword& quadword) {
    const auto path_index = static_cast<std::size_t>(path);
    const bool packet_was_open = gif_packet_observation_open_[path_index];
    gif_packet_observation_open_[path_index] = true;
    if (gif_packet_eop_tracking_active_) {
        const bool primary_command_available =
            !raw_passthrough_only_ && command_observer_ != nullptr;
        if (!packet_was_open) {
            gif_packet_primary_command_continuous_[path_index] =
                gif_packet_start_synchronized_[path_index] && primary_command_available;
        } else {
            gif_packet_primary_command_continuous_[path_index] =
                gif_packet_primary_command_continuous_[path_index] && primary_command_available;
        }
    }

    if (raw_gif_observer_ != nullptr) {
        raw_gif_observer_->on_gs_gif_quadword(path, quadword);
    }
    if (terminal_raw_gif_observer_ != nullptr &&
        terminal_raw_gif_observer_ != raw_gif_observer_) {
        terminal_raw_gif_observer_->on_gs_gif_quadword(path, quadword);
    }
    if (diagnostic_raw_gif_observer_ != nullptr &&
        diagnostic_raw_gif_observer_ != raw_gif_observer_ &&
        diagnostic_raw_gif_observer_ != terminal_raw_gif_observer_) {
        diagnostic_raw_gif_observer_->on_gs_gif_quadword(path, quadword);
    }
    if (raw_passthrough_only_) {
        return;
    }

    const GifPayloadState& payload = gif_payloads_[path_index];
    switch (payload.kind) {
    case GifPayloadKind::Tag:
        begin_gif_tag(path, quadword);
        break;
    case GifPayloadKind::Packed:
        consume_packed_quadword(path, quadword);
        break;
    case GifPayloadKind::Reglist:
        consume_reglist_quadword(path, quadword);
        break;
    case GifPayloadKind::Image:
        consume_image_quadword(path, quadword);
        break;
    }
}

void GsCapture::record_diagnostic(const GsCaptureDiagnosticCode code, const std::uint32_t detail,
                                  const std::uint32_t address) {
    const GsCaptureDiagnostic diagnostic{code, detail, address};
    if (std::find(diagnostics_.begin(), diagnostics_.end(), diagnostic) != diagnostics_.end()) {
        return;
    }
    if (diagnostics_.size() < 128U) {
        diagnostics_.push_back(diagnostic);
    }
}

std::span<const GsRegisterWrite> GsCapture::register_writes() const noexcept {
    return register_writes_;
}

std::span<const GsImageTransfer> GsCapture::image_transfers() const noexcept {
    return image_transfers_;
}

std::span<const GsCaptureDiagnostic> GsCapture::diagnostics() const noexcept {
    return diagnostics_;
}

const GsCaptureActivity& GsCapture::activity() const noexcept {
    return activity_;
}

bool GsCapture::gif_observation_quiescent() const noexcept {
    return std::none_of(gif_packet_observation_open_.begin(), gif_packet_observation_open_.end(),
                        [](const bool open) { return open; });
}

std::vector<GsImageTransfer> GsCapture::take_completed_image_transfers() {
    std::vector<GsImageTransfer> transfers{};
    transfers.reserve(completed_image_transfers_.size());
    while (!completed_image_transfers_.empty()) {
        auto& image = completed_image_transfers_.front();
        queued_image_bytes_ -= image.pixels.size();
        transfers.push_back(std::move(image));
        completed_image_transfers_.pop_front();
    }
    return transfers;
}

void GsCapture::set_command_observer(GsCommandObserver* const observer) noexcept {
    if (command_observer_ != observer) {
        invalidate_open_gif_packet_continuity();
    }
    command_observer_ = observer;
}

void GsCapture::set_raw_gif_observer(GsRawGifObserver* const observer) noexcept {
    if (raw_gif_observer_ != observer) {
        invalidate_open_gif_packet_continuity();
        if (std::any_of(gif_packet_observation_open_.begin(),
                        gif_packet_observation_open_.end(),
                        [](const bool open) { return open; }) &&
            raw_gif_observer_ != nullptr &&
            raw_gif_observer_ != terminal_raw_gif_observer_ &&
            raw_gif_observer_ != diagnostic_raw_gif_observer_) {
            raw_gif_observer_->on_gs_gif_reset();
        }
    }
    raw_gif_observer_ = observer;
    raw_gif_packet_eop_observer_ =
        observer != nullptr && observer->observes_gs_gif_packet_eop() ? observer : nullptr;
    gif_packet_eop_tracking_active_ = raw_gif_packet_eop_observer_ != nullptr ||
                                      terminal_raw_gif_packet_eop_observer_ != nullptr ||
                                      diagnostic_raw_gif_packet_eop_observer_ != nullptr;
    refresh_raw_only_observer();
}

void GsCapture::refresh_raw_only_observer() noexcept {
    const bool only_raw = raw_passthrough_only_ && !gif_packet_eop_tracking_active_ &&
                          raw_gif_observer_ != nullptr &&
                          (terminal_raw_gif_observer_ == nullptr ||
                           terminal_raw_gif_observer_ == raw_gif_observer_) &&
                          (diagnostic_raw_gif_observer_ == nullptr ||
                           diagnostic_raw_gif_observer_ == raw_gif_observer_);
    raw_only_observer_ = only_raw ? raw_gif_observer_ : nullptr;
}

void GsCapture::set_terminal_command_observer(GsCommandObserver* const observer) noexcept {
    if (terminal_command_observer_ != observer) {
        invalidate_open_gif_packet_continuity();
    }
    terminal_command_observer_ = observer;
}

void GsCapture::set_terminal_raw_gif_observer(GsRawGifObserver* const observer) noexcept {
    if (terminal_raw_gif_observer_ != observer) {
        invalidate_open_gif_packet_continuity();
        if (std::any_of(gif_packet_observation_open_.begin(),
                        gif_packet_observation_open_.end(),
                        [](const bool open) { return open; }) &&
            terminal_raw_gif_observer_ != nullptr &&
            terminal_raw_gif_observer_ != raw_gif_observer_ &&
            terminal_raw_gif_observer_ != diagnostic_raw_gif_observer_) {
            terminal_raw_gif_observer_->on_gs_gif_reset();
        }
    }
    terminal_raw_gif_observer_ = observer;
    terminal_raw_gif_packet_eop_observer_ =
        observer != nullptr && observer->observes_gs_gif_packet_eop() ? observer : nullptr;
    gif_packet_eop_tracking_active_ = raw_gif_packet_eop_observer_ != nullptr ||
                                      terminal_raw_gif_packet_eop_observer_ != nullptr ||
                                      diagnostic_raw_gif_packet_eop_observer_ != nullptr;
    refresh_raw_only_observer();
}

void GsCapture::set_diagnostic_command_observer(GsCommandObserver* const observer) noexcept {
    diagnostic_command_observer_ = observer;
}

void GsCapture::set_diagnostic_raw_gif_observer(GsRawGifObserver* const observer) noexcept {
    if (diagnostic_raw_gif_observer_ != observer) {
        invalidate_open_gif_packet_continuity();
        if (std::any_of(gif_packet_observation_open_.begin(),
                        gif_packet_observation_open_.end(),
                        [](const bool open) { return open; }) &&
            diagnostic_raw_gif_observer_ != nullptr &&
            diagnostic_raw_gif_observer_ != raw_gif_observer_ &&
            diagnostic_raw_gif_observer_ != terminal_raw_gif_observer_) {
            diagnostic_raw_gif_observer_->on_gs_gif_reset();
        }
    }
    diagnostic_raw_gif_observer_ = observer;
    diagnostic_raw_gif_packet_eop_observer_ =
        observer != nullptr && observer->observes_gs_gif_packet_eop() ? observer : nullptr;
    gif_packet_eop_tracking_active_ = raw_gif_packet_eop_observer_ != nullptr ||
                                      terminal_raw_gif_packet_eop_observer_ != nullptr ||
                                      diagnostic_raw_gif_packet_eop_observer_ != nullptr;
    refresh_raw_only_observer();
}

void GsCapture::set_raw_passthrough_only(const bool enabled) noexcept {
    if (raw_passthrough_only_ != enabled) {
        invalidate_open_gif_packet_continuity();
        if (std::any_of(gif_packet_observation_open_.begin(),
                        gif_packet_observation_open_.end(),
                        [](const bool open) { return open; })) {
            reset_gif_paths();
        }
    }
    raw_passthrough_only_ = enabled;
    refresh_raw_only_observer();
}

void GsCapture::set_image_history_enabled(const bool enabled) noexcept {
    image_history_enabled_ = enabled;
    if (!enabled) {
        image_transfers_.clear();
        completed_image_transfers_.clear();
        queued_image_bytes_ = 0U;
    }
}

void GsCapture::reset_gif_paths() noexcept {
    gif_payloads_ = {};
    gif_packet_observation_open_ = {};
    gif_packet_primary_command_continuous_ = {};
    gif_packet_start_synchronized_.fill(false);
    host_to_local_transfer_ = {};
    ++next_image_transfer_generation_;
    if (raw_gif_observer_ != nullptr) {
        raw_gif_observer_->on_gs_gif_reset();
    }
    if (terminal_raw_gif_observer_ != nullptr &&
        terminal_raw_gif_observer_ != raw_gif_observer_) {
        terminal_raw_gif_observer_->on_gs_gif_reset();
    }
    if (diagnostic_raw_gif_observer_ != nullptr &&
        diagnostic_raw_gif_observer_ != raw_gif_observer_ &&
        diagnostic_raw_gif_observer_ != terminal_raw_gif_observer_) {
        diagnostic_raw_gif_observer_->on_gs_gif_reset();
    }
}

void GsCapture::reset_gs_state() {
    reset_gif_paths();
    gs_bitbltbuf_ = 0U;
    gs_trxpos_ = 0U;
    gs_trxreg_ = 0U;
    gs_trxdir_ = 0U;
    if (command_observer_ != nullptr) {
        command_observer_->on_gs_reset();
    }
    if (terminal_command_observer_ != nullptr &&
        terminal_command_observer_ != command_observer_) {
        terminal_command_observer_->on_gs_reset();
    }
    if (diagnostic_command_observer_ != nullptr &&
        diagnostic_command_observer_ != command_observer_ &&
        diagnostic_command_observer_ != terminal_command_observer_) {
        diagnostic_command_observer_->on_gs_reset();
    }
}

void GsCapture::clear() noexcept {
    reset_gif_paths();
    vif_direct_quadword_.fill(0U);
    vif_direct_words_remaining_ = 0U;
    vif_direct_lane_ = 0U;
    vif_dma_active_ = false;
    vif_dma_desynchronized_ = false;
    next_image_transfer_generation_ = 1U;
    gs_bitbltbuf_ = 0U;
    gs_trxpos_ = 0U;
    gs_trxreg_ = 0U;
    gs_trxdir_ = 0U;
    next_sequence_ = 1U;
    register_writes_.clear();
    image_transfers_.clear();
    completed_image_transfers_.clear();
    queued_image_bytes_ = 0U;
    diagnostics_.clear();
    activity_ = {};
}

void GsCapture::notify_gif_packet_eop(const GifPath path,
                                      const bool packet_observed_from_start) noexcept {
    const auto path_index = static_cast<std::size_t>(path);
    const bool primary_command_stream_available =
        packet_observed_from_start && gif_packet_start_synchronized_[path_index] &&
        gif_packet_observation_open_[path_index] &&
        gif_packet_primary_command_continuous_[path_index] && !raw_passthrough_only_ &&
        command_observer_ != nullptr;
    GsRawGifObserver* const raw_eop_observer = raw_gif_packet_eop_observer_;
    GsRawGifObserver* const terminal_eop_observer = terminal_raw_gif_packet_eop_observer_;
    GsRawGifObserver* const diagnostic_eop_observer = diagnostic_raw_gif_packet_eop_observer_;
    gif_packet_observation_open_[path_index] = false;
    gif_packet_primary_command_continuous_[path_index] = false;
    gif_packet_start_synchronized_[path_index] = true;
    reset_gif_payload(gif_payloads_[path_index]);
    if (raw_eop_observer != nullptr) {
        raw_eop_observer->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
    if (terminal_eop_observer != nullptr && terminal_eop_observer != raw_eop_observer) {
        terminal_eop_observer->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
    if (diagnostic_eop_observer != nullptr && diagnostic_eop_observer != raw_eop_observer &&
        diagnostic_eop_observer != terminal_eop_observer) {
        diagnostic_eop_observer->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
}

void GsCapture::synchronize_gif_packet_start(const GifPath path) noexcept {
    const auto path_index = static_cast<std::size_t>(path);
    if (!gif_packet_observation_open_[path_index]) {
        gif_packet_start_synchronized_[path_index] = true;
    }
}

void GsCapture::invalidate_open_gif_packet_continuity() noexcept {
    for (std::size_t index = 0U; index < gif_packet_observation_open_.size(); ++index) {
        if (gif_packet_observation_open_[index]) {
            gif_packet_primary_command_continuous_[index] = false;
        }
    }
}

GsReplayStateRestoreStatus GsCapture::restore_idle_state(const GsReplayStateSeed& seed) noexcept {
    const auto status = validate_gs_replay_state_seed(seed);
    if (status != GsReplayStateRestoreStatus::Applied) {
        return status;
    }
    clear();
    gs_bitbltbuf_ = seed.bitbltbuf;
    gs_trxpos_ = seed.trxpos;
    gs_trxreg_ = seed.trxreg;
    gs_trxdir_ = seed.trxdir;
    return GsReplayStateRestoreStatus::Applied;
}

void GsCapture::begin_gif_tag(const GifPath path, const GsQuadword& quadword) {
    GifPayloadState& state = gif_payloads_[static_cast<std::size_t>(path)];
    const auto tag = quadword_u64<0>(quadword);
    const auto loop_count = quadword[0] & 0x7FFFU;
    const auto format = static_cast<unsigned int>(tag >> 58U) & 3U;
    auto register_count = static_cast<unsigned int>(tag >> 60U);
    if (register_count == 0U) {
        register_count = 16U;
    }

    if (loop_count != 0U && command_observer_ != nullptr) {
        command_observer_->on_gs_gif_tag_begin(path);
    }
    if (loop_count != 0U && terminal_command_observer_ != nullptr &&
        terminal_command_observer_ != command_observer_) {
        terminal_command_observer_->on_gs_gif_tag_begin(path);
    }
    if (loop_count != 0U && diagnostic_command_observer_ != nullptr &&
        diagnostic_command_observer_ != command_observer_ &&
        diagnostic_command_observer_ != terminal_command_observer_) {
        diagnostic_command_observer_->on_gs_gif_tag_begin(path);
    }

    if (loop_count != 0U && format == 0U) {
        if ((tag & (1ULL << 46U)) != 0U) {
            const auto prim = (tag >> 47U) & 0x7FFU;
            record_register_write(path, GsRegisterWriteSource::GifTagPrim, 0U, prim, 0U);
        }
    }

    state.register_descriptors = quadword_u64<2>(quadword);
    state.register_count = register_count;
    state.register_index = 0U;
    state.register_values_remaining = 0U;

    if (format == 0U) {
        const auto payload_count = static_cast<std::uint64_t>(loop_count) * register_count;
        state.payload_quadwords_remaining = static_cast<std::uint32_t>(payload_count);
        state.kind = static_cast<GifPayloadKind>(payload_count != 0U);
        return;
    }
    if (format == 1U) {
        const auto register_values = static_cast<std::uint64_t>(loop_count) * register_count;
        state.register_values_remaining = static_cast<std::uint32_t>(register_values);
        const auto payload_quadwords = reglist_quadword_count(register_values);
        state.payload_quadwords_remaining = static_cast<std::uint32_t>(payload_quadwords);
        state.kind = payload_quadwords == 0U ? GifPayloadKind::Tag : GifPayloadKind::Reglist;
        return;
    }

    state.payload_quadwords_remaining = loop_count;
    if (loop_count == 0U) {
        state.kind = GifPayloadKind::Tag;
        state.image_transfer_generation = 0U;
        return;
    }
    state.kind = GifPayloadKind::Image;
    state.image_transfer_generation =
        host_to_local_transfer_.active ? host_to_local_transfer_.generation : 0U;
    const auto supplied_bytes = static_cast<std::uint64_t>(loop_count) * 16U;
    if (!host_to_local_transfer_.active) {
        // IMAGE payload with a valid geometry but no armed transfer would be
        // dropped; report it.
        const auto expected_bytes =
            transfer_byte_count(static_cast<std::uint32_t>(gs_trxreg_) & 0xFFFU,
                                static_cast<std::uint32_t>(gs_trxreg_ >> 32U) & 0xFFFU,
                                static_cast<std::uint32_t>(gs_bitbltbuf_ >> 56U) & 0x3FU);
        if (expected_bytes.has_value()) {
            record_diagnostic(GsCaptureDiagnosticCode::InvalidImageTransfer,
                              static_cast<std::uint32_t>(supplied_bytes));
        }
    } else {
        const auto remaining_bytes =
            host_to_local_transfer_.expected_bytes - host_to_local_transfer_.image.pixels.size();
        if (supplied_bytes > remaining_bytes && supplied_bytes - remaining_bytes > 15U) {
            record_diagnostic(GsCaptureDiagnosticCode::InvalidImageTransfer,
                              static_cast<std::uint32_t>(supplied_bytes));
            interrupt_host_to_local_transfer(false);
            state.image_transfer_generation = 0U;
        }
    }
}

void GsCapture::interrupt_host_to_local_transfer(const bool diagnose_incomplete) {
    if (diagnose_incomplete && host_to_local_transfer_.active &&
        !host_to_local_transfer_.image.pixels.empty()) {
        record_diagnostic(GsCaptureDiagnosticCode::InvalidImageTransfer,
                          static_cast<std::uint32_t>(host_to_local_transfer_.image.pixels.size()));
    }
    host_to_local_transfer_ = {};
}

void GsCapture::consume_packed_quadword(const GifPath path, const GsQuadword& quadword) {
    GifPayloadState& state = gif_payloads_[static_cast<std::size_t>(path)];
    const auto descriptor =
        static_cast<std::uint32_t>(state.register_descriptors >> (state.register_index * 4U)) &
        0xFU;
    if (descriptor == 14U) {
        const auto address = static_cast<std::uint8_t>(quadword[2] & 0x7FU);
        const auto value = quadword_u64<0>(quadword);
        record_register_write(path, GsRegisterWriteSource::Ad, address, value);
        switch (address) {
        case 0x50U:
            gs_bitbltbuf_ = value;
            break;
        case 0x51U:
            gs_trxpos_ = value;
            break;
        case 0x52U:
            gs_trxreg_ = value;
            break;
        case 0x53U:
            interrupt_host_to_local_transfer(true);
            gs_trxdir_ = value;
            begin_host_to_local_transfer();
            break;
        default:
            break;
        }
    } else if (descriptor != 15U) {
        record_register_write(path, GsRegisterWriteSource::Packed,
                              static_cast<std::uint8_t>(descriptor),
                              quadword_u64<0>(quadword),
                              quadword_u64<2>(quadword));
    }

    ++state.register_index;
    if (state.register_index == state.register_count) {
        state.register_index = 0U;
    }
    --state.payload_quadwords_remaining;
    if (state.payload_quadwords_remaining == 0U) {
        reset_gif_payload(state);
    }
}

void GsCapture::apply_ad_write(const GifPath path, const GsQuadword& quadword) {
    const auto address = static_cast<std::uint8_t>(quadword[2] & 0x7FU);
    const auto value = quadword_u64<0>(quadword);
    apply_direct_register_write(path, GsRegisterWriteSource::Ad, address, value);
    switch (address) {
    case 0x50U:
        gs_bitbltbuf_ = value;
        break;
    case 0x51U:
        gs_trxpos_ = value;
        break;
    case 0x52U:
        gs_trxreg_ = value;
        break;
    case 0x53U:
        interrupt_host_to_local_transfer(true);
        gs_trxdir_ = value;
        begin_host_to_local_transfer();
        break;
    default:
        break;
    }
}

void GsCapture::consume_reglist_quadword(const GifPath path, const GsQuadword& quadword) {
    GifPayloadState& state = gif_payloads_[static_cast<std::size_t>(path)];
    const std::array<std::uint64_t, 2> values{quadword_u64<0>(quadword),
                                              quadword_u64<2>(quadword)};
    for (const auto value : values) {
        if (state.register_values_remaining == 0U) {
            break;
        }
        const auto descriptor =
            static_cast<std::uint32_t>(state.register_descriptors >> (state.register_index * 4U)) &
            0xFU;
        if (descriptor == 14U) {
            record_diagnostic(GsCaptureDiagnosticCode::UnsupportedGifFormat, 14U);
        } else if (descriptor != 15U) {
            record_register_write(path, GsRegisterWriteSource::Reglist,
                                  static_cast<std::uint8_t>(descriptor), value);
        }
        ++state.register_index;
        if (state.register_index == state.register_count) {
            state.register_index = 0U;
        }
        --state.register_values_remaining;
    }
    --state.payload_quadwords_remaining;
    if (state.payload_quadwords_remaining == 0U) {
        reset_gif_payload(state);
    }
}

void GsCapture::consume_image_quadword(const GifPath path, const GsQuadword& quadword) {
    GifPayloadState& state = gif_payloads_[static_cast<std::size_t>(path)];
    if (host_to_local_transfer_.active &&
        state.image_transfer_generation == host_to_local_transfer_.generation) {
        for (const auto word : quadword) {
            for (unsigned int byte = 0U; byte < 4U; ++byte) {
                if (host_to_local_transfer_.image.pixels.size() <
                    host_to_local_transfer_.expected_bytes) {
                    host_to_local_transfer_.image.pixels.push_back(
                        static_cast<std::uint8_t>(word >> (byte * 8U)));
                }
            }
        }
        if (host_to_local_transfer_.image.pixels.size() == host_to_local_transfer_.expected_bytes) {
            if (state.payload_quadwords_remaining == 1U) {
                finish_image_transfer(path);
            } else {
                record_diagnostic(GsCaptureDiagnosticCode::InvalidImageTransfer,
                                  state.payload_quadwords_remaining * 16U);
                host_to_local_transfer_ = {};
            }
        }
    }
    --state.payload_quadwords_remaining;
    if (state.payload_quadwords_remaining == 0U) {
        reset_gif_payload(state);
    }
}

void GsCapture::record_register_write(const GifPath path, const GsRegisterWriteSource source,
                                      const std::uint8_t address, const std::uint64_t value,
                                      const std::uint64_t packed_high) {
    // 0x7F is the A+D NOP register; it is never a GS write.
    if (address == 0x7FU) {
        return;
    }
    switch (source) {
    case GsRegisterWriteSource::GifTagPrim:
        ++activity_.gif_tag_prim_writes;
        break;
    case GsRegisterWriteSource::Packed:
        ++activity_.packed_register_writes;
        break;
    case GsRegisterWriteSource::Reglist:
        ++activity_.reglist_register_writes;
        break;
    case GsRegisterWriteSource::Ad:
        ++activity_.ad_register_writes;
        break;
    }
    const GsRegisterWrite write{
        .sequence = next_sequence_++,
        .path = path,
        .source = source,
        .address = address,
        .value = value,
        .packed_high = packed_high,
    };
    if (command_observer_ != nullptr) {
        command_observer_->on_gs_register_write(write);
    }
    if (terminal_command_observer_ != nullptr &&
        terminal_command_observer_ != command_observer_) {
        terminal_command_observer_->on_gs_register_write(write);
    }
    if (diagnostic_command_observer_ != nullptr &&
        diagnostic_command_observer_ != command_observer_ &&
        diagnostic_command_observer_ != terminal_command_observer_) {
        diagnostic_command_observer_->on_gs_register_write(write);
    }
    if (register_writes_.size() * sizeof(GsRegisterWrite) > 131040U) {
        ++activity_.omitted_register_writes;
        return;
    }
    register_writes_.push_back(write);
}

void GsCapture::apply_direct_register_write(const GifPath path, const GsRegisterWriteSource source,
                                            const std::uint8_t address, const std::uint64_t value,
                                            const std::uint64_t packed_high) {
    record_register_write(path, source, address, value, packed_high);
}

void GsCapture::begin_host_to_local_transfer() {
    if ((gs_trxdir_ & 3U) != 0U) {
        return;
    }
    HostToLocalTransferState& transfer = host_to_local_transfer_;
    transfer = {};
    transfer.image.destination_base_pointer =
        static_cast<std::uint32_t>(gs_bitbltbuf_ >> 32U) & 0x3FFFU;
    transfer.image.destination_buffer_width =
        static_cast<std::uint32_t>(gs_bitbltbuf_ >> 48U) & 0x3FU;
    transfer.image.destination_psm = static_cast<std::uint32_t>(gs_bitbltbuf_ >> 56U) & 0x3FU;
    transfer.image.destination_x = static_cast<std::uint32_t>(gs_trxpos_ >> 32U) & 0x7FFU;
    transfer.image.destination_y = static_cast<std::uint32_t>(gs_trxpos_ >> 48U) & 0x7FFU;
    transfer.image.width = static_cast<std::uint32_t>(gs_trxreg_) & 0xFFFU;
    transfer.image.height = static_cast<std::uint32_t>(gs_trxreg_ >> 32U) & 0xFFFU;
    const auto expected_bytes = transfer_byte_count(transfer.image.width, transfer.image.height,
                                                    transfer.image.destination_psm);
    if (!expected_bytes.has_value()) {
        record_diagnostic(GsCaptureDiagnosticCode::UnsupportedPixelStorageMode,
                          transfer.image.destination_psm);
        return;
    }
    transfer.expected_bytes = *expected_bytes;
    transfer.generation = next_image_transfer_generation_++;
    transfer.active = true;
    transfer.image.pixels.reserve(*expected_bytes);
}

void GsCapture::finish_image_transfer(const GifPath path) {
    if (host_to_local_transfer_.active &&
        host_to_local_transfer_.image.pixels.size() == host_to_local_transfer_.expected_bytes) {
        GsImageTransfer& pending_image = host_to_local_transfer_.image;
        pending_image.sequence = next_sequence_++;
        pending_image.path = path;

        // With history off and a single decoded-command consumer, the pixel
        // vector can be handed over instead of copied.
        const bool move_to_command =
            !image_history_enabled_ && command_observer_ != nullptr &&
            (diagnostic_command_observer_ == nullptr ||
             command_observer_ == diagnostic_command_observer_) &&
            (terminal_command_observer_ == nullptr || command_observer_ == terminal_command_observer_);
        const bool move_to_terminal =
            !image_history_enabled_ && command_observer_ == nullptr &&
            terminal_command_observer_ != nullptr &&
            (diagnostic_command_observer_ == nullptr ||
             terminal_command_observer_ == diagnostic_command_observer_);

        if (move_to_command) {
            command_observer_->on_gs_image_transfer(std::move(pending_image));
        } else if (command_observer_ != nullptr) {
            command_observer_->on_gs_image_transfer(pending_image);
        }
        if (terminal_command_observer_ != nullptr &&
            terminal_command_observer_ != command_observer_) {
            if (move_to_terminal) {
                terminal_command_observer_->on_gs_image_transfer(std::move(pending_image));
            } else {
                terminal_command_observer_->on_gs_image_transfer(pending_image);
            }
        }
        if (diagnostic_command_observer_ != nullptr &&
            diagnostic_command_observer_ != command_observer_ &&
            diagnostic_command_observer_ != terminal_command_observer_) {
            diagnostic_command_observer_->on_gs_image_transfer(pending_image);
        }

        if (image_history_enabled_ && image_transfers_.size() >= kMaxImageHistory) {
            record_diagnostic(GsCaptureDiagnosticCode::CaptureLimit,
                              static_cast<std::uint32_t>(image_transfers_.size()));
        }
        if (image_history_enabled_ && image_transfers_.size() < kMaxImageHistory) {
            const auto duplicate = std::find_if(
                image_transfers_.begin(), image_transfers_.end(),
                [&pending_image](const GsImageTransfer& existing) {
                    return existing.path == pending_image.path &&
                           existing.destination_base_pointer == pending_image.destination_base_pointer &&
                           existing.destination_buffer_width == pending_image.destination_buffer_width &&
                           existing.destination_psm == pending_image.destination_psm &&
                           existing.destination_x == pending_image.destination_x &&
                           existing.destination_y == pending_image.destination_y &&
                           existing.width == pending_image.width &&
                           existing.height == pending_image.height &&
                           existing.pixels == pending_image.pixels;
                });
            if (duplicate == image_transfers_.end()) {
                image_transfers_.push_back(pending_image);
            }
        }
        if (image_history_enabled_) {
            queue_completed_image_transfer(std::move(pending_image));
        }
    }
    host_to_local_transfer_ = {};
}

void GsCapture::queue_completed_image_transfer(GsImageTransfer&& image) {
    const std::size_t image_bytes = image.pixels.size();
    if (kMaxQueuedImageBytes - queued_image_bytes_ < image_bytes) {
        ++activity_.omitted_queued_image_transfers;
        return;
    }
    queued_image_bytes_ += image_bytes;
    completed_image_transfers_.push_back(std::move(image));
    ++activity_.queued_image_transfers;
}
void GsCapture::reset_gif_payload(GifPayloadState& state) noexcept {
    state = {};
}

GsCapture::GifPayloadState& GsCapture::gif_payload_state(const GifPath path) noexcept {
    return gif_payloads_[static_cast<std::size_t>(path)];
}

std::vector<std::uint8_t> decode_gs_image_rgba(const GsImageTransfer& image) {
    const auto expected_bytes =
        transfer_byte_count(image.width, image.height, image.destination_psm);
    if (!expected_bytes.has_value() || image.pixels.size() != *expected_bytes) {
        throw std::invalid_argument("GS image transfer is not a complete supported image");
    }
    if (image.destination_psm == 19U || image.destination_psm == 20U) {
        throw std::invalid_argument(
            "indexed GS image transfer requires texture coordinates and a CLUT");
    }
    if (image.destination_psm == 0U) {
        return image.pixels;
    }
    if (image.destination_psm == 1U) {
        std::vector<std::uint8_t> output{};
        output.reserve(image.pixels.size() / 3U * 4U);
        for (std::size_t offset = 0U; offset < image.pixels.size(); offset += 3U) {
            output.push_back(image.pixels[offset]);
            output.push_back(image.pixels[offset + 1U]);
            output.push_back(image.pixels[offset + 2U]);
            output.push_back(255U);
        }
        return output;
    }

    std::vector<std::uint8_t> output{};
    output.reserve(*expected_bytes * 2U);
    for (std::size_t offset = 0U; offset < image.pixels.size(); offset += 2U) {
        const int pixel = image.pixels[offset] |
                          static_cast<std::uint16_t>(image.pixels[offset + 1U] << 8U);
        output.push_back(expand_rgb5(static_cast<std::uint16_t>(pixel & 31)));
        output.push_back(expand_rgb5(static_cast<std::uint16_t>((pixel >> 5) & 31)));
        output.push_back(expand_rgb5(static_cast<std::uint16_t>((pixel >> 10) & 31)));
        output.push_back((pixel & 0x8000) != 0 ? 255U : 0U);
    }
    return output;
}

std::vector<std::uint8_t> encode_gs_image_ppm(const GsImageTransfer& image) {
    if (image.destination_psm == 19U || image.destination_psm == 20U) {
        // Indexed uploads have no palette here, so export the raw indices as a
        // P5 gray map (PSMT4 nibbles are widened to 0..255).
        const auto expected_bytes =
            transfer_byte_count(image.width, image.height, image.destination_psm);
        if (!expected_bytes.has_value() || image.pixels.size() != *expected_bytes) {
            throw std::invalid_argument("GS image transfer is not a complete indexed image");
        }
        const auto header = "P5\n" + std::to_string(image.width) + " " +
                                   std::to_string(image.height) + "\n255\n";
        std::vector<std::uint8_t> output{};
        output.reserve(header.size() + static_cast<std::size_t>(image.width) * image.height);
        output.insert(output.end(), header.begin(), header.end());
        if (image.destination_psm == 19U) {
            output.insert(output.end(), image.pixels.begin(), image.pixels.end());
        } else {
            const auto pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
            std::uint64_t emitted = 0U;
            for (const auto packed : image.pixels) {
                if (emitted < pixel_count) {
                    output.push_back(static_cast<std::uint8_t>((packed & 15U) * 17U));
                    ++emitted;
                }
                if (emitted < pixel_count) {
                    output.push_back(static_cast<std::uint8_t>((packed >> 4U) * 17U));
                    ++emitted;
                }
            }
        }
        return output;
    }

    const auto rgba = decode_gs_image_rgba(image);
    const auto header = "P6\n" + std::to_string(image.width) + " " +
                               std::to_string(image.height) + "\n255\n";
    const auto rgb_bytes = rgba.size() / 4U * 3U;
    std::vector<std::uint8_t> output{};
    output.reserve(header.size() + rgb_bytes);
    output.insert(output.end(), header.begin(), header.end());
    for (std::size_t offset = 0U; offset < rgba.size(); offset += 4U) {
        output.push_back(rgba[offset]);
        output.push_back(rgba[offset + 1U]);
        output.push_back(rgba[offset + 2U]);
    }
    return output;
}

void write_gs_image_ppm(const GsImageTransfer& image, const std::filesystem::path& output_path) {
    const auto encoded = encode_gs_image_ppm(image);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create GS image capture: " + output_path.string());
    }
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) {
        throw std::runtime_error("unable to write GS image capture: " + output_path.string());
    }
}
} // namespace takedown::guest
