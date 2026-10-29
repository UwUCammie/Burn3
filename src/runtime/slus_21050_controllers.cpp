#include "takedown/runtime/slus_21050_controllers.hpp"

#include "takedown/guest/ee_memory.hpp"

namespace takedown::runtime {
namespace {

constexpr guest::GuestAddress kDecodedSampleBase = 0x004F52F8U;
constexpr std::uint32_t kCapabilityChannels = 40U;

std::uint32_t capability_kind(const std::uint32_t channel) {
    switch (channel) {
    case 0U: case 1U: case 2U: case 3U: case 4U: case 5U: case 6U: case 7U:
    case 8U: case 9U: case 10U: case 11U: case 12U: case 13U: case 14U: case 15U:
        return 0U;
    case 16U: case 17U: case 18U: case 19U: case 20U: case 21U: case 22U: case 23U:
    case 24U: case 25U: case 26U: case 27U: case 28U: case 29U: case 30U: case 31U:
    case 35U: case 36U: case 37U: case 38U:
        return 1U;
    default:
        return 8U;
    }
}

} // namespace

bool Slus21050Controllers::publish_second(guest::EeMemory& memory, bool connected,
                                          const guest::RetailDs2DecodedSample& sample) {
    if (memory.read_u32_le(decode_boundary) != 0x00111880U) {
        return false;
    }
    if (memory.read_u32_le(0x004F542CU) != 0x004DDFF0U) {
        return false;
    }
    if (!connected && !owned_) {
        return true;
    }

    const auto& input = connected ? sample : guest::make_neutral_retail_ds2_sample();
    for (std::size_t byte = 0U; byte != guest::kRetailDs2DecodedSampleSize; ++byte) {
        memory.write_u8(kDecodedSampleBase + static_cast<std::uint32_t>(byte), input[byte]);
    }

    if (connected) {
        memory.write_u32_le(0x006767B4U, 1U);
        for (std::uint32_t channel = 0U; channel != kCapabilityChannels; ++channel) {
            const auto at = 0x006767C8U + channel * 16U;
            const bool digital = channel <= 15U;
            const bool axis = (channel >= 16U && channel <= 31U) || (channel >= 35U && channel <= 38U);
            memory.write_u32_le(at, digital || axis ? 1U : 0U);
            memory.write_u32_le(at + 4U, capability_kind(channel));
            memory.write_u32_le(at + 8U, channel <= 15U ? channel >> 3U
                                         : axis         ? (channel <= 31U ? channel - 14U : channel - 17U)
                                                        : 0U);
            memory.write_u32_le(at + 12U, channel <= 15U ? channel & 7U : 0U);
        }
        memory.write_u32_le(0x004F5510U, 0x004F50C0U);
        memory.write_u32_le(0x004F5514U, 1U);
        memory.write_u32_le(0x004F52C4U, 0x004F542CU);
        memory.write_u32_le(0x004F52D4U, 4U);
        memory.write_u32_le(0x004F531CU, 1U);
        owned_ = true;
    } else {
        memory.write_u32_le(0x004F52D4U, 0U);
        owned_ = false;
    }
    return true;
}

} // namespace takedown::runtime
