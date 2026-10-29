#include "takedown/engine/track/fade_effects.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <cstdint>

namespace takedown::engine {
namespace {

// The game's arithmetic: operands read as the processor reads them (no
// denormals) and results chopped toward zero.
float mul(const float a, const float b) noexcept { return chop_mul(game_operand(a), game_operand(b)); }
float sub(const float a, const float b) noexcept { return chop_sub(game_operand(a), game_operand(b)); }
float from_byte(const std::uint8_t value) noexcept { return chop_from_int(static_cast<std::int32_t>(value)); }

// 255 times `value` as the game truncates it into a byte, including its way
// of converting what exceeds the signed range.
std::uint8_t level_byte(const float value) noexcept {
    const float scaled = mul(255.0F, value);
    if (game_less_equal(2147483648.0F, scaled)) {
        const auto word = static_cast<std::uint32_t>(truncate_word(sub(scaled, 2147483648.0F)));
        return static_cast<std::uint8_t>(word | 0x80000000U);
    }
    return static_cast<std::uint8_t>(truncate_word(scaled));
}

// `value` limited to 0..1.
float unit_limited(const float value) noexcept {
    if (game_less(value, 0.0F)) {
        return 0.0F;
    }
    return game_less_equal(value, 1.0F) ? value : 1.0F;
}

constexpr std::uint8_t kFadingAllowed = 0x02U;
constexpr std::uint8_t kFlagScaleDirect = 0x10U;
constexpr std::uint8_t kFlagFading = 0x02U;
constexpr std::uint8_t kFlagFadingOut = 0x04U;
constexpr std::uint8_t kFlagHeld = 0x08U;
constexpr std::uint8_t kFlagGated = 0x40U;
constexpr std::uint8_t kFlagGateOpen = 0x80U;

float scaled_by(const std::uint8_t flags, const float value, const float kind_value, const float shared) noexcept {
    return (flags & kFlagScaleDirect) != 0U ? mul(value, kind_value) : mul(kind_value, mul(value, shared));
}

// One step of the fade-in / fade-out ramp of the alpha: the alpha times
// `fraction`, except below the context's cutoff, where it drops to zero.
void ramp_alpha(FadeRecord& record, FadeTarget& target, const FadeContext& context, const float fraction) noexcept {
    const float ratio = game_div(from_byte(target.alpha), 255.0F);
    float result = mul(ratio, fraction);
    if (record.cutoff < context.level_floor) {
        result = 0.0F;
    }
    target.alpha = level_byte(result);
}

} // namespace

bool fade_record_running(FadeRecord& record, const FadeTarget& target) noexcept {
    switch (record.state) {
    case 2:
        return (record.flags & kFlagHeld) != 0U;
    case 0:
    case 1:
    case 3:
        if ((record.flags & kFlagGated) == 0U) {
            return (target.options & kFadingAllowed) != 0U;
        }
        if ((record.flags & kFlagGateOpen) != 0U && (target.options & kFadingAllowed) == 0U && record.state == 0U) {
            record.latch = 1U;
        }
        return (record.flags & kFlagGateOpen) != 0U;
    default:
        return false;
    }
}

FadeStatus advance_fade_record(FadeRecord& record, FadeTarget& target, const FadeContext& context) noexcept {
    if (record.state == 1U) {
        if (record.step >= 5U) {
            if (target.has_removal) {
                return FadeStatus::NeedsOriginal;
            }
            record.step = 0U;
            record.state = 2U;
            return FadeStatus::Done;
        }
        const bool go = (record.flags & kFlagGated) == 0U ? (target.options & kFadingAllowed) != 0U
                                                          : (record.flags & kFlagGateOpen) != 0U;
        if (!go) {
            record.step = 0U;
            record.state = 2U;
            record.flags = static_cast<std::uint8_t>(record.flags & 0xf7U);
            return FadeStatus::Done;
        }
        ramp_alpha(record, target, context, sub(1.0F, game_div(from_byte(record.step), 5.0F)));
        if (target.has_handle) {
            return FadeStatus::NeedsOriginal;
        }
        record.step = static_cast<std::uint8_t>(record.step + 1U);
        return FadeStatus::Done;
    }
    if (record.state == 3U) {
        if ((record.flags & kFlagHeld) == 0U) {
            record.state = 0U;
            return FadeStatus::Done;
        }
        if (record.step == 0U) {
            return FadeStatus::NeedsOriginal;
        }
        if (record.step >= 5U) {
            record.state = 0U;
            record.step = 0U;
            return FadeStatus::Done;
        }
        const bool go = (record.flags & kFlagGated) == 0U ? (target.options & kFadingAllowed) != 0U
                                                          : (record.flags & kFlagGateOpen) != 0U;
        if (!go) {
            record.state = 0U;
            record.step = 0U;
            return FadeStatus::Done;
        }
        ramp_alpha(record, target, context, game_div(from_byte(record.step), 5.0F));
        if (target.has_handle) {
            return FadeStatus::NeedsOriginal;
        }
        record.step = static_cast<std::uint8_t>(record.step + 1U);
        return FadeStatus::Done;
    }
    return FadeStatus::Done;
}

FadeStatus update_fade_record(FadeRecord& record, FadeTarget& target, const FadeContext& context) noexcept {
    record.flags = static_cast<std::uint8_t>(record.flags & 0xdfU);
    if (record.state == 0U) {
        target.scale = scaled_by(record.flags, record.strength, record.kind_scale, context.scale_a);
        if (target.has_handle) {
            return FadeStatus::NeedsOriginal;
        }
        const float level = record.mode != 0U ? mul(record.level, record.kind_level)
                                              : mul(record.kind_level, mul(record.level, context.scale_b));
        target.alpha = level_byte(unit_limited(level));
        if (target.has_handle) {
            return FadeStatus::NeedsOriginal;
        }
        if ((record.flags & kFlagFading) != 0U) {
            record.countdown = sub(record.countdown, context.dt);
            if (game_less(record.countdown, 0.0F)) {
                return FadeStatus::NeedsOriginal;
            }
        } else if ((record.flags & kFlagFadingOut) != 0U) {
            if (game_less_equal(record.remaining, 0.0F)) {
                return FadeStatus::NeedsOriginal;
            }
            if ((target.options & kFadingAllowed) == 0U) {
                record.flags = static_cast<std::uint8_t>(record.flags & 0xfbU);
            } else {
                const float fraction = game_div(from_byte(target.alpha), 255.0F);
                const float progress = game_div(record.remaining, record.span);
                target.alpha = level_byte(mul(fraction, progress));
                if (target.has_handle) {
                    return FadeStatus::NeedsOriginal;
                }
                record.remaining = sub(record.remaining, context.dt);
            }
        }
    }
    if (!fade_record_running(record, target) && (record.flags & kFlagGateOpen) != 0U) {
        record.flags = static_cast<std::uint8_t>(record.flags & 0x7fU);
    }
    return advance_fade_record(record, target, context);
}

FadeStatus update_fade_drive(const FadeDrive& drive, const FadeContext& context, FadeDriveOutput& output) noexcept {
    output.scale = scaled_by(drive.flags, drive.strength, drive.kind_scale, context.scale_a);
    const float level = drive.mode != 0U ? mul(drive.level, drive.kind_level)
                                         : mul(drive.kind_level, mul(drive.level, context.scale_b));
    output.blend = unit_limited(level);
    return FadeStatus::Done;
}

bool fade_drive_running(const FadeDrive& drive) noexcept {
    switch (drive.state) {
    case 2:
        return (drive.flags & kFlagHeld) != 0U;
    case 0:
    case 1:
    case 3:
        return drive.chain_value != 3U && drive.chain_value != 1U;
    default:
        return false;
    }
}

FadeDriveAdvance advance_fade_drive(FadeDrive& drive, const float blend) noexcept {
    FadeDriveAdvance result;
    result.blend = blend;
    const auto chain_ended = [&] { return drive.chain_value == 3U || drive.chain_value == 1U; };
    if (drive.state == 1U) {
        if (drive.step >= 5U) {
            drive.flags = static_cast<std::uint8_t>(drive.flags | kFlagHeld);
            result.events = kFadeDriveTakeHolder | kFadeDriveChainOne;
            drive.step = 0U;
            drive.state = 2U;
        } else if (chain_ended()) {
            drive.step = 0U;
            result.events = kFadeDriveChainOne;
            drive.state = 2U;
            drive.flags = static_cast<std::uint8_t>(drive.flags & 0xf7U);
        } else {
            result.blend = mul(blend, sub(1.0F, game_div(from_byte(drive.step), 5.0F)));
            result.events = kFadeDriveBlend;
            drive.step = static_cast<std::uint8_t>(drive.step + 1U);
        }
    } else if (drive.state == 3U) {
        if ((drive.flags & kFlagHeld) == 0U) {
            drive.state = 0U;
            return result;
        }
        if (drive.step == 0U) {
            result.events = kFadeDriveChainTwo | kFadeDriveBits80 | kFadeDriveGiveHolder;
            drive.chain_value = 2U;
        }
        if (drive.step >= 5U || chain_ended()) {
            drive.state = 0U;
            drive.step = 0U;
        } else {
            result.blend = mul(blend, game_div(from_byte(drive.step), 5.0F));
            result.events |= kFadeDriveBlend;
            drive.step = static_cast<std::uint8_t>(drive.step + 1U);
        }
    }
    return result;
}

FadeEaseOutput ease_fade_slot(const FadeEase& slot, const float dt) noexcept {
    FadeEaseOutput output;
    if (game_less(slot.countdown, 0.0F) || game_less_equal(slot.duration, 0.0F)) {
        return output;
    }
    const float remaining = sub(1.0F, game_div(slot.countdown, slot.duration));
    const float from_a = chop_add(0.0F, game_operand(slot.from_a));
    const float from_b = chop_add(0.0F, game_operand(slot.from_b));
    output.a = chop_add(from_a, mul(remaining, sub(slot.to_a, slot.from_a)));
    output.b = chop_add(from_b, mul(remaining, sub(slot.to_b, slot.from_b)));
    output.countdown = sub(slot.countdown, dt);
    output.eased = true;
    return output;
}

} // namespace takedown::engine
