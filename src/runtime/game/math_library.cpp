// Adapters for the game's math library: trigonometry and square root, object
// bounds, polygon and segment clipping, curve modifiers, block coverage and
// axis rotations.

#include "takedown/engine/ai/driver_speed.hpp"
#include "takedown/engine/ai/lane_steering.hpp"
#include "takedown/engine/audio/positional_sound.hpp"
#include "takedown/engine/math/axis_rotation.hpp"
#include "takedown/engine/math/block_coverage.hpp"
#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/clipping.hpp"
#include "takedown/engine/math/curve.hpp"
#include "takedown/engine/math/game_library.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/trigonometry.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace takedown::runtime {

namespace {

using guest::GuestAddress;

// sinf's kernel (__kernel_sinf): sin(x + tail) for a reduced argument.
NativeFunctionExecution sine_kernel(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_float(engine::sine_kernel(call.float_arg(0U), call.float_arg(1U), call.arg(0U) != 0U));
    return call.finish(30U);
}

NativeFunctionExecution square_root(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto bits = call.cpu().read_fpr_bits(12U);
    if ((bits & 0x7f800000U) == 0U) {
        call.cpu().write_fpr_bits(0U, bits);
    } else {
        call.return_float_value(std::sqrt(call.float_arg_value(0U)));
    }
    return call.finish(150U);
}

// The object's local box (at +8) moved by its transform (at +4); the world
// box goes to +16 (minimum) and +32 (maximum).
NativeFunctionExecution transform_object_bounds(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress object = call.arg(1U);
    const GuestAddress transform_address = memory.read_u32_le(object + 4U);
    const GuestAddress local_address = memory.read_u32_le(object + 8U);
    engine::Transform transform;
    for (int row = 0; row < 3; ++row) {
        transform.basis[row] = load_vector4(memory, transform_address + static_cast<GuestAddress>(row * 16)).xyz();
    }
    transform.translation = load_vector4(memory, transform_address + 48U).xyz();
    const engine::Bounds local{load_vector4(memory, local_address).xyz(),
                               load_vector4(memory, local_address + 16U).xyz()};
    const engine::Bounds world = engine::transform_bounds(transform, local);
    store_float(memory, object + 16U, world.minimum.x);
    store_float(memory, object + 20U, world.minimum.y);
    store_float(memory, object + 24U, world.minimum.z);
    store_float(memory, object + 32U, world.maximum.x);
    store_float(memory, object + 36U, world.maximum.y);
    store_float(memory, object + 40U, world.maximum.z);
    return call.finish(76U);
}

// Clips the polygon of a2 vertices at a1 to the slab f12..f13 on axis a3,
// writing the result to a0 and returning its vertex count.
NativeFunctionExecution clip_polygon_to_slab(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress output = call.arg(0U);
    const GuestAddress input = call.arg(1U);
    const auto count = call.arg_signed(2U);
    const int axis = static_cast<int>(call.arg(3U));
    std::vector<engine::Vector4> vertices;
    for (std::int32_t index = 0; index < count; ++index) {
        vertices.push_back(load_vector4(memory, input + static_cast<GuestAddress>(index * 16)));
    }
    std::vector<engine::Vector4> clipped(vertices.size() * 2U);
    const auto written = engine::clip_polygon_to_slab(vertices, axis, call.float_arg(0U), call.float_arg(1U), clipped);
    for (std::size_t index = 0U; index < written; ++index) {
        store_vector4(memory, output + static_cast<GuestAddress>(index) * 16U, clipped[index]);
    }
    call.return_value(static_cast<std::uint32_t>(written));
    return call.finish(static_cast<std::uint32_t>(std::max(count, 0)) * 30U + 12U);
}


// A curve modifier: reads the curve at the input slot it names and stores the
// result in (or multiplies it into, for output 0) the output slot it names.
void apply_curve_modifier(guest::EeMemory& memory, const GuestAddress modifier, const GuestAddress inputs,
                          const GuestAddress outputs) {
    const GuestAddress points = memory.read_u32_le(modifier);
    const auto point_at = [&points, &memory](const std::int32_t index) {
        const GuestAddress at = points + static_cast<GuestAddress>(index * 8);
        return engine::CurvePoint{load_float(memory, at), load_float(memory, at + 4U)};
    };
    const auto input = static_cast<std::int8_t>(memory.read_u8(modifier + 5U));
    const auto output = static_cast<std::int8_t>(memory.read_u8(modifier + 6U));
    const float value = engine::evaluate_curve(point_at, static_cast<std::int32_t>(memory.read_u8(modifier + 4U)),
                                               load_float(memory, inputs + static_cast<GuestAddress>(input * 4)));
    const GuestAddress target = outputs + static_cast<GuestAddress>(output * 4);
    store_float(memory, target, output == 0 ? value * load_float(memory, target) : value);
}

NativeFunctionExecution apply_curve_modifier(NativeFunctionContext& context) {
    GuestCall call{context};
    apply_curve_modifier(call.memory(), call.arg(0U), call.arg(1U), call.arg(2U));
    return call.finish(60U);
}

// Evaluates every list of modifiers into its own block of 20 slots (reset to
// 1.0 first).
NativeFunctionExecution apply_curve_modifier_set(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress set = call.arg(0U);
    const GuestAddress inputs = call.arg(1U);
    const GuestAddress outputs = call.arg(2U);
    const GuestAddress lists = memory.read_u32_le(set + 60U);
    const std::uint32_t list_count = memory.read_u8(set + 64U);
    std::uint32_t cost = 10U;
    for (std::uint32_t list = 0U; list < list_count; ++list) {
        const GuestAddress block = outputs + list * 80U;
        for (std::uint32_t index = 0U; index < 20U; ++index) {
            store_float(memory, block + index * 4U, 1.0F);
        }
        const GuestAddress entry = lists + list * 16U;
        const GuestAddress modifiers = memory.read_u32_le(entry + 8U);
        const std::uint32_t modifier_count = memory.read_u8(entry + 12U);
        for (std::uint32_t modifier = 0U; modifier < modifier_count; ++modifier) {
            apply_curve_modifier(memory, modifiers + modifier * 8U, inputs, block);
        }
        cost += modifier_count * 70U + 40U;
    }
    return call.finish(cost);
}

// One block-coverage record (see engine/math/block_coverage.hpp): its enable
// flag, unit count at +20, thresholds at +28, and the scale, level and covered
// blocks it keeps at +32, +36 and +40. The range comes from the owner's
// record at +320, and the quarter-to-full factor from a game constant.
// Returns the guest work the original would have retired.
std::uint32_t run_block_coverage(guest::EeMemory& memory, const GuestAddress record, const GuestAddress range_address,
                                 const float input, const float dt) {
    if (memory.read_u32_le(record) == 0U) {
        return 4U;
    }
    const GuestAddress thresholds = memory.read_u32_le(record + 28U);
    const std::uint32_t units = memory.read_u32_le(record + 20U);
    const engine::CoverageRange range{load_float(memory, range_address + 320U),
                                      load_float(memory, range_address + 324U), load_float(memory, 5116796U)};
    const auto result = engine::update_block_coverage(
        units,
        [&memory, &thresholds](const std::uint32_t block) { return load_float(memory, thresholds + block * 4U); },
        input, load_float(memory, record + 36U), range, dt);
    memory.write_u32_le(record + 40U, result.covered_blocks);
    store_float(memory, record + 32U, result.scale);
    store_float(memory, record + 36U, result.level);
    return ((units + 59U) / 60U) * 9U + 40U;
}

NativeFunctionExecution update_block_coverage(NativeFunctionContext& context) {
    GuestCall call{context};
    return call.finish(run_block_coverage(call.memory(), call.arg(0U), call.arg(1U), call.float_arg(0U),
                                          call.float_arg(1U)));
}

// A pair of block-coverage records (at +232 and +276) driven by the owner's
// clock, then the pointed-to scale applied to a vector of the owner's.
NativeFunctionExecution update_coverage_pair(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress owner = call.arg(0U);
    const GuestAddress other = call.arg(1U);
    const float input = load_float(memory, owner + 124U);
    const float dt = call.float_arg(0U) - input;
    std::uint32_t cost = run_block_coverage(memory, owner + 232U, owner, input, dt) + 30U;
    cost += run_block_coverage(memory, owner + 276U, owner, load_float(memory, owner + 124U), dt);
    const GuestAddress source = memory.read_u32_le(owner + 228U);
    const float factor = load_float(memory, source + 72U);
    const GuestAddress scaled = owner + 96U;
    store_float(memory, scaled, factor * load_float(memory, other) + 0.0F);
    store_float(memory, scaled + 4U, load_float(memory, scaled + 4U) + 0.0F);
    store_float(memory, scaled + 8U, factor * load_float(memory, other + 4U) + 0.0F);
    return call.finish(cost);
}

// Compares two floats in memory: -1 when the first is smaller, else 1.
NativeFunctionExecution compare_floats(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const float left = load_float(memory, call.arg(0U));
    const float right = load_float(memory, call.arg(1U));
    call.return_value(left < right ? 0xffffffffU : 1U);
    return call.finish(9U);
}

// Keeps the parts of a2 line segments (pairs of 16-byte points at a1) behind
// the plane at a3, writing the points to a0 and returning how many.
NativeFunctionExecution clip_segments_to_plane(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    GuestAddress output = call.arg(0U);
    const GuestAddress input = call.arg(1U);
    const std::uint32_t count = call.arg(2U);
    const engine::Vector4 plane = load_vector4(memory, call.arg(3U) & ~15U);
    const auto segment = [&input, &memory](const std::size_t index) {
        const GuestAddress first = input + static_cast<GuestAddress>(index) * 32U;
        return std::pair{load_vector4(memory, first), load_vector4(memory, first + 16U)};
    };
    const std::size_t emitted = engine::clip_segments_to_plane(
        (count + 1U) >> 1U, plane, segment, [&memory, &output](const engine::Vector4& point) {
            store_vector4(memory, output, point);
            output += 16U;
        });
    call.return_value(static_cast<std::uint32_t>(emitted));
    return call.finish(((count + 1U) >> 1U) * 22U + static_cast<std::uint32_t>(emitted) * 12U + 10U);
}

// The rotation about the axis at a1 by f12, as three rows at a0 (the
// translation zeroed).
NativeFunctionExecution build_axis_rotation(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress destination = call.arg(0U);
    const engine::RotationRows rotation =
        engine::axis_rotation(load_vector4(memory, call.arg(1U) & ~15U).xyz(), call.float_arg(0U));
    for (GuestAddress row = 0U; row < 3U; ++row) {
        const engine::Vector3 value = rotation.rows[row];
        store_float(memory, destination + row * 16U, value.x);
        store_float(memory, destination + row * 16U + 4U, value.y);
        store_float(memory, destination + row * 16U + 8U, value.z);
    }
    for (GuestAddress lane = 0U; lane < 3U; ++lane) {
        memory.write_u32_le(destination + 48U + lane * 4U, 0U);
    }
    return call.finish(110U);
}

// The C library's float functions in double precision (see
// engine/math/game_library.hpp for the float range they keep).
double sine(const double value) { return std::sin(value); }
double cosine(const double value) { return std::cos(value); }
double tangent(const double value) { return std::tan(value); }
double arc_tangent(const double value) { return std::atan(value); }
double arc_cosine(const double value) { return std::acos(value); }
double arc_sine(const double value) { return std::asin(value); }
double natural_log(const double value) { return std::log(value); }
double arc_tangent2(const double y, const double x) { return std::atan2(y, x); }
double power(const double base, const double exponent) { return std::pow(base, exponent); }

template <double (*Function)(double)>
NativeFunctionExecution unary_function(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_float_value(Function(call.float_arg_value(0U)));
    return call.finish(60U);
}

template <double (*Function)(double, double)>
NativeFunctionExecution binary_function(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_float_value(Function(call.float_arg_value(0U), call.float_arg_value(1U)));
    return call.finish(120U);
}

// The words of each row's fourth lane, which the original leaves as it found
// them.
constexpr std::array kRotationUnusedWords{
    UnusedWord{-1, 12U},
    UnusedWord{-1, 28U},
    UnusedWord{-1, 44U},
    UnusedWord{-1, 60U},
};

// The controller record's input state, as GetInputAxis reads it: the pad at
// +4 and the second device at +8 (each a record of a device's buttons and
// axes), the result cached at +296.
struct InputObjectLayout {
    static constexpr GuestAddress kPad = 4U;
    static constexpr GuestAddress kDevice = 8U;
    static constexpr GuestAddress kLastAxis = 296U;
};
struct PadLayout {
    static constexpr GuestAddress kConnected = 8U;     // u8
    static constexpr GuestAddress kButtons = 10U;      // 28 u8 (0 while held)
    static constexpr GuestAddress kButtonMask = 38U;   // 28 u8 (non-zero: the button counts)
    static constexpr GuestAddress kAxes = 72U;         // 28 floats
};
struct DeviceLayout {
    static constexpr GuestAddress kConnected = 8U;     // u8
    static constexpr GuestAddress kAlternate = 9U;     // u8: which input table maps
    static constexpr GuestAddress kButtons = 10U;
    static constexpr GuestAddress kButtonMask = 45U;
    static constexpr GuestAddress kAxes = 80U;         // floats
};
constexpr std::uint32_t kInputCount = 28U;
constexpr GuestAddress kDeviceInputTableFlagged = 0x00487b60U;    // 28 s32: device input per action when DeviceLayout::kAlternate is set
constexpr GuestAddress kDeviceInputTableDefault = 0x00487bd0U;    // 28 s32
constexpr GuestAddress kInputAxisCached = 0x004f50a0U;            // float stored at +296 when non-zero
constexpr std::uint32_t kDeviceInputLimit = 64U;

// The float unit's operand as a host value: an all-ones exponent is an
// ordinary (huge) number and denormals read as zero.
double fpu_value(const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xffU);
    const double sign = (bits & 0x80000000U) != 0U ? -1.0 : 1.0;
    if (exponent == 0) {
        return sign * 0.0;
    }
    return sign * std::ldexp(1.0 + static_cast<double>(bits & 0x7fffffU) / 8388608.0, exponent - 127);
}

// The device input an action maps to (-1 for none), 0 when no second device
// is connected.
std::int32_t mapped_device_input(const guest::EeMemory& memory, const GuestAddress device, const std::uint32_t action) {
    if (device == 0U || memory.read_u8(device + DeviceLayout::kConnected) == 0U) {
        return -1;
    }
    const GuestAddress table =
        memory.read_u8(device + DeviceLayout::kAlternate) != 0U ? kDeviceInputTableFlagged : kDeviceInputTableDefault;
    return static_cast<std::int32_t>(memory.read_u32_le(table + action * 4U));
}

// Reads that stay inside main RAM; a call whose records lie elsewhere runs
// the original.
std::uint32_t input_axis_cost(const NativeFunctionPrepareContext& context) {
    const auto& cpu = context.cpu;
    const auto& memory = context.memory;
    const GuestAddress object = cpu.read_gpr_u32(4U);
    if (cpu.read_gpr_u64(5U) >= kInputCount || !memory.is_main_ram_range(object, InputObjectLayout::kLastAxis + 4U)) {
        return 0U;
    }
    const GuestAddress pad = memory.read_u32_le(object + InputObjectLayout::kPad);
    if (!memory.is_main_ram_range(pad, PadLayout::kAxes + kInputCount * 4U)) {
        return 0U;
    }
    const GuestAddress device = memory.read_u32_le(object + InputObjectLayout::kDevice);
    if (device == 0U) {
        return 70U;
    }
    if (!memory.is_main_ram_range(device, DeviceLayout::kAxes + kDeviceInputLimit * 4U)) {
        return 0U;
    }
    const std::int32_t input = mapped_device_input(memory, device, static_cast<std::uint32_t>(cpu.read_gpr_u64(5U)));
    return input >= -1 && input < static_cast<std::int32_t>(kDeviceInputLimit) ? 70U : 0U;
}

// GetInputAxis (original 0x0012fc30): how far action a1 is pressed, 0 to 1,
// by the pad (a float axis) plus the second device (a float axis), each
// counting only when its button is not held down if a2 is set; the sum is
// capped at 1 and, when not zero, the global float at 0x004f50a0 is cached at
// +296 of the record a0.
NativeFunctionExecution get_input_axis(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0U);
    const auto action = static_cast<std::uint32_t>(call.arg64(1U));
    const bool buttons_only = call.arg64(2U) != 0U;

    float pad_axis = 0.0F;
    const GuestAddress pad = memory.read_u32_le(object + InputObjectLayout::kPad);
    if (memory.read_u8(pad + PadLayout::kConnected) != 0U) {
        pad_axis = load_float(memory, pad + PadLayout::kAxes + action * 4U);
        if (buttons_only && !(memory.read_u8(pad + PadLayout::kButtonMask + action) != 0U &&
                              memory.read_u8(pad + PadLayout::kButtons + action) == 0U)) {
            pad_axis = 0.0F;
        }
    }

    float device_axis = 0.0F;
    const GuestAddress device = memory.read_u32_le(object + InputObjectLayout::kDevice);
    const std::int32_t input = mapped_device_input(memory, device, action);
    if (input != -1) {
        const auto offset = static_cast<GuestAddress>(input);
        device_axis = load_float(memory, device + offset * 4U + DeviceLayout::kAxes);
        if (buttons_only && !(memory.read_u8(device + offset + DeviceLayout::kButtonMask) != 0U &&
                              memory.read_u8(device + offset + DeviceLayout::kButtons) == 0U)) {
            device_axis = 0.0F;
        }
    }

    float total = engine::chop(fpu_value(pad_axis) + fpu_value(device_axis));
    if (!(total <= 1.0F)) {
        total = 1.0F;
    }
    if (total != 0.0F) {
        memory.write_u32_le(object + InputObjectLayout::kLastAxis, memory.read_u32_le(kInputAxisCached));
    }
    call.return_float(total);
    return call.finish(0U);  // reported from input_axis_cost
}

// PositionSound (original 0x00298420): a sound's volume, pitch factor, angle
// and placement from the listener (the camera) and the sound source.
// a0 listener, a1 source, a2 results (five floats), a3 the pan mode, t0 the
// flags (engine/audio/positional_sound.hpp).
namespace position_sound_layout {
constexpr GuestAddress kListenerPosition = 64U;
constexpr GuestAddress kListenerVelocity = 76U;
constexpr GuestAddress kListenerSide = 88U;
constexpr GuestAddress kListenerPan = 100U;
constexpr GuestAddress kListenerForward = 112U;
constexpr GuestAddress kListenerFalloff = 124U;
constexpr GuestAddress kListenerSpeedScale = 128U;
constexpr GuestAddress kListenerUnitScale = 132U;
constexpr GuestAddress kListenerSize = 136U;
constexpr GuestAddress kEmitterSize = 32U;
constexpr GuestAddress kResultSize = 20U;
constexpr GuestAddress kFullTurnOffset = 0U - 32560U;  // gp-relative
constexpr GuestAddress kPanModeTable = 0x4c0f70U;      // jump table, seven modes
constexpr GuestAddress kPanHalveTarget = 0x29876cU;
constexpr GuestAddress kPanCentreTarget = 0x298790U;
constexpr GuestAddress kSquareRootTable = 2421056U * 2U;
} // namespace position_sound_layout

engine::Vector3 load_vector3(const guest::EeMemory& memory, const GuestAddress address) {
    return {load_float(memory, address), load_float(memory, address + 4U), load_float(memory, address + 8U)};
}

engine::SoundListener load_sound_listener(const guest::EeMemory& memory, const GuestAddress listener) {
    using namespace position_sound_layout;
    return {.position = load_vector3(memory, listener + kListenerPosition),
            .velocity = load_vector3(memory, listener + kListenerVelocity),
            .side_axis = load_vector3(memory, listener + kListenerSide),
            .pan_axis = load_vector3(memory, listener + kListenerPan),
            .forward_axis = load_vector3(memory, listener + kListenerForward),
            .falloff_slope = load_float(memory, listener + kListenerFalloff),
            .speed_scale = load_float(memory, listener + kListenerSpeedScale),
            .unit_scale = load_float(memory, listener + kListenerUnitScale)};
}

engine::SoundEmitter load_sound_emitter(const guest::EeMemory& memory, const GuestAddress emitter) {
    return {.position = load_vector3(memory, emitter),
            .velocity = load_vector3(memory, emitter + 12U),
            .near_distance = load_float(memory, emitter + 24U),
            .far_distance = load_float(memory, emitter + 28U)};
}

std::uint32_t position_sound_cost(const NativeFunctionPrepareContext& context) {
    using namespace position_sound_layout;
    const auto& cpu = context.cpu;
    const auto& memory = context.memory;
    const GuestAddress listener = cpu.read_gpr_u32(4U);
    const GuestAddress emitter = cpu.read_gpr_u32(5U);
    const GuestAddress results = cpu.read_gpr_u32(6U);
    const GuestAddress full_turn = cpu.read_gpr_u32(28U) + kFullTurnOffset;
    if (((listener | emitter | results | full_turn) & 3U) != 0U ||
        !memory.is_main_ram_range(listener, kListenerSize) || !memory.is_main_ram_range(emitter, kEmitterSize) ||
        !memory.is_main_ram_range(results, kResultSize) || !memory.is_main_ram_range(full_turn, 4U)) {
        return 0U;
    }
    const std::uint32_t mode = cpu.read_gpr_u32(7U);
    if (mode < 7U && !memory.is_main_ram_range(kPanModeTable + mode * 4U, 4U)) {
        return 0U;
    }
    const std::uint32_t flags = cpu.read_gpr_u32(8U);
    return engine::sound_needs_original(load_sound_listener(memory, listener), load_sound_emitter(memory, emitter),
                                        flags)
               ? 0U
               : 800U;
}

NativeFunctionExecution position_sound(NativeFunctionContext& context) {
    using namespace position_sound_layout;
    GuestCall call{context};
    guest::EeMemory& memory = call.memory();
    const GuestAddress results = call.arg(2U);
    const std::uint32_t mode = call.arg(3U);
    const std::uint32_t flags = call.arg(4U);
    engine::PanShape shape = engine::PanShape::Keep;
    if (mode < 7U) {
        const GuestAddress target = memory.read_u32_le(kPanModeTable + mode * 4U);
        shape = target == kPanHalveTarget     ? engine::PanShape::Halve
                : target == kPanCentreTarget ? engine::PanShape::Centre
                                             : engine::PanShape::Keep;
    }
    const auto table = [&memory](const std::uint32_t index) {
        return static_cast<std::int16_t>(memory.read_u16_le(kSquareRootTable + index * 2U));
    };
    const engine::SoundParameters sound = engine::position_sound(
        load_sound_listener(memory, call.arg(0U)), load_sound_emitter(memory, call.arg(1U)), flags, shape,
        load_float(memory, call.cpu().read_gpr_u32(28U) + kFullTurnOffset), table);
    if ((flags & engine::kSoundVolume) != 0U) {
        store_float(memory, results, sound.volume);
    }
    if ((flags & engine::kSoundPitch) != 0U) {
        store_float(memory, results + 4U, sound.pitch_factor);
    }
    if ((flags & engine::kSoundPlacement) != 0U) {
        store_float(memory, results + 8U, sound.angle);
        store_float(memory, results + 12U, sound.pan);
        store_float(memory, results + 16U, sound.spread);
    }
    return call.finish(0U);  // reported from position_sound_cost
}

// Guest memory as the AI engine code sees it: reads of main RAM at aligned
// addresses, writes held back in a small log so a call can be run once to
// learn whether the native code covers it (no memory changes) and again to
// apply it. A read it cannot serve, or a state the engine code leaves to the
// original, marks the run unsupported.
class AiMemory {
  public:
    // A run that only reads (writes stay in the log).
    explicit AiMemory(const guest::EeMemory& memory) noexcept : memory_(memory) {}
    // A run whose log commit() applies.
    explicit AiMemory(guest::EeMemory& memory) noexcept : memory_(memory), writable_(&memory) {}

    [[nodiscard]] std::uint8_t u8(const GuestAddress address) { return static_cast<std::uint8_t>(read(address, 1U)); }
    [[nodiscard]] std::uint16_t u16(const GuestAddress address) { return static_cast<std::uint16_t>(read(address, 2U)); }
    [[nodiscard]] std::uint32_t u32(const GuestAddress address) { return read(address, 4U); }
    [[nodiscard]] float f32(const GuestAddress address) { return std::bit_cast<float>(read(address, 4U)); }
    // The x, y and z of a quadword vector (loaded with a 16-byte aligned access).
    [[nodiscard]] engine::Vector3 vec3(const GuestAddress address) {
        if ((address & 15U) != 0U) {
            unsupported();
            return {};
        }
        return {f32(address), f32(address + 4U), f32(address + 8U)};
    }

    void put_u8(const GuestAddress address, const std::uint8_t value) { write(address, 1U, value); }
    void put_u16(const GuestAddress address, const std::uint16_t value) { write(address, 2U, value); }
    void put_u32(const GuestAddress address, const std::uint32_t value) { write(address, 4U, value); }
    void put_f32(const GuestAddress address, const float value) { write(address, 4U, std::bit_cast<std::uint32_t>(value)); }

    void unsupported() noexcept { unsupported_ = true; }
    [[nodiscard]] bool supported() const noexcept { return !unsupported_; }

    // Performs the logged writes.
    void commit() {
        if (writable_ == nullptr) {
            return;
        }
        guest::EeMemory& target = *writable_;
        for (std::size_t i = 0U; i < count_; ++i) {
            const Write& entry = log_[i];
            if (entry.size == 1U) {
                target.write_u8(entry.address, static_cast<std::uint8_t>(entry.value));
            } else if (entry.size == 2U) {
                target.write_u16_le(entry.address, static_cast<std::uint16_t>(entry.value));
            } else {
                target.write_u32_le(entry.address, entry.value);
            }
        }
    }

  private:
    struct Write {
        GuestAddress address{};
        std::uint32_t size{};
        std::uint32_t value{};
    };

    [[nodiscard]] bool usable(const GuestAddress address, const std::uint32_t size) {
        if ((address & (size - 1U)) != 0U || !memory_.is_main_ram_range(address, size)) {
            unsupported_ = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint32_t read(const GuestAddress address, const std::uint32_t size) {
        if (!usable(address, size)) {
            return 0U;
        }
        for (std::size_t i = count_; i-- > 0U;) {
            const Write& entry = log_[i];
            if (entry.address == address && entry.size == size) {
                return entry.value;
            }
            if (entry.address < address + size && address < entry.address + entry.size) {
                unsupported_ = true;  // a read of part of an earlier write
                return 0U;
            }
        }
        switch (size) {
        case 1U:
            return memory_.read_u8(address);
        case 2U:
            return memory_.read_u16_le(address);
        default:
            return memory_.read_u32_le(address);
        }
    }

    void write(const GuestAddress address, const std::uint32_t size, const std::uint32_t value) {
        if (!usable(address, size)) {
            return;
        }
        for (std::size_t i = 0U; i < count_; ++i) {
            if (log_[i].address == address && log_[i].size == size) {
                log_[i].value = value;
                return;
            }
        }
        if (count_ == log_.size()) {
            unsupported_ = true;
            return;
        }
        log_[count_++] = Write{address, size, value};
    }

    const guest::EeMemory& memory_;
    guest::EeMemory* writable_{};
    std::array<Write, 48U> log_{};
    std::size_t count_{};
    bool unsupported_{};
};

// UpdateDriverSpeed (original 0x00161b50): an AI driver's state, target speed
// and speed for this step (engine/ai/driver_speed.hpp). a0 the driver, a1 the
// number of obstacles of the first kind, a2 and a3 non-zero to keep the driver
// from starting to avoid an obstacle.
std::uint32_t driver_speed_cost(const NativeFunctionPrepareContext& context) {
    AiMemory memory{context.memory};
    const auto& cpu = context.cpu;
    const engine::ai::DriverSpeedOutcome outcome = engine::ai::update_driver_speed(
        memory, cpu.read_gpr_u32(4U), static_cast<std::int64_t>(cpu.read_gpr_u64(5U)), cpu.read_gpr_u32(6U),
        cpu.read_gpr_u32(7U), cpu.read_gpr_u32(28U));
    if (outcome.spawns || !memory.supported()) {
        return 0U;
    }
    return std::max<std::uint32_t>(outcome.instructions, 1U);
}

NativeFunctionExecution update_driver_speed(NativeFunctionContext& context) {
    GuestCall call{context};
    AiMemory memory{call.memory()};
    static_cast<void>(engine::ai::update_driver_speed(memory, call.arg(0U), static_cast<std::int64_t>(call.arg64(1U)),
                                                      call.arg(2U), call.arg(3U), call.cpu().read_gpr_u32(28U)));
    memory.commit();
    return call.finish(0U);  // reported from driver_speed_cost
}

// LaneSteeringCorrection (original 0x001605c0): the lateral correction for a
// driver, as a float (engine/ai/lane_steering.hpp).
std::uint32_t lane_steering_cost(const NativeFunctionPrepareContext& context) {
    AiMemory memory{context.memory};
    static_cast<void>(engine::ai::lane_steering_correction(memory, context.cpu.read_gpr_u32(4U),
                                                           context.cpu.read_gpr_u32(28U)));
    return memory.supported() ? 330U : 0U;
}

NativeFunctionExecution lane_steering(NativeFunctionContext& context) {
    GuestCall call{context};
    AiMemory memory{call.memory()};
    const float correction =
        engine::ai::lane_steering_correction(memory, call.arg(0U), call.cpu().read_gpr_u32(28U));
    memory.commit();
    call.return_float(correction);
    return call.finish(0U);  // reported from lane_steering_cost
}

constexpr std::array<EngineFunction, 29U> kFunctions{{
    {
        .address = 1167136U,
        .code_bytes = 260U,
        .code_fnv1a64 = 0x35fc8ae07ae05635ULL,
        .name = "SineKernel",
        .cost = 40U,
        .implementation = sine_kernel,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1788592U,
        .code_bytes = 308U,
        .code_fnv1a64 = 0x54999647e69ef76fULL,
        .name = "TransformObjectBounds",
        .cost = 80U,
        .implementation = transform_object_bounds,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 2879152U,
        .code_bytes = 572U,
        .code_fnv1a64 = 0x4b6820e7f4b48674ULL,
        .name = "ClipPolygonToSlab",
        .cost = 2000U,
        .implementation = clip_polygon_to_slab,
        .returns = EngineReturn::Integer,
    },
    {
        .address = 1171768U,
        .code_bytes = 232U,
        .code_fnv1a64 = 0x59ca10455e13aba9ULL,
        .name = "sinf",
        .cost = 2000U,
        .implementation = unary_function<sine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1171096U,
        .code_bytes = 212U,
        .code_fnv1a64 = 0xd6fec836b80ae05bULL,
        .name = "cosf",
        .cost = 2000U,
        .implementation = unary_function<cosine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172000U,
        .code_bytes = 120U,
        .code_fnv1a64 = 0xc7c32674ffd616daULL,
        .name = "tanf",
        .cost = 2000U,
        .implementation = unary_function<tangent>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1170160U,
        .code_bytes = 652U,
        .code_fnv1a64 = 0x4a2aab3e58ee1a46ULL,
        .name = "atanf",
        .cost = 200U,
        .implementation = unary_function<arc_tangent>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1154080U,
        .code_bytes = 1060U,
        .code_fnv1a64 = 0x3fe2a9f2f39d0aadULL,
        .name = "__ieee754_acosf",
        .cost = 400U,
        .implementation = unary_function<arc_cosine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172264U,
        .code_bytes = 20U,
        .code_fnv1a64 = 0x1c7d4b074cbb1255ULL,
        .name = "acosf",
        .cost = 400U,
        .implementation = unary_function<arc_cosine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1155144U,
        .code_bytes = 928U,
        .code_fnv1a64 = 0x671d520521a8e111ULL,
        .name = "__ieee754_asinf",
        .cost = 400U,
        .implementation = unary_function<arc_sine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172288U,
        .code_bytes = 20U,
        .code_fnv1a64 = 0xe5315c669bb3af36ULL,
        .name = "asinf",
        .cost = 400U,
        .implementation = unary_function<arc_sine>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1156072U,
        .code_bytes = 480U,
        .code_fnv1a64 = 0x7bb3583ed019deecULL,
        .name = "__ieee754_atan2f",
        .cost = 300U,
        .implementation = binary_function<arc_tangent2>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172312U,
        .code_bytes = 20U,
        .code_fnv1a64 = 0x04a13065d81cbd1eULL,
        .name = "atan2f",
        .cost = 300U,
        .implementation = binary_function<arc_tangent2>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1156552U,
        .code_bytes = 736U,
        .code_fnv1a64 = 0x0b7a3f240004020bULL,
        .name = "__ieee754_logf",
        .cost = 300U,
        .implementation = unary_function<natural_log>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172336U,
        .code_bytes = 20U,
        .code_fnv1a64 = 0x4a5ca5adf4e7916dULL,
        .name = "logf",
        .cost = 300U,
        .implementation = unary_function<natural_log>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1157288U,
        .code_bytes = 2016U,
        .code_fnv1a64 = 0xa91866b070629e71ULL,
        .name = "__ieee754_powf",
        .cost = 600U,
        .implementation = binary_function<power>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1172360U,
        .code_bytes = 20U,
        .code_fnv1a64 = 0x56e7ced3a1b10494ULL,
        .name = "powf",
        .cost = 600U,
        .implementation = binary_function<power>,
        .returns = EngineReturn::Float,
    },
    {
        .address = 1160248U,
        .code_bytes = 204U,
        .code_fnv1a64 = 0xe6f25877087756c2ULL,
        .name = "__ieee754_sqrtf",
        .cost = 400U,
        .implementation = square_root,
        .returns = EngineReturn::Float,
    },
    {
        .address = 2444432U,
        .code_bytes = 168U,
        .code_fnv1a64 = 0xbaedff64cbe0578aULL,
        .name = "UpdateCoveragePair",
        .cost = 4000U,
        .implementation = update_coverage_pair,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 1808416U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0x41a219486bd1d2baULL,
        .name = "CompareFloats",
        .cost = 20U,
        .implementation = compare_floats,
        .returns = EngineReturn::Integer,
    },
    {
        .address = 2447088U,
        .code_bytes = 424U,
        .code_fnv1a64 = 0x9ece6cb347429ff8ULL,
        .name = "UpdateBlockCoverage",
        .cost = 2000U,
        .implementation = update_block_coverage,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 3182752U,
        .code_bytes = 600U,
        .code_fnv1a64 = 0x99806cf924ba5ecdULL,
        .name = "BuildAxisRotation",
        .cost = 200U,
        .implementation = build_axis_rotation,
        .returns = EngineReturn::Nothing,
        .unused_words = kRotationUnusedWords,
        .check_absolute = 1e-4,
    },
    {
        .address = 2848928U,
        .code_bytes = 344U,
        .code_fnv1a64 = 0x55c13c5786896734ULL,
        .name = "ClipSegmentsToPlane",
        .cost = 20000U,
        .implementation = clip_segments_to_plane,
        .returns = EngineReturn::Integer,
    },
    {
        .address = 3126848U,
        .code_bytes = 304U,
        .code_fnv1a64 = 0xa082b59ef58e6cd5ULL,
        .name = "ApplyCurveModifier",
        .cost = 200U,
        .implementation = apply_curve_modifier,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 3127152U,
        .code_bytes = 280U,
        .code_fnv1a64 = 0xefcdc52619a58ccbULL,
        .name = "ApplyCurveModifierSet",
        .cost = 20000U,
        .implementation = apply_curve_modifier_set,
        .returns = EngineReturn::Nothing,
    },
    {
        .address = 1244208U,
        .code_bytes = 360U,
        .code_fnv1a64 = 0xd659020cbb1397d1ULL,
        .name = "GetInputAxis",
        .cost = 150U,
        .implementation = get_input_axis,
        .returns = EngineReturn::Float,
        .call_cost = input_axis_cost,
    },
    {
        .address = 2720800U,
        .code_bytes = 1208U,
        .code_fnv1a64 = 0xb8385242a9df1b96ULL,
        .name = "PositionSound",
        .cost = 800U,
        .implementation = position_sound,
        .returns = EngineReturn::Nothing,
        .call_cost = position_sound_cost,
    },
    {
        .address = 1443264U,
        .code_bytes = 1172U,
        .code_fnv1a64 = 0xac902d7867e7c7daULL,
        .name = "LaneSteeringCorrection",
        .cost = 700U,
        .implementation = lane_steering,
        .returns = EngineReturn::Float,
        .call_cost = lane_steering_cost,
    },
    {
        .address = 1448784U,
        .code_bytes = 4240U,
        .code_fnv1a64 = 0x9071a126e8d7c7d8ULL,
        .name = "UpdateDriverSpeed",
        .cost = 1500U,
        .implementation = update_driver_speed,
        .returns = EngineReturn::Nothing,
        .call_cost = driver_speed_cost,
    },
}};

} // namespace

std::span<const EngineFunction> math_library_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
