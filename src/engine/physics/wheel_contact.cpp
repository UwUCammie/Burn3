#include "takedown/engine/physics/wheel_contact.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

namespace takedown::engine {
namespace {

using Step = WheelContactStep;

constexpr float kProbeTravel = 0.75F;   // of the suspension's travel below rest
constexpr float kTiltedLift = 0.25F;    // of the radius
constexpr float kGroundDepth = 30.0F;   // how far below the car the ground is looked for
constexpr float kNoGround = 1000.0F;
constexpr float kNoGroundDistance = 9999.0F;
constexpr std::uint8_t kLowSurfaces = 12U;
constexpr std::uint8_t kOrdinarySurfaces = 21U;

// A point on the car through its transform (x, then y, then z, then the
// position, each a multiply-add).
Vector3 on_car(const Transform& transform, const Vector3 local) noexcept {
    Vector3 point = chop_mul(transform.basis[0], local.x);
    point = chop_add(point, chop_mul(transform.basis[1], local.y));
    point = chop_add(point, chop_mul(transform.basis[2], local.z));
    return chop_add(point, transform.translation);
}

} // namespace

WheelContactSearch find_wheel_contacts(WheelContactCar& car) {
    WheelContactSearch search;
    const Transform& transform = *car.transform;
    const auto count = static_cast<std::size_t>(car.wheel_count);
    search.pass(Step::Start);
    search.pass(Step::SaveCheck, static_cast<std::uint32_t>(count) + 1U);
    search.pass(Step::Save, static_cast<std::uint32_t>(count));

    // The probe's ends on the car.
    search.pass(Step::EndsSetUp);
    std::array<Vector3, kSuspensionWheels> tops{};
    std::array<Vector3, kSuspensionWheels> bottoms{};
    for (std::size_t index = 0U; index < count; ++index) {
        search.pass(Step::EndsCheck);
        search.pass(Step::Ends);
        WheelContactWheel& wheel = car.wheels[index];
        search.pass(index < 2U ? Step::FrontTravel : Step::RearTravel);
        const float travel = index < 2U ? car.front_travel : car.rear_travel;
        search.pass(Step::EndsHeights);
        wheel.top_height = chop_add(wheel.rest_offset, wheel.radius);
        wheel.bottom_height = chop_sub(chop_sub(wheel.rest_offset, chop_mul(kProbeTravel, travel)), wheel.radius);
        tops[index] = {wheel.model_position.x, chop_add(0.0F, wheel.top_height), wheel.model_position.z};
        bottoms[index] = {wheel.model_position.x, chop_add(0.0F, wheel.bottom_height), wheel.model_position.z};
        if (wheel.mount == WheelMount::Tilted) {
            search.pass(Step::Tilted);
            bottoms[index] = chop_add(bottoms[index],
                                      Vector3{0.0F, chop_add(0.0F, chop_mul(kTiltedLift, wheel.radius)), 0.0F});
        } else {
            search.pass(Step::Upright);
        }
    }
    search.pass(Step::EndsCheck);
    search.pass(Step::TopsSetUp);
    for (std::size_t index = 0U; index < count; ++index) {
        search.pass(Step::TopsCheck);
        search.pass(Step::Top);
        car.wheels[index].top = on_car(transform, tops[index]);
    }
    search.pass(Step::TopsCheck);
    search.pass(Step::BottomsSetUp);
    for (std::size_t index = 0U; index < count; ++index) {
        search.pass(Step::BottomsCheck);
        search.pass(Step::Bottom);
        car.wheels[index].bottom = on_car(transform, bottoms[index]);
    }
    search.pass(Step::BottomsCheck);

    // One down vector for every wheel: from the highest probe top to the
    // lowest probe bottom of the second and last wheels.
    search.pass(Step::Down);
    const float top = maximum(car.wheels[1].top_height, car.wheels[count - 1U].top_height);
    const float bottom = minimum(car.wheels[1].bottom_height, car.wheels[count - 1U].bottom_height);
    const Vector3 up = transform.basis[1];
    search.down = chop_mul(up, -chop_sub(top, bottom));

    search.marker = car.marker;
    for (std::size_t index = 0U; index < count; ++index) {
        search.pass(Step::ContactCheck);
        search.pass(Step::ContactStart);
        WheelContactWheel& wheel = car.wheels[index];
        wheel.grounded = false;
        if (wheel.mount != WheelMount::Missing) {
            search.pass(Step::Probe);
            wheel.probe = probe_mesh(car.mesh, wheel.top, wheel.bottom, car.rules, search.marker);
            wheel.probed = true;
            search.marker = wheel.probe.marker;
            search.pass(Step::ProbeResult);
            wheel.grounded = wheel.probe.nearest >= 0;
        }
        if (!wheel.grounded) {
            search.pass(Step::NoContact);
            wheel.contact_point = chop_sub(wheel.position, chop_mul(up, wheel.radius));
            continue;
        }
        search.pass(Step::Contact);
        const auto nearest = static_cast<std::size_t>(wheel.probe.nearest);
        wheel.steep = game_less(car.mesh[nearest].normal.y, car.least_upward_normal);
        search.pass(wheel.steep ? Step::SteepNormal : Step::TriangleNormal);
        const auto surface = static_cast<std::uint8_t>(car.surfaces[nearest] & 0xffU);
        search.pass(Step::SurfaceCheck);
        search.pass(Step::LowCheck);
        const bool low = surface < kLowSurfaces;
        search.pass(low ? Step::Low : Step::NotLow);
        search.pass(Step::LowBranch);
        wheel.surface_taken = low;
        if (!low) {
            search.pass(Step::MiddleCheck);
            search.pass(Step::MiddleRangeCheck);
            const bool middle = surface < kOrdinarySurfaces;
            search.pass(middle ? Step::Middle : Step::NotMiddle);
            search.pass(Step::MiddleBranch);
            wheel.surface_taken = middle;
            if (!middle) {
                search.pass(Step::SpecialCheck);
                wheel.surface_taken = surface == kSpecialSurface;
            }
        }
        if (wheel.surface_taken) {
            search.pass(Step::SurfaceTaken);
        }
        search.pass(Step::ContactPoint);
        const float length = chop_sqrt(chop_dot(search.down, search.down));
        const float along = game_div(wheel.probe.distance, length);
        wheel.contact_point = chop_add(wheel.top, chop_mul(search.down, along));
        wheel.ground_offset = chop_add(wheel.radius, chop_sub(wheel.top_height, wheel.probe.distance));
        search.pass(Step::WheelDone);
    }
    search.pass(Step::ContactCheck);

    // The ground under the car.
    search.pass(Step::GroundSetUp);
    search.ground_start = transform.translation;
    search.ground_end = {transform.translation.x, chop_add(0.0F, chop_sub(transform.translation.y, kGroundDepth)),
                         transform.translation.z};
    search.ground_distance = kNoGroundDistance;
    float nearest = kNoGround;
    for (std::size_t index = 0U; index < car.mesh.size(); ++index) {
        search.pass(Step::GroundCheck);
        search.pass(Step::GroundTest);
        const CollisionTriangle& triangle = car.mesh[index];
        search.last_ground_test =
            test_segment_against_triangle(search.ground_start, search.ground_end, triangle.a, triangle.b, triangle.c);
        ++search.ground_tests;
        if (search.last_ground_test.tested) {
            search.ground_solved = true;
            search.last_ground_solved = search.last_ground_test;
        }
        search.last_ground_test_nearer = false;
        if (!search.last_ground_test.hit) {
            search.pass(Step::GroundMiss);
        } else {
            search.pass(Step::GroundHit);
            ++search.ground_hits;
            if (game_less(search.last_ground_test.t, nearest)) {
                search.pass(Step::GroundNearer);
                nearest = search.last_ground_test.t;
                search.ground_triangle = static_cast<std::int32_t>(index);
                search.ground_distance = chop_add(0.0F, chop_mul(kGroundDepth, nearest));
                search.last_ground_test_nearer = true;
            }
        }
        search.pass(Step::GroundNext);
    }
    search.pass(Step::GroundCheck);
    search.pass(Step::Finish);
    return search;
}

} // namespace takedown::eng
