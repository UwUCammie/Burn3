#pragma once

#include "takedown/config/app_settings.hpp"
#include "takedown/guest/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

// RENDER DISTANCE and MODEL QUALITY for SLUS-21050, applied as the game's own
// per-object-type draw parameters.
//
// What the game does (all addresses from the retail image):
//   * Every camera has a far plane of 10000 m (`FUN_0019ee40` sets it once;
//     `FUN_00227360` recomputes it through `FUN_00240ef0`, which always
//     returns 10000). Nothing near that limit is culled by distance.
//   * The track is split into streamed zones. The view keeps up to 17 zone
//     slots (current zone +-8); empty slots are not drawn, and the six-plane
//     zone test in `FUN_0021d080` uses the 10000 m far plane. Which zones are
//     resident is decided by the streamer and is not touched here.
//   * Scenery that repeats (buildings, signs, lamp posts, fences...) is a table
//     of 80-byte object-type records, reached through the scene pointer at
//     [0x0064c994]: halfword count at +0x34, record array at +0x38. The
//     renderer `FUN_0021c120` draws each type's instance lists with
//     `FUN_0026a7e0`, passing three values from the record:
//       +0x3e  flags; bit 0 = the type has a far level of detail,
//       +0x40  LOD switch distance (metres),
//       +0x44  maximum draw distance (metres).
//     Instances nearer than the switch distance use the near model (record
//     +0x38, "mode 1"), instances farther use the far model (+0x3a, "mode 2");
//     the VU0 instance program tests the squared camera distance against
//     the switch distance squared (`FUN_0026a7e0` stores it with the camera
//     position at +0x70, the test runs in micro-program entry 39). Types
//     without bit 0 are drawn at every distance. `FUN_0026a710` moves the
//     frustum far plane of that draw to the maximum draw distance whenever it
//     is below the camera far of 10000 m.
//   Silver Lake: 32 types, switch distances 75-250 m, maximum 700-1000 m.
//
// What this module does: it rewrites those two floats per record, never any
// code or pointer, so only what is already resident is drawn.
//   RENDER DISTANCE  Original: unchanged; Far: maximum draw distance x2;
//                    Maximum: the per-type limit is removed (the 10000 m
//                    camera far plane remains).
//   MODEL QUALITY    Original: unchanged; Low: switch distance x0.5 (the far
//                    model is used sooner); Medium: x2; High: x4; Ultra: the
//                    near model at every distance up to the maximum draw
//                    distance. A switch distance never exceeds the maximum
//                    draw distance in effect.
// With both settings at Original nothing is ever written. Values are derived
// from the originals the game loaded, so changing a setting back restores them
// exactly, and a track reload is picked up because a record the game rewrote
// no longer equals the value written here.
//
// Not covered: the one-per-zone road and terrain objects (they have no
// distance parameter), the second table of the scene (halfword count at +0x36,
// records at +0x3c, 7 in Silver Lake), which `FUN_002f99e0` draws without a
// distance test (squared distances at or above the camera far plane), vehicles
// and traffic (a separate draw path), the number of resident zones, and
// split-screen specific detail (none was found).
struct SceneryDetailPolicy {
    float far_scale{1.0F};     // multiplies the per-type maximum draw distance
    bool far_unlimited{false}; // removes the per-type maximum draw distance
    float lod_scale{1.0F};     // multiplies the LOD switch distance
    bool lod_highest{false};   // near model at every distance in range

    [[nodiscard]] bool is_retail() const noexcept {
        return far_scale == 1.0F && !far_unlimited && lod_scale == 1.0F && !lod_highest;
    }
    bool operator==(const SceneryDetailPolicy&) const = default;
};

// The camera far plane the game uses; a per-type maximum at or above it has no
// effect, so it is also the value that means "no per-type limit".
inline constexpr float kSceneryCameraFarMetres = 10000.0F;

[[nodiscard]] SceneryDetailPolicy
scenery_detail_policy(config::RenderDistance distance, config::ModelQuality quality) noexcept;

// Pure helpers over one record's originals (also what the tests check).
[[nodiscard]] float scenery_maximum_distance(float original_maximum,
                                             const SceneryDetailPolicy& policy) noexcept;
// `maximum` is the maximum draw distance in effect (the result of
// scenery_maximum_distance). Only for types with a far model.
[[nodiscard]] float scenery_switch_distance(float original_switch, float maximum,
                                            const SceneryDetailPolicy& policy) noexcept;

// Value sets the PC OPTIONS page offers for the two rows, in enum order (the
// page maps the selected index straight to the enum). Every value is
// implemented above.
inline constexpr std::array<std::string_view, 3U> kRenderDistanceMenuValues{"Original", "Far",
                                                                           "Maximum"};
inline constexpr std::array<std::string_view, 5U> kModelQualityMenuValues{
    "Original", "Low", "Medium", "High", "Ultra"};

class Slus21050SceneryDetail {
  public:
    struct Statistics {
        std::uint64_t scans{};
        std::uint64_t writes{};
        std::uint64_t restores{};
        std::uint64_t rejected_scenes{};
    };

    // Describes the first instruction word that differs from the retail image
    // this module was written against; nullopt when all of them match.
    [[nodiscard]] static std::optional<std::string>
    signature_mismatch(const guest::EeMemory& memory);

    void configure(const SceneryDetailPolicy& policy) noexcept { policy_ = policy; }
    [[nodiscard]] const SceneryDetailPolicy& policy() const noexcept { return policy_; }

    // Cheap while the policy is retail and nothing is written: no guest access.
    void on_vblank(guest::EeMemory& memory);

    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] std::size_t tracked_records() const noexcept { return records_.size(); }

  private:
    struct Record {
        guest::GuestAddress address{};
        std::uint32_t original_switch{}, original_maximum{};
        std::uint32_t written_switch{}, written_maximum{};
        bool switch_written{false}, maximum_written{false};
    };

    void restore_all(guest::EeMemory& memory);
    void apply(guest::EeMemory& memory);

    SceneryDetailPolicy policy_{};
    std::vector<Record> records_;
    Statistics statistics_{};
};

} // namespace takedown::runtime
