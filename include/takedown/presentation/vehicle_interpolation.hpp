#pragma once

#include "takedown/runtime/native_simulation_state.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace takedown::presentation {

// One guest 128-bit lane copied without interpretation. The fourth word of
// every lane is intentionally opaque: the presentation path has no authority
// to assign a meaning to the guest's w component.
struct RawVector128Words {
    std::array<std::uint32_t, 4U> words{};

    bool operator==(const RawVector128Words&) const = default;
};

// The vehicle endpoints are four 128-bit affine lanes: three basis lanes and
// one translation lane. This type is a value-only presentation copy of the
// guest words; it is never written back to runtime or guest state.
struct RawAffineTransformWords {
    std::array<RawVector128Words, 4U> lanes{};

    [[nodiscard]] static RawAffineTransformWords from_guest_words(
        const std::array<std::uint32_t, 16U>& words) noexcept;
    [[nodiscard]] std::array<std::uint32_t, 16U> guest_words() const noexcept;

    bool operator==(const RawAffineTransformWords&) const = default;
};

struct VehiclePresentationVec3 {
    double x{};
    double y{};
    double z{};
};

struct VehiclePresentationQuaternion {
    double x{};
    double y{};
    double z{};
    double w{1.0};
};

struct VehiclePresentationTransform {
    RawAffineTransformWords previous_raw{};
    RawAffineTransformWords current_raw{};
    VehiclePresentationVec3 translation{};
    VehiclePresentationQuaternion rotation{};
};

struct VehiclePresentationFrame {
    std::uint64_t session_epoch{};
    std::uint64_t previous_publication_sequence{};
    std::uint64_t current_publication_sequence{};
    double alpha{};
    VehiclePresentationTransform primary{};   // vehicle +0x00
    VehiclePresentationTransform secondary{}; // vehicle +0x19b0
};

enum class VehicleInterpolationStatus : std::uint8_t {
    Disabled,
    Unavailable,
    CatchUpLimited,
    InvalidPresentationMetadata,
    IncompatibleEndpoints,
    NonFiniteBasis,
    NonRigidBasis,
    Ready,
};

[[nodiscard]] const char* to_string(VehicleInterpolationStatus status) noexcept;

// Presentation interpolation is an explicit experiment. A default-constructed
// configuration performs no host-side transform decoding or interpolation.
struct VehicleInterpolationConfig {
    bool enabled{};
};

struct VehicleInterpolationResult {
    VehicleInterpolationStatus status{VehicleInterpolationStatus::Disabled};
    std::optional<VehiclePresentationFrame> frame{};

    [[nodiscard]] bool ready() const noexcept {
        return status == VehicleInterpolationStatus::Ready && frame.has_value();
    }
};

// Pure, presentation-only projection of one atomically copied runtime pair.
// It never advances simulation, mutates guest memory, or changes the source
// snapshots. The initial implementation accepts rigid finite bases, uses
// shortest-path quaternion slerp for orientation, and linearly interpolates
// endpoint translation without estimating velocity or extrapolating.
[[nodiscard]] VehicleInterpolationResult interpolate_vehicle(
    const runtime::NativeSimulationInterpolationState& state,
    VehicleInterpolationConfig config = {}) noexcept;

} // namespace takedown::presentation
