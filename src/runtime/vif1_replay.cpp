// Replays captured VIF1 and GIF DMA source chains from raw EE RAM images, and
// fingerprints the GS command stream they produce.

#include "takedown/runtime/vif1_replay.hpp"

#include "takedown/guest/gs_capture.hpp"

#include "takedown/runtime/vu1_worker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Folds the low `Bytes` bytes of `value` into an FNV-1a 64 hash, least
// significant byte first.
template <std::size_t Bytes, typename T>
void fold_bytes(std::uint64_t& hash, const T value) noexcept {
    for (unsigned byte = 0U; byte != Bytes; ++byte) {
        hash = (hash ^ static_cast<std::uint64_t>((value >> (byte * 8U)) & 0xFFU)) * kFnvPrime;
    }
}

// Command observer that folds every GS command into the fingerprint.
class GsFingerprintObserver final : public guest::GsCommandObserver {
  public:
    explicit GsFingerprintObserver(Vif1ReplayGsFingerprint& fingerprint) noexcept : fingerprint_(fingerprint) {}

    void on_gs_gif_tag_begin(const guest::GifPath path) override {
        fingerprint_.fnv1a64 = (fingerprint_.fnv1a64 ^ 1U) * kFnvPrime;
        fingerprint_.fnv1a64 = (static_cast<std::uint64_t>(path) ^ fingerprint_.fnv1a64) * kFnvPrime;
        fingerprint_.gif_tag_begins += 1U;
    }

    void on_gs_register_write(const guest::GsRegisterWrite& write) override {
        fingerprint_.fnv1a64 = (fingerprint_.fnv1a64 ^ 2U) * kFnvPrime;
        fingerprint_.fnv1a64 = (static_cast<std::uint64_t>(write.path) ^ fingerprint_.fnv1a64) * kFnvPrime;
        fingerprint_.fnv1a64 = (static_cast<std::uint64_t>(write.source) ^ fingerprint_.fnv1a64) * kFnvPrime;
        fingerprint_.fnv1a64 = (static_cast<std::uint64_t>(write.address) ^ fingerprint_.fnv1a64) * kFnvPrime;
        fold_bytes<8>(fingerprint_.fnv1a64, write.value);
        fold_bytes<8>(fingerprint_.fnv1a64, write.packed_high);
        fingerprint_.register_writes += 1U;
    }

    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override {
        fingerprint_.fnv1a64 = (fingerprint_.fnv1a64 ^ 3U) * kFnvPrime;
        fingerprint_.fnv1a64 = (static_cast<std::uint64_t>(transfer.path) ^ fingerprint_.fnv1a64) * kFnvPrime;
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.destination_base_pointer);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.destination_buffer_width);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.destination_psm);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.destination_x);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.destination_y);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.width);
        fold_bytes<4>(fingerprint_.fnv1a64, transfer.height);
        const std::uint64_t payload_bytes = transfer.pixels.size();
        fold_bytes<8>(fingerprint_.fnv1a64, payload_bytes);
        for (const std::uint8_t byte : transfer.pixels) {
            fingerprint_.fnv1a64 = (fingerprint_.fnv1a64 ^ static_cast<std::uint64_t>(byte)) * kFnvPrime;
        }
        fingerprint_.image_transfers += 1U;
        fingerprint_.image_payload_bytes += payload_bytes;
    }

  private:
    Vif1ReplayGsFingerprint& fingerprint_;
};

void validate_source_chain_tadr(const guest::GuestAddress tadr, const char* const channel) {
    if ((tadr & 15U) != 0U || tadr > 33554416U) {
        std::string message{channel};
        message += " TADR must be 16-byte aligned inside the 32 MiB EE-RAM image";
        throw std::invalid_argument(message);
    }
}

[[nodiscard]] std::uint32_t read_u32_le(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                                        const char* const description) {
    if (offset > bytes.size() || bytes.size() - offset <= 3U) {
        std::string message{"truncated "};
        message += description;
        throw std::runtime_error(message);
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::vector<std::uint8_t> read_exact_binary(const std::filesystem::path& path,
                                                           const std::size_t expected_bytes,
                                                           const char* const description) {
    std::ifstream input;
    input.open(path, std::ios::binary);
    if (!input.is_open()) {
        std::string message{"unable to open "};
        message += description;
        message += ": ";
        throw std::runtime_error(message + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        std::string message{"unable to determine size of "};
        message += description;
        throw std::runtime_error(message);
    }
    if (static_cast<std::size_t>(end) != expected_bytes) {
        std::string message{description};
        message += " must be exactly ";
        message += std::to_string(expected_bytes);
        message += " bytes (got ";
        message += std::to_string(end);
        message += ")";
        throw std::invalid_argument(message);
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(expected_bytes);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::streamsize>(bytes.size()) != input.gcount() || input.rdstate() != std::ios::goodbit) {
        std::string message{"unable to read complete "};
        message += description;
        throw std::runtime_error(message);
    }
    return bytes;
}

void load_raw_ee_ram(const std::filesystem::path& path, guest::EeMemory& memory) {
    constexpr std::size_t kImageBytes = 33554432U;
    constexpr std::size_t kChunkBytes = 65536U;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("unable to open raw EE-RAM image");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("unable to determine raw EE-RAM image size");
    }
    const auto byte_count = static_cast<std::size_t>(end);
    if (byte_count != kImageBytes) {
        std::string message{"raw EE-RAM image must be exactly "};
        message += std::to_string(kImageBytes);
        message += " bytes (got ";
        message += std::to_string(byte_count);
        message += ")";
        throw std::invalid_argument(message);
    }
    input.seekg(0, std::ios::beg);
    std::array<std::byte, kChunkBytes> chunk;
    std::size_t destination_offset = 0U;
    while (destination_offset < kImageBytes) {
        const std::size_t chunk_size = destination_offset + kChunkBytes <= kImageBytes
                                           ? kChunkBytes
                                           : kImageBytes - destination_offset;
        input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk_size));
        if (static_cast<std::streamsize>(chunk_size) != input.gcount() || input.rdstate() != std::ios::goodbit) {
            throw std::runtime_error("unable to read complete raw EE-RAM image");
        }
        memory.write_main_ram_bytes(static_cast<guest::GuestAddress>(destination_offset),
                                    std::span<const std::byte>(chunk.data(), chunk_size));
        destination_offset += chunk_size;
    }
}

} // namespace

Vif1ReplayResult replay_vif1_from_raw_ee_ram(const std::filesystem::path& raw_ram_path,
                                            const guest::GuestAddress tadr, const Vif1ReplayOptions& options) {
    validate_source_chain_tadr(tadr, "VIF1");
    guest::EeMemory memory;
    load_raw_ee_ram(raw_ram_path, memory);
    Vif1ReplayResult result{};
    Vif1ReplayResult::Pcsx2VuSeed pcsx2_vu_seed{};
    if (options.pcsx2_vu_state_directory.has_value()) {
        const auto& directory = *options.pcsx2_vu_state_directory;
        static_cast<void>(read_exact_binary(directory / "vu1Memory.bin", 16384U, "PCSX2 vu1Memory.bin"));
        const auto vu_micro = read_exact_binary(directory / "vu1MicroMem.bin", 16384U, "PCSX2 vu1MicroMem.bin");
        const auto ee_hw = read_exact_binary(directory / "eeHwRegs.bin", 65536U, "PCSX2 eeHwRegs.bin");
        memory.replace_vu1_micro_memory(std::as_bytes(std::span<const std::uint8_t>(vu_micro)));
        const std::uint32_t cycle = read_u32_le(ee_hw, 15424U, "PCSX2 VIF1 CYCLE");
        const std::uint32_t mode = read_u32_le(ee_hw, 15440U, "PCSX2 VIF1 MODE");
        const std::uint32_t mask = read_u32_le(ee_hw, 15472U, "PCSX2 VIF1 MASK");
        const std::uint32_t itops = read_u32_le(ee_hw, 15504U, "PCSX2 VIF1 ITOPS");
        const std::uint32_t base = read_u32_le(ee_hw, 15520U, "PCSX2 VIF1 BASE");
        const std::uint32_t offset = read_u32_le(ee_hw, 15536U, "PCSX2 VIF1 OFFSET");
        memory.write_u32_le(0x10005000U, (cycle & 0xFFFFU) | 0x01000000U);
        memory.write_u32_le(0x10005000U, (base & 1023U) | 0x03000000U);
        memory.write_u32_le(0x10005000U, (offset & 1023U) | 0x02000000U);
        memory.write_u32_le(0x10005000U, (itops & 1023U) | 0x04000000U);
        memory.write_u32_le(0x10005000U, (mode & 3U) | 0x05000000U);
        memory.write_u32_le(0x10005000U, 0x20000000U);
        memory.write_u32_le(0x10005000U, mask);
        memory.write_u32_le(0x10005000U, 0x30000000U);
        for (std::uint32_t lane = 0U; lane != 4U; ++lane) {
            const std::uint32_t row = read_u32_le(ee_hw, (976U + lane) * 16U, "PCSX2 VIF1 ROW");
            memory.write_u32_le(0x10005000U, row);
        }
        memory.write_u32_le(0x10005000U, 0x31000000U);
        for (std::uint32_t lane = 0U; lane != 4U; ++lane) {
            const std::uint32_t col = read_u32_le(ee_hw, (980U + lane) * 16U, "PCSX2 VIF1 COL");
            memory.write_u32_le(0x10005000U, col);
        }
        pcsx2_vu_seed.loaded = true;
        pcsx2_vu_seed.cycle = cycle;
        pcsx2_vu_seed.mode = mode;
        pcsx2_vu_seed.mask = mask;
        pcsx2_vu_seed.itops = itops;
        pcsx2_vu_seed.base = base;
        pcsx2_vu_seed.offset = offset;
        pcsx2_vu_seed.register_seed_words = 17U;
    }
    Vif1ReplayGsFingerprint gs_fingerprint{};
    GsFingerprintObserver gs_observer{gs_fingerprint};
    gs_fingerprint.enabled = true;
    gs_fingerprint.fnv1a64 = 14695981039346656037ULL;
    if (options.fingerprint_gs_commands || options.raw_gif_observer != nullptr) {
        memory.set_gs_capture_enabled(true);
        memory.gs_capture().set_image_history_enabled(false);
    }
    if (options.fingerprint_gs_commands) {
        memory.gs_capture().set_command_observer(&gs_observer);
    }
    memory.gs_capture().set_raw_gif_observer(options.raw_gif_observer);
    memory.write_u32_le(0x10009030U, tadr);
    const std::uint32_t chcr = options.tag_transfer_enable ? 325U : 261U;
    memory.write_u32_le(0x10009000U, chcr);
    memory.vu1_worker_consume_fence(runtime::Vu1WorkerConsumeReason::Sync);
    if (options.fingerprint_gs_commands) {
        gs_fingerprint.diagnostics = memory.gs_capture().diagnostics().size();
        gs_fingerprint.capture_activity = memory.gs_capture().activity();
    } else {
        gs_fingerprint = {};
    }
    result.activity = memory.vu1_activity();
    result.final_chcr = memory.read_u32_le(0x10009000U);
    result.final_madr = memory.read_u32_le(0x10009010U);
    result.final_qwc = memory.read_u32_le(0x10009020U);
    result.final_tadr = memory.read_u32_le(0x10009030U);
    result.final_asr0 = memory.read_u32_le(0x10009040U);
    result.final_asr1 = memory.read_u32_le(0x10009050U);
    result.completion_events = memory.dmac_completion_counts()[1];
    result.queued_completion_events = memory.queued_dmac_completion_count();
    result.pcsx2_vu_seed = pcsx2_vu_seed;
    result.gs_fingerprint = gs_fingerprint;
    return result;
}

Vif1ReplayResult replay_vif1_from_raw_ee_ram(const std::filesystem::path& raw_ram_path,
                                            const guest::GuestAddress tadr, const bool tag_transfer_enable,
                                            const bool fingerprint_gs_commands) {
    Vif1ReplayOptions options{};
    options.tag_transfer_enable = tag_transfer_enable;
    options.fingerprint_gs_commands = fingerprint_gs_commands;
    return replay_vif1_from_raw_ee_ram(raw_ram_path, tadr, options);
}

GifReplayResult replay_gif_from_raw_ee_ram(const std::filesystem::path& raw_ram_path,
                                           const guest::GuestAddress tadr) {
    validate_source_chain_tadr(tadr, "GIF");
    guest::EeMemory memory;
    load_raw_ee_ram(raw_ram_path, memory);
    GifReplayResult result{};
    Vif1ReplayGsFingerprint gs_fingerprint{};
    GsFingerprintObserver gs_observer{gs_fingerprint};
    gs_fingerprint.enabled = true;
    gs_fingerprint.fnv1a64 = 14695981039346656037ULL;
    memory.set_gs_capture_enabled(true);
    memory.gs_capture().set_image_history_enabled(false);
    memory.gs_capture().set_command_observer(&gs_observer);
    memory.write_u32_le(0x1000A030U, tadr);
    memory.write_u32_le(0x1000A000U, 260U);
    gs_fingerprint.diagnostics = memory.gs_capture().diagnostics().size();
    gs_fingerprint.capture_activity = memory.gs_capture().activity();
    result.final_chcr = memory.read_u32_le(0x1000A000U);
    result.completion_events = memory.dmac_completion_counts()[2];
    result.queued_completion_events = memory.queued_dmac_completion_count();
    result.gs_fingerprint = gs_fingerprint;
    return result;
}

} // namespace takedown::runtime
