#pragma once

#include "takedown/audio/rwa_spu2_voice.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::audio {

// Executable import boundary for the retail RWA.IRX image. The IOP interpreter
// calls this before executing an import stub; the stub and ordinal are checked
// byte-for-byte, so an unrelated module address can never become an audio call.
class RwaIrxLibSdHooks {
  public:
    explicit RwaIrxLibSdHooks(RwaSpu2VoiceState& voices) : voices_(voices) {}

    enum class Result { NotImport, Handled, Rejected };

    Result execute(std::span<const std::byte> text, std::uint32_t text_pc,
                   std::array<std::uint32_t, 32>& gpr, std::uint32_t& pc) {
        const auto ordinal = ordinal_for(text_pc);
        if (ordinal == 0U) {
            return Result::NotImport;
        }
        if (text_pc > text.size() || text.size() - text_pc < 8U ||
            read_u32(text, text_pc) != 0x03e00008U ||
            read_u32(text, text_pc + 4U) != (0x24020000U | ordinal)) {
            return Result::Rejected;
        }

        bool accepted = false;
        if (ordinal == 5U) {
            accepted = voices_.set_param(static_cast<std::uint16_t>(gpr[4U]),
                                         static_cast<std::uint16_t>(gpr[5U]));
        } else if (ordinal == 7U) {
            accepted = voices_.set_switch(static_cast<std::uint16_t>(gpr[4U]), gpr[5U]);
        } else if (ordinal == 9U) {
            accepted = voices_.set_addr(static_cast<std::uint16_t>(gpr[4U]), gpr[5U]);
        }
        if (!accepted) {
            return Result::Rejected;
        }
        gpr[2U] = 0U;
        pc = gpr[31U];
        return Result::Handled;
    }

  private:
    static constexpr std::uint32_t ordinal_for(std::uint32_t pc) {
        switch (pc) {
        case 0x8c04U: return 5U; // sceSdSetParam
        case 0x8c14U: return 7U; // sceSdSetSwitch
        case 0x8c24U: return 9U; // sceSdSetAddr
        default: return 0U;
        }
    }
    static std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
        return std::to_integer<std::uint32_t>(bytes[offset]) |
               (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
    }

    RwaSpu2VoiceState& voices_;
};

} // namespace takedown::audio
