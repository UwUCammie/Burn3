#pragma once

// P1 / WP2: the per-entry car constants (VU1 qw 962..1013) computed from game state, plus the shading state of one DrawCar call.
//
// start_shading() and part_entries() are declared in native_car_draw.hpp; their definitions are here (they are templates over the
// memory accessor) and the arithmetic they call is in src/runtime/game/native_car_constants.cpp. Include this header where they
// are instantiated.
//
// The values are bit for bit what the guest's DrawCar/DrawEntryList/DrawSkinnedModel and their VU0 microprograms (0xfb, 0x16a)
// left in the display list, but nothing here emulates them: StartEffect's colour quads, the part frame (view vectors, spherical
// harmonics light matrix) and the per-entry colour/ALPHA/flag table are plain functions over floats and integers. The float
// arithmetic uses the VU0 rounding helpers (native_car_vu0_math.hpp) in the microcode's operation order, because the packets
// must carry the identical bits.
//
// What a call reads (all read-only):
//   S   the draw context 0x0067d490:  +32 + 16 * slot TEX0 slots, +192 camera up, +224 camera position
//   F   the car frame:                +48 position, +92..124 light probe SH coefficients
//   T   the car texture descriptor    (plan.texture = M.w[96]): +60/+64 TEX0 words, +144 palette offsets, +160 palette count
//   the draw list and its entries     (+0 count, +4 entries, +8 float; entries: +0/+4 mesh packets, +8 material, +10..+13 bytes)
//   the static mesh packets           (vertex counts only)
//   the game's constants at gp-30520 / -30516 / -30512 (StartEffect)
//
// Memory is any read-only accessor with u8/s8/u16/u32/f32(GuestAddress), as in native_car_draw.hpp.

#include "takedown/runtime/native_car_draw.hpp"
#include "takedown/runtime/native_car_vu0_math.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace takedown::runtime::native_car {

// ---- plain functions (native_car_constants.cpp) ---------------------------------------------------------------------------------

// The light probe as the irradiance matrix of the part (VU1 966..969): the SH coefficients `sh` (world space, rows = c0..c2,
// c3..c5, c6..c8) rotated into the frame whose basis rows are `r`, as the symmetric 4x4 matrix m with E(n) = (n,1) M (n,1)^T.
void light_matrix(const float (&r)[3][3], const float (&sh)[3][3], float (&m)[4][4]);

// The view vectors of a part (VU1 970..973). r = part basis rows, t = part position, up/cam = the draw context's camera up
// and position, del = car position - camera position. v[0].xyz is the unit direction camera -> car in part space (970; its w
// is the draw list's float, not computed here); v[1..3] are the camera basis rows (xyz) with the camera position in part space (w).
void view_vectors(const float (&r)[3][3], const float (&t)[3], const float (&up)[3], const float (&cam)[3], const float (&del)[3],
                  float (&v)[4][4]);

// StartEffect's three colour quads and the spread (W+0..63) as the runtime's StartEffect native computes them (host float products, round
// to nearest even; the guest's EE FPU would chop): extent/half-size, rate and size scaled by the car's f12 (alpha) and f13
// (brightness). The three scale constants are the game's words at gp-30512 (depth), gp-30520 (rate), gp-30516 (size).
void start_effect_quads(std::uint32_t depth_scale, std::uint32_t rate_scale, std::uint32_t size_scale, float f12, float f13,
                        std::array<Qw, 4>& w);

// Everything one entry reads: the draw-list entry (after the glass patch), the call and the shading state.
struct EntryInput {
    std::uint32_t material{};      // entry u16 at +8: bit 1 details, 0x300 glass, bits 2..7 lights
    std::uint32_t slot{};          // entry byte +11: TEX0 slot (4 = glass slot 4 alpha path)
    std::uint32_t blend{};         // entry byte +10: ALPHA_1 raw / blend mode
    std::uint32_t glass_alpha{};   // entry byte +13
    std::uint32_t light_bits{};    // s5, the lights that are on
    float f12{1.0F};               // the part's f12 (W+52)
    std::uint32_t value{};         // 1013.x
    std::uint32_t zone_count{};    // 1013.z
};
struct EntryConstants {
    Qw alpha1{}, alpha2{};         // 1008, 1009
    Qw colour0{}, colour1{}, env{};  // 1010, 1011, 1012
    Qw flags{};                    // 1013
    bool reflective{};             // 1013.y == 0: the entry also gets an environment-map pass
};
// The colour / ALPHA / flag table (microprogram 0x16a). w = the shading quads W+0..W+48.
[[nodiscard]] EntryConstants entry_constants(const EntryInput& input, const std::array<Qw, 4>& w);

// Static mesh packet statistics: walks the packet's DMA chain and VIF codes (CNT/REF/CALL/RET/END tags; UNPACK, MSCAL/MSCNT).
struct MeshStats {
    std::uint32_t vertices{};      // UNPACK V4-32 elements written to VU1 address 0 (the position stream)
    std::uint32_t launches{};      // MSCAL / MSCNT / MSCALF codes
    bool ok{true};
};

namespace detail {

inline constexpr std::uint32_t kGlobalPointer = 0x004e8670U;   // gp of the US executable
inline constexpr std::uint32_t kSlotTable = 32U;               // S + 32 + 16 * slot
inline constexpr std::uint32_t kCameraUp = 192U;               // S + 192 (176 + 16)
inline constexpr std::uint32_t kCameraPos = 224U;              // S + 224 (176 + 48)
inline constexpr std::uint32_t kMaxMeshPackets = 4096U;
inline constexpr std::size_t kZoneQw = 12U;                    // 974
inline constexpr std::size_t kPrefixQw = 44U;                  // 1006

template <class Memory>
[[nodiscard]] Qw load_qw(const Memory& memory, const std::uint32_t address) {
    return Qw{memory.u32(address), memory.u32(address + 4U), memory.u32(address + 8U), memory.u32(address + 12U)};
}

[[nodiscard]] inline float lane(const Qw& q, const std::size_t i) noexcept { return std::bit_cast<float>(q[i]); }

} // namespace detail

// Mesh packet vertex/launch counts (a packet is a DMA chain of CNT/REF/CALL/RET/END tags carrying VIF codes).
template <class Memory>
[[nodiscard]] MeshStats scan_mesh_packet(const Memory& memory, std::uint32_t address) {
    MeshStats stats;
    std::uint32_t stack[2]{};
    std::uint32_t depth = 0U;
    std::uint32_t pending_words = 0U;          // data words still owed to the UNPACK / MPG / DIRECT in flight
    for (std::uint32_t tags = 0U; tags < detail::kMaxMeshPackets; ++tags) {
        const std::uint32_t word0 = memory.u32(address);
        const std::uint32_t qwc = word0 & 0xffffU;
        const std::uint32_t id = (word0 >> 28U) & 7U;
        const std::uint32_t pointer = memory.u32(address + 4U) & 0x7fffffffU;
        std::uint32_t code[2] = {memory.u32(address + 8U), memory.u32(address + 12U)};
        // The data of a REF/REFS/REFE tag is at its address field; other tags carry it behind the tag.
        const bool by_reference = id == 3U || id == 4U || id == 0U;
        const std::uint32_t data = by_reference ? pointer : address + 16U;
        // VIF codes: the two in the tag, then the data quadwords.
        auto feed = [&](const std::uint32_t word) {
            if (pending_words != 0U) {
                --pending_words;
                return;
            }
            const std::uint32_t command = (word >> 24U) & 0x7fU;
            const std::uint32_t immediate = word & 0xffffU;
            const std::uint32_t num = (word >> 16U) & 0xffU;
            if ((command & 0x60U) == 0x60U) {
                static constexpr std::uint8_t kBits[16] = {32, 16, 8, 0, 64, 32, 16, 0, 96, 48, 24, 0, 128, 64, 32, 16};
                const std::uint32_t items = num == 0U ? 256U : num;
                const std::uint32_t bits_total = items * kBits[command & 0xfU];
                // CL == WL (no skipping write) is all the car meshes use; a different cycle would need the STCYCL state.
                pending_words = (bits_total + 31U) / 32U;
                if ((command & 0xfU) == 0xcU && (immediate & 0x3ffU) == 0U) {
                    stats.vertices += items;
                }
            } else if (command == 0x14U || command == 0x15U || command == 0x17U) {
                ++stats.launches;
            } else if (command == 0x20U) {
                pending_words = 1U;
            } else if (command == 0x30U || command == 0x31U) {
                pending_words = 4U;
            } else if (command == 0x4aU) {
                pending_words = (num == 0U ? 256U : num) * 2U;
            } else if (command == 0x50U || command == 0x51U) {
                pending_words = (immediate == 0U ? 65536U : immediate) * 4U;
            }
        };
        feed(code[0]);
        feed(code[1]);
        for (std::uint32_t i = 0U; i < qwc * 4U; ++i) {
            feed(memory.u32(data + 4U * i));
        }
        const std::uint32_t after = by_reference ? address + 16U : address + 16U + 16U * qwc;
        switch (id) {
        case 1U: case 3U: case 4U: address = after; break;      // cnt, ref, refs: continue behind the tag
        case 2U: address = pointer; break;                       // next
        case 5U:                                                 // call
            if (depth == 2U) { stats.ok = false; return stats; }
            stack[depth++] = after;
            address = pointer;
            break;
        case 6U:                                                 // ret
            if (depth == 0U) { stats.ok = false; return stats; }
            address = stack[--depth];
            break;
        default:                                                 // refe, end
            stats.ok = pending_words == 0U;                      // false: the chain ended inside a VIF data block
            return stats;
        }
    }
    stats.ok = false;
    return stats;
}

// ---- start_shading ---------------------------------------------------------------------------------------------------------------

template <class Memory>
void start_shading(const Memory& memory, const CarDrawPlan& plan, CarShading& shading) {
    const std::uint32_t gp = detail::kGlobalPointer;
    start_effect_quads(memory.u32(gp - 30512U), memory.u32(gp - 30520U), memory.u32(gp - 30516U), plan.f12, plan.f13, shading.w);

    // The light probe: the three vec3 at F+92, +104, +116 (the fourth lane is stack residue in the original; nothing reads it).
    for (std::uint32_t i = 0U; i < 3U; ++i) {
        const std::uint32_t at = plan.f + 92U + 12U * i;
        shading.axes[i] = Qw{memory.u32(at), memory.u32(at + 4U), memory.u32(at + 8U), 0U};
    }

    // W+832: car position - camera position, a host float subtraction as the runtime's StartEffect native does it (not the guest's chopped
    // vsub.xyz; see native_car_vu0_math.hpp). The w lane is the draw list's float, supplied per part.
    const std::uint32_t cam = plan.s + detail::kCameraPos;
    const std::uint32_t pos = plan.f + 48U;
    shading.delta = Qw{};
    for (std::uint32_t i = 0U; i < 3U; ++i) {
        shading.delta[i] = vu::host_sub(memory.u32(pos + 4U * i), memory.u32(cam + 4U * i));
    }

    // TEX0 of slot 0, which 0x21ff60 writes to S+32 for the car texture: (T.w[60], T.w[64] + (palette offset << 5), 6, 0).
    shading.slot0_valid = plan.texture != 0U;
    if (shading.slot0_valid) {
        const std::uint32_t t = plan.texture;
        std::uint32_t high = memory.u32(t + 64U);
        const auto palette = static_cast<std::int32_t>(plan.s252);
        if (palette >= 0 && palette < static_cast<std::int32_t>(memory.u8(t + 160U))) {
            high += static_cast<std::uint32_t>(memory.u16(t + 144U + 2U * static_cast<std::uint32_t>(palette))) << 5U;
        }
        shading.slot0 = Qw{memory.u32(t + 60U), high, 6U, 0U};
    }
    // The lighting matrix a part with VI3 == 0 reuses before any part of this call computed one is the guest's W+144 (left there by the
    // previous DrawEntryList that did, possibly another car's). It is only as good as that scratch field: lighting_valid stays false, and a
    // caller that keeps W+144 up to date (writes shading.lighting back after its last lit part) may draw such a call natively.
    const std::uint32_t work = memory.u32(plan.s + 256U);
    if (work >= 0x70000000U && work + 208U <= 0x70004000U) {
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            shading.lighting[i] = detail::load_qw(memory, work + 144U + 16U * i);
        }
    }
    shading.lighting_valid = false;
}

// ---- part_entries ---------------------------------------------------------------------------------------------------------------

template <class Memory>
std::size_t part_entries(const Memory& memory, const CarDrawPlan& plan, const PartDraw& part, CarShading& shading,
                         std::array<EntryDraw, kMaxEntries>& out) {
    namespace d = detail;
    // 962..965: the matrix as the guest passes it (rows 0..2 with their w residue; the translation's w is forced to 0).
    std::array<Qw, kConstQw> frame{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        frame[i] = part.matrix[i];
    }
    frame[3][3] = 0U;
    float r[3][3], t[3];
    for (std::size_t i = 0U; i < 3U; ++i) {
        for (std::size_t l = 0U; l < 3U; ++l) {
            r[i][l] = d::lane(part.matrix[i], l);
        }
        t[i] = d::lane(part.matrix[3], i);
    }

    // 966..969: the light matrix of this part (VI3 != 0) or of the last lit part (VI3 == 0).
    if (part.lighting) {
        float sh[3][3], m[4][4];
        for (std::size_t i = 0U; i < 3U; ++i) {
            for (std::size_t l = 0U; l < 3U; ++l) {
                sh[i][l] = d::lane(shading.axes[i], l);
            }
        }
        light_matrix(r, sh, m);
        for (std::size_t i = 0U; i < 4U; ++i) {
            for (std::size_t l = 0U; l < 4U; ++l) {
                shading.lighting[i][l] = vu::bits(m[i][l]);
            }
        }
        shading.lighting_valid = true;
    }
    for (std::size_t i = 0U; i < 4U; ++i) {
        frame[4U + i] = shading.lighting[i];
    }

    // 970..973: view vectors. 970.w is the draw list's float (the original stores it at W+844 before microprogram 0xfb).
    {
        float up[3], cam[3], del[3], v[4][4];
        for (std::uint32_t l = 0U; l < 3U; ++l) {
            up[l] = vu::flt(memory.u32(plan.s + d::kCameraUp + 4U * l));
            cam[l] = vu::flt(memory.u32(plan.s + d::kCameraPos + 4U * l));
            del[l] = d::lane(shading.delta, l);
        }
        view_vectors(r, t, up, cam, del, v);
        for (std::size_t l = 0U; l < 3U; ++l) {
            frame[8][l] = vu::bits(v[0][l]);
        }
        frame[8][3] = memory.u32(part.list + 8U);
        for (std::size_t i = 1U; i < 4U; ++i) {
            for (std::size_t l = 0U; l < 4U; ++l) {
                frame[8U + i][l] = vu::bits(v[i][l]);
            }
        }
    }

    // 974..1005: damage zones (DrawSkinnedModel): 4 * T quadwords copied from the zone array.
    const std::size_t zone_qw = part.skinned ? 4U * static_cast<std::size_t>(part.zone_count) : 0U;
    for (std::size_t i = 0U; i < zone_qw && d::kZoneQw + i < d::kPrefixQw; ++i) {
        frame[d::kZoneQw + i] = d::load_qw(memory, part.zones + 16U * static_cast<std::uint32_t>(i));
    }
    std::uint64_t part_block = (std::uint64_t{1} << (d::kZoneQw + zone_qw)) - 1U;   // 962..973 + zones

    // The shading quads W+0..W+48 the entry table reads: the car's quads with this part's f12 (W+52) and the light bits (W+56).
    std::array<Qw, 4> w = shading.w;
    w[3][1] = vu::bits(part.f12);
    w[3][2] = plan.light_bits;

    const std::uint32_t count = static_cast<std::uint32_t>(static_cast<std::int8_t>(memory.s8(part.list)));
    const std::uint32_t entries = memory.u32(part.list + 4U);
    std::size_t produced = 0U;
    for (std::uint32_t index = 0U; index < count && produced < kMaxEntries; ++index) {
        const std::uint32_t entry = entries + 16U * index;
        const std::uint32_t material = memory.u16(entry + 8U);
        if ((part.mask & material) == 0U) {
            continue;
        }
        EntryInput input;
        input.material = material;
        input.slot = memory.u8(entry + 11U);
        input.blend = memory.u8(entry + 10U);
        input.glass_alpha = memory.u8(entry + 13U);
        for (std::size_t p = 0U; p < plan.glass_patch_count; ++p) {      // the glass patch DrawCar applies in place
            if (plan.glass_patches[p].entry == entry) {
                input.slot = plan.glass_patches[p].b11;
                input.glass_alpha = plan.glass_patches[p].b13;
            }
        }
        input.light_bits = plan.light_bits;
        input.f12 = part.f12;
        input.value = part.value;
        input.zone_count = part.skinned ? part.zone_count : 0U;
        const EntryConstants c = entry_constants(input, w);

        EntryDraw& draw = out[produced];
        draw = EntryDraw{};
        draw.qw = frame;
        // 1006: the GIF tag template (3 A+D registers) BeginCars seeds; 1007: the TEX0 slot; 1008..1013 from the table.
        draw.qw[d::kPrefixQw] = Qw{3U, 0x10000000U, 0xeU, 0U};
        if (input.slot == 0U && shading.slot0_valid) {
            draw.qw[d::kPrefixQw + 1U] = shading.slot0;
        } else {
            draw.qw[d::kPrefixQw + 1U] = d::load_qw(memory, plan.s + d::kSlotTable + 16U * input.slot);
        }
        draw.qw[d::kPrefixQw + 2U] = c.alpha1;
        draw.qw[d::kPrefixQw + 3U] = c.alpha2;
        draw.qw[d::kPrefixQw + 4U] = c.colour0;
        draw.qw[d::kPrefixQw + 5U] = c.colour1;
        draw.qw[d::kPrefixQw + 6U] = c.env;
        draw.qw[d::kPrefixQw + 7U] = c.flags;
        draw.sent = (produced == 0U ? part_block : 0U) | (std::uint64_t{0xffU} << d::kPrefixQw);
        draw.prefix = true;
        draw.entry = entry;
        draw.batch[0] = memory.u32(entry);
        draw.batch[1] = memory.u32(entry + 4U);
        draw.env = c.reflective;
        for (std::size_t m = 0U; m < 2U; ++m) {
            if (draw.batch[m] != 0U) {
                const MeshStats stats = scan_mesh_packet(memory, draw.batch[m]);
                draw.vertices[m] = stats.vertices;
                draw.launches[m] = stats.launches;
            }
        }
        ++produced;
    }
    return produced;
}

} // namespace takedown::runtime::native_car
