#pragma once

// Field of view for SLUS-21050's cameras (docs/USAGE.md, "Field of view").
//
// The per-frame camera update 0x00211ED0 reads the current field of view in
// degrees from the camera mode (it moves with speed and boost), turns it into
// tan(fov / 2) and sets the RenderWare view window from it:
//
//   0x00212280  lwc1  f0, -32640(gp)    degrees to radians
//   0x00212284  lwc1  f20, 4(v1)        f20 = current FOV in degrees
//   0x00212288  mul.s f1, f1, f20       f1 = 0.5 * FOV
//   0x0021228C  jal   0x0011E220        tan
//
// The hook stands in for the instruction at 0x00212288. It widens or narrows
// the FOV by a constant factor on its tangent, so the game's own changes
// (boost zoom, crash cameras) keep their shape:
//
//   tan(new / 2) = tan(fov / 2) * tan(target / 2) / tan(base / 2)
//
// where `base` is the game's FOV at full boost (90) and `target` the
// player's choice. With target == base the hook does nothing (the game's own value
// is used unchanged). The resulting FOV is stored by the game itself (+108 of
// the camera), exactly as if the camera mode had asked for it.

#include "takedown/guest/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime {

class Slus21050CameraFov {
  public:
    // The game's own horizontal FOV at full boost (degrees): measured over a
    // race replay, the camera uses 45 on the intro, 60 on the start line and
    // widens to 90 as it boosts. The player's setting is the FOV at full
    // boost; every other camera state scales with it, so the boost widening
    // keeps its shape.
    static constexpr float kBaseFov = 90.0F;
    static constexpr guest::GuestAddress kFovUse = 0x00212288U;
    // BeginUiPacketSubmission (docs/archive/features/UI_DRAW_SUBMISSION_NATIVE.md): the game
    // starts its UI packets here once per frame, before the frame's GS data
    // is sent; the Panini warp of that frame is armed in the renderer's
    // stream at this point.
    static constexpr guest::GuestAddress kUiSubmissionBegin = 0x0021A0F0U;
    // The active player's vehicle (transform at +0, translation at +0x30).
    static constexpr guest::GuestAddress kActivePlayerVehicle = 0x0064D388U;
    // From the car's origin back to its rear along the chase camera's view
    // (game units): the chase camera sits 5.15 behind the origin, and the
    // car's width on screen matches the game's own view at FOV 120 when the
    // rear is taken 3.4 in front of the camera.
    static constexpr float kCarRearOffset = 1.75F;
    // The closest a forward move brings the camera to the car's rear (game
    // units). The full move at 140 degrees would leave 1.2, inside the car as
    // soon as the chase camera closes in; past this gap the car shows smaller.
    static constexpr float kMinimumGap = 1.6F;
    // The camera counts as chasing the car while the car's origin is this far
    // ahead along the view and this close to the view axis (game units).
    static constexpr float kChaseMinimumAlong = 2.5F;
    static constexpr float kChaseMaximumAlong = 12.0F;
    static constexpr float kChaseMaximumLateral = 3.0F;

    enum class HookResult : std::uint8_t { NotHooked, Replaced };

    struct Statistics {
        std::uint64_t calls{0U};
        float minimum_fov{0.0F};
        float maximum_fov{0.0F};
        float last_fov{0.0F};
        // Calls per whole degree of the game's FOV (0..179).
        std::array<std::uint32_t, 180> histogram{};
    };

    [[nodiscard]] static std::span<const guest::GuestAddress> hook_addresses() noexcept;
    // Describes the first code word that differs from SLUS-21050; nullopt
    // when the hook site is the expected code.
    [[nodiscard]] static std::optional<std::string>
    signature_mismatch(const guest::EeMemory& memory);

    // `target_degrees` is the FOV the player wants at full boost; 0 or
    // kBaseFov leaves the game alone. `correction` moves the camera so the
    // car keeps its size on screen; with `panini` that size is the one the
    // renderer's Panini projection shows (its middle is magnified) for a
    // picture `aspect` wide over high.
    void configure(float target_degrees, bool correction, bool panini, float aspect) noexcept;
    [[nodiscard]] float aspect() const noexcept { return aspect_; }
    [[nodiscard]] float target() const noexcept { return target_; }
    [[nodiscard]] bool panini() const noexcept { return panini_; }

    // Maps one FOV the game asked for to the one it gets.
    [[nodiscard]] float map(float game_degrees) const noexcept;

    HookResult on_dispatch(guest::R5900State& state, guest::EeMemory& memory);

    // The native UpdateCameraFromOrientation (runtime/game/render.cpp) runs
    // the whole camera update, so the guest instruction at kFovUse never
    // executes. It calls this at the same point instead, with the camera's
    // RenderWare frame (the hook's s3) and the game's FOV, and uses the FOV
    // returned; the camera is moved exactly as on_dispatch moves it.
    [[nodiscard]] float on_native_camera(guest::EeMemory& memory, guest::GuestAddress frame,
                                         float game_degrees);
    // The instance configure() was last called on (the one main's PC
    // options drive), or null before that.
    [[nodiscard]] static Slus21050CameraFov* native_instance() noexcept;

    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }

    // The last camera the game built: whether it was the chase camera on the
    // player's car, the FOV the game asked for and the one it got; `serial`
    // counts camera updates.
    struct View {
        std::uint64_t serial{0U};
        bool chasing{false};
        float game_fov{0.0F};
        float fov{0.0F};
    };
    [[nodiscard]] const View& view() const noexcept { return view_; }

  private:
    // The car's distance ahead along the view while the camera is chasing it.
    [[nodiscard]] std::optional<float> chase_distance(guest::GuestAddress frame,
                                                      const guest::EeMemory& memory) const;
    void dolly(guest::GuestAddress frame, guest::EeMemory& memory, float game_degrees,
               float fov_degrees, float along) const;
    // The FOV mapping, camera move and statistics shared by both paths.
    [[nodiscard]] float apply(guest::EeMemory& memory, guest::GuestAddress frame, float game_degrees);

    bool correction_{true};
    bool panini_{false};
    float aspect_{4.0F / 3.0F};
    float minimum_gap_{kMinimumGap};
    float target_{0.0F};
    float tangent_scale_{1.0F};
    bool active_{false};
    std::optional<bool> code_matches_;
    Statistics statistics_{};
    View view_{};
};

} // namespace takedown::runtime
