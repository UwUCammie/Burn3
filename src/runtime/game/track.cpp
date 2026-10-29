// Adapters for the track system (sections, routes).

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/cubic_basis.hpp"
#include "takedown/engine/math/quad_edges.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/trigonometry.hpp"
#include "takedown/engine/track/fade_effects.hpp"
#include "takedown/engine/track/lane_bins.hpp"
#include "takedown/engine/track/quad_rules.hpp"
#include "takedown/engine/track/route.hpp"
#include "takedown/engine/track/sections.hpp"
#include "takedown/engine/track/strip_follow.hpp"
#include "takedown/engine/track/strip_search.hpp"
#include "takedown/engine/track/surface_follow.hpp"
#include "takedown/engine/track/traffic_drive.hpp"
#include "takedown/engine/world/ray_mesh_query.hpp"
#include "takedown/runtime/game_car_motion.hpp"
#include "takedown/runtime/game_segment_test.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <type_traits>

namespace takedown::runtime {
namespace {

using guest::GuestAddress;

// The game's loop of track sections (a member of the track object).
struct SectionLoopLayout {
    static constexpr GuestAddress kCount = 12U;      // s32
    static constexpr GuestAddress kValid = 68U;      // u8, 0 while no track is loaded
    static constexpr GuestAddress kLast = 76U;       // s32, a recently found section
    static constexpr GuestAddress kSections = 380U;  // section pointers, one per index
};
// A section: its four edge lines at +96 (x coefficients), +112 (z
// coefficients) and +128 (offsets), one edge per lane.
constexpr GuestAddress kSectionEdges = 96U;

// FindTrackSection (original 0x00251740): the index of the section that
// contains the point at a1 (x and z), searched near the loop's last section,
// or -1.
NativeFunctionExecution find_track_section(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress loop = call.arg(0);
    const GuestAddress point = call.arg(1);
    std::int32_t result = -1;
    if (memory.read_u8(loop + SectionLoopLayout::kValid) != 0U) {
        const auto count = static_cast<std::int32_t>(memory.read_u32_le(loop + SectionLoopLayout::kCount));
        const auto last = static_cast<std::int32_t>(memory.read_u32_le(loop + SectionLoopLayout::kLast));
        const float x = load_float(memory, point);
        const float z = load_float(memory, point + 8U);
        const auto found = engine::find_section_near(count, last, x, z, [&](const std::int32_t index) {
            const GuestAddress section = memory.read_u32_le(
                loop + SectionLoopLayout::kSections + static_cast<GuestAddress>(index) * 4U);
            return engine::SectionEdges{load_vector4(memory, section + kSectionEdges),
                                        load_vector4(memory, section + kSectionEdges + 16U),
                                        load_vector4(memory, section + kSectionEdges + 32U)};
        });
        result = found.value_or(-1);
    }
    call.return_value(static_cast<std::uint32_t>(result));
    return call.finish(120U);
}

// A strip mesh loaded with the track (0x0014fe2c stores its vertex array
// pointer): 16-byte vertices, and per object a u16 index strip of vertex
// pairs, quad n using indices 2n to 2n+3.
constexpr GuestAddress kStripVertices = 0x0064d354U;  // pointer
constexpr GuestAddress kStripUp = 0x004c9cf0U;        // the axis quads are seen along

// The strip objects: a count, and 8-byte entries from 0x00522660 whose
// second word points to the object's strip (u16 index list pointer at +0,
// quad count u16 at +12). Callers are given the entry's address + 44.
constexpr GuestAddress kStripObjectCount = 0x0064d358U;
constexpr GuestAddress kStripObjects = 0x00522660U;
constexpr GuestAddress kStripObjectHandle = 44U;
constexpr GuestAddress kStripObjectStrip = 48U;
constexpr GuestAddress kStripQuadCount = 12U;
constexpr GuestAddress kNearestQuadLimit = 0x004e0c14U;  // FLT_MAX

// The search's cost: the original's instructions grow with the quads tested.
std::uint32_t nearest_strip_quad_cost(const NativeFunctionPrepareContext& context) {
    const auto& memory = context.memory;
    const auto objects = static_cast<std::int32_t>(memory.read_u32_le(kStripObjectCount));
    std::uint32_t quads = 0U;
    for (std::int32_t object = 0; object < objects; ++object) {
        const GuestAddress strip =
            memory.read_u32_le(kStripObjects + static_cast<GuestAddress>(object) * 8U + kStripObjectStrip);
        quads += memory.read_u16_le(strip + kStripQuadCount);
    }
    return 20U + 10U * static_cast<std::uint32_t>(objects > 0 ? objects : 0) + 40U * quads;
}

// FindNearestStripQuad (original 0x00147290): the strip quad whose centre is
// nearest the point at a3 (an aligned quadword). Writes the object's handle
// to a0 (0 when none), the quad index to the u16 at a1 (0xffff when none),
// and clears the byte at a2 when one is found.
NativeFunctionExecution find_nearest_strip_quad(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    memory.write_u32_le(call.arg(0), 0U);
    memory.write_u16_le(call.arg(1), 0xffffU);
    const engine::Vector3 point = load_vector4(memory, call.arg(3) & ~15U).xyz();
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    const auto strip = [&](const std::int32_t object) {
        return memory.read_u32_le(kStripObjects + static_cast<GuestAddress>(object) * 8U + kStripObjectStrip);
    };
    const auto object_count = [&] { return static_cast<std::int32_t>(memory.read_u32_le(kStripObjectCount)); };
    const auto quad_count = [&](const std::int32_t object) {
        return static_cast<std::int32_t>(memory.read_u16_le(strip(object) + kStripQuadCount));
    };
    const auto corner = [&](const std::int32_t object, const std::int32_t quad, const std::int32_t k) {
        const GuestAddress index = memory.read_u32_le(strip(object)) + static_cast<GuestAddress>(quad * 4 + k * 2);
        return load_vector4(memory, vertices + memory.read_u16_le(index) * 16U).xyz();
    };
    const engine::NearestStripQuad nearest = engine::find_nearest_strip_quad(
        point, load_float(memory, kNearestQuadLimit), object_count, quad_count, corner);
    if (nearest.object >= 0) {
        memory.write_u32_le(call.arg(0),
                            kStripObjects + static_cast<GuestAddress>(nearest.object) * 8U + kStripObjectHandle);
        memory.write_u16_le(call.arg(1), static_cast<std::uint16_t>(nearest.quad));
        memory.write_u8(call.arg(2), 0U);
    }
    return call.finish(0U);  // reported from nearest_strip_quad_cost
}

// StripEdgeFraction (original 0x00145ef0): engine::fraction_along_edge of
// the point at a2 (an aligned quadword) along edge a1 of the strip of object
// a0 with margin f12; 0.5 for an edge whose two ends are the same vertex.
NativeFunctionExecution strip_edge_fraction(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress edge = memory.read_u32_le(memory.read_u32_le(call.arg(0) + 4U)) + (call.arg(1) & 0xffffU) * 4U;
    const std::uint16_t from_index = memory.read_u16_le(edge);
    const std::uint16_t to_index = memory.read_u16_le(edge + 2U);
    if (from_index == to_index) {
        call.return_float(0.5F);
        return call.finish(12U);
    }
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    call.return_float(engine::fraction_along_edge(load_vector4(memory, vertices + from_index * 16U).xyz(),
                                                  load_vector4(memory, vertices + to_index * 16U).xyz(),
                                                  load_vector4(memory, call.arg(2) & ~15U).xyz(), call.float_arg(0)));
    return call.finish(80U);
}

// The vehicle fields DistanceAhead reads.
struct HeadingLayout {
    static constexpr GuestAddress kPosition = 48U;
    static constexpr GuestAddress kFallback = 32U;
    static constexpr GuestAddress kHeading = 6352U;
};

// DistanceAhead (original 0x001419f0): how far the object at a1 is ahead of
// the vehicle at a0 along its flat heading (engine::distance_ahead).
NativeFunctionExecution distance_ahead(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress self = call.arg(0);
    const float distance = engine::distance_ahead(
        load_vector4(memory, self + HeadingLayout::kPosition).xyz(),
        load_vector4(memory, call.arg(1) + HeadingLayout::kPosition).xyz(),
        load_vector4(memory, self + HeadingLayout::kHeading).xyz(),
        load_vector4(memory, self + HeadingLayout::kFallback).xyz());
    call.return_float(distance);
    call.return_value(std::bit_cast<std::uint32_t>(distance));
    return call.finish(50U);
}

// A sampled curve: a sample count (s32 at +16) and 8-byte samples (the value
// first) through the pointer at +4.
struct SampledCurveLayout {
    static constexpr GuestAddress kSamples = 4U;
    static constexpr GuestAddress kCount = 16U;
};

// CurveDifference (original 0x001602f0): engine::sampled_curve of the curve
// a0 points to at f12 minus that at f13.
NativeFunctionExecution curve_difference(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress curve = memory.read_u32_le(call.arg(0));
    const GuestAddress samples = memory.read_u32_le(curve + SampledCurveLayout::kSamples);
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(curve + SampledCurveLayout::kCount));
    const auto sample = [&](const std::int32_t index) {
        return load_float(memory, samples + static_cast<GuestAddress>(index * 8));
    };
    call.return_float(engine::sampled_curve(sample, count, call.float_arg(0)) -
                      engine::sampled_curve(sample, count, call.float_arg(1)));
    return call.finish(40U);
}

// A strip object's strip: its index list pointer (+0), the number of quads
// (u16 at +12) and whether it closes into a loop (u8 at +14).
struct StripLayout {
    static constexpr GuestAddress kIndices = 0U;
    static constexpr GuestAddress kQuadCount = 12U;
    static constexpr GuestAddress kClosed = 14U;
};

// The step is the difference of two sums of four corners at track
// coordinates (hundreds of metres), so a small component keeps the rounding
// of those sums: up to 6e-5 m between rounding to nearest and toward zero.
constexpr double kQuadStepAbsoluteTolerance = 2e-4;

// StripQuadStep (original 0x00147d00): writes to a2 the sum of the corners
// two quads on from quad a1 of the strip of object a0, less the sum of quad
// a1's corners (four times the step between their centres; w from the third
// corner ahead). Near the end it wraps to the start of a closed strip, or
// repeats the strip's last edge of an open one.
NativeFunctionExecution strip_quad_step(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress strip = memory.read_u32_le(call.arg(0) + 4U);
    const GuestAddress indices = memory.read_u32_le(strip + StripLayout::kIndices);
    const std::int32_t quads = memory.read_u16_le(strip + StripLayout::kQuadCount);
    const bool closed = memory.read_u8(strip + StripLayout::kClosed) != 0U;
    const auto quad = static_cast<std::int32_t>(call.arg(1) & 0xffffU);
    const auto index = [&](const std::int32_t k) {
        return memory.read_u16_le(indices + static_cast<GuestAddress>(k * 2));
    };
    std::uint16_t ahead[4];
    if (quad == quads - 2) {
        ahead[0] = index(2 * quad + 4);
        ahead[1] = index(2 * quad + 5);
        ahead[2] = closed ? index(2) : index(2 * quads);
        ahead[3] = closed ? index(3) : index(2 * quads + 1);
    } else if (quad == quads - 1) {
        ahead[0] = closed ? index(2) : index(2 * quads);
        ahead[1] = closed ? index(3) : index(2 * quads + 1);
        ahead[2] = closed ? index(4) : index(2 * quads);
        ahead[3] = closed ? index(5) : index(2 * quads + 1);
    } else {
        for (std::int32_t k = 0; k < 4; ++k) {
            ahead[k] = index(2 * quad + 4 + k);
        }
    }
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    const auto corner = [&](const std::uint16_t vertex) { return load_vector4(memory, vertices + vertex * 16U); };
    const engine::Vector3 current =
        ((corner(index(2 * quad)).xyz() + corner(index(2 * quad + 1)).xyz()) + corner(index(2 * quad + 2)).xyz()) +
        corner(index(2 * quad + 3)).xyz();
    const engine::Vector4 third = corner(ahead[2]);
    const engine::Vector3 next =
        ((corner(ahead[0]).xyz() + corner(ahead[1]).xyz()) + third.xyz()) + corner(ahead[3]).xyz();
    const engine::Vector3 step = next - current;
    store_vector4(memory, call.arg(2) & ~15U, {step.x, step.y, step.z, third.w});
    return call.finish(70U);
}

// StripEdgeLength (original 0x00147c10): the horizontal length of edge a1
// (a vertex index pair) of the strip of object a0.
float strip_edge_horizontal_length(const guest::EeMemory& memory, const GuestAddress object,
                                   const std::uint32_t edge_index) {
    const GuestAddress edge = memory.read_u32_le(memory.read_u32_le(object + 4U)) + (edge_index & 0xffffU) * 4U;
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    const engine::Vector3 from = load_vector4(memory, vertices + memory.read_u16_le(edge) * 16U).xyz();
    const engine::Vector3 to = load_vector4(memory, vertices + memory.read_u16_le(edge + 2U) * 16U).xyz();
    engine::Vector3 along = from - to;
    along.y = 0.0F;
    return std::sqrt(engine::dot(along, along));
}

NativeFunctionExecution strip_edge_length(NativeFunctionContext& context) {
    GuestCall call{context};
    const float length = strip_edge_horizontal_length(call.memory(), call.arg(0), call.arg(1));
    call.return_float(length);
    call.return_value(std::bit_cast<std::uint32_t>(length));
    return call.finish(32U);
}

// ClassifyPointInStripQuad (original 0x00148440): which edges of quad a2 of
// the strip of object a0 (through the pointer at +4, then +0) the point at a1
// lies outside of (engine::QuadEdge bits). The point is read as an aligned
// quadword, like the original's lq.
// Quad `quad_index` of the strip of the object whose handle is `object`.
engine::StripQuad load_strip_quad(const guest::EeMemory& memory, const GuestAddress object,
                                  const std::uint32_t quad_index) {
    const GuestAddress indices = memory.read_u32_le(memory.read_u32_le(object + 4U)) + (quad_index & 0xffffU) * 4U;
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    std::uint16_t corner_index[4];
    engine::StripQuad quad;
    for (GuestAddress corner = 0U; corner < 4U; ++corner) {
        corner_index[corner] = static_cast<std::uint16_t>(memory.read_u16_le(indices + corner * 2U));
        quad.corners[corner] = load_vector4(memory, vertices + corner_index[corner] * 16U).xyz();
    }
    quad.has_start = corner_index[0] != corner_index[1];
    quad.has_end = corner_index[2] != corner_index[3];
    return quad;
}

std::uint32_t strip_quad_outside_edges(const guest::EeMemory& memory, const GuestAddress object,
                                       const std::uint32_t quad_index, const engine::Vector3 point) {
    return engine::quad_outside_edges(load_strip_quad(memory, object, quad_index), point,
                                      load_vector4(memory, kStripUp).xyz());
}

NativeFunctionExecution classify_point_in_strip_quad(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_value(strip_quad_outside_edges(call.memory(), call.arg(0), call.arg(2),
                                               load_vector4(call.memory(), call.arg(1) & ~15U).xyz()));
    return call.finish(100U);
}

// A strip quad's 10-byte record (from the strip's pointer at +8): its kind
// in the low three bits of byte 3, and the neighbour across each side (the
// object at +4 and +5, its quad at +6 and +8; object 255 is none).
constexpr GuestAddress kStripRecords = 8U;
constexpr GuestAddress kQuadRecordBytes = 10U;
constexpr GuestAddress kQuadRecordKind = 3U;
constexpr GuestAddress kQuadRecordNeighbourObject = 4U;
constexpr GuestAddress kQuadRecordNeighbourQuad = 6U;
constexpr std::uint8_t kNoStripObject = 255U;

GuestAddress strip_object_strip(const guest::EeMemory& memory, const std::uint32_t object) {
    return memory.read_u32_le(kStripObjects + (object & 0xffU) * 8U + kStripObjectStrip);
}

GuestAddress quad_record(const guest::EeMemory& memory, const GuestAddress strip, const std::uint32_t quad) {
    return memory.read_u32_le(strip + kStripRecords) + (quad & 0xffffU) * kQuadRecordBytes;
}

std::uint8_t quad_kind(const guest::EeMemory& memory, const GuestAddress strip, const std::uint32_t quad) {
    return static_cast<std::uint8_t>(memory.read_u8(quad_record(memory, strip, quad) + kQuadRecordKind) & 7U);
}

engine::RouteAhead route_open_along(const guest::EeMemory& memory, const GuestAddress strip,
                                    const std::uint32_t quad, const std::uint32_t count) {
    return engine::route_open_ahead([&](const std::uint16_t at) { return quad_kind(memory, strip, at); },
                                    memory.read_u16_le(strip + kStripQuadCount),
                                    memory.read_u8(strip + StripLayout::kClosed) != 0U,
                                    static_cast<std::uint16_t>(quad), static_cast<std::uint16_t>(count));
}

engine::RouteAhead strip_route_open_ahead(const guest::EeMemory& memory, const std::uint32_t object,
                                          const std::uint32_t quad, const std::uint32_t count) {
    return route_open_along(memory, strip_object_strip(memory, object), quad, count);
}

// RouteOpenAhead (original 0x00148ca0): 1 when none of the a2 quads from quad
// a1 of strip object a0 is closed (engine::route_open_ahead), else 0; 0 for
// no object (255).
NativeFunctionExecution route_open_ahead(NativeFunctionContext& context) {
    GuestCall call{context};
    if ((call.arg(0) & 0xffU) == kNoStripObject) {
        call.return_value(0U);
        return call.finish(5U);
    }
    const auto ahead = strip_route_open_ahead(call.memory(), call.arg(0), call.arg(1), call.arg(2));
    call.return_value(ahead.open ? 1U : 0U);
    return call.finish(14U + 16U * static_cast<std::uint32_t>(ahead.quads_tested));
}

// RouteOpenAheadOf (original 0x00148d60): route_open_ahead for the strip
// object whose handle is a0.
NativeFunctionExecution route_open_ahead_of(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto ahead = route_open_along(call.memory(), call.memory().read_u32_le(call.arg(0) + 4U), call.arg(1),
                                        call.arg(2));
    call.return_value(ahead.open ? 1U : 0U);
    return call.finish(18U + 16U * static_cast<std::uint32_t>(ahead.quads_tested));
}

// Strip navigation. A place on the strips is an object handle (the address
// FindNearestStripQuad gives, 0 for none) and a quad of that object's strip.
// Each quad record names the neighbour across its start side (0) and its
// end side (1).
GuestAddress strip_object_handle(const std::uint8_t object) {
    return kStripObjects + static_cast<GuestAddress>(object) * 8U + kStripObjectHandle;
}
GuestAddress handle_strip(const guest::EeMemory& memory, const GuestAddress handle) {
    return memory.read_u32_le(handle + 4U);
}
bool quad_open(const guest::EeMemory& memory, const GuestAddress handle, const std::uint16_t quad) {
    return route_open_along(memory, handle_strip(memory, handle), quad, 1U).open;
}

struct StripNeighbour {
    GuestAddress handle{};  // 0 for none
    std::uint16_t quad{};
};
StripNeighbour strip_neighbour(const guest::EeMemory& memory, const GuestAddress handle, const std::uint16_t quad,
                               const unsigned side) {
    const GuestAddress record = quad_record(memory, handle_strip(memory, handle), quad);
    const std::uint8_t object = memory.read_u8(record + kQuadRecordNeighbourObject + side);
    if (object == kNoStripObject) {
        return {};
    }
    return {strip_object_handle(object),
            static_cast<std::uint16_t>(memory.read_u16_le(record + kQuadRecordNeighbourQuad + side * 2U))};
}

// The first open quad across `side`, crossing closed ones; 0 (and `quad`
// unchanged) when the side runs out first.
GuestAddress cross_strip_side(const guest::EeMemory& memory, GuestAddress handle, std::uint16_t& quad,
                              const unsigned side, std::uint32_t& work) {
    std::uint16_t at = quad;
    for (;;) {
        const StripNeighbour next = strip_neighbour(memory, handle, at, side);
        if (next.handle == 0U) {
            work += 6U;
            return 0U;
        }
        work += 30U;
        handle = next.handle;
        at = next.quad;
        if (quad_open(memory, handle, at)) {
            quad = at;
            return handle;
        }
    }
}

// CrossStripSide (original 0x00146db0): cross_strip_side from handle a0 and
// the u16 quad at a1 across side a2.
NativeFunctionExecution cross_strip_side_call(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    auto quad = static_cast<std::uint16_t>(memory.read_u16_le(call.arg(1)));
    std::uint32_t work = 32U;
    const GuestAddress handle = cross_strip_side(memory, call.arg(0), quad, call.arg(2), work);
    if (handle != 0U) {
        memory.write_u16_le(call.arg(1), quad);
    }
    call.return_value(handle);
    return call.finish(work);
}

// `delta` quads along the strip: wrapping round a closed strip, or onto the
// first open neighbour of the current quad past either end of an open one
// (whose quad the original leaves as it was). 0 when there is none.
GuestAddress step_along_strip(const guest::EeMemory& memory, const GuestAddress handle, std::uint16_t& quad,
                              const std::int32_t delta) {
    const GuestAddress strip = handle_strip(memory, handle);
    const std::int32_t count = memory.read_u16_le(strip + StripLayout::kQuadCount);
    const std::int32_t next = quad + delta;
    if (next >= 0 && next < count) {
        quad = static_cast<std::uint16_t>(next);
        return handle;
    }
    if (memory.read_u8(strip + StripLayout::kClosed) != 0U) {
        // The processor's remainder of a division by zero is the dividend.
        quad = static_cast<std::uint16_t>(count == 0 ? next + count : (next + count) % count);
        return handle;
    }
    for (unsigned side = 0U; side < 2U; ++side) {
        const StripNeighbour neighbour = strip_neighbour(memory, handle, quad, side);
        if (neighbour.handle != 0U && quad_open(memory, neighbour.handle, neighbour.quad)) {
            return neighbour.handle;
        }
    }
    return 0U;
}

// StepAlongStrip (original 0x00146f20): step_along_strip from handle a0 and
// the u16 quad at a1 by the signed byte a2.
NativeFunctionExecution step_along_strip_call(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    auto quad = static_cast<std::uint16_t>(memory.read_u16_le(call.arg(1)));
    const std::uint16_t before = quad;
    const GuestAddress handle =
        step_along_strip(memory, call.arg(0), quad, static_cast<std::int8_t>(call.arg(2) & 0xffU));
    if (quad != before) {
        memory.write_u16_le(call.arg(1), quad);
    }
    call.return_value(handle);
    return call.finish(handle == call.arg(0) ? 16U : 80U);
}

// The quad containing `point`, walking from `handle` and `quad` toward the
// edges it lies outside of (engine::QuadEdge bits: 4 steps on along the
// strip, 8 back, 1 across the start side, 2 across the end side; a side is
// crossed only onto an open quad), never straight back the way it came and
// for at most 127 steps. Stops where it can go no further.
GuestAddress locate_strip_quad(const guest::EeMemory& memory, GuestAddress handle, std::uint16_t& quad,
                               const engine::Vector3 point, std::uint32_t& work) {
    enum : std::uint32_t { kNone = 16U };
    std::uint32_t came = kNone;
    std::uint32_t edges = strip_quad_outside_edges(memory, handle, quad, point);
    work += 143U;
    for (std::int32_t step = 1; edges != 0U && step < 128; ++step) {
        std::uint32_t move = 0U;
        GuestAddress next = 0U;
        std::uint16_t next_quad = quad;
        if ((edges & engine::kQuadEdgeEnd) != 0U) {
            if (came != engine::kQuadEdgeStart) {
                move = engine::kQuadEdgeEnd;
                next = step_along_strip(memory, handle, next_quad, 1);
            }
        } else if ((edges & engine::kQuadEdgeStart) != 0U) {
            if (came != engine::kQuadEdgeEnd) {
                move = engine::kQuadEdgeStart;
                next = step_along_strip(memory, handle, next_quad, -1);
            }
        } else if ((edges & (engine::kQuadEdgeStartSide | engine::kQuadEdgeEndSide)) != 0U) {
            const bool start_side = (edges & engine::kQuadEdgeStartSide) != 0U;
            if (came != (start_side ? engine::kQuadEdgeEndSide : engine::kQuadEdgeStartSide)) {
                move = start_side ? engine::kQuadEdgeStartSide : engine::kQuadEdgeEndSide;
                const StripNeighbour neighbour = strip_neighbour(memory, handle, quad, start_side ? 0U : 1U);
                if (neighbour.handle != 0U && quad_open(memory, neighbour.handle, neighbour.quad)) {
                    next = neighbour.handle;
                    next_quad = neighbour.quad;
                }
            }
        } else {
            move = came;  // no edge it can act on: look again
            next = handle;
        }
        if (next == 0U) {
            break;
        }
        handle = next;
        quad = next_quad;
        came = move;
        edges = strip_quad_outside_edges(memory, handle, quad, point);
        work += 183U;
    }
    return handle;
}

// LocateStripQuad (original 0x001461d0): locate_strip_quad from handle a0 and
// the u16 quad at a1 (which gets the quad) of the point at a2.
NativeFunctionExecution locate_strip_quad_call(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    auto quad = static_cast<std::uint16_t>(memory.read_u16_le(call.arg(1)));
    std::uint32_t work = 30U;
    const GuestAddress handle =
        locate_strip_quad(memory, call.arg(0), quad, load_vector4(memory, call.arg(2) & ~15U).xyz(), work);
    memory.write_u16_le(call.arg(1), quad);
    call.return_value(handle);
    return call.finish(work);
}

// StripQuadEdgesBeyond (original 0x00148690): engine::quad_edges_beyond of
// the point at a1 for quad a2 of the strip of handle a0, margin f12.
NativeFunctionExecution strip_quad_edges_beyond(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const engine::StripQuad quad = load_strip_quad(memory, call.arg(0), call.arg(2));
    call.return_value(engine::quad_edges_beyond(quad, load_vector4(memory, call.arg(1) & ~15U).xyz(),
                                                load_vector4(memory, kStripUp).xyz(), call.float_arg(0)));
    return call.finish(220U);
}

// A strip side's outward direction (not unit length): across the strip's up
// axis from the side's edge (start side: corner 0 from corner 2; end side:
// corner 3 from corner 1). The fourth lane is the up axis's.
engine::Vector4 strip_side_normal(const guest::EeMemory& memory, const GuestAddress handle,
                                  const std::uint32_t quad, const unsigned side) {
    const engine::StripQuad corners = load_strip_quad(memory, handle, quad);
    const engine::Vector4 up = load_vector4(memory, kStripUp);
    const engine::Vector3 edge = side == 0U ? engine::chop_sub(corners.corners[0], corners.corners[2])
                                            : engine::chop_sub(corners.corners[3], corners.corners[1]);
    const engine::Vector3 normal = engine::chop_cross(edge, up.xyz());
    return {normal.x, normal.y, normal.z, up.w};
}

// StripStartSideNormal / StripEndSideNormal (originals 0x00147f50 and
// 0x00147ee0): strip_side_normal of quad a1 of handle a0 to a2; returns 1.
template <unsigned Side>
NativeFunctionExecution strip_side_normal_call(NativeFunctionContext& context) {
    GuestCall call{context};
    store_vector4(call.memory(), call.arg(2) & ~15U, strip_side_normal(call.memory(), call.arg(0), call.arg(1), Side));
    call.return_value(1U);
    return call.finish(27U);
}

// An AI racer's road-following state (in its AI driver record).
struct RacerFollowLayout {
    static constexpr GuestAddress kFrame = 0U;            // rows right, up, at, position
    static constexpr GuestAddress kStripHandle = 6324U;
    static constexpr GuestAddress kStripQuad = 6328U;     // u16
    static constexpr GuestAddress kHeading = 6576U;
    static constexpr GuestAddress kPreviousHeading = 6592U;
    static constexpr GuestAddress kRouteHeading = 8608U;
    static constexpr GuestAddress kTargetSpeed = 9140U;
    static constexpr GuestAddress kCar = 9264U;           // pointer
    static constexpr GuestAddress kWidth = 9268U;
    static constexpr GuestAddress kRideHeight = 9276U;
};
constexpr GuestAddress kRacerStepTime = 0x0052267cU;
constexpr GuestAddress kRacerSlowStep = 0x01e754e0U;
constexpr GuestAddress kRacerTurnLimit = 0x01e754e4U;   // degrees per frame
constexpr GuestAddress kDegreesPerRadian = 0x004e069cU;
constexpr GuestAddress kRadiansPerDegree = 0x004e0d60U;
constexpr GuestAddress kCarSpeedUpRate = 5076U;           // in the car; the step is this / 970
constexpr GuestAddress kCarFirstWheelRadius = 2160U;
constexpr GuestAddress kCarVelocityLanes = 176U;          // w is the speed
// Stack words below the caller's stack pointer that the original's frame
// (6864 bytes) reads without writing them first, as fourth lanes of vectors
// whose other lanes it fills. The undecided rebuild stores the first as the
// forward row's fourth lane; a limited turn computes its sine and cosine four
// at once, and the heading's fourth lane is the folded fourth lane of that
// quad squared, from the second word (turning by -limit) or the third.
constexpr GuestAddress kStaleLaneBelowEntry = 1124U;
constexpr GuestAddress kStaleTurnLaneNegative = 5860U;
constexpr GuestAddress kStaleTurnLanePositive = 6324U;

// RacerFollowStrip (original 0x001423e0): moves the AI racer car of
// the driver record a0 one frame along the road strip and sets its speed.
NativeFunctionExecution racer_follow_strip(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress record = call.arg(0);
    const auto at = [&](const GuestAddress offset) { return record + offset; };
    const float stale_lane = load_float(memory, call.stack_pointer() - kStaleLaneBelowEntry);
    const GuestAddress handle = memory.read_u32_le(at(RacerFollowLayout::kStripHandle));
    const auto quad = static_cast<std::uint16_t>(memory.read_u16_le(at(RacerFollowLayout::kStripQuad)));
    const engine::Vector4 up = load_vector4(memory, kStripUp);
    std::uint32_t work = 120U;

    // Turn toward the route's heading (straight to it from a standstill).
    const engine::Vector4 previous = load_vector4(memory, at(RacerFollowLayout::kHeading));
    store_vector4(memory, at(RacerFollowLayout::kPreviousHeading), previous);
    const engine::Vector4 route = load_vector4(memory, at(RacerFollowLayout::kRouteHeading));
    engine::Vector4 heading = route;
    const std::uint32_t heading_bits = std::bit_cast<std::uint32_t>(previous.x) |
                                       std::bit_cast<std::uint32_t>(previous.y) |
                                       std::bit_cast<std::uint32_t>(previous.z);
    if ((heading_bits & 0x7fffffffU) != 0U) {
        const engine::HeadingTurnTuning tuning{load_float(memory, kRacerTurnLimit),
                                               load_float(memory, kDegreesPerRadian),
                                               load_float(memory, kRadiansPerDegree)};
        const auto turn = engine::turn_heading_toward(previous.xyz(), route.xyz(), up.xyz(), tuning);
        work += turn.limited ? 700U : 160U;
        if (turn.limited) {
            const float stale = engine::game_operand(load_float(
                memory, call.stack_pointer() - (turn.positive_turn ? kStaleTurnLanePositive : kStaleTurnLaneNegative)));
            const float folded = engine::wave_fold_chopped(stale);
            heading = {turn.heading.x, turn.heading.y, turn.heading.z, engine::chop_mul(folded, folded)};
        }
    }
    store_vector4(memory, at(RacerFollowLayout::kHeading), heading);

    // Stand on the road along the heading, tilted with the strip, then move.
    const engine::StripQuad entry = load_strip_quad(memory, handle, quad);
    const engine::Vector3 along = engine::chop_sub(entry.corners[2], entry.corners[0]);
    const float rise = engine::chop_mul(along.y, engine::chop_reciprocal_sqrt(engine::chop_dot(along, along)));
    const GuestAddress car = memory.read_u32_le(at(RacerFollowLayout::kCar));
    engine::Vector4 position = load_vector4(memory, at(RacerFollowLayout::kFrame + 48U));
    position.y = engine::chop_add(engine::chop_sub(position.y, load_float(memory, at(RacerFollowLayout::kRideHeight))),
                                  load_float(memory, car + kCarFirstWheelRadius));
    const engine::RotationBasis frame = engine::road_frame(heading.xyz(), rise, up.xyz());
    const float step = engine::chop_mul(load_float(memory, car + kCarVelocityLanes + 12U),
                                        load_float(memory, kRacerStepTime));
    const engine::Vector3 moved = engine::chop_add(position.xyz(), engine::chop_mul(heading.xyz(), step));
    position = {moved.x, moved.y, moved.z, position.w};

    // Find the quad the car is now on (from the next open one across a side
    // when its own is closed) and keep it off sides it may not cross.
    GuestAddress from = handle;
    std::uint16_t place = quad;
    if (!route_open_along(memory, handle_strip(memory, handle), quad, 1U).open) {
        from = cross_strip_side(memory, handle, place, 0U, work);
        if (from == 0U) {
            place = quad;
            from = cross_strip_side(memory, handle, place, 1U, work);
            if (from == 0U) {
                from = handle;
            }
        }
    }
    const GuestAddress located = locate_strip_quad(memory, from, place, position.xyz(), work);
    const float width = load_float(memory, at(RacerFollowLayout::kWidth));
    const std::uint32_t beyond = engine::quad_edges_beyond(load_strip_quad(memory, located, place), position.xyz(),
                                                           up.xyz(), engine::chop_mul(-0.5F, width));
    const auto side_closed = [&](const unsigned side) {
        const StripNeighbour neighbour = strip_neighbour(memory, located, place, side);
        work += 40U;
        return neighbour.handle == 0U ||
               !route_open_along(memory, handle_strip(memory, neighbour.handle), neighbour.quad, 1U).open;
    };
    const auto keep_inside = [&](const unsigned side) {
        const engine::StripQuad corners = load_strip_quad(memory, located, place);
        const engine::Vector3 inside =
            engine::kept_inside_side(position.xyz(), strip_side_normal(memory, located, place, side).xyz(),
                                     corners.corners[side == 0U ? 0U : 1U], width);
        position = {inside.x, inside.y, inside.z, position.w};
        work += 90U;
    };
    if ((beyond & engine::kQuadEdgeStartSide) != 0U && side_closed(0U)) {
        keep_inside(0U);
    } else if ((beyond & engine::kQuadEdgeEndSide) != 0U && side_closed(1U)) {
        keep_inside(1U);
    }

    // Square the rotation up and place the car. Every row's fourth lane is
    // the heading's; the rebuilt rows take another row's, as the original's
    // vector moves do.
    const engine::Orthonormalized square = engine::orthonormalized(frame);
    std::array<engine::Vector4, 4> rows{};
    const float w = heading.w;
    float right_w = w;
    float up_w = w;
    float at_w = w;
    switch (square.kept) {
    case engine::KeptRow::Up:
    case engine::KeptRow::At:
    case engine::KeptRow::Right:
        break;  // all the same lane
    case engine::KeptRow::RightUndecided:
        at_w = stale_lane;
        break;
    }
    rows[0] = {square.basis.right.x, square.basis.right.y, square.basis.right.z, right_w};
    rows[1] = {square.basis.up.x, square.basis.up.y, square.basis.up.z, up_w};
    rows[2] = {square.basis.at.x, square.basis.at.y, square.basis.at.z, at_w};
    rows[3] = position;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        store_vector4(memory, at(RacerFollowLayout::kFrame + row * 16U), rows[row]);
    }
    work += 500U + set_car_transform(memory, car, rows);

    // Speed toward the target, limited per frame.
    const float speed = load_float(memory, car + kCarVelocityLanes + 12U);
    const float target = load_float(memory, at(RacerFollowLayout::kTargetSpeed));
    const float speed_up = engine::game_div(load_float(memory, car + kCarSpeedUpRate), 970.0F);
    const float set = engine::follow_speed(speed, target, load_float(memory, kRacerSlowStep), speed_up);
    work += 20U + set_car_speed(memory, car, set, engine::chop_sub(speed, target));
    return call.finish(work);
}

// A driver's rules for entering quads (an AI driver record + 6640).
struct QuadRulesLayout {
    static constexpr GuestAddress kRace = 4U;          // pointer; its +6420 is nonzero to apply the rules
    static constexpr GuestAddress kSideKindsAllowed = 496U;   // u8
    static constexpr GuestAddress kRestrictedAllowed = 497U;  // u8
    static constexpr GuestAddress kSideKindsActive = 508U;    // u32
    static constexpr GuestAddress kRestrictedDiscouragedOnly = 657U;  // u8
    static constexpr GuestAddress kSideKindsDiscouragedOnly = 658U;   // u8
};
constexpr GuestAddress kRaceAppliesQuadRules = 6420U;

std::uint32_t quad_entry_for(const guest::EeMemory& memory, const GuestAddress rules, const GuestAddress strip,
                             const std::uint32_t quad) {
    const std::uint8_t kind = quad_kind(memory, strip, quad);
    if (kind == engine::kQuadKindClosed) {
        return engine::kQuadEntryClosed;
    }
    engine::QuadRules quad_rules;
    quad_rules.apply =
        memory.read_u32_le(memory.read_u32_le(rules + QuadRulesLayout::kRace) + kRaceAppliesQuadRules) != 0U;
    if (quad_rules.apply) {
        quad_rules.restricted_allowed = memory.read_u8(rules + QuadRulesLayout::kRestrictedAllowed) != 0U;
        quad_rules.restricted_discouraged_only =
            memory.read_u8(rules + QuadRulesLayout::kRestrictedDiscouragedOnly) != 0U;
        quad_rules.side_kinds_allowed = memory.read_u8(rules + QuadRulesLayout::kSideKindsAllowed) != 0U;
        quad_rules.side_kinds_active = memory.read_u32_le(rules + QuadRulesLayout::kSideKindsActive) != 0U;
        quad_rules.side_kinds_discouraged_only =
            memory.read_u8(rules + QuadRulesLayout::kSideKindsDiscouragedOnly) != 0U;
    }
    return engine::quad_entry(kind, quad_rules);
}

// QuadEntryForDriver (original 0x0014a480): the engine::QuadEntry bits of
// quad a2 of the strip object whose handle is at a1, under the rules at a0.
NativeFunctionExecution quad_entry_for_driver(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    call.return_value(quad_entry_for(memory, call.arg(0), memory.read_u32_le(call.arg(1) + 4U), call.arg(2)));
    return call.finish(22U);
}

// The vehicle fields the route spline uses.
struct RouteSplineLayout {
    static constexpr GuestAddress kDriver = 340U;     // pointer
    static constexpr GuestAddress kSecondSet = 336U;  // u8: control points at +96 instead of +0
};
constexpr GuestAddress kSecondControlPoints = 96U;
constexpr GuestAddress kDriverProgress = 7120U;   // float, 0 to 1 along the segment
constexpr GuestAddress kDriverQuadRules = 6640U;  // the QuadRulesLayout record
constexpr GuestAddress kRouteSplineAhead = 0x004b5e70U;   // engine::CubicBasis
constexpr GuestAddress kRouteSplineWindow = 0x004b5eb0U;  // engine::CubicBasis
constexpr std::uint32_t kRouteControlPoints = 6U;

engine::CubicBasis load_cubic_basis(const guest::EeMemory& memory, const GuestAddress at) {
    return {load_vector4(memory, at), load_vector4(memory, at + 16U), load_vector4(memory, at + 32U),
            load_vector4(memory, at + 48U)};
}

// NeighbourQuadEntry (original 0x00149c90): for the point at a1 in quad a3 of
// strip object a2 (its handle), the point is copied to t0 when it lies
// within the quad's sides; otherwise the quads across the side it lies
// beyond are walked until one the driver of vehicle a0 may enter (by
// RouteOpenAhead one quad, then QuadEntryForDriver), whose entry bits are
// returned.
NativeFunctionExecution neighbour_quad_entry(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress point = call.arg(1) & ~15U;
    const std::uint32_t outside = strip_quad_outside_edges(memory, call.arg(2), call.arg(3),
                                                          load_vector4(memory, point).xyz());
    if ((outside & (engine::kQuadEdgeStartSide | engine::kQuadEdgeEndSide)) == 0U) {
        copy_quadword(memory, call.arg(4) & ~15U, point);
        call.return_value(outside);
        return call.finish(140U);
    }
    const GuestAddress side = (outside & engine::kQuadEdgeStartSide) != 0U ? 0U : 1U;
    const GuestAddress rules = memory.read_u32_le(call.arg(0) + RouteSplineLayout::kDriver) + kDriverQuadRules;
    GuestAddress record = quad_record(memory, memory.read_u32_le(call.arg(2) + 4U), call.arg(3));
    std::uint32_t cost = 150U;
    for (;;) {
        const std::uint8_t object = memory.read_u8(record + kQuadRecordNeighbourObject + side);
        const std::uint16_t quad = memory.read_u16_le(record + kQuadRecordNeighbourQuad + side * 2U);
        cost += 45U;
        if (object != kNoStripObject) {
            const auto ahead = strip_route_open_ahead(memory, object, quad, 1U);
            cost += 16U * static_cast<std::uint32_t>(ahead.quads_tested);
            if (ahead.open) {
                const std::uint32_t entry = quad_entry_for(memory, rules, strip_object_strip(memory, object), quad);
                if ((entry & engine::kQuadEntryClosed) == 0U) {
                    call.return_value(entry);
                    return call.finish(cost + 22U);
                }
            }
        }
        record = quad_record(memory, strip_object_strip(memory, object), quad);
    }
}

// PlaceRouteSplinePoints (original 0x00149180): puts the vehicle a0's six
// route control points (at +0, or +96 when the second set is in use) at the
// fractions a3 (floats) along the edges a1 (u32 quad indices) of strip
// object a2, then evaluates the route spline at the driver's progress: t3
// gets the point on control points 1 to 4 by the ahead basis, and t0, t1
// and t2 those on points 0 to 3, 1 to 4 and 2 to 5 by the window basis. Near
// the end of an open strip the later window points repeat earlier ones.
// (The original also calls NeighbourQuadEntry for each point, which only
// reads memory and copies the point onto itself, so it is left out.)
NativeFunctionExecution place_route_spline_points(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress vehicle = call.arg(0);
    const GuestAddress edges = call.arg(1);
    const GuestAddress object = call.arg(2);
    const GuestAddress fractions = call.arg(3);
    const GuestAddress control = vehicle + (memory.read_u8(vehicle + RouteSplineLayout::kSecondSet) != 0U
                                                ? kSecondControlPoints
                                                : 0U);
    const GuestAddress indices = memory.read_u32_le(memory.read_u32_le(object + 4U));
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    std::array<engine::Vector4, kRouteControlPoints> points;
    for (std::uint32_t i = 0U; i < kRouteControlPoints; ++i) {
        const std::uint16_t edge = memory.read_u16_le(edges + i * 4U);
        const float distance =
            load_float(memory, fractions + i * 4U) * strip_edge_horizontal_length(memory, object, edge);
        const std::uint16_t from_index = memory.read_u16_le(indices + edge * 4U);
        const std::uint16_t to_index = memory.read_u16_le(indices + edge * 4U + 2U);
        const engine::Vector4 from = load_vector4(memory, vertices + from_index * 16U);
        if (from_index == to_index) {
            points[i] = from;
        } else {
            const engine::Vector3 along = load_vector4(memory, vertices + to_index * 16U).xyz() - from.xyz();
            const float length_squared = (along.x * along.x + along.y * along.y) + along.z * along.z;
            const engine::Vector3 placed = (along * (1.0F / std::sqrt(length_squared))) * distance + from.xyz();
            points[i] = {placed.x, placed.y, placed.z, from.w};
        }
        store_vector4(memory, control + i * 16U, points[i]);
    }
    const GuestAddress driver = memory.read_u32_le(vehicle + RouteSplineLayout::kDriver);
    const float progress =
        engine::minimum(engine::maximum(load_float(memory, driver + kDriverProgress), 0.0F), 1.0F);
    const auto spline_point = [&](const engine::CubicBasis& basis, const std::uint32_t first,
                                  const std::uint32_t w_from) {
        const engine::Vector3 point = engine::cubic_point(
            {points[first].xyz(), points[first + 1U].xyz(), points[first + 2U].xyz(), points[first + 3U].xyz()},
            engine::cubic_weights(basis, progress));
        return engine::Vector4{point.x, point.y, point.z, points[w_from].w};
    };
    const GuestAddress window[3] = {call.arg(4), call.arg(5), call.arg(6)};
    const GuestAddress ahead = call.arg(7);
    if (ahead != 0U) {
        store_vector4(memory, ahead, spline_point(load_cubic_basis(memory, kRouteSplineAhead), 1U, 3U));
    }
    const engine::CubicBasis window_basis = load_cubic_basis(memory, kRouteSplineWindow);
    for (std::uint32_t k = 0U; k < 3U; ++k) {
        if (window[k] != 0U) {
            store_vector4(memory, window[k], spline_point(window_basis, k, k + 2U));
        }
    }
    const GuestAddress strip = memory.read_u32_le(object + 4U);
    if (memory.read_u8(strip + StripLayout::kClosed) == 0U) {
        const std::int32_t quads = memory.read_u16_le(strip + StripLayout::kQuadCount);
        const auto second = static_cast<std::int32_t>(memory.read_u32_le(edges + 4U));
        if (second == quads - 3) {
            if (window[2] != 0U) {
                if (window[1] != 0U) {
                    copy_quadword(memory, window[2], window[1]);
                } else if (window[0] != 0U) {
                    copy_quadword(memory, window[2], window[0]);
                }
            }
        } else if (second == quads - 2 || second == quads - 1) {
            for (const std::uint32_t k : {2U, 1U}) {
                if (window[k] != 0U && window[0] != 0U) {
                    copy_quadword(memory, window[k], window[0]);
                }
            }
        }
    }
    return call.finish(1800U);
}

// The owner of a lane bin profile.
struct LaneProfileLayout {
    static constexpr GuestAddress kOrigin = 16U;
    static constexpr GuestAddress kDirection = 48U;
    static constexpr GuestAddress kTags = 64U;        // u8[256]
    static constexpr GuestAddress kDistances = 320U;  // u8[256]
    static constexpr GuestAddress kLevels = 576U;     // s16[256]
    static constexpr GuestAddress kCarWidth = 1088U;  // u8, in bins
    static constexpr GuestAddress kCarHigh = 1089U;   // u8, the car's highest bin
    static constexpr GuestAddress kCarLow = 1090U;    // u8, the car's lowest bin
    static constexpr GuestAddress kGapEnd = 1092U;
    static constexpr GuestAddress kCentre = 1100U;
    static constexpr GuestAddress kBlendSource = 1108U;  // pointer; its +6332 is the blend
    // The steering target the profile chooses.
    static constexpr GuestAddress kTarget = 1120U;
    static constexpr GuestAddress kTargetDirection = 1136U;
    static constexpr GuestAddress kTargetTime = 1156U;  // distance over speed
    static constexpr GuestAddress kTargetSpeed = 1160U;
    static constexpr GuestAddress kDanger = 1164U;
    static constexpr GuestAddress kHighClearance = 1168U;
    static constexpr GuestAddress kLowClearance = 1172U;
    static constexpr GuestAddress kHighBlocked = 1176U;
    static constexpr GuestAddress kLowBlocked = 1180U;
    static constexpr GuestAddress kRacer = 1184U;       // pointer
    static constexpr GuestAddress kDriver = 1188U;      // pointer
    static constexpr GuestAddress kTargetSide = 1196U;  // u8: 1 high, 2 low, 16 open road
    static constexpr GuestAddress kLowTargetBin = 1197U;
    static constexpr GuestAddress kHighTargetBin = 1198U;
};
constexpr GuestAddress kBlendOffset = 6332U;
constexpr GuestAddress kLaneDistanceUnit = 0x004e0e98U;  // 8/255
constexpr GuestAddress kLaneLevelUnit = 0x004e0e9cU;     // 500/32767
constexpr GuestAddress kLaneBinWidth = 0x004e0e00U;           // 0.2
constexpr GuestAddress kLaneGapLeastScore = 0x004e0ea8U;

class GuestLaneBins {
  public:
    GuestLaneBins(guest::EeMemory& memory, const GuestAddress profile) : memory_(memory), profile_(profile) {}
    [[nodiscard]] std::uint8_t distance(const std::int32_t bin) const {
        return memory_.read_u8(at(LaneProfileLayout::kDistances, bin, 1));
    }
    void set_distance(const std::int32_t bin, const std::uint8_t value, const std::uint8_t tag) {
        memory_.write_u8(at(LaneProfileLayout::kDistances, bin, 1), value);
        memory_.write_u8(at(LaneProfileLayout::kTags, bin, 1), tag);
    }
    [[nodiscard]] std::int8_t tag(const std::int32_t bin) const {
        return static_cast<std::int8_t>(memory_.read_u8(at(LaneProfileLayout::kTags, bin, 1)));
    }
    [[nodiscard]] std::int16_t level(const std::int32_t bin) const {
        return static_cast<std::int16_t>(memory_.read_u16_le(at(LaneProfileLayout::kLevels, bin, 2)));
    }
    void set_level(const std::int32_t bin, const std::int16_t value) {
        memory_.write_u16_le(at(LaneProfileLayout::kLevels, bin, 2), static_cast<std::uint16_t>(value));
    }

  private:
    [[nodiscard]] GuestAddress at(const GuestAddress array, const std::int32_t bin, const int size) const {
        return profile_ + array + static_cast<GuestAddress>(bin * size);
    }
    guest::EeMemory& memory_;
    GuestAddress profile_;
};

// MarkStripQuadInLaneProfile (original 0x0013e530): projects quad a2 of the
// strip of object a1 onto the lane profile a0 (points blended along the
// quad's sides by the profile's blend) and marks the covered bins with
// distance f12, level f13 and tag a3.
NativeFunctionExecution mark_strip_quad_in_lane_profile(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress profile = call.arg(0);
    const GuestAddress indices = memory.read_u32_le(memory.read_u32_le(call.arg(1) + 4U)) + (call.arg(2) & 0xffffU) * 4U;
    const GuestAddress vertices = memory.read_u32_le(kStripVertices);
    const auto vertex = [&](const GuestAddress slot) {
        return load_vector4(memory, vertices + memory.read_u16_le(indices + slot * 2U) * 16U).xyz();
    };
    const float blend = engine::maximum(
        engine::minimum(load_float(memory, memory.read_u32_le(profile + LaneProfileLayout::kBlendSource) + kBlendOffset), 1.0F),
        0.0F);
    const auto lerp = [&](const engine::Vector3 from, const engine::Vector3 to) {
        return (from - from * blend) + to * blend;
    };
    const engine::LaneBinAxis axis{load_vector4(memory, profile + LaneProfileLayout::kOrigin).xyz(),
                                   load_vector4(memory, profile + LaneProfileLayout::kDirection).xyz(),
                                   load_float(memory, profile + LaneProfileLayout::kCentre)};
    const std::int32_t high = engine::lane_bin(axis, lerp(vertex(0U), vertex(2U)));
    const std::int32_t low = engine::lane_bin(axis, lerp(vertex(1U), vertex(3U)));
    const engine::LaneMark mark{call.float_arg(0), call.float_arg(1), static_cast<std::uint8_t>(call.arg(3)),
                                load_float(memory, kLaneDistanceUnit), load_float(memory, kLaneLevelUnit)};
    GuestLaneBins bins{memory, profile};
    engine::mark_lane_bins(bins, low, high, mark);
    const std::int32_t span = high >= low ? high - low + 1 : 0;
    return call.finish(120U + 20U * static_cast<std::uint32_t>(span > 0 ? span : 0));
}

std::uint32_t best_lane_gap_cost(const std::int32_t low, const std::int32_t high) {
    return 30U + 42U * static_cast<std::uint32_t>(high - low > 6 ? high - low - 6 : 0);
}

// BestLaneGap (original 0x0013e790): the most open bin of lane profile a0
// between the bins of its offsets 0 and +1092 (relative to its centre).
NativeFunctionExecution best_lane_gap(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress profile = call.arg(0);
    const float centre = load_float(memory, profile + LaneProfileLayout::kCentre);
    const std::int32_t low = engine::clamped_lane_bin(-centre);
    const std::int32_t high = engine::clamped_lane_bin(
        engine::chop_sub(load_float(memory, profile + LaneProfileLayout::kGapEnd), centre));
    const engine::LaneGapWeights weights{load_float(memory, kLaneBinWidth), load_float(memory, kLaneDistanceUnit),
                                         load_float(memory, kLaneGapLeastScore)};
    const GuestLaneBins bins{memory, profile};
    const std::int32_t gap = engine::best_lane_gap(bins, low, high, weights);
    call.return_value(static_cast<std::uint32_t>(gap));
    return call.finish(best_lane_gap_cost(low, high));
}

// The driver and car fields ChooseLaneTarget reads.
struct LaneDriverLayout {
    static constexpr GuestAddress kForward = 6848U;
    static constexpr GuestAddress kSide = 6864U;
    static constexpr GuestAddress kSideRequest = 7140U;  // 2 high, 4 low
    static constexpr GuestAddress kSideRequestState = 7144U;
    static constexpr GuestAddress kSideRequestHeld = 7148U;
    static constexpr GuestAddress kCar = 9264U;  // pointer
};
constexpr GuestAddress kCarVelocity = 176U;  // w is the speed
constexpr GuestAddress kCarMatrix = 516U;    // pointer
constexpr GuestAddress kMatrixAt = 32U;
constexpr GuestAddress kMatrixPosition = 48U;
constexpr GuestAddress kRacerCruiseSpeed = 464U;
constexpr GuestAddress kRacerHolding = 504U;
constexpr GuestAddress kRacerOverride = 508U;
constexpr GuestAddress kLaneHeightSpeeds = 0x01e7553cU;   // levels below 10, 20 and 30
constexpr GuestAddress kLaneSqueezeSpeeds = 0x01e75548U;  // squeezed, lightly squeezed
constexpr GuestAddress kLaneLeadLevelScale = 0x01e75550U;
constexpr GuestAddress kLaneSqueezeFree = 0x004e0ea4U;    // 0.9
constexpr GuestAddress kLaneSqueezeLight = 0x004e0978U;

engine::Vector4 chopped_scaled(const engine::Vector4 v, const float s) {
    return {engine::chop_mul(v.x, s), engine::chop_mul(v.y, s), engine::chop_mul(v.z, s), v.w};
}
engine::Vector4 chopped_sum(const engine::Vector4 a, const engine::Vector4 b, const float w) {
    return {engine::chop_add(a.x, b.x), engine::chop_add(a.y, b.y), engine::chop_add(a.z, b.z), w};
}
float chopped_length_squared(const engine::Vector4 v) {
    return engine::chop_add(engine::chop_add(engine::chop_mul(v.x, v.x), engine::chop_mul(v.y, v.y)),
                            engine::chop_mul(v.z, v.z));
}

// ChooseLaneTarget (original 0x00140a30): the steering target of the lane
// profile a0. With something within 4 of the car across its bins it takes
// the clearer of the escapes up and down the profile; otherwise it heads for
// the best gap ahead. Writes the target point, its direction and time away,
// the speed to hold (lower near drops and when squeezed) and the clearances
// found on each side.
NativeFunctionExecution choose_lane_target(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    using L = LaneProfileLayout;
    const GuestAddress profile = call.arg(0);
    const GuestAddress driver = memory.read_u32_le(profile + L::kDriver);
    const GuestAddress racer = memory.read_u32_le(profile + L::kRacer);
    const GuestAddress car = memory.read_u32_le(driver + LaneDriverLayout::kCar);
    const GuestAddress matrix = memory.read_u32_le(car + kCarMatrix);
    std::uint32_t side_request = 0U;
    if (memory.read_u32_le(driver + LaneDriverLayout::kSideRequestHeld) == 0U &&
        memory.read_u32_le(driver + LaneDriverLayout::kSideRequestState) - 1U < 2U) {
        side_request = memory.read_u32_le(driver + LaneDriverLayout::kSideRequest);
    }
    store_float(memory, profile + L::kLowClearance, 0.0F);
    store_float(memory, profile + L::kHighClearance, 0.0F);
    memory.write_u8(profile + L::kTargetSide, 16U);
    const float unit = load_float(memory, kLaneDistanceUnit);
    const float level_unit = load_float(memory, kLaneLevelUnit);
    const float bin_width = load_float(memory, kLaneBinWidth);
    const GuestLaneBins bins{memory, profile};
    const auto distance = [&](const std::int32_t bin) {
        return engine::chop_mul(unit, engine::chop_from_int(bins.distance(bin)));
    };
    const auto level = [&](const std::int32_t bin) {
        return engine::chop_mul(level_unit, engine::chop_from_int(bins.level(bin)));
    };
    const std::int32_t width = memory.read_u8(profile + L::kCarWidth);
    const std::int32_t high = memory.read_u8(profile + L::kCarHigh);
    const std::int32_t low = memory.read_u8(profile + L::kCarLow);
    float nearest = distance(low);
    float lowest = level(low);
    for (std::int32_t bin = low + 1; bin <= high; ++bin) {
        nearest = engine::minimum(nearest, distance(bin));
        lowest = engine::minimum(lowest, level(bin));
    }
    // The lowest level across the car's middle half, leaving out bins of tag
    // 6 unless the danger is under 50.
    const auto middle_level = [&] {
        const std::int32_t quarter = (high - low) / 4;
        const bool skip_tagged = !engine::game_less(load_float(memory, profile + L::kDanger), 50.0F);
        float result = 500.0F;
        for (std::int32_t bin = low + quarter; bin < high - quarter; ++bin) {
            if (!skip_tagged || bins.tag(bin) != 6) {
                result = engine::minimum(result, level(bin));
            }
        }
        return result;
    };
    // The speed for a drop beside the car, if it is deep enough to matter.
    const auto height_speed = [&](const float drop, GuestAddress& speed_source) {
        for (std::uint32_t tier = 0U; tier < 3U; ++tier) {
            if (engine::game_less(drop, 10.0F * static_cast<float>(tier + 1U))) {
                speed_source = kLaneHeightSpeeds + tier * 4U;
                return true;
            }
        }
        return false;
    };
    const engine::Vector4 side = load_vector4(memory, driver + LaneDriverLayout::kSide);
    const engine::Vector4 forward = load_vector4(memory, driver + LaneDriverLayout::kForward);
    const engine::Vector4 position = load_vector4(memory, matrix + kMatrixPosition);
    const float speed = load_float(memory, car + kCarVelocity + 12U);
    std::uint32_t cost = 0U;
    GuestAddress speed_source = racer + kRacerCruiseSpeed;
    std::int32_t high_target = 0;
    std::int32_t low_target = 0;
    if (engine::game_less(nearest, 4.0F)) {
        std::uint32_t steps = 0U;
        high_target = engine::lane_escape_bin(bins, low, high, width, 1, nearest, unit, bin_width, steps);
        low_target = engine::lane_escape_bin(bins, low, high, width, -1, nearest, unit, bin_width, steps);
        // The mean openness of a car width from `from`.
        const auto clearance = [&](const std::int32_t from, const int step) {
            float sum = 0.0F;
            for (std::int32_t k = 0; k < width; ++k) {
                sum = engine::chop_add(sum, engine::chop_sub(8.0F, distance(from + step * k)));
            }
            return engine::game_div(sum, engine::chop_from_int(width));
        };
        // How much is within 2 on the way from `from` to `to`.
        const auto blocked = [&](std::int32_t from, const std::int32_t to, const int step) {
            float sum = 0.0F;
            for (; step > 0 ? from <= to : from >= to; from += step) {
                const float here = distance(from);
                if (engine::game_less(here, 2.0F)) {
                    sum = engine::chop_add(sum, engine::chop_sub(8.0F, here));
                }
            }
            return sum;
        };
        const float high_clearance = clearance(high_target, -1);
        const float high_blocked = blocked(low, high_target, 1);
        const float low_clearance = clearance(low_target, 1);
        const float low_blocked = blocked(high, low_target, -1);
        store_float(memory, profile + L::kHighClearance, high_clearance);
        store_float(memory, profile + L::kHighBlocked, high_blocked);
        store_float(memory, profile + L::kLowClearance, low_clearance);
        store_float(memory, profile + L::kLowBlocked, low_blocked);
        // How hemmed in the car is by what is beside it (tags 2 to 4).
        float squeeze = 0.0F;
        for (std::int32_t k = 0; k < width; ++k) {
            const std::int8_t tag = bins.tag(low + k);
            if (tag >= 2 && tag < 5) {
                squeeze = engine::chop_add(squeeze, engine::chop_sub(8.0F, distance(low + k)));
            }
        }
        squeeze = engine::game_div(squeeze, engine::chop_mul(8.0F, engine::chop_from_int(width)));
        const bool high_open = engine::game_equal(8.0F, high_clearance);
        const bool low_open = engine::game_equal(8.0F, low_clearance);
        bool go_high;
        if (!low_open && (side_request == 4U || high_open)) {
            go_high = false;
        } else if (high_target == high) {
            go_high = false;
        } else if (!high_open && (side_request == 2U || low_open)) {
            go_high = true;
        } else if (low_target == low) {
            go_high = true;
        } else {
            go_high = engine::game_less(engine::chop_add(high_clearance, high_blocked),
                                        engine::chop_add(low_clearance, low_blocked));
        }
        const float offset =
            engine::chop_mul(bin_width, engine::chop_from_int(go_high ? high_target - high : low_target - low));
        memory.write_u8(profile + L::kTargetSide, go_high ? 1U : 2U);
        const float lead = engine::maximum(
            5.0F, engine::chop_mul(2.0F, engine::game_div(lowest, load_float(memory, kLaneLeadLevelScale))));
        const engine::Vector4 ahead = chopped_sum(chopped_scaled(forward, lead), position, position.w);
        store_vector4(memory, profile + L::kTarget, chopped_sum(chopped_scaled(side, offset), ahead, side.w));
        if (!height_speed(middle_level(), speed_source)) {
            if (!engine::game_less(load_float(memory, kLaneSqueezeFree), squeeze)) {
                speed_source = racer + kRacerCruiseSpeed;
            } else if (!engine::game_less(load_float(memory, kLaneSqueezeLight), squeeze)) {
                speed_source = kLaneSqueezeSpeeds + 4U;
            } else {
                speed_source = kLaneSqueezeSpeeds;
            }
        }
        const auto spanned = [](const std::int32_t from, const std::int32_t to) {
            return static_cast<std::uint32_t>(to >= from ? to - from + 1 : 0);
        };
        cost = 290U + 18U * steps + 68U * static_cast<std::uint32_t>(width) +
               24U * (spanned(low, high_target) + spanned(low_target, high));
    } else {
        const float centre = load_float(memory, profile + L::kCentre);
        const std::int32_t gap_low = engine::clamped_lane_bin(-centre);
        const std::int32_t gap_high =
            engine::clamped_lane_bin(engine::chop_sub(load_float(memory, profile + L::kGapEnd), centre));
        const std::int32_t gap = engine::best_lane_gap(
            bins, gap_low, gap_high, engine::LaneGapWeights{bin_width, unit, load_float(memory, kLaneGapLeastScore)});
        high_target = gap;
        low_target = gap;
        const engine::Vector4 offset =
            chopped_scaled(side, engine::chop_mul(bin_width, engine::chop_from_int(gap - 128)));
        const bool holding =
            memory.read_u32_le(racer + kRacerOverride) == 0U && memory.read_u32_le(racer + kRacerHolding) != 0U;
        const engine::Vector4 lead =
            chopped_sum(chopped_scaled(forward, holding ? speed : engine::chop_mul(4.0F, speed)), offset, forward.w);
        engine::Vector4 target = chopped_sum(position, lead, forward.w);
        const engine::Vector4 swerve =
            chopped_sum(position, chopped_scaled(forward, engine::chop_mul(10.0F, speed)), forward.w);
        // Within 1 just below the car: blocked, and swerve straight ahead if
        // the gap is that way.
        for (std::int32_t bin = low; bin >= low - 4; --bin) {
            if (bin >= 0 && engine::game_less(distance(bin), 1.0F)) {
                store_float(memory, profile + L::kLowClearance, 1.0F);
                if (gap < low) {
                    target = swerve;
                }
            }
        }
        // The game checks the bins above the car with a loop that runs while
        // the bin is at least four past the car's edge, starting at the edge:
        // it never runs, so only the low side can mark itself blocked.
        store_vector4(memory, profile + L::kTarget, target);
        static_cast<void>(height_speed(middle_level(), speed_source));
        cost = 390U + best_lane_gap_cost(gap_low, gap_high) +
               24U * static_cast<std::uint32_t>(high >= low ? high - low : 0);
    }
    store_float(memory, profile + L::kTargetSpeed, load_float(memory, speed_source));
    const engine::Vector4 target = load_vector4(memory, profile + L::kTarget);
    engine::Vector4 direction{engine::chop_sub(target.x, position.x), engine::chop_sub(target.y, position.y),
                              engine::chop_sub(target.z, position.z), position.w};
    if (engine::game_less(chopped_length_squared(direction), 2.3283064e-10F)) {
        direction = load_vector4(memory, matrix + kMatrixAt);
    }
    const float length_squared = chopped_length_squared(direction);
    const float distance_away = engine::chop_sqrt(length_squared);
    store_float(memory, profile + L::kTargetTime,
                engine::game_less(1.0F, speed) ? engine::game_div(distance_away, speed) : distance_away);
    store_vector4(memory, profile + L::kTargetDirection,
                  chopped_scaled(direction, engine::chop_reciprocal_sqrt(length_squared)));
    memory.write_u8(profile + L::kHighTargetBin, static_cast<std::uint8_t>(high_target));
    memory.write_u8(profile + L::kLowTargetBin, static_cast<std::uint8_t>(low_target));
    return call.finish(cost);
}

// The control points' and spline points' fourth words: the original leaves
// register contents there whenever a point is placed along an edge.
constexpr std::array kRouteSplineUnusedWords{
    UnusedWord{-1, 12U},  UnusedWord{-1, 28U},  UnusedWord{-1, 44U},  UnusedWord{-1, 60U},  UnusedWord{-1, 76U},
    UnusedWord{-1, 92U},  UnusedWord{-1, 108U}, UnusedWord{-1, 124U}, UnusedWord{-1, 140U}, UnusedWord{-1, 156U},
    UnusedWord{-1, 172U}, UnusedWord{-1, 188U}, UnusedWord{-1, 204U}, UnusedWord{-1, 236U}, UnusedWord{-1, 268U},
    UnusedWord{-1, 316U}, UnusedWord{-1, 332U},
};

// The spline points are weighted sums of control points hundreds of metres
// out that cancel to small coordinates, so a coordinate keeps the rounding
// of the large terms: up to 0.5 mm between rounding to nearest and toward
// zero (the captures match exactly when rounding toward zero).
constexpr double kRouteSplineAbsoluteTolerance = 1e-3;

// Something that rides the track strips (traffic among others): its
// position (with its height above the surface in the fourth lane), its up
// direction, the strip quad it is on as four corner indices into a vertex
// array, its row along the strip and the section whose strip it rides.
struct StripRiderLayout {
    static constexpr GuestAddress kPosition = 0U;
    static constexpr GuestAddress kUp = 16U;
    static constexpr GuestAddress kCorners = 32U;   // u16 a, b, c, d
    static constexpr GuestAddress kVertices = 40U;  // pointer, 16 bytes each
    static constexpr GuestAddress kRow = 44U;       // s16, -1 off the strip
    static constexpr GuestAddress kSection = 46U;   // u8, 255 for none
    static constexpr GuestAddress kRecord = 47U;    // u8: its traffic record, 255 for none
};
// The game state's track sections (76 bytes each from +556) and traffic
// records (384 bytes each from +0x175b0).
constexpr GuestAddress kGameState = 0x00522660U;
constexpr GuestAddress kStripSections = kGameState + 556U;
constexpr GuestAddress kStripSectionBytes = 76U;
constexpr GuestAddress kTrafficRecords = kGameState + 0x175b0U;
constexpr GuestAddress kTrafficRecordBytes = 384U;
struct StripSectionLayout {
    static constexpr GuestAddress kStrip = 0U;   // pointer: the rows pointer, then at +16 the row count
    static constexpr GuestAddress kId = 71U;     // u8
};
struct TrafficRecordLayout {
    static constexpr GuestAddress kUp = 128U;
    static constexpr GuestAddress kLeader = 268U;  // pointer, when following something
    static constexpr GuestAddress kRoute = 276U;   // pointer, otherwise
};
constexpr GuestAddress kLeaderRow = 92U;      // s16
constexpr GuestAddress kLeaderSection = 94U;  // u8
constexpr GuestAddress kRouteRow = 48U;       // float, truncated
constexpr GuestAddress kRouteSection = 65U;   // u8
constexpr std::uint8_t kNoId = 255U;

constexpr std::uint32_t kSettleInstructions = 43U + 3U + 2U + 15U + 7U;  // around a crossing
constexpr std::uint32_t kSettleSurface = 50U;
constexpr std::uint32_t kSettleDegenerate = 12U;
constexpr std::uint32_t kSettleSecondTest = 8U + 2U;
constexpr std::uint32_t kSettleMiss = 43U + 3U + 2U + 8U;

std::uint32_t settle_instructions(const engine::SurfaceSettle& settle) {
    const std::uint32_t first = settle.first.hit ? kSegmentTestHitInstructions : kSegmentTestInstructions;
    const std::uint32_t second =
        settle.second_tested ? kSettleSecondTest + (settle.second.hit ? kSegmentTestHitInstructions
                                                                      : kSegmentTestInstructions)
                             : 0U;
    if (!settle.hit) {
        return kSettleMiss + first + second;
    }
    return kSettleInstructions + first + second + kSettleSurface + (settle.degenerate ? kSettleDegenerate : 0U);
}

// A rider as the native code reads it.
struct StripRiderView {
    GuestAddress address{};
    GuestAddress vertices{};
    guest::Gpr128 position_quad;
    engine::StripRider rider;
};

StripRiderView read_strip_rider(const guest::EeMemory& memory, const GuestAddress rider) {
    StripRiderView view;
    view.address = rider;
    view.vertices = memory.read_u32_le(rider + StripRiderLayout::kVertices);
    view.position_quad = load_quad(memory, rider + StripRiderLayout::kPosition);
    view.rider.position = load_quad_operand(memory, rider + StripRiderLayout::kPosition);
    view.rider.height = engine::game_operand(std::bit_cast<float>(view.position_quad.words[3]));
    view.rider.up = load_quad_operand(memory, rider + StripRiderLayout::kUp);
    for (GuestAddress corner = 0U; corner < 4U; ++corner) {
        view.rider.corners[corner] = memory.read_u16_le(rider + StripRiderLayout::kCorners + corner * 2U);
    }
    view.rider.row = static_cast<std::int16_t>(memory.read_u16_le(rider + StripRiderLayout::kRow));
    return view;
}

GuestAddress strip_vertex(const StripRiderView& view, const std::uint16_t index) {
    return view.vertices + static_cast<GuestAddress>(index) * 16U;
}

// What the settle leaves in the vector unit, and on a crossing in the rider.
void apply_settle(guest::R5900State& cpu, guest::EeMemory& memory, const StripRiderView& view,
                  const guest::Gpr128& up_quad, const std::array<std::uint16_t, 4>& corner_indices,
                  const engine::SurfaceSettle& settle) {
    const auto with = [](guest::Gpr128 quad, const engine::Vector3 xyz) {
        quad.words[0] = std::bit_cast<std::uint32_t>(xyz.x);
        quad.words[1] = std::bit_cast<std::uint32_t>(xyz.y);
        quad.words[2] = std::bit_cast<std::uint32_t>(xyz.z);
        return quad;
    };
    const engine::Vector3 position = view.rider.position;
    const engine::Vector3 up{engine::game_operand(std::bit_cast<float>(up_quad.words[0])),
                             engine::game_operand(std::bit_cast<float>(up_quad.words[1])),
                             engine::game_operand(std::bit_cast<float>(up_quad.words[2]))};
    const engine::Vector3 start = engine::chop_add(position, engine::chop_mul(up, 10.0F));
    const guest::Gpr128 start_quad = with(up_quad, start);
    const guest::Gpr128 end_quad = with(up_quad, engine::chop_add(position, engine::chop_mul(up, -10.0F)));
    const GuestAddress a = strip_vertex(view, corner_indices[0]);
    const GuestAddress b = strip_vertex(view, corner_indices[1]);
    const GuestAddress c = strip_vertex(view, corner_indices[2]);
    const GuestAddress d = strip_vertex(view, corner_indices[3]);
    cpu.write_vu0_vector(kVfMargins, end_quad);
    cpu.write_vu0_vector(kVfDirectionQ, view.position_quad);
    if (settle.first.tested) {
        leave_solved_test_registers(cpu, settle.first);
    }
    if (settle.second_tested && settle.second.tested) {
        leave_solved_test_registers(cpu, settle.second);
    }
    if (settle.second_tested) {
        leave_segment_test(cpu, memory, start_quad, end_quad, c, b, d, start, settle.second);
    } else {
        leave_segment_test(cpu, memory, start_quad, end_quad, a, b, c, start, settle.first);
    }
    if (!settle.hit) {
        return;
    }
    using engine::chop_mul;
    const guest::Gpr128 a_quad = load_quad(memory, a);
    const guest::Gpr128 b_quad = load_quad(memory, b);
    const guest::Gpr128 d_quad = load_quad(memory, d);
    const float lift = engine::chop_add(view.rider.height, settle.rise);
    const float risen = engine::chop_add(position.y, chop_mul(up.y, lift));
    guest::Gpr128 up_risen = up_quad;
    up_risen.words[1] = std::bit_cast<std::uint32_t>(risen);
    cpu.write_vu0_vector(kVfMargins, up_risen);
    const engine::Vector3 squares{chop_mul(settle.cross.x, settle.cross.x), chop_mul(settle.cross.y, settle.cross.y),
                                  chop_mul(settle.cross.z, settle.cross.z)};
    guest::Gpr128 lanes = with(d_quad, settle.degenerate ? engine::Vector3{0.0F, 1.0F, 0.0F} : squares);
    lanes.words[3] = std::bit_cast<std::uint32_t>(lift);
    cpu.write_vu0_vector(kVfStart, lanes);
    cpu.write_vu0_vector(kVfDirectionQ, with(view.position_quad, settle.position));
    guest::Gpr128 position_risen = view.position_quad;
    position_risen.words[1] = std::bit_cast<std::uint32_t>(engine::chop_add(0.0F, risen));
    cpu.write_vu0_vector(kVfDirection, position_risen);
    cpu.write_vu0_vector(kVfA, b_quad);
    cpu.write_vu0_vector(kVfB, with(b_quad, engine::chop_sub(load_quad_operand(memory, d), load_quad_operand(memory, b))));
    guest::Gpr128 acc = cpu.vu0_accumulator();
    acc.words[0] = std::bit_cast<std::uint32_t>(engine::chop_add(squares.x, squares.y));
    acc.words[1] = std::bit_cast<std::uint32_t>(chop_mul(settle.across.z, settle.along.x));
    acc.words[2] = std::bit_cast<std::uint32_t>(chop_mul(settle.across.x, settle.along.y));
    cpu.set_vu0_accumulator(acc);
    cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(settle.inverse_length));

    const GuestAddress rider = view.address;
    store_vector4(memory, rider + StripRiderLayout::kPosition,
                  {settle.position.x, settle.position.y, settle.position.z,
                   std::bit_cast<float>(view.position_quad.words[3])});
    const guest::Gpr128 new_up = with(a_quad, settle.up);
    for (GuestAddress word = 0U; word < 4U; ++word) {
        memory.write_u32_le(rider + StripRiderLayout::kUp + word * 4U, new_up.words[word]);
    }
}

engine::SurfaceSettle settle_rider(const guest::EeMemory& memory, const StripRiderView& view) {
    const auto vertex = [&](const std::uint16_t index) { return load_quad_operand(memory, strip_vertex(view, index)); };
    const auto& corners = view.rider.corners;
    return engine::settle_on_quad(view.rider.position, view.rider.height, view.rider.up,
                                  {vertex(corners[0]), vertex(corners[1]), vertex(corners[2]), vertex(corners[3])});
}

std::uint32_t settle_on_strip_quad_cost(const NativeFunctionPrepareContext& context) {
    if (!segment_test_program_loaded(context.memory)) {
        return 0U;
    }
    return settle_instructions(settle_rider(context.memory, read_strip_rider(context.memory, context.cpu.read_gpr_u32(4U))));
}

// SettleOnStripQuad (original 0x0015f410): puts rider a0 on its strip
// quad's surface (engine::settle_on_quad); returns whether its segment
// crossed the quad.
NativeFunctionExecution settle_on_strip_quad(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const StripRiderView view = read_strip_rider(memory, call.arg(0));
    const engine::SurfaceSettle settle = settle_rider(memory, view);
    apply_settle(call.cpu(), memory, view, load_quad(memory, view.address + StripRiderLayout::kUp), view.rider.corners,
                 settle);
    call.return_value(settle.hit ? 1U : 0U);
    return call.finish(settle_instructions(settle));
}

// A rider's position as a caller is about to leave it (natives that run
// these calls inside their own evaluate them before writing anything).
void override_rider_position(StripRiderView& view, const guest::Gpr128& position) {
    view.position_quad = position;
    view.rider.position = quad_operand(position);
    view.rider.height = engine::game_operand(std::bit_cast<float>(position.words[3]));
}

void store_quad(guest::EeMemory& memory, const GuestAddress address, const guest::Gpr128& quad) {
    for (GuestAddress word = 0U; word < 4U; ++word) {
        memory.write_u32_le(address + word * 4U, quad.words[word]);
    }
}

// FollowStrip (original 0x0015f650): keeps rider a0 on its strip
// (engine::keep_on_strip), or, off the strip, looks for it from its
// record's leader or route row (engine::find_on_strip). Returns 1 on the
// strip, 0 off it.
struct StripFollowCall {
    GuestAddress rider{};
    bool valid{};
    StripRiderView view;
    bool sectioned{};
    bool searching{};
    bool from_leader{};
    guest::Gpr128 up_quad;
    std::int32_t start_row{};
    GuestAddress section{};
    engine::StripFollow follow;
    // The rider's position and up direction afterwards.
    guest::Gpr128 position_after;
    guest::Gpr128 up_after;
    std::uint32_t instructions{};
};
thread_local StripFollowCall prepared_strip_follow;

GuestAddress strip_section(const std::uint8_t id) {
    return kStripSections + static_cast<GuestAddress>(id) * kStripSectionBytes;
}

void finish_strip_follow(const guest::EeMemory& memory, StripFollowCall& call) {
    if (call.searching) {
        call.up_after = call.up_quad;
    }
    if (call.follow.on_strip) {
        const engine::StripAttempt& hit = call.follow.attempts[call.follow.attempt_count - 1U];
        call.position_after = with_xyz(call.view.position_quad, hit.settle.position);
        call.up_after = with_xyz(load_quad(memory, strip_vertex(call.view, hit.corners[0])), hit.settle.up);
    }
}

// `position`, when given, replaces the rider's position in memory.
bool evaluate_strip_follow(const guest::EeMemory& memory, const GuestAddress rider, StripFollowCall& call,
                           const guest::Gpr128* position = nullptr) {
    call.rider = rider;
    call.view = read_strip_rider(memory, rider);
    if (position != nullptr) {
        override_rider_position(call.view, *position);
    }
    call.follow = {};
    call.searching = false;
    call.position_after = call.view.position_quad;
    call.up_after = load_quad(memory, rider + StripRiderLayout::kUp);
    const std::uint8_t section_id = memory.read_u8(rider + StripRiderLayout::kSection);
    call.sectioned = section_id != kNoId;
    if (!call.sectioned) {
        call.instructions = 22U;
        return true;
    }
    if (!segment_test_program_loaded(memory)) {
        return false;
    }
    engine::StripRider rider_state = call.view.rider;
    const auto vertex = [&](const std::uint16_t index) {
        return load_quad_operand(memory, strip_vertex(call.view, index));
    };
    std::uint32_t settles = 0U;
    call.searching = call.view.rider.row < 0;
    if (!call.searching) {
        const GuestAddress strip = memory.read_u32_le(strip_section(section_id) + StripSectionLayout::kStrip);
        const GuestAddress rows = memory.read_u32_le(strip);
        const auto count = static_cast<std::int32_t>(memory.read_u32_le(strip + 16U));
        const auto row = [&](const std::int32_t index) {
            const GuestAddress at = rows + static_cast<GuestAddress>(index) * 4U;
            return engine::StripRow{memory.read_u16_le(at), memory.read_u16_le(at + 2U)};
        };
        call.up_quad = load_quad(memory, rider + StripRiderLayout::kUp);
        call.follow = engine::keep_on_strip(rider_state, count, row, vertex);
        for (std::size_t attempt = 0U; attempt < call.follow.attempt_count; ++attempt) {
            settles += settle_instructions(call.follow.attempts[attempt].settle);
        }
        // 12 + 2 + 9 + 3 to here; then the first settle and its check.
        std::uint32_t instructions = 26U + 2U + 2U;
        if (call.follow.attempts[0].settle.hit) {
            instructions += 2U;
        } else if (call.follow.at_end) {
            instructions += 7U + 3U + 2U;
        } else {
            instructions += 7U + 22U + 2U + (call.follow.on_strip ? 2U : 3U + 2U);
        }
        call.instructions = instructions + 8U + settles;
        call.view.rider = rider_state;
        finish_strip_follow(memory, call);
        return true;
    }

    // Searching: the record must exist, and so must the section it names.
    const std::uint8_t record_id = memory.read_u8(rider + StripRiderLayout::kRecord);
    if (record_id == kNoId) {
        return false;
    }
    const GuestAddress record = kTrafficRecords + static_cast<GuestAddress>(record_id) * kTrafficRecordBytes;
    const GuestAddress leader = memory.read_u32_le(record + TrafficRecordLayout::kLeader);
    call.from_leader = leader != 0U;
    std::uint8_t start_section;
    if (call.from_leader) {
        start_section = memory.read_u8(leader + kLeaderSection);
        call.start_row = static_cast<std::int16_t>(memory.read_u16_le(leader + kLeaderRow));
    } else {
        const GuestAddress route = memory.read_u32_le(record + TrafficRecordLayout::kRoute);
        start_section = memory.read_u8(route + kRouteSection);
        call.start_row = engine::truncate_word(load_float(memory, route + kRouteRow));
    }
    if (start_section == kNoId) {
        return false;
    }
    call.section = strip_section(start_section);
    call.up_quad = load_quad(memory, record + TrafficRecordLayout::kUp);
    rider_state.up = load_quad_operand(memory, record + TrafficRecordLayout::kUp);
    const GuestAddress strip = memory.read_u32_le(call.section + StripSectionLayout::kStrip);
    const GuestAddress rows = memory.read_u32_le(strip);
    const auto row = [&](const std::int32_t index) {
        const GuestAddress at = rows + static_cast<GuestAddress>(index) * 4U;
        return engine::StripRow{memory.read_u16_le(at), memory.read_u16_le(at + 2U)};
    };
    call.follow = engine::find_on_strip(rider_state, call.start_row, row, vertex);
    for (std::size_t attempt = 0U; attempt < call.follow.attempt_count; ++attempt) {
        settles += settle_instructions(call.follow.attempts[attempt].settle);
    }
    // 12 + 2 + 9 + 3, 4 + 9 + 3, the start (4 + 9 + 1 from a leader, 8 + 9
    // + 2 from a route), 14, then per attempt 2 + 3 + 17 + 2 and a miss's 2,
    // a final check when none hit, and the result.
    std::uint32_t instructions = 26U + 16U + (call.from_leader ? 14U : 19U) + 14U;
    instructions += static_cast<std::uint32_t>(call.follow.attempt_count) * 24U;
    // Each miss steps back a row.
    const auto attempts = static_cast<std::uint32_t>(call.follow.attempt_count);
    if (call.follow.on_strip) {
        instructions += (attempts - 1U) * 2U + 2U + 2U + 2U + 4U;
    } else {
        // The loop ends on a row check.
        instructions += attempts * 2U;
        instructions += call.start_row - static_cast<std::int32_t>(call.follow.attempt_count) < 0 ? 2U : 5U;
        instructions += 2U + 3U + 2U;
    }
    call.instructions = instructions + 8U + settles;
    call.view.rider = rider_state;
    finish_strip_follow(memory, call);
    return true;
}

// The evaluated follow's writes: the vector unit as each settle leaves it,
// then the rider's quad, row and (when found) section.
void apply_strip_follow(guest::R5900State& cpu, guest::EeMemory& memory, const StripFollowCall& result) {
    if (!result.sectioned) {
        return;
    }
    const GuestAddress rider = result.rider;
    if (result.searching) {
        store_quad(memory, rider + StripRiderLayout::kUp, result.up_quad);
    }
    const engine::StripFollow& follow = result.follow;
    for (std::size_t attempt = 0U; attempt < follow.attempt_count; ++attempt) {
        apply_settle(cpu, memory, result.view, result.up_quad, follow.attempts[attempt].corners,
                     follow.attempts[attempt].settle);
    }
    const engine::StripRider& rider_state = result.view.rider;
    for (GuestAddress corner = 0U; corner < 4U; ++corner) {
        memory.write_u16_le(rider + StripRiderLayout::kCorners + corner * 2U, rider_state.corners[corner]);
    }
    memory.write_u16_le(rider + StripRiderLayout::kRow, static_cast<std::uint16_t>(rider_state.row));
    if (result.searching && follow.on_strip) {
        memory.write_u8(rider + StripRiderLayout::kSection, memory.read_u8(result.section + StripSectionLayout::kId));
    }
}

std::uint32_t follow_strip_cost(const NativeFunctionPrepareContext& context) {
    StripFollowCall& call = prepared_strip_follow;
    call.valid = evaluate_strip_follow(context.memory, context.cpu.read_gpr_u32(4U), call);
    return call.valid ? call.instructions : 0U;
}

NativeFunctionExecution follow_strip(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress rider = call.arg(0);
    StripFollowCall& result = prepared_strip_follow;
    if (!result.valid || result.rider != rider) {
        evaluate_strip_follow(memory, rider, result);
    }
    result.valid = false;
    apply_strip_follow(call.cpu(), memory, result);
    call.return_value(result.follow.on_strip ? 1U : 0U);
    return call.finish(result.instructions);
}

// SwingOntoCentreLine (original 0x0015f270): lines rider a0 up with its
// quad's centre line, keeping its distance from the pivot at a1
// (engine::swing_onto_centre_line). A rider with an unset corner (0xffff)
// stays where it is.
constexpr std::uint16_t kNoCorner = 0xffffU;
constexpr std::uint32_t kSwingInstructions = 101U;

struct SwingCall {
    std::uint32_t instructions{};
    bool moved{};
    StripRiderView view;
    std::array<std::uint16_t, 4> corners{};
    guest::Gpr128 pivot_quad;
    engine::CentreLineSwing swing;
    guest::Gpr128 placed;
};

SwingCall evaluate_swing(const guest::EeMemory& memory, const GuestAddress rider, const guest::Gpr128& pivot_quad,
                         const guest::Gpr128* position = nullptr) {
    SwingCall call;
    for (GuestAddress corner = 0U; corner < 4U; ++corner) {
        call.corners[corner] = memory.read_u16_le(rider + StripRiderLayout::kCorners + corner * 2U);
        if (call.corners[corner] == kNoCorner) {
            // The checks so far, then the return.
            call.instructions = 6U + corner * 3U;
            return call;
        }
    }
    call.moved = true;
    call.instructions = kSwingInstructions;
    call.view = read_strip_rider(memory, rider);
    if (position != nullptr) {
        override_rider_position(call.view, *position);
    }
    call.pivot_quad = pivot_quad;
    const auto vertex = [&](const std::uint16_t index) {
        return load_quad_operand(memory, strip_vertex(call.view, index));
    };
    call.swing = engine::swing_onto_centre_line(
        call.view.rider.position, quad_operand(pivot_quad),
        {vertex(call.corners[0]), vertex(call.corners[1]), vertex(call.corners[2]), vertex(call.corners[3])});
    call.placed = with_xyz(call.view.position_quad, engine::chop_add(engine::Vector3{}, call.swing.position));
    return call;
}

// The vector unit as the original leaves it (the half it loads into VF5
// keeps the top word of a2, `a2_top`, in its fourth lane, and VF1 and VF4
// keep corner c's), then the rider's new position.
void apply_swing(guest::R5900State& cpu, guest::EeMemory& memory, const SwingCall& call, const std::uint32_t a2_top) {
    if (!call.moved) {
        return;
    }
    const engine::CentreLineSwing& swing = call.swing;
    const guest::Gpr128 c_quad = load_quad(memory, strip_vertex(call.view, call.corners[2]));
    guest::Gpr128 terms{};
    terms.words[3] = a2_top;
    cpu.write_vu0_vector(kVfA, with_xyz(terms, swing.along_terms));
    const engine::Vector3 toward = engine::chop_sub(swing.projected, quad_operand(call.pivot_quad));
    guest::Gpr128 acc = cpu.vu0_accumulator();
    acc.words[0] = std::bit_cast<std::uint32_t>(
        engine::chop_add(engine::chop_mul(toward.x, toward.x), engine::chop_mul(toward.y, toward.y)));
    cpu.set_vu0_accumulator(acc);
    cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(swing.toward_inverse_length));
    cpu.write_vu0_vector(kVfDirection, with_xyz(c_quad, swing.toward));
    cpu.write_vu0_vector(kVfDirectionQ, call.pivot_quad);
    cpu.write_vu0_vector(kVfStart, call.placed);
    cpu.write_vu0_vector(kVfMargins, with_xyz(c_quad, swing.position));
    cpu.write_fpr_bits(0U, std::bit_cast<std::uint32_t>(swing.distance));
    store_quad(memory, call.view.address + StripRiderLayout::kPosition, call.placed);
}

NativeFunctionExecution swing_onto_centre_line(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const SwingCall swing = evaluate_swing(memory, call.arg(0), load_quad(memory, call.arg(1)));
    apply_swing(call.cpu(), memory, swing, call.cpu().read_gpr(6U).words[3]);
    return call.finish(swing.instructions);
}

// TrafficMove (original 0x00172e40): drives traffic car a0 along its lane
// (engine/track/traffic_drive): steers and steps its front axle, drags its
// rear axle, swings both onto the strip's centre line for the kinds that
// keep to it, keeps both on the road (FollowStrip), rebuilds its frame,
// turns its wheels and eases its lean. Calls whose FollowStrip would run
// as original code run as original code.
struct TrafficCarLayout {
    static constexpr GuestAddress kFront = 0U;       // front axle rider
    static constexpr GuestAddress kFrontUp = 16U;
    static constexpr GuestAddress kRear = 48U;       // rear axle rider
    static constexpr GuestAddress kRearUp = 64U;
    static constexpr GuestAddress kTarget = 96U;
    static constexpr GuestAddress kSide = 112U;
    static constexpr GuestAddress kUp = 128U;
    static constexpr GuestAddress kForward = 144U;
    static constexpr GuestAddress kRearAnchor = 160U;
    static constexpr GuestAddress kWheels = 176U;    // pointer: its byte 13 counts turning wheels
    static constexpr GuestAddress kPitch = 180U;
    static constexpr GuestAddress kPitchCosine = 184U;
    static constexpr GuestAddress kRoll = 188U;
    static constexpr GuestAddress kRollCosine = 192U;
    static constexpr GuestAddress kSpeed = 196U;
    static constexpr GuestAddress kRoute = 276U;     // pointer
    static constexpr GuestAddress kWheelRadius = 288U;
    static constexpr GuestAddress kWrongSideReach = 292U;
    static constexpr GuestAddress kWheelAngle = 296U;
    static constexpr GuestAddress kSteer = 304U;
    static constexpr GuestAddress kRearReach = 332U;
    static constexpr GuestAddress kAxleOffset = 336U;
    static constexpr GuestAddress kPitchGain = 340U;
    static constexpr GuestAddress kRollGain = 344U;
    static constexpr GuestAddress kLeanLimit = 348U;
    static constexpr GuestAddress kKind = 371U;      // s8
    static constexpr GuestAddress kFlags = 372U;     // u8
};
struct TrafficRouteLayout {
    static constexpr GuestAddress kTurn = 24U;
    static constexpr GuestAddress kSide = 40U;
    static constexpr GuestAddress kLane = 73U;      // u8
};
constexpr GuestAddress kTrafficLanes = kGameState + 0x34210U;
constexpr GuestAddress kTrafficLaneBytes = 80U;
struct TrafficLaneLayout {
    static constexpr GuestAddress kSide = 0U;
    static constexpr GuestAddress kUp = 16U;
    static constexpr GuestAddress kAhead = 32U;
    static constexpr GuestAddress kPoint = 64U;
};
constexpr GuestAddress kWheelCount = 13U;           // s8, in the wheels' block
constexpr std::uint8_t kNoLean = 4U;
constexpr std::int8_t kFirstSwingingKind = 8;       // kinds 8 and 9 keep to the centre line
constexpr GuestAddress kTrafficStepTime = 0x0052267cU;
constexpr GuestAddress kTrafficApproachRate = 0x004e1038U;
constexpr GuestAddress kTrafficSteerSmallStep = 0x004e1170U;
constexpr GuestAddress kTrafficSteerLargeStep = 0x004e0b18U;
constexpr GuestAddress kTrafficSteerSmallBack = 0x004e1260U;
constexpr GuestAddress kTrafficSteerLargeBack = 0x004e125cU;
constexpr GuestAddress kTrafficWheelTurnWrap = 0x004e1264U;
constexpr GuestAddress kTrafficRollKeep = 0x004e0978U;
constexpr GuestAddress kTrafficRollRate = 0x004e126cU;
constexpr GuestAddress kTrafficPitchKeep = 0x004e1270U;

engine::TrafficTuning traffic_tuning(const guest::EeMemory& memory) {
    return {load_float(memory, kTrafficApproachRate), load_float(memory, kTrafficSteerSmallStep),
            load_float(memory, kTrafficSteerLargeStep), load_float(memory, kTrafficSteerSmallBack),
            load_float(memory, kTrafficSteerLargeBack), load_float(memory, kTrafficWheelTurnWrap),
            load_float(memory, kTrafficRollKeep), load_float(memory, kTrafficRollRate),
            load_float(memory, kTrafficPitchKeep)};
}

struct TrafficMoveCall {
    GuestAddress car{};
    bool valid{};
    std::uint32_t instructions{};
    GuestAddress lane{};
    guest::Gpr128 front_quad;
    guest::Gpr128 forward_quad;
    engine::TrafficSteer steer;
    std::uint32_t aim_top{};          // the aim direction's fourth lane
    guest::Gpr128 target_quad;
    guest::Gpr128 front_placed;
    guest::Gpr128 rear_placed;
    bool swings{};
    SwingCall front_swing;
    SwingCall rear_swing;
    guest::Gpr128 front_moved;        // after the swings
    engine::TrafficHeading heading;
    StripFollowCall front_follow;
    StripFollowCall rear_follow;
    guest::Gpr128 front_kept;         // after FollowStrip
    engine::TrafficHeading kept_heading;
    guest::Gpr128 forward_kept;
    engine::TrafficFrame frame;
    guest::Gpr128 axle_up_quad;
    guest::Gpr128 side_quad;
    guest::Gpr128 up_quad;
    bool wheels_turn{};
    bool lean{};
    engine::TrafficLean leaning;
};
thread_local TrafficMoveCall prepared_traffic_move;

// The cost of a normalisation's branch: too short, or (after normalising)
// no length.
std::uint32_t heading_instructions(const engine::TrafficHeading& heading, const std::uint32_t checks) {
    return checks + (heading.tiny ? 6U : 26U + (heading.degenerate ? 5U : 0U));
}

bool evaluate_traffic_move(const guest::EeMemory& memory, const GuestAddress car, TrafficMoveCall& call) {
    using L = TrafficCarLayout;
    const auto value = [&](const GuestAddress offset) { return load_float(memory, car + offset); };
    call.car = car;
    const GuestAddress route = memory.read_u32_le(car + L::kRoute);
    call.lane = kTrafficLanes +
                static_cast<GuestAddress>(memory.read_u8(route + TrafficRouteLayout::kLane)) * kTrafficLaneBytes;
    const engine::TrafficTuning tuning = traffic_tuning(memory);
    call.front_quad = load_quad(memory, car + L::kFront);
    call.forward_quad = load_quad(memory, car + L::kForward);
    const guest::Gpr128 lane_point = load_quad(memory, call.lane + TrafficLaneLayout::kPoint);
    const guest::Gpr128 lane_ahead = load_quad(memory, call.lane + TrafficLaneLayout::kAhead);

    engine::TrafficSteerInput input;
    input.front = quad_operand(call.front_quad);
    input.up = load_quad_operand(memory, car + L::kUp);
    input.forward = quad_operand(call.forward_quad);
    input.rear_anchor = load_quad_operand(memory, car + L::kRearAnchor);
    input.lane = {load_quad_operand(memory, call.lane + TrafficLaneLayout::kSide), quad_operand(lane_ahead),
                  quad_operand(lane_point)};
    input.route_side = engine::game_operand(load_float(memory, route + TrafficRouteLayout::kSide));
    input.wheel_radius = engine::game_operand(value(L::kWheelRadius));
    input.speed = value(L::kSpeed);
    input.step_time = load_float(memory, kTrafficStepTime);
    input.wrong_side_reach = value(L::kWrongSideReach);
    input.steer = value(L::kSteer);
    input.rear_reach = engine::game_operand(value(L::kRearReach));
    call.steer = engine::steer_traffic(input, tuning);
    const engine::TrafficSteer& s = call.steer;
    call.aim_top = s.behind ? lane_ahead.words[3] : call.front_quad.words[3];
    call.target_quad = with_xyz(lane_point, s.target);
    call.front_placed = with_xyz(call.front_quad, s.front);
    call.rear_placed = with_xyz(load_quad(memory, car + L::kRear), s.rear);

    // The steering's blocks.
    std::uint32_t instructions = 83U + (s.behind ? 16U : 5U) + 8U + (s.step_capped ? 2U : 0U) + 5U +
                                 (s.step_negative ? 2U : 0U) + 5U +
                                 (engine::game_less_equal(s.side_offset, 0.0F) ? 7U : 4U) + 6U + 12U + 17U;
    instructions += s.aim_tiny ? 5U : 24U + (s.aim_degenerate ? 6U : 0U);
    instructions += 12U + (engine::game_less_equal(s.turn.y, 0.0F) ? 20U : 18U);
    const bool right = !engine::game_less_equal(input.steer, 0.0F);
    instructions += 5U + 4U + (s.steered_out ? 1U : 0U) + 4U + (s.steered_back ? (right ? 3U : 2U) : 0U);
    instructions += 45U;

    // Kinds that keep to the centre line swing both axles onto it, the rear
    // about the front.
    const auto kind = static_cast<std::int8_t>(memory.read_u8(car + L::kKind));
    call.swings = kind == kFirstSwingingKind || kind == kFirstSwingingKind + 1;
    call.front_moved = call.front_placed;
    guest::Gpr128 rear_moved = call.rear_placed;
    if (call.swings) {
        call.front_swing = evaluate_swing(memory, car + L::kFront, call.target_quad, &call.front_placed);
        if (call.front_swing.moved) {
            call.front_moved = call.front_swing.placed;
        }
        call.rear_swing = evaluate_swing(memory, car + L::kRear, call.front_moved, &call.rear_placed);
        if (call.rear_swing.moved) {
            rear_moved = call.rear_swing.placed;
        }
        instructions += 3U + call.front_swing.instructions + 5U + call.rear_swing.instructions + 2U;
    } else {
        instructions += 1U;
    }
    const float axle_offset = engine::game_operand(value(L::kAxleOffset));
    call.heading = engine::traffic_heading(quad_operand(call.front_moved), quad_operand(rear_moved), axle_offset);
    instructions += heading_instructions(call.heading, 15U);

    // Both axles kept on the road: an axle off it takes the lane's up
    // direction (the front) or the front's (the rear).
    if (!evaluate_strip_follow(memory, car + L::kFront, call.front_follow, &call.front_moved) ||
        !evaluate_strip_follow(memory, car + L::kRear, call.rear_follow, &rear_moved)) {
        return false;
    }
    const bool front_on = call.front_follow.follow.on_strip;
    const bool rear_on = call.rear_follow.follow.on_strip;
    instructions += 12U + call.front_follow.instructions + 2U + (front_on ? 0U : 3U) + 4U +
                    call.rear_follow.instructions + 2U + (rear_on ? 0U : 3U);
    call.front_kept = call.front_follow.position_after;
    const guest::Gpr128 front_up =
        front_on ? call.front_follow.up_after : load_quad(memory, call.lane + TrafficLaneLayout::kUp);
    const guest::Gpr128 rear_up = rear_on ? call.rear_follow.up_after : front_up;
    call.kept_heading = engine::traffic_heading(quad_operand(call.front_kept),
                                                quad_operand(call.rear_follow.position_after), axle_offset);
    instructions += heading_instructions(call.kept_heading, 16U);
    call.forward_kept = with_xyz(call.front_kept, call.kept_heading.forward);

    // The frame from the axles.
    call.frame = engine::traffic_frame(call.kept_heading.forward, load_quad_operand(memory, car + L::kSide),
                                       quad_operand(front_up), quad_operand(rear_up));
    call.axle_up_quad = with_xyz(rear_up, call.frame.axle_up.unit);
    call.side_quad = with_xyz(call.forward_kept, call.frame.side.unit);
    call.up_quad = with_xyz(call.side_quad, call.frame.up.unit);
    instructions += 48U + (call.frame.axle_up.degenerate ? 6U : 0U) + 33U + (call.frame.side.degenerate ? 6U : 0U) +
                    33U + (call.frame.up.degenerate ? 6U : 0U);

    // Wheels and lean.
    const GuestAddress wheels = memory.read_u32_le(car + L::kWheels);
    call.wheels_turn = static_cast<std::int8_t>(memory.read_u8(wheels + kWheelCount)) > 0;
    call.lean = (memory.read_u8(car + L::kFlags) & kNoLean) == 0U;
    engine::TrafficLeanInput lean;
    lean.wheel_angle = value(L::kWheelAngle);
    lean.wheel_radius = value(L::kWheelRadius);
    lean.wheels_turn = call.wheels_turn;
    lean.lean = call.lean;
    lean.roll = value(L::kRoll);
    lean.roll_gain = value(L::kRollGain);
    lean.route_turn = load_float(memory, route + TrafficRouteLayout::kTurn);
    lean.pitch = value(L::kPitch);
    lean.pitch_gain = value(L::kPitchGain);
    lean.lean_limit = value(L::kLeanLimit);
    lean.speed = value(L::kSpeed);
    lean.step = s.step;
    lean.forward_along_side = call.frame.forward_along_side;
    call.leaning = engine::lean_traffic(lean, tuning);
    instructions += 4U;
    if (call.wheels_turn) {
        instructions += 8U + (call.leaning.wrapped_down ? 2U : 4U + (call.leaning.wrapped_up ? 4U : 0U));
    }
    instructions += 4U + (call.lean ? 54U : 0U) + 2U + 8U;
    call.instructions = instructions;
    return true;
}

void apply_traffic_move(guest::R5900State& cpu, guest::EeMemory& memory, const TrafficMoveCall& call) {
    using L = TrafficCarLayout;
    const GuestAddress car = call.car;
    const engine::TrafficSteer& s = call.steer;
    store_quad(memory, car + L::kTarget, call.target_quad);
    store_float(memory, car + L::kSteer, s.steer);
    store_quad(memory, car + L::kFront, call.front_placed);
    store_quad(memory, car + L::kRear, call.rear_placed);
    // The steering leaves the old forward direction in VF4 and the heading
    // (x and z turned, y from the aim) in VF5; the rest is overwritten.
    cpu.write_vu0_vector(kVfDirection, call.forward_quad);
    guest::Gpr128 heading{};
    heading.words[3] = call.aim_top;
    cpu.write_vu0_vector(kVfA, with_xyz(heading, s.heading));
    if (call.swings) {
        apply_swing(cpu, memory, call.front_swing, call.front_quad.words[3]);
        apply_swing(cpu, memory, call.rear_swing, call.front_quad.words[3]);
    }
    store_quad(memory, car + L::kForward, with_xyz(call.front_moved, call.heading.forward));
    store_quad(memory, car + L::kRearAnchor, with_xyz(call.front_moved, call.heading.rear_anchor));
    apply_strip_follow(cpu, memory, call.front_follow);
    if (!call.front_follow.follow.on_strip) {
        store_quad(memory, car + L::kFrontUp, load_quad(memory, call.lane + TrafficLaneLayout::kUp));
    }
    apply_strip_follow(cpu, memory, call.rear_follow);
    if (!call.rear_follow.follow.on_strip) {
        store_quad(memory, car + L::kRearUp, load_quad(memory, car + L::kFrontUp));
    }
    store_quad(memory, car + L::kForward, call.forward_kept);
    store_quad(memory, car + L::kRearAnchor, with_xyz(call.front_kept, call.kept_heading.rear_anchor));
    store_quad(memory, car + L::kSide, call.side_quad);
    store_quad(memory, car + L::kUp, call.up_quad);
    const engine::TrafficLean& lean = call.leaning;
    if (call.wheels_turn) {
        store_float(memory, car + L::kWheelAngle, lean.wheel_angle);
    }
    if (call.lean) {
        store_float(memory, car + L::kRoll, lean.roll);
        store_float(memory, car + L::kRollCosine, lean.roll_cosine);
        store_float(memory, car + L::kPitch, lean.pitch);
        store_float(memory, car + L::kPitchCosine, lean.pitch_cosine);
    }

    // The vector unit as the up direction's normalisation leaves it, with
    // the front axle's up direction in VF3.
    const engine::TrafficUnit& up = call.frame.up;
    cpu.write_vu0_vector(kVfDirectionQ, load_quad(memory, car + L::kFrontUp));
    cpu.write_vu0_vector(kVfMargins, call.up_quad);
    if (up.degenerate) {
        guest::Gpr128 lanes{};
        lanes.words[0] = std::bit_cast<std::uint32_t>(1.0F);
        lanes.words[2] = std::bit_cast<std::uint32_t>(up.unit.z);
        lanes.words[3] = call.side_quad.words[3];
        cpu.write_vu0_vector(kVfStart, lanes);
    } else {
        using engine::chop_mul;
        cpu.write_vu0_vector(kVfStart, with_xyz(call.forward_kept, {chop_mul(up.raw.x, up.raw.x),
                                                                    chop_mul(up.raw.y, up.raw.y),
                                                                    chop_mul(up.raw.z, up.raw.z)}));
    }
    const engine::Vector3 forward = quad_operand(call.forward_kept);
    const engine::Vector3 side = quad_operand(call.side_quad);
    guest::Gpr128 acc = cpu.vu0_accumulator();
    acc.words[0] = std::bit_cast<std::uint32_t>(
        engine::chop_add(engine::chop_mul(up.raw.x, up.raw.x), engine::chop_mul(up.raw.y, up.raw.y)));
    acc.words[1] = std::bit_cast<std::uint32_t>(engine::chop_mul(forward.z, side.x));
    acc.words[2] = std::bit_cast<std::uint32_t>(engine::chop_mul(forward.x, side.y));
    cpu.set_vu0_accumulator(acc);
    cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(up.inverse_length));
}

std::uint32_t traffic_move_cost(const NativeFunctionPrepareContext& context) {
    TrafficMoveCall& call = prepared_traffic_move;
    call.valid = evaluate_traffic_move(context.memory, context.cpu.read_gpr_u32(4U), call);
    return call.valid ? call.instructions : 0U;
}

NativeFunctionExecution traffic_move(NativeFunctionContext& context) {
    GuestCall call{context};
    const GuestAddress car = call.arg(0);
    TrafficMoveCall& result = prepared_traffic_move;
    if (!result.valid || result.car != car) {
        evaluate_traffic_move(call.memory(), car, result);
    }
    result.valid = false;
    apply_traffic_move(call.cpu(), call.memory(), result);
    return call.finish(result.instructions);
}

// The scene's fading effects (see engine/track/fade_effects.hpp): a pool of
// 16 drives, a pool of 128 records and a table of 16 easing slots, all
// members of one owner record.
struct FadePoolLayout {
    static constexpr GuestAddress kDrives = 6660U;         // 16 x 32 bytes
    static constexpr GuestAddress kEaseSlots = 7172U;      // 16 x 20 bytes
    static constexpr GuestAddress kRecordList = 7492U;     // byte list of finished records
    static constexpr GuestAddress kDriveList = 7716U;      // byte list of finished drives
    static constexpr GuestAddress kRecordCount = 7736U;
    static constexpr GuestAddress kDriveCount = 7740U;
    static constexpr GuestAddress kRecordStride = 52U;     // the record proper starts 4 bytes in
};
struct FadeRecordLayout {
    static constexpr GuestAddress kLatch = 0U;
    static constexpr GuestAddress kTarget = 4U;
    static constexpr GuestAddress kLevel = 20U;
    static constexpr GuestAddress kStrength = 24U;
    static constexpr GuestAddress kRemaining = 28U;
    static constexpr GuestAddress kSpan = 32U;
    static constexpr GuestAddress kCountdown = 40U;
    static constexpr GuestAddress kState = 44U;
    static constexpr GuestAddress kStep = 45U;
    static constexpr GuestAddress kKind = 46U;
    static constexpr GuestAddress kFlags = 47U;
    static constexpr GuestAddress kCutoff = 48U;
    static constexpr GuestAddress kMode = 49U;
};
struct FadeTargetLayout {
    static constexpr GuestAddress kHandle = 96U;
    static constexpr GuestAddress kRemoval = 100U;
    static constexpr GuestAddress kScale = 120U;
    static constexpr GuestAddress kAlpha = 133U;
    static constexpr GuestAddress kOptions = 138U;
};
struct FadeDriveLayout {
    static constexpr GuestAddress kLevel = 12U;
    static constexpr GuestAddress kStrength = 16U;
    static constexpr GuestAddress kObject = 20U;
    static constexpr GuestAddress kState = 24U;
    static constexpr GuestAddress kStep = 25U;
    static constexpr GuestAddress kKind = 26U;
    static constexpr GuestAddress kFlags = 27U;
    static constexpr GuestAddress kMode = 29U;
};
// A drive's object: a pointer to a word its removal sets, and the values
// the drive gives it.
struct FadeObjectLayout {
    static constexpr GuestAddress kChain = 48U;
    static constexpr GuestAddress kHolder = 56U;
    static constexpr GuestAddress kBits = 92U;  // u16
    static constexpr GuestAddress kScale = 152U;
    static constexpr GuestAddress kBlend = 160U;
};
constexpr GuestAddress kFadeKindTable = 0x01ea4b00U;  // 8 bytes per kind: scale, level
constexpr unsigned kGlobalPointerRegister = 28U;
constexpr std::int32_t kGlobalScaleA = -22508;
constexpr std::int32_t kGlobalScaleB = -22504;
constexpr std::int32_t kGlobalLevelFloor = -26632;
constexpr unsigned kFadeScaleBArgument = 0U;  // f12
constexpr unsigned kFadeScaleAArgument = 1U;  // f13
constexpr unsigned kFadeSecondsArgument = 3U; // f15
// Guest instructions of one pass over an idle record / drive / slot, which
// only pace the remaining original code's timers.
constexpr std::uint32_t kFadeRecordCost = 134U;
constexpr std::uint32_t kFadeRecordPassCost = 190U;  // with the running test and the list
constexpr std::uint32_t kFadeDriveCost = 125U;
constexpr std::uint32_t kFadeEaseCost = 38U;
constexpr std::uint32_t kFadePassCost = 60U;

[[nodiscard]] GuestAddress global_address(const guest::R5900State& cpu, const std::int32_t offset) {
    return cpu.read_gpr_u32(kGlobalPointerRegister) + static_cast<GuestAddress>(offset);
}

[[nodiscard]] float float_argument(const guest::R5900State& cpu, const unsigned index) {
    return std::bit_cast<float>(cpu.read_fpr_bits(12U + index));
}

// The values a pass shares, with the scales from the registers (the pass
// stores them in the globals first) or, for a single record, the globals.
[[nodiscard]] engine::FadeContext fade_context_from_globals(const guest::EeMemory& memory,
                                                            const guest::R5900State& cpu, const float dt) {
    engine::FadeContext context;
    context.dt = dt;
    context.scale_a = load_float(memory, global_address(cpu, kGlobalScaleA));
    context.scale_b = load_float(memory, global_address(cpu, kGlobalScaleB));
    context.level_floor = memory.read_u8(global_address(cpu, kGlobalLevelFloor));
    return context;
}

[[nodiscard]] engine::FadeContext fade_context_from_arguments(const guest::EeMemory& memory,
                                                              const guest::R5900State& cpu) {
    engine::FadeContext context;
    context.dt = float_argument(cpu, kFadeSecondsArgument);
    context.scale_a = float_argument(cpu, kFadeScaleAArgument);
    context.scale_b = float_argument(cpu, kFadeScaleBArgument);
    context.level_floor = memory.read_u8(global_address(cpu, kGlobalLevelFloor));
    return context;
}

struct LoadedFadeRecord {
    engine::FadeRecord record;
    engine::FadeTarget target;
    GuestAddress target_address{};
};

[[nodiscard]] LoadedFadeRecord load_fade_record(const guest::EeMemory& memory, const GuestAddress address) {
    LoadedFadeRecord loaded;
    engine::FadeRecord& record = loaded.record;
    record.latch = memory.read_u8(address + FadeRecordLayout::kLatch);
    record.state = memory.read_u8(address + FadeRecordLayout::kState);
    record.step = memory.read_u8(address + FadeRecordLayout::kStep);
    record.kind = memory.read_u8(address + FadeRecordLayout::kKind);
    record.flags = memory.read_u8(address + FadeRecordLayout::kFlags);
    record.cutoff = memory.read_u8(address + FadeRecordLayout::kCutoff);
    record.mode = memory.read_u8(address + FadeRecordLayout::kMode);
    record.level = load_float(memory, address + FadeRecordLayout::kLevel);
    record.strength = load_float(memory, address + FadeRecordLayout::kStrength);
    record.remaining = load_float(memory, address + FadeRecordLayout::kRemaining);
    record.span = load_float(memory, address + FadeRecordLayout::kSpan);
    record.countdown = load_float(memory, address + FadeRecordLayout::kCountdown);
    const GuestAddress kind_entry = kFadeKindTable + static_cast<GuestAddress>(record.kind) * 8U;
    record.kind_scale = load_float(memory, kind_entry);
    record.kind_level = load_float(memory, kind_entry + 4U);
    const GuestAddress target = memory.read_u32_le(address + FadeRecordLayout::kTarget);
    loaded.target_address = target;
    loaded.target.scale = load_float(memory, target + FadeTargetLayout::kScale);
    loaded.target.alpha = memory.read_u8(target + FadeTargetLayout::kAlpha);
    loaded.target.options = memory.read_u8(target + FadeTargetLayout::kOptions);
    loaded.target.has_handle = memory.read_u32_le(target + FadeTargetLayout::kHandle) != 0U;
    loaded.target.has_removal = memory.read_u32_le(target + FadeTargetLayout::kRemoval) != 0U;
    return loaded;
}

[[nodiscard]] bool same_bits(const float a, const float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// Stores what a record's update changed.
void store_fade_record(guest::EeMemory& memory, const GuestAddress address, const LoadedFadeRecord& before,
                       const LoadedFadeRecord& after) {
    const engine::FadeRecord& was = before.record;
    const engine::FadeRecord& now = after.record;
    if (now.latch != was.latch) {
        memory.write_u8(address + FadeRecordLayout::kLatch, now.latch);
    }
    if (now.state != was.state) {
        memory.write_u8(address + FadeRecordLayout::kState, now.state);
    }
    if (now.step != was.step) {
        memory.write_u8(address + FadeRecordLayout::kStep, now.step);
    }
    if (now.flags != was.flags) {
        memory.write_u8(address + FadeRecordLayout::kFlags, now.flags);
    }
    if (!same_bits(now.remaining, was.remaining)) {
        store_float(memory, address + FadeRecordLayout::kRemaining, now.remaining);
    }
    if (!same_bits(now.countdown, was.countdown)) {
        store_float(memory, address + FadeRecordLayout::kCountdown, now.countdown);
    }
    const GuestAddress target = after.target_address;
    if (!same_bits(after.target.scale, before.target.scale)) {
        store_float(memory, target + FadeTargetLayout::kScale, after.target.scale);
    }
    if (after.target.alpha != before.target.alpha) {
        memory.write_u8(target + FadeTargetLayout::kAlpha, after.target.alpha);
    }
}

// The update (or, for AdvanceOnly, the state advance alone) of the record
// at `address`. With a const memory it only reports whether the native code
// covers the call.
template <bool AdvanceOnly, class Memory>
engine::FadeStatus run_fade_record(Memory& memory, const GuestAddress address, const engine::FadeContext& context) {
    const LoadedFadeRecord before = load_fade_record(memory, address);
    LoadedFadeRecord after = before;
    const engine::FadeStatus status = AdvanceOnly ? engine::advance_fade_record(after.record, after.target, context)
                                                  : engine::update_fade_record(after.record, after.target, context);
    if constexpr (!std::is_const_v<Memory>) {
        if (status == engine::FadeStatus::Done) {
            store_fade_record(memory, address, before, after);
        }
    }
    return status;
}

std::uint32_t fade_record_update_cost(const NativeFunctionPrepareContext& context) {
    const auto fade = fade_context_from_globals(context.memory, context.cpu, float_argument(context.cpu, kFadeSecondsArgument));
    return run_fade_record<false>(context.memory, context.cpu.read_gpr_u32(4U), fade) == engine::FadeStatus::Done
               ? kFadeRecordCost
               : 0U;
}

// FadeRecordUpdate (original 0x00388080): the record at a0 over f15 seconds.
NativeFunctionExecution fade_record_update(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto fade = fade_context_from_globals(call.memory(), call.cpu(), call.float_arg(kFadeSecondsArgument));
    static_cast<void>(run_fade_record<false>(call.memory(), call.arg(0), fade));
    return call.finish(kFadeRecordCost);
}

constexpr std::uint32_t kFadeAdvanceCost = 40U;  // original max 104

std::uint32_t fade_record_advance_cost(const NativeFunctionPrepareContext& context) {
    const auto fade = fade_context_from_globals(context.memory, context.cpu, 0.0F);
    return run_fade_record<true>(context.memory, context.cpu.read_gpr_u32(4U), fade) == engine::FadeStatus::Done
               ? kFadeAdvanceCost
               : 0U;
}

// FadeRecordAdvance (original 0x003873c0): the fade steps of the record at a0.
NativeFunctionExecution fade_record_advance(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto fade = fade_context_from_globals(call.memory(), call.cpu(), 0.0F);
    static_cast<void>(run_fade_record<true>(call.memory(), call.arg(0), fade));
    return call.finish(kFadeAdvanceCost);
}

void apply_fade_drive(guest::EeMemory& memory, const GuestAddress owner, const GuestAddress index,
                      const engine::FadeContext& context) {
    const GuestAddress drive_address = owner + FadePoolLayout::kDrives + index * 32U;
    engine::FadeDrive drive;
    drive.state = memory.read_u8(drive_address + FadeDriveLayout::kState);
    drive.step = memory.read_u8(drive_address + FadeDriveLayout::kStep);
    drive.flags = memory.read_u8(drive_address + FadeDriveLayout::kFlags);
    const GuestAddress object = memory.read_u32_le(drive_address + FadeDriveLayout::kObject);
    const auto bits = [&] { return memory.read_u16_le(object + FadeObjectLayout::kBits); };
    const auto set_bits = [&](const std::uint16_t mask) {
        memory.write_u16_le(object + FadeObjectLayout::kBits, static_cast<std::uint16_t>(bits() | mask));
    };
    const std::uint8_t state_before = drive.state;
    const std::uint8_t step_before = drive.step;
    const std::uint8_t flags_before = drive.flags;

    float blend = 0.0F;
    if (drive.state != 2U) {
        drive.kind = memory.read_u8(drive_address + FadeDriveLayout::kKind);
        drive.mode = memory.read_u8(drive_address + FadeDriveLayout::kMode);
        drive.level = load_float(memory, drive_address + FadeDriveLayout::kLevel);
        drive.strength = load_float(memory, drive_address + FadeDriveLayout::kStrength);
        const GuestAddress kind_entry = kFadeKindTable + static_cast<GuestAddress>(drive.kind) * 8U;
        drive.kind_scale = load_float(memory, kind_entry);
        drive.kind_level = load_float(memory, kind_entry + 4U);
        engine::FadeDriveOutput output;
        static_cast<void>(engine::update_fade_drive(drive, context, output));
        set_bits(1U);
        store_float(memory, object + FadeObjectLayout::kScale, output.scale);
        set_bits(2U);
        store_float(memory, object + FadeObjectLayout::kBlend, output.blend);
        blend = output.blend;
    }

    const GuestAddress chain = memory.read_u32_le(object + FadeObjectLayout::kChain);
    if (drive.state == 1U || drive.state == 3U) {
        drive.chain_value = memory.read_u32_le(chain);
        const engine::FadeDriveAdvance advance = engine::advance_fade_drive(drive, blend);
        if ((advance.events & engine::kFadeDriveTakeHolder) != 0U) {
            memory.write_u32_le(drive_address + 8U, memory.read_u32_le(chain + 20U));
        }
        if ((advance.events & engine::kFadeDriveChainOne) != 0U) {
            memory.write_u32_le(chain, 1U);
        }
        if ((advance.events & engine::kFadeDriveChainTwo) != 0U) {
            memory.write_u32_le(chain, 2U);
        }
        if ((advance.events & engine::kFadeDriveBits80) != 0U) {
            set_bits(0x80U);
        }
        if ((advance.events & engine::kFadeDriveGiveHolder) != 0U) {
            memory.write_u32_le(chain + 24U, memory.read_u32_le(drive_address + 8U));
        }
        if ((advance.events & engine::kFadeDriveBlend) != 0U) {
            set_bits(2U);
            store_float(memory, object + FadeObjectLayout::kBlend, advance.blend);
        }
        if (drive.step != step_before) {
            memory.write_u8(drive_address + FadeDriveLayout::kStep, drive.step);
        }
        if (drive.state != state_before) {
            memory.write_u8(drive_address + FadeDriveLayout::kState, drive.state);
        }
        if (drive.flags != flags_before) {
            memory.write_u8(drive_address + FadeDriveLayout::kFlags, drive.flags);
        }
    }

    // Whether the drive is finished, from its state, flags and chain word as
    // the advance left them.
    if (drive.state != 2U && drive.state <= 3U) {
        drive.chain_value = memory.read_u32_le(chain);
    }
    if (engine::fade_drive_running(drive) || (drive.flags & 1U) != 0U) {
        return;
    }
    // The finished drive's removal (original 0x00385680, which includes the
    // object's 0x002b0f50), then its place in the list.
    memory.write_u32_le(chain, 1U);
    set_bits(0x40U);
    memory.write_u32_le(object + FadeObjectLayout::kHolder, 0U);
    memory.write_u8(drive_address + FadeDriveLayout::kState, 0U);
    memory.write_u8(drive_address + FadeDriveLayout::kFlags, static_cast<std::uint8_t>(drive.flags & 1U));
    const std::uint32_t count = memory.read_u32_le(owner + FadePoolLayout::kDriveCount);
    memory.write_u32_le(owner + FadePoolLayout::kDriveCount, count + 1U);
    memory.write_u8(owner + FadePoolLayout::kDriveList + count, static_cast<std::uint8_t>(index));
}

void apply_fade_record(guest::EeMemory& memory, const GuestAddress owner, const GuestAddress index,
                       const engine::FadeContext& context) {
    const GuestAddress address = owner + index * FadePoolLayout::kRecordStride + 4U;
    const LoadedFadeRecord before = load_fade_record(memory, address);
    LoadedFadeRecord after = before;
    // The decision was made on the same data: the update completes.
    static_cast<void>(engine::update_fade_record(after.record, after.target, context));
    const bool running = engine::fade_record_running(after.record, after.target);
    store_fade_record(memory, address, before, after);
    if (running || (after.record.flags & 1U) != 0U) {
        return;
    }
    const std::uint32_t count = memory.read_u32_le(owner + FadePoolLayout::kRecordCount);
    memory.write_u32_le(owner + FadePoolLayout::kRecordCount, count + 1U);
    memory.write_u8(owner + FadePoolLayout::kRecordList + count, static_cast<std::uint8_t>(index));
}

void apply_fade_ease(guest::EeMemory& memory, const GuestAddress owner, const GuestAddress index, const float dt) {
    const GuestAddress slot_address = owner + FadePoolLayout::kEaseSlots + index * 20U;
    const GuestAddress out = kFadeKindTable + index * 8U;
    const GuestAddress target = memory.read_u32_le(slot_address + 8U);
    engine::FadeEase slot;
    slot.from_a = load_float(memory, slot_address);
    slot.from_b = load_float(memory, slot_address + 4U);
    slot.duration = load_float(memory, slot_address + 12U);
    slot.countdown = load_float(memory, slot_address + 16U);
    // The target is read only by a slot that eases.
    if (!engine::game_less(slot.countdown, 0.0F) && !engine::game_less_equal(slot.duration, 0.0F)) {
        slot.to_a = load_float(memory, target);
        slot.to_b = load_float(memory, target + 4U);
    }
    const engine::FadeEaseOutput eased = engine::ease_fade_slot(slot, dt);
    if (eased.eased) {
        store_float(memory, out, eased.a);
        store_float(memory, out + 4U, eased.b);
        store_float(memory, slot_address + 16U, eased.countdown);
    } else if (target != 0U) {
        memory.write_u32_le(out, memory.read_u32_le(target));
        memory.write_u32_le(out + 4U, memory.read_u32_le(target + 4U));
    }
}

// Whether the pass over the owner's pools stays inside what the native code
// covers: no record update that reaches an effect object's control code.
// Decided on the data before the pass; the pass writes nothing these
// decisions read.
std::uint32_t fade_pass_cost(const NativeFunctionPrepareContext& context) {
    const auto fade = fade_context_from_arguments(context.memory, context.cpu);
    const GuestAddress owner = context.cpu.read_gpr_u32(4U);
    for (GuestAddress index = 0U; index < 128U; ++index) {
        if (run_fade_record<false>(context.memory, owner + index * FadePoolLayout::kRecordStride + 4U, fade) !=
            engine::FadeStatus::Done) {
            return 0U;
        }
    }
    return kFadePassCost + 16U * kFadeDriveCost + 128U * kFadeRecordPassCost + 16U * kFadeEaseCost;
}

// UpdateFadeEffects (original 0x00389100): the owner a0's drives, records
// and easing slots over f15 seconds, with f12 and f13 as the shared scales.
NativeFunctionExecution update_fade_effects(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress owner = call.arg(0);
    const auto fade = fade_context_from_arguments(memory, call.cpu());
    store_float(memory, global_address(call.cpu(), kGlobalScaleA), fade.scale_a);
    store_float(memory, global_address(call.cpu(), kGlobalScaleB), fade.scale_b);
    memory.write_u32_le(owner + FadePoolLayout::kDriveCount, 0U);
    for (GuestAddress index = 0U; index < 16U; ++index) {
        apply_fade_drive(memory, owner, index, fade);
    }
    memory.write_u32_le(owner + FadePoolLayout::kRecordCount, 0U);
    for (GuestAddress index = 0U; index < 128U; ++index) {
        apply_fade_record(memory, owner, index, fade);
    }
    for (GuestAddress index = 0U; index < 16U; ++index) {
        apply_fade_ease(memory, owner, index, fade.dt);
    }
    return call.finish(kFadePassCost + 16U * kFadeDriveCost + 128U * kFadeRecordPassCost + 16U * kFadeEaseCost);
}

// A compressed triangle mesh (the first argument of 0x00273bf0): a record
// list (+0: pointer, +13: record size, +14: record count), a table of
// 16-bit vertex codes (+4: pointer, six bytes each) and the signed origin
// bytes at +10..+12. See engine/world/ray_mesh_query.hpp.
struct CompressedMeshLayout {
    static constexpr GuestAddress kRecords = 0U;
    static constexpr GuestAddress kVertices = 4U;
    static constexpr GuestAddress kOrigin = 10U;
    static constexpr GuestAddress kRecordSize = 13U;
    static constexpr GuestAddress kRecordCount = 14U;
};
// The query's result record (third argument): the hit point at +0, the
// triangle's normal at +16, the best t at +80 (read first as the limit a hit
// must beat), u and v at +84/+88, the record at +96, whether it is the
// second triangle at +100 and the vertex table at +104.
struct MeshHitLayout {
    static constexpr GuestAddress kPoint = 0U;
    static constexpr GuestAddress kNormal = 16U;
    static constexpr GuestAddress kT = 80U;
    static constexpr GuestAddress kU = 84U;
    static constexpr GuestAddress kV = 88U;
    static constexpr GuestAddress kRecord = 96U;
    static constexpr GuestAddress kSecond = 100U;
    static constexpr GuestAddress kVertices = 104U;
};
constexpr std::uint32_t kMeshQuerySetupInstructions = 150U;
constexpr std::uint32_t kMeshQueryTriangleInstructions = 105U;
constexpr std::uint32_t kMeshQueryHitInstructions = 330U;

// 0 hands the call to the original code: the vector unit's segment test
// program is not loaded (the original uploads it first), or a vertex table
// is not 2-byte aligned (the original's loads would fault).
std::uint32_t mesh_query_cost(const NativeFunctionPrepareContext& context) {
    if (!segment_test_program_loaded(context.memory)) {
        return 0U;
    }
    const GuestAddress mesh = context.cpu.read_gpr_u32(5U);
    if ((context.memory.read_u32_le(mesh + CompressedMeshLayout::kVertices) & 1U) != 0U) {
        return 0U;
    }
    const std::uint32_t count = context.memory.read_u8(mesh + CompressedMeshLayout::kRecordCount);
    return kMeshQuerySetupInstructions + 2U * count * kMeshQueryTriangleInstructions + kMeshQueryHitInstructions;
}

// ClosestMeshHit (original 0x00273bf0): the closest crossing of the segment
// a0 (start, then end, 16 bytes apart) with the compressed mesh a1 that is
// nearer than the t held at +80 of the result record a2. On a hit fills the
// record and returns 1.
NativeFunctionExecution closest_mesh_hit(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress segment = call.arg(0);
    const GuestAddress mesh = call.arg(1);
    const GuestAddress result = call.arg(2);
    const GuestAddress records = memory.read_u32_le(mesh + CompressedMeshLayout::kRecords);
    const GuestAddress vertices = memory.read_u32_le(mesh + CompressedMeshLayout::kVertices);
    const std::uint32_t record_size = memory.read_u8(mesh + CompressedMeshLayout::kRecordSize);
    const std::uint32_t record_count = memory.read_u8(mesh + CompressedMeshLayout::kRecordCount);
    const auto origin = engine::mesh_origin(
        static_cast<std::int8_t>(memory.read_u8(mesh + CompressedMeshLayout::kOrigin)),
        static_cast<std::int8_t>(memory.read_u8(mesh + CompressedMeshLayout::kOrigin + 1U)),
        static_cast<std::int8_t>(memory.read_u8(mesh + CompressedMeshLayout::kOrigin + 2U)));
    const engine::Vector3 start = load_quad_operand(memory, segment);
    const engine::Vector3 end = load_quad_operand(memory, segment + 16U);
    const auto vertex = [&](const std::uint8_t index) {
        const GuestAddress code = vertices + static_cast<GuestAddress>(index) * 6U;
        return engine::mesh_vertex(origin, {memory.read_u16_le(code), memory.read_u16_le(code + 2U),
                                            memory.read_u16_le(code + 4U)});
    };
    const auto indices = [&](const GuestAddress record) {
        return std::array<std::uint8_t, 4>{memory.read_u8(record), memory.read_u8(record + 1U),
                                           memory.read_u8(record + 2U), memory.read_u8(record + 3U)};
    };
    const auto hit = engine::closest_mesh_hit(start, end, load_float(memory, result + MeshHitLayout::kT), records,
                                              records + record_size * record_count, record_size, indices, vertex);
    std::uint32_t instructions = kMeshQuerySetupInstructions + hit.tests * kMeshQueryTriangleInstructions;
    if (hit.found) {
        const guest::Gpr128 end_quad = load_quad(memory, segment + 16U);
        const engine::Vector3 point = engine::segment_point(start, end, hit.t);
        store_float(memory, result + MeshHitLayout::kPoint, point.x);
        store_float(memory, result + MeshHitLayout::kPoint + 4U, point.y);
        store_float(memory, result + MeshHitLayout::kPoint + 8U, point.z);
        memory.write_u32_le(result + MeshHitLayout::kPoint + 12U, end_quad.words[3]);
        const GuestAddress record = hit.record;
        const auto corner = [&](const unsigned slot) {
            return vertex(memory.read_u8(record + slot));
        };
        const engine::Vector3 normal =
            hit.second == 0U ? engine::triangle_unit_normal(corner(0U), corner(1U), corner(2U))
                             : engine::triangle_unit_normal(corner(2U), corner(1U), corner(3U));
        store_float(memory, result + MeshHitLayout::kNormal, normal.x);
        store_float(memory, result + MeshHitLayout::kNormal + 4U, normal.y);
        store_float(memory, result + MeshHitLayout::kNormal + 8U, normal.z);
        // The normal's fourth lane is the upper half of a1 as the original last
        // left it: the entry value, or what its last pending-result read
        // copied from the vector unit (VF24 after an odd number of
        // steps, a freshly loaded VF8 after an even one).
        constexpr unsigned kMeshRegister = 5U;
        constexpr std::size_t kVfFirstResult = 24U;
        memory.write_u32_le(result + MeshHitLayout::kNormal + 12U,
                            hit.tests == 1U      ? call.cpu().read_gpr(kMeshRegister).words[3]
                            : (hit.tests & 1U) == 0U ? call.cpu().read_vu0_vector(kVfFirstResult).words[3]
                                                     : 0U);
        store_float(memory, result + MeshHitLayout::kT, hit.t);
        store_float(memory, result + MeshHitLayout::kU, hit.u);
        store_float(memory, result + MeshHitLayout::kV, hit.v);
        memory.write_u32_le(result + MeshHitLayout::kRecord, record);
        memory.write_u32_le(result + MeshHitLayout::kSecond, hit.second);
        memory.write_u32_le(result + MeshHitLayout::kVertices, vertices);
        instructions += kMeshQueryHitInstructions;
    }
    call.return_value(hit.found ? 1U : 0U);
    return call.finish(instructions);
}

constexpr std::array kFunctions{
    EngineFunction{0x00273bf0U, 3836U, 0x7c022089b3e25d03ULL, "ClosestMeshHit", 20000U, closest_mesh_hit,
                   EngineReturn::Integer, {}, 1e-4, 1e-6, {}, mesh_query_cost},
    EngineFunction{0x0015f410U, 572U, 0xcc2fa496bd0c0567ULL, "SettleOnStripQuad", 300U, settle_on_strip_quad, EngineReturn::Integer,
                   {}, 1e-4, 1e-6, {}, settle_on_strip_quad_cost},
    EngineFunction{0x00172e40U, 2824U, 0xea57e829604053d6ULL, "TrafficMove", 200000U, traffic_move, EngineReturn::Nothing,
                   {}, 1e-4, 1e-6, {}, traffic_move_cost},
    EngineFunction{0x0015f270U, 404U, 0xe9cf393f8adc2faaULL, "SwingOntoCentreLine", 200U, swing_onto_centre_line,
                   EngineReturn::Nothing},
    EngineFunction{0x0015f650U, 752U, 0x1fb4eb03b0a45907ULL, "FollowStrip", 4000U, follow_strip, EngineReturn::Integer,
                   {}, 1e-4, 1e-6, {}, follow_strip_cost},
    EngineFunction{0x0013e530U, 608U, 0xce3fc2efd6a88772ULL, "MarkStripQuadInLaneProfile", 6000U,
                   mark_strip_quad_in_lane_profile, EngineReturn::Nothing},
    EngineFunction{0x0013e790U, 396U, 0xd143f438dce3df91ULL, "BestLaneGap", 12000U, best_lane_gap},
    EngineFunction{0x00140a30U, 3656U, 0x92eb58fdde23c811ULL, "ChooseLaneTarget", 30000U, choose_lane_target,
                   EngineReturn::Nothing},
    EngineFunction{0x00148440U, 592U, 0x5b33c143df96797eULL, "ClassifyPointInStripQuad", 200U,
                   classify_point_in_strip_quad},
    EngineFunction{0x00148ca0U, 184U, 0x6ea4094a0703a949ULL, "RouteOpenAhead", 1100000U, route_open_ahead},
    EngineFunction{0x00148d60U, 144U, 0xf6a6b9b487d050c6ULL, "RouteOpenAheadOf", 1100000U, route_open_ahead_of},
    EngineFunction{0x00146db0U, 364U, 0xdef7d04d9136b4f9ULL, "CrossStripSide", 20000U, cross_strip_side_call},
    EngineFunction{0x00146f20U, 604U, 0x6a4c38295610723cULL, "StepAlongStrip", 400U, step_along_strip_call},
    EngineFunction{0x001461d0U, 868U, 0xca99b68df4155885ULL, "LocateStripQuad", 60000U, locate_strip_quad_call},
    EngineFunction{0x00148690U, 1000U, 0x62c81bf6961255cbULL, "StripQuadEdgesBeyond", 400U, strip_quad_edges_beyond},
    EngineFunction{0x00147f50U, 112U, 0x68d8b4b079ae8645ULL, "StripStartSideNormal", 40U, strip_side_normal_call<0U>},
    EngineFunction{0x00147ee0U, 112U, 0x61337fbcf1ef7a2dULL, "StripEndSideNormal", 40U, strip_side_normal_call<1U>},
    EngineFunction{0x001423e0U, 5240U, 0xa6d14f5e0af121f3ULL, "RacerFollowStrip", 80000U, racer_follow_strip,
                   EngineReturn::Nothing},
    EngineFunction{0x0014a480U, 224U, 0x3d823e68a7cbc973ULL, "QuadEntryForDriver", 40U, quad_entry_for_driver},
    EngineFunction{0x00149c90U, 496U, 0xe4be0e32c30a5c2eULL, "NeighbourQuadEntry", 100000U, neighbour_quad_entry},
    EngineFunction{0x00149180U, 1080U, 0x96ebc2aa20070d02ULL, "PlaceRouteSplinePoints", 2000U, place_route_spline_points,
                   EngineReturn::Nothing, kRouteSplineUnusedWords, 1e-4, kRouteSplineAbsoluteTolerance},
    EngineFunction{0x00251740U, 184U, 0x48758c7b3e254637ULL, "FindTrackSection", 200U, find_track_section},
    EngineFunction{0x00147c10U, 156U, 0xe804eb4eede70c47ULL, "StripEdgeLength", 60U, strip_edge_length, EngineReturn::Float},
    EngineFunction{0x00145ef0U, 336U, 0x3a8b2c8cdf620db7ULL, "StripEdgeFraction", 100U, strip_edge_fraction,
                   EngineReturn::Float},
    EngineFunction{0x001419f0U, 228U, 0xfcf3ec5d6a5b6e72ULL, "DistanceAhead", 80U, distance_ahead, EngineReturn::Float},
    EngineFunction{0x001602f0U, 264U, 0x5b3bba2081a111d2ULL, "CurveDifference", 60U, curve_difference,
                   EngineReturn::Float},
    EngineFunction{0x00147d00U, 392U, 0x52f2acd6d9287952ULL, "StripQuadStep", 100U, strip_quad_step,
                   EngineReturn::Nothing, {}, 1e-4, kQuadStepAbsoluteTolerance},
    EngineFunction{0x00147290U, 380U, 0x5265d69dabeb0478ULL, "FindNearestStripQuad", 400000U, find_nearest_strip_quad,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, nearest_strip_quad_cost},    EngineFunction{0x00389100U, 640U, 0x4c049e5e6413f3a6ULL, "UpdateFadeEffects", 40000U, update_fade_effects,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, fade_pass_cost},
    EngineFunction{0x00388080U, 1092U, 0xcbaeb6e62f8cda6cULL, "FadeRecordUpdate", 400U, fade_record_update,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, fade_record_update_cost},
    EngineFunction{0x003873c0U, 1288U, 0x7636815c3a1a8e35ULL, "FadeRecordAdvance", 120U, fade_record_advance,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, fade_record_advance_cost},
};

} // namespace

std::span<const EngineFunction> track_engine_functions() { return kFunctions; }

} // namespace takedown::runtime
