#pragma once

// How a sound at a point in the world is heard from the listener (the
// camera): its volume falloff, Doppler pitch factor, angle and left/right
// placement. This is the game's 3D sound positioning routine
// (SLUS-21050 0x00298420), with the game's table square root for distances.

#include "takedown/engine/audio/voice_pan.hpp"
#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/game_library.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cmath>
#include <cstdint>

namespace takedown::engine {

// The listener fields the routine reads (offsets in the camera record).
struct SoundListener {
    Vector3 position;       // +64
    Vector3 velocity;       // +76
    Vector3 side_axis;      // +88: the sign of the sound's angle comes from it
    Vector3 pan_axis;       // +100: angle and spread are measured against it
    Vector3 forward_axis;   // +112: the sound's placement across the picture
    float falloff_slope{};  // +124: how fast volume falls beyond the near distance
    float speed_scale{};    // +128: scales the closing speed
    float unit_scale{};     // +132: scales the speed of sound (340)
};

// The sound source fields it reads.
struct SoundEmitter {
    Vector3 position;       // +0
    Vector3 velocity;       // +12
    float near_distance{};  // +24: full volume inside it
    float far_distance{};   // +28: silent beyond it
};

// What to compute (the routine's flags argument).
inline constexpr std::uint32_t kSoundVolume = 1U;
inline constexpr std::uint32_t kSoundPitch = 2U;
inline constexpr std::uint32_t kSoundPlacement = 4U;

// How the placement's pan is shaped (the routine's mode argument, from the
// sound output setting): 2 centres it, 3 halves its spread, others keep it.
enum class PanShape : std::uint8_t { Keep, Halve, Centre };

// The five floats it writes; a result stays unset when its flag was not
// requested (the original leaves that word alone).
struct SoundParameters {
    float volume{};        // [0]: 1 inside the near distance, 0 beyond the far one
    float pitch_factor{};  // [1]: Doppler factor
    float angle{};         // [2]: angle between the direction to the sound and the pan axis
    float pan{};           // [3]: 0 to 1 across the picture
    float spread{};        // [4]: 0 to 1 against the pan axis
};

namespace sound_detail {

inline constexpr float kHalf = 0.5F;
inline constexpr float kSpeedOfSound = 340.0F;
inline constexpr float kSpeedLimit = 150.0F;

[[nodiscard]] inline float length_squared_zyx(const Vector3 v) noexcept {
    return chop_add(chop_add(chop_mul(v.y, v.y), chop_mul(v.x, v.x)), chop_mul(v.z, v.z));
}

// The game's normalise: scale by 1 / length. Fails (the original reports an
// error) when the length is not positive.
[[nodiscard]] inline bool normalise(Vector3& v) noexcept {
    const float length = game_sqrt(chop_dot(v, v));
    if (!game_less(0.0F, length)) {
        return false;
    }
    const float scale = game_div(1.0F, length);
    v = chop_mul(v, scale);
    return true;
}

[[nodiscard]] inline float unit_interval(const float value) noexcept {
    if (game_less(value, 0.0F)) {
        return 0.0F;
    }
    return game_less_equal(value, 1.0F) ? value : 1.0F;
}

// The direction the placement is measured along: the direction to the sound
// with the pan axis' y, or the pan axis itself when that has no direction.
[[nodiscard]] inline bool placement_direction(const Vector3 offset, const SoundListener& listener,
                                              Vector3& direction) noexcept {
    direction = {offset.x, listener.pan_axis.y, offset.z};
    if (game_equal(direction.x, 0.0F) && game_equal(direction.y, 0.0F) && game_equal(direction.z, 0.0F)) {
        direction = listener.pan_axis;
        return true;
    }
    return normalise(direction);
}

} // namespace sound_detail

// Whether the original takes a path this code leaves to it: a direction to
// normalise whose length is not positive (the game reports an error there).
[[nodiscard]] inline bool sound_needs_original(const SoundListener& listener, const SoundEmitter& emitter,
                                               const std::uint32_t flags) noexcept {
    const Vector3 offset = chop_sub(emitter.position, listener.position);
    if ((flags & kSoundPitch) != 0U && sound_detail::length_squared_zyx(offset) != 0.0F) {
        Vector3 direction = offset;
        if (!sound_detail::normalise(direction)) {
            return true;
        }
    }
    if ((flags & kSoundPlacement) != 0U) {
        Vector3 direction;
        if (!sound_detail::placement_direction(offset, listener, direction)) {
            return true;
        }
    }
    return false;
}

// `table(index)` returns entry `index` of the game's table of square roots;
// `full_turn` is the constant a negative orientation's angle is taken from.
// The caller has checked sound_needs_original.
template <class Table>
[[nodiscard]] SoundParameters position_sound(const SoundListener& listener, const SoundEmitter& emitter,
                                             const std::uint32_t flags, const PanShape shape,
                                             const float full_turn, Table&& table) {
    using namespace sound_detail;
    SoundParameters result;
    const Vector3 offset = chop_sub(emitter.position, listener.position);
    float distance = table_square_root(length_squared_zyx(offset), table);

    if ((flags & kSoundPitch) != 0U) {
        if (game_equal(distance, 0.0F)) {
            result.pitch_factor = 1.0F;
        } else {
            Vector3 direction = offset;
            (void)normalise(direction);
            const float closing = chop_add(
                chop_add(chop_mul(chop_sub(emitter.velocity.y, listener.velocity.y), direction.y),
                         chop_mul(chop_sub(emitter.velocity.x, listener.velocity.x), direction.x)),
                chop_mul(chop_sub(emitter.velocity.z, listener.velocity.z), direction.z));
            float speed = chop_mul(listener.speed_scale, closing);
            if (!game_less_equal(speed, kSpeedLimit)) {
                speed = kSpeedLimit;
            } else if (game_less(speed, -kSpeedLimit)) {
                speed = -kSpeedLimit;
            }
            const float scaled = chop_mul(kSpeedOfSound, listener.unit_scale);
            result.pitch_factor = game_div(scaled, chop_add(scaled, speed));
        }
    }

    if ((flags & kSoundVolume) != 0U) {
        if (!game_less_equal(distance, emitter.far_distance)) {
            result.volume = 0.0F;
        } else if (game_less(distance, emitter.near_distance)) {
            result.volume = 1.0F;
        } else {
            distance = chop_sub(distance, emitter.near_distance);
            const float range = chop_sub(emitter.far_distance, emitter.near_distance);
            result.volume = game_div(chop_sub(range, distance),
                                     chop_add(range, chop_mul(distance, listener.falloff_slope)));
        }
    }

    if ((flags & kSoundPlacement) != 0U) {
        Vector3 u;
        (void)placement_direction(offset, listener, u);
        const Vector3 f = listener.forward_axis;
        const Vector3 a = listener.pan_axis;
        const auto dot_yxz = [](const Vector3 p, const Vector3 q) {
            return chop_add(chop_add(chop_mul(p.y, q.y), chop_mul(p.x, q.x)), chop_mul(p.z, q.z));
        };
        const float along_forward = dot_yxz(u, f);
        result.pan = unit_interval(chop_sub(kHalf, chop_mul(kHalf, along_forward)));
        const float along_axis = dot_yxz(u, a);
        result.spread = unit_interval(chop_sub(kHalf, chop_mul(kHalf, along_axis)));
        float cosine = along_axis;
        if (!game_less_equal(cosine, 1.0F)) {
            cosine = 1.0F;
        }
        if (game_less(cosine, -1.0F)) {
            cosine = -1.0F;
        }
        // The cosine is within -1 to 1 here, so no game_acos range handling is needed
        // (a denormal cosine acts as zero, whose arc cosine rounds to the same float).
        result.angle = game_float_result(std::acos(static_cast<double>(cosine)));
        const Vector3 around{chop_sub(chop_mul(u.y, a.z), chop_mul(u.z, a.y)),
                             chop_sub(chop_mul(u.z, a.x), chop_mul(u.x, a.z)),
                             chop_sub(chop_mul(u.x, a.y), chop_mul(u.y, a.x))};
        const float side = dot_yxz(around, listener.side_axis);
        if (!game_less_equal(side, 0.0F)) {
            result.angle = chop_sub(full_turn, result.angle);
        }
        if (shape == PanShape::Halve) {
            result.pan = chop_add(chop_mul(chop_sub(result.pan, kHalf), kHalf), kHalf);
        } else if (shape == PanShape::Centre) {
            result.pan = chop_add(chop_mul(chop_sub(result.pan, kHalf), 0.0F), kHalf);
        }
    }
    return result;
}

} // namespace takedown::engine
