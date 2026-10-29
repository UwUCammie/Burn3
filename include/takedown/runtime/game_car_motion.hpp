#pragma once

// Car record updates other adapters share (src/runtime/game/physics.cpp):
// placing a car and setting its speed outright, as the original's
// SetCarTransform (0x001ab610) and SetCarSpeed (0x001bed00) do.

#include "takedown/engine/math/vector.hpp"
#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstdint>

namespace takedown::runtime {

// Copies the four rows (right, up, at, position; fourth lanes included)
// into the car at `car`'s transform and its inverse. Returns the original's
// instruction count.
std::uint32_t set_car_transform(guest::EeMemory& memory, guest::GuestAddress car,
                                const std::array<engine::Vector4, 4>& rows);

// Sets the car moving at `speed` along its forward axis (wheels and gearbox
// to match). `unchanged_engine_rate` is the rate a gearbox of under two gears
// is left with (the original's f3). Returns the original's instruction count.
std::uint32_t set_car_speed(guest::EeMemory& memory, guest::GuestAddress car, float speed,
                            float unchanged_engine_rate);

} // namespace takedown::runtime
