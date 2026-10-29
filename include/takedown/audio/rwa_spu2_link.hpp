#pragma once

// Bridge from the executed retail libsd traffic onto the hardware-faithful
// SPU2: RwaSpu2VoiceState observes the exact sceSdSet* calls the shadow IOP's
// RWA.IRX makes and the executor observes every sceSdVoiceTrans byte copy;
// this adapter applies both onto a Spu2System so guest audio state lives in
// the modelled machine (docs/AUDIO.md §8 program, increment 2).
//
// Addressing: the executor's SPU2 shadow image and the SSA values the KONs
// carry are byte addresses in the 2 MiB space (the SSA == VoiceTrans byte
// offset identity is what the exact-wave matching already proves); the
// Spu2System voice core is word-addressed, so the link halves them.

#include "takedown/audio/rwa_spu2_voice.hpp"
#include "takedown/audio/spu2_core.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace takedown::audio {

class Spu2GuestLink {
  public:
    explicit Spu2GuestLink(Spu2System& system) noexcept : system_(system) {}

    // Guest-write ordering barrier (docs/archive/AUDIO_HISTORY.md section 8, increment 6).
    // The owner (the app, which owns the guest clock) installs a callback
    // that retires every modelled sample due strictly before the current
    // guest cycle — e.g. Spu2Renderer::drain_through(emulated_ee_cycles) —
    // so no later register/DMA/KON/KOFF write can alter an earlier sample
    // regardless of how far periodic rendering lags behind. Unset (tests,
    // direct machine driving): no barrier, historical behavior.
    void set_write_barrier(std::function<void()> barrier) noexcept {
        write_barrier_ = std::move(barrier);
    }

    // A real guest KON: program the voice's register file, then latch KON
    // (write-one; re-KON restarts a running voice, as on hardware).
    void program_key_on(const RwaSpu2KeyOn& event) noexcept {
        program_key_on_impl(event, false, 0);
    }

    // RWA's opcode-0x14 sequence writes LSAX after KON.  Keep that ordering
    // attached to the sample-boundary KON latch so voice start cannot erase
    // the manual loop selection before the first decoded block.
    void program_key_on_with_post_loop(const RwaSpu2KeyOn& event,
                                       std::uint32_t loop_byte_address) noexcept {
        program_key_on_impl(event, true, loop_byte_address);
    }

    // A real guest KOFF latch (24-bit voice mask per core).
    void key_off(const std::uint8_t core, const std::uint32_t mask) noexcept {
        if (write_barrier_) write_barrier_();
        if (core >= 2U) {
            return;
        }
        system_.key_off(core, mask & 0xFFFFFFU);
        ++key_offs_;
    }

    // sceSdSetCoreAttr writes one named core attribute, not the packed SPU2
    // ATTR register consumed by Spu2Core::write_attr.  The selector occupies
    // the even bits and bit 0 selects the core.  Keep the field-wise update
    // here so changing EFFECT_ENABLE cannot accidentally reset NOISE_CLK (or
    // vice versa). IRQ, mute, and SPDIF are retained as register state even
    // though today's PCM renderer does not consume those controls.
    void set_core_attr(const std::uint16_t entry, const std::uint16_t value) noexcept {
        if (write_barrier_) write_barrier_();
        Spu2Core& core = system_.cores[entry & 1U];
        switch (entry & 0xfffeU) {
        case 0x0002U: // SD_CORE_EFFECT_ENABLE
            core.fx_enable = value != 0U;
            break;
        case 0x0008U: // SD_CORE_NOISE_CLK (six-bit hardware field)
            core.noise_clk = value & 0x3fU;
            break;
        case 0x0004U: // SD_CORE_IRQ_ENABLE
            core.irq_enable = value != 0U;
            break;
        case 0x0006U: // SD_CORE_MUTE_ENABLE
            core.mute_enable = value != 0U;
            break;
        case 0x000aU: // SD_CORE_SPDIF_MODE (global; core bit is ignored)
            system_.spdif_mode = value;
            break;
        default:
            break;
        }
    }

    // Register call application: voice params, the 0x80-tagged core params,
    // VADDR address writes, core-level addresses, and the switch families
    // (PMON/NON/ENDX/VMIX*).  `voice` is 0xff for core-level entries.
    // libsd addresses are byte addresses; the core is word-addressed, so
    // address values are halved here and nowhere else.
    void set_param(const std::uint8_t core, const std::uint8_t voice, const std::uint16_t entry,
                   const std::uint32_t value) noexcept {
        if (write_barrier_) write_barrier_();
        if (core >= 2U) {
            return;
        }
        // Core-level params: 0x80 tag, selector in bits 14:8, core in bit 0.
        if ((entry & 0x8080U) == 0x0080U) {
            Spu2Core& c = system_.cores[core];
            switch (entry & 0x7f00U) {
            case 0x0900U:
                c.master_volume_left.set_reg(static_cast<std::uint16_t>(value));
                break;
            case 0x0a00U:
                c.master_volume_right.set_reg(static_cast<std::uint16_t>(value));
                break;
            case 0x0b00U:
                c.fx_volume_left = static_cast<std::int16_t>(value);
                break;
            case 0x0c00U:
                c.fx_volume_right = static_cast<std::int16_t>(value);
                break;
            case 0x0d00U: // AVOL: core 0 external-input volume
            case 0x0f00U: // BVOL: core 1 external-input volume
                c.ext_volume_left = static_cast<std::int16_t>(value);
                break;
            case 0x0e00U:
            case 0x1000U:
                c.ext_volume_right = static_cast<std::int16_t>(value);
                break;
            case 0x1100U:
                c.master_volume_left.set_value_raw(value);
                break;
            case 0x1200U:
                c.master_volume_right.set_value_raw(value);
                break;
            default:
                return;
            }
            ++param_writes_;
            return;
        }
        // Switch families arrive through the same sink (voice = 0xff).
        if (voice == 0xffU) {
            Spu2Core& c = system_.cores[core];
            switch (entry & 0xff00U) {
            case 0x1300U: // PMON
                c.write_pmon(static_cast<std::uint16_t>(value & 0xffffU),
                             static_cast<std::uint16_t>((value >> 16) & 0xffU));
                break;
            case 0x1400U: // NON
                c.write_non(static_cast<std::uint16_t>(value & 0xffffU),
                            static_cast<std::uint16_t>((value >> 16) & 0xffU));
                break;
            case 0x1700U: // ENDX write clears bits
                c.endx &= value & 0xFFFFFFU;
                break;
            case 0x1c00U: // ESA
                c.effects_start_a = value >> 1U;
                break;
            case 0x1d00U: // EEA
                c.effects_end_a = value >> 1U;
                break;
            case 0x1e00U: // TSA
                c.transfer_start_a = value >> 1U;
                break;
            case 0x1f00U: // IRQA
                c.irqa = value >> 1U;
                break;
            case 0x1800U:
                apply_vmix_bus(c, 0, value);
                break; // VMIXL
            case 0x1900U:
                apply_vmix_bus(c, 1, value);
                break; // VMIXEL
            case 0x1a00U:
                apply_vmix_bus(c, 2, value);
                break; // VMIXR
            case 0x1b00U:
                apply_vmix_bus(c, 3, value);
                break; // VMIXER
            default:
                return;
            }
            ++param_writes_;
            return;
        }
        if (voice >= kSpu2VoicesPerCore) {
            return;
        }
        Spu2Voice& v = system_.cores[core].voices[voice];
        switch (entry & 0xffc0U) {
        case 0x0000U:
            v.volume_left.set_reg(static_cast<std::uint16_t>(value));
            break;
        case 0x0100U:
            v.volume_right.set_reg(static_cast<std::uint16_t>(value));
            break;
        case 0x0200U:
            v.pitch = static_cast<std::uint16_t>(value);
            break;
        case 0x0300U:
            v.adsr.set_regs((v.adsr.reg32 & 0xFFFF0000U) | (value & 0xFFFFU));
            break;
        case 0x0400U:
            v.adsr.set_regs((v.adsr.reg32 & 0x0000FFFFU) | (value << 16));
            break;
        case 0x2000U: // SSA (bit 6 selects core 1, already decoded for us)
        case 0x2040U:
            v.set_start_a(value >> 1U);
            break;
        case 0x2100U: // LSAX
        case 0x2140U:
            v.set_loop_start_a(value >> 1U);
            break;
        case 0x2200U: // NAX
        case 0x2240U:
            v.set_next_a(value >> 1U);
            break;
        case 0x1c00U: // ESA
            system_.cores[core].effects_start_a = value >> 1U;
            break;
        case 0x1d00U: // EEA
            system_.cores[core].effects_end_a = value >> 1U;
            break;
        case 0x1e00U: // TSA
            system_.cores[core].transfer_start_a = value >> 1U;
            break;
        case 0x1f00U: // IRQA
            system_.cores[core].irqa = value >> 1U;
            break;
        default:
            return;
        }
        ++param_writes_;
    }

    // Apply one voice-volume register write from an event already executing
    // at the SPU2 sample boundary. Calling the guest-cycle write barrier from
    // inside Spu2System::step would recursively drain the renderer; this
    // narrow entry retains selector validation and write telemetry without
    // attempting a second timeline synchronization.
    void set_voice_volume_at_sample_boundary(const std::uint8_t core,
                                             const std::uint8_t voice,
                                             const bool right,
                                             const std::uint16_t value) noexcept {
        if (core >= system_.cores.size() || voice >= kSpu2VoicesPerCore) return;
        auto& spu_voice = system_.cores[core].voices[voice];
        if (right) {
            spu_voice.volume_right.set_reg(value);
        } else {
            spu_voice.volume_left.set_reg(value);
        }
        ++param_writes_;
    }

    // The executor's VoiceTrans byte copies land in the modelled RAM.
    // Returns the bytes actually written (even-byte pairs).
    std::size_t dma_upload(const std::uint32_t byte_address,
                           const std::span<const std::byte> bytes) noexcept {
        if (write_barrier_) write_barrier_();
        const std::size_t written = system_.dma_upload_bytes(byte_address, bytes);
        dma_bytes_written_ += written;
        if (written != 0U) {
            ++dma_transfers_written_;
        }
        return written;
    }

    [[nodiscard]] std::uint64_t key_ons_programmed() const noexcept { return key_ons_programmed_; }
    [[nodiscard]] std::uint64_t key_offs() const noexcept { return key_offs_; }
    [[nodiscard]] std::uint64_t param_writes() const noexcept { return param_writes_; }
    [[nodiscard]] std::uint64_t dma_bytes_written() const noexcept { return dma_bytes_written_; }
    [[nodiscard]] std::uint64_t dma_transfers_written() const noexcept {
        return dma_transfers_written_;
    }
    // Voices with a non-stopped envelope across both cores (audited state,
    // read at end-of-run reporting; the core is not yet rendered to a sink).
    [[nodiscard]] int active_voices() const noexcept {
        int active = 0;
        for (const auto& core : system_.cores) {
            for (const auto& voice : core.voices) {
                if (voice.active())
                    ++active;
            }
        }
        return active;
    }

  private:
    void program_key_on_impl(const RwaSpu2KeyOn& event, const bool has_post_loop,
                             const std::uint32_t loop_byte_address) noexcept {
        if (write_barrier_) write_barrier_();
        if (event.core >= 2U || event.voice >= kSpu2VoicesPerCore) {
            return;
        }
        Spu2Voice& voice = system_.cores[event.core].voices[event.voice];
        voice.set_start_a(event.start_address >> 1U);
        voice.pitch = event.pitch;
        voice.volume_left.set_reg(event.volume_left);
        voice.volume_right.set_reg(event.volume_right);
        voice.adsr.set_regs(Spu2Adsr::make_regs(event.adsr1, event.adsr2));
        if (has_post_loop) {
            system_.key_on_with_post_loop_start(event.core, event.voice,
                                                loop_byte_address >> 1U);
        } else {
            system_.key_on(event.core, 1U << event.voice);
        }
        ++key_ons_programmed_;
    }

    // VMIX switch masks are 24-bit voice allocations for one bus.
    void apply_vmix_bus(Spu2Core& core, const int bus, const std::uint32_t mask) const noexcept {
        for (int v = 0; v < kSpu2VoicesPerCore; ++v) {
            const bool on = ((mask >> v) & 1U) != 0U;
            switch (bus) {
            case 0:
                core.voices[v].gate_dry_l = on;
                break;
            case 1:
                core.voices[v].gate_wet_l = on;
                break;
            case 2:
                core.voices[v].gate_dry_r = on;
                break;
            case 3:
                core.voices[v].gate_wet_r = on;
                break;
            default:
                break;
            }
        }
    }

    Spu2System& system_;
    std::function<void()> write_barrier_{};
    std::uint64_t key_ons_programmed_{0};
    std::uint64_t key_offs_{0};
    std::uint64_t param_writes_{0};
    std::uint64_t dma_bytes_written_{0};
    std::uint64_t dma_transfers_written_{0};
};

} // namespace takedown::audio
