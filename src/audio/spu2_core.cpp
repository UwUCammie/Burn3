#include "takedown/audio/spu2_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace takedown::audio {

namespace {

// XA ADPCM predictor coefficients (1/64 fixed point); filters 5..15 are
// unused by real data and decode as filter 0.
constexpr std::int32_t kXaFactor[16][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0},  {0, 0},     {0, 0},    {0, 0},     {0, 0}, {0, 0}, {0, 0},
};

// Round half away from zero (the rounding the reference table generator uses).
double cx_round(const double x) noexcept {
    return x >= 0.0 ? std::floor(x + 0.5) : std::ceil(x - 0.5);
}

constexpr double kPi = 3.14159265358979323846;

} // namespace

Spu2GaussianInterpTable::Spu2GaussianInterpTable() {
    // 512-entry windowed sinc, stored mirrored so phase rows read forwards.
    std::array<double, 512> table{};
    for (std::uint32_t n = 0; n < 512; ++n) {
        const double k = 0.5 + n;
        const double s = std::sin(k * kPi * 2.048 / 1024.0);
        const double t = (std::cos(k * kPi * 2.0 / 1023.0) - 1.0) * 0.5;
        const double u = (std::cos(k * kPi * 4.0 / 1023.0) - 1.0) * 0.08;
        table[511 - n] = (t + u + 1.0) * s / k;
    }

    double sum = 0.0;
    for (const double n : table) {
        sum += n;
    }
    const double scale = 0x7f80 * 128 / sum;
    for (double& value : table) {
        value *= scale;
    }

    // Spread each phase's rounding error evenly over its four taps so every
    // row sums to exactly 0x7f80.
    for (std::uint32_t ph = 0; ph < 256; ++ph) {
        const double phase_sum = table[ph] + table[ph + 256] + table[511 - ph] + table[255 - ph];
        const double diff = (phase_sum - 0x7f80) / 4.0;
        phase[255 - ph][0] = static_cast<std::int16_t>(cx_round(table[ph] - diff));
        phase[255 - ph][1] = static_cast<std::int16_t>(cx_round(table[ph + 256] - diff));
        phase[255 - ph][2] = static_cast<std::int16_t>(cx_round(table[511 - ph] - diff));
        phase[255 - ph][3] = static_cast<std::int16_t>(cx_round(table[255 - ph] - diff));
    }
}

const Spu2GaussianInterpTable& spu2_gaussian_table() noexcept {
    static const Spu2GaussianInterpTable table;
    return table;
}

// ---------------------------------------------------------------------------
// Spu2Adsr

void Spu2Adsr::update_cache() noexcept {
    const std::uint32_t sl = reg32 & 0xf;
    const std::uint32_t decay_shift = (reg32 >> 4) & 0xf;
    const std::uint32_t attack_step = (reg32 >> 8) & 0x3;
    const std::uint32_t attack_shift = (reg32 >> 10) & 0x1f;
    const std::uint32_t release_shift = (reg32 >> 16) & 0x1f;
    const std::uint32_t sustain_step = (reg32 >> 22) & 0x3;
    const std::uint32_t sustain_shift = (reg32 >> 24) & 0x1f;

    cached_[kPhaseAttack] = PhaseParams{false, ((reg32 >> 15) & 1) != 0, attack_shift,
                                        7 - static_cast<std::int32_t>(attack_step), kMaxValue};
    cached_[kPhaseDecay] = PhaseParams{true, true, decay_shift, -8, (static_cast<std::int32_t>(sl) + 1) << 11};

    PhaseParams& sustain = cached_[kPhaseSustain];
    sustain = PhaseParams{((reg32 >> 30) & 1) != 0, ((reg32 >> 31) & 1) != 0, sustain_shift,
                          7 - static_cast<std::int32_t>(sustain_step), 0};
    if (sustain.decr) {
        sustain.step = ~sustain.step;
    }

    cached_[kPhaseRelease] = PhaseParams{true, ((reg32 >> 21) & 1) != 0, release_shift, -8, 0};
}

void Spu2Adsr::attack() noexcept {
    phase = kPhaseAttack;
    counter = 0;
    value = 0;
    update_cache();
}

void Spu2Adsr::release() noexcept {
    if (phase != kPhaseStopped) {
        phase = kPhaseRelease;
        counter = 0;
        update_cache();
    }
}

bool Spu2Adsr::calculate() noexcept {
    const PhaseParams& p = cached_[phase];
    std::uint32_t counter_inc = 0x8000U >> std::max(0, static_cast<int>(p.shift) - 11);
    std::int32_t level_inc = p.step << std::max(0, 11 - static_cast<int>(p.shift));
    if (p.exp) {
        if (!p.decr && value > 0x6000) {
            counter_inc >>= 2;
        }
        if (p.decr) {
            level_inc = static_cast<std::int16_t>((level_inc * value) >> 15);
        }
    }

    counter += std::max(counter_inc, 1U);
    if (counter >= 0x8000) {
        counter = 0;
        value = std::clamp(value + level_inc, 0, kMaxValue);
    }

    // Sustain has no target: it holds until key-off or decays to silence.
    if (phase == kPhaseSustain) {
        return value != 0;
    }
    if ((!p.decr && value >= p.target) || (p.decr && value <= p.target)) {
        ++phase;
        if (phase > kPhaseRelease) {
            return false;
        }
        update_cache();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Spu2VolumeSlide

void Spu2VolumeSlide::set_reg(const std::uint16_t src) noexcept {
    reg_vol = src;
    if (!sweep_enabled()) {
        value = static_cast<std::int16_t>(src << 1);
    }
}

void Spu2VolumeSlide::update() noexcept {
    if (!sweep_enabled()) {
        return;
    }

    std::int32_t step_size = 7 - static_cast<std::int32_t>(step());
    if (decr()) {
        step_size = ~step_size;
    }
    std::uint32_t counter_inc = 0x8000U >> std::max(0, static_cast<int>(shift()) - 11);
    std::int32_t level_inc = step_size << std::max(0, 11 - static_cast<int>(shift()));
    if (exp()) {
        if (!decr() && value > 0x6000) {
            counter_inc >>= 2;
        }
        if (decr()) {
            level_inc = static_cast<std::int16_t>((level_inc * value) >> 15);
        }
    }

    // Step 3 / shift 31 is the hardware's "never advance" setting.
    if (step() != 3 || shift() != 0x1f) {
        counter_inc = std::max<std::uint32_t>(1, counter_inc);
    }
    counter += counter_inc;

    // Linear sweeps with the phase bit set run in the inverted direction.
    if (!(exp() && decr()) && phase()) {
        level_inc = -level_inc;
    }

    if (counter >= 0x8000) {
        counter = 0;
        if (!decr()) {
            value = std::clamp(value + level_inc, -0x8000, 0x7fff);
        } else {
            std::int32_t low = phase() ? -0x8000 : 0;
            std::int32_t high = phase() ? 0 : 0x7fff;
            if (exp()) {
                low = 0;
                high = 0x7fff;
            }
            value = std::clamp(value + level_inc, low, high);
        }
    }
}

// ---------------------------------------------------------------------------
// ADPCM

void spu2_decode_adpcm_block(std::int16_t* out_28, const std::uint16_t* const block, std::int32_t& prev1,
                             std::int32_t& prev2) noexcept {
    const std::int32_t header = block[0];
    const std::int32_t shift = (header & 0xf) + 16;
    const int id = (header >> 4) & 0xf;
    const std::int32_t pred1 = kXaFactor[id][0];
    const std::int32_t pred2 = kXaFactor[id][1];

    const auto* const data_bytes = reinterpret_cast<const std::uint8_t*>(block);
    for (int i = 0; i < 14; ++i) {
        std::int32_t data = data_bytes[i + 2] << 28;
        std::int32_t pcm = (data >> shift) + ((prev1 * pred1 + prev2 * pred2 + 32) >> 6);
        pcm = spu2_clamp16(pcm);
        *out_28++ = static_cast<std::int16_t>(pcm);

        data = (data_bytes[i + 2] >> 4) << 28;
        std::int32_t pcm2 = (data >> shift) + ((pcm * pred1 + prev1 * pred2 + 32) >> 6);
        pcm2 = spu2_clamp16(pcm2);
        *out_28++ = static_cast<std::int16_t>(pcm2);

        prev2 = pcm;
        prev1 = pcm2;
    }
}

// ---------------------------------------------------------------------------
// Spu2Voice

void Spu2Voice::set_start_a(std::uint32_t addr) noexcept {
    addr &= kSpu2AddressMask;
    if ((addr & 7) != 0) {
        addr = (addr & kSpu2BlockMask) + 8;
        addr &= kSpu2AddressMask;
    }
    start_a = addr;
}

void Spu2Voice::set_loop_start_a(std::uint32_t addr) noexcept {
    loop_mode = 1;
    loop_start_a = addr & kSpu2BlockMask;
}

void Spu2Voice::set_next_a(std::uint32_t addr) noexcept {
    next_a = (addr & kSpu2BlockMask) | 1;
    sbuffer_valid_ = false;
}

void Spu2Voice::start() noexcept {
    if ((start_a & 7) != 0) {
        set_start_a(start_a);
    }
    adsr.attack();
    loop_mode = 0;
    sp = 0;
    loop_flags = 0;
    // NAX starts one word past SSA: the first word is the block header.
    next_a = start_a | 1;
    prev1 = 0;
    prev2 = 0;
    sbuffer_valid_ = false;
    dec_pos_read = 0;
    dec_pos_write = 0;
}

void Spu2Voice::stop() noexcept {
    adsr.stop();
}

void Spu2Voice::update_block_header(const std::uint16_t* ram) noexcept {
    loop_flags = static_cast<std::uint8_t>(ram[next_a & kSpu2BlockMask] >> 8);
    if ((loop_flags & kSpu2XaFlagLoopStart) != 0 && loop_mode == 0) {
        loop_start_a = next_a & kSpu2BlockMask;
    }
}

void Spu2Voice::increment_next_a() noexcept {
    next_a = (next_a + 1) & kSpu2AddressMask;
}

void Spu2Voice::get_next_data_buffered(const std::uint16_t* ram) noexcept {
    if (!sbuffer_valid_) {
        spu2_decode_adpcm_block(sbuffer_.data(), ram + (next_a & kSpu2BlockMask), prev1, prev2);
        sbuffer_valid_ = true;
    }
    // Words 1..7 of the block each hold four samples.
    const int sample_idx = (static_cast<int>(next_a & 7) - 1) * 4;
    for (int i = 0; i < 4; ++i) {
        decode_fifo[(dec_pos_write + i) & 31] = sbuffer_[i + sample_idx];
    }
}

void Spu2Voice::decode_samples(const std::uint16_t* ram) noexcept {
    // The header is re-read every sample: the guest may move NAX at any time.
    update_block_header(ram);
    if (static_cast<int>(dec_pos_write - dec_pos_read) > 12) {
        return;
    }
    if (adsr.phase != Spu2Adsr::kPhaseStopped) {
        get_next_data_buffered(ram);
    }
    dec_pos_write += 4;

    increment_next_a();
    if ((next_a & 7) == 0) {
        if ((loop_flags & kSpu2XaFlagLoopEnd) != 0) {
            hit_loop_end = true;
            next_a = loop_start_a;
            if ((loop_flags & kSpu2XaFlagLoop) == 0) {
                stop();
            }
        }
        increment_next_a();
        sbuffer_valid_ = false;
    }
}

namespace {

// One phase row of the Gaussian filter (phase = bits 4..11 of the pitch counter).
const std::array<std::int16_t, 4>& gaussian_taps(const std::uint32_t phase) noexcept {
    return spu2_gaussian_table().phase[phase];
}

} // namespace

std::int32_t Spu2Voice::get_voice_values() const noexcept {
    const auto& taps = gaussian_taps((sp >> 4) & 0xff);
    std::int32_t out = 0;
    out += (taps[0] * decode_fifo[(dec_pos_read + 0) & 31]) >> 15;
    out += (taps[1] * decode_fifo[(dec_pos_read + 1) & 31]) >> 15;
    out += (taps[2] * decode_fifo[(dec_pos_read + 2) & 31]) >> 15;
    out += (taps[3] * decode_fifo[(dec_pos_read + 3) & 31]) >> 15;
    return out;
}

std::int32_t Spu2Voice::mix_step(const std::uint16_t* const ram, const std::int32_t noise_out,
                                 const std::int32_t prev_out_x, const int voice_index) noexcept {
    hit_loop_end = false;
    volume_left.update();
    volume_right.update();
    decode_samples(ram);

    std::int32_t value = 0;
    if (active()) {
        value = noise ? noise_out : get_voice_values();
        if (!adsr.calculate()) {
            stop();
        }
        value = (adsr.value * value) >> 15;
        out_x = value;
    }

    std::int32_t p = pitch;
    if (modulated && voice_index != 0) {
        p = std::clamp((p * (prev_out_x + 32768)) >> 15, 0, 0x3fff);
    }
    p = std::min(p, 0x3fff);
    sp += static_cast<std::uint32_t>(p);
    const std::uint32_t consumed = sp >> 12;
    sp &= 0xfff;
    dec_pos_read += consumed;
    return value;
}

bool Spu2Voice::mix_block(const std::uint16_t* const ram, const std::size_t count, const std::int32_t* const noise_out,
                          const std::int32_t* const prev_out_x, const int voice_index, std::int32_t* const out_x_out,
                          const BlockBus& bus) noexcept {
    const bool gate_dry_left = gate_dry_l;
    const bool gate_dry_right = gate_dry_r;
    const bool gate_wet_left = gate_wet_l;
    const bool gate_wet_right = gate_wet_r;

    bool loop_end_any = false;

    // A stopped, unmodulated voice only advances its decode bookkeeping (and
    // any volume sweeps): no output, OutX frozen. The decoder acts only on
    // samples where the ring has drained to 12 or fewer entries, the block
    // header read is idempotent while NAX is unchanged, and the pitch
    // accumulator is linear, so whole runs of decode-free samples collapse to
    // one closed-form step. Every state change lands exactly where
    // per-sample mix_step calls would have put it.
    const auto run_stopped = [&](const std::size_t from) noexcept {
        const bool sweep = volume_left.sweep_enabled() || volume_right.sweep_enabled();
        const std::uint64_t step = std::min<std::uint32_t>(pitch, 0x3fffU);
        std::size_t i = from;
        while (i < count) {
            hit_loop_end = false;
            update_block_header(ram);
            const int fill = static_cast<int>(dec_pos_write - dec_pos_read);
            if (fill > 12) {
                // Samples i.. i+n-1 see more than 12 buffered entries: sample
                // j (0-based from i) has consumed floor((sp + j * step) / 4096)
                // entries before its decode test.
                std::size_t n = count - i;
                if (step != 0) {
                    const std::uint64_t need = static_cast<std::uint64_t>(fill - 12) * 4096U - sp;
                    n = std::min<std::uint64_t>(n, (need + step - 1) / step);
                }
                if (sweep) {
                    for (std::size_t k = 0; k < n; ++k) {
                        volume_left.update();
                        volume_right.update();
                    }
                }
                const std::uint64_t total = sp + n * step;
                dec_pos_read += static_cast<std::uint32_t>(total >> 12);
                sp = static_cast<std::uint32_t>(total & 0xfff);
                if (out_x_out != nullptr) {
                    std::fill(out_x_out + i, out_x_out + i + n, out_x);
                }
                i += n;
                continue;
            }
            if (sweep) {
                volume_left.update();
                volume_right.update();
            }
            decode_samples(ram);
            loop_end_any = loop_end_any || hit_loop_end;
            const std::uint64_t total = sp + step;
            dec_pos_read += static_cast<std::uint32_t>(total >> 12);
            sp = static_cast<std::uint32_t>(total & 0xfff);
            if (out_x_out != nullptr) {
                out_x_out[i] = out_x;
            }
            ++i;
        }
    };

    // The header only needs re-reading after NAX moved (a decode step).
    bool header_stale = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (!modulated && adsr.stopped()) {
            run_stopped(i);
            break;
        }

        hit_loop_end = false;
        if (volume_left.sweep_enabled()) {
            volume_left.update();
        }
        if (volume_right.sweep_enabled()) {
            volume_right.update();
        }
        if (header_stale) {
            update_block_header(ram);
            header_stale = false;
        }
        if (static_cast<int>(dec_pos_write - dec_pos_read) <= 12) {
            decode_samples(ram);
            header_stale = true;
        }

        std::int32_t value = 0;
        if (active()) {
            value = noise ? noise_out[i] : get_voice_values();
            if (!adsr.calculate()) {
                stop();
            }
            value = (adsr.value * value) >> 15;
            out_x = value;
        }

        std::int32_t p = pitch;
        if (modulated && voice_index != 0) {
            p = std::clamp((p * (prev_out_x[i] + 32768)) >> 15, 0, 0x3fff);
        }
        p = std::min(p, 0x3fff);
        sp += static_cast<std::uint32_t>(p);
        dec_pos_read += sp >> 12;
        sp &= 0xfff;

        loop_end_any = loop_end_any || hit_loop_end;
        if (out_x_out != nullptr) {
            out_x_out[i] = out_x;
        }
        if (value != 0) {
            const int scaled_left = static_cast<int>((static_cast<std::int64_t>(value) * volume_left.value) >> 15);
            const int scaled_right = static_cast<int>((static_cast<std::int64_t>(value) * volume_right.value) >> 15);
            if (gate_dry_left) {
                bus.dry_left[i] += scaled_left;
            }
            if (gate_dry_right) {
                bus.dry_right[i] += scaled_right;
            }
            if (gate_wet_left) {
                bus.wet_left[i] += scaled_left;
            }
            if (gate_wet_right) {
                bus.wet_right[i] += scaled_right;
            }
        }
    }
    return loop_end_any;
}

// ---------------------------------------------------------------------------
// Spu2Core

Spu2Core::Spu2Core(const int core_index) noexcept : index(core_index) {}

void Spu2Core::write_pmon(const std::uint16_t low, const std::uint16_t high) noexcept {
    for (int v = 0; v < 16; ++v) {
        voices[v].modulated = ((low >> v) & 1) != 0;
    }
    for (int v = 0; v < 8; ++v) {
        voices[v + 16].modulated = ((high >> v) & 1) != 0;
    }
}

void Spu2Core::write_non(const std::uint16_t low, const std::uint16_t high) noexcept {
    for (int v = 0; v < 16; ++v) {
        voices[v].noise = ((low >> v) & 1) != 0;
    }
    for (int v = 0; v < 8; ++v) {
        voices[v + 16].noise = ((high >> v) & 1) != 0;
    }
}

void Spu2Core::write_vmix(const std::uint32_t vmixl, const std::uint32_t vmixr, const std::uint32_t vmixel,
                          const std::uint32_t vmixer) noexcept {
    for (int v = 0; v < 16; ++v) {
        voices[v].gate_dry_l = ((vmixl >> v) & 1) != 0;
        voices[v].gate_dry_r = ((vmixr >> v) & 1) != 0;
        voices[v].gate_wet_l = ((vmixel >> v) & 1) != 0;
        voices[v].gate_wet_r = ((vmixer >> v) & 1) != 0;
    }
    for (int v = 0; v < 8; ++v) {
        voices[v + 16].gate_dry_l = ((vmixl >> (v + 16)) & 1) != 0;
        voices[v + 16].gate_dry_r = ((vmixr >> (v + 16)) & 1) != 0;
        voices[v + 16].gate_wet_l = ((vmixel >> (v + 16)) & 1) != 0;
        voices[v + 16].gate_wet_r = ((vmixer >> (v + 16)) & 1) != 0;
    }
}

void Spu2Core::write_master_volume(const std::uint16_t left, const std::uint16_t right) noexcept {
    master_volume_left.set_reg(left);
    master_volume_right.set_reg(right);
}

void Spu2Core::write_attr(const std::uint16_t value) noexcept {
    noise_clk = (value >> 8) & 0x3f;
    fx_enable = ((value >> 7) & 1) != 0;
}

void Spu2Core::queue_post_key_on_loop_start(const int voice, const std::uint32_t addr) noexcept {
    if (voice < 0 || voice >= kSpu2VoicesPerCore) {
        return;
    }
    post_key_on_loop_start_a_[voice] = addr & kSpu2BlockMask;
    post_key_on_loop_start_mask_ |= 1U << voice;
}

// Output samples rendered so far (key-latch trace timestamps only).
static std::uint64_t g_trace_samples = 0;

void Spu2Core::process_key_latches() noexcept {
    static const bool trace = std::getenv("TAKEDOWN_SPU2_KON_TRACE") != nullptr;
    const std::uint64_t now = g_trace_samples;
    if (trace && key_off != 0) {
        std::fprintf(stderr, "spu2 koff: core=%d t=%llu mask=%06x\n", index, now, key_off);
    }
    if (trace && key_on != 0) {
        for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
            if (((key_on >> v) & 1) == 0) {
                continue;
            }
            std::fprintf(stderr, "spu2 kon: core=%d t=%llu voice=%d ssa=%05x pitch=%04x vol=%d,%d adsr=%08x\n", index,
                         now, v, voices[v].start_a, voices[v].pitch, voices[v].volume_left.value,
                         voices[v].volume_right.value, voices[v].adsr.reg32);
        }
    }

    // Key-off is processed before key-on.
    if (key_off != 0) {
        for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
            if (((key_off >> v) & 1) != 0) {
                voices[v].adsr.release();
            }
        }
        key_off = 0;
    }
    if (key_on != 0) {
        const std::uint32_t on = key_on;
        endx &= ~on;
        for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
            if (((on >> v) & 1) == 0) {
                continue;
            }
            voices[v].start();
            if (((post_key_on_loop_start_mask_ >> v) & 1) != 0) {
                voices[v].set_loop_start_a(post_key_on_loop_start_a_[v]);
            }
        }
        key_on = 0;
        post_key_on_loop_start_mask_ &= ~on;
    }
}

void Spu2Core::update_noise() noexcept {
    static const std::uint8_t noise_add[64] = {
        1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
        0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1,
    };
    static const std::uint16_t noise_freq_add[5] = {0, 84, 140, 180, 210};

    std::uint32_t level = 0x8000 >> (noise_clk >> 2);
    level <<= 16;

    noise_cnt += 0x10000;
    noise_cnt += noise_freq_add[noise_clk & 3];
    if ((noise_cnt & 0xffff) >= noise_freq_add[4]) {
        noise_cnt += 0x10000;
        noise_cnt -= noise_freq_add[noise_clk & 3];
    }

    if (noise_cnt >= level) {
        while (noise_cnt >= level) {
            noise_cnt -= level;
        }
        noise_out = static_cast<std::uint16_t>((noise_out << 1) | noise_add[(noise_out >> 10) & 63]);
    }
}

Spu2Core::MixResult Spu2Core::mix_voices(const std::uint16_t* const ram, const std::int32_t ext_left,
                                         const std::int32_t ext_right) noexcept {
    master_volume_left.update();
    master_volume_right.update();
    update_noise();

    std::int32_t dry_left = 0;
    std::int32_t dry_right = 0;
    std::int32_t wet_left = 0;
    std::int32_t wet_right = 0;
    std::int32_t prev_out_x = 0;
    for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
        Spu2Voice& voice = voices[v];
        const std::int32_t value = voice.mix_step(ram, noise_value(), prev_out_x, v);
        prev_out_x = voice.out_x;
        if (voice.hit_loop_end) {
            endx |= 1U << v;
        }
        const int scaled_left = static_cast<int>((static_cast<std::int64_t>(value) * voice.volume_left.value) >> 15);
        const int scaled_right = static_cast<int>((static_cast<std::int64_t>(value) * voice.volume_right.value) >> 15);
        dry_left += voice.gate_dry_l ? scaled_left : 0;
        dry_right += voice.gate_dry_r ? scaled_right : 0;
        wet_left += voice.gate_wet_l ? scaled_left : 0;
        wet_right += voice.gate_wet_r ? scaled_right : 0;
    }

    const std::int32_t voice_dry_left = spu2_clamp16(dry_left);
    const std::int32_t voice_dry_right = spu2_clamp16(dry_right);
    const std::int32_t voice_wet_left = spu2_clamp16(wet_left);
    const std::int32_t voice_wet_right = spu2_clamp16(wet_right);
    return MixResult{
        spu2_clamp16((bus_voice_dry_l ? voice_dry_left : 0) + (bus_ext_dry_l ? ext_left : 0)),
        spu2_clamp16((bus_voice_dry_r ? voice_dry_right : 0) + (bus_ext_dry_r ? ext_right : 0)),
        spu2_clamp16((bus_voice_wet_l ? voice_wet_left : 0) + (bus_ext_wet_l ? ext_left : 0)),
        spu2_clamp16((bus_voice_wet_r ? voice_wet_right : 0) + (bus_ext_wet_r ? ext_right : 0)),
    };
}

void Spu2Core::mix_block(const std::uint16_t* const ram, const std::size_t count, const std::int32_t* const ext_left,
                         const std::int32_t* const ext_right, MixResult* const out, std::int32_t* const master_left,
                         std::int32_t* const master_right) noexcept {
    // Core-level per-sample state first: master sweeps and the noise generator
    // do not depend on the voices.
    std::array<std::int32_t, kMaxMixBlock> noise;
    for (std::size_t i = 0; i < count; ++i) {
        if (master_volume_left.sweep_enabled()) {
            master_volume_left.update();
        }
        if (master_volume_right.sweep_enabled()) {
            master_volume_right.update();
        }
        if (master_left != nullptr) {
            master_left[i] = master_volume_left.value;
        }
        if (master_right != nullptr) {
            master_right[i] = master_volume_right.value;
        }
        update_noise();
        noise[i] = noise_value();
    }

    std::array<std::int32_t, kMaxMixBlock> dry_left;
    std::array<std::int32_t, kMaxMixBlock> dry_right;
    std::array<std::int32_t, kMaxMixBlock> wet_left;
    std::array<std::int32_t, kMaxMixBlock> wet_right;
    std::fill_n(dry_left.data(), count, 0);
    std::fill_n(dry_right.data(), count, 0);
    std::fill_n(wet_left.data(), count, 0);
    std::fill_n(wet_right.data(), count, 0);
    const Spu2Voice::BlockBus bus{dry_left.data(), dry_right.data(), wet_left.data(), wet_right.data()};

    // OutX is only buffered when the next voice is pitch-modulated by it;
    // two buffers alternate so a voice never overwrites its own input.
    std::array<std::array<std::int32_t, kMaxMixBlock>, 2> out_x_buffers;
    std::array<std::int32_t, kMaxMixBlock> zeros;
    std::fill_n(zeros.data(), count, 0);
    const std::int32_t* prev_out_x = zeros.data();
    for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
        Spu2Voice& voice = voices[v];
        const bool feeds_next = v + 1 < kSpu2VoicesPerCore && voices[v + 1].modulated;
        std::int32_t* const out_x = feeds_next ? out_x_buffers[v & 1].data() : nullptr;
        if (voice.mix_block(ram, count, noise.data(), prev_out_x, v, out_x, bus)) {
            endx |= 1U << v;
        }
        prev_out_x = feeds_next ? out_x : zeros.data();
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t voice_dry_left = spu2_clamp16(dry_left[i]);
        const std::int32_t voice_dry_right = spu2_clamp16(dry_right[i]);
        const std::int32_t voice_wet_left = spu2_clamp16(wet_left[i]);
        const std::int32_t voice_wet_right = spu2_clamp16(wet_right[i]);
        const std::int32_t el = ext_left != nullptr ? ext_left[i] : 0;
        const std::int32_t er = ext_right != nullptr ? ext_right[i] : 0;
        MixResult& result = out[i];
        result.mix_left = spu2_clamp16((bus_voice_dry_l ? voice_dry_left : 0) + (bus_ext_dry_l ? el : 0));
        result.mix_right = spu2_clamp16((bus_voice_dry_r ? voice_dry_right : 0) + (bus_ext_dry_r ? er : 0));
        result.wet_left = spu2_clamp16((bus_voice_wet_l ? voice_wet_left : 0) + (bus_ext_wet_l ? el : 0));
        result.wet_right = spu2_clamp16((bus_voice_wet_r ? voice_wet_right : 0) + (bus_ext_wet_r ? er : 0));
    }
}

// ---------------------------------------------------------------------------
// Spu2System

Spu2System::Spu2System() noexcept : cores{Spu2Core{0}, Spu2Core{1}} {}

bool Spu2System::capture_irq_due(const std::size_t core, const std::uint32_t position) const noexcept {
    if (!cores[core].irq_enable) {
        return false;
    }
    // The fixed capture rings occupy words 0x400..0x1fff.
    const auto irqa = cores[core].irqa & kSpu2AddressMask;
    return irqa >= 0x400 && irqa < 0x2000 && (irqa & 0x1ff) == position;
}

Spu2System::BlockResult Spu2System::step_block(const std::span<Stereo> out) noexcept {
    std::size_t count = std::min(out.size(), kMaxBlockSamples);
    BlockResult result{};
    if (count == 0) {
        return result;
    }

    if (fixed_capture_irq_observer_) {
        // End the block on the first sample whose capture write hits an
        // enabled IRQA, so the observer runs exactly between two samples.
        for (std::size_t core = 0; core < 2; ++core) {
            if (!cores[core].irq_enable) {
                continue;
            }
            const auto irqa = cores[core].irqa & kSpu2AddressMask;
            if (irqa < 0x400 || irqa >= 0x2000) {
                continue;
            }
            count = std::min(static_cast<std::size_t>(((irqa - capture_position_) & 0x1ff) + 1), count);
        }
        for (std::size_t core = 0; core < 2; ++core) {
            if (capture_irq_due(core, (capture_position_ + static_cast<std::uint32_t>(count) - 1) & 0x1ff)) {
                result.irq_core_mask |= static_cast<std::uint8_t>(1U << core);
            }
        }
    }

    cores[0].process_key_latches();
    cores[1].process_key_latches();

    // Only the first `count` entries of each are written before being read.
    std::array<Spu2Core::MixResult, kMaxBlockSamples> mix0;
    std::array<Spu2Core::MixResult, kMaxBlockSamples> mix1;
    std::array<std::int32_t, kMaxBlockSamples> master0_left;
    std::array<std::int32_t, kMaxBlockSamples> master0_right;
    std::array<std::int32_t, kMaxBlockSamples> master1_left;
    std::array<std::int32_t, kMaxBlockSamples> master1_right;
    std::array<std::int32_t, kMaxBlockSamples> ext_left;
    std::array<std::int32_t, kMaxBlockSamples> ext_right;

    cores[0].mix_block(ram.data(), count, nullptr, nullptr, mix0.data(), master0_left.data(), master0_right.data());
    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t core0_left = spu2_clamp16(mix0[i].mix_left);
        const std::int32_t core0_right = spu2_clamp16(mix0[i].mix_right);
        std::int32_t left = (master0_left[i] * core0_left) >> 15;
        std::int32_t right = (master0_right[i] * core0_right) >> 15;
        ext_left[i] = (cores[1].ext_volume_left * left) >> 15;
        ext_right[i] = (cores[1].ext_volume_right * right) >> 15;
    }

    cores[1].mix_block(ram.data(), count, ext_left.data(), ext_right.data(), mix1.data(), master1_left.data(),
                       master1_right.data());
    for (std::size_t i = 0; i < count; ++i) {
        out[i].left = static_cast<std::int16_t>(spu2_clamp16((spu2_clamp16(mix1[i].mix_left) * master1_left[i]) >> 15));
        out[i].right =
            static_cast<std::int16_t>(spu2_clamp16((spu2_clamp16(mix1[i].mix_right) * master1_right[i]) >> 15));
    }

    capture_position_ = (capture_position_ + static_cast<std::uint32_t>(count)) & 0x1ff;
    g_trace_samples += count;
    result.samples = count;
    return result;
}

std::size_t Spu2System::dma_upload_bytes(const std::uint32_t byte_address,
                                         const std::span<const std::byte> bytes) noexcept {
    std::uint32_t word = (byte_address >> 1) & kSpu2AddressMask;
    std::size_t written = 0;
    for (; written + 1 < bytes.size(); written += 2) {
        const auto [lo, hi] = std::array{bytes[written], bytes[written + 1]};
        ram[word] = static_cast<std::uint16_t>(static_cast<int>(lo) | (static_cast<int>(hi) << 8));
        word = (word + 1) & kSpu2AddressMask;
    }
    return written;
}

Spu2System::Stereo Spu2System::step() noexcept {
    cores[0].process_key_latches();
    cores[1].process_key_latches();

    const Spu2Core::MixResult m0 = cores[0].mix_voices(ram.data(), 0, 0);
    const std::int32_t core0_left = spu2_clamp16(m0.mix_left);
    const std::int32_t core0_right = spu2_clamp16(m0.mix_right);
    std::int32_t ext_left = (cores[0].master_volume_left.value * core0_left) >> 15;
    std::int32_t ext_right = (cores[0].master_volume_right.value * core0_right) >> 15;
    ext_left = (cores[1].ext_volume_left * ext_left) >> 15;
    ext_right = (cores[1].ext_volume_right * ext_right) >> 15;

    const Spu2Core::MixResult m1 = cores[1].mix_voices(ram.data(), ext_left, ext_right);
    const Stereo result{
        static_cast<std::int16_t>(spu2_clamp16((spu2_clamp16(m1.mix_left) * cores[1].master_volume_left.value) >> 15)),
        static_cast<std::int16_t>(
            spu2_clamp16((spu2_clamp16(m1.mix_right) * cores[1].master_volume_right.value) >> 15)),
    };

    if (fixed_capture_irq_observer_) {
        // Word bases of the fixed capture rings written every sample.
        static constexpr std::array<std::uint32_t, 14> kCaptureBases{
            0x400, 0x600, 0x800, 0xa00, 0xc00, 0xe00, 0x1000, 0x1200, 0x1400, 0x1600, 0x1800, 0x1a00, 0x1c00, 0x1e00,
        };
        for (std::uint8_t irq_core = 0; irq_core < 2; ++irq_core) {
            if (!cores[irq_core].irq_enable) {
                continue;
            }
            const std::uint32_t irqa = cores[irq_core].irqa & kSpu2AddressMask;
            if (std::ranges::any_of(kCaptureBases,
                                    [&](const std::uint32_t base) { return base + capture_position_ == irqa; })) {
                fixed_capture_irq_observer_(irq_core);
            }
        }
    }
    capture_position_ = (capture_position_ + 1) & 0x1ff;
    return result;
}

} // namespace takedown::audio
