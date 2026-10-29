#include "takedown/runtime/slus_21050_ui_text_conversion_guest.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace takedown::runtime::slus_21050 {

std::optional<presentation::NativeUiTextConversionInput>
read_ui_text_conversion_from_guest(const guest::EeMemory& memory) {
    if (memory.size() <= 4955071U) {
        return std::nullopt;
    }
    if (memory.size() <= 6802847U) {
        return std::nullopt;
    }
    presentation::NativeUiTextConversionInput result{};
    result.color_scale_bits = memory.read_u32_le(4955068U);
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        result.global_position_offset_bits[lane] =
            memory.read_u32_le((static_cast<std::uint32_t>(lane) + 1700708U) * 4U);
    }
    return result;
}

bool is_ui_text_conversion_current(const guest::EeMemory& memory,
                                   const presentation::NativeUiTextConversionInput& expected) {
    const std::optional<presentation::NativeUiTextConversionInput> current =
        read_ui_text_conversion_from_guest(memory);
    if (current.has_value()) {
        if (current->color_scale_bits == expected.color_scale_bits) {
            if (std::memcmp(current->global_position_offset_bits.data(),
                            expected.global_position_offset_bits.data(), 16U) == 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace takedown::runtime::slus_21050
