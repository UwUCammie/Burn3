#include "takedown/runtime/slus_21050_text_glyph.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr std::uint32_t kBinary32SignMask = 0x80000000U;
constexpr std::uint32_t kBinary32ExponentMask = 0x7F800000U;
constexpr std::uint32_t kBinary32LargestFinite = 0x7F7FFFFFU;

float flush_binary32_operand(std::uint32_t bits) noexcept {
    if ((bits & kBinary32ExponentMask) == 0U) {
        bits &= kBinary32SignMask;
    }
    return std::bit_cast<float>(bits);
}

std::uint32_t finish_binary32_result(const double exact, const float rounded) noexcept {
    unsigned int bits = std::bit_cast<unsigned int>(rounded);
    if (exact > 0.0 && exact < static_cast<double>(rounded)) {
        bits = bits + 0xFFFFFFFFU;
    } else if (exact < 0.0 && exact > static_cast<double>(rounded)) {
        bits = bits + 0xFFFFFFFFU;
    }
    const unsigned int exponent = bits & kBinary32ExponentMask;
    if (exponent == 0U) {
        return bits & kBinary32SignMask;
    }
    if (exponent == kBinary32ExponentMask) {
        return (bits & kBinary32SignMask) | kBinary32LargestFinite;
    }
    return bits;
}

std::uint32_t subtract_s_bits(std::uint32_t left, std::uint32_t right) noexcept {
    const float a = flush_binary32_operand(left);
    const float b = flush_binary32_operand(right);
    const double exact = static_cast<double>(a) - static_cast<double>(b);
    return finish_binary32_result(exact, a - b);
}

TextGlyphMetricsBits read_glyph_metrics(const guest::EeMemory& memory,
                                           const guest::GuestAddress address) {
    return TextGlyphMetricsBits{
        memory.read_u32_le(address),
        memory.read_u32_le(address + 4U),
        memory.read_u32_le(address + 8U),
        memory.read_u32_le(address + 12U),
        memory.read_u32_le(address + 16U),
        memory.read_u32_le(address + 20U),
        memory.read_u32_le(address + 24U),
        memory.read_u16_le(address + 28U),
        memory.read_u16_le(address + 30U),
    };
}

bool finite_rectangle_plan(const TextGlyphRectanglePlan& plan) noexcept {
    for (std::size_t index = 0; index < plan.rectangle_count; ++index) {
        for (const std::uint32_t value : plan.rectangles[index].position) {
            if ((value & kBinary32ExponentMask) == kBinary32ExponentMask) {
                return false;
            }
        }
        for (const std::uint32_t value : plan.rectangles[index].texture) {
            if ((value & kBinary32ExponentMask) == kBinary32ExponentMask) {
                return false;
            }
        }
    }
    return true;
}

std::uint64_t function_fingerprint(const guest::EeMemory& memory,
                                   const guest::GuestAddress address,
                                   const std::size_t body_bytes) noexcept {
    constexpr char kDomain[] = "takedown-ee-function-v1";
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](const std::uint8_t value) {
        hash = (hash ^ value) * 1099511628211ULL;
    };
    const auto mix_u32 = [&mix](const std::uint32_t value) {
        for (unsigned int shift = 0; shift != 32; shift += 8) {
            mix(static_cast<std::uint8_t>(value >> shift));
        }
    };
    for (const char value : kDomain) {
        mix(static_cast<std::uint8_t>(value));
    }
    mix_u32(address);
    mix_u32(static_cast<std::uint32_t>(body_bytes));
    for (std::size_t offset = 0; offset < body_bytes; ++offset) {
        mix(memory.read_u8(address + static_cast<std::uint32_t>(offset)));
    }
    return hash;
}

} // namespace

namespace text_float_detail {

std::uint32_t add_s_bits(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float a = flush_binary32_operand(left);
    const float b = flush_binary32_operand(right);
    const double exact = static_cast<double>(a) + static_cast<double>(b);
    return finish_binary32_result(exact, a + b);
}

std::uint32_t multiply_s_bits(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float a = flush_binary32_operand(left);
    const float b = flush_binary32_operand(right);
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    return finish_binary32_result(exact, a * b);
}

} // namespace text_float_detail

namespace text_glyph_guest_detail {

bool text_glyph_line_publication_is_disjoint(const DecodedTextGlyphLine& line,
                                             const std::span<const DirectGuestMemoryRange> additional_reads,
                                             const std::span<const DirectGuestMemoryRange> additional_writes)
{
    const unsigned long long output_bytes =
        static_cast<unsigned long long>(line.rectangle_plan.rectangle_count) * 16U;
    const std::array<DirectGuestMemoryRange, 2> fixed_writes{
        DirectGuestMemoryRange{.address = kTextGlyphPositionRectanglesAddress, .length = output_bytes},
        DirectGuestMemoryRange{.address = kTextGlyphTextureRectanglesAddress, .length = output_bytes},
    };

    const auto overlaps = [](const DirectGuestMemoryRange first,
                             const DirectGuestMemoryRange second) {
        const unsigned long long first_begin = first.address;
        const unsigned long long first_end = first.length + first_begin;
        const unsigned long long second_begin = second.address;
        const unsigned long long second_end = second_begin + second.length;
        return first_begin < second_end && first_end > second_begin;
    };

    if (overlaps(fixed_writes[0], fixed_writes[1])) {
        return false;
    }

    for (std::size_t first = 0; first < additional_writes.size(); ++first) {
        for (std::size_t second = first + 1; second < additional_writes.size(); ++second) {
            if (overlaps(additional_writes[first], additional_writes[second])) {
                return false;
            }
        }
        for (const DirectGuestMemoryRange& write : fixed_writes) {
            if (overlaps(additional_writes[first], write)) {
                return false;
            }
        }
    }

    const auto overlaps_any_read = [&](const DirectGuestMemoryRange write) {
        for (const DirectGuestMemoryRange& range : line.publication_ranges) {
            if (overlaps(write, range)) {
                return true;
            }
        }
        for (const DirectGuestMemoryRange& range : additional_reads) {
            if (overlaps(write, range)) {
                return true;
            }
        }
        return false;
    };

    for (const DirectGuestMemoryRange write : fixed_writes) {
        if (overlaps_any_read(write)) {
            return false;
        }
    }

    for (const DirectGuestMemoryRange write : additional_writes) {
        if (overlaps_any_read(write)) {
            return false;
        }
    }

    return true;
}

TextGlyphLineGuestResult make_text_glyph_line_prevalidated_result(const DecodedTextGlyphLine& line) {
    const TextGlyphRectanglePlan& rectangle_plan = line.rectangle_plan;
    const unsigned int final_text_cursor =
        line.input.text_cursor +
        static_cast<unsigned int>(rectangle_plan.selected_span.end_code_unit) * 2;
    return TextGlyphLineGuestResult{
        kTextGlyphPositionRectanglesAddress,
        kTextGlyphTextureRectanglesAddress,
        final_text_cursor,
        rectangle_plan,
    };
}

namespace {

void write_text_glyph_line_values_prevalidated(guest::EeMemory& memory,
                                               const TextGlyphLineGuestResult& result,
                                               const std::optional<std::uint32_t> text_cursor_cell) {
    if (text_cursor_cell) {
        memory.write_u32_le(*text_cursor_cell, result.final_text_cursor);
    }
    for (std::size_t index = 0; index < result.rectangle_plan.rectangle_count; ++index) {
        const TextGlyphRectangleBits& rectangle = result.rectangle_plan.rectangles[index];
        const unsigned int offset = static_cast<unsigned int>(index) * 16U;
        memory.write_u32_le(result.position_rectangles + offset, rectangle.position[0]);
        memory.write_u32_le(result.position_rectangles + offset + 4U, rectangle.position[1]);
        memory.write_u32_le(result.texture_rectangles + offset, rectangle.texture[0]);
        memory.write_u32_le(result.texture_rectangles + offset + 4U, rectangle.texture[1]);
        memory.write_u32_le(result.position_rectangles + offset + 8U, rectangle.position[2]);
        memory.write_u32_le(result.position_rectangles + offset + 12U, rectangle.position[3]);
        memory.write_u32_le(result.texture_rectangles + offset + 8U, rectangle.texture[2]);
        memory.write_u32_le(result.texture_rectangles + offset + 12U, rectangle.texture[3]);
    }
}

} // namespace

void write_text_glyph_line_prevalidated(guest::EeMemory& memory,
                                        const TextGlyphLineGuestResult& result,
                                        const std::optional<std::uint32_t> text_cursor_cell) {
    write_text_glyph_line_values_prevalidated(memory, result, text_cursor_cell);
}

TextGlyphLineGuestResult publish_text_glyph_line_prevalidated(guest::EeMemory& memory,
                                                              const DecodedTextGlyphLine& line,
                                                              const std::optional<std::uint32_t> text_cursor_cell) {
    const TextGlyphLineGuestResult result = make_text_glyph_line_prevalidated_result(line);
    write_text_glyph_line_values_prevalidated(memory, result, text_cursor_cell);
    return result;
}

} // namespace text_glyph_guest_detail

} // namespace takedown::runtime::slus_21050
