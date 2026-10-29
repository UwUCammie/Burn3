#pragma once

// Sleeps the calling thread for about `nanoseconds` without depending on the
// system timer tick. On Windows std::this_thread::sleep_for can round up to the
// scheduler tick (15.6 ms on an idle system unless something raised it),
// which turns a frame-pacing wait of a few milliseconds into an overshoot that
// is itself a late frame. This uses a high-resolution waitable timer where the
// OS has one (Windows 10 1803 and later) and falls back to sleep_for otherwise.
// TAKEDOWN_PRECISE_SLEEP=0 forces the fallback (A/B).
//
// Wake-up may still be late under CPU contention, so callers that need a hard
// deadline keep a short spin after it. Sleeps shorter than 100 microseconds
// return immediately.

#include <chrono>

namespace takedown::platform {

void precise_sleep(std::chrono::nanoseconds duration);

} // namespace takedown::platform
