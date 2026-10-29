#pragma once

// The game's Music and SFX levels before a profile loads (SLUS-21050).
//
// The game keeps its audio options in a settings block at 0x004F5040 (the
// Music level is the signed byte at 0x004F5074, the SFX level the one at
// 0x004F5075, each 0 to 100 in steps of 5). At boot its defaults routine
// (0x0021B770) sets both to 90, and only loading a profile (0x00421940)
// copies the player's own levels in, so everything before that (the intro,
// the title screen, the profile screens) plays at 90 whatever the player
// chose. (The profile pointer at 0x0051BF9C is already set at boot, so it
// does not tell whether a profile is loaded.)
//
// This remembers the player's levels in the PC settings and writes them over
// the boot defaults as soon as those are set (the three 90s), so the game
// starts at the player's levels. From then on the game's own block is never
// written again: every later change of the levels (a profile loading, the
// Options page) is reported for saving.

#include "takedown/guest/types.hpp"

#include <cstdint>
#include <optional>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

struct GameAudioLevels {
    std::uint32_t music{};
    std::uint32_t effects{};

    bool operator==(const GameAudioLevels&) const = default;
};

class Slus21050BootVolume {
public:
    static constexpr guest::GuestAddress kMusicLevel = 0x004F5074U;
    static constexpr guest::GuestAddress kEffectsLevel = 0x004F5075U;
    // The third level the defaults routine sets with them (90).
    static constexpr guest::GuestAddress kThirdLevel = 0x004F5076U;
    static constexpr std::uint32_t kBootDefault = 90U;

    // `remembered`: the levels saved from an earlier session, if any.
    explicit Slus21050BootVolume(std::optional<GameAudioLevels> remembered) noexcept
        : remembered_(remembered) {}

    // One poll of the guest. Returns the levels to remember when the game's
    // levels changed after boot and differ from the remembered ones.
    std::optional<GameAudioLevels> poll(guest::EeMemory& memory);

    [[nodiscard]] bool applied() const noexcept { return applied_; }
    [[nodiscard]] const std::optional<GameAudioLevels>& remembered() const noexcept {
        return remembered_;
    }

private:
    std::optional<GameAudioLevels> remembered_;
    // The levels last seen after the boot defaults; nullopt before them.
    std::optional<GameAudioLevels> last_;
    bool applied_{false};
};

} // namespace takedown::runtime
