#pragma once

// Hardware-faithful SPU2 voice core: the per-voice register file, envelope,
// pitch/resample machinery, key-on/key-off latches, and per-core mixdown,
// modelled on the documented PS2 SPU2 behaviour (nocash/ps2tek ADSR and
// ADPCM block documentation; Gaussian resample filter credited to nocash and
// Ryphecha) as implemented by the reference emulator's SPU2 (PCSX2
// SPU2/{ADSR,Mixer,defs}.cpp at the time of writing). This replaces the
// removed behavioral SfxAdsrEnvelope approximation. Executed retail libsd
// state reaches this core through Spu2GuestLink, and only its final mixed bus
// is transported to the host device.
//
// Scope of this increment (docs/AUDIO.md §8 program, increment 1):
//   - 2 cores x 24 voices over a shared 2 MiB SPU2 RAM word array;
//   - per-voice SSA/LSA/NAX, 16-bit pitch, ADSR1/ADSR2, VOL L/R with the
//     hardware sweep mode, VMIX dry/wet gates, PMON pitch modulation,
//     NON noise allocation and the noise generator;
//   - KON/KOFF as write-one 32-bit core latches processed at the start of
//     every output sample (key-off before key-on), ENDX semantics;
//   - PS-ADPCM block decode with XA loop flags (LOOP_END/LOOP/LOOP_START),
//     LSA auto-capture unless written manually (loop_mode), voice stop on
//     LOOP_END without LOOP, restart-on-KON;
//   - core chaining per the documented topology: core 0's mix is core 1's
//     external input (scaled by core 0 master volume, then core 1 external
//     volume); final output is core 1's mix scaled by its master volume;
//   - deterministic saturated mixdown at every bus.
//
// Deferred, with seams left in place (docs/AUDIO.md §8 increments 2+):
// reverb processing (the pre-reverb wet bus is accumulated and exposed),
// input/ADMA streams and CDDA, capture-buffer contents, general SPU2 IRQ
// sources beyond the fixed per-sample capture/output writes, SPDIF.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

namespace takedown::audio {

// Number of voices per SPU2 core (hardware constant).
inline constexpr int kSpu2VoicesPerCore = 24;
// SPU2 RAM is 2 MiB, addressed as 16-bit words; voice addresses use the low
// 20 bits (word address) with block granularity of 8 words (16 bytes).
inline constexpr std::uint32_t kSpu2RamWords = 0x100000;
inline constexpr std::uint32_t kSpu2AddressMask = kSpu2RamWords - 1;
inline constexpr std::uint32_t kSpu2BlockMask = 0xFFFF8;

// XA loop flags (high byte of the 16-byte ADPCM block header word):
//   LOOP_END: end of loop body — sets the voice's ENDX bit, rewinds NAX to
//             LSA, and (without LOOP) stops the voice.
//   LOOP:     with LOOP_END, keeps the voice looping instead of stopping.
//   LOOP_START: this block is the loop start; LSA captures NAX here unless
//             LSA was written manually since key-on (loop_mode).
inline constexpr std::uint8_t kSpu2XaFlagLoopEnd = 1u << 0;
inline constexpr std::uint8_t kSpu2XaFlagLoop = 1u << 1;
inline constexpr std::uint8_t kSpu2XaFlagLoopStart = 1u << 2;

// The SPU2 resamples each voice through a 256-phase, 4-tap Gaussian
// interpolation filter.  The table is generated at startup from the
// documented closed-form construction (nocash/Ryphecha; the same formula the
// reference emulator compiles to constants), normalised so each phase's four
// taps sum to 0x7f80.  Tests assert the generated taps match the reference
// emulator's committed table rows bit-exactly.
struct Spu2GaussianInterpTable {
    std::array<std::array<std::int16_t, 4>, 256> phase{};

    Spu2GaussianInterpTable();
};

// Process-wide singleton; the table construction is pure and deterministic.
const Spu2GaussianInterpTable& spu2_gaussian_table() noexcept;

// ---------------------------------------------------------------------------
// Spu2Adsr: the hardware envelope.
//
// Register layout (32-bit value ADSR1 | ADSR2 << 16, bit 0 = ADSR1.0):
//   ADSR1: SL[3:0] DR[7:4] ARStep[9:8] ARShift[14:10] AM[15]
//   ADSR2: RRShift[4:0] RRMode[5] SRStep[7:6] SRShift[12:8] (bit13 unused)
//          SDir[14] SMode[15]
//
// Phases are numbered 0..4 with 0 = stopped so that phase++ follows the
// hardware's fixed phase order.
class Spu2Adsr {
public:
    static constexpr std::int32_t kMaxValue = 0x7fff;

    enum Phase : std::uint8_t {
        kPhaseStopped = 0,
        kPhaseAttack = 1,
        kPhaseDecay = 2,
        kPhaseSustain = 3,
        kPhaseRelease = 4,
    };

    // One envelope phase's cached step parameters (rebuilt on register
    // writes).  decr/exp are the decrease/exponential mode flags; step is
    // the signed base step; target is the level at which the phase advances.
    struct PhaseParams {
        bool decr;
        bool exp;
        std::uint32_t shift;
        std::int32_t step;
        std::int32_t target;
    };

    std::uint32_t reg32{0};
    std::int32_t value{0};
    std::uint32_t counter{0};
    std::uint8_t phase{kPhaseStopped};

    // Rebuilds the cached phase parameters from reg32.  Must be called after
    // any direct register write (set_regs does it for you).
    void update_cache() noexcept;

    void set_regs(std::uint32_t regs) noexcept {
        reg32 = regs;
        update_cache();
    }

    static std::uint32_t make_regs(std::uint16_t adsr1, std::uint16_t adsr2) noexcept {
        return static_cast<std::uint32_t>(adsr1) |
               (static_cast<std::uint32_t>(adsr2) << 16);
    }

    // Key-on: begin attack from zero.
    void attack() noexcept;

    // Key-off: jump to release (no-op when already stopped).
    void release() noexcept;

    // Advances one output sample.  Returns false when the envelope finished
    // (release reached zero, or sustain decayed to silence); the caller must
    // stop the voice.  Sustain holds indefinitely while its level is
    // non-zero (until key-off).
    bool calculate() noexcept;

    void stop() noexcept {
        value = 0;
        phase = kPhaseStopped;
    }

    [[nodiscard]] bool stopped() const noexcept { return phase == kPhaseStopped; }

    [[nodiscard]] const PhaseParams& params() const noexcept { return cached_[phase]; }

private:
    PhaseParams cached_[5]{};
};

// ---------------------------------------------------------------------------
// Spu2VolumeSlide: a 16-bit volume register with the hardware sweep mode.
//
// Register layout (VOL): Step[1:0] Shift[6:2] (bits 11:7 unused) Phase[12]
// Decr[13] Exp[14] Enable[15].  When Enable is clear the register value is
// the volume itself (sign-extended (value << 1)); when set the register
// configures a sweep that slides the cached value with the same
// counter/step machinery as the envelope.
class Spu2VolumeSlide {
public:
    std::uint16_t reg_vol{0};
    std::uint32_t counter{0};
    std::int32_t value{0};

    void set_reg(std::uint16_t src) noexcept;

    // Master-volume "extend" write (MVOLXL/R): replaces the volume value
    // directly without touching the sweep configuration register.
    void set_value_raw(std::uint32_t raw) noexcept {
        value = static_cast<std::int16_t>(raw & 0xffffU);
    }

    // Advances the sweep one output sample (no-op unless enabled).
    void update() noexcept;

    [[nodiscard]] bool sweep_enabled() const noexcept {
        return ((reg_vol >> 15) & 1) != 0;
    }

private:
    [[nodiscard]] std::uint32_t step() const noexcept { return reg_vol & 0x3; }
    [[nodiscard]] std::uint32_t shift() const noexcept {
        return (reg_vol >> 2) & 0x1f;
    }
    [[nodiscard]] bool phase() const noexcept { return ((reg_vol >> 12) & 1) != 0; }
    [[nodiscard]] bool decr() const noexcept { return ((reg_vol >> 13) & 1) != 0; }
    [[nodiscard]] bool exp() const noexcept { return ((reg_vol >> 14) & 1) != 0; }
};

// ---------------------------------------------------------------------------
// Spu2Voice: one voice's hardware state and per-output-sample behaviour.
//
// Register file (wired to libsd addresses in a later increment):
//   volume L/R (Spu2VolumeSlide), pitch (4.12 fixed point, 0x1000 = 1.0),
//   ADSR1/ADSR2 (adsr.reg32), SSA (start address), LSA (loop start), and
//   NAX (current decode address, writable on hardware).
class Spu2Voice {
public:
    Spu2VolumeSlide volume_left;
    Spu2VolumeSlide volume_right;
    std::uint16_t pitch{0};
    Spu2Adsr adsr;

    std::uint32_t start_a{0};
    std::uint32_t loop_start_a{0};
    std::uint32_t next_a{0};

    // Decode pipeline: 32-entry ring of decoded samples fed 4 samples per
    // consumed ADPCM block quarter; sp is the 4.12 pitch accumulator whose
    // integer part consumes ring entries.  The positions accumulate without
    // wrapping (only their low 5 bits index the ring).
    std::array<std::int16_t, 32> decode_fifo{};
    std::uint32_t dec_pos_read{0};
    std::uint32_t dec_pos_write{0};
    std::uint32_t sp{0};
    std::int32_t prev1{0};
    std::int32_t prev2{0};
    std::uint8_t loop_flags{0};
    std::uint8_t loop_mode{0};
    std::int32_t out_x{0};
    bool modulated{false};
    bool noise{false};

    // Per-voice VMIX gate enables (register bit set = voice mixed into that
    // bus).  Reset state: all open.
    bool gate_dry_l{true};
    bool gate_dry_r{true};
    bool gate_wet_l{true};
    bool gate_wet_r{true};

    // Owning-core wiring: decode_samples reports LOOP_END passages through
    // this per-sample flag (the owning core turns it into the voice's ENDX
    // bit); mix_step clears it at entry.  Kept out of the constructor on
    // purpose so Spu2Core stays safely copyable.
    bool hit_loop_end{false};

    // SSA is a 20-bit, 8-word-aligned address; misaligned values round up to
    // the next block boundary (invalid input; recorded by the caller via
    // set_start_a's return).
    void set_start_a(std::uint32_t addr) noexcept;
    // LSA write; a manual LSA write sets loop_mode, which stops the
    // LOOP_START flag from auto-capturing LSA afterwards.
    void set_loop_start_a(std::uint32_t addr) noexcept;
    // NAX write (the decoder continues from the new location).
    void set_next_a(std::uint32_t addr) noexcept;

    // Key-on: resets the decode pipeline and starts the envelope.
    void start() noexcept;
    // Voice stop: envelope zeroed and stopped.
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept { return !adsr.stopped(); }

    // One output sample of voice processing against `ram`.  Returns the
    // post-envelope mono sample (before channel volume; the SPU2 voice
    // output is mono, fanned to both channels after volume).
    // `noise_out` supplies the core noise-generator sample (used when
    // `noise` is set); `prev_out_x` is the previous voice's OutX for pitch
    // modulation; `voice_index` gates modulation (voice 0 is never
    // modulated on hardware).
    std::int32_t mix_step(const std::uint16_t* ram, std::int32_t noise_out,
                          std::int32_t prev_out_x, int voice_index) noexcept;

    // Per-sample bus accumulators a voice block adds its volume-scaled output
    // to (pre-saturation 32-bit sums; the core saturates after all voices).
    struct BlockBus {
        std::int32_t* dry_left;
        std::int32_t* dry_right;
        std::int32_t* wet_left;
        std::int32_t* wet_right;
    };

    // `count` consecutive output samples of mix_step for this voice, with the
    // voice's volume-scaled output (after the VMIX gates) added into `bus`.
    // `noise_out[i]` / `prev_out_x[i]` are the per-sample arguments of
    // mix_step; `out_x_out`, when non-null, receives the voice's OutX after
    // each sample (the next voice's `prev_out_x`). Bit-identical to calling
    // mix_step once per sample; only voices whose state provably cannot
    // change for the rest of the block (stopped, silent, pitch 0, decoder
    // buffered ahead, no volume sweep) skip the remaining samples. Returns
    // true when any sample passed a LOOP_END block (the ENDX bit).
    bool mix_block(const std::uint16_t* ram, std::size_t count, const std::int32_t* noise_out,
                   const std::int32_t* prev_out_x, int voice_index, std::int32_t* out_x_out,
                   const BlockBus& bus) noexcept;

private:
    // True when every further mix_step would leave this voice unchanged and
    // add nothing to the buses (see mix_block).
    [[nodiscard]] bool quiescent() const noexcept {
        return adsr.stopped() && pitch == 0U && !volume_left.sweep_enabled() &&
               !volume_right.sweep_enabled() &&
               static_cast<int>(dec_pos_write - dec_pos_read) > 12;
    }

    // True while `sbuffer_` holds the decoded contents of the block at
    // `next_a`'s block (cleared on every block boundary).
    bool sbuffer_valid_{false};
    std::array<std::int16_t, 28> sbuffer_{};

    void update_block_header(const std::uint16_t* ram) noexcept;
    void get_next_data_buffered(const std::uint16_t* ram) noexcept;
    void decode_samples(const std::uint16_t* ram) noexcept;
    void increment_next_a() noexcept;
    [[nodiscard]] std::int32_t get_voice_values() const noexcept;
};

// Decode one 16-byte PS-ADPCM block (8 SPU2 RAM words) into 28 samples,
// carrying the predictor history across blocks (documented XA/SPU2 block
// format: shift and selector in the low header byte, loop flags in the high
// byte, low nibble before high nibble within each data byte).
void spu2_decode_adpcm_block(std::int16_t* out_28, const std::uint16_t* block,
                             std::int32_t& prev1, std::int32_t& prev2) noexcept;

// ---------------------------------------------------------------------------
// Spu2Core: one SPU2 core (24 voices, key latches, mix buses, master
// volume, noise generator).
class Spu2Core {
public:
    explicit Spu2Core(int core_index) noexcept;

    int index{0};
    std::array<Spu2Voice, kSpu2VoicesPerCore> voices{};

    // Write-one key latches (the KON/KOFF registers map directly onto
    // these); processed at the start of each output sample, key-off first.
    std::uint32_t key_on{0};
    std::uint32_t key_off{0};

    // ENDX: bit per voice; cleared on key-on, set when a voice's decode
    // passes a LOOP_END block.  Reset value 0xffffff (hardware confirmed).
    std::uint32_t endx{0xffffff};

    // ATTR register fields used by the core (write_attr decodes them).
    std::uint32_t noise_clk{0};
    bool fx_enable{false};
    // Named libsd core-attribute state. IRQ delivery and global mute are not
    // yet consumed by the PCM renderer, but retaining the exact writes keeps
    // native RWA initialization observable instead of silently dropping them.
    bool irq_enable{false};
    bool mute_enable{false};

    // Master volume (REG_P_MVOLL/R, sweep-capable) and external-input
    // volume (the scale applied to the other core's mix entering this core).
    Spu2VolumeSlide master_volume_left;
    Spu2VolumeSlide master_volume_right;
    std::int16_t ext_volume_left{0};
    std::int16_t ext_volume_right{0};
    // Effect (reverb) send volumes; consumed by the reverb increment.
    std::int16_t fx_volume_left{0};
    std::int16_t fx_volume_right{0};

    // Core-level bus gates (default open): the whole voice bus and the
    // external input are gated separately on the dry and wet paths.
    bool bus_voice_dry_l{true};
    bool bus_voice_dry_r{true};
    bool bus_voice_wet_l{true};
    bool bus_voice_wet_r{true};
    bool bus_ext_dry_l{true};
    bool bus_ext_dry_r{true};
    bool bus_ext_wet_l{true};
    bool bus_ext_wet_r{true};

    std::uint32_t noise_cnt{0};
    std::uint16_t noise_out{0};

    // Core-level addresses written through libsd set_addr (consumed by the
    // reverb/DMA/IRQ increments; stored so retail writes are never lost).
    std::uint32_t effects_start_a{0}; // SD_ADDR_ESA
    std::uint32_t effects_end_a{0};   // SD_ADDR_EEA
    std::uint32_t transfer_start_a{0}; // SD_ADDR_TSA (DMA transfer cursor)
    std::uint32_t irqa{0};             // SD_ADDR_IRQA

    // PMON (pitch modulation) allocation bits; voice 0 is never modulated.
    void write_pmon(std::uint16_t low, std::uint16_t high) noexcept;
    // NON (noise) allocation bits.
    void write_non(std::uint16_t low, std::uint16_t high) noexcept;
    // VMIX dry/wet gate bits (bit set = voice mixed into that bus).
    void write_vmix(std::uint32_t vmixl, std::uint32_t vmixr,
                    std::uint32_t vmixel, std::uint32_t vmixer) noexcept;

    void write_master_volume(std::uint16_t left, std::uint16_t right) noexcept;
    void write_attr(std::uint16_t value) noexcept;

    // Applies pending key latches (key-off first, then key-on), clearing
    // both.  Called once per output sample before mixing; split out so the
    // system can run both cores' latches before either core mixes.
    void process_key_latches() noexcept;

    // Queue an LSA write that must take effect immediately after the next
    // KON for `voice` is consumed.  RWA writes LSAX after KON; deferring the
    // write through the same sample-boundary latch prevents Spu2Voice::start
    // from clearing the manual-loop state before playback begins.
    void queue_post_key_on_loop_start(int voice, std::uint32_t addr) noexcept;

    struct MixResult {
        // The core's full mix (dry voice bus + external input; the reverb
        // increment adds the processed wet bus here).
        std::int32_t mix_left;
        std::int32_t mix_right;
        // The pre-reverb wet bus, accumulated and exposed for the reverb
        // increment (currently unprocessed, never mixed into mix_*).
        std::int32_t wet_left;
        std::int32_t wet_right;
    };

    // One output sample of voice mixing against `ram`.  `ext_left/right` is
    // the external input (the other core's mix, already volume-scaled).
    // All voices are processed every sample — the hardware keeps the
    // decode/pitch bookkeeping of stopped voices advancing — and every bus
    // saturates to 16 bits exactly where the hardware clamps.
    MixResult mix_voices(const std::uint16_t* ram, std::int32_t ext_left,
                         std::int32_t ext_right) noexcept;

    // Upper bound on the block length mix_block accepts.
    static constexpr std::size_t kMaxMixBlock = 256;

    // `count` (<= kMaxMixBlock) consecutive samples of mix_voices, voice-major
    // so each voice's state stays hot across the block. `ext_left/right` are
    // the per-sample external inputs (null = zeros). `out[i]` receives the
    // sample's MixResult and `master_left/right[i]` (optional) the core's
    // master volume after that sample's sweep update, which the system reads
    // when scaling the core output. Equivalent to `count` mix_voices calls
    // with no key latch processing in between (latches are written only by
    // guest register writes, which land between blocks).
    void mix_block(const std::uint16_t* ram, std::size_t count, const std::int32_t* ext_left,
                   const std::int32_t* ext_right, MixResult* out, std::int32_t* master_left,
                   std::int32_t* master_right) noexcept;

    // Convenience for standalone cores/tests: latches then mix.
    MixResult step(const std::uint16_t* ram, std::int32_t ext_left,
                   std::int32_t ext_right) noexcept {
        process_key_latches();
        return mix_voices(ram, ext_left, ext_right);
    }

    // Advances the core noise generator one output sample (Dr. Hell's
    // algorithm as documented in the reference implementation).
    void update_noise() noexcept;
    [[nodiscard]] std::int16_t noise_value() const noexcept {
        return static_cast<std::int16_t>(noise_out);
    }

private:
    std::array<std::uint32_t, kSpu2VoicesPerCore> post_key_on_loop_start_a_{};
    std::uint32_t post_key_on_loop_start_mask_{0};
};

// ---------------------------------------------------------------------------
// Spu2System: both cores over one shared 2 MiB RAM.
class Spu2System {
public:
    Spu2System() noexcept;

    std::array<std::uint16_t, kSpu2RamWords> ram{};
    std::array<Spu2Core, 2> cores;
    // Global digital-output mode written by SD_C_SPDIF_MODE. The SDL PCM sink
    // does not emulate an SPDIF transport, but the guest register is retained.
    std::uint16_t spdif_mode{0};

    std::uint16_t* ram_data() noexcept { return ram.data(); }
    [[nodiscard]] const std::uint16_t* ram_data() const noexcept { return ram.data(); }

    // DMA / transfer entry: the sceSdVoiceTrans-style byte upload the
    // executed retail IOP code makes into SPU2 RAM.  `byte_address` is a
    // byte address in the 2 MiB space (rounded down to a halfword); bytes
    // are little-endian halfwords; transfers wrap at the RAM end exactly
    // where the address wraps.  Returns the number of bytes written
    // (full `bytes.size()` unless a trailing unaligned byte is dropped).
    std::size_t dma_upload_bytes(std::uint32_t byte_address,
                                 std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::uint16_t ram_read_word(std::uint32_t word_address) const noexcept {
        return ram[word_address & kSpu2AddressMask];
    }
    void ram_write_word(std::uint32_t word_address, std::uint16_t value) noexcept {
        ram[word_address & kSpu2AddressMask] = value;
    }

    // Advances one 48 kHz output sample through both cores and assembles the
    // stereo output: core 0's saturated mix, scaled by core 0's master
    // volume and then core 1's external volume, feeds core 1 as its external
    // input; core 1's saturated mix scaled by its master volume is the
    // output.  Returns 16-bit stereo.
    struct Stereo {
        std::int16_t left;
        std::int16_t right;
    };
    Stereo step() noexcept;

    // Upper bound on one step_block call.
    static constexpr std::size_t kMaxBlockSamples = Spu2Core::kMaxMixBlock;

    struct BlockResult {
        // Samples rendered into the front of the caller's buffer.
        std::size_t samples{};
        // Cores whose fixed-capture IRQ observer is due after the LAST rendered
        // sample (bit n = core n). The block always ends on such a sample so
        // the observer's volume writes land exactly between two samples; the
        // caller delivers them with notify_capture_irq(), core 0 first.
        std::uint8_t irq_core_mask{};
    };
    // Renders up to out.size() (<= kMaxBlockSamples) samples: the same stream
    // as that many step() calls, with the observer notifications deferred to
    // the caller (see BlockResult).
    BlockResult step_block(std::span<Stereo> out) noexcept;
    void notify_capture_irq(const std::uint8_t core) const {
        if (fixed_capture_irq_observer_) fixed_capture_irq_observer_(core);
    }

    // Narrow IRQ seam for the fixed capture/output RAM writes performed every
    // sample. This is not the complete SPU2 IRQ controller: decode, DMA, input,
    // status/latch, and acknowledgement semantics remain separate work. RWA
    // uses core 0 IRQA 0x0800 (byte address), reached by the voice-1 capture
    // ring every 512 samples, to wake its volume worker.
    void set_fixed_capture_irq_observer(std::function<void(std::uint8_t)> observer) {
        fixed_capture_irq_observer_ = std::move(observer);
    }

    // Key-on/key-off through the latches (how KON/KOFF register writes land).
    void key_on(int core, std::uint32_t voices) noexcept {
        cores[core].key_on |= voices;
    }
    // Latch one KON together with an LSA write ordered after voice start.
    void key_on_with_post_loop_start(int core, int voice,
                                     std::uint32_t loop_start_a) noexcept {
        cores[core].key_on |= 1U << voice;
        cores[core].queue_post_key_on_loop_start(voice, loop_start_a);
    }
    void key_off(int core, std::uint32_t voices) noexcept {
        cores[core].key_off |= voices;
    }

private:
    // True when the fixed capture write at sample position `position` (mod
    // 512) asserts core `core`'s IRQA.
    [[nodiscard]] bool capture_irq_due(std::size_t core, std::uint32_t position) const noexcept;

    std::function<void(std::uint8_t)> fixed_capture_irq_observer_{};
    std::uint32_t capture_position_{};
};

// Saturating mix helper shared with tests.
[[nodiscard]] constexpr std::int32_t spu2_clamp16(std::int32_t v) noexcept {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

} // namespace takedown::audio
