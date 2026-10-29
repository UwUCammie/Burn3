#include "takedown/platform/precise_sleep.hpp"

#include <cstdlib>
#include <string_view>
#include <thread>

#include <windows.h>

namespace takedown::platform {

namespace {

bool disabled_by_environment() {
    static const bool disabled = [] {
        const char* value = std::getenv("TAKEDOWN_PRECISE_SLEEP");
        return value != nullptr && std::string_view{value} == "0";
    }();
    return disabled;
}

// One high-resolution timer per thread; creation is attempted once.
struct TimerHandle {
    HANDLE handle{};
    bool attempted{};

    ~TimerHandle() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
};

} // namespace

void precise_sleep(const std::chrono::nanoseconds duration) {
    if (duration < std::chrono::microseconds(100)) {
        return;
    }

    thread_local TimerHandle timer;
    if (!timer.attempted) {
        timer.attempted = true;
        if (!disabled_by_environment()) {
            timer.handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
        }
    }

    if (timer.handle != nullptr) {
        LARGE_INTEGER due;
        // Negative values are relative, in 100 ns units.
        due.QuadPart = -(duration.count() / 100);
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) != 0 &&
            WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }

    std::this_thread::sleep_for(duration);
}

} // namespace takedown::platform
