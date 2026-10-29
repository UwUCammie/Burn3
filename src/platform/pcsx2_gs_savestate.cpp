#include "takedown/platform/pcsx2_gs_savestate.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace takedown::platform {

namespace {

// PCSX2 pads the ASCII "vuMicroRegs" tag to 32 bytes inside Internal Structures.
constexpr std::array<std::uint8_t, 32U> kVuMicroRegsTag = {"vuMicroRegs"};

// Offsets of the two drawing contexts' register blocks inside the GS state.
constexpr std::array<unsigned long long, 2U> kContextOffsets = {124U, 220U};

constexpr std::size_t kPrivilegedRegisterBlockBytes = 8192U;
constexpr std::size_t kVideoModeBytesBeforeTag = 4U;
constexpr std::size_t kMaximumInternalStructuresBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaximumGsBinBytes = kPcsx2GsStateVersion9Bytes;
constexpr std::uint32_t kPackedQOne = 0x3f800000U;
constexpr std::size_t kGifPathCount = 4U;
constexpr std::size_t kGifPathBytes = 20U;
constexpr std::size_t kGifPathsOffset = kPcsx2GsStateVersion9LocalMemoryOffset + kPcsx2GsLocalMemoryBytes;

[[noreturn]] void malformed_savestate(const std::string& detail) {
    throw std::runtime_error("invalid PCSX2 GS savestate: " + detail);
}

void require_bytes(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t count,
                   const char* const detail) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        malformed_savestate(detail);
    }
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                                     const char* detail) {
    require_bytes(bytes, offset, 4U, detail);
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint64_t read_u64(const std::span<const std::uint8_t> bytes, const std::size_t offset, const char* detail) {
    require_bytes(bytes, offset, 8U, detail);
    std::uint64_t value = 0U;
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] std::vector<std::uint8_t> read_bounded_file(const std::filesystem::path& path,
                                                          const std::size_t maximum_bytes) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("unable to inspect PCSX2 savestate entry " + path.string() + ": " + error.message());
    }
    if (file_bytes == 0U || file_bytes > maximum_bytes) {
        throw std::runtime_error("PCSX2 savestate entry has an invalid size: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (input.fail()) {
        throw std::runtime_error("unable to open PCSX2 savestate entry: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_bytes));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.fail()) {
        throw std::runtime_error("unable to read complete PCSX2 savestate entry: " + path.string());
    }
    return bytes;
}

// Locates the single padded vuMicroRegs tag. The privileged-register block and the
// video-mode word sit immediately before it, so the caller validates the offset.
[[nodiscard]] std::size_t find_vu_micro_regs_tag_offset(const std::span<const std::uint8_t> internal_structures) {
    const auto tag = std::search(internal_structures.begin(), internal_structures.end(), kVuMicroRegsTag.begin(),
                                 kVuMicroRegsTag.end());
    if (tag == internal_structures.end()) {
        malformed_savestate("missing padded vuMicroRegs tag in internal structures");
    }
    const auto next = tag + 1;
    if (std::search(next, internal_structures.end(), kVuMicroRegsTag.begin(), kVuMicroRegsTag.end()) !=
        internal_structures.end()) {
        malformed_savestate("duplicate padded vuMicroRegs tags in internal structures");
    }
    return static_cast<std::size_t>(std::distance(internal_structures.begin(), tag));
}

} // namespace

Pcsx2GsStateSeed parse_pcsx2_gs_state_seed_bytes(const std::span<const std::uint8_t> gs_bin,
                                                 const std::span<const std::uint8_t> internal_structures) {
    if (gs_bin.size() != kPcsx2GsStateVersion9Bytes) {
        malformed_savestate("GS.bin is not the exact GS state v9 size");
    }
    const std::uint32_t state_version = read_u32(gs_bin, 0U, "truncated GS.bin state version");
    if (state_version != kPcsx2GsStateVersion9) {
        malformed_savestate("unsupported GS.bin state version " + std::to_string(state_version));
    }
    require_bytes(gs_bin, kPcsx2GsStateVersion9LocalMemoryOffset, kPcsx2GsLocalMemoryBytes,
                  "truncated GS local memory");

    const std::size_t tag_offset = find_vu_micro_regs_tag_offset(internal_structures);
    if (tag_offset < kPrivilegedRegisterBlockBytes + kVideoModeBytesBeforeTag) {
        malformed_savestate("vuMicroRegs tag precedes a complete GS privileged-register block");
    }
    const auto privileged_offset = tag_offset - (kPrivilegedRegisterBlockBytes + kVideoModeBytesBeforeTag);
    const auto video_mode_offset = tag_offset - kVideoModeBytesBeforeTag;
    require_bytes(internal_structures, privileged_offset, kPrivilegedRegisterBlockBytes,
                  "truncated GS privileged-register block");
    const auto registers = internal_structures.subspan(privileged_offset, kPrivilegedRegisterBlockBytes);

    Pcsx2GsStateSeed seed{};
    auto& replay = seed.replay;
    replay.packed_q_bits = kPackedQOne;
    seed.state_version = kPcsx2GsStateVersion9;
    seed.video_mode = read_u32(internal_structures, video_mode_offset, "truncated GS video mode");
    const std::uint64_t pmode = read_u64(registers, 0U, "truncated PMODE register");
    const std::uint64_t smode2 = read_u64(registers, 32U, "truncated SMODE2 register");
    const std::uint64_t dispfb1 = read_u64(registers, 112U, "truncated DISPFB1 register");
    const std::uint64_t display1 = read_u64(registers, 128U, "truncated DISPLAY1 register");
    const std::uint64_t dispfb2 = read_u64(registers, 144U, "truncated DISPFB2 register");
    const std::uint64_t display2 = read_u64(registers, 160U, "truncated DISPLAY2 register");
    const std::uint64_t bgcolor = read_u64(registers, 224U, "truncated BGCOLOR register");
    seed.crtc.pmode = pmode;
    seed.crtc.smode2 = smode2;
    seed.crtc.dispfb1 = dispfb1;
    seed.crtc.display1 = display1;
    seed.crtc.dispfb2 = dispfb2;
    seed.crtc.display2 = display2;
    seed.crtc.bgcolor = bgcolor;

    replay.local_memory.resize(kPcsx2GsLocalMemoryBytes);
    replay.texa = read_u64(gs_bin, 36U, "truncated TEXA register");
    replay.colclamp = read_u64(gs_bin, 68U, "truncated COLCLAMP register");
    replay.bitbltbuf = read_u64(gs_bin, 84U, "truncated BITBLTBUF register");
    replay.trxdir = read_u64(gs_bin, 92U, "truncated TRXDIR register");
    replay.trxpos = read_u64(gs_bin, 100U, "truncated TRXPOS register");
    replay.trxreg = read_u64(gs_bin, 108U, "truncated TRXREG register");
    replay.transfer = guest::GsReplayTransferProgress{
        read_u32(gs_bin, 412U, "truncated transfer total"),
        read_u32(gs_bin, 416U, "truncated transfer start"),
        read_u32(gs_bin, 420U, "truncated transfer end"),
        gs_bin[424U] != 0U,
    };
    replay.packed_q_bits = read_u32(gs_bin, kPcsx2GsStateVersion9Bytes - 4U, "truncated PACKED Q latch");
    for (std::size_t path = 0U; path != kGifPathCount; ++path) {
        const std::uint64_t tag_word =
            read_u64(gs_bin, kGifPathsOffset + path * kGifPathBytes, "truncated GIF path state");
        if ((tag_word & 0x7fffU) != 0U) {
            ++replay.active_gif_paths;
        }
    }

    auto& drawing = replay.drawing;
    drawing.scanmsk = {read_u64(gs_bin, 28U, "truncated SCANMSK register"), true};
    drawing.dthe = {read_u64(gs_bin, 60U, "truncated DTHE register"), true};
    drawing.pabe = {read_u64(gs_bin, 76U, "truncated PABE register"), true};
    const std::uint64_t prim = read_u64(gs_bin, 4U, "truncated PRIM register");
    drawing.prim = {prim, true};
    drawing.prmodecont = {read_u64(gs_bin, 12U, "truncated PRMODECONT register"), true};
    drawing.prmode = {prim, true};
    for (std::size_t context = 0U; context != 2U; ++context) {
        const auto base = kContextOffsets[context];
        drawing.fba[context] = {read_u64(gs_bin, base + 72U, "truncated FBA register"), true};
        auto& state = drawing.contexts[context];
        state.xyoffset = {read_u64(gs_bin, base, "truncated XYOFFSET register"), true};
        state.tex0 = {read_u64(gs_bin, base + 8U, "truncated TEX0 register"), true};
        state.tex1 = {read_u64(gs_bin, base + 16U, "truncated TEX1 register"), true};
        state.clamp = {read_u64(gs_bin, base + 24U, "truncated CLAMP register"), true};
        state.miptbp1 = {read_u64(gs_bin, base + 32U, "truncated MIPTBP1 register"), true};
        state.miptbp2 = {read_u64(gs_bin, base + 40U, "truncated MIPTBP2 register"), true};
        state.scissor = {read_u64(gs_bin, base + 48U, "truncated SCISSOR register"), true};
        state.alpha = {read_u64(gs_bin, base + 56U, "truncated ALPHA register"), true};
        state.test = {read_u64(gs_bin, base + 64U, "truncated TEST register"), true};
        state.frame = {read_u64(gs_bin, base + 80U, "truncated FRAME register"), true};
        state.zbuf = {read_u64(gs_bin, base + 88U, "truncated ZBUF register"), true};
    }

    drawing.vertex.rgbaq = {read_u64(gs_bin, 316U, "truncated RGBAQ register"), true};
    drawing.vertex.st = {read_u64(gs_bin, 324U, "truncated ST register"), true};
    drawing.vertex.uv = {read_u32(gs_bin, 332U, "truncated UV register"), true};
    drawing.vertex.position = {read_u64(gs_bin, 340U, "truncated XYZ register"), true};
    drawing.vertex.position_kind = guest::GsVertexPositionKind::Xyz;
    drawing.vertex.position_register_address = static_cast<std::uint8_t>(guest::GsRegisterAddress::Xyz2);

    std::copy_n(gs_bin.data() + kPcsx2GsStateVersion9LocalMemoryOffset, kPcsx2GsLocalMemoryBytes,
                replay.local_memory.begin());
    return seed;
}

Pcsx2GsScanoutSeed parse_pcsx2_gs_scanout_seed_bytes(const std::span<const std::uint8_t> gs_bin,
                                                     const std::span<const std::uint8_t> internal_structures) {
    auto state = parse_pcsx2_gs_state_seed_bytes(gs_bin, internal_structures);
    return Pcsx2GsScanoutSeed{
        state.state_version,
        state.video_mode,
        std::move(state.replay.local_memory),
        state.crtc,
    };
}

Pcsx2GsScanoutSeed load_pcsx2_gs_scanout_seed(const std::filesystem::path& gs_bin_path,
                                              const std::filesystem::path& internal_structures_path) {
    const auto gs_bin = read_bounded_file(gs_bin_path, kMaximumGsBinBytes);
    const auto internal_structures = read_bounded_file(internal_structures_path, kMaximumInternalStructuresBytes);
    return parse_pcsx2_gs_scanout_seed_bytes(gs_bin, internal_structures);
}

Pcsx2GsStateSeed load_pcsx2_gs_state_seed(const std::filesystem::path& gs_bin_path,
                                          const std::filesystem::path& internal_structures_path) {
    const auto gs_bin = read_bounded_file(gs_bin_path, kMaximumGsBinBytes);
    const auto internal_structures = read_bounded_file(internal_structures_path, kMaximumInternalStructuresBytes);
    return parse_pcsx2_gs_state_seed_bytes(gs_bin, internal_structures);
}

} // namespace takedown::platform
