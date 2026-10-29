#pragma once

// Authoritative shared guest clock ratios (docs/archive/AUDIO_HISTORY.md section 8,
// increment 6). Every modelled audio timeline derives from one master: the
// EE cycle counter. The ratios below are exact integers, so accrual never
// drifts and no wall-clock or host batching can change modelled time.
//
// Evidence:
// - EE core clock 294.912 MHz (PS2 hardware; the same figure the runtime's
//   6144-EE-cycles-per-48 kHz-sample pacing already relies on).
// - IOP SysClock 36.864 MHz = exactly EE/8. The retail RWA.IRX thbase 1.2
//   `USec2SysClock` import converts with `usec * 36864 / 1000` (disassembly
//   executed by `IopR3000Executor`), and `GetSystemTime`/`DelayThread`
//   operate in that same tick domain — the executor's `clock_ticks_` IS the
//   SysClock, not the 37.5 MHz IOP CPU issue rate. Instruction execution is
//   approximated as one SysClock tick per retired instruction (a documented
//   model simplification; the CPU core itself is 37.5 MHz).
// - SPU2 output sample rate 48 kHz: 294.912 MHz / 48 kHz = 6144 EE cycles
//   exactly, consistent with the retail RWA.IRX pitch computation
//   `(sample_rate << 12) / 48000` (docs/archive/AUDIO_HISTORY.md section 8, increment 4).
// - Video: the runtime models VBlank at exactly 60.00 Hz (4,915,200 EE
//   cycles per field). Retail NTSC CRTC output is 59.94005994 Hz
//   (60 * 1000/1001) = 4,920,115.2 EE cycles per field — a recorded,
//   CRTC-wide model discrepancy that is NOT corrected here because it
//   moves timing for every subsystem, not only audio. The ratios above are
//   exact under either field rate.

#include <cstdint>

namespace takedown::audio::guest_clocks {

inline constexpr std::uint32_t kEeHz = 294'912'000U;
inline constexpr std::uint32_t kIopSysClockHz = kEeHz / 8U; // 36,864,000
inline constexpr std::uint32_t kSpu2OutputHz = 48'000U;

// Exact integer ratios (compile-time pinned so a wrong figure cannot build).
inline constexpr std::uint64_t kEeCyclesPerIopSysTick = kEeHz / kIopSysClockHz;
inline constexpr std::uint64_t kEeCyclesPerSpu2Sample = kEeHz / kSpu2OutputHz;

static_assert(kEeHz % kIopSysClockHz == 0U);
static_assert(kEeHz % kSpu2OutputHz == 0U);
static_assert(kEeCyclesPerIopSysTick == 8U);
static_assert(kEeCyclesPerSpu2Sample == 6144U);
// The retail thbase 1.2 conversion constant: 1 us = 36.864 SysClock ticks.
static_assert(kIopSysClockHz % 1000U == 0U);
static_assert(kIopSysClockHz / 1000U == 36'864U);

} // namespace takedown::audio::guest_clocks
