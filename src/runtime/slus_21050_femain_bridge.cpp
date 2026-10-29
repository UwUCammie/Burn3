#include "takedown/runtime/slus_21050_femain_bridge.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>

namespace takedown::runtime {

namespace {

[[nodiscard]] std::uint8_t clamp_channel(const int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

constexpr std::uint32_t kSlus21050FemainMovieWorkSlotBytes = 24U;

struct MovieWorkSlot {
    std::uint32_t buffer{};
    std::uint32_t texture_columns{};
    std::uint32_t texture_rows{};
    std::uint32_t macroblock_columns{};
    std::uint32_t macroblock_rows{};
    std::uint32_t gs_base{};
};

[[nodiscard]] MovieWorkSlot read_movie_work_slot(const guest::EeMemory& memory,
                                                 std::size_t index) {
    const auto base = kSlus21050FemainMovieWork + static_cast<std::uint32_t>(index) * 24U;
    MovieWorkSlot slot;
    slot.buffer = memory.read_u32_le(base);
    slot.texture_columns = memory.read_u32_le(base + 4U);
    slot.texture_rows = memory.read_u32_le(base + 8U);
    slot.macroblock_columns = memory.read_u32_le(base + 12U);
    slot.macroblock_rows = memory.read_u32_le(base + 16U);
    slot.gs_base = memory.read_u32_le(base + 20U);
    return slot;
}

} // namespace

const char* to_string(const Slus21050FemainLoadStatus status) noexcept {
    switch (status) {
    case Slus21050FemainLoadStatus::Empty:
        return "empty";
    case Slus21050FemainLoadStatus::Ready:
        return "ready";
    case Slus21050FemainLoadStatus::MovieOpenFailed:
        return "movie-open-failed";
    case Slus21050FemainLoadStatus::DecoderUnavailable:
        return "decoder-unavailable";
    case Slus21050FemainLoadStatus::NoRealFrame:
        return "no-real-frame";
    case Slus21050FemainLoadStatus::ConcealedFrame:
        return "concealed-frame";
    case Slus21050FemainLoadStatus::UnexpectedFrameGeometry:
        return "unexpected-frame-geometry";
    case Slus21050FemainLoadStatus::InvalidFramePlanes:
        return "invalid-frame-planes";
    }
    return "unknown";
}

const char* to_string(const Slus21050FemainSeedStatus status) noexcept {
    switch (status) {
    case Slus21050FemainSeedStatus::NotAttempted:
        return "not-attempted";
    case Slus21050FemainSeedStatus::Seeded:
        return "seeded";
    case Slus21050FemainSeedStatus::FrameNotReady:
        return "frame-not-ready";
    case Slus21050FemainSeedStatus::GuestLayoutMismatch:
        return "guest-layout-mismatch";
    case Slus21050FemainSeedStatus::GuestBufferMismatch:
        return "guest-buffer-mismatch";
    case Slus21050FemainSeedStatus::GuestRangeInvalid:
        return "guest-range-invalid";
    }
    return "unknown";
}

Slus21050FemainLoadStatus
Slus21050FemainBridge::cache_decoded_frame(const guest::IpuDecodedFrame& frame) {
    if (frame.concealed) {
        return Slus21050FemainLoadStatus::ConcealedFrame;
    }
    if (frame.width != kSlus21050FemainWidth || frame.height != kSlus21050FemainHeight) {
        return Slus21050FemainLoadStatus::UnexpectedFrameGeometry;
    }
    if (frame.y_plane.size() != static_cast<std::size_t>(kSlus21050FemainWidth) *
                                    kSlus21050FemainHeight ||
        frame.cb_plane.size() != static_cast<std::size_t>(kSlus21050FemainWidth / 2U) *
                                     (kSlus21050FemainHeight / 2U) ||
        frame.cr_plane.size() != static_cast<std::size_t>(kSlus21050FemainWidth / 2U) *
                                     (kSlus21050FemainHeight / 2U)) {
        return Slus21050FemainLoadStatus::InvalidFramePlanes;
    }

    strip_major_rgba_.assign(kSlus21050FemainBufferBytes, 0U);
    for (std::size_t column = 0U; column < kSlus21050FemainMacroblockColumns; ++column) {
        for (std::size_t y = 0U; y < kSlus21050FemainHeight; ++y) {
            for (std::size_t within = 0U; within < 16U; ++within) {
                const auto x = column * 16U + within;
                const int luma = frame.y_plane[y * kSlus21050FemainWidth + x];
                const auto chroma_index = (y >> 1U) * (kSlus21050FemainWidth / 2U) +
                                                 (x >> 1U);
                const int cb = frame.cb_plane[chroma_index];
                const int cr = frame.cr_plane[chroma_index];
                const int c = luma - 16;
                const int d = cb - 128;
                const int e = cr - 128;
                const int red = (298 * c + 409 * e + 128) >> 8;
                const int green = (298 * c - 100 * d - 208 * e + 128) >> 8;
                const int blue = (298 * c + 516 * d + 128) >> 8;
                const auto destination =
                    ((column * kSlus21050FemainHeight + y) * 16U + within) * 4U;
                strip_major_rgba_[destination] =
                    clamp_channel(red);
                strip_major_rgba_[destination + 1U] =
                    clamp_channel(green);
                strip_major_rgba_[destination + 2U] =
                    clamp_channel(blue);
                strip_major_rgba_[destination + 3U] = 128U;
            }
        }
    }
    statistics_.source_sequence = frame.sequence;
    statistics_.source_checksum = frame.checksum;
    return Slus21050FemainLoadStatus::Ready;
}

bool Slus21050FemainBridge::receive_first_valid_frame(
    guest::IpuMpeg2Decoder& decoder, Slus21050FemainLoadStatus& last_failure) {
    while (auto frame = decoder.receive_frame()) {
        ++statistics_.frames_received;
        if (frame->concealed) {
            ++statistics_.concealed_frames;
            if (last_failure == Slus21050FemainLoadStatus::Empty) {
                last_failure = Slus21050FemainLoadStatus::ConcealedFrame;
            }
            continue;
        }
        const Slus21050FemainLoadStatus status = cache_decoded_frame(*frame);
        if (status == Slus21050FemainLoadStatus::Ready) {
            load_status_ = Slus21050FemainLoadStatus::Ready;
            return true;
        }
        last_failure = status;
    }
    return false;
}

Slus21050FemainBridge
Slus21050FemainBridge::from_decoded_frame(const guest::IpuDecodedFrame& frame) {
    Slus21050FemainBridge bridge;
    bridge.statistics_.frames_received = 1U;
    if (frame.concealed) {
        bridge.statistics_.concealed_frames = 1U;
    }
    bridge.load_status_ = bridge.cache_decoded_frame(frame);
    return bridge;
}

Slus21050FemainBridge
Slus21050FemainBridge::decode_first_real_frame(const std::span<const std::uint8_t> elementary_stream,
                                               guest::IpuMpeg2Decoder& decoder) {
    Slus21050FemainBridge bridge;
    if (decoder.backend_name() == std::string_view{"null"}) {
        bridge.load_status_ = Slus21050FemainLoadStatus::DecoderUnavailable;
        return bridge;
    }

    constexpr std::size_t kChunkBytes = 65536U;
    Slus21050FemainLoadStatus last_failure = Slus21050FemainLoadStatus::Empty;
    std::size_t offset = 0U;
    while (offset < elementary_stream.size() && !bridge.ready()) {
        const std::size_t length = std::min(kChunkBytes, elementary_stream.size() - offset);
        const auto chunk = elementary_stream.subspan(offset, length);
        bridge.statistics_.input_bytes += chunk.size();
        decoder.push(chunk);
        offset += chunk.size();
        static_cast<void>(bridge.receive_first_valid_frame(decoder, last_failure));
    }
    if (!bridge.ready()) {
        decoder.flush();
        static_cast<void>(bridge.receive_first_valid_frame(decoder, last_failure));
    }
    if (!bridge.ready()) {
        bridge.load_status_ = last_failure != Slus21050FemainLoadStatus::Empty
                                  ? last_failure
                                  : Slus21050FemainLoadStatus::NoRealFrame;
    }
    return bridge;
}

Slus21050FemainBridge
Slus21050FemainBridge::decode_first_real_frame(const std::filesystem::path& movie_path) {
    Slus21050FemainBridge bridge;
    std::ifstream stream(movie_path, std::ios::binary);
    if (!stream) {
        bridge.load_status_ = Slus21050FemainLoadStatus::MovieOpenFailed;
        return bridge;
    }
    auto decoder = guest::create_libavcodec_mpeg2_decoder();
    if (decoder == nullptr || decoder->backend_name() == std::string_view{"null"}) {
        bridge.load_status_ = Slus21050FemainLoadStatus::DecoderUnavailable;
        return bridge;
    }

    Slus21050FemainLoadStatus last_failure = Slus21050FemainLoadStatus::Empty;
    std::array<std::uint8_t, 65536U> chunk{};
    while (stream && !bridge.ready()) {
        stream.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count == 0U) {
            break;
        }
        bridge.statistics_.input_bytes += count;
        decoder->push(std::span<const std::uint8_t>{chunk.data(), count});
        static_cast<void>(bridge.receive_first_valid_frame(*decoder, last_failure));
    }
    if (!bridge.ready()) {
        decoder->flush();
        static_cast<void>(bridge.receive_first_valid_frame(*decoder, last_failure));
    }
    if (!bridge.ready()) {
        bridge.load_status_ = last_failure != Slus21050FemainLoadStatus::Empty
                                  ? last_failure
                                  : Slus21050FemainLoadStatus::NoRealFrame;
    }
    return bridge;
}

bool Slus21050FemainBridge::ready() const noexcept {
    return load_status_ == Slus21050FemainLoadStatus::Ready &&
           strip_major_rgba_.size() == kSlus21050FemainBufferBytes;
}

Slus21050FemainLoadStatus Slus21050FemainBridge::load_status() const noexcept {
    return load_status_;
}

std::span<const std::uint8_t> Slus21050FemainBridge::strip_major_rgba() const noexcept {
    return strip_major_rgba_;
}

const Slus21050FemainBridgeStatistics& Slus21050FemainBridge::statistics() const noexcept {
    return statistics_;
}

Slus21050FemainSeedStatus Slus21050FemainBridge::seed_guest_buffers(guest::EeMemory& memory) {
    ++statistics_.seed_attempts;
    if (!ready()) {
        statistics_.last_seed_status = Slus21050FemainSeedStatus::FrameNotReady;
        return Slus21050FemainSeedStatus::FrameNotReady;
    }
    if (!memory.is_main_ram_range(kSlus21050FemainMovieWork, kSlus21050FemainMovieWorkSlotBytes * 2U)) {
        statistics_.last_seed_status = Slus21050FemainSeedStatus::GuestLayoutMismatch;
        return Slus21050FemainSeedStatus::GuestLayoutMismatch;
    }

    const MovieWorkSlot first = read_movie_work_slot(memory, 0U);
    const MovieWorkSlot second = read_movie_work_slot(memory, 1U);
    const auto shape_matches = [](const MovieWorkSlot& layout) {
        return layout.texture_columns == kSlus21050FemainTextureColumns &&
               layout.texture_rows == kSlus21050FemainTextureRows &&
               layout.macroblock_columns == kSlus21050FemainMacroblockColumns &&
               layout.macroblock_rows == kSlus21050FemainMacroblockRows &&
               layout.gs_base == kSlus21050FemainGsBase;
    };
    if (!shape_matches(first) || !shape_matches(second)) {
        statistics_.last_seed_status = Slus21050FemainSeedStatus::GuestLayoutMismatch;
        return Slus21050FemainSeedStatus::GuestLayoutMismatch;
    }

    const auto expected_second =
        static_cast<std::uint64_t>(first.buffer) + kSlus21050FemainBufferBytes;
    if (first.buffer == 0U || second.buffer != expected_second || (first.buffer & 15U) != 0U ||
        (second.buffer & 15U) != 0U) {
        statistics_.last_seed_status = Slus21050FemainSeedStatus::GuestBufferMismatch;
        return Slus21050FemainSeedStatus::GuestBufferMismatch;
    }
    // The second buffer's last texture row is only half visible, so the
    // retail packet walks 0x800 bytes past it.
    if (!memory.is_main_ram_range(first.buffer, kSlus21050FemainBufferBytes) ||
        !memory.is_main_ram_range(second.buffer, kSlus21050FemainBufferBytes +
                                                     kSlus21050FemainFinalTexturePaddingBytes) ||
        memory.main_ram_ranges_overlap(first.buffer, kSlus21050FemainBufferBytes, second.buffer,
                                       kSlus21050FemainBufferBytes)) {
        statistics_.last_seed_status = Slus21050FemainSeedStatus::GuestRangeInvalid;
        return Slus21050FemainSeedStatus::GuestRangeInvalid;
    }

    const std::span<const std::byte> frame_bytes = std::as_bytes(std::span{strip_major_rgba_});
    memory.write_main_ram_bytes(first.buffer, frame_bytes);
    memory.write_main_ram_bytes(second.buffer, frame_bytes);
    ++statistics_.seed_calls;
    statistics_.seeded_buffers += 2U;
    statistics_.seeded_bytes += kSlus21050FemainBufferBytes * 2U;
    statistics_.last_seed_status = Slus21050FemainSeedStatus::Seeded;
    return Slus21050FemainSeedStatus::Seeded;
}

} // namespace takedown::runtime
