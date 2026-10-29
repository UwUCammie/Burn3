#pragma once

#include <cstdint>

namespace takedown::runtime {

// Exact emulation-thread event point used by FEMain proof sidecars.  The
// VBlank value is the number of guest VBlank-start interrupts already
// delivered when the sample is taken; the audio value is the matching guest
// SPU2 sample cursor due through that exact EE cycle. It is independent of
// bounded host-render debt and does not mutate the audio model.
struct Slus21050FemainFrameGuestBoundary {
    std::uint64_t guest_vblank{};
    std::uint64_t audio_sample{};

    bool operator==(const Slus21050FemainFrameGuestBoundary&) const = default;
};

} // namespace takedown::runtime
