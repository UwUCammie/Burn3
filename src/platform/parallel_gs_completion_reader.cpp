#include "takedown/platform/parallel_gs_completion_reader.hpp"

#include <array>
#include <cerrno>
#include <system_error>

#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace takedown::platform {

namespace {

using State = ParallelGsCompletionReaderState;
using Failure = ParallelGsCompletionReaderFailure;
using Record = ParallelGsCompletionRecord;

// Hello carries only the session epoch; the payload words must be zero.
bool has_empty_hello_words(const Record& record) {
    return record.flags == 0U && record.value == 0U && record.reserved0 == 0U &&
           record.reserved1 == 0U;
}

} // namespace

std::string_view to_string(const State state) noexcept {
    switch (state) {
    case ParallelGsCompletionReaderState::AwaitingHello:
        return "awaiting-hello";
    case ParallelGsCompletionReaderState::Active:
        return "active";
    case ParallelGsCompletionReaderState::Terminal:
        return "terminal";
    case ParallelGsCompletionReaderState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(const Failure failure) noexcept {
    switch (failure) {
    case ParallelGsCompletionReaderFailure::None:
        return "none";
    case ParallelGsCompletionReaderFailure::ReadFailure:
        return "read-failure";
    case ParallelGsCompletionReaderFailure::MalformedRecord:
        return "malformed-record";
    case ParallelGsCompletionReaderFailure::HelloRequired:
        return "hello-required";
    case ParallelGsCompletionReaderFailure::HelloInvalid:
        return "hello-invalid";
    case ParallelGsCompletionReaderFailure::DuplicateHello:
        return "duplicate-hello";
    case ParallelGsCompletionReaderFailure::SessionMismatch:
        return "session-mismatch";
    case ParallelGsCompletionReaderFailure::SequenceMismatch:
        return "sequence-mismatch";
    case ParallelGsCompletionReaderFailure::CompleteInvalid:
        return "complete-invalid";
    case ParallelGsCompletionReaderFailure::ErrorInvalid:
        return "error-invalid";
    case ParallelGsCompletionReaderFailure::RendererError:
        return "renderer-error";
    case ParallelGsCompletionReaderFailure::EosInvalid:
        return "eos-invalid";
    case ParallelGsCompletionReaderFailure::UnexpectedAfterError:
        return "unexpected-after-error";
    case ParallelGsCompletionReaderFailure::UnexpectedAfterEos:
        return "unexpected-after-eos";
    case ParallelGsCompletionReaderFailure::PartialRecordAtEof:
        return "partial-record-at-eof";
    case ParallelGsCompletionReaderFailure::PeerClosedBeforeEos:
        return "peer-closed-before-eos";
    case ParallelGsCompletionReaderFailure::PendingOverflow:
        return "pending-overflow";
    case ParallelGsCompletionReaderFailure::RawVsyncMismatch:
        return "raw-vsync-mismatch";
    }
    return "unknown";
}

ParallelGsCompletionReader::ParallelGsCompletionReader(const std::filesystem::path& fifo_path)
    : fifo_path_(fifo_path) {
    if (fifo_path_.empty()) {
        throw std::invalid_argument("paraLLEl-GS completion requires a non-empty FIFO path");
    }
#if defined(_WIN32)
    pipe_ = windows_gs_pipe::open_reader(fifo_path_.c_str());
#else
    // RECOVERY: only the Windows branch was present in the recovered object; this POSIX
    // branch is a plain nonblocking FIFO open written to match the header.
    descriptor_ = ::open(fifo_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "could not open paraLLEl-GS completion FIFO");
    }
#endif
    pending_bytes_.reserve(kParallelGsCompletionRecordBytes * 4U);
}

ParallelGsCompletionReader::~ParallelGsCompletionReader() {
#if defined(_WIN32)
    if (pipe_ != nullptr) {
        CloseHandle(pipe_);
    }
#else
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
#endif
}

void ParallelGsCompletionReader::fail(const Failure failure) noexcept {
    // The first failure is kept, except that "unexpected after" reports replace it.
    if (failure_ == ParallelGsCompletionReaderFailure::None ||
        failure == ParallelGsCompletionReaderFailure::UnexpectedAfterError ||
        failure == ParallelGsCompletionReaderFailure::UnexpectedAfterEos) {
        failure_ = failure;
    }
    ++protocol_failures_;
    // A renderer-reported error is recorded but the stream stays readable until Eos.
    if (failure != ParallelGsCompletionReaderFailure::RendererError) {
        state_ = ParallelGsCompletionReaderState::Failed;
    }
}

bool ParallelGsCompletionReader::is_known_error(const std::uint64_t value) noexcept {
    switch (static_cast<ParallelGsCompletionError>(value)) {
    case ParallelGsCompletionError::StreamTruncated:
    case ParallelGsCompletionError::FenceUnavailable:
    case ParallelGsCompletionError::OutputWrite:
    case ParallelGsCompletionError::PendingOverflow:
        return true;
    }
    return false;
}

bool ParallelGsCompletionReader::has_only_eos_flags(const std::uint32_t flags) noexcept {
    return (flags & ~kParallelGsCompletionEosClean) == 0U;
}

bool ParallelGsCompletionReader::validate_common(const Record& record) noexcept {
    if (record.session_epoch != session_epoch_) {
        fail(ParallelGsCompletionReaderFailure::SessionMismatch);
        return false;
    }
    if (last_sequence_ == UINT64_MAX || record.sequence == 0U ||
        record.sequence != last_sequence_ + 1U) {
        fail(ParallelGsCompletionReaderFailure::SequenceMismatch);
        return false;
    }
    last_sequence_ = record.sequence;
    return true;
}

void ParallelGsCompletionReader::accept_record(const Record& record) noexcept {
    ++records_decoded_;
    if (state_ == ParallelGsCompletionReaderState::Failed) {
        return;
    }
    if (state_ == ParallelGsCompletionReaderState::Terminal) {
        fail(ParallelGsCompletionReaderFailure::UnexpectedAfterEos);
        return;
    }
    if (state_ == ParallelGsCompletionReaderState::AwaitingHello) {
        if (record.type != ParallelGsCompletionRecordType::Hello) {
            fail(ParallelGsCompletionReaderFailure::HelloRequired);
            return;
        }
        if (record.session_epoch == 0U || record.sequence != 0U ||
            !has_empty_hello_words(record) || record.raw_vsync_index != 0U) {
            fail(ParallelGsCompletionReaderFailure::HelloInvalid);
            return;
        }
        session_epoch_ = record.session_epoch;
        ++hello_records_;
        state_ = ParallelGsCompletionReaderState::Active;
        return;
    }

    if (record.type == ParallelGsCompletionRecordType::Hello) {
        fail(ParallelGsCompletionReaderFailure::DuplicateHello);
        return;
    }
    if (!validate_common(record)) {
        return;
    }

    switch (record.type) {
    case ParallelGsCompletionRecordType::Hello:
        fail(ParallelGsCompletionReaderFailure::DuplicateHello);
        return;
    case ParallelGsCompletionRecordType::Complete:
        if (renderer_error_seen_ || record.flags != 0U || record.value != 0U ||
            record.reserved0 != 0U || record.reserved1 != 0U || record.raw_vsync_index == 0U ||
            record.raw_vsync_index <= last_raw_vsync_index_) {
            fail(renderer_error_seen_ ? ParallelGsCompletionReaderFailure::UnexpectedAfterError
                                      : ParallelGsCompletionReaderFailure::CompleteInvalid);
            return;
        }
        if (pending_completes_.size() >= kMaximumPendingCompleteRecords) {
            fail(ParallelGsCompletionReaderFailure::PendingOverflow);
            return;
        }
        last_raw_vsync_index_ = record.raw_vsync_index;
        pending_completes_.push_back(record);
        ++complete_records_;
        return;
    case ParallelGsCompletionRecordType::Error:
        if (renderer_error_seen_) {
            fail(ParallelGsCompletionReaderFailure::UnexpectedAfterError);
            return;
        }
        if (record.flags != 0U || record.reserved0 != 0U || record.reserved1 != 0U ||
            !is_known_error(record.value) || record.raw_vsync_index < last_raw_vsync_index_) {
            fail(ParallelGsCompletionReaderFailure::ErrorInvalid);
            return;
        }
        ++error_records_;
        renderer_error_seen_ = true;
        fail(ParallelGsCompletionReaderFailure::RendererError);
        return;
    case ParallelGsCompletionRecordType::Eos:
        if (renderer_error_seen_ && (record.flags & kParallelGsCompletionEosClean) != 0U) {
            fail(ParallelGsCompletionReaderFailure::UnexpectedAfterError);
            return;
        }
        if (!has_only_eos_flags(record.flags) || record.reserved0 != 0U || record.reserved1 != 0U ||
            record.value != record.raw_vsync_index ||
            record.raw_vsync_index < last_raw_vsync_index_) {
            fail(ParallelGsCompletionReaderFailure::EosInvalid);
            return;
        }
        ++eos_records_;
        eos_raw_vsync_index_ = record.raw_vsync_index;
        clean_eos_ = (record.flags & kParallelGsCompletionEosClean) != 0U;
        state_ = ParallelGsCompletionReaderState::Terminal;
        return;
    }
    fail(ParallelGsCompletionReaderFailure::MalformedRecord);
}

void ParallelGsCompletionReader::accept_bytes(const std::uint8_t* const bytes,
                                              const std::size_t size) noexcept {
    if (bytes == nullptr || size == 0U || state_ == ParallelGsCompletionReaderState::Failed) {
        return;
    }
    pending_bytes_.insert(pending_bytes_.end(), bytes, bytes + size);
    if (pending_bytes_.size() >
        (kMaximumPendingCompleteRecords + 1U) * kParallelGsCompletionRecordBytes) {
        fail(ParallelGsCompletionReaderFailure::PendingOverflow);
        return;
    }

    std::size_t offset = 0U;
    while (pending_bytes_.size() - offset >= kParallelGsCompletionRecordBytes &&
           state_ != ParallelGsCompletionReaderState::Failed) {
        Record record;
        if (!decode_parallel_gs_completion(pending_bytes_.data() + offset,
                                           kParallelGsCompletionRecordBytes, record)) {
            fail(ParallelGsCompletionReaderFailure::MalformedRecord);
            return;
        }
        accept_record(record);
        offset += kParallelGsCompletionRecordBytes;
    }
    if (offset != 0U) {
        pending_bytes_.erase(pending_bytes_.begin(),
                             pending_bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void ParallelGsCompletionReader::poll() noexcept {
#if defined(_WIN32)
    if (pipe_ == nullptr || state_ == ParallelGsCompletionReaderState::Failed) {
        return;
    }
    std::array<std::uint8_t, 4096U> bytes{};
    try {
        for (;;) {
            const int count = windows_gs_pipe::read_available(pipe_, bytes.data(), bytes.size());
            if (count > 0) {
                bytes_read_ += static_cast<std::uint64_t>(count);
                accept_bytes(bytes.data(), static_cast<std::size_t>(count));
                if (state_ == ParallelGsCompletionReaderState::Failed) {
                    return;
                }
                continue;
            }
            if (count < 0) {
                peer_closed_ = true;
                if (!pending_bytes_.empty()) {
                    fail(ParallelGsCompletionReaderFailure::PartialRecordAtEof);
                } else if (state_ != ParallelGsCompletionReaderState::Terminal) {
                    fail(ParallelGsCompletionReaderFailure::PeerClosedBeforeEos);
                }
            }
            return;
        }
    } catch (const std::system_error& error) {
        last_read_errno_ = error.code().value();
        fail(ParallelGsCompletionReaderFailure::ReadFailure);
    }
#else
    // RECOVERY: POSIX branch reconstructed from the header; the recovered object is Windows-only.
    if (descriptor_ < 0 || state_ == ParallelGsCompletionReaderState::Failed) {
        return;
    }
    std::array<std::uint8_t, 4096U> bytes{};
    for (;;) {
        const auto count = ::read(descriptor_, bytes.data(), bytes.size());
        if (count > 0) {
            bytes_read_ += static_cast<std::uint64_t>(count);
            accept_bytes(bytes.data(), static_cast<std::size_t>(count));
            if (state_ == ParallelGsCompletionReaderState::Failed) {
                return;
            }
            continue;
        }
        if (count == 0) {
            // No writer: idle until Hello, an unexpected disappearance afterwards.
            if (state_ == ParallelGsCompletionReaderState::AwaitingHello) {
                return;
            }
            peer_closed_ = true;
            if (!pending_bytes_.empty()) {
                fail(ParallelGsCompletionReaderFailure::PartialRecordAtEof);
            } else if (state_ != ParallelGsCompletionReaderState::Terminal) {
                fail(ParallelGsCompletionReaderFailure::PeerClosedBeforeEos);
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            last_read_errno_ = errno;
            fail(ParallelGsCompletionReaderFailure::ReadFailure);
        }
        return;
    }
#endif
}

std::optional<ParallelGsCompletionRecord>
ParallelGsCompletionReader::consume_complete_for_raw_vsync(
    const std::uint64_t raw_vsync_index) noexcept {
    if (raw_vsync_index == 0U || pending_completes_.empty() ||
        state_ == ParallelGsCompletionReaderState::Failed) {
        return std::nullopt;
    }
    const auto& next = pending_completes_.front();
    if (next.raw_vsync_index != raw_vsync_index) {
        fail(ParallelGsCompletionReaderFailure::RawVsyncMismatch);
        return std::nullopt;
    }
    const Record result = next;
    pending_completes_.pop_front();
    ++complete_records_consumed_;
    return result;
}

ParallelGsCompletionReaderStatistics ParallelGsCompletionReader::statistics() const noexcept {
    return {.bytes_read = bytes_read_,
            .records_decoded = records_decoded_,
            .hello_records = hello_records_,
            .complete_records = complete_records_,
            .complete_records_consumed = complete_records_consumed_,
            .error_records = error_records_,
            .eos_records = eos_records_,
            .protocol_failures = protocol_failures_,
            .session_epoch = session_epoch_,
            .last_sequence = last_sequence_,
            .last_raw_vsync_index = last_raw_vsync_index_,
            .eos_raw_vsync_index = eos_raw_vsync_index_,
            .pending_complete_records = pending_completes_.size(),
            .last_read_errno = last_read_errno_,
            .clean_eos = clean_eos_,
            .peer_closed = peer_closed_};
}

} // namespace takedown::platform
