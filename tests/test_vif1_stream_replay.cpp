// Replay test and microbenchmark for the VIF1 DMA chain -> VU1 UNPACK/MSCAL ->
// GIF plumbing on the emulation thread.
//
// A synthetic stream shaped like a retail race frame (small CNT/REF/CALL/RET
// DMA tags carrying STCYCL + UNPACK V4-32 geometry from main RAM, MSCAL
// launches whose program XGKICKs a packet, DIRECT PATH2 packets, plus a
// concurrent PATH3 texture upload) is driven through the timed VIF1/GIF
// device model under both the run-ahead and the fixed-chunk service. Each
// scenario folds everything guest visible (the raw GIF QW stream with path and
// packet boundaries, per-step DMAC/VIF/GIF registers, VU1 data memory, cycle
// counts, activity counters) into one 64-bit digest. The digests are pinned to
// the values the original word-at-a-time implementation produced, so any
// change to device behaviour or timing fails the test.
//
//   takedown_vif1_stream_replay_tests              verify digests
//   takedown_vif1_stream_replay_tests --print      print the digests
//   takedown_vif1_stream_replay_tests --benchmark  ns per quadword

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <atomic>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using takedown::guest::EeMemory;
using takedown::guest::GifPath;
using takedown::guest::GsQuadword;
using takedown::guest::GsRawGifObserver;

constexpr std::uint32_t kGifControl = 0x10003000U;
constexpr std::uint32_t kGifMode = 0x10003010U;
constexpr std::uint32_t kGifStatus = 0x10003020U;
constexpr std::uint32_t kGifChcr = 0x1000a000U;
constexpr std::uint32_t kGifTadr = 0x1000a030U;
constexpr std::uint32_t kVif1Chcr = 0x10009000U;
constexpr std::uint32_t kVif1Tadr = 0x10009030U;
constexpr std::uint32_t kVif1Stat = 0x10003c00U;
constexpr std::uint32_t kVu1Micro = 0x11008000U;
constexpr std::uint32_t kVu1Data = 0x1100c000U;
constexpr std::uint32_t kKickVector = 0x40U;

struct Digest {
    std::uint64_t value{0xcbf29ce484222325ULL};
    void mix(const std::uint64_t word) noexcept {
        value ^= word + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value *= 0x100000001b3ULL;
    }
};

class DigestObserver final : public GsRawGifObserver {
  public:
    explicit DigestObserver(Digest& digest) : digest_(digest) {}
    void on_gs_gif_quadword(const GifPath path, const GsQuadword& quadword) override {
        digest_.mix(static_cast<std::uint32_t>(path));
        digest_.mix(quadword[0] | (static_cast<std::uint64_t>(quadword[1]) << 32U));
        digest_.mix(quadword[2] | (static_cast<std::uint64_t>(quadword[3]) << 32U));
        ++quadwords;
    }
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override { return true; }
    void on_gs_gif_packet_eop(const GifPath path, const bool primary) noexcept override {
        digest_.mix(0x100U | static_cast<std::uint32_t>(path) | (primary ? 0x10U : 0U));
    }
    void on_gs_gif_reset() noexcept override { digest_.mix(0x200U); }
    std::uint64_t quadwords{};

  private:
    Digest& digest_;
};

GsQuadword dma_tag(const std::uint32_t id, const std::uint32_t qwc, const std::uint32_t addr = 0U) {
    return {(id << 28U) | qwc, addr, 0U, 0U};
}

GsQuadword gif_tag(const std::uint32_t loops, const std::uint32_t format,
                   const std::uint32_t register_count, const std::uint64_t registers,
                   const bool eop) {
    const auto low = static_cast<std::uint64_t>(loops) |
                     (eop ? std::uint64_t{1U} << 15U : 0U) |
                     (static_cast<std::uint64_t>(format) << 58U) |
                     (static_cast<std::uint64_t>(register_count & 0x0fU) << 60U);
    return {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
            static_cast<std::uint32_t>(registers), static_cast<std::uint32_t>(registers >> 32U)};
}

GsQuadword ad_write(const std::uint8_t address, const std::uint64_t value) {
    return {static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32U), address,
            0U};
}

void write_vu1_programs(EeMemory& memory) {
    const auto pair = [&memory](const std::uint32_t index, const std::uint32_t lower,
                                const std::uint32_t upper) {
        memory.write_u32_le(kVu1Micro + index * 8U, lower);
        memory.write_u32_le(kVu1Micro + index * 8U + 4U, upper);
    };
    constexpr std::uint32_t kNopLower = 0x8000033cU;
    constexpr std::uint32_t kNopUpper = 0x000002ffU;
    constexpr std::uint32_t kEndUpper = 0x400002ffU;
    constexpr std::uint32_t kIaddiuVi15 = 0x100f0000U | kKickVector;
    constexpr std::uint32_t kXgkickVi15 = 0x80007efcU;
    const auto program = [&](std::uint32_t start, const std::uint32_t before_kick,
                             const std::uint32_t after_kick) {
        pair(start++, kIaddiuVi15, kNopUpper);
        for (std::uint32_t index = 0U; index < before_kick; ++index) {
            pair(start++, kNopLower, kNopUpper);
        }
        pair(start++, kXgkickVi15, kNopUpper);
        for (std::uint32_t index = 0U; index < after_kick; ++index) {
            pair(start++, kNopLower, kNopUpper);
        }
        pair(start++, kNopLower, kEndUpper);
        pair(start, kNopLower, kNopUpper);
    };
    program(0U, 10U, 2U);
    program(100U, 40U, 20U);
    const auto vector = [&memory](const std::uint32_t index, const GsQuadword& value) {
        memory.write_u128_le(kVu1Data + index * 16U, value);
    };
    // A 121-quadword PATH1 packet (40 loops of ST/RGBAQ/XYZ2), like a small
    // retail draw.
    vector(kKickVector, gif_tag(40U, 0U, 3U, 0x512U, true));
    for (std::uint32_t index = 0U; index < 120U; ++index) {
        vector(kKickVector + 1U + index,
               {index * 7U + 1U, index * 13U + 5U, index * 17U + 9U, index});
    }
}

std::atomic<bool> g_sampling{false};
std::uint32_t g_draws = 220U;
std::uint32_t g_launch_period = 1U;  // an MSCAL every Nth draw

// Extra device configurations layered on a scenario (bit set in Scenario::variant).
constexpr std::uint32_t kVariantImt = 1U;       // GIF_MODE.IMT: PATH3 IMAGE may be sliced
constexpr std::uint32_t kVariantMask = 2U;      // MSKPATH3 windows inside the stream
constexpr std::uint32_t kVariantPause = 4U;     // GIF_CTRL.PSE toggled between service steps
constexpr std::uint32_t kVariantRawOnly = 8U;   // raw passthrough: spans reach the observer
constexpr std::uint32_t kVariantDirectHl = 16U; // some DIRECTs are DIRECTHL
std::uint32_t g_variant = 0U;

struct Layout {
    std::uint32_t vif_root{};
    std::uint32_t path3_root{};
    std::uint64_t vif_quadwords{};
    std::uint64_t path3_quadwords{};
};

class ChainBuilder {
  public:
    ChainBuilder(EeMemory& memory, const std::uint32_t base) : memory_(memory), cursor_(base) {}
    [[nodiscard]] std::uint32_t cursor() const noexcept { return cursor_; }
    void align() noexcept { cursor_ = (cursor_ + 15U) & ~15U; }
    std::uint32_t put(const GsQuadword& quadword) {
        const auto at = cursor_;
        memory_.write_u128_le(at, quadword);
        cursor_ += 16U;
        ++quadwords;
        return at;
    }
    std::uint64_t quadwords{};

  private:
    EeMemory& memory_;
    std::uint32_t cursor_;
};

// Appends 32-bit words as quadwords, padding the tail with VIF NOPs.
void put_words(ChainBuilder& out, std::vector<std::uint32_t> words) {
    while (words.size() % 4U != 0U) {
        words.push_back(0U);
    }
    for (std::size_t index = 0U; index < words.size(); index += 4U) {
        out.put({words[index], words[index + 1U], words[index + 2U], words[index + 3U]});
    }
}

Layout build_scene(EeMemory& memory, const std::uint32_t seed, const bool with_path3) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<std::uint32_t> word;
    Layout layout;
    ChainBuilder data(memory, 0x100000U);  // referenced vertex data
    ChainBuilder chain(memory, 0x10000U);  // the tag chain itself
    layout.vif_root = chain.cursor();

    // Frame preamble: buffer geometry for TOPS-relative unpacks.
    {
        std::vector<std::uint32_t> words;
        words.push_back(0x03000200U);  // BASE 0x200
        words.push_back(0x02000100U);  // OFFSET 0x100
        words.push_back(0x01000404U);  // STCYCL CL=4 WL=4
        words.push_back(0x05000000U);  // MODE 0
        chain.put(dma_tag(1U, 1U));
        put_words(chain, words);
    }

    ChainBuilder sub(memory, 0x140000U);  // CALL/RET subroutines
    std::uint32_t cl = 4U;
    std::uint32_t wl = 4U;
    const std::uint32_t kDraws = g_draws;
    for (std::uint32_t draw = 0U; draw < kDraws; ++draw) {
        const auto kind = draw % 11U;
        // UNPACK format for this draw: mostly V4-32 (retail geometry), the
        // rest narrower / three-component / masked / filling-write forms.
        std::uint32_t command = 0x6cU;
        std::uint32_t bytes_per_vector = 16U;
        bool masked = false;
        if (kind == 3U) {
            command = 0x6dU;  // V4-16
            bytes_per_vector = 8U;
        } else if (kind == 6U) {
            command = 0x68U;  // V3-32
            bytes_per_vector = 12U;
        } else if (kind == 9U) {
            command = 0x6eU;  // V4-8
            bytes_per_vector = 4U;
        } else if (kind == 10U) {
            masked = true;
        }
        const auto count = std::uniform_int_distribution<std::uint32_t>(8U, 96U)(random);
        const bool relative = draw % 2U == 0U;
        const auto address = relative ? 0x8000U | (draw % 3U) * 8U
                                      : 0x100U + (draw % 4U) * 0x20U;
        std::vector<std::uint32_t> words;
        if (masked) {
            words.push_back(0x20000000U);
            words.push_back(0x4444aa11U);
        }
        if (kind == 8U) {
            cl = 2U;
            wl = 4U;  // filling write
            words.push_back(0x01000402U);
        }
        while (words.size() % 4U != 3U) words.push_back(0U);
        words.push_back((masked ? 0x10000000U : 0U) | (command << 24U) | (count << 16U) | address);
        const auto source_vectors =
            wl <= cl ? count : cl * (count / wl) + std::min(count % wl, cl);
        const auto data_words = (source_vectors * bytes_per_vector + 3U) / 4U;
        std::vector<std::uint32_t> data_payload(data_words);
        for (auto& value : data_payload) {
            value = word(random);
        }
        const auto data_quadwords = (data_words + 3U) / 4U;
        // The UNPACK command lives in a short CNT tag; its data is a REF to
        // the separate vertex region (static model data), except for a
        // tenth of the draws which carry the data inline.
        if (draw % 10U == 4U) {
            words.insert(words.end(), data_payload.begin(), data_payload.end());
            chain.put(dma_tag(1U, static_cast<std::uint32_t>((words.size() + 3U) / 4U)));
            put_words(chain, words);
        } else {
            chain.put(dma_tag(1U, static_cast<std::uint32_t>((words.size() + 3U) / 4U)));
            put_words(chain, words);
            data.align();
            const auto data_address = data.cursor();
            put_words(data, data_payload);
            chain.put(dma_tag(3U, data_quadwords, data_address));
        }
        // Launch + a DIRECT packet.
        std::vector<std::uint32_t> tail;
        if (kind == 8U) {
            cl = 4U;
            wl = 4U;
            tail.push_back(0x01000404U);
        }
        if (draw % g_launch_period == 0U) {
            tail.push_back(0x14000000U | (draw % 7U == 0U ? 100U : 0U));
        }
        if ((g_variant & kVariantMask) != 0U) {
            if (draw % 9U == 4U) {
                tail.push_back(0x06008000U);  // MSKPATH3 on
            } else if (draw % 9U == 6U) {
                tail.push_back(0x06000000U);  // MSKPATH3 off
            }
        }
        if (draw % 3U == 0U) {
            const auto loops = 1U + draw % 3U;
            while (tail.size() % 4U != 3U) tail.push_back(0U);
            const bool direct_hl = (g_variant & kVariantDirectHl) != 0U && draw % 2U == 0U;
            tail.push_back((direct_hl ? 0x51000000U : 0x50000000U) | (loops + 1U));
            for (const auto value : gif_tag(loops, 0U, 1U, 0x0eU, true)) {
                tail.push_back(value);
            }
            for (std::uint32_t loop = 0U; loop < loops; ++loop) {
                for (const auto value : ad_write(0x4cU, word(random))) {
                    tail.push_back(value);
                }
            }
        }
        if (draw % 13U == 12U && g_launch_period <= 8U) {
            tail.push_back(0x17000000U);  // MSCNT
        }
        const auto tail_qw = static_cast<std::uint32_t>((tail.size() + 3U) / 4U);
        if (draw % 5U == 2U) {
            // The tail hides behind a CALL/RET subroutine.
            const auto sub_at = sub.cursor();
            sub.put(dma_tag(1U, tail_qw));
            put_words(sub, tail);
            sub.put(dma_tag(6U, 0U));
            chain.put(dma_tag(5U, 0U, sub_at));
        } else {
            chain.put(dma_tag(1U, tail_qw));
            put_words(chain, tail);
        }
    }
    {
        std::vector<std::uint32_t> words{0x06000000U, 0x11000000U, 0x13000000U};
        chain.put(dma_tag(7U, 1U));
        put_words(chain, words);
    }
    layout.vif_quadwords = chain.quadwords + data.quadwords + sub.quadwords;

    if (with_path3) {
        ChainBuilder path3(memory, 0x180000U);
        layout.path3_root = path3.cursor();
        for (std::uint32_t packet = 0U; packet < 6U; ++packet) {
            const auto loops = 512U + packet * 64U;
            const bool last = packet + 1U == 6U;
            // Each packet in its own tag so tag boundaries are exercised.
            path3.put(dma_tag(last ? 7U : 1U, loops + 1U));
            path3.put(gif_tag(loops, 2U, 0U, 0U, true));
            for (std::uint32_t index = 0U; index < loops; ++index) {
                path3.put({word(random), word(random), word(random), word(random)});
            }
        }
        layout.path3_quadwords = path3.quadwords;
    }
    return layout;
}

enum class Service { RunAhead, Chunked };

struct Result {
    std::uint64_t digest{};
    std::uint64_t cycles{};
    std::uint64_t gif_quadwords{};
    std::uint64_t source_quadwords{};
    std::string summary;
    double seconds{};
};

Result run(const std::uint32_t seed, const Service service, const bool with_path3,
           const bool sample_registers, const std::uint32_t repeats) {
    using Clock = std::chrono::steady_clock;
    Digest digest;
    DigestObserver observer(digest);
    EeMemory memory(0x200000U);
    memory.set_gs_capture_enabled(true);
    memory.set_gif_dmac_timing_enabled(true);
    memory.set_vif1_dmac_timing_enabled(true);
    memory.set_gif_blocked_arbiter_cache_enabled(true);
    // The shipping configuration: all of these default on in the app.
    memory.set_fast_gif_status_read_enabled(true);
    memory.set_gif_path3_batch_enabled(true);
    memory.set_gif_idle_pump_gate_enabled(true);
    memory.gs_capture().set_raw_gif_observer(&observer);
    if ((g_variant & kVariantRawOnly) != 0U) {
        memory.gs_capture().set_raw_passthrough_only(true);
    }
    if ((g_variant & kVariantImt) != 0U) {
        memory.write_u32_le(kGifMode, 0x4U);
    }
    write_vu1_programs(memory);
    const auto layout = build_scene(memory, seed, with_path3);

    Result result;
    // The first repeat is warm-up (VU1 program compilation, page faults) and
    // is excluded from timing and sampling when more than one is requested.
    g_sampling = repeats == 1U;
    auto begin = Clock::now();
    for (std::uint32_t repeat = 0U; repeat < repeats; ++repeat) {
        if (repeat == 1U) {
            begin = Clock::now();
            result.source_quadwords = 0U;
            g_sampling = true;
        }
        memory.write_u32_le(kVif1Tadr, layout.vif_root);
        memory.write_u32_le(kVif1Chcr, 0x00000105U);
        if (with_path3) {
            memory.write_u32_le(kGifTadr, layout.path3_root);
            memory.write_u32_le(kGifChcr, 0x00000104U);
        }
        result.source_quadwords += layout.vif_quadwords + layout.path3_quadwords;
        std::uint32_t guard = 0U;
        while (memory.vif1_dmac_in_flight() || memory.gif_dmac_in_flight()) {
            if (++guard > 4000000U) {
                std::cerr << "scenario did not drain\n";
                break;
            }
            if ((g_variant & kVariantPause) != 0U) {
                const auto phase = guard % 41U;
                if (phase == 7U) {
                    memory.write_u32_le(kGifControl, 0x8U);
                } else if (phase == 11U) {
                    memory.write_u32_le(kGifControl, 0U);
                }
            }
            std::uint64_t advanced{};
            if (service == Service::RunAhead) {
                advanced = memory.run_graphics_ahead(std::uint64_t{600} + (guard % 7U) * 997U);
                if (advanced == 0U) {
                    // Blocked on a guest-visible edge (e.g. a masked path).
                    memory.write_u32_le(kGifControl, 0U);
                    if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress()) {
                        break;
                    }
                }
            } else {
                constexpr std::uint64_t kStep = 24U;
                if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress()) {
                    memory.write_u32_le(kGifControl, 0U);
                    if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress()) {
                        break;
                    }
                }
                memory.advance_vif1_dmac(kStep);
                memory.advance_gif_dmac(kStep);
                advanced = kStep;
            }
            result.cycles += advanced;
            digest.mix(advanced);
            if (sample_registers) {
                for (const auto address :
                     {kVif1Stat, 0x10003ce0U, 0x10003cc0U, 0x10003cd0U, 0x10003c40U, kGifStatus,
                      kVif1Chcr, kVif1Chcr + 0x10U, kVif1Chcr + 0x20U, kVif1Chcr + 0x30U,
                      kVif1Chcr + 0x40U, kVif1Chcr + 0x50U, kGifChcr, kGifChcr + 0x10U,
                      kGifChcr + 0x20U, kGifChcr + 0x30U}) {
                    digest.mix(memory.read_u32_le(address));
                }
            }
            while (memory.take_completed_dmac_channel().has_value()) {
            }
        }
        if (memory.vif1_dmac_in_flight() || memory.gif_dmac_in_flight()) {
            std::cerr << "scenario stalled: vif1 " << memory.vif1_dmac_in_flight() << " gif "
                      << memory.gif_dmac_in_flight() << " vif1_stat " << std::hex
                      << memory.read_u32_le(kVif1Stat) << " gif_stat "
                      << memory.read_u32_le(kGifStatus) << std::dec << " repeat " << repeat
                      << " unsupported " << memory.vu1_activity().vif1_unsupported_commands
                      << " first 0x" << std::hex
                      << memory.vu1_activity().vif1_first_unsupported_word << std::dec
                      << " faults " << memory.vu1_activity().vif1_chain_faults << "\n";
        }
    }
    g_sampling = false;
    result.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    for (std::uint32_t index = 0U; index < 0x4000U / 4U; ++index) {
        digest.mix(memory.read_u32_le(kVu1Data + index * 4U));
    }
    for (const auto count : memory.dmac_completion_counts()) {
        digest.mix(count);
    }
    const auto& activity = memory.vu1_activity();
    digest.mix(activity.micro_runs_completed);
    digest.mix(activity.xgkick_packets_completed);
    digest.mix(activity.vif1_unpack_commands);
    digest.mix(activity.vif1_unpack_vectors_written);
    digest.mix(activity.vif1_vumem_words_written);
    digest.mix(activity.vif1_chain_words_submitted);
    digest.mix(activity.vif1_chain_tags_processed);
    digest.mix(memory.gs_capture().register_writes().size());
    digest.mix(result.cycles);
    result.digest = digest.value;
    result.gif_quadwords = observer.quadwords;
    result.summary = std::to_string(activity.micro_runs_completed) + " VU runs, " + std::to_string(activity.vif1_unpack_commands) + " unpacks, " + std::to_string(activity.xgkick_packets_completed) + "/" + std::to_string(activity.xgkick_packets) + " kicks " + std::to_string(activity.xgkick_quadwords) + " QW " + std::to_string(activity.xgkick_faults) + " faults, " + std::to_string(activity.vif1_unsupported_commands) + " unsupported";
    return result;
}

// ---- Asynchronous graphics window ---------------------------------------
//
// The same EE-side script (kick a chain, run the device ahead, do "EE work",
// observe) runs with the window off (everything synchronous) and on (the
// device model on a worker thread). The window is only an ownership hand-off,
// so everything guest visible -- the raw GIF stream with path and packet
// boundaries, registers sampled after each observation, VU1 data memory,
// completion counts, activity counters -- must be identical.

using takedown::runtime::GraphicsAsyncFence;
using takedown::runtime::GraphicsAsyncMode;

constexpr std::uint64_t kHugeRunAhead = 64ULL * 4'915'200ULL;

struct AsyncRun {
    std::uint64_t digest{};
    takedown::runtime::GraphicsAsyncStatistics stats{};
    std::uint64_t gif_quadwords{};
    double seconds{};
};

// How the window is left. 0: a VIF1_STAT load; 1: an explicit fence;
// 2: a VU1 data-memory load; 3: the cycle bound (EE time passes until the
// latency elapses).
void leave_window(EeMemory& memory, Digest& digest, const std::uint32_t how,
                  const std::uint64_t latency) {
    switch (how % 4U) {
    case 0U:
        digest.mix(memory.read_u32_le(kVif1Stat));
        break;
    case 1U:
        memory.graphics_async_fence();
        break;
    case 2U:
        digest.mix(memory.read_u32_le(kVu1Data + 0x40U));
        break;
    default: {
        // Service calls of arbitrary chunking until the bound has passed.
        std::uint64_t elapsed{};
        std::uint64_t chunk = 701U;
        while (elapsed < latency + 5000U) {
            memory.advance_vif1_dmac(chunk);
            memory.advance_gif_dmac(chunk);
            elapsed += chunk;
            chunk = chunk % 1999U + 311U;
        }
        break;
    }
    }
}

AsyncRun run_async_script(const std::uint32_t seed, const bool with_path3,
                          const std::uint32_t period, const std::uint32_t variant,
                          const std::uint32_t pattern, const GraphicsAsyncMode mode,
                          const std::uint64_t latency, const std::uint64_t run_limit,
                          const std::uint32_t repeats) {
    using Clock = std::chrono::steady_clock;
    g_launch_period = period;
    g_variant = variant;
    Digest digest;
    DigestObserver observer(digest);
    EeMemory memory(0x200000U);
    memory.set_gs_capture_enabled(true);
    memory.set_gif_dmac_timing_enabled(true);
    memory.set_vif1_dmac_timing_enabled(true);
    memory.set_gif_blocked_arbiter_cache_enabled(true);
    memory.set_fast_gif_status_read_enabled(true);
    memory.set_gif_path3_batch_enabled(true);
    memory.set_gif_idle_pump_gate_enabled(true);
    memory.gs_capture().set_raw_gif_observer(&observer);
    if ((variant & kVariantRawOnly) != 0U) {
        memory.gs_capture().set_raw_passthrough_only(true);
    }
    if ((variant & kVariantImt) != 0U) {
        memory.write_u32_le(kGifMode, 0x4U);
    }
    write_vu1_programs(memory);
    const auto layout = build_scene(memory, seed, with_path3);
    memory.configure_graphics_async(mode, latency, 0U);

    const auto begin = Clock::now();
    for (std::uint32_t repeat = 0U; repeat < repeats; ++repeat) {
        memory.write_u32_le(kVif1Tadr, layout.vif_root);
        memory.write_u32_le(kVif1Chcr, 0x00000105U);
        if (with_path3) {
            memory.write_u32_le(kGifTadr, layout.path3_root);
            memory.write_u32_le(kGifChcr, 0x00000104U);
        }
        std::uint32_t guard = 0U;
        for (;;) {
            memory.graphics_async_fence();
            if (!memory.vif1_dmac_in_flight() && !memory.gif_dmac_in_flight()) {
                break;
            }
            if (++guard > 20000U) {
                std::cerr << "async scenario did not drain\n";
                break;
            }
            if ((variant & kVariantPause) != 0U) {
                const auto phase = guard % 41U;
                if (phase == 7U) {
                    memory.write_u32_le(kGifControl, 0x8U);
                } else if (phase == 11U) {
                    memory.write_u32_le(kGifControl, 0U);
                }
            }
            static_cast<void>(memory.run_graphics_ahead(run_limit + (guard % 5U) * 311U));
            // EE work while the device runs: bus traffic that must not look
            // at it (RAM, scratchpad, timers), queries answered from state
            // which cannot differ, ordered stream markers, service calls.
            memory.write_u32_le(0x1f0000U + (guard % 64U) * 4U, guard);
            const auto ram_value = memory.read_u32_le(0x1f0000U + (guard % 64U) * 4U);
            const auto pattern_step = pattern + guard;
            if (pattern_step % 3U == 1U) {
                memory.run_in_graphics_order([&digest, guard]() { digest.mix(0xabc0U + guard); });
            }
            if (pattern_step % 5U == 2U) {
                for (std::uint32_t index = 0U; index < 6U; ++index) {
                    memory.advance_vif1_dmac(997U);
                    memory.advance_gif_dmac(997U);
                }
            }
            leave_window(memory, digest, pattern_step / 2U, latency);
            // From here the window is closed in both modes.
            digest.mix(ram_value);
            if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress() &&
                (memory.vif1_dmac_in_flight() || memory.gif_dmac_in_flight())) {
                // Blocked on a guest-visible edge (a masked or paused path).
                memory.write_u32_le(kGifControl, 0U);
                if (!memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress()) {
                    break;
                }
            }
            for (const auto address :
                 {kVif1Stat, kGifStatus, kVif1Chcr, kVif1Chcr + 0x30U, kGifChcr}) {
                digest.mix(memory.read_u32_le(address));
            }
            while (memory.take_completed_dmac_channel().has_value()) {
            }
        }
    }
    AsyncRun result;
    result.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    memory.graphics_async_fence();
    for (std::uint32_t index = 0U; index < 0x4000U / 4U; ++index) {
        digest.mix(memory.read_u32_le(kVu1Data + index * 4U));
    }
    for (const auto count : memory.dmac_completion_counts()) {
        digest.mix(count);
    }
    const auto& activity = memory.vu1_activity();
    digest.mix(activity.micro_runs_completed);
    digest.mix(activity.xgkick_packets_completed);
    digest.mix(activity.vif1_unpack_commands);
    digest.mix(activity.vif1_unpack_vectors_written);
    digest.mix(activity.vif1_vumem_words_written);
    digest.mix(activity.vif1_chain_words_submitted);
    digest.mix(activity.vif1_chain_tags_processed);
    digest.mix(memory.gs_capture().register_writes().size());
    result.digest = digest.value;
    result.gif_quadwords = observer.quadwords;
    result.stats = memory.graphics_async_statistics();
    return result;
}

struct AsyncScenario {
    std::uint32_t seed;
    bool with_path3;
    std::uint32_t period;
    std::uint32_t variant;
    std::uint32_t pattern;
    std::uint64_t latency;
    std::uint64_t run_limit = kHugeRunAhead;  // small: the device is left mid-chain
};

// Chosen to cover every way out of a window (bus load, fence, VU1 load, the
// cycle bound), ordered stream markers, small and large bounds, the PATH3
// competitor, and the device variants that block on guest writes.
constexpr AsyncScenario kAsyncScenarios[] = {
    {101U, false, 1U, 0U, 0U, 1'500'000U},
    {102U, true, 1U, 0U, 1U, 1'500'000U},
    {103U, true, 8U, 0U, 2U, 40'000U},
    {104U, false, 8U, 0U, 3U, 1'500'000U},
    {105U, true, 8U, kVariantImt, 4U, 20'000U},
    {106U, true, 8U, kVariantImt | kVariantMask, 5U, 1'500'000U},
    {107U, true, 100000U, kVariantPause | kVariantMask, 6U, 9'000U},
    {108U, true, 8U, kVariantRawOnly, 7U, 1'500'000U},
    {109U, true, 8U, kVariantImt | kVariantMask | kVariantPause | kVariantDirectHl, 2U, 100U},
    {110U, false, 4U, kVariantDirectHl, 0U, 1U},
    // Short run-ahead limits leave the device mid-chain, so the service calls
    // owed at the close are applied to a device which still has work.
    {111U, true, 8U, 0U, 0U, 1'500'000U, 700U},
    {112U, true, 1U, kVariantImt, 3U, 50'000U, 2'500U},
    {113U, true, 8U, kVariantImt | kVariantMask | kVariantPause, 5U, 1'500'000U, 1'200U},
    {114U, false, 100000U, kVariantRawOnly, 6U, 8'000U, 4'000U},
};

// Behavior that only exists while a window is open.
int check_window_semantics() {
    int failures{};
    const auto expect = [&failures](const bool condition, const char* what) {
        if (!condition) {
            std::cerr << "FAIL: window semantics: " << what << '\n';
            ++failures;
        }
    };
    g_launch_period = 8U;
    g_variant = 0U;
    Digest digest;
    DigestObserver observer(digest);
    EeMemory memory(0x200000U);
    memory.set_gs_capture_enabled(true);
    memory.set_gif_dmac_timing_enabled(true);
    memory.set_vif1_dmac_timing_enabled(true);
    memory.gs_capture().set_raw_gif_observer(&observer);
    write_vu1_programs(memory);
    const auto layout = build_scene(memory, 7U, false);
    constexpr std::uint64_t kLatency = 200'000U;
    memory.configure_graphics_async(GraphicsAsyncMode::On, kLatency, 0U);

    memory.write_u32_le(kVif1Tadr, layout.vif_root);
    memory.write_u32_le(kVif1Chcr, 0x00000105U);
    const auto status_before = memory.dmac_status();
    expect(memory.vif1_dmac_can_progress(), "a kicked chain can progress");
    static_cast<void>(memory.run_graphics_ahead(kHugeRunAhead));
    expect(memory.graphics_async_window_open(), "run-ahead opened a window");
    expect(!memory.vif1_dmac_can_progress() && !memory.gif_dmac_can_progress(),
           "nothing for the emulation thread to service");
    expect(!memory.vif1_dmac_in_flight(), "no transfer reported in flight");
    expect(memory.graphics_dmacs_idle(), "service calls are plain clock ticks");
    expect(memory.queued_dmac_completion_count() == 0U, "no completion visible yet");
    expect(!memory.take_completed_dmac_channel().has_value(), "nothing to take yet");
    expect(memory.dmac_status() == status_before, "D_STAT unchanged inside the window");
    expect(memory.read_u32_le(0x1000e010U) == status_before, "D_STAT load unchanged");
    expect(memory.graphics_async_window_open(), "RAM, D_STAT and queries keep it open");
    const auto deadline = memory.graphics_service_deadline();
    expect(deadline.has_internal_deadline() && deadline.cycles_until_event == kLatency,
           "the scheduler sees the latency bound");
    memory.advance_idle_vif1_dmac(kLatency - 2U);
    memory.advance_idle_gif_dmac(kLatency - 2U);
    expect(memory.graphics_async_window_open(), "still open just short of the bound");
    expect(memory.graphics_service_deadline().cycles_until_event == 2U, "bound counts down");
    memory.advance_vif1_dmac(1U);
    expect(memory.graphics_async_window_open(), "one cycle short of the bound");
    memory.advance_vif1_dmac(1U);
    expect(!memory.graphics_async_window_open(), "the bound closes the window");
    expect(memory.queued_dmac_completion_count() == 1U, "completion visible after the close");
    expect((memory.dmac_status() & 2U) != 0U && (memory.dmac_status() & ~2U) == status_before,
           "completion set CIS1 in D_STAT");
    expect(!memory.vif1_dmac_in_flight(), "chain finished");
    const auto stats = memory.graphics_async_statistics();
    expect(stats.windows == 1U && stats.closed_by[static_cast<std::size_t>(
                                      GraphicsAsyncFence::Latency)] == 1U,
           "statistics saw one latency-closed window");
    expect(stats.job_nanoseconds != 0U, "the worker did the work");
    return failures;
}

int check_worker() {
    int failures{};
    const auto expect = [&failures](const bool condition, const char* what) {
        if (!condition) {
            std::cerr << "FAIL: graphics async worker: " << what << '\n';
            ++failures;
        }
    };
    takedown::runtime::GraphicsAsyncWorker worker;
    std::vector<int> order;
    bool on_worker = false;
    for (int index = 0; index < 100; ++index) {
        worker.post([&order, &on_worker, index]() {
            order.push_back(index);
            on_worker = takedown::runtime::on_graphics_async_worker_thread();
        });
    }
    std::uint64_t waited{};
    worker.wait_idle(waited);
    expect(order.size() == 100U && std::is_sorted(order.begin(), order.end()),
           "tasks run once, in submission order");
    expect(on_worker && !takedown::runtime::on_graphics_async_worker_thread(),
           "tasks run on the worker thread only");
    expect(worker.idle(), "idle after wait");
    worker.post([]() { throw std::runtime_error("boom"); });
    bool later_ran = false;
    worker.post([&later_ran]() { later_ran = true; });
    bool thrown = false;
    try {
        worker.wait_idle(waited);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    expect(thrown, "a task exception is rethrown on the waiting thread");
    expect(!later_ran, "tasks queued behind a failed one are skipped");
    worker.post([&later_ran]() { later_ran = true; });
    worker.wait_idle(waited);
    expect(later_ran, "the worker recovers after the error was delivered");
    return failures;
}

// A benchmark of the mechanism: while the device runs, the emulation thread
// does `ee_fraction` of the work it takes (a spin standing for guest code).
void benchmark_async() {
    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t kRepeats = 40U;
    g_launch_period = 1U;
    g_variant = 0U;
    const auto time_run = [&](const GraphicsAsyncMode mode, const double spin_seconds) {
        Digest digest;
        DigestObserver observer(digest);
        EeMemory memory(0x200000U);
        memory.set_gs_capture_enabled(true);
        memory.set_gif_dmac_timing_enabled(true);
        memory.set_vif1_dmac_timing_enabled(true);
        memory.gs_capture().set_raw_gif_observer(&observer);
        write_vu1_programs(memory);
        const auto layout = build_scene(memory, 3U, true);
        memory.configure_graphics_async(mode, 4'000'000U, 0U);
        const auto begin = Clock::now();
        for (std::uint32_t repeat = 0U; repeat < kRepeats; ++repeat) {
            memory.write_u32_le(kVif1Tadr, layout.vif_root);
            memory.write_u32_le(kVif1Chcr, 0x00000105U);
            memory.write_u32_le(kGifTadr, layout.path3_root);
            memory.write_u32_le(kGifChcr, 0x00000104U);
            static_cast<void>(memory.run_graphics_ahead(kHugeRunAhead));
            const auto spin_until = Clock::now() + std::chrono::duration<double>(spin_seconds);
            while (Clock::now() < spin_until) {
            }
            memory.graphics_async_fence();
            while (memory.take_completed_dmac_channel().has_value()) {
            }
        }
        return std::chrono::duration<double>(Clock::now() - begin).count() / kRepeats;
    };
    const double job = time_run(GraphicsAsyncMode::Off, 0.0);
    std::cout << "async window: device job " << job * 1e3 << " ms per chain\n";
    for (const double fraction : {0.0, 0.5, 1.0, 2.0}) {
        const double spin = job * fraction;
        const double off = time_run(GraphicsAsyncMode::Off, spin);
        const double on = time_run(GraphicsAsyncMode::On, spin);
        std::cout << "  EE work " << spin * 1e3 << " ms: off " << off * 1e3 << " ms, on "
                  << on * 1e3 << " ms per chain (" << off / on << "x)\n";
    }
}

struct Scenario {
    std::uint32_t seed;
    Service service;
    bool with_path3;
    std::uint32_t launch_period;
    std::uint64_t expected;
    std::uint32_t variant = 0U;
};

// Digests produced by the original implementation (word-at-a-time VIF1,
// std::function chain walker, quadword-at-a-time polled GIF arbiter). Seeds
// 13-20 were pinned against the pre-batching implementation too.
constexpr Scenario kScenarios[] = {
    {1U, Service::RunAhead, false, 1U, 0x179252d22af28a64ULL},
    {2U, Service::RunAhead, true, 1U, 0x22c321986ac81195ULL},
    {3U, Service::Chunked, false, 1U, 0x9d67930c975f591cULL},
    {4U, Service::Chunked, true, 1U, 0x3dda6aae0542dc19ULL},
    {5U, Service::RunAhead, false, 8U, 0xf767382b9dbaf6c5ULL},
    {6U, Service::RunAhead, true, 8U, 0xa50fb8e640d604ddULL},
    {7U, Service::Chunked, false, 8U, 0x58a19ec353da85b2ULL},
    {8U, Service::Chunked, true, 8U, 0x3a3482feb0a10165ULL},
    {9U, Service::RunAhead, false, 100000U, 0xa89b58da7bb00eafULL},
    {10U, Service::RunAhead, true, 100000U, 0x97d98e1027475f20ULL},
    {11U, Service::Chunked, false, 100000U, 0xf687a822279a0843ULL},
    {12U, Service::Chunked, true, 100000U, 0x5d4645d68bd5a72eULL},
    // Device configurations the plain scenarios leave out: IMT slicing,
    // MSKPATH3 windows, GIF pause, DIRECTHL and the raw passthrough observer.
    {13U, Service::RunAhead, true, 8U, 0x33a70b5397069cafULL, kVariantImt},
    {14U, Service::Chunked, true, 8U, 0xf720f25b4d2373b5ULL, kVariantImt | kVariantDirectHl},
    {15U, Service::RunAhead, true, 8U, 0x489a4e11a036cd1aULL, kVariantImt | kVariantMask},
    {16U, Service::Chunked, true, 8U, 0x4ddb5c18f361c806ULL, kVariantImt | kVariantMask | kVariantPause},
    {17U, Service::RunAhead, true, 100000U, 0x7ebee451be420d7fULL, kVariantPause | kVariantMask},
    {18U, Service::RunAhead, true, 8U, 0xb0911e476ef24a0aULL, kVariantRawOnly},
    {19U, Service::Chunked, true, 100000U, 0x2fb08e54a92169b1ULL, kVariantImt | kVariantRawOnly},
    {20U, Service::RunAhead, true, 8U, 0x8d2fc3f1106cf500ULL,
     kVariantImt | kVariantMask | kVariantPause | kVariantDirectHl},
};

} // namespace

#ifdef _WIN32
// Poor man's sampling profiler: a second thread suspends the benchmark thread
// every ~100 us and records its RIP (relative to the image base) into
// <path>; resolve against `nm -n -C <exe>` offline.
void profile_scenarios(const char* path, const std::uint32_t scenario_index,
                       const int attempts) {
    std::atomic<bool> stop{false};
    // Pre-sized: the sampler must not allocate while the target may hold the
    // heap lock.
    std::vector<std::uint64_t> samples;
    samples.reserve(1U << 20U);
    std::vector<std::uint64_t> callers;
    callers.reserve(1U << 20U);
    HANDLE target{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0,
                    FALSE, DUPLICATE_SAME_ACCESS);
    const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleA(nullptr));
    std::thread sampler([&] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        while (!stop.load()) {
            if (!g_sampling.load()) {
                continue;
            }
            LARGE_INTEGER start;
            QueryPerformanceCounter(&start);
            LARGE_INTEGER now;
            do {
                QueryPerformanceCounter(&now);
            } while ((now.QuadPart - start.QuadPart) * 10000 < frequency.QuadPart && !stop.load());
            if (SuspendThread(target) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(target, &context) && samples.size() < samples.capacity()) {
                samples.push_back(context.Rip);
                // First frame inside the benchmark image, found by unwinding
                // through whatever system DLL the thread is in.
                std::uint64_t first_image_frame = 0;
                CONTEXT walk = context;
                for (int depth = 0; depth < 16; ++depth) {
                    if (walk.Rip - base < 0x10000000ULL) {
                        first_image_frame = walk.Rip - base;
                        break;
                    }
                    DWORD64 image_base = 0;
                    const auto* entry = RtlLookupFunctionEntry(walk.Rip, &image_base, nullptr);
                    if (entry == nullptr) {
                        break;
                    }
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, walk.Rip,
                                     const_cast<PRUNTIME_FUNCTION>(entry), &walk, &handler_data,
                                     &establisher, nullptr);
                }
                callers.push_back(first_image_frame);
            }
            ResumeThread(target);
        }
    });
    const auto& scenario = kScenarios[scenario_index];
    g_launch_period = scenario.launch_period;
    g_variant = scenario.variant;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        run(scenario.seed, scenario.service, scenario.with_path3, false, 12U);
    }
    stop = true;
    sampler.join();
    std::ofstream out(path);
    std::ofstream caller_out(std::string(path) + ".callers");
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        const auto sample = samples[index];
        if (callers[index] != 0U) {
            caller_out << std::hex << callers[index] << '\n';
        }
        HMODULE module{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(sample), &module)) {
            char name[260]{};
            GetModuleFileNameA(module, name, sizeof(name));
            if (reinterpret_cast<std::uint64_t>(module) == base) {
                out << std::hex << (sample - base) << '\n';
            } else {
                out << "DLL " << name << " " << std::hex
                    << (sample - reinterpret_cast<std::uint64_t>(module)) << '\n';
            }
        } else {
            out << "UNKNOWN " << std::hex << sample << '\n';
        }
    }
    std::cout << samples.size() << " samples\n";
}
#endif

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
#ifdef _WIN32
    if (mode == "--profile" && argc > 3) {
        profile_scenarios(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])),
                          argc > 4 ? std::stoi(argv[4]) : 6);
        return 0;
    }
#endif
    if (mode == "--benchmark-async") {
        benchmark_async();
        return 0;
    }
    if (mode == "--benchmark") {
        for (const auto& scenario : kScenarios) {
            if (scenario.launch_period == 1U || scenario.variant != 0U) {
                continue;  // digest-only: dominated by VU1 execution / device variants
            }
            g_launch_period = scenario.launch_period;
            g_variant = scenario.variant;
            // Warm up once, then take the best of three.
            run(scenario.seed, scenario.service, scenario.with_path3, false, 1U);
            double best = 1e30;
            Result last;
            for (int attempt = 0; attempt < 9; ++attempt) {
                last = run(scenario.seed, scenario.service, scenario.with_path3, false, 25U);
                best = std::min(best, last.seconds);
            }
            std::cout << (scenario.service == Service::RunAhead ? "run-ahead " : "chunk-24  ")
                      << (scenario.with_path3 ? "vif1+path3 " : "vif1       ") << "MSCAL/"
                      << scenario.launch_period << ": "
                      << best * 1e9 / static_cast<double>(last.source_quadwords)
                      << " ns/source-QW (" << last.source_quadwords << " source QWs, "
                      << last.gif_quadwords << " GIF QWs, " << last.cycles << " cycles, "
                      << best * 1e3 << " ms)\n";
        }
        return 0;
    }
    int failures{};
    for (const auto& scenario : kScenarios) {
        g_launch_period = scenario.launch_period;
        g_variant = scenario.variant;
        const auto result =
            run(scenario.seed, scenario.service, scenario.with_path3, true, 2U);
        std::cout << "seed " << scenario.seed << ": digest 0x" << std::hex << result.digest
                  << std::dec << " (" << result.cycles << " cycles, " << result.gif_quadwords
                  << " GIF QWs, " << result.summary << ")\n";
        if (mode != "--print" && result.digest != scenario.expected) {
            std::cerr << "FAIL: seed " << scenario.seed << " digest changed\n";
            ++failures;
        }
    }
    failures += check_worker();
    failures += check_window_semantics();
    for (const auto& scenario : kAsyncScenarios) {
        const auto off = run_async_script(scenario.seed, scenario.with_path3, scenario.period,
                                          scenario.variant, scenario.pattern,
                                          GraphicsAsyncMode::Off, scenario.latency, scenario.run_limit, 2U);
        const auto on = run_async_script(scenario.seed, scenario.with_path3, scenario.period,
                                         scenario.variant, scenario.pattern,
                                         GraphicsAsyncMode::On, scenario.latency, scenario.run_limit, 2U);
        const auto measured =
            run_async_script(scenario.seed, scenario.with_path3, scenario.period,
                             scenario.variant, scenario.pattern, GraphicsAsyncMode::Measure,
                             scenario.latency, scenario.run_limit, 2U);
        std::size_t closes{};
        for (const auto count : on.stats.closed_by) {
            closes += count;
        }
        std::cout << "async seed " << scenario.seed << ": off 0x" << std::hex << off.digest
                  << " on 0x" << on.digest << " measure 0x" << measured.digest << std::dec << " ("
                  << on.gif_quadwords << " GIF QWs, " << on.stats.windows << " windows, "
                  << on.stats.declined_total << " declined, " << on.stats.ordered_tasks
                  << " ordered)\n";
        if (on.digest != off.digest || measured.digest != off.digest ||
            on.gif_quadwords != off.gif_quadwords) {
            std::cerr << "FAIL: async seed " << scenario.seed
                      << " differs from the synchronous run\n";
            ++failures;
        }
        if (on.stats.windows == 0U || closes != on.stats.windows ||
            on.stats.worker_exceptions != 0U) {
            std::cerr << "FAIL: async seed " << scenario.seed << " windows " << on.stats.windows
                      << " closes " << closes << " exceptions " << on.stats.worker_exceptions
                      << '\n';
            ++failures;
        }
        if (measured.stats.windows != on.stats.windows) {
            std::cerr << "FAIL: async seed " << scenario.seed
                      << " measure mode saw a different window count\n";
            ++failures;
        }
    }
    if (failures != 0) {
        return 1;
    }
    std::cout << "VIF1 stream replay tests passed\n";
    return 0;
}
