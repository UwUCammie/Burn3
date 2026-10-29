#include "takedown/audio/rwa_spu2_boot.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

namespace takedown::audio {

namespace {

constexpr std::size_t kSupportedFileBytes = 64588U;
constexpr std::size_t kProgramHeaderOffset = 52U;
constexpr std::size_t kProgramHeaderBytes = 32U;
constexpr std::uint16_t kProgramHeaderCount = 2U;
// File offset of the system-bank parking waveform inside the PT_LOAD image.
constexpr std::size_t kParkingWaveOffset = 0xabb0U;

std::uint64_t fnv1a64(const std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint16_t read_u16(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                      (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

} // namespace

bool RwaSpu2BootInitializer::load(const std::filesystem::path& rwa_irx_path) {
    loaded_ = false;
    module_start_applied_ = false;
    error_.clear();
    parking_wave_.fill(std::byte{});

    std::ifstream input(rwa_irx_path, std::ios::binary | std::ios::ate);
    if (!input) {
        error_ = "could not open the retail RWA.IRX";
        return false;
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::streamoff>(end) != static_cast<std::streamoff>(kSupportedFileBytes)) {
        error_ = "RWA.IRX size does not match the supported SLUS-210.50 revision";
        return false;
    }
    input.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(kSupportedFileBytes);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        error_ = "could not read the complete retail RWA.IRX";
        return false;
    }
    const std::span<const std::byte> image(bytes.data(), bytes.size());
    if (fnv1a64(image) != kSupportedFileFnv1a64) {
        error_ = "RWA.IRX fingerprint does not match the supported SLUS-210.50 revision";
        return false;
    }

    // ELF32 little-endian, IRX module type (0xff80), MIPS, two 32-byte
    // program headers directly after the ELF header.
    constexpr std::array<std::byte, 7U> kElfIdentity{std::byte{0x7f}, std::byte{'E'}, std::byte{'L'},
                                                     std::byte{'F'},  std::byte{1},   std::byte{1},
                                                     std::byte{1}};
    if (!std::equal(kElfIdentity.begin(), kElfIdentity.end(), image.begin()) ||
        read_u16(image, 16U) != 0xff80U || read_u16(image, 18U) != 8U ||
        read_u32(image, 28U) != kProgramHeaderOffset || read_u16(image, 42U) != kProgramHeaderBytes ||
        read_u16(image, 44U) != kProgramHeaderCount) {
        error_ = "RWA.IRX is not the supported little-endian IOP module layout";
        return false;
    }

    bool load_segment_found = false;
    for (std::uint16_t index = 0U; index < kProgramHeaderCount; ++index) {
        const auto header = kProgramHeaderOffset + static_cast<std::size_t>(index) * kProgramHeaderBytes;
        if (read_u32(image, header) == 1U /*PT_LOAD*/ && read_u32(image, header + 4U) == 0xb0U &&
            read_u32(image, header + 8U) == 0U && read_u32(image, header + 16U) == 0xb300U &&
            read_u32(image, header + 20U) == 0x104e0U) {
            load_segment_found = true;
            break;
        }
    }
    if (!load_segment_found) {
        error_ = "RWA.IRX PT_LOAD geometry does not match the supported revision";
        return false;
    }

    const auto parking_wave =
        image.subspan(kParkingWaveOffset, kParkingWaveBytes);
    if (fnv1a64(parking_wave) != kParkingWaveFnv1a64) {
        error_ = "RWA.IRX system-bank parking waveform failed validation";
        return false;
    }
    std::copy(parking_wave.begin(), parking_wave.end(), parking_wave_.begin());
    loaded_ = true;
    return true;
}

RwaSpu2BootInitializer::ApplyResult RwaSpu2BootInitializer::apply(RwaSpu2VoiceState& voices,
                                                                  Spu2GuestLink& link) noexcept {
    ApplyResult result{};
    if (!loaded_) {
        return result;
    }
    result = apply_transaction(parking_wave_, voices, link, !module_start_applied_);
    if (result.applied) {
        module_start_applied_ = true;
    }
    return result;
}

RwaSpu2BootInitializer::ApplyResult
RwaSpu2BootInitializer::apply_transaction(const std::span<const std::byte, kParkingWaveBytes> parking_wave,
                                          RwaSpu2VoiceState& voices, Spu2GuestLink& link,
                                          const bool include_module_start) noexcept {
    ApplyResult result{};
    bool accepted = true;

    const auto parameter = [&](const std::uint16_t entry, const std::uint16_t value) {
        ++result.parameter_writes;
        accepted = voices.set_param(entry, value) && accepted;
    };
    const auto address = [&](const std::uint16_t entry, const std::uint32_t value) {
        ++result.address_writes;
        accepted = voices.set_addr(entry, value) && accepted;
    };
    // sceSdSetCoreAttr goes straight to the link; it has no voice-state shadow.
    const auto core_attribute = [&](const std::uint16_t entry, const std::uint16_t value) {
        ++result.core_attribute_writes;
        link.set_core_attr(entry, value);
    };
    const auto module_start_address = [&](const std::uint16_t entry, const std::uint32_t value) {
        ++result.module_start_address_writes;
        accepted = voices.set_addr(entry, value) && accepted;
    };
    const auto module_start_core_attribute = [&](const std::uint16_t entry, const std::uint16_t value) {
        ++result.module_start_core_attribute_writes;
        link.set_core_attr(entry, value);
    };
    const auto transfer = [&](const VoiceTransfer request, const std::span<const std::byte> bytes) {
        if (result.dma_transfers < result.transfers.size()) {
            result.transfers[result.dma_transfers] = request;
        }
        ++result.dma_transfers;
        const auto written = bytes.size() == request.size ? link.dma_upload(request.spu_address, bytes) : 0U;
        result.dma_bytes += static_cast<std::uint32_t>(written);
        accepted = written == bytes.size() && accepted;
    };

    // RWA.IRX module start: reset both cores' IRQ state, then arm core 0 at 0x800.
    if (include_module_start) {
        module_start_core_attribute(0x0004U, 0U);
        module_start_address(0x1f00U, 0U);
        module_start_core_attribute(0x0004U, 0U);
        module_start_core_attribute(0x0005U, 0U);
        module_start_address(0x1f01U, 0U);
        module_start_core_attribute(0x0005U, 0U);
        module_start_address(0x1f00U, 0x800U);
        module_start_core_attribute(0x0004U, 1U);
    }

    // Opcode 0x11: master volumes.
    parameter(0x0980U, 0x3fffU);
    parameter(0x0a80U, 0x3fffU);
    parameter(0x0981U, 0U);
    parameter(0x0a81U, 0U);

    // Clear both cores' scratch areas and upload the parking waveform.
    const std::array<std::byte, 2048U> zeros{};
    transfer(VoiceTransfer{0U, 8U, 0x70d30U, 0x4000U, 2048U}, zeros);
    transfer(VoiceTransfer{1U, 8U, 0x70d30U, 0x4800U, 2048U}, zeros);
    transfer(VoiceTransfer{0U, 8U, 0x6fc30U, 0x5010U, static_cast<std::uint32_t>(kParkingWaveBytes)},
             parking_wave);

    parameter(0x0f80U, 0x7fffU);
    parameter(0x1080U, 0x7fffU);
    parameter(0x0f81U, 0x7fffU);
    parameter(0x1081U, 0x7fffU);

    for (std::uint16_t slot = 0U; slot < 48U; ++slot) {
        const auto selector =
            static_cast<std::uint16_t>((slot % 24U) * 2U + (slot > 23U ? 1U : 0U));
        parameter(selector | 0x0200U, 0U);
        address(selector | 0x2040U, 0x5000U);
        address(selector | 0x2140U, 0x5000U);
        parameter(selector | 0x0300U, 0x000fU);
        parameter(selector | 0x0400U, 0x0005U);
    }

    // Effect work areas.
    address(0x1c00U, 0x1c0000U);
    address(0x1d00U, 0x1dffffU);
    address(0x1c01U, 0x1e0000U);
    address(0x1d01U, 0x1fffffU);
    parameter(0x0b80U, 0U);
    parameter(0x0c80U, 0U);
    parameter(0x0b81U, 0U);
    parameter(0x0c81U, 0U);
    parameter(0x0b81U, 0U);
    parameter(0x0c81U, 0U);
    core_attribute(0x0003U, 0U);
    core_attribute(0x000aU, 0U);
    parameter(0x0981U, 0x3fffU);
    parameter(0x0a81U, 0x3fffU);

    for (std::uint16_t slot = 0U; slot < 48U; ++slot) {
        const auto selector =
            static_cast<std::uint16_t>((slot % 24U) * 2U + (slot > 23U ? 1U : 0U));
        address(selector | 0x2040U, 0x5000U);
        parameter(selector | 0x0200U, 0x0759U);
        parameter(selector, 0x3fffU);
        parameter(selector | 0x0100U, 0x3fffU);
    }

    result.applied = accepted &&
                     result.module_start_address_writes == (include_module_start ? 3U : 0U) &&
                     result.module_start_core_attribute_writes == (include_module_start ? 5U : 0U) &&
                     result.address_writes == 148U && result.parameter_writes == 304U &&
                     result.core_attribute_writes == 2U && result.dma_transfers == 3U &&
                     result.dma_bytes == 4160U;
    return result;
}

} // namespace takedown::audio
