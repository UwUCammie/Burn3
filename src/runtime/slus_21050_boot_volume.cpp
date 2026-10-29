#include "takedown/runtime/slus_21050_boot_volume.hpp"

#include "takedown/guest/ee_memory.hpp"

namespace takedown::runtime {

std::optional<GameAudioLevels> Slus21050BootVolume::poll(guest::EeMemory& memory) {
    if (!memory.contains(kMusicLevel, 3U)) {
        return std::nullopt;
    }
    const GameAudioLevels current{memory.read_u8(kMusicLevel), memory.read_u8(kEffectsLevel)};

    if (!last_.has_value()) {
        // Still waiting for the defaults routine: all three levels read 90.
        if (current.music != kBootDefault || current.effects != kBootDefault ||
            memory.read_u8(kThirdLevel) != kBootDefault) {
            return std::nullopt;
        }
        if (remembered_.has_value()) {
            memory.write_u8(kMusicLevel, static_cast<std::uint8_t>(remembered_->music));
            memory.write_u8(kEffectsLevel, static_cast<std::uint8_t>(remembered_->effects));
            applied_ = true;
            last_ = remembered_;
        } else {
            last_ = current;
        }
        return std::nullopt;
    }

    const GameAudioLevels& levels = current;
    if (levels == *last_ || current.music > 100U || current.effects > 100U) {
        return std::nullopt;
    }
    last_ = levels;
    if (remembered_ == levels) {
        return std::nullopt;
    }
    remembered_ = levels;
    return levels;
}

} // namespace takedown::runtime
