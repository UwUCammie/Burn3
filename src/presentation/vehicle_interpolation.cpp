#include "takedown/presentation/vehicle_interpolation.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace takedown::presentation {

namespace {

struct Vec3 {
    double x{};
    double y{};
    double z{};
};

struct Quaternion {
    double x{};
    double y{};
    double z{};
    double w{};
};

struct DecodedTransform {
    std::array<Vec3, 3> basis{};
    Vec3 translation{};
};

inline bool finite_value(const double value) noexcept {
    return std::abs(value) <= std::numeric_limits<double>::max();
}

// Reads one guest lane as an IEEE single and widens it.
inline double guest_float(const RawAffineTransformWords& raw, const std::size_t lane, const std::size_t word) noexcept {
    return static_cast<double>(std::bit_cast<float>(raw.lanes[lane].words[word]));
}

[[nodiscard]] std::optional<DecodedTransform> decode(const RawAffineTransformWords& raw) noexcept {
    if (!finite_value(guest_float(raw, 0, 0)) || !finite_value(guest_float(raw, 0, 1)) ||
        !finite_value(guest_float(raw, 0, 2)) || !finite_value(guest_float(raw, 1, 0)) ||
        !finite_value(guest_float(raw, 1, 1)) || !finite_value(guest_float(raw, 1, 2)) ||
        !finite_value(guest_float(raw, 2, 0)) || !finite_value(guest_float(raw, 2, 1)) ||
        !finite_value(guest_float(raw, 2, 2)) || !finite_value(guest_float(raw, 3, 0)) ||
        !finite_value(guest_float(raw, 3, 1)) || !finite_value(guest_float(raw, 3, 2))) {
        return std::nullopt;
    }
    return DecodedTransform{
        {Vec3{guest_float(raw, 0, 0), guest_float(raw, 0, 1), guest_float(raw, 0, 2)},
         Vec3{guest_float(raw, 1, 0), guest_float(raw, 1, 1), guest_float(raw, 1, 2)},
         Vec3{guest_float(raw, 2, 0), guest_float(raw, 2, 1), guest_float(raw, 2, 2)}},
        Vec3{guest_float(raw, 3, 0), guest_float(raw, 3, 1), guest_float(raw, 3, 2)},
    };
}

// Three unit, mutually orthogonal, right-handed axes.
[[nodiscard]] bool rigid_basis(const std::array<Vec3, 3>& basis) noexcept {
    constexpr double kLengthTolerance = 2.0e-3;
    constexpr double kMinimumLength = 1.0e-8;
    for (const Vec3 value : basis) {
        const double length = std::sqrt(value.z * value.z + (value.y * value.y + value.x * value.x));
        if (!finite_value(length) || length < kMinimumLength) {
            return false;
        }
        if (std::abs(length - 1.0) > kLengthTolerance) {
            return false;
        }
    }
    const Vec3 left = basis[0];
    const Vec3 right = basis[1];
    const Vec3& third = basis[2];
    const double left_right = right.x * left.x + left.y * right.y + left.z * right.z;
    if (std::abs(left_right) > kLengthTolerance) {
        return false;
    }
    const double left_third = left.x * third.x + left.y * third.y + left.z * third.z;
    if (std::abs(left_third) > kLengthTolerance) {
        return false;
    }
    const double right_third = right.x * third.x + right.y * third.y + right.z * third.z;
    if (std::abs(right_third) > kLengthTolerance) {
        return false;
    }
    const double determinant = (left.y * right.z - left.z * right.y) * third.x +
                               (left.z * right.x - left.x * right.z) * third.y +
                               (right.y * left.x - left.y * right.x) * third.z;
    return finite_value(determinant) && determinant > kLengthTolerance;
}

// Shepperd's method: the largest diagonal term picks the stable branch.
[[nodiscard]] Quaternion quaternion_from_basis(const std::array<Vec3, 3>& basis) noexcept {
    const double trace = basis[0].x + basis[1].y + basis[2].z;
    const double m00 = basis[0].x;
    const double m01 = basis[1].x;
    const double m02 = basis[2].x;
    const double m10 = basis[0].y;
    const double m11 = basis[1].y;
    const double m12 = basis[2].y;
    const double m20 = basis[0].z;
    const double m21 = basis[1].z;
    const double m22 = basis[2].z;
    Quaternion result;
    if (trace > 0.0) {
        const double root = std::sqrt(trace + 1.0);
        result.w = root * 5.0e-1;
        const double reciprocal = 5.0e-1 / root;
        result.x = (m21 - m12) * reciprocal;
        result.y = (m02 - m20) * reciprocal;
        result.z = (m10 - m01) * reciprocal;
    } else if (m00 > m11 && m00 > m22) {
        const double root = std::sqrt(m00 + 1.0 - m11 - m22 > 0.0 ? m00 + 1.0 - m11 - m22 : 0.0);
        result.x = root * 5.0e-1;
        const double reciprocal = root > 1.0e-8 ? 5.0e-1 / root : 0.0;
        result.y = (m01 + m10) * reciprocal;
        result.z = (m02 + m20) * reciprocal;
        result.w = (m21 - m12) * reciprocal;
    } else if (m11 > m22) {
        const double root = std::sqrt(m11 + 1.0 - m00 - m22 > 0.0 ? m11 + 1.0 - m00 - m22 : 0.0);
        result.y = root * 5.0e-1;
        const double reciprocal = root > 1.0e-8 ? 5.0e-1 / root : 0.0;
        result.x = (m01 + m10) * reciprocal;
        result.z = (m12 + m21) * reciprocal;
        result.w = (m02 - m20) * reciprocal;
    } else {
        const double root = std::sqrt(m22 + 1.0 - m00 - m11 > 0.0 ? m22 + 1.0 - m00 - m11 : 0.0);
        result.z = root * 5.0e-1;
        const double reciprocal = root > 1.0e-8 ? 5.0e-1 / root : 0.0;
        result.x = (m02 + m20) * reciprocal;
        result.y = (m12 + m21) * reciprocal;
        result.w = (m10 - m01) * reciprocal;
    }
    const Quaternion value = result;
    const double magnitude = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    return Quaternion{value.x / magnitude, value.y / magnitude, value.z / magnitude, value.w / magnitude};
}

} // namespace


namespace {

// Interpolates one affine endpoint pair; status reports why a pair cannot be used.
[[nodiscard]] std::optional<VehiclePresentationTransform> interpolate_transform(const std::array<std::uint32_t, 16U>& previous_words,
                                                                  const std::array<std::uint32_t, 16U>& current_words,
                                                                  const double alpha,
                                                                  VehicleInterpolationStatus& status) noexcept {
    const RawAffineTransformWords previous_raw = RawAffineTransformWords::from_guest_words(previous_words);
    const RawAffineTransformWords current_raw = RawAffineTransformWords::from_guest_words(current_words);
    const auto previous = decode(previous_raw);
    const auto current = decode(current_raw);
    if (!previous.has_value() || !current.has_value()) {
        status = VehicleInterpolationStatus::NonFiniteBasis;
        return std::nullopt;
    }
    if (!rigid_basis(previous->basis) || !rigid_basis(current->basis)) {
        status = VehicleInterpolationStatus::NonRigidBasis;
        return std::nullopt;
    }
    const Quaternion previous_rotation = quaternion_from_basis(previous->basis);
    const Quaternion current_rotation = quaternion_from_basis(current->basis);
    if (!finite_value(previous_rotation.x) || !finite_value(previous_rotation.y) ||
        !finite_value(previous_rotation.z) || !finite_value(previous_rotation.w) ||
        !finite_value(current_rotation.x) || !finite_value(current_rotation.y) ||
        !finite_value(current_rotation.z) || !finite_value(current_rotation.w)) {
        status = VehicleInterpolationStatus::NonFiniteBasis;
        return std::nullopt;
    }
    const VehiclePresentationVec3 translation{
        (1.0 - alpha) * previous->translation.x + current->translation.x * alpha,
        (1.0 - alpha) * previous->translation.y + current->translation.y * alpha,
        (1.0 - alpha) * previous->translation.z + current->translation.z * alpha,
    };
    Quaternion rotation;
    if (alpha == 0.0) {
        rotation = previous_rotation;
    } else {
        double endpoint_x = current_rotation.x;
        double endpoint_y = current_rotation.y;
        double endpoint_z = current_rotation.z;
        double endpoint_w = current_rotation.w;
        double cosine = previous_rotation.x * endpoint_x + previous_rotation.y * endpoint_y +
                        previous_rotation.z * endpoint_z + previous_rotation.w * endpoint_w;
        if (cosine < 0.0) {
            endpoint_x = -endpoint_x;
            endpoint_y = -endpoint_y;
            endpoint_z = -endpoint_z;
            endpoint_w = -endpoint_w;
            cosine = -cosine;
        }
        const double clamped = std::min(std::max(cosine, -1.0), 1.0);
        if (clamped > 9.995e-1) {
            const double x = alpha * endpoint_x + (1.0 - alpha) * previous_rotation.x;
            const double y = alpha * endpoint_y + (1.0 - alpha) * previous_rotation.y;
            const double z = alpha * endpoint_z + (1.0 - alpha) * previous_rotation.z;
            const double w = alpha * endpoint_w + (1.0 - alpha) * previous_rotation.w;
            const double magnitude = std::sqrt(x * x + y * y + z * z + w * w);
            rotation = Quaternion{x / magnitude, y / magnitude, z / magnitude, w / magnitude};
        } else {
            const double angle = std::acos(clamped);
            const double sine = std::sin(angle);
            if (std::abs(sine) < 1.0e-8) {
                rotation = previous_rotation;
            } else {
                const double x = endpoint_x * (std::sin(alpha * angle) / sine) +
                                 previous_rotation.x * (std::sin((1.0 - alpha) * angle) / sine);
                const double y = endpoint_y * (std::sin(alpha * angle) / sine) +
                                 previous_rotation.y * (std::sin((1.0 - alpha) * angle) / sine);
                const double z = endpoint_z * (std::sin(alpha * angle) / sine) +
                                 previous_rotation.z * (std::sin((1.0 - alpha) * angle) / sine);
                const double w = endpoint_w * (std::sin(alpha * angle) / sine) +
                                 previous_rotation.w * (std::sin((1.0 - alpha) * angle) / sine);
                const double magnitude = std::sqrt(x * x + y * y + z * z + w * w);
                rotation = Quaternion{x / magnitude, y / magnitude, z / magnitude, w / magnitude};
            }
        }
    }
    return VehiclePresentationTransform{previous_raw, current_raw, translation,
                                        VehiclePresentationQuaternion{rotation.x, rotation.y, rotation.z, rotation.w}};
}

} // namespace

RawAffineTransformWords RawAffineTransformWords::from_guest_words(const std::array<std::uint32_t, 16U>& words) noexcept {
    RawAffineTransformWords result{};
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        for (std::size_t word = 0; word < 4U; ++word) {
            result.lanes[lane].words[word] = words[lane * 4U + word];
        }
    }
    return result;
}

std::array<std::uint32_t, 16U> RawAffineTransformWords::guest_words() const noexcept {
    std::array<std::uint32_t, 16U> words{};
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        for (std::size_t word = 0; word < 4U; ++word) {
            words[lane * 4U + word] = lanes[lane].words[word];
        }
    }
    return words;
}

const char* to_string(const VehicleInterpolationStatus status) noexcept {
    switch (status) {
    case VehicleInterpolationStatus::Disabled:
        return "disabled";
    case VehicleInterpolationStatus::Unavailable:
        return "unavailable";
    case VehicleInterpolationStatus::CatchUpLimited:
        return "catch-up-limited";
    case VehicleInterpolationStatus::InvalidPresentationMetadata:
        return "invalid-presentation-metadata";
    case VehicleInterpolationStatus::IncompatibleEndpoints:
        return "incompatible-endpoints";
    case VehicleInterpolationStatus::NonFiniteBasis:
        return "non-finite-basis";
    case VehicleInterpolationStatus::NonRigidBasis:
        return "non-rigid-basis";
    case VehicleInterpolationStatus::Ready:
        return "ready";
    }
    return "unknown";
}


namespace {

// First failing pair or presentation check, in the order the presentation path applies them.
VehicleInterpolationStatus pair_status(const runtime::NativeSimulationInterpolationState& state) noexcept {
    using runtime::NativeSimulationVehicleState;
    const auto& presentation = state.presentation;
    const auto& previous = state.pair.previous;
    const auto& current = state.pair.current;
    if (presentation.catch_up_limited) {
        return VehicleInterpolationStatus::CatchUpLimited;
    }
    if (presentation.session_epoch == 0U) {
        return VehicleInterpolationStatus::Unavailable;
    }
    if (!presentation.authoritative) {
        return VehicleInterpolationStatus::Unavailable;
    }
    if (!presentation.presentation_allowed) {
        return VehicleInterpolationStatus::Unavailable;
    }
    const double alpha = presentation.interpolation_alpha;
    if (!finite_value(alpha)) {
        return VehicleInterpolationStatus::InvalidPresentationMetadata;
    }
    if (alpha < 0.0) {
        return VehicleInterpolationStatus::InvalidPresentationMetadata;
    }
    if (alpha >= 1.0) {
        return VehicleInterpolationStatus::InvalidPresentationMetadata;
    }
    if (previous.session_epoch != current.session_epoch) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (presentation.session_epoch != previous.session_epoch) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.publication_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (current.publication_sequence != previous.publication_sequence + 1U) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (current.publication_sequence != presentation.associated_publication_sequence) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.simulation_rate.numerator != current.simulation_rate.numerator) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.simulation_rate.denominator != current.simulation_rate.denominator) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.reference_rate.numerator != current.reference_rate.numerator) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.reference_rate.denominator != current.reference_rate.denominator) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (!previous.authoritative) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (!current.authoritative) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.step.simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (current.step.simulation_tick != previous.step.simulation_tick + 1U) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (current.guest_time_cycles < previous.guest_time_cycles) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (!previous.vehicle_state.has_value()) {
        return VehicleInterpolationStatus::Unavailable;
    }
    if (!current.vehicle_state.has_value()) {
        return VehicleInterpolationStatus::Unavailable;
    }
    const NativeSimulationVehicleState& before = *previous.vehicle_state;
    const NativeSimulationVehicleState& after = *current.vehicle_state;
    if (!before.complete || !after.complete) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (!before.role_valid || !after.role_valid) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (before.vehicle_object != after.vehicle_object) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (before.physics_object != after.physics_object) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (before.physics_component != after.physics_component) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (before.owner_vehicle != after.owner_vehicle) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (after.source_sequence <= before.source_sequence) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (previous.guest_time_cycles != before.guest_time_cycles) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    if (current.guest_time_cycles != after.guest_time_cycles) {
        return VehicleInterpolationStatus::IncompatibleEndpoints;
    }
    return VehicleInterpolationStatus::Ready;
}

} // namespace

VehicleInterpolationResult interpolate_vehicle(const runtime::NativeSimulationInterpolationState& state,
                                               const VehicleInterpolationConfig config) noexcept {
    if (!config.enabled) {
        return {};
    }
    const VehicleInterpolationStatus pair_status_value = pair_status(state);
    if (pair_status_value != VehicleInterpolationStatus::Ready) {
        return VehicleInterpolationResult{pair_status_value, std::nullopt};
    }
    VehicleInterpolationStatus status = VehicleInterpolationStatus::Ready;
    const double alpha = state.presentation.interpolation_alpha;
    const auto& before = *state.pair.previous.vehicle_state;
    const auto& after = *state.pair.current.vehicle_state;
    const auto primary = interpolate_transform(before.transform_words, after.transform_words, alpha, status);
    if (!primary.has_value()) {
        return VehicleInterpolationResult{status, std::nullopt};
    }
    const auto secondary = interpolate_transform(before.vector_words, after.vector_words, alpha, status);
    if (!secondary.has_value()) {
        return VehicleInterpolationResult{status, std::nullopt};
    }
    VehiclePresentationFrame frame;
    frame.primary = *primary;
    frame.secondary = *secondary;
    frame.session_epoch = state.presentation.session_epoch;
    frame.previous_publication_sequence = state.pair.previous.publication_sequence;
    frame.current_publication_sequence = state.pair.current.publication_sequence;
    frame.alpha = alpha;
    return VehicleInterpolationResult{VehicleInterpolationStatus::Ready, frame};
}

} // namespace takedown::presentation

