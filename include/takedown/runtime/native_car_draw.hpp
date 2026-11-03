#pragma once

// P1 native car drawing: what one DrawCar (0x0021e6f0) call draws, computed from game state.
//
// Split in two pure steps (no guest writes, no VU0, no packets):
//   plan_draw_car()   which part draws the call makes, in the game's order, with their world matrices      (WP3)
//   part_entries()    for one part draw, the per-entry VU1 constants 962..1013 and the mesh batches       (WP2)
// The DrawCar replacement (WP5) runs both, performs DrawCar's remaining side effects and submits the result.
// Verified against expected values from re-running the original code on captured calls.
//
// Memory is any read-only accessor with u8/s8/u16/u32/f32(GuestAddress) (the guest memory view at the call, or a
// capture image in the offline tests). Every function is a function of that memory and its arguments only.

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::guest { class EeMemory; }

namespace takedown::runtime::native_car {

using Qw = std::array<std::uint32_t, 4>;   // one VU1 quadword as raw IEEE-754 / integer words

inline constexpr std::size_t kMaxPartDraws = 96;    // body + 6 panels + glass + wheels + extras + 64 pool parts
inline constexpr std::size_t kMaxEntries = 24;      // entries of one draw list
inline constexpr std::size_t kConstQw = 52;         // VU1 qw 962..1013

enum class PartKind : std::uint8_t { Body, Panel, Glass, Wheel, Pool, Extra };

// One DrawEntryList (0x0021fbb0) / DrawSkinnedModel (0x0021f790) call of DrawCar.
struct PartDraw {
    PartKind kind{};
    std::uint8_t index{};          // panel j, wheel k, pool slot i, extra e
    bool skinned{};                // DrawSkinnedModel (damage zones)
    bool lighting{};               // VI3 != 0: recompute the lighting matrix (else reuse the last one)
    std::uint32_t list{};          // guest address of the entry list
    std::uint32_t mask{};          // entry mask (0x00FF body, 0x03FF parts, 0x0300 glass)
    std::uint32_t value{};         // clip flag (a1), lands in 1013.x
    std::uint32_t zones{};         // guest address of the damage zones (skinned)
    std::uint32_t zone_count{};    // T: 8 body, 2 panel, 0 none
    float f12{1.0F};
    std::array<Qw, 4> matrix{};    // world matrix as the guest passes it (3 basis rows + translation)
};

// Everything one DrawCar call decides.
struct CarDrawPlan {
    std::uint32_t s{}, f{}, b{}, m{}, blk{};   // S (render state), car frame, draw block, model, LOD block
    std::int32_t lod{};                        // LOD actually used
    std::uint32_t light_bits{};                // s5 = F.u8[88] & 0xFC
    std::int32_t frustum_class{};              // F.s8[91]
    float f12{1.0F}, f13{1.0F};                // StartEffect colour scales
    bool drawn{};                              // false: DrawCar returns early (still stores S+248/S+252)
    // Side-effect inputs the replacement applies (recon-drawcar section 5):
    std::uint32_t s248{};                      // S.w[248] := B.w[1184]
    std::int8_t s252{};                        // S.s8[252] := F.s8[89] (palette index)
    std::uint32_t texture{};                   // M.w[96] (0: no bind)
    std::size_t glass_patch_count{};
    struct GlassPatch { std::uint32_t entry{}; std::uint8_t b11{}, b13{}; };
    std::array<GlassPatch, 8> glass_patches{}; // BGV glass entry bytes +11/+13, in the order DrawCar writes them
    std::size_t part_count{};
    std::array<PartDraw, kMaxPartDraws> parts{};
};

// Returns false when the call is a case the native path does not handle (the caller then runs the guest DrawCar).
template <class Memory>
[[nodiscard]] bool plan_draw_car(const Memory& memory, std::uint32_t s, std::uint32_t f, std::uint32_t b, std::int32_t lod,
                                 CarDrawPlan& plan);

// plan_draw_car over the live guest memory (instantiated in src/runtime/game/native_car_plan.cpp).
[[nodiscard]] bool plan_draw_car_ee(const guest::EeMemory& memory, std::uint32_t s, std::uint32_t f, std::uint32_t b,
                                    std::int32_t lod, CarDrawPlan& plan);

// Per DrawCar call: StartEffect's W quads and the other values every entry uses (camera up/pos from S, ...).
struct CarShading {
    std::array<Qw, 4> w{};                     // StartEffect W+0..63
    std::array<Qw, 3> axes{};                  // W+784/800/816
    std::array<Qw, 4> lighting{};              // last lighting matrix 966..969 (the VI3 == 0 reuse state)
    bool lighting_valid{};
    // WP2 additions:
    Qw delta{};                                // W+832: car position - camera position (xyz); the w lane is the draw list's float
    Qw slot0{};                                // TEX0 quad 0x21ff60 writes to S+32 for the car texture
    bool slot0_valid{};                        // plan.texture != 0: slot 0 comes from slot0, not from S memory
};

template <class Memory>
void start_shading(const Memory& memory, const CarDrawPlan& plan, CarShading& shading);

// One entry of a draw list: the constants the car program reads and the static mesh it draws.
struct EntryDraw {
    std::array<Qw, kConstQw> qw{};            // VU1 962..1013 as the guest packet leaves them (962..964.w are stack residue)
    bool prefix{};                            // 1006..1009 A+D prefix is sent with this entry
    std::uint32_t batch[2]{};                 // static mesh packets (entry+0, entry+4); 0 = none
    std::uint32_t vertices[2]{};
    bool env{};                               // reflective: second (env) pass
    // WP2 additions:
    std::uint64_t sent{};                     // bit i: qw 962+i is sent by the guest's packet with this entry (the rest persist in VU1)
    std::uint32_t entry{};                    // guest address of the 16-byte entry
    std::uint32_t launches[2]{};              // MSCAL/MSCNT codes inside the two mesh packets (vertices[] = positions to VU1 addr 0)
};

template <class Memory>
std::size_t part_entries(const Memory& memory, const CarDrawPlan& plan, const PartDraw& part, CarShading& shading,
                         std::array<EntryDraw, kMaxEntries>& out);

} // namespace takedown::runtime::native_car
