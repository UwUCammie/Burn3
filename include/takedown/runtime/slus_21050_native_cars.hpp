#pragma once

// Native cars for SLUS-21050: the game's car renderer (its VU1 car program)
// in native code.
//
// The game draws every car part (body, panels, wheels; the game's own cars
// and car mods alike, which are ordinary car files) as batches of vertices:
// it uploads the part's constants and one batch of vertices to VU1 memory and
// launches the car program, which deforms, lights and projects the batch and
// sends it to the GS. This class recognises that program and runs each launch
// natively (Slus21050CarProgram below) instead: the same GS packets, bit for
// bit (the car arithmetic rounds like the VU, toward zero), and the register
// and memory state the program leaves for later launches.
//
// The car program's constants (VU1 data memory, in quadwords), recovered
// from the program:
//    962..965  the part's matrix: basis x, y, z and translation (xyz)
//    965.w     integer: the part is prepared (set once the program has
//              combined the matrix with the projection; the game clears it
//              for each part)
//    966..969  the lighting: symmetric 4x4 irradiance matrix M; a surface of
//              normal n receives (n, 1)^T M (n, 1)
//    970       direction to the viewer (xyz); w: the bias of stored normals
//    971..973  the part's axes in view space (xyz) and the eye position in
//              the part's space (w), for the environment map
//    974..1005 damage: 8 zones of 4 quadwords (basis x, y, z, translation)
//    1006..1009 a GS register prefix: a GIF tag (A+D; word x the count, 0
//              for none) and three register writes, sent ahead of the batch
//              and then cleared
//    1010,1011 vertex colour without and with full light (rgb); w: the alpha
//              facing the viewer and at grazing angles
//    1012      the colour of the environment map pass
//    1013      flags: x bit 1 clip triangles at the guard band; y no
//              environment map pass; z the damage zones are valid
//    1014..1017 projection, columns x, y, z, w
//    1018,1019 viewport scale and offset (xyz)
//    1022,1023 guard band scales (clipping)
//
// A batch (from XTOP; XITOP vertices, in groups of four) holds per vertex
// three quadwords: position (xyz; w the GS ADC bit), extra data and normal
// (xyz plus the bias in 970.w). The extra data of a group of four:
//    vertex 0 qw x,y: texture u, v (12-bit fraction) of vertices 0 and 2
//    vertex 1 qw x..w: first damage influence of vertices 0 to 3
//    vertex 2 qw x,y: texture u, v of vertices 1 and 3 (z,w likewise)
//    vertex 3 qw x..w: second damage influence of vertices 0 to 3
// A damage influence is (weight << 8 | zone << 2) with weight / 65535 the
// share and the zone's quadwords at 974 + the low 16 bits.

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace takedown::runtime {

// One launch of the car program, run natively: a pure function of VU1
// memory and registers at the launch.
class Slus21050CarProgram {
  public:
    // Entry points: 0 resets the output buffers; 4 draws a batch, and so
    // does 0x29C (where an MSCNT continues after a batch: it jumps to 4).
    static constexpr std::uint16_t kResetEntry = 0U;
    static constexpr std::uint16_t kDrawEntry = 4U;
    static constexpr std::uint16_t kContinueEntry = 0x29CU;

    // Whether `micro` holds the car program.
    [[nodiscard]] static bool recognise(std::span<const std::byte> micro) noexcept;

    // A vertex as the program leaves it in its input buffer (verification).
    struct TracedVertex {
        std::array<float, 3> position{};
        std::array<float, 3> normal{};
        float u{0.0F}, v{0.0F}, s{0.0F}, t{0.0F};
    };

    // What the launch sends and leaves behind, or nothing for a launch it
    // does not cover (an unknown entry, an empty batch). `trace` receives
    // the batch's vertices.
    [[nodiscard]] static std::optional<guest::Vu1LaunchReplacement>
    run(const guest::Vu1Launch& launch, std::vector<TracedVertex>* trace = nullptr);
};

class Slus21050NativeCars final : public guest::Vu1LaunchObserver {
  public:
    struct Statistics {
        std::uint64_t launches{0U};
        std::uint64_t batches{0U};
        std::uint64_t vertices{0U};
        std::uint64_t damaged{0U};
        std::uint64_t reflective{0U};
        std::uint64_t clipped{0U};
        // Car program launches left to the program (none expected).
        std::uint64_t declined{0U};
        // Verification (TAKEDOWN_NATIVE_CARS_VERIFY=1): the program runs and
        // the native result is compared with it.
        std::uint64_t verified{0U};
        std::uint64_t mismatched{0U};
        std::uint64_t mismatched_words{0U};
    };

    // `verify`: run the program as well and compare (slow; for testing).
    explicit Slus21050NativeCars(bool verify = false);

    std::optional<guest::Vu1LaunchReplacement> before_vu1_launch(const guest::EeMemory& memory,
                                                                 const guest::Vu1Launch& launch) override;
    void after_vu1_launch(const guest::EeMemory& memory, const guest::Vu1Launch& launch) override;

    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }

  private:
    void report_mismatch(const std::string& what);

    bool verify_{false};
    // The native result of the launch being verified, and the output buffer
    // the program writes.
    std::optional<guest::Vu1LaunchReplacement> expected_;
    std::uint16_t expected_buffer_{0U};
    bool expected_prefix_{false};
    bool expected_damaged_{false};
    bool expected_reflective_{false};
    std::vector<Slus21050CarProgram::TracedVertex> expected_trace_;
    std::uint64_t reports_{0U};
    Statistics statistics_{};
};

} // namespace takedown::runtime
