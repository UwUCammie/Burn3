#include "takedown/platform/gs_semantic_trace.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <span>
#include <vector>

namespace takedown::platform {

namespace {

constexpr std::uint64_t kUint64Max = ~std::uint64_t{0};

void saturating_increment(std::uint64_t& counter) noexcept {
    if (counter != kUint64Max) {
        ++counter;
    }
}

void saturating_add(std::uint64_t& value, const std::uint64_t amount) noexcept {
    if (amount > kUint64Max - value) {
        value = kUint64Max;
    } else {
        value += amount;
    }
}

[[nodiscard]] std::uint64_t fnv1a_hash(const std::vector<std::uint8_t>& bytes) noexcept {
    auto hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::uint64_t vu0_micro_memory_hash(const guest::EeMemory& memory) {
    const std::span<const std::byte> bytes = memory.view(0x11000000U, 4096U);
    auto hash = 14695981039346656037ULL;
    for (const std::byte byte : bytes) {
        hash ^= std::to_integer<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::uint64_t quadword_low(const guest::GsQuadword& quadword) noexcept {
    return static_cast<std::uint64_t>(quadword[0]) |
           (static_cast<std::uint64_t>(quadword[1]) << 32U);
}

[[nodiscard]] std::uint64_t quadword_high(const guest::GsQuadword& quadword) noexcept {
    return static_cast<std::uint64_t>(quadword[2]) |
           (static_cast<std::uint64_t>(quadword[3]) << 32U);
}

[[nodiscard]] const char* source_name(const guest::GsRegisterWriteSource source) noexcept {
    switch (source) {
    case guest::GsRegisterWriteSource::GifTagPrim:
        return "tag-prim";
    case guest::GsRegisterWriteSource::Packed:
        return "packed";
    case guest::GsRegisterWriteSource::Reglist:
        return "reglist";
    case guest::GsRegisterWriteSource::Ad:
        return "ad";
    }
    return "unknown";
}

[[nodiscard]] const char* stop_name(const guest::DmacSourceStop stop) noexcept {
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

[[nodiscard]] std::uint64_t upper_half(const std::uint64_t value) noexcept {
    return value >> 32U;
}

// PACKED register encodings, canonicalised the way the renderer feeds them to
// the primitive assembler.
[[nodiscard]] std::uint64_t canonical_packed_rgba(const std::uint64_t low, const std::uint64_t high,
                                                  const std::uint32_t q_bits) noexcept {
    const auto red = static_cast<unsigned int>(low) & 0xffU;
    const auto green = static_cast<unsigned int>(low >> 32U) & 0xffU;
    const auto blue = static_cast<unsigned int>(high) & 0xffU;
    const auto alpha = static_cast<unsigned int>(high >> 32U) & 0xffU;
    return static_cast<std::uint64_t>(red) | (static_cast<std::uint64_t>(green) << 8U) |
           (static_cast<std::uint64_t>(blue) << 16U) | (static_cast<std::uint64_t>(alpha) << 24U) |
           (static_cast<std::uint64_t>(q_bits) << 32U);
}

[[nodiscard]] std::uint64_t canonical_packed_uv(const std::uint64_t low) noexcept {
    const auto u = static_cast<unsigned int>(low) & 16383U;
    const auto v = static_cast<unsigned int>(low >> 32U) & 16383U;
    return static_cast<std::uint64_t>(u) | (static_cast<std::uint64_t>(v) << 16U);
}

[[nodiscard]] std::uint64_t canonical_packed_position(const std::uint8_t address,
                                                      const std::uint64_t low,
                                                      const std::uint64_t high) noexcept {
    const auto y_bits = upper_half(low);
    const auto word2 = static_cast<unsigned int>(high);
    const auto word3 = static_cast<unsigned int>(high >> 32U);
    const auto x = low & 0xffffU;
    const auto y = (y_bits << 16U) & 0xffffffffU;
    if (address == 12U) {
        const auto z = (static_cast<std::uint64_t>(word2 >> 4U) << 32U) & 0x00ffffff00000000U;
        const auto fog = static_cast<std::uint64_t>(word3 >> 4U) << 56U;
        return x | y | z | fog;
    }
    return x | y | (high << 32U);
}

} // namespace

void GsSemanticTraceWriter::on_gs_image_transfer(const guest::GsImageTransfer& transfer) {
    saturating_increment(statistics_.image_transfers_observed);
    if (!begin_semantic_event()) {
        return;
    }
    check_write(std::fprintf(
        event_output(),
        "%012llu IMAGE cmdseq=%llu path=%u bytes=%llu hash=%016llx dbp=%04x dbw=%02x "
        "dpsm=%02x dsax=%03x dsay=%03x width=%u height=%u bitbltbuf=%016llx "
        "trxpos=%016llx trxreg=%016llx trxdir=%016llx xgkick=%llu\n",
        event_number(), transfer.sequence,
        static_cast<unsigned int>(transfer.path) + 1U,
        static_cast<std::uint64_t>(transfer.pixels.size()), fnv1a_hash(transfer.pixels),
        transfer.destination_base_pointer, transfer.destination_buffer_width,
        transfer.destination_psm, transfer.destination_x, transfer.destination_y,
        transfer.width, transfer.height, bitbltbuf_, trxpos_, trxreg_, trxdir_,
        transfer.path == guest::GifPath::Path1 ? current_path1_xgkick_sequence_ : 0U));
}

bool GsSemanticTraceWriter::begin_event() {
    if (!active_) {
        saturating_increment(statistics_.events_filtered);
        return false;
    }
    if (config_.retain_last_nonempty_completed_vblank) {
        if (interval_events_written_ >= config_.maximum_events) {
            saturating_increment(interval_events_omitted_);
            interval_limit_reached_ = true;
            return false;
        }
        ++interval_events_written_;
        return true;
    }
    if (statistics_.events_written >= config_.maximum_events) {
        saturating_increment(statistics_.events_omitted);
        statistics_.event_limit_reached = true;
        return false;
    }
    ++statistics_.events_written;
    return true;
}

// RECOVERY: the original GIMPLE keeps an extra <L3> switch label here that this source does not reproduce.
void GsSemanticTraceWriter::on_gs_gif_quadword(const guest::GifPath path,
                                               const guest::GsQuadword& quadword) {
    current_path1_xgkick_sequence_ = associate_path1_quadword(path);
    RawPathState& raw_path = raw_paths_[static_cast<std::size_t>(path)];
    if (raw_path.payload_quadwords_remaining != 0U) {
        --raw_path.payload_quadwords_remaining;
        return;
    }
    const std::uint64_t tag = quadword_low(quadword);
    const std::uint64_t regs = quadword_high(quadword);
    const unsigned int loop_count = quadword[0] & 0x7fffU;
    const unsigned int eop = static_cast<unsigned int>(tag >> 15U) & 1U;
    const unsigned int pre = static_cast<unsigned int>(tag >> 46U) & 1U;
    const unsigned int primitive = static_cast<unsigned int>(tag >> 47U) & 0x7ffU;
    const unsigned int format = static_cast<unsigned int>(tag >> 58U) & 3U;
    unsigned int register_count = static_cast<unsigned int>(tag >> 60U);
    if (register_count == 0U) {
        register_count = 16U;
    }
    switch (format) {
    case 0U:
    default:
        raw_path.payload_quadwords_remaining =
            static_cast<std::uint64_t>(loop_count) * register_count;
        break;
    case 1U: {
        const auto values = static_cast<std::uint64_t>(loop_count) * register_count;
        raw_path.payload_quadwords_remaining = (values + 1U) / 2U;
        break;
    }
    case 2U:
    case 3U:
        raw_path.payload_quadwords_remaining = loop_count;
        break;
    }
    saturating_increment(statistics_.tags_observed);
    if (begin_semantic_event()) {
        check_write(std::fprintf(
            event_output(),
            "%012llu TAG path=%u nloop=%u eop=%u pre=%u prim=%03x flg=%u nreg=%u "
            "regs=%016llx raw=%016llx xgkick=%llu\n",
            event_number(), static_cast<unsigned int>(path) + 1U, loop_count, eop, pre,
            primitive, format, register_count, regs, tag, current_path1_xgkick_sequence_));
    }
}

std::uint64_t GsSemanticTraceWriter::associate_path1_quadword(const guest::GifPath path) noexcept {
    if (path != guest::GifPath::Path1) {
        return 0U;
    }
    if (!pending_path1_producers_.empty()) {
        PendingPath1Producer& producer = pending_path1_producers_.front();
        const auto sequence = producer.sequence;
        --producer.quadwords_remaining;
        if (producer.quadwords_remaining == 0U) {
            pending_path1_producers_.pop_front();
        }
        return sequence;
    }
    if (active_xgkick_sequence_ != 0U) {
        ++active_xgkick_path1_quadwords_observed_;
        return active_xgkick_sequence_;
    }
    return 0U;
}

namespace {

struct FilePositionGuard {
    std::FILE* file_{};
    std::fpos_t position_{};

    FilePositionGuard(std::FILE* const file, const std::fpos_t position)
        : file_(file), position_(position) {}

    int restore() {
        std::clearerr(file_);
        return std::fsetpos(file_, std::addressof(position_));
    }

    ~FilePositionGuard() {
        if (file_ != nullptr) {
            restore();
        }
    }
};

[[nodiscard]] bool write_vu0_cull_state(std::FILE* const output,
                                        const guest::R5900State& state) noexcept;

[[nodiscard]] std::uint64_t gpr_low_half(const guest::R5900State& state,
                                         const std::size_t index) {
    const guest::Gpr128 value = state.read_gpr(index);
    return static_cast<std::uint64_t>(value.words[0]) |
           (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

// Everything the VU0 microprogram can observe at launch, one line per call.
[[nodiscard]] bool write_vu0_cull_state(std::FILE* const output,
                                        const guest::R5900State& state) noexcept {
    if (std::fprintf(output, " s0=%016llx s1=%016llx s2=%016llx s3=%016llx s4=%016llx vi=",
                     gpr_low_half(state, 16U), gpr_low_half(state, 17U),
                     gpr_low_half(state, 18U), gpr_low_half(state, 19U),
                     gpr_low_half(state, 20U)) < 0) {
        return false;
    }
    for (std::size_t index = 0; index < 16U; ++index) {
        if (std::fprintf(output, "%s%04x", index == 0U ? "" : ",",
                         state.read_vu0_integer(index)) < 0) {
            return false;
        }
    }
    const auto& mac = state.vu0_mac_flag_history();
    if (std::fprintf(output, " mac=%04x,%04x,%04x,%04x", mac[0], mac[1], mac[2], mac[3]) < 0) {
        return false;
    }
    const guest::Gpr128 accumulator = state.vu0_accumulator();
    if (std::fprintf(output, " acc=%08x,%08x,%08x,%08x", accumulator.words[0],
                     accumulator.words[1], accumulator.words[2], accumulator.words[3]) < 0) {
        return false;
    }
    for (std::size_t index = 1; index < 15U; ++index) {
        const guest::Gpr128 vector = state.read_vu0_vector(index);
        if (std::fprintf(output, " vf%02zu=%08x,%08x,%08x,%08x", index, vector.words[0],
                         vector.words[1], vector.words[2], vector.words[3]) < 0) {
            return false;
        }
    }
    return std::fputc('\n', output) != EOF;
}

[[noreturn]] void throw_file_error(const std::filesystem::path& path,
                                   const std::string& operation) {
    const int error = errno != 0 ? errno : EIO;
    throw std::system_error(error, std::generic_category(), operation + " " + path.string());
}

void write_last_footer(std::FILE* output, const std::filesystem::path& path,
                       const GsSemanticTraceStatistics& statistics) {
    const auto retained = statistics.retained_nonempty_vblank.has_value()
                                     ? std::to_string(*statistics.retained_nonempty_vblank)
                                     : std::string{"none"};
    if (std::fprintf(output,
                     "# end events=%llu omitted=%llu filtered=%llu tags=%llu registers=%llu "
                     "images=%llu kicks=%llu resets=%llu last-vblank=%llu "
                     "retained-nonempty-vblank=%s limit=%u\n",
                     statistics.events_written, statistics.events_omitted,
                     statistics.events_filtered, statistics.tags_observed,
                     statistics.register_writes_observed, statistics.image_transfers_observed,
                     statistics.draw_kicks_observed, statistics.resets_observed,
                     statistics.current_vblank, retained.c_str(),
                     statistics.event_limit_reached ? 1U : 0U) < 0) {
        throw_file_error(path, "could not write GS semantic trace");
    }
}

void write_last_header(_iobuf* output, const std::filesystem::path& path,
                       const std::uint64_t maximum_events) {
    if (std::fprintf(output,
                     "# takedown-gs-semantic-v1 max-events=%llu selected-vblank=last "
                     "retention=completed-nonempty\n",
                     maximum_events) < 0) {
        throw_file_error(path, "could not write GS semantic trace");
    }
}

[[nodiscard]] std::FILE* try_create_exclusive(const std::filesystem::path& path) noexcept {
    int descriptor = -1;
    if (_wsopen_s(&descriptor, path.c_str(), 34049, 64, 384) != 0) {
        return nullptr;
    }
    if (descriptor < 0) {
        return nullptr;
    }
    std::FILE* const file = _fdopen(descriptor, "wb");
    if (file == nullptr) {
        _close(descriptor);
    }
    return file;
}

[[nodiscard]] inline std::FILE* require_created(std::FILE* const file,
                                                const std::filesystem::path& path,
                                                const char* const message) {
    if (file == nullptr) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), message + path.string());
    }
    return file;
}

[[nodiscard]] inline std::FILE* open_exclusive(const std::filesystem::path& path,
                                               const char* const message) {
    return require_created(try_create_exclusive(path), path, message);
}

[[nodiscard]] inline std::FILE* open_trace_output(const std::filesystem::path& output_path,
                                                  const GsSemanticTraceConfig& config) {
    if (config.maximum_events == 0U) {
        throw std::invalid_argument("GS semantic trace maximum event count must be nonzero");
    }
    if (config.retain_last_nonempty_completed_vblank && config.selected_vblank.has_value()) {
        throw std::invalid_argument(
            "GS semantic trace cannot select both an exact and the last completed VBlank");
    }
    return open_exclusive(output_path, "could not exclusively create GS semantic trace ");
}

// RECOVERY: the original restores the file position through a differently shaped inlined helper (address temporaries) than this source.
void copy_file_from_start_preserving_position(std::FILE* source,
                                              std::FILE* destination,
                                              const std::filesystem::path& destination_path) {
    if (std::fflush(source) != 0) {
        throw_file_error(destination_path, "could not flush temporary GS semantic trace interval");
    }
    std::fpos_t original_position{};
    if (std::fgetpos(source, &original_position) != 0) {
        throw_file_error(destination_path,
                         "could not query temporary GS semantic trace interval position");
    }
    FilePositionGuard position_guard{source, original_position};
    std::clearerr(source);
    if (std::fseek(source, 0, SEEK_SET) != 0) {
        throw_file_error(destination_path, "could not rewind temporary GS semantic trace interval");
    }
    std::array<char, 65536> buffer{};
    for (;;) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), source);
        if (count != 0U && std::fwrite(buffer.data(), 1, count, destination) != count) {
            throw_file_error(destination_path,
                             "could not copy retained GS semantic trace interval");
        }
        if (count != buffer.size()) {
            if (std::ferror(source) != 0) {
                throw_file_error(destination_path,
                                 "could not read retained GS semantic trace interval");
            }
            break;
        }
    }
    if (position_guard.restore() != 0) {
        throw_file_error(destination_path,
                         "could not restore temporary GS semantic trace interval");
    }
    position_guard.file_ = nullptr;
}

} // namespace

GsSemanticTraceWriter::GsSemanticTraceWriter(const std::filesystem::path& output_path,
                                             const GsSemanticTraceConfig config)
    : output_(open_trace_output(output_path, config)),
      output_path_(output_path), config_(config) {
    try {
        active_ = selected_interval(0U);
        footer_written_ = false;
        if (config_.retain_last_nonempty_completed_vblank) {
            begin_last_interval(0U);
        }
        write_header();
        if (active_ && begin_event()) {
            check_write(std::fprintf(event_output(), "%012llu VBLANK index=0 phase=begin\n",
                                     event_number()));
        }
    } catch (...) {
        close_temporary_files();
        std::fclose(output_);
        output_ = nullptr;
        std::error_code ignored;
        std::filesystem::remove(output_path_, ignored);
        throw;
    }
}

GsSemanticTraceWriter::~GsSemanticTraceWriter() {
    if (output_ == nullptr) {
        close_temporary_files();
        return;
    }
    try {
        close();
    } catch (...) {
        if (output_ != nullptr) {
            std::fclose(output_);
            output_ = nullptr;
        }
        close_temporary_files();
    }
}

void GsSemanticTraceWriter::close_temporary_files() noexcept {
    if (current_interval_output_ != nullptr) {
        std::fclose(current_interval_output_);
        current_interval_output_ = nullptr;
    }
    if (completed_interval_output_ != nullptr) {
        std::fclose(completed_interval_output_);
        completed_interval_output_ = nullptr;
    }
}

void GsSemanticTraceWriter::on_gs_gif_reset() noexcept {
    raw_paths_ = {};
    pending_path1_producers_.clear();
    active_xgkick_path1_quadwords_observed_ = 0U;
    current_path1_xgkick_sequence_ = 0U;
}

void GsSemanticTraceWriter::on_gs_gif_tag_begin(const guest::GifPath path) {
    static_cast<void>(path);
    packed_q_bits_ = 0x3f800000U;
}

void GsSemanticTraceWriter::on_gs_reset() {
    reset_drawing_state();
    on_gs_gif_reset();
    saturating_increment(statistics_.resets_observed);
    if (begin_semantic_event()) {
        check_write(std::fprintf(event_output(), "%012llu RESET kind=gs\n", event_number()));
    }
}

// RECOVERY: the original GIMPLE contains a dead address + 252 statement that this source does not reproduce.
void GsSemanticTraceWriter::on_gs_register_write(const guest::GsRegisterWrite& write) {
    switch (write.address) {
    case 0x50U:
        bitbltbuf_ = write.value;
        break;
    case 0x51U:
        trxpos_ = write.value;
        break;
    case 0x52U:
        trxreg_ = write.value;
        break;
    case 0x53U:
        trxdir_ = write.value;
        break;
    default:
        break;
    }
    const auto kick = forward_register_write(write);
    saturating_increment(statistics_.register_writes_observed);
    if (begin_semantic_event()) {
        check_write(std::fprintf(
            event_output(),
            "%012llu REG cmdseq=%llu path=%u source=%s addr=%02x value=%016llx high=%016llx "
            "xgkick=%llu\n",
            event_number(), write.sequence, static_cast<unsigned int>(write.path) + 1U,
            source_name(write.source), static_cast<unsigned int>(write.address), write.value,
            write.packed_high, write.path == guest::GifPath::Path1 ? current_path1_xgkick_sequence_ : 0U));
    }
    if (kick.has_value()) {
        saturating_increment(statistics_.draw_kicks_observed);
        write_draw_kick(write, *kick);
    }
}

// RECOVERY: the original GIMPLE contains a dead address + 252 statement that this source does not reproduce.
std::optional<guest::GsPrimitiveKickSnapshot>
GsSemanticTraceWriter::forward_register_write(const guest::GsRegisterWrite& write) noexcept {
    if (write.source != guest::GsRegisterWriteSource::Packed) {
        return assembler_.write(guest::GsAssemblerRegisterWrite{
            .address = write.address,
            .value = write.value,
        });
    }

    switch (static_cast<guest::GsRegisterAddress>(write.address)) {
    case guest::GsRegisterAddress::Xyzf2:
    case guest::GsRegisterAddress::Xyz2:
        return assembler_.write_packed_position(guest::GsPackedPositionWrite{
            .address = static_cast<guest::GsRegisterAddress>(write.address),
            .payload = {.low = write.value, .high = write.packed_high},
        });
    case guest::GsRegisterAddress::Xyzf3:
    case guest::GsRegisterAddress::Xyz3:
        return assembler_.write(guest::GsAssemblerRegisterWrite{
            .address = write.address,
            .value = canonical_packed_position(write.address, write.value, write.packed_high),
        });
    case guest::GsRegisterAddress::Rgbaq:
        return assembler_.write(guest::GsRegisterAddress::Rgbaq,
                                canonical_packed_rgba(write.value, write.packed_high, packed_q_bits_));
    case guest::GsRegisterAddress::St: {
        packed_q_bits_ = static_cast<std::uint32_t>(write.packed_high);
        const auto current_rgba = assembler_.state().vertex.rgbaq;
        const auto rgba = current_rgba.known ? static_cast<unsigned int>(current_rgba.raw) : 0U;
        static_cast<void>(assembler_.write(
            guest::GsRegisterAddress::Rgbaq,
            static_cast<std::uint64_t>(rgba) | (static_cast<std::uint64_t>(packed_q_bits_) << 32U)));
        return assembler_.write(guest::GsRegisterAddress::St, write.value);
    }
    case guest::GsRegisterAddress::Uv:
        return assembler_.write(guest::GsRegisterAddress::Uv, canonical_packed_uv(write.value));
    default:
        return assembler_.write(guest::GsAssemblerRegisterWrite{
            .address = write.address,
            .value = write.value,
        });
    }
}

bool GsSemanticTraceWriter::on_vu0_micro_call_begin(
    const guest::Vu0MicroCallObservation& observation, const guest::R5900State& state,
    const guest::EeMemory& memory) noexcept {
    try {
        const bool relevant_caller =
            observation.caller_pc == 0x26a1e0U || observation.caller_pc == 0x26a670U;
        if (!relevant_caller) {
            return false;
        }
        if (observation.entry != 0x124U) {
            return false;
        }
        const auto sequence = ++vu0_call_sequence_;
        if (output_ == nullptr || !begin_semantic_event()) {
            return false;
        }
        std::FILE* const output = event_output();
        if (output == nullptr) {
            return false;
        }
        if (std::fprintf(output,
                         "%012llu VU0CALL phase=begin call=%llu caller=%08x entry=%03x "
                         "micro-hash=%016llx",
                         event_number(), sequence, observation.caller_pc,
                         static_cast<unsigned int>(observation.entry),
                         vu0_micro_memory_hash(memory)) < 0) {
            return false;
        }
        if (!write_vu0_cull_state(output, state)) {
            return false;
        }
        active_vu0_call_sequence_ = sequence;
        return true;
    } catch (...) {
    }
    return false;
}

void GsSemanticTraceWriter::on_vu0_micro_call_end(
    const guest::Vu0MicroCallObservation& observation, const guest::R5900State& state,
    const guest::Vu0MicroRunResult& result) noexcept {
    const auto sequence = active_vu0_call_sequence_;
    active_vu0_call_sequence_ = 0U;
    if (sequence == 0U) {
        return;
    }
    try {
        if (output_ == nullptr || !begin_semantic_event()) {
            return;
        }
        const auto status = guest::to_string(result.status);
        std::FILE* const output = event_output();
        if (output != nullptr &&
            std::fprintf(output,
                         "%012llu VU0CALL phase=end call=%llu caller=%08x entry=%03x "
                         "status=%.*s pairs=%llu stop-pc=%03x lower=%08x upper=%08x",
                         event_number(), sequence, observation.caller_pc,
                         static_cast<unsigned int>(observation.entry),
                         static_cast<int>(status.size()), status.data(), result.committed_pairs,
                         static_cast<unsigned int>(result.pc), result.lower_word,
                         result.upper_word) >= 0) {
            static_cast<void>(write_vu0_cull_state(output, state));
        }
    } catch (...) {
    }
}

bool GsSemanticTraceWriter::on_vu1_xgkick_begin(
    const guest::Vu1XgkickObservation& observation) noexcept {
    try {
        const auto sequence = ++xgkick_sequence_;
        active_xgkick_hash_ = 14695981039346656037ULL;
        active_xgkick_quadwords_ = 0U;
        active_xgkick_path1_quadwords_observed_ = 0U;
        active_xgkick_first_quadword_ = {};
        active_xgkick_has_quadword_ = false;
        if (output_ == nullptr || !begin_semantic_event() || event_output() == nullptr) {
            return false;
        }
        if (std::fprintf(
                event_output(),
                "%012llu XGKICK phase=begin kick=%llu vector=%03x entry=%03x resolved-entry=%03x "
                "run=%llu program-hash-known=%u program-hash-high=%016llx "
                "program-hash-low=%016llx vif-source-known=%u vif-source=%08x "
                "chain-root-known=%u chain-root=%08x tag-location-known=%u tag-location=%08x "
                "tag=%08x,%08x,%08x,%08x\n",
                event_number(), sequence,
                static_cast<unsigned int>(observation.vector_address),
                static_cast<unsigned int>(observation.micro_entry),
                static_cast<unsigned int>(observation.micro_resolved_entry),
                observation.micro_run_sequence, observation.micro_program_hash_known ? 1U : 0U,
                observation.micro_program_hash.high, observation.micro_program_hash.low,
                observation.vif_command_source_known ? 1U : 0U, observation.vif_command_source,
                observation.vif_chain_root_known ? 1U : 0U, observation.vif_chain_root,
                observation.vif_tag_location_known ? 1U : 0U, observation.vif_tag_location,
                observation.initial_tag[0], observation.initial_tag[1],
                observation.initial_tag[2], observation.initial_tag[3]) < 0) {
            return false;
        }
        active_xgkick_sequence_ = sequence;
        return true;
    } catch (...) {
        return false;
    }
}

void GsSemanticTraceWriter::on_vu1_xgkick_quadword(const guest::GsQuadword& quadword) noexcept {
    if (active_xgkick_sequence_ == 0U) {
        return;
    }
    if (!active_xgkick_has_quadword_) {
        active_xgkick_first_quadword_ = quadword;
        active_xgkick_has_quadword_ = true;
    }
    for (const unsigned int word : quadword) {
        for (unsigned int byte = 0; byte < 4U; ++byte) {
            active_xgkick_hash_ ^= (word >> (byte * 8U)) & 0xffU;
            active_xgkick_hash_ *= 1099511628211ULL;
        }
    }
    ++active_xgkick_quadwords_;
}

void GsSemanticTraceWriter::on_vu1_xgkick_end(const bool completed) noexcept {
    try {
        const auto sequence = active_xgkick_sequence_;
        active_xgkick_sequence_ = 0U;
        if (sequence == 0U) {
            return;
        }
        if (completed && active_xgkick_quadwords_ > active_xgkick_path1_quadwords_observed_) {
            pending_path1_producers_.emplace_back(PendingPath1Producer{
                sequence, active_xgkick_quadwords_ - active_xgkick_path1_quadwords_observed_});
        }
        if (output_ == nullptr || !begin_semantic_event()) {
            return;
        }
        if (event_output() != nullptr) {
            std::fprintf(event_output(),
                         "%012llu XGKICK phase=end kick=%llu completed=%u qwords=%llu "
                         "hash=%016llx first=%08x,%08x,%08x,%08x\n",
                         event_number(), sequence, completed ? 1U : 0U, active_xgkick_quadwords_,
                         active_xgkick_hash_, active_xgkick_first_quadword_[0],
                         active_xgkick_first_quadword_[1], active_xgkick_first_quadword_[2],
                         active_xgkick_first_quadword_[3]);
        }
    } catch (...) {
    }
}

bool GsSemanticTraceWriter::on_vif1_dmac_chain_begin(
    const guest::Vif1DmacChainObservation& observation) noexcept {
    try {
        const auto sequence = ++vif1_chain_sequence_;
        if (output_ == nullptr || !begin_semantic_event() || event_output() == nullptr) {
            return false;
        }
        if (std::fprintf(event_output(),
                         "%012llu VIF1CHAIN phase=begin chain=%llu root=%08x start-pc=%08x\n",
                         event_number(), sequence, observation.chain_root,
                         observation.start_pc) < 0) {
            return false;
        }
        active_vif1_chain_sequence_ = sequence;
        return true;
    } catch (...) {
        return false;
    }
}

void GsSemanticTraceWriter::on_vif1_dmac_chain_tag(
    const guest::Vif1DmacTagObservation& observation) noexcept {
    try {
        const auto sequence = active_vif1_chain_sequence_;
        if (sequence == 0U || output_ == nullptr || !begin_semantic_event()) {
            return;
        }
        const auto& tag = observation.tag;
        if (event_output() != nullptr) {
            std::fprintf(
                event_output(),
                "%012llu VIF1TAG chain=%llu root=%08x ordinal=%u location=%08x id=%u qwc=%u "
                "payload=%08x writer-pc-known=%u writer-pc=%08x tag=%08x,%08x,%08x,%08x\n",
                event_number(), sequence, observation.chain_root, observation.ordinal,
                tag.location.value, tag.id, tag.qwc, tag.payload.value,
                observation.last_writer_pc_known ? 1U : 0U, observation.last_writer_pc,
                tag.words[0], tag.words[1], tag.words[2], tag.words[3]);
        }
    } catch (...) {
    }
}

void GsSemanticTraceWriter::on_vif1_dmac_chain_end(
    const guest::Vif1DmacChainResultObservation& observation) noexcept {
    try {
        const auto sequence = active_vif1_chain_sequence_;
        active_vif1_chain_sequence_ = 0U;
        if (sequence == 0U || output_ == nullptr || !begin_semantic_event()) {
            return;
        }
        if (event_output() != nullptr) {
            std::fprintf(event_output(),
                         "%012llu VIF1CHAIN phase=end chain=%llu root=%08x stop=%s tags=%llu "
                         "payload-qwords=%llu\n",
                         event_number(), sequence, observation.chain_root,
                         stop_name(observation.stop),
                         static_cast<std::uint64_t>(observation.tags_processed),
                         static_cast<std::uint64_t>(observation.payload_quadwords));
        }
    } catch (...) {
    }
}

void GsSemanticTraceWriter::note_vblank(const std::uint64_t vblank_index) {
    if (statistics_.current_vblank >= vblank_index) {
        throw std::invalid_argument("GS semantic trace VBlank indices must increase");
    }
    if (active_ && begin_event()) {
        check_write(std::fprintf(event_output(), "%012llu VBLANK index=%llu phase=end\n",
                                 event_number(), statistics_.current_vblank));
    }
    if (config_.retain_last_nonempty_completed_vblank) {
        complete_last_interval();
    }
    statistics_.current_vblank = vblank_index;
    active_ = selected_interval(vblank_index);
    if (config_.retain_last_nonempty_completed_vblank) {
        begin_last_interval(vblank_index);
    }
    if (active_ && begin_event()) {
        check_write(std::fprintf(event_output(), "%012llu VBLANK index=%llu phase=begin\n",
                                 event_number(), vblank_index));
    }
}

const GsSemanticTraceStatistics& GsSemanticTraceWriter::statistics() const noexcept {
    return statistics_;
}

GsSemanticTraceCheckpointStatus
GsSemanticTraceWriter::checkpoint_last_completed_vblank(const std::filesystem::path& output_path) {
    require_open();
    if (!config_.retain_last_nonempty_completed_vblank) {
        throw std::logic_error("GS semantic trace checkpoints require selected-vblank=last");
    }
    if (!have_completed_interval_ || completed_interval_output_ == nullptr) {
        return GsSemanticTraceCheckpointStatus::NoRetainedFrame;
    }
    auto checkpoint = open_exclusive(
        output_path, "could not exclusively create GS semantic trace checkpoint ");
    try {
        const GsSemanticTraceStatistics checkpoint_statistics = statistics_;
        write_last_header(checkpoint, output_path, config_.maximum_events);
        copy_file_from_start_preserving_position(completed_interval_output_, checkpoint,
                                                 output_path);
        write_last_footer(checkpoint, output_path, checkpoint_statistics);
        if (std::fflush(checkpoint) != 0) {
            throw_file_error(output_path, "could not flush GS semantic trace checkpoint");
        }
        std::FILE* const closing = checkpoint;
        checkpoint = nullptr;
        if (std::fclose(closing) != 0) {
            throw_file_error(output_path, "could not close GS semantic trace checkpoint");
        }
    } catch (...) {
        if (checkpoint != nullptr) {
            std::fclose(checkpoint);
        }
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        throw;
    }
    return GsSemanticTraceCheckpointStatus::Written;
}

void GsSemanticTraceWriter::require_open() const {
    if (output_ == nullptr) {
        throw std::logic_error("GS semantic trace is closed");
    }
}

void GsSemanticTraceWriter::flush() {
    require_open();
    if (std::fflush(output_) != 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "could not flush GS semantic trace " + output_path_.string());
    }
}

void GsSemanticTraceWriter::close() {
    if (output_ == nullptr) {
        return;
    }
    try {
        if (config_.retain_last_nonempty_completed_vblank) {
            discard_current_last_interval();
            copy_completed_last_interval();
        }
        write_footer();
        flush();
    } catch (...) {
        close_temporary_files();
        std::fclose(output_);
        output_ = nullptr;
        throw;
    }
    close_temporary_files();
    std::FILE* closing = output_;
    output_ = nullptr;
    if (std::fclose(closing) != 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "could not close GS semantic trace " + output_path_.string());
    }
}

void GsSemanticTraceWriter::check_write(const int result) const {
    if (result < 0) {
        const int error = errno != 0 ? errno : EIO;
        throw std::system_error(error, std::generic_category(),
                                "could not write GS semantic trace " + output_path_.string());
    }
}

void GsSemanticTraceWriter::copy_completed_last_interval() {
    if (!have_completed_interval_ || completed_interval_output_ == nullptr) {
        statistics_.events_written = 0U;
        statistics_.events_omitted = 0U;
        statistics_.event_limit_reached = false;
        statistics_.retained_nonempty_vblank.reset();
        return;
    }
    copy_file_from_start_preserving_position(completed_interval_output_, output_, output_path_);
}

void GsSemanticTraceWriter::discard_current_last_interval() noexcept {
    saturating_add(statistics_.events_filtered, interval_events_written_);
    saturating_add(statistics_.events_filtered, interval_events_omitted_);
    if (current_interval_output_ != nullptr) {
        std::fclose(current_interval_output_);
        current_interval_output_ = nullptr;
    }
    interval_events_written_ = 0U;
    interval_events_omitted_ = 0U;
    interval_semantic_events_observed_ = 0U;
    interval_limit_reached_ = false;
}

bool GsSemanticTraceWriter::begin_semantic_event() {
    if (config_.retain_last_nonempty_completed_vblank) {
        saturating_increment(interval_semantic_events_observed_);
    }
    return begin_event();
}

std::FILE* GsSemanticTraceWriter::event_output() const noexcept {
    return config_.retain_last_nonempty_completed_vblank ? current_interval_output_ : output_;
}

std::uint64_t GsSemanticTraceWriter::event_number() const noexcept {
    return config_.retain_last_nonempty_completed_vblank ? interval_events_written_
                                                          : statistics_.events_written;
}

bool GsSemanticTraceWriter::selected_interval(const std::uint64_t vblank_index) const noexcept {
    return config_.retain_last_nonempty_completed_vblank || !config_.selected_vblank.has_value() ||
           *config_.selected_vblank == vblank_index;
}

void GsSemanticTraceWriter::begin_last_interval(const std::uint64_t vblank_index) {
    static_cast<void>(vblank_index);
    current_interval_output_ = std::tmpfile();
    if (current_interval_output_ == nullptr) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "could not create temporary GS semantic trace interval");
    }
    interval_events_written_ = 0U;
    interval_events_omitted_ = 0U;
    interval_semantic_events_observed_ = 0U;
    interval_limit_reached_ = false;
}

void GsSemanticTraceWriter::complete_last_interval() {
    if (interval_semantic_events_observed_ == 0U) {
        discard_current_last_interval();
        return;
    }
    if (completed_interval_output_ != nullptr) {
        saturating_add(statistics_.events_filtered, completed_events_written_);
        saturating_add(statistics_.events_filtered, completed_events_omitted_);
        std::fclose(completed_interval_output_);
    }
    completed_interval_output_ = current_interval_output_;
    current_interval_output_ = nullptr;
    completed_events_written_ = interval_events_written_;
    completed_events_omitted_ = interval_events_omitted_;
    completed_limit_reached_ = interval_limit_reached_;
    completed_vblank_ = statistics_.current_vblank;
    have_completed_interval_ = true;
    statistics_.events_written = completed_events_written_;
    statistics_.events_omitted = completed_events_omitted_;
    statistics_.event_limit_reached = completed_limit_reached_;
    statistics_.retained_nonempty_vblank = completed_vblank_;
}

void GsSemanticTraceWriter::write_header() {
    if (config_.retain_last_nonempty_completed_vblank) {
        write_last_header(output_, output_path_, config_.maximum_events);
    } else if (config_.selected_vblank.has_value()) {
        check_write(std::fprintf(output_,
                                 "# takedown-gs-semantic-v1 max-events=%llu selected-vblank=%llu\n",
                                 config_.maximum_events, *config_.selected_vblank));
    } else {
        check_write(std::fprintf(output_,
                                 "# takedown-gs-semantic-v1 max-events=%llu selected-vblank=all\n",
                                 config_.maximum_events));
    }
}

void GsSemanticTraceWriter::write_footer() {
    if (footer_written_) {
        return;
    }
    if (config_.retain_last_nonempty_completed_vblank) {
        write_last_footer(output_, output_path_, statistics_);
    } else {
        check_write(std::fprintf(
            output_,
            "# end events=%llu omitted=%llu filtered=%llu tags=%llu registers=%llu images=%llu "
            "kicks=%llu resets=%llu last-vblank=%llu limit=%u\n",
            statistics_.events_written, statistics_.events_omitted, statistics_.events_filtered,
            statistics_.tags_observed, statistics_.register_writes_observed,
            statistics_.image_transfers_observed, statistics_.draw_kicks_observed,
            statistics_.resets_observed, statistics_.current_vblank,
            statistics_.event_limit_reached ? 1U : 0U));
    }
    footer_written_ = true;
}

void GsSemanticTraceWriter::reset_drawing_state() noexcept {
    assembler_.reset();
    bitbltbuf_ = 0U;
    trxpos_ = 0U;
    trxreg_ = 0U;
    trxdir_ = 0U;
    packed_q_bits_ = 0x3f800000U;
}

void GsSemanticTraceWriter::write_draw_kick(const guest::GsRegisterWrite& write,
                                            const guest::GsPrimitiveKickSnapshot& kick) {
    if (!begin_semantic_event()) {
        return;
    }
    const bool q_known = kick.vertex.rgbaq.known;
    const auto q = static_cast<unsigned int>(kick.vertex.rgbaq.raw >> 32U);
    check_write(std::fprintf(
        event_output(),
        "%012llu KICK cmdseq=%llu path=%u source=%s ctx=%u prim=%03llx attrs=%03llx tex=%u "
        "fixed-uv=%u tex0=%u:%016llx frame=%u:%016llx test=%u:%016llx zbuf=%u:%016llx "
        "st=%u:%016llx q=%u:%08x q-latch=%08x xyz=%016llx xgkick=%llu\n",
        event_number(), write.sequence, static_cast<unsigned int>(write.path) + 1U,
        source_name(write.source),
        kick.controls.context == guest::GsDrawingContext::Context1 ? 1U : 2U,
        kick.controls.prim_raw, kick.controls.effective_attributes_raw,
        kick.controls.texture_mapping_enabled ? 1U : 0U, kick.controls.fixed_uv ? 1U : 0U,
        kick.context.tex0.known ? 1U : 0U, kick.context.tex0.raw,
        kick.context.frame.known ? 1U : 0U, kick.context.frame.raw,
        kick.context.test.known ? 1U : 0U, kick.context.test.raw,
        kick.context.zbuf.known ? 1U : 0U, kick.context.zbuf.raw,
        kick.vertex.st.known ? 1U : 0U, kick.vertex.st.raw, q_known ? 1U : 0U, q,
        packed_q_bits_, kick.vertex.position.raw,
        write.path == guest::GifPath::Path1 ? current_path1_xgkick_sequence_ : 0U));
}

} // namespace takedown::platform
