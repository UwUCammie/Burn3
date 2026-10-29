#pragma once
// CPU reference of the exact effect path (takedown_exact_reference.cpp).
#include "gs_interface.hpp"
#include <cstdint>
#include <vector>

namespace ParallelGS
{
struct ExactStats
{
	uint64_t colour = 0, depth = 0; // pixels written
};
// The pixels a recorded sprite covers (inclusive, scissored); false when none.
bool exact_sprite_rect(const GSInterface::TakedownExactPrim &prim, int &x0, int &y0, int &x1, int &y1);
// Executes one recorded sprite on `vram` (4 MiB of GS memory).
ExactStats exact_execute(std::vector<uint8_t> &vram, const GSInterface::TakedownExactPrim &prim);
}
