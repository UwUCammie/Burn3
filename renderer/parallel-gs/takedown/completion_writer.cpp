// Renderer side of the completion FIFO. The wire rules below are taken from
// src/platform/parallel_gs_completion_reader.cpp (the runtime reader) and the
// ordering the ledger needs in src/runtime/slus_21050_femain_parallel_gs_completion_ledger.cpp.
//
// Stream rules enforced by the reader:
//  1. The first record is Hello: type Hello, session_epoch != 0, sequence 0,
//     raw_vsync_index 0, flags 0, value 0, reserved0/1 0. Any other first
//     record is rejected, and a second Hello is rejected.
//  2. Every record after Hello carries that same session_epoch.
//  3. Sequence: the first record after Hello has sequence 1; each later record
//     is previous + 1. Hello does not advance the counter.
//  4. Complete: flags 0, value 0, reserved 0, raw_vsync_index >= 1 and strictly
//     greater than the previous Complete. The ledger also needs exactly one
//     Complete per raw VBlank with consecutive indices starting at 1, so
//     Completes are emitted in submission order and never skipped except
//     when an Error ends the stream.
//  5. Error: flags 0, reserved 0, value one of StreamTruncated(1),
//     FenceUnavailable(2), OutputWrite(3), PendingOverflow(4), and
//     raw_vsync_index >= the last Complete index (0 is accepted before any
//     Complete). After an Error the reader accepts no Complete and no second
//     Error, only Eos.
//  6. Eos: flags only kParallelGsCompletionEosClean (bit 0), reserved 0,
//     value == raw_vsync_index, raw_vsync_index >= the last Complete index.
//     A clean Eos after an Error is rejected. Nothing may follow Eos.
//  7. A peer that disappears before Eos, or leaves a partial 64-byte record,
//     is a reader failure. finish() therefore always ends the stream with Eos.
//  8. The reader rejects bad magic, version or size. encode_* always writes
//     exactly 64 bytes.
//
// Writer policy that follows from the rules:
//  - Hello is queued first; Completes follow in submission order; at most one
//    Error ends the Complete stream; Eos always closes it.
//  - The clean flag is only set when no Error was queued.
//  - Records are queued in memory and written whole (POSIX: nonblocking FIFO,
//    64 bytes is within PIPE_BUF so a write is all-or-none; Windows: message
//    pipe, all-or-none). A partially written record stays at the queue head.

#include "completion_writer.hpp"

#include "takedown/platform/parallel_gs_completion.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace TakedownRenderer
{
namespace
{
using takedown::platform::kParallelGsCompletionEosClean;
using takedown::platform::kParallelGsCompletionRecordBytes;
using takedown::platform::ParallelGsCompletionBytes;
using takedown::platform::ParallelGsCompletionError;
using takedown::platform::ParallelGsCompletionRecord;
using takedown::platform::ParallelGsCompletionRecordType;
using takedown::platform::encode_parallel_gs_completion;

// Fixed capacity of unsignalled fences held before an Error(PendingOverflow).
constexpr std::size_t kPendingCapacity = 1024;
// finish() waits this long for pending fences before reporting them lost.
constexpr auto kFenceWait = std::chrono::milliseconds(1500);
// finish() waits at most this long in total for the bytes to leave.
constexpr auto kFinishDeadline = std::chrono::milliseconds(2000);
constexpr auto kRetryPause = std::chrono::milliseconds(1);

using Clock = std::chrono::steady_clock;

// Nonzero per-writer session id. The reader only requires it to be nonzero
// and constant for the session; it does not need to be cryptographic.
std::uint64_t make_session_epoch(const void *owner)
{
	const auto ticks = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
	const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner));
	const std::uint64_t epoch = ticks ^ (address << 13U) ^ 0x9E3779B97F4A7C15ULL;
	return epoch != 0U ? epoch : 1U;
}

#if !defined(_WIN32)
// A reader that disappears must surface as EPIPE on write, not kill the renderer.
void ignore_sigpipe()
{
	struct sigaction current {};
	if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
		struct sigaction ignore {};
		ignore.sa_handler = SIG_IGN;
		::sigaction(SIGPIPE, &ignore, nullptr);
	}
}
#endif
} // namespace

struct CompletionWriter::Impl
{
	struct PendingFence
	{
		std::uint64_t raw_vsync_index;
		std::function<bool()> is_signalled;
	};

	Impl(const std::string &fifo_path, bool &failed_flag);
	~Impl();
	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;

	bool hello();
	void on_vsync_submitted(std::uint64_t raw_vsync_index, std::function<bool()> is_signalled);
	void poll();
	void error(ParallelGsCompletionError code, std::uint64_t raw_vsync_index);
	void finish(bool clean);

	void enqueue(ParallelGsCompletionRecordType type, std::uint64_t raw_vsync_index,
	             std::uint64_t value, std::uint32_t flags);
	void ensure_hello();
	void emit_ready_completes();
	void queue_error(ParallelGsCompletionError code, std::uint64_t raw_vsync_index);
	bool try_open();
	long long write_some(const std::uint8_t *bytes, std::size_t size);
	void flush();
	void fail_broken(const char *what, int code);

	bool &failed;
	std::string path;
	std::uint64_t session_epoch = 0;
	std::uint64_t next_sequence = 1;
	std::uint64_t last_submitted_index = 0;
	// Raw index of the last Complete or Error queued into the byte stream.
	std::uint64_t last_emitted_index = 0;
	std::deque<PendingFence> pending;
	std::deque<ParallelGsCompletionBytes> output;
	std::size_t output_offset = 0;
	int last_errno = 0;
	bool hello_queued = false;
	bool error_queued = false;
	bool finished = false;
	// A permanent write failure: nothing more is sent or queued.
	bool broken = false;
#if defined(_WIN32)
	std::unique_ptr<takedown::platform::windows_gs_pipe::Output> pipe;
#else
	int fd = -1;
#endif
};

CompletionWriter::Impl::Impl(const std::string &fifo_path, bool &failed_flag)
	: failed(failed_flag), path(fifo_path), session_epoch(make_session_epoch(this))
{
#if defined(_WIN32)
	try {
		pipe = std::make_unique<takedown::platform::windows_gs_pipe::Output>(path.c_str());
	} catch (const std::exception &error) {
		throw std::runtime_error(std::string("could not create paraLLEl-GS completion pipe: ") +
		                         error.what());
	}
#else
	ignore_sigpipe();
	fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 && errno != ENXIO) {
		const int code = errno;
		throw std::runtime_error("could not open paraLLEl-GS completion FIFO '" + path +
		                         "': " + std::strerror(code));
	}
	// ENXIO: no reader yet. try_open() retries from hello(), poll() and finish().
#endif
}

CompletionWriter::Impl::~Impl()
{
#if !defined(_WIN32)
	if (fd >= 0) {
		::close(fd);
	}
#endif
}

void CompletionWriter::Impl::enqueue(ParallelGsCompletionRecordType type,
                                     const std::uint64_t raw_vsync_index,
                                     const std::uint64_t value, const std::uint32_t flags)
{
	ParallelGsCompletionRecord record;
	record.type = type;
	record.flags = flags;
	record.session_epoch = session_epoch;
	record.sequence = type == ParallelGsCompletionRecordType::Hello ? 0U : next_sequence++;
	record.raw_vsync_index = raw_vsync_index;
	record.value = value;
	output.push_back(encode_parallel_gs_completion(record));
}

void CompletionWriter::Impl::ensure_hello()
{
	if (!hello_queued) {
		enqueue(ParallelGsCompletionRecordType::Hello, 0U, 0U, 0U);
		hello_queued = true;
	}
}

bool CompletionWriter::Impl::hello()
{
	if (finished || broken) {
		return false;
	}
	ensure_hello();
	try_open();
	flush();
	return true;
}

void CompletionWriter::Impl::emit_ready_completes()
{
	if (broken) {
		return;
	}
	// Hello must precede every Complete, so it is queued here if hello() has not run yet.
	ensure_hello();
	// Strictly in order: an unsignalled front fence blocks every later Complete.
	while (!error_queued && !pending.empty() && pending.front().is_signalled()) {
		const std::uint64_t index = pending.front().raw_vsync_index;
		pending.pop_front();
		enqueue(ParallelGsCompletionRecordType::Complete, index, 0U, 0U);
		last_emitted_index = index;
	}
}

void CompletionWriter::Impl::queue_error(const ParallelGsCompletionError code,
                                         const std::uint64_t raw_vsync_index)
{
	ensure_hello();
	// The reader rejects an Error that is older than the last Complete.
	const std::uint64_t index = std::max(raw_vsync_index, last_emitted_index);
	enqueue(ParallelGsCompletionRecordType::Error, index,
	        static_cast<std::uint64_t>(code), 0U);
	last_emitted_index = index;
	error_queued = true;
	failed = true;
}

void CompletionWriter::Impl::on_vsync_submitted(const std::uint64_t raw_vsync_index,
                                                std::function<bool()> is_signalled)
{
	if (finished || broken || error_queued) {
		return;
	}
	last_submitted_index = std::max(last_submitted_index, raw_vsync_index);
	emit_ready_completes();
	if (broken || error_queued) {
		return;
	}
	if (!is_signalled) {
		error(ParallelGsCompletionError::FenceUnavailable, raw_vsync_index);
		return;
	}
	if (pending.size() >= kPendingCapacity) {
		std::fprintf(stderr,
		             "takedown: paraLLEl-GS completion fences exceeded %zu pending; "
		             "reporting PendingOverflow\n",
		             kPendingCapacity);
		// Unsignalled fences are dropped: no Complete may follow the Error.
		pending.clear();
		queue_error(ParallelGsCompletionError::PendingOverflow, raw_vsync_index);
		return;
	}
	pending.push_back(PendingFence{raw_vsync_index, std::move(is_signalled)});
	emit_ready_completes();
}

void CompletionWriter::Impl::error(const ParallelGsCompletionError code,
                                   const std::uint64_t raw_vsync_index)
{
	if (finished || broken || error_queued) {
		return;
	}
	// Completes that already signalled are true, so they go out before the Error.
	emit_ready_completes();
	pending.clear();
	queue_error(code, raw_vsync_index);
}

void CompletionWriter::Impl::poll()
{
	if (broken) {
		return;
	}
	try_open();
	emit_ready_completes();
	flush();
}

void CompletionWriter::Impl::finish(const bool clean)
{
	if (finished) {
		return;
	}
	finished = true;
	if (broken) {
		return;
	}
	ensure_hello();
	try_open();

	const auto start = Clock::now();
	emit_ready_completes();
	flush();
	while (!error_queued && !pending.empty() && !broken &&
	       Clock::now() - start < kFenceWait) {
		std::this_thread::sleep_for(kRetryPause);
		emit_ready_completes();
		flush();
	}
	if (!broken && !error_queued && !pending.empty()) {
		const std::uint64_t index = pending.front().raw_vsync_index;
		std::fprintf(stderr, "takedown: paraLLEl-GS completion fence not signalled at finish; "
		                     "reporting FenceUnavailable\n");
		pending.clear();
		queue_error(ParallelGsCompletionError::FenceUnavailable, index);
	}
	if (broken) {
		return;
	}

	const std::uint64_t eos_index = std::max(last_submitted_index, last_emitted_index);
	// A clean Eos after an Error is rejected by the reader.
	const std::uint32_t flags = (clean && !error_queued) ? kParallelGsCompletionEosClean : 0U;
	enqueue(ParallelGsCompletionRecordType::Eos, eos_index, eos_index, flags);
	flush();
	while (!broken && !output.empty() && Clock::now() - start < kFinishDeadline) {
		std::this_thread::sleep_for(kRetryPause);
		flush();
	}
}

bool CompletionWriter::Impl::try_open()
{
#if defined(_WIN32)
	return pipe != nullptr;
#else
	if (fd >= 0) {
		return true;
	}
	if (broken) {
		return false;
	}
	fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		return true;
	}
	if (errno != ENXIO) {
		fail_broken("could not open FIFO", errno);
	}
	return false;
#endif
}

// Returns bytes accepted (>0). Returns 0 when the reader is not connected or
// the pipe is full, so the bytes stay queued. Returns -1 on a permanent error.
long long CompletionWriter::Impl::write_some(const std::uint8_t *bytes, const std::size_t size)
{
#if defined(_WIN32)
	if (pipe == nullptr) {
		return 0;
	}
	const int count = pipe->write(bytes, size);
	if (count < 0) {
		return -1;
	}
	return count;
#else
	if (!try_open()) {
		return 0;
	}
	for (;;) {
		const ssize_t count = ::write(fd, bytes, size);
		if (count >= 0) {
			return static_cast<long long>(count);
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		last_errno = errno;
		return -1;
	}
#endif
}

void CompletionWriter::Impl::flush()
{
	while (!broken && !output.empty()) {
		const std::size_t remaining = kParallelGsCompletionRecordBytes - output_offset;
		const long long count = write_some(output.front().data() + output_offset, remaining);
		if (count == 0) {
			return;
		}
		if (count < 0) {
			fail_broken("write failed", last_errno);
			return;
		}
		output_offset += static_cast<std::size_t>(count);
		if (output_offset == kParallelGsCompletionRecordBytes) {
			output.pop_front();
			output_offset = 0;
		}
	}
}

void CompletionWriter::Impl::fail_broken(const char *what, const int code)
{
	if (code != 0) {
		std::fprintf(stderr, "takedown: paraLLEl-GS completion %s: %s; completion records stop\n",
		             what, std::strerror(code));
	} else {
		std::fprintf(stderr, "takedown: paraLLEl-GS completion %s; completion records stop\n",
		             what);
	}
	broken = true;
	failed = true;
	output.clear();
	output_offset = 0;
}

CompletionWriter::CompletionWriter(const std::string &path)
	: impl_(std::make_unique<Impl>(path, failed_))
{
}

CompletionWriter::~CompletionWriter() = default;

bool CompletionWriter::hello()
{
	return impl_->hello();
}

void CompletionWriter::on_vsync_submitted(const std::uint64_t raw_vsync_index,
                                          std::function<bool()> is_signalled)
{
	impl_->on_vsync_submitted(raw_vsync_index, std::move(is_signalled));
}

void CompletionWriter::poll()
{
	impl_->poll();
}

void CompletionWriter::error(const takedown::platform::ParallelGsCompletionError code,
                             const std::uint64_t raw_vsync_index)
{
	impl_->error(code, raw_vsync_index);
}

void CompletionWriter::finish(const bool clean)
{
	impl_->finish(clean);
}
} // namespace TakedownRenderer
