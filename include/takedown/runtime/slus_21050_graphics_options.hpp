#pragma once

#include "takedown/guest/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {
class NativeFunctionDispatcher;

// PC graphics options that SLUS-21050 itself implements, applied natively.
//
// The retail game carries an unused widescreen camera: a flag in its global
// render settings selects 16:9 aspect values that are already present in its
// data, and two field-of-view constants widen the view to match. Motion blur
// and bloom use native pass admission at their complete game-effect call
// boundaries. Their options never overwrite the console's effect flag bytes.
//
// The values the game resets are enforced at every VBlank while an option
// differs from retail; when an option returns to retail the retail value is
// written back once and the block is left alone again. With every option at
// its retail value nothing is ever written, so default runs stay identical.
//
// Not covered: the 2D overlays (HUD, menus, borders) keep their 4:3 layout
// and are stretched with the picture, because repositioning them needs
// changes inside game functions rather than in data.
class Slus21050GraphicsOptions {
  public:
    struct Settings {
        bool widescreen{false};
        bool motion_blur{true};
        bool bloom{true};

        bool operator==(const Settings&) const = default;
    };

    struct Statistics {
        std::uint64_t vblanks{};
        std::uint64_t writes{};
        std::uint64_t restores{};
        std::uint64_t effects_skipped{};
    };

    // Describes the first data value that differs from the revision this
    // module was written against (in either its retail or its overridden
    // form); nullopt when all of them match.
    [[nodiscard]] static std::optional<std::string>
    signature_mismatch(const guest::EeMemory& memory);

    void configure(const Settings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }
    // Authenticate whole game-effect routines and route disabled effects at
    // their native call boundaries. Enabled effects retain their game code.
    [[nodiscard]] bool register_native_effects(NativeFunctionDispatcher&, const guest::EeMemory&);

    // Cheap when every option is at its retail value: no memory access.
    void on_vblank(guest::EeMemory& memory);

    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }

  private:
    enum class Group : std::uint8_t { Widescreen };
    struct Override {
        guest::GuestAddress address;
        std::uint8_t bytes; // 1 or 4
        std::uint32_t retail;
        std::uint32_t replacement;
        Group group;
    };

    [[nodiscard]] bool group_active(Group group) const noexcept;

    static const std::array<Override, 5U> kOverrides;

    Settings settings_{};
    // Overrides currently written into guest memory.
    std::array<bool, 5U> applied_{};
    bool any_applied_{false};
    Statistics statistics_{};
};

} // namespace takedown::runtime
