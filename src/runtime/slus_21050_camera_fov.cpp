#include "takedown/runtime/slus_21050_camera_fov.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/presentation/graphics_options.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace takedown::runtime {
namespace {

constexpr std::size_t kF1 = 1U;
constexpr std::size_t kF20 = 20U;
// s3 holds the camera's RenderWare frame at the hook.
constexpr std::size_t kFrameRegister = 19U;
constexpr float kPi = 3.14159265358979323846F;

struct CodeWord {
    guest::GuestAddress address;
    std::uint32_t value;
};

constexpr std::array<CodeWord, 4> kCode{{
    {0x00212280U, 0xC7808080U}, // lwc1 f0, -32640(gp)
    {0x00212284U, 0xC4740004U}, // lwc1 f20, 4(v1)
    {0x00212288U, 0x46140842U}, // mul.s f1, f1, f20
    {0x0021228CU, 0x0C047888U}, // jal 0x0011e220
}};

float tangent_of_half(const float degrees) noexcept {
    return std::tan(degrees * kPi / 360.0F);
}

Slus21050CameraFov* g_native_instance = nullptr;

} // namespace

std::span<const guest::GuestAddress> Slus21050CameraFov::hook_addresses() noexcept {
    static constexpr std::array<guest::GuestAddress, 2> kAddresses{kFovUse, kUiSubmissionBegin};
    return kAddresses;
}

std::optional<std::string> Slus21050CameraFov::signature_mismatch(const guest::EeMemory& memory) {
    for (const CodeWord& word : kCode) {
        if (!memory.contains(word.address, 4U) ||
            memory.read_u32_le(word.address) != word.value) {
            std::ostringstream out;
            out << "camera FOV code at 0x" << std::hex << std::setw(8) << std::setfill('0')
                << word.address << " is not the SLUS-21050 instruction";
            return out.str();
        }
    }
    return std::nullopt;
}

void Slus21050CameraFov::configure(const float target_degrees, const bool correction,
                                   const bool panini, const float aspect) noexcept {
    g_native_instance = this;
    correction_ = correction;
    panini_ = panini;
    aspect_ = aspect > 0.0F ? aspect : 4.0F / 3.0F;
    // TAKEDOWN_CAMERA_DOLLY_GAP overrides the minimum gap (for calibration).
    minimum_gap_ = kMinimumGap;
    if (const char* gap = std::getenv("TAKEDOWN_CAMERA_DOLLY_GAP"); gap != nullptr) {
        minimum_gap_ = std::strtof(gap, nullptr);
    }
    target_ = target_degrees;
    active_ = target_degrees > 0.0F && std::abs(target_degrees - kBaseFov) > 0.01F;
    tangent_scale_ =
        active_ ? tangent_of_half(std::clamp(target_degrees, 1.0F, 170.0F)) /
                      tangent_of_half(kBaseFov)
                : 1.0F;
}

float Slus21050CameraFov::map(const float game_degrees) const noexcept {
    if (!active_ || !(game_degrees > 0.0F && game_degrees < 179.0F)) {
        return game_degrees;
    }
    const float half = std::atan(tangent_of_half(game_degrees) * tangent_scale_);
    return std::clamp(half * 360.0F / kPi, 1.0F, 170.0F);
}

// Moves the camera along its view direction so the player's car keeps its
// size and place on screen: with the FOV tangent scaled by k, a point at
// depth D looks the same from depth D / k. D is measured every frame, from
// the camera to the car's rear (the active player's vehicle at 0x0064D388,
// its translation at +0x30, less kCarRearOffset), so the move follows the
// chase camera closing in or falling back. The camera only moves while it is
// chasing the car (the car ahead, near the view axis): crash, replay and
// other cameras are left alone. A forward move always leaves `minimum_gap_`
// between the camera and the car's rear, so a wide FOV cannot put the camera
// inside the car; past that the car shows smaller. The frame is the camera's
// RenderWare frame (s3, rebuilt by the game this frame from its own camera
// state, so nothing accumulates): `at` at +48, position at +64.
std::optional<float> Slus21050CameraFov::chase_distance(const guest::GuestAddress frame,
                                                        const guest::EeMemory& memory) const {
    const std::uint32_t car = memory.contains(kActivePlayerVehicle, 4U)
                                  ? memory.read_u32_le(kActivePlayerVehicle)
                                  : 0U;
    if (!memory.contains(frame + 48U, 28U) || car == 0U || !memory.contains(car + 48U, 12U)) {
        return std::nullopt;
    }
    const auto read = [&memory](const guest::GuestAddress at) {
        return std::bit_cast<float>(memory.read_u32_le(at));
    };
    std::array<float, 3> at{};
    std::array<float, 3> offset{};
    float along = 0.0F;
    float distance_squared = 0.0F;
    for (std::uint32_t axis = 0U; axis < 3U; ++axis) {
        at[axis] = read(frame + 48U + 4U * axis);
        offset[axis] = read(car + 48U + 4U * axis) - read(frame + 64U + 4U * axis);
        along += offset[axis] * at[axis];
        distance_squared += offset[axis] * offset[axis];
    }
    const float lateral_squared = distance_squared - along * along;
    if (!std::isfinite(distance_squared) || along < kChaseMinimumAlong || along > kChaseMaximumAlong ||
        lateral_squared > kChaseMaximumLateral * kChaseMaximumLateral) {
        return std::nullopt;
    }
    return along;
}

void Slus21050CameraFov::dolly(const guest::GuestAddress frame, guest::EeMemory& memory,
                               const float game_degrees, const float fov_degrees,
                               const float along) const {
    // The car's size on screen goes with the picture's half width in the
    // middle: the tangent, or with Panini the Panini half width.
    const float tangent = tangent_of_half(fov_degrees);
    const float shown =
        panini_ ? presentation::panini_half_width(tangent, presentation::panini_strength(game_degrees),
                                                  aspect_)
                : tangent;
    const float k = shown / tangent_of_half(game_degrees);
    if (!(k > 0.0F) || !correction_) {
        return;
    }
    const float rear = along - kCarRearOffset;
    float shift = rear * (1.0F - 1.0F / k);
    if (shift > 0.0F) {
        shift = std::min(shift, std::max(0.0F, rear - minimum_gap_));
    }
    for (std::uint32_t axis = 0U; axis < 3U; ++axis) {
        const float at = std::bit_cast<float>(memory.read_u32_le(frame + 48U + 4U * axis));
        const guest::GuestAddress position = frame + 64U + 4U * axis;
        const float value = std::bit_cast<float>(memory.read_u32_le(position)) + at * shift;
        memory.write_u32_le(position, std::bit_cast<std::uint32_t>(value));
    }
}

Slus21050CameraFov::HookResult Slus21050CameraFov::on_dispatch(guest::R5900State& state,
                                                              guest::EeMemory& memory) {
    if (state.pc() != kFovUse || state.is_executing_delay_slot()) {
        return HookResult::NotHooked;
    }
    if (!code_matches_.has_value()) {
        code_matches_ = !signature_mismatch(memory).has_value();
    }
    if (!*code_matches_) {
        return HookResult::NotHooked;
    }
    const float game = std::bit_cast<float>(state.read_fpr_bits(kF20));
    const float fov = apply(memory, state.read_gpr_u32(kFrameRegister), game);
    state.write_fpr_bits(kF20, std::bit_cast<std::uint32_t>(fov));
    // mul.s f1, f1, f20
    const float half = std::bit_cast<float>(state.read_fpr_bits(kF1)) * fov;
    state.write_fpr_bits(kF1, std::bit_cast<std::uint32_t>(half));
    state.set_pc(kFovUse + 4U);
    return HookResult::Replaced;
}

float Slus21050CameraFov::apply(guest::EeMemory& memory, const guest::GuestAddress frame,
                                const float game) {
    if (statistics_.calls == 0U) {
        statistics_.minimum_fov = game;
        statistics_.maximum_fov = game;
    }
    ++statistics_.calls;
    statistics_.minimum_fov = std::min(statistics_.minimum_fov, game);
    statistics_.maximum_fov = std::max(statistics_.maximum_fov, game);
    statistics_.last_fov = game;
    if (game >= 0.0F && game < 180.0F) {
        ++statistics_.histogram[static_cast<std::size_t>(game)];
    }
    const float fov = map(game);
    const std::optional<float> along = chase_distance(frame, memory);
    if ((fov != game || panini_) && along.has_value()) {
        dolly(frame, memory, game, fov, *along);
    }
    ++view_.serial;
    view_.chasing = along.has_value();
    view_.game_fov = game;
    view_.fov = fov;
    return fov;
}

float Slus21050CameraFov::on_native_camera(guest::EeMemory& memory, const guest::GuestAddress frame,
                                           const float game_degrees) {
    if (!code_matches_.has_value()) {
        code_matches_ = !signature_mismatch(memory).has_value();
    }
    if (!*code_matches_) {
        return game_degrees;
    }
    return apply(memory, frame, game_degrees);
}

Slus21050CameraFov* Slus21050CameraFov::native_instance() noexcept {
    return g_native_instance;
}

} // namespace takedown::runtime
