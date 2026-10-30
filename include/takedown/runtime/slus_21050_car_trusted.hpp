#pragma once

// Trusted car launches (opt3x). The native command emitter draws a car part from its mesh (the VU1 batch as uploaded), its constants
// and the clip history at the launch, so the GS packet the car program would send is never read. This computes only what a later
// launch (or the emitter) can observe of a launch of the car program: the registers, the few data memory words the program keeps, the
// CLIP history, the GIF tag prefix, and the tags of the strips that would have been sent. The vertex arithmetic (damage, lighting,
// environment map, projection) and the writes of the output buffers are skipped: nothing reads them before the guest overwrites them,
// except the clip lookahead quadword (see Slus21050CarTrusted::run, and TAKEDOWN_NATIVE_CMD_CARCHECK in the emitter).

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace takedown::runtime {

class Slus21050CarTrusted {
  public:
    using Quad = std::array<std::uint32_t, 4>;
    struct Strips {
        std::uint32_t count{0U};
        std::uint32_t passes{0U};   // strips of the packet the program would send (1, or 2 with the environment map)
        std::array<Quad, 2> tags{}; // their GIF tags
        bool damaged{false};
        bool reflective{false};
        bool prepared{false};
        bool clip{false};
    };
    // The replacement for a launch of the car program (packet: the register prefix when there is one; the caller appends the strips),
    // or nothing for a launch it does not cover (the program or the full native car program must handle it).
    [[nodiscard]] static std::optional<guest::Vu1LaunchReplacement> run(const guest::Vu1Launch& launch, Strips& strips);
};

} // namespace takedown::runtime
