#pragma once

#include <array>
#include <cstdint>

namespace takedown::runtime::slus_21050 {

// Raw binary32 lanes stay as bits across the recovered EE/VU boundary. This
// preserves signed zero, denormals, and exponent-255 inputs until the PS2
// arithmetic helpers apply their normalization rules.
struct RawVector4Bits {
    std::array<std::uint32_t, 4U> lanes{};

    bool operator==(const RawVector4Bits&) const = default;
};

// The caller supplies one XY translation pair. StreamEnvironmentPacket
// duplicates it to XYXY before adding the global position-offset vector.
struct StreamEnvironmentTranslationBits {
    std::uint32_t x{};
    std::uint32_t y{};

    bool operator==(const StreamEnvironmentTranslationBits&) const = default;
};

// One four-lane rectangle produced by the text geometry path: x0, y0, x1, y1.
struct StreamEnvironmentPositionRectangleBits {
    RawVector4Bits xyxy;

    bool operator==(const StreamEnvironmentPositionRectangleBits&) const = default;
};

// Parallel texture rectangle: u0, v0, u1, v1. These words are packet payload;
// StreamEnvironmentPacket does not perform arithmetic on them.
struct StreamEnvironmentTextureRectangleBits {
    RawVector4Bits uvuv;

    bool operator==(const StreamEnvironmentTextureRectangleBits&) const = default;
};

// One 16-byte packet vertex. Texture coordinates occupy the low doubleword;
// the fixed-point XY pair and caller-selected packet tag occupy the high one.
struct StreamEnvironmentPacketVertex {
    std::uint64_t texture_coordinates{};
    std::uint64_t packed_position_and_tag{};

    bool operator==(const StreamEnvironmentPacketVertex&) const = default;
};

// One input rectangle always emits two consecutive packet vertices. words()
// exposes their exact little-endian eight-word packet representation without
// relying on host struct layout.
struct EncodedStreamEnvironmentElement {
    std::array<StreamEnvironmentPacketVertex, 2U> vertices{};

    [[nodiscard]] std::array<std::uint32_t, 8U> words() const noexcept;

    bool operator==(const EncodedStreamEnvironmentElement&) const = default;
};

// Computes the descriptor compared with and stored beside the active packet
// header. VMULW broadcasts scale_vector.w, VFTOI0 converts all four products,
// and the low byte of each converted lane is packed below the fixed 1.0f tag.
[[nodiscard]] std::uint64_t
make_stream_environment_header_descriptor(const RawVector4Bits& header_attribute_vector,
                                          const RawVector4Bits& scale_vector) noexcept;

// Duplicates translation as {x, y, x, y}, then applies the global vector with
// the exact macro-mode VADD rules used by StreamEnvironmentPacket.
[[nodiscard]] RawVector4Bits
make_stream_environment_position_offset(const StreamEnvironmentTranslationBits& translation,
                                        const RawVector4Bits& global_offset) noexcept;

// Applies the recovered VADD/VFTOI4/PPACH element path. Only the low 16 bits of
// each converted position lane enter the XY pairs. Texture words pass through
// unchanged, and packet_tag is zero-extended into each vertex's high word.
[[nodiscard]] EncodedStreamEnvironmentElement
encode_stream_environment_element(const StreamEnvironmentPositionRectangleBits& position,
                                  const StreamEnvironmentTextureRectangleBits& texture,
                                  const RawVector4Bits& position_offset,
                                  std::uint32_t packet_tag) noexcept;

} // namespace takedown::runtime::slus_21050
