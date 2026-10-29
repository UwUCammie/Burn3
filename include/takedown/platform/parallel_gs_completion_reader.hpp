#pragma once

#include "takedown/platform/parallel_gs_completion.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace takedown::platform {

enum class ParallelGsCompletionReaderState : std::uint8_t {
    AwaitingHello,
    Active,
    Terminal,
    Failed,
};

enum class ParallelGsCompletionReaderFailure : std::uint8_t {
    None,
    ReadFailure,
    MalformedRecord,
    HelloRequired,
    HelloInvalid,
    DuplicateHello,
    SessionMismatch,
    SequenceMismatch,
    CompleteInvalid,
    ErrorInvalid,
    RendererError,
    EosInvalid,
    UnexpectedAfterError,
    UnexpectedAfterEos,
    PartialRecordAtEof,
    PeerClosedBeforeEos,
    PendingOverflow,
    RawVsyncMismatch,
};

[[nodiscard]] std::string_view to_string(ParallelGsCompletionReaderState state) noexcept;
[[nodiscard]] std::string_view to_string(ParallelGsCompletionReaderFailure failure) noexcept;

struct ParallelGsCompletionReaderStatistics {
    std::uint64_t bytes_read{};
    std::uint64_t records_decoded{};
    std::uint64_t hello_records{};
    std::uint64_t complete_records{};
    std::uint64_t complete_records_consumed{};
    std::uint64_t error_records{};
    std::uint64_t eos_records{};
    std::uint64_t protocol_failures{};
    std::uint64_t session_epoch{};
    std::uint64_t last_sequence{};
    std::uint64_t last_raw_vsync_index{};
    std::uint64_t eos_raw_vsync_index{};
    std::uint64_t pending_complete_records{};
    std::int32_t last_read_errno{};
    bool clean_eos{};
    bool peer_closed{};
};

// Serialized, nonblocking consumer for the optional paraLLEl-GS completion
// FIFO. The renderer writes fixed-size records; this reader never creates,
// replaces, or removes the FIFO and never starts a polling thread. Call poll()
// from the emulation thread, then consume_complete_for_raw_vsync() only at an
// authenticated game-side VBlank correlation boundary.
class ParallelGsCompletionReader final {
  public:
    explicit ParallelGsCompletionReader(const std::filesystem::path& fifo_path);
    ~ParallelGsCompletionReader();

    ParallelGsCompletionReader(const ParallelGsCompletionReader&) = delete;
    ParallelGsCompletionReader& operator=(const ParallelGsCompletionReader&) = delete;
    ParallelGsCompletionReader(ParallelGsCompletionReader&&) = delete;
    ParallelGsCompletionReader& operator=(ParallelGsCompletionReader&&) = delete;

    // Drains all currently available bytes without waiting. A missing writer
    // before Hello is an idle state; a peer which disappears after Hello must
    // send Eos or the reader records a failure.
    void poll() noexcept;

    // Returns and removes the exact next Complete record. An empty queue means
    // that the renderer has not reached the requested raw VSync yet. A queued
    // different index is a strict correlation failure: records are ordered and
    // cannot legitimately skip the requested raw VSync.
    [[nodiscard]] std::optional<ParallelGsCompletionRecord>
    consume_complete_for_raw_vsync(std::uint64_t raw_vsync_index) noexcept;

    [[nodiscard]] ParallelGsCompletionReaderState state() const noexcept { return state_; }
    [[nodiscard]] ParallelGsCompletionReaderFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] bool healthy() const noexcept {
        return failure_ == ParallelGsCompletionReaderFailure::None;
    }
    [[nodiscard]] bool terminal() const noexcept {
        return state_ == ParallelGsCompletionReaderState::Terminal;
    }
    [[nodiscard]] const std::filesystem::path& fifo_path() const noexcept { return fifo_path_; }
    [[nodiscard]] ParallelGsCompletionReaderStatistics statistics() const noexcept;

  private:
    static constexpr std::size_t kMaximumPendingCompleteRecords = 4096U;

    void accept_bytes(const std::uint8_t* bytes, std::size_t size) noexcept;
    void accept_record(const ParallelGsCompletionRecord& record) noexcept;
    void fail(ParallelGsCompletionReaderFailure failure) noexcept;
    [[nodiscard]] bool validate_common(const ParallelGsCompletionRecord& record) noexcept;
    [[nodiscard]] static bool is_known_error(std::uint64_t value) noexcept;
    [[nodiscard]] static bool has_only_eos_flags(std::uint32_t flags) noexcept;

    std::filesystem::path fifo_path_;
    int descriptor_{-1};
#if defined(_WIN32)
    void* pipe_{nullptr};
#endif
    std::vector<std::uint8_t> pending_bytes_;
    std::deque<ParallelGsCompletionRecord> pending_completes_;
    ParallelGsCompletionReaderState state_{
        ParallelGsCompletionReaderState::AwaitingHello};
    ParallelGsCompletionReaderFailure failure_{
        ParallelGsCompletionReaderFailure::None};
    std::uint64_t bytes_read_{};
    std::uint64_t records_decoded_{};
    std::uint64_t hello_records_{};
    std::uint64_t complete_records_{};
    std::uint64_t complete_records_consumed_{};
    std::uint64_t error_records_{};
    std::uint64_t eos_records_{};
    std::uint64_t protocol_failures_{};
    std::uint64_t session_epoch_{};
    std::uint64_t last_sequence_{};
    std::uint64_t last_raw_vsync_index_{};
    std::uint64_t eos_raw_vsync_index_{};
    std::int32_t last_read_errno_{};
    bool clean_eos_{};
    bool peer_closed_{};
    bool renderer_error_seen_{};
};

} // namespace takedown::platform
