#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace takedown::audio {

struct RwaSpu2KeyOn {
    std::uint8_t core{};
    std::uint8_t voice{};
    std::uint32_t start_address{};
    std::uint16_t pitch{};
    std::uint16_t volume_left{};
    std::uint16_t volume_right{};
    std::uint16_t adsr1{};
    std::uint16_t adsr2{};
};

// Minimal libsd-facing state used by the retail RWA mixer. It intentionally
// models register calls, not game events: callers must supply the exact
// sceSdSet* arguments produced by executed IOP code.
class RwaSpu2VoiceState {
  public:
    using KeyOnSink = std::function<void(const RwaSpu2KeyOn&)>;
    // KOFF latch (set_switch function 0x1600): core + 24-bit voice mask.
    using KeyOffSink = std::function<void(std::uint8_t core, std::uint32_t mask)>;
    // Live register update on an accepted sceSdSetParam/sceSdSetAddr:
    // core, voice, raw entry selector (0x0000 VOLL, 0x0100 VOLR, 0x0200
    // pitch, 0x0300 ADSR1, 0x0400 ADSR2, 0x2000 SSA) and the raw value
    // (u16 fields as written; SSA is the 32-bit address).
    using ParamSink =
        std::function<void(std::uint8_t core, std::uint8_t voice, std::uint16_t entry,
                           std::uint32_t value)>;

    explicit RwaSpu2VoiceState(KeyOnSink sink = {}) : key_on_sink_(std::move(sink)) {
        // SPU2 reset: every voice mixes into both outputs on both cores.
        for (auto core = 0U; core < 2U; ++core) {
            for (auto index = 5U; index <= 8U; ++index)
                switch_masks_[core][index] = 0x00ffffffU;
        }
    }

    void set_key_off_sink(KeyOffSink sink) { key_off_sink_ = std::move(sink); }
    void set_param_sink(ParamSink sink) { param_sink_ = std::move(sink); }
    struct Stats { std::uint64_t set_addr{}, set_param{}, set_switch{}, rejected{};
                   std::uint64_t kon{}, koff{}; std::uint16_t last_entry{};
                   std::uint32_t last_value{};
                   // Rejection catalogue, indexed by the entry's selector
                   // byte ((entry >> 8) & 0xff) across set_param/set_switch,
                   // and bucket 0xff for set_addr rejections.  Shows which
                   // libsd selectors the executed retail code uses that the
                   // filter does not yet model.
                   std::array<std::uint64_t, 256> rejected_by_selector{};
                   // The most recent rejected calls verbatim (entry, value),
                   // so the selector histogram can be resolved into exact
                   // entries without another instrumented run.
                   std::array<std::uint16_t, 8> last_rejected_entries{};
                   std::array<std::uint32_t, 8> last_rejected_values{};
                   // 0 = set_addr, 1 = set_param, 2 = set_switch.
                   std::array<std::uint8_t, 8> last_rejected_kinds{};
                   std::uint32_t last_rejected_count{0}; };

    [[nodiscard]] bool set_addr(std::uint16_t entry, std::uint32_t value) {
        // Core-level addresses (ESA/EEA/TSA/IRQA): selector byte, core in
        // bit 0.  Voice addresses (SSA/LSAX/NAX): voice in bits 5:1, with
        // core 1 carrying selector bit 6 (observed executed RWA.IRX writes
        // 0x2040-family entries for its core-1 voices; bit-6-clear entries
        // keep the legacy bit-0 core decode that produced the verified
        // exact-wave matches).
        const auto base = entry & 0xff00U;
        if (base == 0x1c00U || base == 0x1d00U || base == 0x1e00U ||
            base == 0x1f00U) {
            core_addrs_[entry & 1U][(base - 0x1c00U) >> 8U] = value;
            ++stats_.set_addr; stats_.last_entry = entry; stats_.last_value = value;
            if (param_sink_) {
                param_sink_(static_cast<std::uint8_t>(entry & 1U), 0xffU, entry, value);
            }
            return true;
        }
        if (base == 0x2000U || base == 0x2100U || base == 0x2200U) {
            const auto voice = static_cast<std::uint8_t>((entry >> 1U) & 0x1fU);
            // Retail selector construction (RWA init 0x1934-0x19ac): entry
            // = family | 0x40 | (voice << 1) | core — the 0x40 tag belongs
            // to the SSA/LSAX/NAX selector BASE (0x2062 = SSA of voice 17
            // CORE 0), it is not a core bit. Core is bit 0 alone; the old
            // bit-6 decode mis-routed every tagged core-0 voice to core 1.
            const auto core = static_cast<std::uint8_t>(entry & 1U);
            if (voice >= 24U || value >= 0x200000U) {
                record_reject(entry, value, 0);
                return false;
            }
            ++stats_.set_addr; stats_.last_entry = entry; stats_.last_value = value;
            voices_[core][voice].start_address = value;
            if (param_sink_) {
                param_sink_(core, voice, entry, value);
            }
            return true;
        }
        record_reject(entry, value, 0);
        return false;
    }

    [[nodiscard]] bool set_param(std::uint16_t entry, std::uint16_t value) {
        // Core-level params carry an 0x80 tag in the low byte (ps2sdk
        // libsd-common.h): MVOLL/MVOLR, EVOLL/EVOLR, AVOLL/AVOLR,
        // BVOLL/BVOLR, MVOLXL/MVOLXR — all observed in executed retail
        // traffic.  Core in bit 0.  Checked before the voice-param decode
        // because the voice switch's selector set excludes them.
        if ((entry & 0x8080U) == 0x0080U) {
            const auto core = static_cast<std::uint8_t>(entry & 1U);
            switch (entry & 0x7f00U) {
            case 0x0900U:  // MVOLL
            case 0x0a00U:  // MVOLR
            case 0x0b00U:  // EVOLL
            case 0x0c00U:  // EVOLR
            case 0x0d00U:  // AVOLL
            case 0x0e00U:  // AVOLR
            case 0x0f00U:  // BVOLL
            case 0x1000U:  // BVOLR
            case 0x1100U:  // MVOLXL
            case 0x1200U:  // MVOLXR
                break;
            default:
                record_reject(entry, value, 1);
                return false;
            }
            core_params_[core][(entry & 0x7f00U) >> 8U] = value;
            ++stats_.set_param; stats_.last_entry = entry; stats_.last_value = value;
            if (param_sink_) {
                param_sink_(core, 0xffU, entry, value);
            }
            return true;
        }
        const auto decoded = decode_voice(entry);
        if (!decoded.valid) {
            record_reject(entry, value, 1);
            return false;
        }
        auto& voice = voices_[decoded.core][decoded.voice];
        switch (entry & 0xff00U) {
        case 0x0000U: voice.volume_left = value; break;
        case 0x0100U: voice.volume_right = value; break;
        case 0x0200U: voice.pitch = value; break;
        case 0x0300U: voice.adsr1 = value; break;
        case 0x0400U: voice.adsr2 = value; break;
        default: record_reject(entry, value, 1); return false;
        }
        ++stats_.set_param; stats_.last_entry = entry; stats_.last_value = value;
        if (param_sink_) {
            param_sink_(decoded.core, decoded.voice, entry & 0xff00U, value);
        }
        return true;
    }

    [[nodiscard]] bool set_switch(std::uint16_t entry, std::uint32_t mask) {
        const auto core = static_cast<std::uint8_t>(entry & 1U);
        const auto function = static_cast<std::uint16_t>(entry & 0xff00U);
        const auto known = function == 0x1500U /*KON*/ ||
                           function == 0x1600U /*KOFF*/ ||
                           function == 0x1300U /*PMON*/ ||
                           function == 0x1400U /*NON*/ ||
                           function == 0x1700U /*ENDX*/ ||
                           function == 0x1800U /*VMIXL*/ ||
                           function == 0x1900U /*VMIXEL*/ ||
                           function == 0x1a00U /*VMIXR*/ ||
                           function == 0x1b00U /*VMIXER*/;
        if ((mask & 0xff000000U) != 0U || !known) {
            record_reject(entry, mask, 2);
            return false;
        }
        ++stats_.set_switch; stats_.last_entry = entry; stats_.last_value = mask;
        switch_masks_[core][(function - 0x1300U) >> 8U] = mask;
        if (function != 0x1500U && function != 0x1600U) {
            // PMON/NON/ENDX/VMIX*: passed through the param sink with the
            // raw entry; the link maps them onto the core.
            if (param_sink_) {
                param_sink_(core, 0xffU, entry, mask);
            }
            return true;
        }
        if (function == 0x1600U) {
            ++stats_.koff;
            keyed_[core] &= ~mask;
            if (key_off_sink_) key_off_sink_(core, mask);
            return true;
        }
        // KON is a write-one trigger, not an edge against the voice's active
        // state. The hardware latch is consumed/cleared by the SPU2 mixer, so
        // writing KON again restarts an already active voice without a
        // preceding KOFF (PCSX2 likewise queues every written KON bit).
        const auto triggered = mask;
        ++stats_.kon;
        keyed_[core] |= mask;
        for (std::uint8_t voice = 0U; voice < 24U; ++voice) {
            if ((triggered & (1U << voice)) == 0U || !key_on_sink_) {
                continue;
            }
            const auto& state = voices_[core][voice];
            key_on_sink_({core, voice, state.start_address, state.pitch,
                          state.volume_left, state.volume_right, state.adsr1, state.adsr2});
        }
        return true;
    }

    [[nodiscard]] std::uint32_t keyed_mask(std::uint8_t core) const {
        return core < keyed_.size() ? keyed_[core] : 0U;
    }
    [[nodiscard]] const Stats& stats() const { return stats_; }

    // sceSdGetSwitch: the tracked switch mask for the entry's function.
    // Hardware reset defaults per the SPU2 register map: KON/KOFF/PMON/NON/
    // ENDX read 0; the four VMIX switches read all-voices-enabled.
    [[nodiscard]] std::uint32_t get_switch(std::uint16_t entry) const {
        const auto core = static_cast<std::uint8_t>(entry & 1U);
        const auto function = static_cast<std::uint16_t>(entry & 0xff00U);
        if (function == 0x1500U) return keyed_mask(core);
        if (function < 0x1300U || function > 0x1b00U) return 0U;
        return switch_masks_[core][(function - 0x1300U) >> 8U];
    }

    // sceSdGetParam: the tracked voice or core parameter.
    [[nodiscard]] std::uint16_t get_param(std::uint16_t entry) const {
        if ((entry & 0x8080U) == 0x0080U) {
            const auto core = static_cast<std::uint8_t>(entry & 1U);
            const auto selector = (entry & 0x7f00U) >> 8U;
            if (selector < 9U || selector >= core_params_[core].size()) return 0U;
            return core_params_[core][selector];
        }
        const auto decoded = decode_voice(entry);
        if (!decoded.valid) return 0U;
        const auto& voice = voices_[decoded.core][decoded.voice];
        switch (entry & 0xff00U) {
        case 0x0000U: return voice.volume_left;
        case 0x0100U: return voice.volume_right;
        case 0x0200U: return voice.pitch;
        case 0x0300U: return voice.adsr1;
        case 0x0400U: return voice.adsr2;
        default: return 0U;
        }
    }

    // sceSdGetAddr: the tracked SSA/LSAX/NAX voice address, or the core
    // ESA/EEA/TSA/IRQA address.
    [[nodiscard]] std::uint32_t get_addr(std::uint16_t entry) const {
        const auto base = static_cast<std::uint16_t>(entry & 0xff00U);
        if (base == 0x1c00U || base == 0x1d00U || base == 0x1e00U ||
            base == 0x1f00U) {
            return core_addrs_[entry & 1U][(base - 0x1c00U) >> 8U];
        }
        const auto decoded = decode_voice(entry);
        if (!decoded.valid) return 0U;
        return voices_[decoded.core][decoded.voice].start_address;
    }

  private:
    void record_reject(std::uint16_t entry, std::uint32_t value, std::uint8_t kind) {
        ++stats_.rejected;
        ++stats_.rejected_by_selector[(entry >> 8U) & 0xffU];
        const auto slot = stats_.last_rejected_count % stats_.last_rejected_entries.size();
        stats_.last_rejected_entries[slot] = entry;
        stats_.last_rejected_values[slot] = value;
        stats_.last_rejected_kinds[slot] = kind;
        ++stats_.last_rejected_count;
    }

    struct Voice {
        std::uint32_t start_address{0x5000U};
        std::uint16_t pitch{};
        std::uint16_t volume_left{};
        std::uint16_t volume_right{};
        std::uint16_t adsr1{};
        std::uint16_t adsr2{};
    };
    struct DecodedVoice {
        std::uint8_t core{};
        std::uint8_t voice{};
        bool valid{};
    };
    static constexpr DecodedVoice decode_voice(std::uint16_t entry) {
        const auto voice = static_cast<std::uint8_t>((entry >> 1U) & 0x1fU);
        return {static_cast<std::uint8_t>(entry & 1U), voice, voice < 24U};
    }

    std::array<std::array<Voice, 24>, 2> voices_{};
    std::array<std::uint32_t, 2> keyed_{};
    // Read side of the executed sceSdGet* imports: last-written switch
    // masks (functions 0x1300-0x1b00) with VMIX reset defaults, core-level
    // parameters (selectors 0x0900-0x1100), and core addresses.
    std::array<std::array<std::uint32_t, 9>, 2> switch_masks_{};
    std::array<std::array<std::uint16_t, 16>, 2> core_params_{};
    std::array<std::array<std::uint32_t, 4>, 2> core_addrs_{};
    KeyOnSink key_on_sink_{};
    KeyOffSink key_off_sink_{};
    ParamSink param_sink_{};
    Stats stats_{};
};

} // namespace takedown::audio
