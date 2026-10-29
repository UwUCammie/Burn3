#include "takedown/runtime/slus_21050_movie_player.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::runtime {
namespace {

// Identities are never zero; the counter stops handing them out if it ever wraps.
std::uint64_t allocate_movie_player_identity() noexcept {
    static std::atomic<std::uint64_t> next_identity{1U};
    auto candidate = next_identity.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next_identity.compare_exchange_weak(candidate, candidate + 1U, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0U;
}

// One of the retail movie player's staging slots in guest memory.
struct MovieWorkSlot {
    std::uint32_t buffer{};
    std::uint32_t texture_columns{};
    std::uint32_t texture_rows{};
    std::uint32_t macroblock_columns{};
    std::uint32_t macroblock_rows{};
    std::uint32_t gs_base{};
};

MovieWorkSlot read_movie_work_slot(const guest::EeMemory& memory, const std::size_t index) {
    const auto base = static_cast<guest::GuestAddress>(
        kSlus21050MovieWork + static_cast<guest::GuestAddress>(index) * 24U);
    MovieWorkSlot slot{};
    slot.buffer = memory.read_u32_le(base);
    slot.texture_columns = memory.read_u32_le(base + 4U);
    slot.texture_rows = memory.read_u32_le(base + 8U);
    slot.macroblock_columns = memory.read_u32_le(base + 12U);
    slot.macroblock_rows = memory.read_u32_le(base + 16U);
    slot.gs_base = memory.read_u32_le(base + 20U);
    return slot;
}

struct MovieFrameRate {
    std::uint32_t numerator{};
    std::uint32_t denominator{};
};

// The binding of a drop batch that was authorised to precede the due frame.
struct AuthorizedDropBatchBinding {
    std::uint64_t player_identity{};
    std::uint64_t session_generation{};
    std::uint64_t journal_revision{};
    std::size_t count{};
    std::array<Slus21050MovieDroppedFrameIdentity, kSlus21050MovieDroppedFrameJournalCapacity>
        frames{};
};

// The identity of the picture which was taken out of the player for staging.
struct CheckedOutPreparedFrame {
    Slus21050MovieFrameLease lease{};
    std::optional<std::uint64_t> timeline_frame{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    Slus21050MovieGeometry geometry{};

    explicit CheckedOutPreparedFrame(const Slus21050MoviePreparedFrame& frame)
        : lease(frame.lease()), timeline_frame(frame.timeline_frame()),
          source_sequence(frame.source_sequence()), source_checksum(frame.source_checksum()),
          payload_digest(frame.payload_digest()), geometry(frame.geometry()) {}
};

bool same_geometry(const Slus21050MovieGeometry& left, const Slus21050MovieGeometry& right) noexcept {
    return left.width == right.width && left.height == right.height &&
           left.texture_columns == right.texture_columns && left.texture_rows == right.texture_rows &&
           left.macroblock_columns == right.macroblock_columns &&
           left.macroblock_rows == right.macroblock_rows && left.column_bytes == right.column_bytes &&
           left.buffer_bytes == right.buffer_bytes &&
           left.final_texture_padding_bytes == right.final_texture_padding_bytes;
}

// A session in one of these states has nothing left to decode.
constexpr bool inactive_status(const Slus21050MoviePlayerStatus status) noexcept {
    return status == Slus21050MoviePlayerStatus::Empty ||
           status == Slus21050MoviePlayerStatus::EndOfStream ||
           status == Slus21050MoviePlayerStatus::MovieOpenFailed ||
           status == Slus21050MoviePlayerStatus::DecoderUnavailable ||
           status == Slus21050MoviePlayerStatus::MovieReadFailed ||
           status == Slus21050MoviePlayerStatus::SourceFrameRateMismatch;
}

inline bool has_payload_and_lease(const Slus21050MoviePreparedFrame& frame) noexcept {
    return frame.lease().session_generation != 0U && frame.lease().lease_generation != 0U &&
           !frame.bytes().empty() && frame.geometry().buffer_bytes == frame.bytes().size();
}

bool matches_checked_out(const CheckedOutPreparedFrame& checked,
                         const Slus21050MoviePreparedFrame& frame) noexcept {
    return checked.lease == frame.lease() && checked.timeline_frame == frame.timeline_frame() &&
           checked.source_sequence == frame.source_sequence() &&
           checked.source_checksum == frame.source_checksum() &&
           checked.payload_digest == frame.payload_digest() &&
           same_geometry(checked.geometry, frame.geometry());
}

// Only the MPEG-2 frame rates the retail player expects are accepted.
std::optional<MovieFrameRate> reduced_frame_rate(const std::uint32_t numerator,
                                                 const std::uint32_t denominator) {
    if (numerator == 0U || denominator == 0U) {
        return std::nullopt;
    }
    const auto divisor = std::gcd(numerator, denominator);
    const MovieFrameRate rate{numerator / divisor, denominator / divisor};
    constexpr std::array<MovieFrameRate, 8> kMpeg2Rates{{{24000U, 1001U},
                                                         {24U, 1U},
                                                         {25U, 1U},
                                                         {30000U, 1001U},
                                                         {30U, 1U},
                                                         {50U, 1U},
                                                         {60000U, 1001U},
                                                         {60U, 1U}}};
    const auto match = std::find_if(kMpeg2Rates.begin(), kMpeg2Rates.end(), [&](const MovieFrameRate candidate) {
        return candidate.numerator == rate.numerator && candidate.denominator == rate.denominator;
    });
    if (match == kMpeg2Rates.end()) {
        return std::nullopt;
    }
    return rate;
}

// The three planes must hold exactly one 4:2:0 picture.
bool planes_match_frame(const guest::IpuDecodedFrame& frame) {
    const auto luma_bytes = static_cast<std::size_t>(frame.width) * frame.height;
    const auto chroma_width = static_cast<std::size_t>(frame.width / 2U);
    const auto chroma_height = static_cast<std::size_t>(frame.height / 2U);
    const auto chroma_bytes = chroma_width * chroma_height;
    return frame.y_plane.size() == luma_bytes && frame.cb_plane.size() == chroma_bytes &&
           frame.cr_plane.size() == chroma_bytes;
}

// The retail player only handles 640-wide pictures in three heights.
std::optional<Slus21050MovieGeometry> geometry_for_frame(const guest::IpuDecodedFrame& frame) {
    if (frame.width != 640U || (frame.height != 352U && frame.height != 480U && frame.height != 512U)) {
        return std::nullopt;
    }
    const auto texture_rows = (frame.height + 63U) / 64U;
    const auto padded_height = texture_rows * 64U;
    return Slus21050MovieGeometry{
        .width = 640U,
        .height = frame.height,
        .texture_columns = 10U,
        .texture_rows = texture_rows,
        .macroblock_columns = 40U,
        .macroblock_rows = frame.height / 16U,
        .column_bytes = static_cast<std::size_t>(frame.height) * 64U,
        .buffer_bytes = static_cast<std::size_t>(frame.height) * 64U * 40U,
        .final_texture_padding_bytes = static_cast<std::size_t>(padded_height - frame.height) * 64U};
}

std::uint8_t clamp_channel(const int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// YCbCr 4:2:0 to the GS texture strip layout: 16-pixel columns, 64 bytes per row.
std::vector<std::uint8_t> convert_to_strip_major_rgba(const guest::IpuDecodedFrame& frame,
                                                     const Slus21050MovieGeometry& geometry) {
    std::vector<std::uint8_t> rgba(geometry.buffer_bytes, std::uint8_t{0});
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    const auto chroma_width = width / 2U;
    for (std::size_t column = 0; column < geometry.macroblock_columns; ++column) {
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t within = 0; within < 16U; ++within) {
                const auto x = column * 16U + within;
                const int luma = frame.y_plane[y * width + x];
                const auto chroma_index = (y / 2U) * chroma_width + x / 2U;
                const int cb = frame.cb_plane[chroma_index];
                const int cr = frame.cr_plane[chroma_index];
                const int l = (std::max(luma - 16, 0) * 149) >> 6;
                const int d = cb - 128;
                const int e = cr - 128;
                const int red = (l + ((e * 204) >> 6) + 1) >> 1;
                const int green = (l + ((e * -104) >> 6) + ((d * -50) >> 6) + 1) >> 1;
                const int blue = (l + ((d * 258) >> 6) + 1) >> 1;
                const auto destination =
                    column * geometry.column_bytes + y * 64U + within * 4U;
                rgba[destination] = clamp_channel(red);
                rgba[destination + 1U] = clamp_channel(green);
                rgba[destination + 2U] = clamp_channel(blue);
                rgba[destination + 3U] = 128U;
            }
        }
    }
    return rgba;
}

struct DecodeAttempt {
    std::optional<guest::IpuDecodedFrame> frame;
    Slus21050MoviePlayerStatus status;
};

bool multiply_overflows(const std::uint64_t left, const std::uint64_t right) noexcept {
    std::uint64_t product{};
    return __builtin_mul_overflow(left, right, &product);
}

// Nanoseconds spanned by a whole number of frames, rounded up and saturating.
std::uint64_t nanoseconds_for_frames(const std::uint64_t frames, const MovieFrameRate rate) noexcept {
    const auto duration_numerator = static_cast<std::uint64_t>(rate.denominator) * 1000000000ULL;
    const auto divisor = std::gcd(duration_numerator, static_cast<std::uint64_t>(rate.numerator));
    const auto reduced_numerator = duration_numerator / divisor;
    const auto reduced_denominator = static_cast<std::uint64_t>(rate.numerator) / divisor;
    const auto whole = frames / reduced_denominator;
    if (multiply_overflows(reduced_numerator, whole)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto remainder = frames % reduced_denominator;
    const auto whole_nanoseconds = reduced_numerator * whole;
    const auto remainder_product = reduced_numerator * remainder;
    const auto remainder_nanoseconds =
        remainder_product / reduced_denominator + static_cast<std::uint64_t>(remainder_product % reduced_denominator != 0U);
    if (whole_nanoseconds > std::numeric_limits<std::uint64_t>::max() - remainder_nanoseconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_nanoseconds + remainder_nanoseconds;
}

// The timeline frame that the elapsed time has reached at the given frame rate.
std::uint64_t timeline_frame_at(const std::uint64_t elapsed_nanoseconds, const MovieFrameRate rate) noexcept {
    const auto time_denominator = static_cast<std::uint64_t>(rate.denominator) * 1000000000ULL;
    const auto divisor = std::gcd(static_cast<std::uint64_t>(rate.numerator), time_denominator);
    const auto multiplier = static_cast<std::uint64_t>(rate.numerator) / divisor;
    const auto reduced_denominator = time_denominator / divisor;
    const auto whole = elapsed_nanoseconds / reduced_denominator;
    if (multiply_overflows(multiplier, whole)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto remainder = elapsed_nanoseconds % reduced_denominator;
    return multiplier * whole + multiplier * remainder / reduced_denominator;
}

} // namespace

Slus21050MoviePreparedFrame::Slus21050MoviePreparedFrame(
    const Slus21050MovieFrameLease lease, const std::optional<std::uint64_t> timeline_frame,
    const std::uint64_t source_sequence, const std::uint32_t source_checksum,
    Slus21050MovieGeometry geometry,
    std::vector<std::uint8_t>&& strip_major_rgba) noexcept
    : lease_(lease), timeline_frame_(timeline_frame), source_sequence_(source_sequence),
      source_checksum_(source_checksum), geometry_(geometry),
      strip_major_rgba_(std::move(strip_major_rgba)) {
    // FNV-1a 64 binds a completion receipt to this exact converted payload.
    std::uint64_t digest = 14695981039346656037ULL;
    for (const auto byte : std::span<const std::uint8_t>{strip_major_rgba_}) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    payload_digest_ = digest;
}

struct Slus21050MoviePlayer::Impl {
    std::uint64_t player_identity{allocate_movie_player_identity()};
    std::filesystem::path movie_path{};
    DecoderFactory decoder_factory{};
    std::ifstream stream{};
    std::unique_ptr<guest::IpuMpeg2Decoder> decoder{};
    bool loop{};
    bool input_eof{};
    bool decoder_flushed{};
    std::uint64_t pass_input_bytes{};
    std::uint64_t pass_decoded_frames{};
    Slus21050MoviePlayerStatus status{Slus21050MoviePlayerStatus::Empty};
    std::optional<Slus21050MovieGeometry> geometry{};
    std::optional<Slus21050MoviePreparedFrame> pending_frame{};
    bool prepared_frame_outstanding{};
    std::optional<CheckedOutPreparedFrame> checked_out_frame{};
    std::optional<AuthorizedDropBatchBinding> authorized_drop_batch{};
    std::uint64_t session_generation{};
    std::uint64_t next_lease_generation{};
    std::uint64_t live_admission_session_generation{};
    std::uint64_t live_admission_lease_generation{};
    std::optional<MovieFrameRate> source_frame_rate{};
    std::uint64_t timeline_frames_consumed{};
    std::uint64_t pass_first_timeline_frame{};
    std::optional<std::uint64_t> last_timeline_target{};
    std::array<Slus21050MovieDroppedFrameIdentity, kSlus21050MovieDroppedFrameJournalCapacity>
        dropped_frames{};
    std::size_t dropped_frame_count{};
    bool dropped_frame_tracking{};
    std::uint64_t dropped_frame_journal_revision{1U};
    Slus21050MoviePlayerStatistics statistics{};

    Slus21050MoviePlayerStatus set_status(const Slus21050MoviePlayerStatus next) noexcept {
        status = next;
        statistics.last_status = next;
        return next;
    }

    // A new session invalidates every lease issued under the previous one.
    void retire_session() {
        session_generation = session_generation == std::numeric_limits<std::uint64_t>::max()
                                 ? 1U
                                 : session_generation + 1U;
        next_lease_generation = 0U;
        stream.close();
    }

    // Journal tickets are tied to a revision; zero means the counter ran out.
    void advance_journal_revision() noexcept {
        if (dropped_frame_journal_revision == 0U) {
            return;
        }
        if (dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max()) {
            dropped_frame_journal_revision = 0U;
            return;
        }
        ++dropped_frame_journal_revision;
    }

    [[nodiscard]] std::uint64_t following_lease_generation() const noexcept {
        return next_lease_generation == std::numeric_limits<std::uint64_t>::max()
                   ? 1U
                   : next_lease_generation + 1U;
    }

    void authorize_drop_batch(const Slus21050MovieDroppedFrameBatch& batch) {
        AuthorizedDropBatchBinding binding{};
        binding.player_identity = batch.player_identity_;
        binding.session_generation = batch.session_generation_;
        binding.journal_revision = batch.journal_revision_;
        binding.count = batch.count_;
        std::copy(batch.frames_.begin(), batch.frames_.begin() + static_cast<std::ptrdiff_t>(batch.count_),
                  binding.frames.begin());
        authorized_drop_batch = binding;
    }

    void account_staged_frame(const Slus21050MovieFrameCompletionReceipt& receipt) noexcept {
        const auto& frame = *checked_out_frame;
        ++statistics.frames_staged;
        statistics.staged_buffers += 2U;
        statistics.staged_bytes = statistics.staged_bytes + frame.geometry.buffer_bytes * 2U;
        statistics.staged_gs_base = receipt.staged_gs_base;
        if (frame.timeline_frame.has_value()) {
            statistics.last_staged_timeline_frame = *frame.timeline_frame;
        }
        prepared_frame_outstanding = false;
        checked_out_frame.reset();
        set_status(Slus21050MoviePlayerStatus::FrameStaged);
    }

    // Opens the movie and brings up a decoder for it.
    Slus21050MoviePlayerStatus start_decoding(const bool restarting) {
        ++statistics.open_attempts;
        stream.close();
        stream.clear();
        stream.open(movie_path, std::ios::binary);
        if (stream.fail()) {
            decoder.reset();
            return set_status(Slus21050MoviePlayerStatus::MovieOpenFailed);
        }
        decoder = decoder_factory ? decoder_factory() : nullptr;
        if (decoder == nullptr || decoder->backend_name() == std::string_view{"null"}) {
            decoder.reset();
            stream.close();
            return set_status(Slus21050MoviePlayerStatus::DecoderUnavailable);
        }
        input_eof = false;
        decoder_flushed = false;
        pass_input_bytes = 0U;
        pass_decoded_frames = 0U;
        if (restarting) {
            ++statistics.loop_restarts;
            set_status(Slus21050MoviePlayerStatus::LoopRestarted);
            return Slus21050MoviePlayerStatus::LoopRestarted;
        }
        set_status(Slus21050MoviePlayerStatus::Decoding);
        return Slus21050MoviePlayerStatus::Decoding;
    }

    // Remembers the stream's frame rate and flags a change of rate mid-stream.
    std::optional<MovieFrameRate> observe_source_frame_rate(const guest::IpuDecodedFrame& frame) {
        const auto rate = reduced_frame_rate(frame.frame_rate_numerator, frame.frame_rate_denominator);
        if (!rate.has_value()) {
            if (frame.frame_rate_numerator == 0U || frame.frame_rate_denominator == 0U) {
                ++statistics.unknown_timing_frames;
                return source_frame_rate;
            }
            ++statistics.source_rate_changes;
            set_status(Slus21050MoviePlayerStatus::SourceFrameRateMismatch);
            return std::nullopt;
        }
        if (!source_frame_rate.has_value()) {
            source_frame_rate = rate;
            statistics.source_frame_rate_numerator = rate->numerator;
            statistics.source_frame_rate_denominator = rate->denominator;
        } else if (source_frame_rate->numerator != rate->numerator ||
                   source_frame_rate->denominator != rate->denominator) {
            ++statistics.source_rate_changes;
            set_status(Slus21050MoviePlayerStatus::SourceFrameRateMismatch);
            return std::nullopt;
        }
        return source_frame_rate;
    }

    // A frame that is already late is journaled instead of shown; false means the journal cannot take it.
    bool record_dropped_frame(const guest::IpuDecodedFrame& frame,
                              const std::optional<std::uint64_t> timeline_frame,
                              const std::optional<Slus21050FemainFrameGuestBoundary> dropped_at) {
        if (!dropped_frame_tracking) {
            return true;
        }
        if (dropped_frame_journal_revision == 0U ||
            dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (!timeline_frame.has_value() || dropped_frame_count >= dropped_frames.size()) {
            return false;
        }
        dropped_frames[dropped_frame_count++] = Slus21050MovieDroppedFrameIdentity{
            session_generation, *timeline_frame, frame.sequence, frame.checksum, dropped_at};
        advance_journal_revision();
        return true;
    }

    DecodeAttempt decode_one();
    Slus21050MoviePlayerStatus
    prepare_frame(guest::IpuDecodedFrame frame, const std::optional<std::uint64_t>& timeline_frame,
                  bool drop_as_overdue, bool timing_already_observed,
                  std::optional<Slus21050FemainFrameGuestBoundary> dropped_at);
};
DecodeAttempt Slus21050MoviePlayer::Impl::decode_one() {
    if (inactive_status(status) || decoder == nullptr) {
        return {.status = status};
    }
    if (auto frame = decoder->receive_frame(); frame.has_value()) {
        ++pass_decoded_frames;
        return {std::move(frame), status};
    }
    if (!input_eof) {
        std::array<std::uint8_t, kSlus21050MovieDecodeChunkBytes> chunk{};
        stream.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (stream.bad() || (stream.fail() && !stream.eof())) {
            decoder.reset();
            stream.close();
            return {.status = set_status(Slus21050MoviePlayerStatus::MovieReadFailed)};
        }
        if (stream.eof()) {
            input_eof = true;
        }
        if (count != 0U) {
            ++statistics.input_chunks;
            statistics.input_bytes += count;
            pass_input_bytes += count;
            decoder->push(std::span<const std::uint8_t>{chunk.data(), count});
            if (auto frame = decoder->receive_frame(); frame.has_value()) {
                ++pass_decoded_frames;
                return {std::move(frame), status};
            }
            return {.status = set_status(Slus21050MoviePlayerStatus::Decoding)};
        }
        input_eof = true;
    }
    if (!decoder_flushed) {
        decoder->flush();
        decoder_flushed = true;
        ++statistics.flushes;
        if (auto frame = decoder->receive_frame(); frame.has_value()) {
            ++pass_decoded_frames;
            return {std::move(frame), status};
        }
    }
    ++statistics.end_of_streams;
    if (loop && pass_input_bytes != 0U && pass_decoded_frames != 0U) {
        return {.status = start_decoding(true)};
    }
    if (loop && pass_input_bytes != 0U) {
        ++statistics.zero_frame_loop_terminations;
    }
    decoder.reset();
    stream.close();
    return {.status = set_status(Slus21050MoviePlayerStatus::EndOfStream)};
}

inline Slus21050MoviePlayerStatus Slus21050MoviePlayer::Impl::prepare_frame(
    guest::IpuDecodedFrame frame, const std::optional<std::uint64_t>& timeline_frame,
    const bool drop_as_overdue, const bool timing_already_observed,
    const std::optional<Slus21050FemainFrameGuestBoundary> dropped_at) {
    ++statistics.frames_received;
    if (!timing_already_observed) {
        (void)observe_source_frame_rate(frame);
        if (status == Slus21050MoviePlayerStatus::SourceFrameRateMismatch) {
            return status;
        }
    }
    statistics.source_sequence = frame.sequence;
    statistics.source_checksum = frame.checksum;
    if (drop_as_overdue && !record_dropped_frame(frame, timeline_frame, dropped_at)) {
        return set_status(Slus21050MoviePlayerStatus::DroppedFrameJournalFull);
    }
    if (frame.concealed) {
        ++statistics.concealed_frames;
        return set_status(Slus21050MoviePlayerStatus::ConcealedFrame);
    }
    const auto decoded_geometry = geometry_for_frame(frame);
    if (!decoded_geometry.has_value()) {
        ++statistics.unsupported_geometry_frames;
        return set_status(Slus21050MoviePlayerStatus::UnsupportedFrameGeometry);
    }
    if (!planes_match_frame(frame)) {
        ++statistics.invalid_plane_frames;
        return set_status(Slus21050MoviePlayerStatus::InvalidFramePlanes);
    }
    if (drop_as_overdue) {
        ++statistics.late_frames_dropped;
        geometry = decoded_geometry;
        return set_status(Slus21050MoviePlayerStatus::Decoding);
    }
    geometry = decoded_geometry;
    const Slus21050MovieFrameLease lease{session_generation,
                                         next_lease_generation == std::numeric_limits<std::uint64_t>::max()
                                             ? 1U
                                             : next_lease_generation + 1U};
    next_lease_generation = lease.lease_generation;
    pending_frame.emplace(lease, timeline_frame, frame.sequence, frame.checksum, *geometry,
                          convert_to_strip_major_rgba(frame, *geometry));
    ++statistics.frames_converted;
    return set_status(Slus21050MoviePlayerStatus::FrameStaged);
}



const char* to_string(const Slus21050MoviePlayerStatus status) noexcept {
    switch (status) {
    case Slus21050MoviePlayerStatus::Empty:
        return "empty";
    case Slus21050MoviePlayerStatus::Decoding:
        return "decoding";
    case Slus21050MoviePlayerStatus::FrameStaged:
        return "frame-staged";
    case Slus21050MoviePlayerStatus::PreparedFrameOutstanding:
        return "prepared-frame-outstanding";
    case Slus21050MoviePlayerStatus::DroppedFrameBatchOutstanding:
        return "dropped-frame-batch-outstanding";
    case Slus21050MoviePlayerStatus::DroppedFrameBatchInvalid:
        return "dropped-frame-batch-invalid";
    case Slus21050MoviePlayerStatus::DroppedFrameJournalFull:
        return "dropped-frame-journal-full";
    case Slus21050MoviePlayerStatus::WaitingForPresentationTime:
        return "waiting-for-presentation-time";
    case Slus21050MoviePlayerStatus::LoopRestarted:
        return "loop-restarted";
    case Slus21050MoviePlayerStatus::EndOfStream:
        return "end-of-stream";
    case Slus21050MoviePlayerStatus::MovieOpenFailed:
        return "movie-open-failed";
    case Slus21050MoviePlayerStatus::DecoderUnavailable:
        return "decoder-unavailable";
    case Slus21050MoviePlayerStatus::MovieReadFailed:
        return "movie-read-failed";
    case Slus21050MoviePlayerStatus::SourceFrameRateMismatch:
        return "source-frame-rate-mismatch";
    case Slus21050MoviePlayerStatus::ConcealedFrame:
        return "concealed-frame";
    case Slus21050MoviePlayerStatus::UnsupportedFrameGeometry:
        return "unsupported-frame-geometry";
    case Slus21050MoviePlayerStatus::InvalidFramePlanes:
        return "invalid-frame-planes";
    case Slus21050MoviePlayerStatus::GuestLayoutMismatch:
        return "guest-layout-mismatch";
    case Slus21050MoviePlayerStatus::GuestBufferMismatch:
        return "guest-buffer-mismatch";
    case Slus21050MoviePlayerStatus::GuestRangeInvalid:
        return "guest-range-invalid";
    }
    return "unknown";
}

Slus21050MoviePlayer::Slus21050MoviePlayer() : impl_(std::make_unique<Impl>()) {}

Slus21050MoviePlayer::~Slus21050MoviePlayer() = default;
Slus21050MoviePlayer::Slus21050MoviePlayer(Slus21050MoviePlayer&&) noexcept = default;
Slus21050MoviePlayer& Slus21050MoviePlayer::operator=(Slus21050MoviePlayer&&) noexcept = default;

Slus21050MoviePlayerStatus Slus21050MoviePlayer::open(const std::filesystem::path& movie_path,
                                                      const bool loop) {
    return open_with_decoder_factory(
        movie_path, [] { return guest::create_libavcodec_mpeg2_decoder(); }, loop);
}

Slus21050MoviePlayerStatus
Slus21050MoviePlayer::open_with_decoder_factory(const std::filesystem::path& movie_path,
                                                DecoderFactory decoder_factory, const bool loop) {
    if (impl_ == nullptr) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->retire_session();
    impl_->movie_path = movie_path;
    impl_->decoder_factory = std::move(decoder_factory);
    impl_->decoder.reset();
    impl_->loop = loop;
    impl_->input_eof = false;
    impl_->decoder_flushed = false;
    impl_->pass_input_bytes = 0U;
    impl_->pass_decoded_frames = 0U;
    impl_->status = Slus21050MoviePlayerStatus::Empty;
    impl_->geometry.reset();
    impl_->pending_frame.reset();
    impl_->prepared_frame_outstanding = false;
    impl_->checked_out_frame.reset();
    impl_->authorized_drop_batch.reset();
    impl_->source_frame_rate.reset();
    impl_->timeline_frames_consumed = 0U;
    impl_->pass_first_timeline_frame = 0U;
    impl_->last_timeline_target.reset();
    impl_->dropped_frame_tracking = false;
    impl_->dropped_frame_count = 0U;
    impl_->advance_journal_revision();
    impl_->statistics = {};
    return impl_->start_decoding(false);
}

void Slus21050MoviePlayer::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->retire_session();
    impl_->decoder.reset();
    impl_->decoder_factory = nullptr;
    impl_->movie_path.clear();
    impl_->loop = false;
    impl_->input_eof = false;
    impl_->decoder_flushed = false;
    impl_->pass_input_bytes = 0U;
    impl_->pass_decoded_frames = 0U;
    impl_->geometry.reset();
    impl_->pending_frame.reset();
    impl_->prepared_frame_outstanding = false;
    impl_->checked_out_frame.reset();
    impl_->authorized_drop_batch.reset();
    impl_->source_frame_rate.reset();
    impl_->timeline_frames_consumed = 0U;
    impl_->pass_first_timeline_frame = 0U;
    impl_->last_timeline_target.reset();
    impl_->dropped_frame_tracking = false;
    impl_->dropped_frame_count = 0U;
    impl_->advance_journal_revision();
    impl_->statistics = {};
    impl_->set_status(Slus21050MoviePlayerStatus::Empty);
}

std::optional<Slus21050MoviePreparedFrame> Slus21050MoviePlayer::take_pending_frame() noexcept {
    if (impl_ == nullptr || impl_->prepared_frame_outstanding || !impl_->pending_frame.has_value()) {
        return std::nullopt;
    }
    impl_->checked_out_frame.emplace(*impl_->pending_frame);
    std::optional<Slus21050MoviePreparedFrame> frame = std::move(impl_->pending_frame);
    impl_->pending_frame.reset();
    impl_->prepared_frame_outstanding = true;
    return frame;
}

bool Slus21050MoviePlayer::retain_pending_frame(Slus21050MoviePreparedFrame&& frame) noexcept {
    if (impl_ == nullptr || !impl_->prepared_frame_outstanding ||
        !impl_->checked_out_frame.has_value() || impl_->pending_frame.has_value()) {
        return false;
    }
    if (!has_payload_and_lease(frame)) {
        return false;
    }
    if (frame.lease() != impl_->checked_out_frame->lease ||
        frame.timeline_frame() != impl_->checked_out_frame->timeline_frame ||
        frame.source_sequence() != impl_->checked_out_frame->source_sequence ||
        frame.source_checksum() != impl_->checked_out_frame->source_checksum ||
        frame.payload_digest() != impl_->checked_out_frame->payload_digest) {
        return false;
    }
    if (!same_geometry(frame.geometry(), impl_->checked_out_frame->geometry)) {
        return false;
    }
    frame.lease_.lease_generation =
        impl_->next_lease_generation == std::numeric_limits<std::uint64_t>::max()
            ? 1U
            : impl_->next_lease_generation + 1U;
    impl_->next_lease_generation = frame.lease_.lease_generation;
    impl_->geometry = frame.geometry_;
    impl_->pending_frame.emplace(std::move(frame));
    impl_->prepared_frame_outstanding = false;
    impl_->checked_out_frame.reset();
    impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);
    return true;
}

bool Slus21050MoviePlayer::can_complete_pending_frame(
    const Slus21050MovieFrameCompletionReceipt& receipt) const noexcept {
    return impl_ != nullptr && impl_->prepared_frame_outstanding &&
           impl_->checked_out_frame.has_value() && !impl_->authorized_drop_batch.has_value() &&
           receipt.lease == impl_->checked_out_frame->lease &&
           receipt.payload_digest == impl_->checked_out_frame->payload_digest &&
           receipt.lease.session_generation == impl_->session_generation &&
           receipt.staged_gs_base != 0U && receipt.staged_gs_base <= 16383U;
}

bool Slus21050MoviePlayer::complete_pending_frame(
    const Slus21050MovieFrameCompletionReceipt& receipt) noexcept {
    if (!can_complete_pending_frame(receipt)) {
        return false;
    }
    impl_->account_staged_frame(receipt);
    return true;
}

bool Slus21050MoviePlayer::has_outstanding_prepared_frame() const noexcept {
    return impl_ != nullptr && impl_->prepared_frame_outstanding;
}

bool Slus21050MoviePlayer::enable_dropped_frame_tracking() noexcept {
    if (impl_ == nullptr || impl_->player_identity == 0U || impl_->decoder == nullptr ||
        impl_->timeline_frames_consumed != 0U || impl_->pending_frame.has_value() ||
        impl_->prepared_frame_outstanding || impl_->dropped_frame_count != 0U) {
        return false;
    }
    if (!impl_->dropped_frame_tracking) {
        if (impl_->dropped_frame_journal_revision == 0U ||
            impl_->dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++impl_->dropped_frame_journal_revision;
        impl_->dropped_frame_tracking = true;
    }
    return impl_->dropped_frame_journal_revision != 0U;
}

bool Slus21050MoviePlayer::dropped_frame_tracking_enabled() const noexcept {
    return impl_ != nullptr && impl_->dropped_frame_tracking &&
           impl_->dropped_frame_journal_revision != 0U;
}

std::uint64_t Slus21050MoviePlayer::session_generation() const noexcept {
    return impl_ == nullptr ? 0U : impl_->session_generation;
}

std::uint64_t Slus21050MoviePlayer::identity() const noexcept {
    return impl_ == nullptr ? 0U : impl_->player_identity;
}

bool Slus21050MoviePlayer::claim_femain_live_admission(
    const Slus21050MovieFrameLease& lease, const std::uint64_t authentication_tag) const noexcept {
    if (impl_ == nullptr || authentication_tag == 0U || !impl_->prepared_frame_outstanding ||
        !impl_->checked_out_frame.has_value() || lease != impl_->checked_out_frame->lease ||
        lease.session_generation == 0U || lease.lease_generation == 0U ||
        impl_->session_generation != lease.session_generation) {
        return false;
    }
    if (impl_->live_admission_session_generation != impl_->session_generation) {
        impl_->live_admission_session_generation = impl_->session_generation;
        impl_->live_admission_lease_generation = 0U;
    }
    if (lease.lease_generation <= impl_->live_admission_lease_generation) {
        return false;
    }
    impl_->live_admission_lease_generation = lease.lease_generation;
    return true;
}

Slus21050MovieDroppedFrameBatchResult Slus21050MoviePlayer::dropped_frame_batch() const noexcept {
    if (impl_ == nullptr || !impl_->dropped_frame_tracking) {
        return {.status = Slus21050MovieDroppedFrameBatchStatus::Disabled};
    }
    if (impl_->dropped_frame_journal_revision == 0U) {
        return {.status = Slus21050MovieDroppedFrameBatchStatus::GenerationExhausted};
    }
    if (impl_->dropped_frame_count == 0U) {
        return {.status = Slus21050MovieDroppedFrameBatchStatus::Empty};
    }
    Slus21050MovieDroppedFrameBatch batch{};
    batch.owner_ = impl_.get();
    batch.player_identity_ = impl_->player_identity;
    batch.session_generation_ = impl_->session_generation;
    batch.journal_revision_ = impl_->dropped_frame_journal_revision;
    batch.count_ = impl_->dropped_frame_count;
    std::copy_n(impl_->dropped_frames.begin(), impl_->dropped_frame_count, batch.frames_.begin());
    return {Slus21050MovieDroppedFrameBatchStatus::Ready, batch};
}

Slus21050MovieDroppedFrameBatchStatus Slus21050MoviePlayer::validate_dropped_frame_batch(
    const Slus21050MovieDroppedFrameBatch& batch) const noexcept {
    if (impl_ == nullptr || !impl_->dropped_frame_tracking) {
        return Slus21050MovieDroppedFrameBatchStatus::Disabled;
    }
    if (impl_->dropped_frame_journal_revision == 0U) {
        return Slus21050MovieDroppedFrameBatchStatus::GenerationExhausted;
    }
    if (impl_->player_identity == 0U || batch.owner_ != impl_.get() ||
        batch.player_identity_ != impl_->player_identity ||
        batch.session_generation_ != impl_->session_generation ||
        batch.journal_revision_ != impl_->dropped_frame_journal_revision || batch.count_ == 0U ||
        batch.count_ != impl_->dropped_frame_count) {
        return Slus21050MovieDroppedFrameBatchStatus::StaleBatch;
    }
    for (std::size_t index = 0; index < batch.count_; ++index) {
        if (!(batch.frames_[index] == impl_->dropped_frames[index])) {
            return Slus21050MovieDroppedFrameBatchStatus::StaleBatch;
        }
    }
    return Slus21050MovieDroppedFrameBatchStatus::Ready;
}

Slus21050MovieDroppedFrameBatchStatus
Slus21050MoviePlayer::validate_dropped_frame_batch_acknowledgement(
    const Slus21050MovieDroppedFrameBatch& batch,
    const std::size_t acknowledged_count) const noexcept {
    const auto validity = validate_dropped_frame_batch(batch);
    if (validity != Slus21050MovieDroppedFrameBatchStatus::Ready) {
        return validity;
    }
    if (impl_->authorized_drop_batch.has_value()) {
        return Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding;
    }
    if (acknowledged_count == 0U || acknowledged_count > batch.count_) {
        return Slus21050MovieDroppedFrameBatchStatus::InvalidAcknowledgement;
    }
    if (impl_->dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Slus21050MovieDroppedFrameBatchStatus::GenerationExhausted;
    }
    return Slus21050MovieDroppedFrameBatchStatus::Ready;
}

Slus21050MovieDroppedFrameBatchStatus Slus21050MoviePlayer::acknowledge_dropped_frame_batch(
    const Slus21050MovieDroppedFrameBatch& batch, const std::size_t acknowledged_count) noexcept {
    if (impl_ == nullptr || !impl_->dropped_frame_tracking) {
        return Slus21050MovieDroppedFrameBatchStatus::StaleBatch;
    }
    const auto validity = validate_dropped_frame_batch_acknowledgement(batch, acknowledged_count);
    if (validity != Slus21050MovieDroppedFrameBatchStatus::Ready) {
        return validity == Slus21050MovieDroppedFrameBatchStatus::Disabled
                   ? Slus21050MovieDroppedFrameBatchStatus::StaleBatch
                   : validity;
    }
    const auto remaining = impl_->dropped_frame_count - acknowledged_count;
    for (std::size_t index = 0; index < remaining; ++index) {
        impl_->dropped_frames[index] = impl_->dropped_frames[index + acknowledged_count];
    }
    impl_->dropped_frame_count = remaining;
    impl_->advance_journal_revision();
    return Slus21050MovieDroppedFrameBatchStatus::Ready;
}

bool Slus21050MoviePlayer::can_complete_pending_frame_and_acknowledge_dropped_frame_batch(
    const Slus21050MovieFrameCompletionReceipt& receipt,
    const Slus21050MovieDroppedFrameBatch& batch) const noexcept {
    if (validate_dropped_frame_batch(batch) != Slus21050MovieDroppedFrameBatchStatus::Ready ||
        impl_->dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max() ||
        !impl_->authorized_drop_batch.has_value() || !impl_->prepared_frame_outstanding ||
        !impl_->checked_out_frame.has_value() ||
        receipt.lease != impl_->checked_out_frame->lease ||
        receipt.payload_digest != impl_->checked_out_frame->payload_digest ||
        receipt.lease.session_generation != impl_->session_generation ||
        receipt.staged_gs_base == 0U || receipt.staged_gs_base > 16383U) {
        return false;
    }
    const auto& binding = *impl_->authorized_drop_batch;
    if (binding.player_identity != batch.player_identity_ ||
        binding.session_generation != batch.session_generation_ ||
        binding.journal_revision != batch.journal_revision_ || binding.count != batch.count_) {
        return false;
    }
    return std::equal(binding.frames.begin(),
                      binding.frames.begin() + static_cast<std::ptrdiff_t>(binding.count),
                      batch.frames_.begin());
}

Slus21050MovieDroppedFrameBatchStatus
Slus21050MoviePlayer::complete_pending_frame_and_acknowledge_dropped_frame_batch(
    const Slus21050MovieFrameCompletionReceipt& receipt,
    const Slus21050MovieDroppedFrameBatch& batch) noexcept {
    const auto validity = validate_dropped_frame_batch(batch);
    if (validity != Slus21050MovieDroppedFrameBatchStatus::Ready) {
        return validity;
    }
    if (impl_->dropped_frame_journal_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Slus21050MovieDroppedFrameBatchStatus::GenerationExhausted;
    }
    if (!impl_->authorized_drop_batch.has_value()) {
        return Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized;
    }
    const auto& binding = *impl_->authorized_drop_batch;
    if (binding.player_identity != batch.player_identity_ ||
        binding.session_generation != batch.session_generation_ ||
        binding.journal_revision != batch.journal_revision_ || binding.count != batch.count_ ||
        !std::equal(binding.frames.begin(),
                    binding.frames.begin() + static_cast<std::ptrdiff_t>(binding.count),
                    batch.frames_.begin())) {
        return Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized;
    }
    if (!impl_->prepared_frame_outstanding || !impl_->checked_out_frame.has_value() ||
        receipt.lease != impl_->checked_out_frame->lease ||
        receipt.payload_digest != impl_->checked_out_frame->payload_digest ||
        receipt.lease.session_generation != impl_->session_generation ||
        receipt.staged_gs_base == 0U || receipt.staged_gs_base > 16383U) {
        return Slus21050MovieDroppedFrameBatchStatus::InvalidFrameReceipt;
    }
    impl_->account_staged_frame(receipt);
    impl_->authorized_drop_batch.reset();
    impl_->dropped_frame_count = 0U;
    impl_->advance_journal_revision();
    return Slus21050MovieDroppedFrameBatchStatus::Ready;
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::prepare() {
    if (impl_ == nullptr) {
        return Slus21050MoviePlayerStatus::Empty;
    }
    if (impl_->prepared_frame_outstanding) {
        return impl_->set_status(Slus21050MoviePlayerStatus::PreparedFrameOutstanding);
    }
    if (impl_->dropped_frame_tracking && impl_->dropped_frame_count != 0U) {
        return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameBatchOutstanding);
    }
    if (impl_->pending_frame.has_value()) {
        return impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);
    }
    if (inactive_status(impl_->status) || impl_->decoder == nullptr) {
        return impl_->status;
    }
    auto decoded = impl_->decode_one();
    if (!decoded.frame.has_value()) {
        return decoded.status;
    }
    ++impl_->timeline_frames_consumed;
    const auto result =
        impl_->prepare_frame(std::move(*decoded.frame), std::nullopt, false, false, std::nullopt);
    return result;
}

Slus21050MoviePlayerStatus
Slus21050MoviePlayer::prepare_at_media_time(const std::uint64_t elapsed_nanoseconds) {
    return prepare_at_media_time_impl(elapsed_nanoseconds, nullptr, std::nullopt);
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::prepare_at_media_time_with_guest_boundary(
    const std::uint64_t elapsed_nanoseconds, const Slus21050FemainFrameGuestBoundary dropped_at) {
    return prepare_at_media_time_impl(elapsed_nanoseconds, nullptr, dropped_at);
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::prepare_at_media_time(
    const std::uint64_t elapsed_nanoseconds, const Slus21050MovieDroppedFrameBatch& dropped_frames) {
    return prepare_at_media_time_impl(elapsed_nanoseconds, &dropped_frames, std::nullopt);
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::prepare_at_media_time_impl(
    const std::uint64_t elapsed_nanoseconds,
    const Slus21050MovieDroppedFrameBatch* authorized_dropped_frames,
    const std::optional<Slus21050FemainFrameGuestBoundary> dropped_at) {
    if (impl_ == nullptr) {
        return Slus21050MoviePlayerStatus::Empty;
    }
    const bool authorized_valid =
        authorized_dropped_frames != nullptr &&
        validate_dropped_frame_batch(*authorized_dropped_frames) ==
            Slus21050MovieDroppedFrameBatchStatus::Ready;
    if (authorized_dropped_frames != nullptr && !authorized_valid) {
        return Slus21050MoviePlayerStatus::DroppedFrameBatchInvalid;
    }
    ++impl_->statistics.timed_steps;
    impl_->statistics.timeline_elapsed_nanoseconds = elapsed_nanoseconds;
    if (impl_->prepared_frame_outstanding) {
        return impl_->set_status(Slus21050MoviePlayerStatus::PreparedFrameOutstanding);
    }
    if (impl_->dropped_frame_tracking && (impl_->dropped_frame_journal_revision == 0U ||
                                          impl_->dropped_frame_journal_revision ==
                                              std::numeric_limits<std::uint64_t>::max())) {
        return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameJournalFull);
    }
    if (impl_->dropped_frame_tracking && impl_->dropped_frame_count != 0U && !authorized_valid) {
        return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameBatchOutstanding);
    }
    if (impl_->pending_frame.has_value()) {
        if (authorized_valid && !impl_->authorized_drop_batch.has_value()) {
            return Slus21050MoviePlayerStatus::DroppedFrameBatchInvalid;
        }
        return impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);
    }
    if (inactive_status(impl_->status) || impl_->decoder == nullptr) {
        return impl_->status;
    }

    // The frame the timeline wants on screen by now; remembered so it never moves backwards.
    auto target_for_rate = [&elapsed_nanoseconds, this](const MovieFrameRate rate) -> std::uint64_t {
        auto target = timeline_frame_at(elapsed_nanoseconds, rate);
        if (impl_->last_timeline_target.has_value() && *impl_->last_timeline_target > target) {
            target = *impl_->last_timeline_target;
        }
        if (!impl_->last_timeline_target.has_value()) {
            impl_->statistics.presentation_frames_due +=
                target != std::numeric_limits<std::uint64_t>::max()
                    ? target + 1U
                    : std::numeric_limits<std::uint64_t>::max();
        } else if (target > *impl_->last_timeline_target) {
            impl_->statistics.presentation_frames_due += target - *impl_->last_timeline_target;
        }
        impl_->last_timeline_target = target;
        impl_->statistics.timeline_target_frame = target;
        return target;
    };

    if (impl_->source_frame_rate.has_value()) {
        const auto target = target_for_rate(*impl_->source_frame_rate);
        if (impl_->timeline_frames_consumed > target) {
            ++impl_->statistics.presentation_frame_repeats;
            return impl_->set_status(Slus21050MoviePlayerStatus::WaitingForPresentationTime);
        }
    }

    for (;;) {
        if (impl_->dropped_frame_tracking && impl_->source_frame_rate.has_value() &&
            impl_->dropped_frame_count != 0U) {
            const auto target = target_for_rate(*impl_->source_frame_rate);
            if (impl_->timeline_frames_consumed >= target) {
                if (authorized_valid && impl_->timeline_frames_consumed == target) {
                    // The authorised batch covers exactly the frames that are due.
                } else {
                    return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameBatchOutstanding);
                }
            } else {
                if (authorized_valid) {
                    return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameBatchOutstanding);
                }
                if (impl_->dropped_frame_journal_revision == 0U ||
                    impl_->dropped_frame_count > kSlus21050MovieDroppedFrameJournalCapacity - 1U) {
                    return impl_->set_status(Slus21050MoviePlayerStatus::DroppedFrameJournalFull);
                }
            }
        }
        auto decoded = impl_->decode_one();
        if (!decoded.frame.has_value()) {
            if (decoded.status == Slus21050MoviePlayerStatus::Decoding) {
                continue;
            }
            if (impl_->source_frame_rate.has_value() &&
                (decoded.status == Slus21050MoviePlayerStatus::LoopRestarted ||
                 decoded.status == Slus21050MoviePlayerStatus::EndOfStream)) {
                const auto pass_frames =
                    impl_->timeline_frames_consumed - impl_->pass_first_timeline_frame;
                impl_->statistics.media_duration_nanoseconds =
                    nanoseconds_for_frames(pass_frames, *impl_->source_frame_rate);
                impl_->pass_first_timeline_frame = impl_->timeline_frames_consumed;
            }
            if (decoded.status == Slus21050MoviePlayerStatus::LoopRestarted) {
                continue;
            }
            return decoded.status;
        }
        auto frame = std::move(*decoded.frame);
        const auto observed_rate = impl_->observe_source_frame_rate(frame);
        if (impl_->status == Slus21050MoviePlayerStatus::SourceFrameRateMismatch) {
            return Slus21050MoviePlayerStatus::SourceFrameRateMismatch;
        }
        const auto timeline_frame = impl_->timeline_frames_consumed++;
        const bool timed = impl_->source_frame_rate.has_value() && observed_rate.has_value();
        const auto target = timed ? target_for_rate(*impl_->source_frame_rate) : timeline_frame;
        if (timeline_frame < target) {
            (void)impl_->prepare_frame(std::move(frame), timeline_frame, true, true, dropped_at);
            continue;
        }
        if (timeline_frame > target) {
            ++impl_->statistics.presentation_frame_repeats;
            return impl_->set_status(Slus21050MoviePlayerStatus::WaitingForPresentationTime);
        }
        const auto result = impl_->prepare_frame(
            std::move(frame), timed ? std::optional<std::uint64_t>{timeline_frame} : std::nullopt, false, true,
            std::nullopt);
        if (result == Slus21050MoviePlayerStatus::FrameStaged && authorized_valid) {
            impl_->authorize_drop_batch(*authorized_dropped_frames);
            return Slus21050MoviePlayerStatus::FrameStaged;
        }
        return result;
    }
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::status() const noexcept {
    return impl_ == nullptr ? Slus21050MoviePlayerStatus::Empty : impl_->status;
}

bool Slus21050MoviePlayer::active() const noexcept {
    return impl_ != nullptr && impl_->decoder != nullptr && !inactive_status(impl_->status);
}

bool Slus21050MoviePlayer::looping() const noexcept {
    return impl_ != nullptr && impl_->loop;
}

bool Slus21050MoviePlayer::has_pending_frame() const noexcept {
    return impl_ != nullptr && impl_->pending_frame.has_value();
}

std::optional<Slus21050MovieGeometry> Slus21050MoviePlayer::geometry() const noexcept {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->geometry;
}

std::span<const std::uint8_t> Slus21050MoviePlayer::pending_strip_major_rgba() const noexcept {
    return impl_ == nullptr || !impl_->pending_frame.has_value()
               ? std::span<const std::uint8_t>{}
               : impl_->pending_frame->bytes();
}

const Slus21050MoviePlayerStatistics& Slus21050MoviePlayer::statistics() const noexcept {
    static const Slus21050MoviePlayerStatistics empty{};
    return impl_ == nullptr ? empty : impl_->statistics;
}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::step(guest::EeMemory& memory) {
    const auto status = prepare();
    if (status != Slus21050MoviePlayerStatus::FrameStaged) {
        return status;
    }
    if (impl_ == nullptr || !impl_->pending_frame.has_value()) {
        return status;
    }
    if (impl_->prepared_frame_outstanding) {
        return impl_->set_status(Slus21050MoviePlayerStatus::PreparedFrameOutstanding);
    }
    ++impl_->statistics.stage_attempts;
    const auto& pending = *impl_->pending_frame;
    const bool frame_ok = pending.strip_major_rgba_.size() == pending.geometry_.buffer_bytes &&
                          impl_->geometry.has_value() &&
                          same_geometry(pending.geometry_, *impl_->geometry);
    if (!frame_ok) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::InvalidFramePlanes);
    }
    const auto& geometry = *impl_->geometry;
    if (!memory.is_main_ram_range(kSlus21050MovieWork, 48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto first = read_movie_work_slot(memory, 0U);
    const auto second = read_movie_work_slot(memory, 1U);
    if (first.texture_columns != geometry.texture_columns ||
        first.texture_rows != geometry.texture_rows ||
        first.macroblock_columns != geometry.macroblock_columns ||
        first.macroblock_rows != geometry.macroblock_rows ||
        second.texture_columns != geometry.texture_columns ||
        second.texture_rows != geometry.texture_rows ||
        second.macroblock_columns != geometry.macroblock_columns ||
        second.macroblock_rows != geometry.macroblock_rows || first.gs_base == 0U ||
        first.gs_base > 16383U || first.gs_base != second.gs_base) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto expected_second = static_cast<std::uint64_t>(first.buffer) + geometry.buffer_bytes;
    if (first.buffer == 0U || (first.buffer & 15U) != 0U || (second.buffer & 15U) != 0U ||
        expected_second > 0xffffffffULL || expected_second != second.buffer) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestBufferMismatch);
    }
    if (!memory.is_main_ram_range(first.buffer, geometry.buffer_bytes) ||
        !memory.is_main_ram_range(second.buffer,
                                  geometry.buffer_bytes + geometry.final_texture_padding_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, second.buffer,
                                       geometry.buffer_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U) ||
        memory.main_ram_ranges_overlap(second.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestRangeInvalid);
    }
    const std::span<const std::uint8_t> frame_bytes = pending.strip_major_rgba_;
    memory.write_main_ram_bytes(first.buffer, std::as_bytes(frame_bytes));
    memory.write_main_ram_bytes(second.buffer, std::as_bytes(frame_bytes));
    ++impl_->statistics.frames_staged;
    impl_->statistics.staged_buffers += 2U;
    impl_->statistics.staged_bytes += geometry.buffer_bytes * 2U;
    impl_->statistics.staged_gs_base = first.gs_base;
    const auto timeline_frame = pending.timeline_frame_;
    if (timeline_frame.has_value()) {
        impl_->statistics.last_staged_timeline_frame = *timeline_frame;
    }
    impl_->pending_frame.reset();
    return impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);

}

Slus21050MoviePlayerStatus
Slus21050MoviePlayer::step_at_media_time(guest::EeMemory& memory,
                                         const std::uint64_t elapsed_nanoseconds) {
    const auto status = prepare_at_media_time_impl(elapsed_nanoseconds, nullptr, std::nullopt);
    if (status != Slus21050MoviePlayerStatus::FrameStaged) {
        return status;
    }
    if (impl_ == nullptr || !impl_->pending_frame.has_value()) {
        return status;
    }
    if (impl_->prepared_frame_outstanding) {
        return impl_->set_status(Slus21050MoviePlayerStatus::PreparedFrameOutstanding);
    }
    ++impl_->statistics.stage_attempts;
    const auto& pending = *impl_->pending_frame;
    const bool frame_ok = pending.strip_major_rgba_.size() == pending.geometry_.buffer_bytes &&
                          impl_->geometry.has_value() &&
                          same_geometry(pending.geometry_, *impl_->geometry);
    if (!frame_ok) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::InvalidFramePlanes);
    }
    const auto& geometry = *impl_->geometry;
    if (!memory.is_main_ram_range(kSlus21050MovieWork, 48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto first = read_movie_work_slot(memory, 0U);
    const auto second = read_movie_work_slot(memory, 1U);
    if (first.texture_columns != geometry.texture_columns ||
        first.texture_rows != geometry.texture_rows ||
        first.macroblock_columns != geometry.macroblock_columns ||
        first.macroblock_rows != geometry.macroblock_rows ||
        second.texture_columns != geometry.texture_columns ||
        second.texture_rows != geometry.texture_rows ||
        second.macroblock_columns != geometry.macroblock_columns ||
        second.macroblock_rows != geometry.macroblock_rows || first.gs_base == 0U ||
        first.gs_base > 16383U || first.gs_base != second.gs_base) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto expected_second = static_cast<std::uint64_t>(first.buffer) + geometry.buffer_bytes;
    if (first.buffer == 0U || (first.buffer & 15U) != 0U || (second.buffer & 15U) != 0U ||
        expected_second > 0xffffffffULL || expected_second != second.buffer) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestBufferMismatch);
    }
    if (!memory.is_main_ram_range(first.buffer, geometry.buffer_bytes) ||
        !memory.is_main_ram_range(second.buffer,
                                  geometry.buffer_bytes + geometry.final_texture_padding_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, second.buffer,
                                       geometry.buffer_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U) ||
        memory.main_ram_ranges_overlap(second.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestRangeInvalid);
    }
    const std::span<const std::uint8_t> frame_bytes = pending.strip_major_rgba_;
    memory.write_main_ram_bytes(first.buffer, std::as_bytes(frame_bytes));
    memory.write_main_ram_bytes(second.buffer, std::as_bytes(frame_bytes));
    ++impl_->statistics.frames_staged;
    impl_->statistics.staged_buffers += 2U;
    impl_->statistics.staged_bytes += geometry.buffer_bytes * 2U;
    impl_->statistics.staged_gs_base = first.gs_base;
    const auto timeline_frame = pending.timeline_frame_;
    if (timeline_frame.has_value()) {
        impl_->statistics.last_staged_timeline_frame = *timeline_frame;
    }
    impl_->pending_frame.reset();
    return impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);

}

Slus21050MoviePlayerStatus Slus21050MoviePlayer::step_at_media_time_with_guest_boundary(
    guest::EeMemory& memory, const std::uint64_t elapsed_nanoseconds,
    const Slus21050FemainFrameGuestBoundary dropped_at) {
    const auto status = prepare_at_media_time_impl(elapsed_nanoseconds, nullptr, dropped_at);
    if (status != Slus21050MoviePlayerStatus::FrameStaged) {
        return status;
    }
    if (impl_ == nullptr || !impl_->pending_frame.has_value()) {
        return status;
    }
    if (impl_->prepared_frame_outstanding) {
        return impl_->set_status(Slus21050MoviePlayerStatus::PreparedFrameOutstanding);
    }
    ++impl_->statistics.stage_attempts;
    const auto& pending = *impl_->pending_frame;
    const bool frame_ok = pending.strip_major_rgba_.size() == pending.geometry_.buffer_bytes &&
                          impl_->geometry.has_value() &&
                          same_geometry(pending.geometry_, *impl_->geometry);
    if (!frame_ok) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::InvalidFramePlanes);
    }
    const auto& geometry = *impl_->geometry;
    if (!memory.is_main_ram_range(kSlus21050MovieWork, 48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto first = read_movie_work_slot(memory, 0U);
    const auto second = read_movie_work_slot(memory, 1U);
    if (first.texture_columns != geometry.texture_columns ||
        first.texture_rows != geometry.texture_rows ||
        first.macroblock_columns != geometry.macroblock_columns ||
        first.macroblock_rows != geometry.macroblock_rows ||
        second.texture_columns != geometry.texture_columns ||
        second.texture_rows != geometry.texture_rows ||
        second.macroblock_columns != geometry.macroblock_columns ||
        second.macroblock_rows != geometry.macroblock_rows || first.gs_base == 0U ||
        first.gs_base > 16383U || first.gs_base != second.gs_base) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestLayoutMismatch);
    }
    const auto expected_second = static_cast<std::uint64_t>(first.buffer) + geometry.buffer_bytes;
    if (first.buffer == 0U || (first.buffer & 15U) != 0U || (second.buffer & 15U) != 0U ||
        expected_second > 0xffffffffULL || expected_second != second.buffer) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestBufferMismatch);
    }
    if (!memory.is_main_ram_range(first.buffer, geometry.buffer_bytes) ||
        !memory.is_main_ram_range(second.buffer,
                                  geometry.buffer_bytes + geometry.final_texture_padding_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, second.buffer,
                                       geometry.buffer_bytes) ||
        memory.main_ram_ranges_overlap(first.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U) ||
        memory.main_ram_ranges_overlap(second.buffer, geometry.buffer_bytes, kSlus21050MovieWork,
                                       48U)) {
        ++impl_->statistics.stage_failures;
        return impl_->set_status(Slus21050MoviePlayerStatus::GuestRangeInvalid);
    }
    const std::span<const std::uint8_t> frame_bytes = pending.strip_major_rgba_;
    memory.write_main_ram_bytes(first.buffer, std::as_bytes(frame_bytes));
    memory.write_main_ram_bytes(second.buffer, std::as_bytes(frame_bytes));
    ++impl_->statistics.frames_staged;
    impl_->statistics.staged_buffers += 2U;
    impl_->statistics.staged_bytes += geometry.buffer_bytes * 2U;
    impl_->statistics.staged_gs_base = first.gs_base;
    const auto timeline_frame = pending.timeline_frame_;
    if (timeline_frame.has_value()) {
        impl_->statistics.last_staged_timeline_frame = *timeline_frame;
    }
    impl_->pending_frame.reset();
    return impl_->set_status(Slus21050MoviePlayerStatus::FrameStaged);

}

} // namespace takedown::runtime
