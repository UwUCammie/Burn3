// RECOVERY: rebuilt from the GIMPLE of the release object. 49 of 52 functions
// compare identical (library functions print memcpy where the rebuild prints
// __builtin_memcpy; that naming is not a code difference). Left differing,
// without any behavioural difference:
//   ParallelGsOutputSink::consume   - GCC places one write_begin clobber and the
//                                     landing pads of the VBlank-flush throw differently.
//   ParallelGsOutputSink::finish    - the original has an extra empty cleanup
//                                     region around the output fclose try block.
//   ParallelGsRawStreamWriter::write_vblank - basic-block counts differ by a few units.

#include "takedown/platform/parallel_gs_stream.hpp"
#include "takedown/platform/native_cmd_hooks.hpp"

#include "takedown/render/native_model_protocol.hpp"
#include "takedown/platform/windows_gs_pipe.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takedown::platform {

namespace {
NativeCmdStreamHooks* g_native_cmd_hooks = nullptr;
}
void set_native_cmd_stream_hooks(NativeCmdStreamHooks* hooks) noexcept { g_native_cmd_hooks = hooks; }
NativeCmdStreamHooks* native_cmd_stream_hooks() noexcept { return g_native_cmd_hooks; }

namespace {

std::uint64_t nanoseconds_since(const std::chrono::steady_clock::time_point begin) {
    return static_cast<std::uint64_t>((std::chrono::steady_clock::now() - begin).count());
}

} // namespace

// Writes the segment stream to the renderer's pipe or file, and the optional
// capture mirror and digest log. Runs on the transport's pump thread.
class ParallelGsOutputSink final : public GsStreamSink {
  public:
    ParallelGsOutputSink(FILE* output, std::filesystem::path output_path, bool live_fifo, FILE* mirror,
                         std::optional<std::filesystem::path> live_capture_path)
        : output_(output), output_path_(std::move(output_path)), live_fifo_(live_fifo), mirror_(mirror),
          live_capture_path_(std::move(live_capture_path)) {}

    ~ParallelGsOutputSink() override {
        if (digest_log_ != nullptr) {
            std::fclose(digest_log_);
        }
        if (mirror_ != nullptr) {
            std::fclose(mirror_);
        }
        if (output_ != nullptr) {
            std::fclose(output_);
        }
    }

    void consume(const GsStreamSegment& segment) override;
    void finish() override;

    // Folds one piece's bytes into the running stream digest: 8-byte words
    // first, then the tail bytes.
    void fold_piece(const unsigned char* cursor, const std::size_t size) {
        std::size_t remaining = size;
        unsigned long long digest = size ^ digest_;
        while (remaining > 7) {
            std::uint64_t word;
            std::memcpy(&word, cursor, sizeof word);
            digest = std::rotr((digest ^ word) * 0x9E3779B97F4A7C15ULL, 35);
            cursor += 8;
            remaining -= 8;
        }
        while (remaining != 0) {
            digest = std::rotr((digest ^ *cursor) * 1099511628211ULL, 53);
            ++cursor;
            --remaining;
        }
        digest_ = digest;
        digest_bytes_ += size;
    }

    // The log's ordinals continue from the VBlanks already encoded.
    void attach_digest_log(FILE* const log, const std::uint64_t ordinal) {
        digest_log_ = log;
        vblank_ordinal_ = ordinal;
    }

    void write_mirror(const std::uint8_t* const data, const std::size_t size) {
        const auto write_begin = std::chrono::steady_clock::now();
        counters_.mirror_write_operations.fetch_add(1, std::memory_order_relaxed);
        const std::size_t written = std::fwrite(data, 1, size, mirror_);
        counters_.mirror_bytes_written.fetch_add(written, std::memory_order_relaxed);
        counters_.mirror_write_nanoseconds.fetch_add(
            nanoseconds_since(write_begin),
            std::memory_order_relaxed);
        if (written != size) {
            const int error = errno != 0 ? errno : EIO;
            throw std::system_error(error, std::generic_category(),
                                    "could not write paraLLEl-GS live capture " + live_capture_path_->string());
        }
    }

    void write_output(const std::uint8_t* const data, const std::size_t size) {
        if (live_fifo_) {
            constexpr std::size_t kMaximumWriteBytes = 262144;
            const unsigned char* cursor = data;
            std::size_t remaining = size;
            void* const pipe = reinterpret_cast<void*>(_get_osfhandle(_fileno(output_)));
            while (remaining != 0) {
                const auto begin = std::chrono::steady_clock::now();
                counters_.output_write_operations.fetch_add(1, std::memory_order_relaxed);
                DWORD written = 0;
                const BOOL ok = WriteFile(pipe, cursor, static_cast<DWORD>(std::min(remaining, kMaximumWriteBytes)),
                                          &written, nullptr);
                counters_.output_write_nanoseconds.fetch_add(nanoseconds_since(begin), std::memory_order_relaxed);
                if (ok == 0) {
                    throw windows_gs_pipe::error("could not write paraLLEl-GS stream");
                }
                if (written == 0) {
                    throw std::runtime_error("paraLLEl-GS pipe write made no progress");
                }
                cursor += written;
                remaining -= written;
                counters_.output_bytes_written.fetch_add(written, std::memory_order_relaxed);
            }
        } else {
            const auto write_begin = std::chrono::steady_clock::now();
            counters_.output_write_operations.fetch_add(1, std::memory_order_relaxed);
            const std::size_t written = std::fwrite(data, 1, size, output_);
            int write_error = 0;
            if (written != size) {
                write_error = errno != 0 ? errno : EIO;
            }
            counters_.output_bytes_written.fetch_add(written, std::memory_order_relaxed);
            counters_.output_write_nanoseconds.fetch_add(nanoseconds_since(write_begin), std::memory_order_relaxed);
            if (written != size) {
                throw std::system_error(write_error, std::generic_category(),
                                        "could not write paraLLEl-GS stream " + output_path_.string());
            }
        }
    }

    void copy_counters(ParallelGsStreamStatistics& result) const {
    result.mirror_write_operations = counters_.mirror_write_operations.load(std::memory_order_relaxed);
    result.mirror_bytes_written = counters_.mirror_bytes_written.load(std::memory_order_relaxed);
    result.mirror_write_nanoseconds = counters_.mirror_write_nanoseconds.load(std::memory_order_relaxed);
    result.mirror_flush_operations = counters_.mirror_flush_operations.load(std::memory_order_relaxed);
    result.mirror_flush_nanoseconds = counters_.mirror_flush_nanoseconds.load(std::memory_order_relaxed);
    result.output_write_operations = counters_.output_write_operations.load(std::memory_order_relaxed);
    result.output_bytes_written = counters_.output_bytes_written.load(std::memory_order_relaxed);
    result.output_write_nanoseconds = counters_.output_write_nanoseconds.load(std::memory_order_relaxed);
    result.output_flush_operations = counters_.output_flush_operations.load(std::memory_order_relaxed);
    result.output_flush_nanoseconds = counters_.output_flush_nanoseconds.load(std::memory_order_relaxed);
    result.fifo_backpressure_waits = counters_.fifo_backpressure_waits.load(std::memory_order_relaxed);
    result.fifo_backpressure_nanoseconds = counters_.fifo_backpressure_nanoseconds.load(std::memory_order_relaxed);
    }

    // Writer-thread counters, read as a snapshot by the emulation thread.
    struct Counters {
        std::atomic<std::uint64_t> mirror_write_operations{};
        std::atomic<std::uint64_t> mirror_bytes_written{};
        std::atomic<std::uint64_t> mirror_write_nanoseconds{};
        std::atomic<std::uint64_t> mirror_flush_operations{};
        std::atomic<std::uint64_t> mirror_flush_nanoseconds{};
        std::atomic<std::uint64_t> output_write_operations{};
        std::atomic<std::uint64_t> output_bytes_written{};
        std::atomic<std::uint64_t> output_write_nanoseconds{};
        std::atomic<std::uint64_t> output_flush_operations{};
        std::atomic<std::uint64_t> output_flush_nanoseconds{};
        std::atomic<std::uint64_t> fifo_backpressure_waits{};
        std::atomic<std::uint64_t> fifo_backpressure_nanoseconds{};
    };

    FILE* output_{};
    std::filesystem::path output_path_{};
    bool live_fifo_{};
    FILE* mirror_{};
    std::optional<std::filesystem::path> live_capture_path_{};
    // TAKEDOWN_CAPTURE_HOST_PRESENTATION=1 also mirrors the host-only records.
    bool mirror_host_only_ = [] {
        const char* value = std::getenv("TAKEDOWN_CAPTURE_HOST_PRESENTATION");
        return value != nullptr && std::string_view(value) == "1";
    }();
    FILE* digest_log_{};
    std::uint64_t digest_ = 0x9E3779B97F4A7C15ULL;
    std::uint64_t digest_bytes_{};
    std::uint64_t vblank_ordinal_{};
    Counters counters_{};
};

void ParallelGsOutputSink::consume(const GsStreamSegment& segment) {
    if (segment.size == 0U) {
        return;
    }
    if (digest_log_ != nullptr || mirror_ != nullptr) {
        const std::uint8_t* cursor = segment.data();
        const std::uint8_t* run_begin = nullptr;
        std::size_t run_size = 0;
        bool saw_vblank = false;
        // Adjacent non-host-only bytes are written to the mirror as one run.
        const auto flush_run = [this, &run_size, &run_begin]() {
            if (mirror_ != nullptr && run_size != 0) {
                write_mirror(run_begin, run_size);
            }
            run_size = 0;
        };

        for (const GsStreamPiece& piece : segment.pieces) {
            if ((piece.flags & GsStreamPiece::kHostOnly) == 0) {
                if (digest_log_ != nullptr) {
                    fold_piece(cursor, piece.size);
                }
                if (run_size == 0) {
                    run_begin = cursor;
                }
                run_size += piece.size;
                if ((piece.flags & GsStreamPiece::kVblank) != 0) {
                    saw_vblank = true;
                    ++vblank_ordinal_;
                    if (digest_log_ != nullptr) {
                        std::fprintf(digest_log_, "%llu %llu %016llx\n", vblank_ordinal_, digest_bytes_, digest_);
                    }
                }
            } else if (mirror_host_only_) {
                if (run_size == 0) {
                    run_begin = cursor;
                }
                run_size += piece.size;
            } else {
                flush_run();
            }
            cursor += piece.size;
        }
        flush_run();
        if (saw_vblank && mirror_ != nullptr) {
            const auto flush_begin = std::chrono::steady_clock::now();
            counters_.mirror_flush_operations.fetch_add(1, std::memory_order_relaxed);
            const int flush_result = std::fflush(mirror_);
            counters_.mirror_flush_nanoseconds.fetch_add(
                nanoseconds_since(flush_begin),
                std::memory_order_relaxed);
            if (flush_result != 0) {
                const int error = errno != 0 ? errno : EIO;
                throw std::system_error(error, std::generic_category(),
                                        "could not flush paraLLEl-GS live capture " +
                                            live_capture_path_->string());
            }
        }
    }

    // The whole segment goes to the output in order.
    write_output(segment.data(), segment.size);
    if (!live_fifo_) {
        const auto flush_begin = std::chrono::steady_clock::now();
        counters_.output_flush_operations.fetch_add(1, std::memory_order_relaxed);
        const int flush_result = std::fflush(output_);
        counters_.output_flush_nanoseconds.fetch_add(
            nanoseconds_since(flush_begin),
            std::memory_order_relaxed);
        if (flush_result != 0) {
            const int error = errno != 0 ? errno : EIO;
            throw std::system_error(error, std::generic_category(),
                                    "could not flush paraLLEl-GS stream " + output_path_.string());
        }
    }
}

void ParallelGsOutputSink::finish() {
    std::exception_ptr first_error;
    const auto remember = [&first_error](std::exception_ptr error) {
        if (first_error == nullptr) {
            first_error = std::move(error);
        }
    };
    if (digest_log_ != nullptr) {
        std::fclose(digest_log_);
        digest_log_ = nullptr;
    }
    if (mirror_ != nullptr) {
        FILE* mirror = mirror_;
        mirror_ = nullptr;
        if (std::fclose(mirror) != 0) {
            const int error = errno != 0 ? errno : EIO;
            remember(std::make_exception_ptr(
                std::system_error(error, std::generic_category(),
                                  "could not close paraLLEl-GS live capture " + live_capture_path_->string())));
        }
    }
    if (output_ != nullptr) {
        FILE* output = output_;
        output_ = nullptr;
        int close_result = 0;
        int close_error = 0;
        try {
            close_result = std::fclose(output);
            if (close_result != 0) {
                close_error = errno != 0 ? errno : EIO;
            }
        } catch (...) {
            remember(std::current_exception());
        }
        if (close_result != 0) {
            remember(std::make_exception_ptr(std::system_error(
                close_error, std::generic_category(), "could not close paraLLEl-GS stream " + output_path_.string())));
        }
    }
    if (first_error != nullptr) {
        std::rethrow_exception(first_error);
    }
}

} // namespace takedown::platform

namespace takedown::platform {

namespace {

// A Transfer packet header is: u8 kind, u8 path, u32 payload bytes.
constexpr std::size_t kTransferHeaderBytes = 6U;
constexpr std::size_t kTransferChunkBytes = kParallelGsTransferChunkQuadwords * 16U;

// Creates a new regular file for writing; an existing path is never reused.
FILE* create_exclusive_file(const std::filesystem::path& path) {
    int descriptor = -1;
    const errno_t status = _wsopen_s(&descriptor, path.c_str(), 34049, 64, 384);
    if (status != 0 || descriptor < 0) {
        return nullptr;
    }
    FILE* file = _fdopen(descriptor, "wb");
    if (file == nullptr) {
        _close(descriptor);
    }
    return file;
}

void store_u64_le(std::array<std::uint8_t, 0x2000U>& registers, const unsigned int offset,
                  const std::uint64_t value) {
    for (unsigned int byte = 0U; byte != 8U; ++byte) {
        registers[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

// The seven display-relevant registers the observer tracks, at their
// privileged-register offsets; everything else in the snapshot stays zero.
void fill_privileged_registers(std::array<std::uint8_t, 0x2000U>& registers,
                               const guest::GsPrivilegedDisplayState& display_state,
                               const ParallelGsCrtcHint crt_hint) {
    store_u64_le(registers, 0x00U, display_state.pmode.raw);
    store_u64_le(registers, 0x20U, display_state.smode2.raw);
    store_u64_le(registers, 0x70U, display_state.dispfb[0].raw);
    store_u64_le(registers, 0x80U, display_state.display[0].raw);
    store_u64_le(registers, 0x90U, display_state.dispfb[1].raw);
    store_u64_le(registers, 0xA0U, display_state.display[1].raw);
    store_u64_le(registers, 0xE0U, display_state.bgcolor.raw);
    // SetGsCrt is modelled by the kernel, so SMODE1 and SYNCV never arrive as
    // MMIO stores. For the game's NTSC mode, give the renderer the values the
    // BIOS programs; without SMODE1 it cannot pick a video mode and shows nothing.
    if (crt_hint.configured && crt_hint.mode == 2) {
        const bool interlaced = display_state.smode2.seen ? (display_state.smode2.raw & 1U) != 0U
                                                          : crt_hint.interlace != 0;
        store_u64_le(registers, 0x10U, interlaced ? 0x1740414504ULL : 0x1740410504ULL);
        store_u64_le(registers, 0x60U, 0x00C7800001E00009ULL);
    }
}

void store_u32_le(std::uint8_t* const out, const std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
    out[2] = static_cast<std::uint8_t>(value >> 16U);
    out[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint8_t gif_path_index(const guest::GifPath path) {
    switch (path) {
    case guest::GifPath::Path1:
    case guest::GifPath::Path2:
    case guest::GifPath::Path3:
        return static_cast<std::uint8_t>(path);
    }
    throw std::invalid_argument("invalid GIF path for paraLLEl-GS raw stream");
}

} // namespace

void ParallelGsRawStreamWriter::require_open() const {
    if (closed_) {
        throw std::logic_error("paraLLEl-GS raw stream is closed");
    }
}

void ParallelGsRawStreamWriter::throw_if_unsupported_reset() const {
    if (unsupported_reset_seen_) {
        throw std::runtime_error("paraLLEl-GS raw streams cannot encode a GIF reset which discards partial or non-EOP path state");
    }
}

void ParallelGsRawStreamWriter::publish_segment() {
    if (segment_ == nullptr || segment_->size == 0) {
        return;
    }
    GsStreamSegment* segment = segment_;
    segment_ = nullptr;
    transport_->publish(*segment);
}

void ParallelGsRawStreamWriter::ensure_segment_space(const std::size_t bytes) {
    if (segment_ != nullptr && segment_->capacity - segment_->size >= bytes) {
        return;
    }
    publish_segment();
    if (segment_ == nullptr) {
        segment_ = &transport_->acquire();
    }
}

void ParallelGsRawStreamWriter::begin_transfer(const std::size_t path_index) {
    ensure_segment_space(kTransferHeaderBytes + kTransferChunkBytes);
    header_ = segment_->data() + segment_->size;
    *header_ = 0U;
    header_[1] = static_cast<std::uint8_t>(path_index);
    cursor_ = header_ + kTransferHeaderBytes;
    limit_ = header_ + kTransferHeaderBytes + kTransferChunkBytes;
    open_path_ = static_cast<int>(path_index);
}

bool ParallelGsRawStreamWriter::gif_paths_at_eop_boundaries() const noexcept {
    return std::find_if_not(gif_path_framing_.begin(), gif_path_framing_.end(),
                            [](const GifPathFraming& framing) {
                                return framing.payload_quadwords_remaining == 0U && framing.at_eop_boundary;
                            }) == gif_path_framing_.end();
}

void ParallelGsRawStreamWriter::track_gif_quadword(const std::size_t path_index,
                                                   const guest::GsQuadword& quadword) noexcept {
    GifPathFraming& framing = gif_path_framing_[path_index];
    if (framing.payload_quadwords_remaining != 0) {
        --framing.payload_quadwords_remaining;
        if (framing.payload_quadwords_remaining == 0) {
            framing.at_eop_boundary = framing.completing_tag_eop;
        }
        return;
    }
    // A tag quadword starts a packet: NLOOP, FLG, NREG (0 means 16) and EOP.
    const auto tag = static_cast<unsigned long long>(quadword[0]) |
                     (static_cast<unsigned long long>(quadword[1]) << 32U);
    const unsigned int loops = quadword[0] & 0x7FFFU;
    const unsigned int format = static_cast<unsigned int>(tag >> 58U) & 3U;
    unsigned int registers = static_cast<unsigned int>(tag >> 60U);
    if (registers == 0) {
        registers = 16U;
    }
    std::uint32_t payload_quadwords = 0U;
    if (format == 0U) {
        payload_quadwords = loops * registers;
    } else if (format == 1U) {
        payload_quadwords = (loops * registers + 1U) >> 1U;
    } else {
        payload_quadwords = loops;
    }
    framing.completing_tag_eop = (tag & 0x8000U) != 0U;
    framing.payload_quadwords_remaining = payload_quadwords;
    framing.at_eop_boundary = payload_quadwords == 0U && (tag & 0x8000U) != 0U;
}

} // namespace takedown::platform

namespace takedown::platform {

void ParallelGsRawStreamWriter::append_quadword_slow(const guest::GifPath path, const guest::GsQuadword& quadword) {
    require_open();
    throw_if_unsupported_reset();
    const auto path_index = static_cast<std::size_t>(gif_path_index(path));
    write_reset_defaults(path_index);
    if (open_path_ >= 0 && (open_path_ != static_cast<int>(path_index) || cursor_ == limit_)) {
        flush_transfer();
    }
    if (open_path_ < 0) {
        begin_transfer(path_index);
    }
    std::memcpy(cursor_, &quadword, sizeof quadword);
    cursor_ += sizeof quadword;
    track_gif_quadword(path_index, quadword);
    submitted_quadword_ = true;
}

void ParallelGsRawStreamWriter::write_gif_quadword(const guest::GifPath path, const guest::GsQuadword& quadword) {
    if (open_path_ == static_cast<int>(path) && cursor_ != limit_) [[likely]] {
        std::memcpy(cursor_, &quadword, sizeof quadword);
        cursor_ += sizeof quadword;
        track_gif_quadword(static_cast<std::size_t>(path), quadword);
    } else {
        append_quadword_slow(path, quadword);
    }
}

void ParallelGsRawStreamWriter::write_gif_quadwords(const std::span<const ParallelGsStreamQuadword> quadwords) {
    for (const ParallelGsStreamQuadword& item : quadwords) {
        write_gif_quadword(item.path, item.quadword);
    }
}

void ParallelGsRawStreamWriter::on_gs_gif_quadword(const guest::GifPath path, const guest::GsQuadword& quadword) {
    write_gif_quadword(path, quadword);
}

void ParallelGsRawStreamWriter::on_gs_gif_quadwords(const guest::GifPath path,
                                                    std::span<const guest::GsQuadword> quadwords) {
    while (!quadwords.empty()) {
        if (open_path_ != static_cast<int>(path) || cursor_ == limit_) {
            append_quadword_slow(path, quadwords.front());
            quadwords = quadwords.subspan(1U);
            continue;
        }
        const std::size_t count =
            std::min(static_cast<std::size_t>(limit_ - cursor_) / sizeof(guest::GsQuadword), quadwords.size());
        std::memcpy(cursor_, quadwords.data(), count * sizeof(guest::GsQuadword));
        cursor_ += count * sizeof(guest::GsQuadword);
        const auto path_index = static_cast<std::size_t>(path);
        std::size_t index = 0U;
        while (index < count) {
            GifPathFraming& framing = gif_path_framing_[path_index];
            if (framing.payload_quadwords_remaining != 0U) {
                const std::size_t skip =
                    std::min(static_cast<std::size_t>(framing.payload_quadwords_remaining), count - index);
                framing.payload_quadwords_remaining -= static_cast<std::uint32_t>(skip);
                index += skip;
                if (framing.payload_quadwords_remaining == 0U) {
                    framing.at_eop_boundary = framing.completing_tag_eop;
                }
            } else {
                track_gif_quadword(path_index, quadwords[index]);
                ++index;
            }
        }
        quadwords = quadwords.subspan(count);
    }
}

void ParallelGsRawStreamWriter::on_gs_gif_reset() noexcept {
    if (!submitted_quadword_) {
        return;
    }
    if (gif_paths_at_eop_boundaries()) {
        gif_path_framing_ = {};
        return;
    }
    unsupported_reset_seen_ = true;
    limit_ = cursor_;
}

} // namespace takedown::platform

namespace takedown::platform {

void ParallelGsRawStreamWriter::write_reset_defaults(const std::size_t path_index) {
    if (reset_defaults_emitted_) {
        return;
    }
    reset_defaults_emitted_ = true;
    begin_transfer(path_index);
    // Two quadwords: A+D tag for PRMODECONT, then its value 1.
    const std::uint64_t reset_tag[2] = {0x1000000000008001ULL, 0xEULL};
    const std::uint64_t reset_data[2] = {0x1ULL, 0x1AULL};
    std::memcpy(cursor_, reset_tag, sizeof reset_tag);
    std::memcpy(cursor_ + sizeof reset_tag, reset_data, sizeof reset_data);
    cursor_ += sizeof reset_tag + sizeof reset_data;
    open_transfer_is_reset_ = true;
    flush_transfer();
    open_transfer_is_reset_ = false;
}

void ParallelGsRawStreamWriter::flush_transfer() {
    if (open_path_ < 0) {
        return;
    }
    const unsigned int payload_bytes =
        static_cast<unsigned int>(cursor_ - header_) - static_cast<unsigned int>(kTransferHeaderBytes);
    open_path_ = -1;
    if (payload_bytes != 0U) {
        store_u32_le(header_ + 2, payload_bytes);
        const std::size_t packet_bytes = static_cast<std::size_t>(payload_bytes) + kTransferHeaderBytes;
        const auto quadwords = (packet_bytes - kTransferHeaderBytes) >> 4U;
        ++statistics_.transfer_packets_encoded;
        statistics_.transfer_quadwords_encoded += quadwords;
        statistics_.guest_quadwords_submitted += open_transfer_is_reset_ ? 0U : quadwords;
        statistics_.payload_copy_bytes += payload_bytes;
        statistics_.encoded_bytes += packet_bytes;
        segment_->size += packet_bytes;
        if (track_pieces_) {
            segment_->pieces.push_back({static_cast<std::uint32_t>(packet_bytes), 0U});
        }
    }
    limit_ = nullptr;
    cursor_ = nullptr;
    header_ = nullptr;
}

} // namespace takedown::platform

namespace takedown::platform {

void ParallelGsRawStreamWriter::flush() {
    require_open();
    if (g_native_cmd_hooks != nullptr) {
        g_native_cmd_hooks->flush_held();
    }
    flush_transfer();
    publish_segment();
    transport_->drain();
}

void ParallelGsRawStreamWriter::close() {
    if (closed_) {
        return;
    }
    std::exception_ptr pending_error;
    try {
        if (g_native_cmd_hooks != nullptr) {
            g_native_cmd_hooks->flush_held();
        }
        flush_transfer();
        publish_segment();
    } catch (...) {
        pending_error = std::current_exception();
    }
    closed_ = true;
    open_path_ = -1;
    header_ = nullptr;
    limit_ = nullptr;
    cursor_ = nullptr;
    try {
        transport_->close();
    } catch (...) {
        if (pending_error == nullptr) {
            pending_error = std::current_exception();
        }
    }
    if (pending_error != nullptr) {
        std::rethrow_exception(pending_error);
    }
}

ParallelGsStreamStatistics ParallelGsRawStreamWriter::statistics() const {
    ParallelGsStreamStatistics result = statistics_;
    if (open_path_ >= 0 && !open_transfer_is_reset_) {
        result.guest_quadwords_submitted += static_cast<std::uint64_t>(cursor_ - header_ - kTransferHeaderBytes) >> 4U;
    }
    sink_->copy_counters(result);
    const GsStreamTransportStatistics transport = transport_->statistics();
    result.segments_published = transport.segments_published;
    result.oversize_segments = transport.oversize_segments;
    result.producer_wait_operations = transport.producer_waits;
    result.producer_wait_nanoseconds = transport.producer_wait_nanoseconds;
    return result;
}

} // namespace takedown::platform

namespace takedown::platform {

ParallelGsRawStreamWriter::~ParallelGsRawStreamWriter() {
    if (closed_ || transport_ == nullptr) {
        return;
    }
    try {
        close();
    } catch (...) {
    }
}

void ParallelGsRawStreamWriter::append_packet(const void* const data, const std::size_t size,
                                              const std::uint32_t piece_flags) {
    if (size == 0U) {
        return;
    }
    if (g_native_cmd_hooks != nullptr) {
        g_native_cmd_hooks->flush_held();
    }
    flush_transfer();
    if (size > transport_->segment_bytes()) {
        publish_segment();
        GsStreamSegment& oversize = GsStreamTransport::make_oversize(size);
        std::memcpy(oversize.data(), data, size);
        oversize.size = size;
        if (track_pieces_) {
            oversize.pieces.push_back({static_cast<std::uint32_t>(size), piece_flags});
        }
        transport_->publish(oversize);
        return;
    }
    ensure_segment_space(size);
    std::memcpy(segment_->data() + segment_->size, data, size);
    segment_->size += size;
    if (track_pieces_) {
        segment_->pieces.push_back({static_cast<std::uint32_t>(size), piece_flags});
    }
}

void ParallelGsRawStreamWriter::write_native_model(const TakedownNative::ModelPublication& model) {
    require_open();
    if (unsupported_reset_seen_ || !gif_paths_at_eop_boundaries()) {
        throw std::runtime_error("native model publication requires an ordered complete packet boundary");
    }
    if (native_model_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        model.sequence != native_model_sequence_ + 1U) {
        throw std::invalid_argument("out-of-order native model publication");
    }
    const std::vector<std::uint8_t> payload = TakedownNative::encode_model_publication(model);
    std::array<std::uint8_t, 5U> header{4U};
    for (unsigned int i = 0U; i != 4U; ++i) {
        header[i + 1U] = static_cast<std::uint8_t>(payload.size() >> (i * 8U));
    }
    append_packet(header.data(), header.size(), 0U);
    append_packet(payload.data(), payload.size(), 0U);
    native_model_sequence_ = model.sequence;
    ++statistics_.native_models_encoded;
    statistics_.native_texture_definitions_encoded += model.definitions.size();
    for (const TakedownNative::ModelPass& pass : model.passes) {
        for (const TakedownNative::Draw& draw : pass.draws) {
            statistics_.native_triangles_encoded += draw.triangles.size();
        }
    }
    statistics_.encoded_bytes += payload.size() + header.size();
}

void ParallelGsRawStreamWriter::write_native_cmd(const std::span<const std::uint8_t> payload) {
    require_open();
    if (payload.empty()) {
        return;
    }
    std::array<std::uint8_t, 5U> header{8U};
    for (unsigned int i = 0U; i != 4U; ++i) {
        header[i + 1U] = static_cast<std::uint8_t>(payload.size() >> (i * 8U));
    }
    append_packet(header.data(), header.size(), 0U);
    append_packet(payload.data(), payload.size(), 0U);
    statistics_.encoded_bytes += payload.size() + header.size();
}

void ParallelGsRawStreamWriter::write_vblank(const std::uint8_t phase,
                                             const guest::GsPrivilegedDisplayState& display_state,
                                             const ParallelGsCrtcHint crt_hint) {
    require_open();
    if (phase > 1U) {
        throw std::invalid_argument("paraLLEl-GS Vsync phase must be 0 or 1");
    }
    throw_if_unsupported_reset();
    if (g_native_cmd_hooks != nullptr) {
        g_native_cmd_hooks->on_vblank();
    }
    flush_transfer();
    write_pending_host_presentation();
    write_pending_host_ui();
    const auto registers = [&] {
        std::array<std::uint8_t, 0x2000U> snapshot{};
        fill_privileged_registers(snapshot, display_state, crt_hint);
        return snapshot;
    }();
    // PrivRegisters followed by Vsync, as one self-contained packet.
    std::array<std::uint8_t, 8195U> packet{};
    packet[0] = 3U;
    std::copy(registers.begin(), registers.end(), packet.begin() + 1);
    packet[8193] = 1U;
    packet[8194] = phase;
    ++statistics_.vblank_boundaries_encoded;
    statistics_.encoded_bytes += packet.size();
    append_packet(packet.data(), packet.size(), GsStreamPiece::kVblank);
    ++segment_->frames;
    publish_segment();
    transport_->wait_for_frame_budget();
}

void ParallelGsRawStreamWriter::write_panini_warp(const std::uint16_t tangent, const std::uint8_t strength,
                                                  const std::uint16_t extent, const bool widescreen) {
    require_open();
    const std::array<std::uint8_t, 11U> packet{
        6U, 6U, 0U, 0U, 0U,
        static_cast<std::uint8_t>(tangent), static_cast<std::uint8_t>(tangent >> 8U), strength,
        static_cast<std::uint8_t>(widescreen ? 1U : 0U),
        static_cast<std::uint8_t>(extent), static_cast<std::uint8_t>(extent >> 8U)};
    append_packet(packet.data(), packet.size(), GsStreamPiece::kHostOnly);
    statistics_.encoded_bytes += packet.size();
}

namespace {

// FSR state of the host presentation record (set_parallel_gs_upscaling): the
// value the runtime wants, packed, and what was last latched and sent.
std::atomic<std::uint16_t> g_wanted_upscaling{0U};
ParallelGsUpscaling g_pending_upscaling{};
ParallelGsUpscaling g_sent_upscaling{};

ParallelGsUpscaling wanted_upscaling() noexcept {
    const std::uint16_t packed = g_wanted_upscaling.load(std::memory_order_acquire);
    ParallelGsUpscaling upscaling{};
    upscaling.fsr_preset = static_cast<std::uint8_t>(packed & 0xffU);
    upscaling.sharpness = static_cast<std::uint8_t>(packed >> 8U);
    return upscaling;
}

} // namespace

void set_parallel_gs_upscaling(const ParallelGsUpscaling& upscaling) noexcept {
    g_wanted_upscaling.store(static_cast<std::uint16_t>(upscaling.fsr_preset | (upscaling.sharpness << 8U)),
                             std::memory_order_release);
}

void ParallelGsRawStreamWriter::set_host_presentation(const ParallelGsHostPresentation& presentation) {
    const ParallelGsUpscaling upscaling = wanted_upscaling();
    if (presentation == sent_host_presentation_ && upscaling == g_sent_upscaling) {
        pending_host_presentation_.reset();
        return;
    }
    pending_host_presentation_ = presentation;
    g_pending_upscaling = upscaling;
}

void ParallelGsRawStreamWriter::set_host_ui(const ParallelGsHostUi& ui) {
    if (ui == sent_host_ui_) {
        pending_host_ui_.reset();
        return;
    }
    pending_host_ui_ = ui;
}

void ParallelGsRawStreamWriter::write_pending_host_presentation() {
    if (!pending_host_presentation_) {
        return;
    }
    const std::vector<std::uint8_t> payload =
        encode_parallel_gs_host_presentation(*pending_host_presentation_, g_pending_upscaling);
    std::array<std::uint8_t, 5U> header{kParallelGsHostPresentationPacket};
    for (unsigned int index = 0U; index != 4U; ++index) {
        header[index + 1U] = static_cast<std::uint8_t>(payload.size() >> (index * 8U));
    }
    append_packet(header.data(), header.size(), GsStreamPiece::kHostOnly);
    append_packet(payload.data(), payload.size(), GsStreamPiece::kHostOnly);
    statistics_.encoded_bytes += payload.size() + header.size();
    sent_host_presentation_ = std::move(*pending_host_presentation_);
    g_sent_upscaling = g_pending_upscaling;
    pending_host_presentation_.reset();
}

void ParallelGsRawStreamWriter::write_pending_host_ui() {
    if (!pending_host_ui_) {
        return;
    }
    const std::vector<std::uint8_t> payload = encode_parallel_gs_host_ui(*pending_host_ui_);
    std::array<std::uint8_t, 5U> header{kParallelGsHostUiPacket};
    for (unsigned int index = 0U; index != 4U; ++index) {
        header[index + 1U] = static_cast<std::uint8_t>(payload.size() >> (index * 8U));
    }
    append_packet(header.data(), header.size(), GsStreamPiece::kHostOnly);
    append_packet(payload.data(), payload.size(), GsStreamPiece::kHostOnly);
    statistics_.encoded_bytes += payload.size() + header.size();
    sent_host_ui_ = std::move(*pending_host_ui_);
    pending_host_ui_.reset();
}

void write_parallel_gs_raw_stream(const std::filesystem::path& output_path,
                                  const std::span<const ParallelGsStreamQuadword> quadwords, const std::uint8_t phase,
                                  const guest::GsPrivilegedDisplayState& display_state,
                                  const ParallelGsCrtcHint crt_hint) {
    ParallelGsRawStreamWriter writer(output_path, ParallelGsStreamOpenMode::ExclusiveFile);
    writer.write_gif_quadwords(quadwords);
    writer.write_vblank(phase, display_state, crt_hint);
    writer.close();
}

} // namespace takedown::platform

namespace takedown::platform {

ParallelGsRawStreamWriter::ParallelGsRawStreamWriter(const std::filesystem::path& output_path,
                                                     const ParallelGsStreamOpenMode open_mode,
                                                     std::optional<std::filesystem::path> live_capture_path) {
    // A new stream starts from the renderer's defaults, FSR off included.
    g_sent_upscaling = {};
    const bool live_fifo = open_mode == ParallelGsStreamOpenMode::ExistingFifo;
    if (live_capture_path && !live_fifo) {
        throw std::invalid_argument("a paraLLEl-GS live capture requires ExistingFifo output mode");
    }
    if (live_capture_path && live_capture_path->empty()) {
        throw std::invalid_argument("paraLLEl-GS live capture requires a non-empty file path");
    }

    FILE* mirror = nullptr;
    if (live_capture_path) {
        mirror = create_exclusive_file(*live_capture_path);
        if (mirror == nullptr) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(),
                                    "could not exclusively create paraLLEl-GS live capture " +
                                        live_capture_path->string());
        }
    }

    const auto discard_mirror = [&mirror, &live_capture_path]() {
        if (mirror != nullptr) {
            std::fclose(mirror);
            mirror = nullptr;
            std::error_code ignored;
            std::filesystem::remove(*live_capture_path, ignored);
        }
    };

    FILE* output = nullptr;
    try {
        output = live_fifo ? windows_gs_pipe::open_stream_writer(output_path.c_str())
                           : create_exclusive_file(output_path);
    } catch (...) {
        discard_mirror();
        throw;
    }
    if (output == nullptr) {
        const int error = errno;
        discard_mirror();
        throw std::system_error(error, std::generic_category(),
                                "could not exclusively create paraLLEl-GS stream " + output_path.string());
    }
    track_pieces_ = mirror != nullptr;

    auto sink = std::make_unique<ParallelGsOutputSink>(output, output_path, live_fifo, mirror, std::move(live_capture_path));
    sink_ = sink.get();

    GsStreamTransportConfig transport_config{};
    transport_config.segment_bytes = 262144U;
    transport_config.segment_count = 32U;
    transport_config.max_frames_in_flight = 3U;
    if (const char* budget = std::getenv("TAKEDOWN_GS_FRAMES_IN_FLIGHT"); budget != nullptr && *budget != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(budget, &end, 10);
        if (end != budget && *end == '\0' && parsed >= 1UL && parsed <= 16UL) {
            transport_config.max_frames_in_flight = static_cast<std::uint32_t>(parsed);
        }
    }
    transport_ = std::make_unique<GsStreamTransport>(std::move(sink), transport_config);
    if (transport_->segment_bytes() < kTransferHeaderBytes + kTransferChunkBytes) {
        throw std::logic_error("GS stream segment too small for a transfer packet");
    }
}

void ParallelGsRawStreamWriter::record_digest(const std::filesystem::path& path) {
    if (digest_attached_) {
        throw std::logic_error("paraLLEl-GS stream digest is already being recorded");
    }
    if (std::filesystem::exists(path)) {
        throw std::runtime_error("paraLLEl-GS stream digest already exists: " + path.string());
    }
    FILE* log = std::fopen(path.string().c_str(), "w");
    if (log == nullptr) {
        throw std::system_error(errno, std::generic_category(),
                                "could not create paraLLEl-GS stream digest " + path.string());
    }
    try {
        flush();
    } catch (...) {
        std::fclose(log);
        throw;
    }
    sink_->attach_digest_log(log, statistics_.vblank_boundaries_encoded);
    digest_attached_ = true;
    track_pieces_ = true;
}

} // namespace takedown::platform
