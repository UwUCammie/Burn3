#pragma once
#include "takedown/guest/ee_memory.hpp"

namespace takedown::runtime {

// Original main menu in its ordinary input state. Excludes the title screen,
// modal dialogs, online text entry and all other frontend pages.
inline bool slus_21050_developer_unlock_menu(const guest::EeMemory& memory) {
    if (!memory.contains(0x1ea2a10U, 16)) return false;
    return memory.read_u32_le(0x318100U) == 0x27bdffe0U &&
           memory.read_u32_le(0x318114U) == 0x8c630424U &&
           memory.read_u32_le(0x1ea2974U) == 0x1ea2a10U &&
           memory.read_u64_le(0x1ea2a10U) == 0x94413fc035b0f871ULL &&
           memory.read_u32_le(0x1e90430U) == 0x1e94c40U &&
           memory.read_u32_le(0x1e94c40U) == 0x4de3e0U &&
           memory.read_u32_le(0x4de3e8U) == 0x318100U &&
           memory.read_u32_le(0x1e90424U) == 5U &&
           memory.read_u32_le(0x1e94c50U) == 1U &&
           memory.read_u8(0x1e898f2U) == 0;
}

} // namespace takedown::runtime
