// Tests for TakedownRenderer::CompletionWriter over a real FIFO (POSIX).
//
// Every build checks the exact bytes with a raw FIFO reader. Defining
// TAKEDOWN_COMPLETION_TEST_RUNTIME_READER and also compiling
// src/platform/parallel_gs_completion_reader.cpp adds checks that feed the same
// stream through the runtime's own reader. Exit status 0 means every check
// passed.

#include "completion_writer.hpp"

#if defined(_WIN32)
// The FIFO tests use POSIX calls. The Windows build only checks that this
// target still links.
#include <cstdio>

int main()
{
	std::puts("test_completion_writer: skipped (POSIX FIFO test)");
	return 0;
}
#else

#include "takedown/platform/parallel_gs_completion.hpp"
#if defined(TAKEDOWN_COMPLETION_TEST_RUNTIME_READER)
#include "takedown/platform/parallel_gs_completion_reader.hpp"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#if defined(TAKEDOWN_COMPLETION_TEST_RUNTIME_READER)
#include <filesystem>
#endif

namespace {

using takedown::platform::ParallelGsCompletionError;
using takedown::platform::ParallelGsCompletionRecord;
using takedown::platform::ParallelGsCompletionRecordType;
using takedown::platform::decode_parallel_gs_completion;
using takedown::platform::kParallelGsCompletionEosClean;
using takedown::platform::kParallelGsCompletionRecordBytes;
using TakedownRenderer::CompletionWriter;

using Record = ParallelGsCompletionRecord;
using Type = ParallelGsCompletionRecordType;
using Flag = std::shared_ptr<bool>;

int g_failures = 0;

#define EXPECT(condition)                                                              \
	do {                                                                               \
		if (!(condition)) {                                                            \
			++g_failures;                                                              \
			std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, \
			             #condition);                                                  \
		}                                                                              \
	} while (0)

Flag make_flag(const bool value)
{
	return std::make_shared<bool>(value);
}

// Temporary directory holding one FIFO, removed on destruction.
struct TempFifo {
	std::string dir;
	std::string path;

	TempFifo()
	{
		const char *tmp = std::getenv("TMPDIR");
		std::string pattern = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") +
		                      "/takedown-completion-XXXXXX";
		std::vector<char> buffer(pattern.begin(), pattern.end());
		buffer.push_back('\0');
		char *made = ::mkdtemp(buffer.data());
		if (made == nullptr) {
			std::perror("mkdtemp");
			std::exit(2);
		}
		dir = made;
		path = dir + "/completion.fifo";
		if (::mkfifo(path.c_str(), 0600) != 0) {
			std::perror("mkfifo");
			std::exit(2);
		}
	}

	~TempFifo()
	{
		::unlink(path.c_str());
		::rmdir(dir.c_str());
	}

	TempFifo(const TempFifo &) = delete;
	TempFifo &operator=(const TempFifo &) = delete;
};

// Test-side reader that keeps every byte the writer sends, so each record can
// be checked field by field.
class RawReader {
  public:
	explicit RawReader(const std::string &path)
		: fd_(::open(path.c_str(), O_RDONLY | O_NONBLOCK))
	{
		EXPECT(fd_ >= 0);
	}

	~RawReader()
	{
		if (fd_ >= 0) {
			::close(fd_);
		}
	}

	RawReader(const RawReader &) = delete;
	RawReader &operator=(const RawReader &) = delete;

	// Reads whatever is available now and never waits.
	void pump()
	{
		std::uint8_t buffer[4096];
		for (;;) {
			const ssize_t count = ::read(fd_, buffer, sizeof buffer);
			if (count > 0) {
				bytes_.insert(bytes_.end(), buffer, buffer + count);
				continue;
			}
			if (count < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		EXPECT(bytes_.size() % kParallelGsCompletionRecordBytes == 0);
		records_.clear();
		for (std::size_t offset = 0; offset + kParallelGsCompletionRecordBytes <= bytes_.size();
		     offset += kParallelGsCompletionRecordBytes) {
			Record record;
			EXPECT(decode_parallel_gs_completion(bytes_.data() + offset,
			                                     kParallelGsCompletionRecordBytes, record));
			records_.push_back(record);
		}
	}

	const std::vector<Record> &records() const { return records_; }

  private:
	int fd_;
	std::vector<std::uint8_t> bytes_;
	std::vector<Record> records_;
};

// Checks every field of one record. Reserved words and the magic/version/size
// header words must always match the wire contract.
void expect_record(const Record &record, const Type type, const std::uint64_t epoch,
                   const std::uint64_t sequence, const std::uint64_t raw_vsync_index,
                   const std::uint64_t value, const std::uint32_t flags)
{
	EXPECT(record.magic == takedown::platform::kParallelGsCompletionMagic);
	EXPECT(record.version == takedown::platform::kParallelGsCompletionVersion);
	EXPECT(record.size == kParallelGsCompletionRecordBytes);
	EXPECT(record.type == type);
	EXPECT(record.session_epoch == epoch);
	EXPECT(record.sequence == sequence);
	EXPECT(record.raw_vsync_index == raw_vsync_index);
	EXPECT(record.value == value);
	EXPECT(record.flags == flags);
	EXPECT(record.reserved0 == 0U);
	EXPECT(record.reserved1 == 0U);
}

// Returns the session epoch carried by the Hello record.
std::uint64_t expect_hello(const Record &record)
{
	expect_record(record, Type::Hello, record.session_epoch, 0U, 0U, 0U, 0U);
	EXPECT(record.session_epoch != 0U);
	return record.session_epoch;
}

bool require_count(const std::vector<Record> &records, const std::size_t count, const char *what)
{
	if (records.size() == count) {
		return true;
	}
	++g_failures;
	std::fprintf(stderr, "%s: expected %zu records, got %zu\n", what, count, records.size());
	return false;
}

// Hello, then Completes 1..3 with the fence for index 1 blocking the rest.
// Eos is clean and follows the last Complete.
void test_ordered_completions_and_clean_eos()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	const Flag f1 = make_flag(false);
	const Flag f2 = make_flag(false);
	const Flag f3 = make_flag(false);
	const Flag f4 = make_flag(false);
	const Flag f5 = make_flag(false);
	writer.on_vsync_submitted(1, [f1] { return *f1; });
	writer.on_vsync_submitted(2, [f2] { return *f2; });
	writer.on_vsync_submitted(3, [f3] { return *f3; });
	*f2 = true;
	*f3 = true;
	writer.poll();
	raw.pump();
	// Fence 1 is not signalled, so fences 2 and 3 must not be emitted yet.
	if (!require_count(raw.records(), 1, "ordered: after blocked poll")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);

	*f1 = true;
	writer.poll();
	raw.pump();
	if (!require_count(raw.records(), 4, "ordered: after fence 1 signalled")) {
		return;
	}
	expect_record(raw.records()[1], Type::Complete, epoch, 1U, 1U, 0U, 0U);
	expect_record(raw.records()[2], Type::Complete, epoch, 2U, 2U, 0U, 0U);
	expect_record(raw.records()[3], Type::Complete, epoch, 3U, 3U, 0U, 0U);

	*f4 = true;
	writer.on_vsync_submitted(4, [f4] { return *f4; });
	writer.on_vsync_submitted(5, [f5] { return *f5; });
	writer.poll();
	raw.pump();
	// Fence 4 signalled, but fence 5 has not, so only Complete 4 is added.
	if (!require_count(raw.records(), 5, "ordered: after fence 4 blocked on 5")) {
		return;
	}
	expect_record(raw.records()[4], Type::Complete, epoch, 4U, 4U, 0U, 0U);

	*f5 = true;
	writer.finish(true);
	raw.pump();
	if (!require_count(raw.records(), 7, "ordered: after finish")) {
		return;
	}
	expect_record(raw.records()[5], Type::Complete, epoch, 5U, 5U, 0U, 0U);
	expect_record(raw.records()[6], Type::Eos, epoch, 6U, 5U, 5U, kParallelGsCompletionEosClean);
	EXPECT(!writer.failed());
}

// The writer exists before any reader. hello() must succeed, nothing may be
// written while no reader is connected, and records must arrive once a reader
// opens the FIFO.
void test_writer_before_reader()
{
	TempFifo fifo;
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());
	writer.on_vsync_submitted(1, [] { return true; });
	writer.poll();
	EXPECT(!writer.failed());

	RawReader raw(fifo.path);
	raw.pump();
	EXPECT(raw.records().empty());

	writer.poll();
	raw.pump();
	if (!require_count(raw.records(), 2, "writer-first: after reader connected")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	expect_record(raw.records()[1], Type::Complete, epoch, 1U, 1U, 0U, 0U);

	writer.finish(true);
	raw.pump();
	if (!require_count(raw.records(), 3, "writer-first: after finish")) {
		return;
	}
	expect_record(raw.records()[2], Type::Eos, epoch, 2U, 1U, 1U, kParallelGsCompletionEosClean);
	EXPECT(!writer.failed());
}

// 1024 unsignalled fences fill the pending queue. The 1026th submission
// overflows: Complete 1 (already signalled) goes out first, then one
// Error(PendingOverflow), then a non-clean Eos. Later submissions are ignored.
void test_pending_overflow()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	writer.on_vsync_submitted(1, [] { return true; });
	const Flag never = make_flag(false);
	for (std::uint64_t index = 2; index <= 1025; ++index) {
		writer.on_vsync_submitted(index, [never] { return *never; });
	}
	EXPECT(!writer.failed());
	writer.on_vsync_submitted(1026, [never] { return *never; });
	EXPECT(writer.failed());
	writer.on_vsync_submitted(1027, [never] { return *never; });
	writer.finish(true);
	raw.pump();

	if (!require_count(raw.records(), 4, "overflow")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	expect_record(raw.records()[1], Type::Complete, epoch, 1U, 1U, 0U, 0U);
	expect_record(raw.records()[2], Type::Error, epoch, 2U, 1026U,
	              static_cast<std::uint64_t>(ParallelGsCompletionError::PendingOverflow), 0U);
	expect_record(raw.records()[3], Type::Eos, epoch, 3U, 1026U, 1026U, 0U);
	EXPECT(writer.failed());
}

// error() drops unsignalled fences, queues the Error, and the Eos is unclean.
void test_error_then_unclean_finish()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	writer.on_vsync_submitted(1, [] { return true; });
	const Flag never = make_flag(false);
	writer.on_vsync_submitted(2, [never] { return *never; });
	writer.error(ParallelGsCompletionError::StreamTruncated, 2);
	EXPECT(writer.failed());
	writer.on_vsync_submitted(3, [] { return true; });
	writer.finish(true);
	raw.pump();

	if (!require_count(raw.records(), 4, "error-then-finish")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	expect_record(raw.records()[1], Type::Complete, epoch, 1U, 1U, 0U, 0U);
	expect_record(raw.records()[2], Type::Error, epoch, 2U, 2U,
	              static_cast<std::uint64_t>(ParallelGsCompletionError::StreamTruncated), 0U);
	expect_record(raw.records()[3], Type::Eos, epoch, 3U, 2U, 2U, 0U);
}

// hello() has not run, but fence 1 is already signalled. The Complete for 1 must
// still go out before the Error, and Hello must come first.
void test_error_before_hello_keeps_signalled_completes()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);

	writer.on_vsync_submitted(1, [] { return true; });
	const Flag never = make_flag(false);
	writer.on_vsync_submitted(2, [never] { return *never; });
	writer.error(ParallelGsCompletionError::StreamTruncated, 2);
	writer.finish(false);
	raw.pump();

	if (!require_count(raw.records(), 4, "error-before-hello")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	expect_record(raw.records()[1], Type::Complete, epoch, 1U, 1U, 0U, 0U);
	expect_record(raw.records()[2], Type::Error, epoch, 2U, 2U,
	              static_cast<std::uint64_t>(ParallelGsCompletionError::StreamTruncated), 0U);
	expect_record(raw.records()[3], Type::Eos, epoch, 3U, 2U, 2U, 0U);
	EXPECT(writer.failed());
}

// The reader goes away mid-stream. The next write gets EPIPE, which must mark
// the writer failed rather than killing the renderer with SIGPIPE, and later
// calls must stay harmless.
void test_reader_disconnect_marks_failed()
{
	TempFifo fifo;
	auto raw = std::make_unique<RawReader>(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());
	writer.on_vsync_submitted(1, [] { return true; });
	writer.poll();
	EXPECT(!writer.failed());

	raw.reset();
	writer.on_vsync_submitted(2, [] { return true; });
	writer.poll();
	EXPECT(writer.failed());

	writer.on_vsync_submitted(3, [] { return true; });
	writer.poll();
	writer.finish(true);
	EXPECT(writer.failed());
}

// A fence still unsignalled at finish() is reported as FenceUnavailable, and
// the Eos is unclean. Costs about 1.5 s of waiting.
void test_unsignalled_fence_at_finish()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	const Flag never = make_flag(false);
	writer.on_vsync_submitted(1, [never] { return *never; });
	writer.finish(true);
	raw.pump();

	if (!require_count(raw.records(), 3, "unsignalled-at-finish")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	expect_record(raw.records()[1], Type::Error, epoch, 1U, 1U,
	              static_cast<std::uint64_t>(ParallelGsCompletionError::FenceUnavailable), 0U);
	expect_record(raw.records()[2], Type::Eos, epoch, 2U, 1U, 1U, 0U);
	EXPECT(writer.failed());
}

// The reader does not read while 3000 signalled fences are submitted, so the
// kernel pipe fills and the writer must keep the rest queued without losing
// or reordering records.
void test_backpressure_keeps_order()
{
	TempFifo fifo;
	RawReader raw(fifo.path);
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	constexpr std::uint64_t kCount = 3000U;
	for (std::uint64_t index = 1; index <= kCount; ++index) {
		writer.on_vsync_submitted(index, [] { return true; });
		writer.poll();
	}
	EXPECT(!writer.failed());

	for (int round = 0; round < 1000 && raw.records().size() < kCount + 1U; ++round) {
		raw.pump();
		writer.poll();
	}
	raw.pump();
	if (!require_count(raw.records(), kCount + 1U, "backpressure")) {
		return;
	}
	const std::uint64_t epoch = expect_hello(raw.records()[0]);
	for (std::uint64_t index = 1; index <= kCount; ++index) {
		expect_record(raw.records()[index], Type::Complete, epoch, index, index, 0U, 0U);
	}

	writer.finish(true);
	raw.pump();
	if (!require_count(raw.records(), kCount + 2U, "backpressure: after finish")) {
		return;
	}
	expect_record(raw.records()[kCount + 1U], Type::Eos, epoch, kCount + 1U, kCount, kCount,
	              kParallelGsCompletionEosClean);
	EXPECT(!writer.failed());
}

#if defined(TAKEDOWN_COMPLETION_TEST_RUNTIME_READER)
using takedown::platform::ParallelGsCompletionReader;
using takedown::platform::ParallelGsCompletionReaderFailure;
using takedown::platform::ParallelGsCompletionReaderState;

// The runtime reader accepts the writer's stream and reports every completion.
void test_runtime_reader_accepts_stream()
{
	TempFifo fifo;
	ParallelGsCompletionReader reader(std::filesystem::path(fifo.path));
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	const Flag f1 = make_flag(false);
	writer.on_vsync_submitted(1, [f1] { return *f1; });
	writer.on_vsync_submitted(2, [] { return true; });
	writer.poll();
	reader.poll();
	EXPECT(reader.statistics().hello_records == 1U);
	EXPECT(!reader.consume_complete_for_raw_vsync(1).has_value());

	*f1 = true;
	writer.poll();
	reader.poll();
	const auto c1 = reader.consume_complete_for_raw_vsync(1);
	EXPECT(c1.has_value() && c1->raw_vsync_index == 1U);
	const auto c2 = reader.consume_complete_for_raw_vsync(2);
	EXPECT(c2.has_value() && c2->raw_vsync_index == 2U);

	writer.on_vsync_submitted(3, [] { return true; });
	writer.finish(true);
	reader.poll();
	const auto c3 = reader.consume_complete_for_raw_vsync(3);
	EXPECT(c3.has_value() && c3->raw_vsync_index == 3U);

	EXPECT(reader.terminal());
	EXPECT(reader.healthy());
	const auto stats = reader.statistics();
	EXPECT(stats.clean_eos);
	EXPECT(stats.eos_records == 1U);
	EXPECT(stats.complete_records == 3U);
	EXPECT(stats.complete_records_consumed == 3U);
	EXPECT(stats.eos_raw_vsync_index == 3U);
	EXPECT(stats.protocol_failures == 0U);
}

// After an overflow the runtime reader reports RendererError and reaches
// Terminal through the non-clean Eos.
void test_runtime_reader_sees_overflow()
{
	TempFifo fifo;
	ParallelGsCompletionReader reader(std::filesystem::path(fifo.path));
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());

	writer.on_vsync_submitted(1, [] { return true; });
	const Flag never = make_flag(false);
	for (std::uint64_t index = 2; index <= 1025; ++index) {
		writer.on_vsync_submitted(index, [never] { return *never; });
	}
	writer.on_vsync_submitted(1026, [never] { return *never; });
	writer.finish(true);
	reader.poll();

	EXPECT(reader.failure() == ParallelGsCompletionReaderFailure::RendererError);
	EXPECT(reader.state() == ParallelGsCompletionReaderState::Terminal);
	EXPECT(!reader.healthy());
	const auto stats = reader.statistics();
	EXPECT(!stats.clean_eos);
	EXPECT(stats.error_records == 1U);
	EXPECT(stats.eos_records == 1U);
	EXPECT(stats.complete_records == 1U);
	EXPECT(stats.eos_raw_vsync_index == 1026U);
}

// Writer constructed before the reader, as in the not-yet-connected case. The
// reader connects later, and its first poll after connection sees everything.
void test_runtime_reader_writer_first()
{
	TempFifo fifo;
	CompletionWriter writer(fifo.path);
	EXPECT(writer.hello());
	writer.on_vsync_submitted(1, [] { return true; });
	writer.poll();
	EXPECT(!writer.failed());

	ParallelGsCompletionReader reader(std::filesystem::path(fifo.path));
	reader.poll();
	EXPECT(reader.statistics().hello_records == 0U);

	writer.poll();
	reader.poll();
	EXPECT(reader.statistics().hello_records == 1U);
	EXPECT(reader.consume_complete_for_raw_vsync(1).has_value());

	writer.finish(true);
	reader.poll();
	EXPECT(reader.terminal());
	EXPECT(reader.healthy());
	EXPECT(reader.statistics().clean_eos);
}
#endif

} // namespace

int main()
{
	test_ordered_completions_and_clean_eos();
	test_writer_before_reader();
	test_pending_overflow();
	test_error_then_unclean_finish();
	test_unsignalled_fence_at_finish();
	test_backpressure_keeps_order();
	test_error_before_hello_keeps_signalled_completes();
	test_reader_disconnect_marks_failed();
#if defined(TAKEDOWN_COMPLETION_TEST_RUNTIME_READER)
	test_runtime_reader_accepts_stream();
	test_runtime_reader_sees_overflow();
	test_runtime_reader_writer_first();
#endif

	if (g_failures != 0) {
		std::fprintf(stderr, "test_completion_writer: %d expectation(s) failed\n", g_failures);
		return 1;
	}
	std::printf("test_completion_writer: all expectations passed\n");
	return 0;
}
#endif
