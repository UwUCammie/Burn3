#include "takedown/guest/ipu_mpeg2_decoder.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

namespace takedown::guest {

// FNV-1a 32 over the bytes; see the header for the contract.
std::uint32_t ipu_checksum32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

namespace {

// Copies a strided picture plane into a tightly packed vector.
std::vector<std::uint8_t> pack_plane(const std::uint8_t* rows, const int linesize,
                                     const std::size_t width, const std::size_t height) {
    std::vector<std::uint8_t> packed(width * height);
    for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(packed.data() + row * width,
                    rows + static_cast<std::size_t>(linesize) * row, width);
    }
    return packed;
}

// Behavior-neutral backend: accepts bytes, produces nothing.
class NullMpeg2Decoder final : public IpuMpeg2Decoder {
  public:
    ~NullMpeg2Decoder() override = default;

    void push(std::span<const std::uint8_t> bytes) override;
    bool flush() override;
    [[nodiscard]] std::optional<IpuDecodedFrame> receive_frame() override;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept override;
    [[nodiscard]] std::uint64_t frames_decoded() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;

  private:
    std::uint64_t bytes_received_{};
};

// libavcodec MPEG-2 video backend.
class LibAvcodecMpeg2Decoder final : public IpuMpeg2Decoder {
  public:
    LibAvcodecMpeg2Decoder();
    ~LibAvcodecMpeg2Decoder() override;

    void push(std::span<const std::uint8_t> bytes) override;
    bool flush() override;
    [[nodiscard]] std::optional<IpuDecodedFrame> receive_frame() override;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept override;
    [[nodiscard]] std::uint64_t frames_decoded() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;

  private:
    static void gate_av_log(void* avcl, int level, const char* fmt, va_list args);
    void drain_frames();

    AVCodecContext* codec_context_{};
    AVCodecParserContext* parser_{};
    AVFrame* frame_{};
    AVPacket* packet_{};
    std::vector<std::uint8_t> pending_{};
    std::deque<IpuDecodedFrame> decoded_{};
    std::uint64_t bytes_received_{};
    std::uint64_t frames_decoded_{};
    std::string dump_path_{};
    bool available_{};
};

void NullMpeg2Decoder::push(std::span<const std::uint8_t> bytes) {
    bytes_received_ += bytes.size();
}

bool NullMpeg2Decoder::flush() {
    return false;
}

std::optional<IpuDecodedFrame> NullMpeg2Decoder::receive_frame() {
    return {};
}

std::uint64_t NullMpeg2Decoder::bytes_received() const noexcept {
    return bytes_received_;
}

std::uint64_t NullMpeg2Decoder::frames_decoded() const noexcept {
    return 0;
}

std::string_view NullMpeg2Decoder::backend_name() const noexcept {
    return "null";
}

LibAvcodecMpeg2Decoder::LibAvcodecMpeg2Decoder() {
    av_log_set_callback(gate_av_log);
    if (const char* dump = std::getenv("B3_IPU_FEED_DUMP"); dump != nullptr && *dump != '\0') {
        dump_path_ = dump;
        // Truncate the feed dump now so each run starts from an empty file.
        try {
            std::ofstream(dump_path_, std::ios::binary | std::ios::trunc);
        } catch (...) {
        }
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (codec == nullptr) {
        return;
    }
    codec_context_ = avcodec_alloc_context3(codec);
    if (codec_context_ == nullptr) {
        return;
    }
    codec_context_->thread_count = 1;
    if (avcodec_open2(codec_context_, codec, nullptr) < 0) {
        avcodec_free_context(&codec_context_);
        return;
    }
    parser_ = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (parser_ == nullptr) {
        avcodec_free_context(&codec_context_);
        return;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (frame_ == nullptr || packet_ == nullptr) {
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
        }
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
        }
        if (parser_ != nullptr) {
            av_parser_close(parser_);
        }
        if (codec_context_ != nullptr) {
            avcodec_free_context(&codec_context_);
        }
        return;
    }
    available_ = true;
}

void LibAvcodecMpeg2Decoder::gate_av_log(void* avcl, const int level, const char* fmt,
                                         va_list args) {
    // Decoder chatter about concealed or damaged pictures is dropped unless
    // B3_IPU_AVLOG=1 asks for the full log.
    static constexpr std::string_view kCutPictureMarkers[6] = {
        "concealing", "ac-tex damaged", "MVs not available",
        "slice mismatch", "skipped MB", "Invalid mb type",
    };
    const char* value = std::getenv("B3_IPU_AVLOG");
    const bool verbose = value != nullptr && std::strcmp(value, "1") == 0;
    if (!verbose) {
        for (const std::string_view marker : kCutPictureMarkers) {
            if (std::string_view(fmt).find(marker) != std::string_view::npos) {
                return;
            }
        }
    }
    av_log_default_callback(avcl, level, fmt, args);
}

LibAvcodecMpeg2Decoder::~LibAvcodecMpeg2Decoder() {
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (parser_ != nullptr) {
        av_parser_close(parser_);
    }
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }
}

void LibAvcodecMpeg2Decoder::push(std::span<const std::uint8_t> bytes) {
    bytes_received_ += bytes.size();
    if (!available_ || bytes.empty()) {
        return;
    }
    if (!dump_path_.empty()) {
        std::ofstream out(dump_path_, std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());

    while (!pending_.empty()) {
        std::uint8_t* out = nullptr;
        int out_size = 0;
        const int consumed = av_parser_parse2(
            parser_, codec_context_, &out, &out_size, pending_.data(),
            static_cast<int>(pending_.size()), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size > 0 && out != nullptr && av_new_packet(packet_, out_size) == 0) {
            std::memcpy(packet_->data, out, static_cast<std::size_t>(out_size));
            packet_->pts = AV_NOPTS_VALUE;
            packet_->dts = AV_NOPTS_VALUE;
            while (avcodec_send_packet(codec_context_, packet_) == AVERROR(EAGAIN)) {
                drain_frames();
            }
            av_packet_unref(packet_);
            drain_frames();
        }
        if (consumed <= 0) {
            return;
        }
        pending_.erase(pending_.begin(), pending_.begin() + consumed);
    }
}

bool LibAvcodecMpeg2Decoder::flush() {
    if (!available_) {
        return false;
    }
    const auto before = frames_decoded_;

    while (!pending_.empty()) {
        std::uint8_t* out = nullptr;
        int out_size = 0;
        const int consumed = av_parser_parse2(
            parser_, codec_context_, &out, &out_size, pending_.data(),
            static_cast<int>(pending_.size()), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size > 0 && out != nullptr && av_new_packet(packet_, out_size) == 0) {
            std::memcpy(packet_->data, out, static_cast<std::size_t>(out_size));
            packet_->pts = AV_NOPTS_VALUE;
            packet_->dts = AV_NOPTS_VALUE;
            while (avcodec_send_packet(codec_context_, packet_) == AVERROR(EAGAIN)) {
                drain_frames();
            }
            av_packet_unref(packet_);
            drain_frames();
        }
        if (consumed <= 0) {
            break;
        }
        pending_.erase(pending_.begin(), pending_.begin() + consumed);
    }

    // Parser tail: pull out whatever the parser still holds, bounded so a
    // parser that keeps producing empty packets cannot spin forever.
    for (int index = 0; index != 16; ++index) {
        std::uint8_t* out = nullptr;
        int out_size = 0;
        av_parser_parse2(parser_, codec_context_, &out, &out_size, nullptr, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (out_size <= 0) {
            break;
        }
        if (out == nullptr || av_new_packet(packet_, out_size) != 0) {
            continue;
        }
        std::memcpy(packet_->data, out, static_cast<std::size_t>(out_size));
        packet_->pts = AV_NOPTS_VALUE;
        packet_->dts = AV_NOPTS_VALUE;
        while (avcodec_send_packet(codec_context_, packet_) == AVERROR(EAGAIN)) {
            drain_frames();
        }
        av_packet_unref(packet_);
        drain_frames();
    }

    // End of stream: a NULL packet asks the codec to drain its delayed pictures.
    while (avcodec_send_packet(codec_context_, nullptr) == AVERROR(EAGAIN)) {
        drain_frames();
    }
    drain_frames();
    pending_.clear();
    return before != frames_decoded_;
}

std::optional<IpuDecodedFrame> LibAvcodecMpeg2Decoder::receive_frame() {
    if (decoded_.empty()) {
        return std::nullopt;
    }
    IpuDecodedFrame frame = std::move(decoded_.front());
    decoded_.pop_front();
    return frame;
}

std::uint64_t LibAvcodecMpeg2Decoder::bytes_received() const noexcept {
    return bytes_received_;
}

std::uint64_t LibAvcodecMpeg2Decoder::frames_decoded() const noexcept {
    return frames_decoded_;
}

std::string_view LibAvcodecMpeg2Decoder::backend_name() const noexcept {
    return "libavcodec";
}

void LibAvcodecMpeg2Decoder::drain_frames() {
    for (;;) {
        const int status = avcodec_receive_frame(codec_context_, frame_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF || status < 0) {
            return;
        }
        if (frame_->format == AV_PIX_FMT_YUV420P && frame_->width > 0 && frame_->height > 0) {
            IpuDecodedFrame decoded{};
            decoded.width = static_cast<std::uint32_t>(frame_->width);
            decoded.height = static_cast<std::uint32_t>(frame_->height);
            decoded.sequence = frames_decoded_;
            if (codec_context_->framerate.num > 0 && codec_context_->framerate.den > 0) {
                decoded.frame_rate_numerator = static_cast<std::uint32_t>(codec_context_->framerate.num);
                decoded.frame_rate_denominator = static_cast<std::uint32_t>(codec_context_->framerate.den);
            }
            const auto width = static_cast<std::size_t>(frame_->width);
            const auto height = static_cast<std::size_t>(frame_->height);
            const auto chroma_width = static_cast<std::size_t>((static_cast<std::uint32_t>(frame_->width) + 1U) >> 1U);
            const auto chroma_height = static_cast<std::size_t>((static_cast<std::uint32_t>(frame_->height) + 1U) >> 1U);
            decoded.y_plane = pack_plane(frame_->data[0], frame_->linesize[0], width, height);
            decoded.cb_plane = pack_plane(frame_->data[1], frame_->linesize[1], chroma_width, chroma_height);
            decoded.cr_plane = pack_plane(frame_->data[2], frame_->linesize[2], chroma_width, chroma_height);
            // FFmpeg corrupt-frame flag (1) or decode error bit 4: concealed.
            decoded.concealed = (frame_->flags & 1) != 0 || (frame_->decode_error_flags & 4) != 0;
            decoded.checksum = ipu_checksum32(decoded.y_plane) ^ ipu_checksum32(decoded.cb_plane) ^
                               ipu_checksum32(decoded.cr_plane);
            ++frames_decoded_;
            decoded_.push_back(std::move(decoded));
        }
        av_frame_unref(frame_);
    }
}

} // namespace

std::unique_ptr<IpuMpeg2Decoder> create_libavcodec_mpeg2_decoder() {
    return std::make_unique<LibAvcodecMpeg2Decoder>();
}

std::unique_ptr<IpuMpeg2Decoder> create_null_mpeg2_decoder() {
    return std::make_unique<NullMpeg2Decoder>();
}

} // namespace takedown::guest
