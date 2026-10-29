#include "takedown/runtime/slus_21050_stream_environment.hpp"

#include "takedown/guest/vu0_macro_fmac.hpp"

namespace takedown::runtime::slus_21050 {

namespace {

[[nodiscard]] std::uint32_t pack_position_pair(std::uint32_t x, std::uint32_t y) noexcept {
    return (x & 0xFFFFU) | (y << 16U);
}

[[nodiscard]] std::uint64_t join_words(std::uint32_t low, std::uint32_t high) noexcept {
    return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32U);
}

[[nodiscard]] RawVector4Bits to_bits(const guest::Gpr128& value) noexcept {
    return RawVector4Bits{value.words};
}

} // namespace

std::array<std::uint32_t, 8U> EncodedStreamEnvironmentElement::words() const noexcept {
    const auto low = [](std::uint64_t value) { return static_cast<std::uint32_t>(value); };
    const auto high = [](std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32U); };
    return {low(vertices[0].texture_coordinates),        high(vertices[0].texture_coordinates),
            low(vertices[0].packed_position_and_tag),    high(vertices[0].packed_position_and_tag),
            low(vertices[1].texture_coordinates),        high(vertices[1].texture_coordinates),
            low(vertices[1].packed_position_and_tag),    high(vertices[1].packed_position_and_tag)};
}

std::uint64_t
make_stream_environment_header_descriptor(const RawVector4Bits& header_attribute_vector,
                                          const RawVector4Bits& scale_vector) noexcept {
    const guest::Gpr128 attributes{header_attribute_vector.lanes};
    const guest::Gpr128 scale{scale_vector.lanes};
    const auto multiplied =
        guest::execute_vu0_macro_broadcast_multiply(scale, attributes, scale, 0xFU, 3U);

    std::array<std::uint32_t, 4U> converted{};
    for (std::size_t lane = 0U; lane < converted.size(); ++lane) {
        converted[lane] = guest::convert_vu0_macro_float_to_integer_bits(multiplied.value.words[lane], 0U);
    }

    const unsigned int packed_lane_bytes = (converted[0] & 0xFFU) | ((converted[1] & 0xFFU) << 8U) |
                                            ((converted[2] & 0xFFU) << 16U) | ((converted[3] & 0xFFU) << 24U);
    return static_cast<std::uint64_t>(packed_lane_bytes) | 0x3F80000000000000ULL;
}

RawVector4Bits
make_stream_environment_position_offset(const StreamEnvironmentTranslationBits& translation,
                                        const RawVector4Bits& global_offset) noexcept {
    const guest::Gpr128 duplicated_translation{{translation.x, translation.y, translation.x, translation.y}};
    const guest::Gpr128 global{global_offset.lanes};
    return to_bits(guest::execute_vu0_macro_vector_add_subtract(
                       duplicated_translation, duplicated_translation, global, 0xFU, false)
                       .value);
}

EncodedStreamEnvironmentElement
encode_stream_environment_element(const StreamEnvironmentPositionRectangleBits& position,
                                  const StreamEnvironmentTextureRectangleBits& texture,
                                  const RawVector4Bits& position_offset,
                                  const std::uint32_t packet_tag) noexcept {
    const RawVector4Bits& position_lanes = position.xyxy;
    const guest::Gpr128 positions{position_lanes.lanes};
    const guest::Gpr128 offset{position_offset.lanes};
    const auto translated =
        guest::execute_vu0_macro_vector_add_subtract(positions, positions, offset, 0xFU, false);

    std::array<std::uint32_t, 4U> converted{};
    for (std::size_t lane = 0U; lane < converted.size(); ++lane) {
        converted[lane] = guest::convert_vu0_macro_float_to_integer_bits(translated.value.words[lane], 4U);
    }

    const auto first_xy = pack_position_pair(converted[0], converted[1]);
    const auto second_xy = pack_position_pair(converted[2], converted[3]);

    return {{{{join_words(texture.uvuv.lanes[0], texture.uvuv.lanes[1]), join_words(first_xy, packet_tag)},
              {join_words(texture.uvuv.lanes[2], texture.uvuv.lanes[3]), join_words(second_xy, packet_tag)}}}};
}

} // namespace takedown::runtime::slus_21050
