#pragma once
// Renderer side of the optional completion FIFO
// (takedown/platform/parallel_gs_completion.hpp). The runtime reads it with
// src/platform/parallel_gs_completion_reader.cpp; this writer must produce
// exactly the record sequence that reader accepts.
#include "takedown/platform/parallel_gs_completion.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TakedownRenderer
{
class CompletionWriter
{
public:
	// Windows: `path` is "\\.\pipe\...", created with windows_gs_pipe::Output
	// (nonblocking message pipe; the runtime connects as reader).
	// POSIX: `path` is an existing FIFO, opened O_WRONLY | O_NONBLOCK.
	// Throws std::runtime_error when the pipe cannot be created/opened.
	explicit CompletionWriter(const std::string &path);
	~CompletionWriter();
	CompletionWriter(const CompletionWriter &) = delete;
	CompletionWriter &operator=(const CompletionWriter &) = delete;

	// Queues the Hello record. Returns false if it could not be queued.
	bool hello();

	// Called once per raw VBlank, in order, after that VBlank's GPU work has
	// been submitted. raw_vsync_index counts Vsync packets read from the
	// stream, starting at 1. is_signalled() polls the GPU fence without
	// blocking and returns true once the work has completed.
	void on_vsync_submitted(std::uint64_t raw_vsync_index, std::function<bool()> is_signalled);

	// Emits Complete records for the oldest signalled entries, in order, and
	// writes as many queued bytes as the pipe accepts without blocking.
	void poll();

	// Queues an Error record (e.g. StreamTruncated at an unexpected EOF).
	void error(takedown::platform::ParallelGsCompletionError code, std::uint64_t raw_vsync_index);

	// Waits (bounded, up to ~2 s) for pending fences, emits their Complete
	// records, then the Eos record (flag kParallelGsCompletionEosClean when
	// clean), and flushes.
	void finish(bool clean);

	bool failed() const { return failed_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool failed_ = false;
};
}
