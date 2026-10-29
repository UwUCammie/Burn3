#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/presentation/texture_image.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace takedown::guest {

class GsSoftwareRasterizer;

// GS local memory is a 4 MiB circular store addressed in 256-byte blocks.
// These constants intentionally describe storage only; they do not imply a
// complete GIF primitive renderer or CRTC scanout implementation.
inline constexpr std::size_t kGsLocalMemoryBytes = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kGsLocalMemoryBlockBytes = 256U;
inline constexpr std::uint32_t kGsLocalMemoryBlocks =
    static_cast<std::uint32_t>(kGsLocalMemoryBytes / kGsLocalMemoryBlockBytes);

// Conservative physical-block coverage for a GS local-memory image. One bit
// represents one circular 256-byte GS block.
using GsLocalMemoryBlockFootprint = std::bitset<kGsLocalMemoryBlocks>;

// Opaque point in one tracked GsLocalMemory lifetime. Copies of GS memory do
// not inherit this identity, so a checkpoint can never authenticate a replay
// or scanout sidecar which merely contains the same bytes.
class GsLocalMemoryMutationCheckpoint final {
  public:
    constexpr GsLocalMemoryMutationCheckpoint() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return tracker_identity_ != 0U; }

    bool operator==(const GsLocalMemoryMutationCheckpoint&) const = default;

  private:
    friend class GsLocalMemory;

    constexpr GsLocalMemoryMutationCheckpoint(const std::uint64_t tracker_identity,
                                              const std::uint64_t completed_epoch) noexcept
        : tracker_identity_(tracker_identity), completed_epoch_(completed_epoch) {}

    std::uint64_t tracker_identity_{};
    std::uint64_t completed_epoch_{};
};

enum class GsLocalMemoryMutationStatus : std::uint8_t {
    Unchanged,
    Changed,
    Untrackable,
};

inline constexpr std::uint32_t kGsPsmct32 = 0x00U;
inline constexpr std::uint32_t kGsPsmct24 = 0x01U;
inline constexpr std::uint32_t kGsPsmct16 = 0x02U;
inline constexpr std::uint32_t kGsPsmct16s = 0x0aU;
inline constexpr std::uint32_t kGsPsmt8 = 0x13U;
inline constexpr std::uint32_t kGsPsmt4 = 0x14U;
inline constexpr std::uint32_t kGsPsmt8h = 0x1bU;
inline constexpr std::uint32_t kGsPsmz32 = 0x30U;
inline constexpr std::uint32_t kGsPsmz24 = 0x31U;
inline constexpr std::uint32_t kGsPsmz16 = 0x32U;
inline constexpr std::uint32_t kGsPsmz16s = 0x3aU;

enum class GsLocalMemoryStatus : std::uint8_t {
    Applied,
    UnsupportedPixelStorageMode,
    ZeroBufferWidth,
    InvalidBasePointer,
    InvalidGeometry,
    InvalidPayload,
};

[[nodiscard]] const char* to_string(GsLocalMemoryStatus status) noexcept;

// Base pointers are in the GS's 256-byte block units. BITBLTBUF.DBP can be
// used directly; DISPFB.FBP needs converting to those units before use.
struct GsLocalMemorySurface {
    std::uint32_t base_pointer{};
    std::uint32_t buffer_width{};
    std::uint32_t pixel_storage_mode{};

    bool operator==(const GsLocalMemorySurface&) const = default;
};

using GsRgba8 = presentation::Rgba8;

struct GsLocalMemoryApplyResult {
    GsLocalMemoryStatus status{GsLocalMemoryStatus::InvalidGeometry};
    std::size_t bytes_written{};

    [[nodiscard]] bool applied() const noexcept { return status == GsLocalMemoryStatus::Applied; }
};

// One GS local-to-local transfer, latched when TRXDIR.XDIR is written as 2.
// TRXPOS.DIRX/DIRY select the pixel transmission order; they are retained
// separately rather than encoded into the coordinates so overlapping moves
// can follow the GS's ordered read/write behavior.
struct GsLocalToLocalTransfer {
    GsLocalMemorySurface source{};
    GsLocalMemorySurface destination{};
    std::uint32_t source_x{};
    std::uint32_t source_y{};
    std::uint32_t destination_x{};
    std::uint32_t destination_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool reverse_x{};
    bool reverse_y{};
};

// Correctly swizzled storage for the first directly useful GS image formats.
// PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, and PSMT8H
// host-to-local IMAGE transfers are supported. Indexed PSMs retain their raw
// palette indices; palette/CLUT lookup remains outside this storage class.
class GsLocalMemory {
  public:
    GsLocalMemory();
    ~GsLocalMemory();

    // Copies intentionally carry only the 4 MiB byte image. Mutation history
    // belongs to the live renderer instance and is disabled in scanout,
    // feedback, and replay copies. Moves transfer both bytes and history.
    GsLocalMemory(const GsLocalMemory& other);
    GsLocalMemory& operator=(const GsLocalMemory& other);
    GsLocalMemory(GsLocalMemory&& other) noexcept;
    GsLocalMemory& operator=(GsLocalMemory&& other) noexcept;

    // Tracking is opt-in so standalone memory users retain their previous
    // footprint. The optional ceiling is a deterministic exhaustion seam;
    // production callers use the uint64_t maximum. Once exhausted, queries
    // remain Untrackable and callers must use an exact byte/image comparison.
    void enable_mutation_tracking(
        std::uint64_t maximum_epoch = std::numeric_limits<std::uint64_t>::max());
    void disable_mutation_tracking() noexcept;
    [[nodiscard]] GsLocalMemoryMutationCheckpoint capture_mutation_checkpoint() const noexcept;
    // O(1) lifetime/epoch check used before constructing a physical footprint.
    // Changed means some block changed; classify_mutations_since() narrows it
    // to a caller-provided footprint.
    [[nodiscard]] GsLocalMemoryMutationStatus
    classify_any_mutations_since(GsLocalMemoryMutationCheckpoint checkpoint) const noexcept;
    [[nodiscard]] GsLocalMemoryMutationStatus
    classify_mutations_since(GsLocalMemoryMutationCheckpoint checkpoint,
                             const GsLocalMemoryBlockFootprint& footprint) const noexcept;

    [[nodiscard]] GsLocalMemoryApplyResult apply_host_to_local(const GsImageTransfer& transfer);
    [[nodiscard]] GsLocalMemoryApplyResult
    apply_local_to_local(const GsLocalToLocalTransfer& transfer) noexcept;

    // Replaces the complete 4 MiB store atomically. This is used by local
    // replay/verification tools which begin from an already serialized GS
    // state; a partial backing is rejected without changing memory.
    [[nodiscard]] bool load_raw_backing(std::span<const std::uint8_t> backing) noexcept;

    // Writes one framebuffer pixel through the same swizzled address path as
    // image uploads and scanout reads. This is the deliberately small write
    // boundary used by the first software primitive rasterizer.
    [[nodiscard]] GsLocalMemoryStatus write_rgba8(const GsLocalMemorySurface& surface,
                                                  std::uint32_t x, std::uint32_t y,
                                                  GsRgba8 color) noexcept;

    // Returns a decoded pixel for direct-color storage only. Indexed PSMs
    // deliberately return no colour until a separate CLUT implementation is
    // available. Coordinates are intentionally not clamped: the GS address
    // calculation proceeds through its circular local-memory mapping.
    [[nodiscard]] std::optional<GsRgba8> read_rgba8(const GsLocalMemorySurface& surface,
                                                    std::uint32_t x,
                                                    std::uint32_t y) const noexcept;

    // Reads one direct-colour texel with the GS texture-fetch expansion rules.
    // Unlike read_rgba8(), this accepts the depth PSMs when they are selected
    // by TEX0 and applies TEXA to 24/16-bit data. In particular, PSMZ16 and
    // PSMZ16S retain their depth swizzle but their raw RGB5551 word is decoded
    // as colour: bit 15 selects TA0/TA1, and AEM makes only an all-zero raw
    // word transparent. Indexed formats still require the separate CLUT path.
    [[nodiscard]] std::optional<GsRgba8>
    read_texture_rgba8(const GsLocalMemorySurface& surface, std::uint32_t x, std::uint32_t y,
                       std::uint64_t texa) const noexcept;

    // Depth-buffer access for the PSMZ32/PSMZ24/PSMZ16/PSMZ16S formats used
    // by GS depth testing. The z value stored by PSMZ24 keeps its 24
    // significant bits; PSMZ16 stores the low 16 bits. The returned value is
    // always the full stored word width for the format.
    [[nodiscard]] std::optional<std::uint32_t>
    read_z(const GsLocalMemorySurface& surface, std::uint32_t x, std::uint32_t y) const noexcept;
    [[nodiscard]] GsLocalMemoryStatus write_z(const GsLocalMemorySurface& surface, std::uint32_t x,
                                              std::uint32_t y, std::uint32_t z) noexcept;

    // Reads and writes raw PSMT8/PSMT4 palette indices through the same
    // swizzled address path. PSMT4 values use the low four bits and preserve
    // their byte neighbour. These methods do not perform a CLUT lookup.
    [[nodiscard]] GsLocalMemoryStatus write_index(const GsLocalMemorySurface& surface,
                                                  std::uint32_t x, std::uint32_t y,
                                                  std::uint8_t index) noexcept;
    [[nodiscard]] std::optional<std::uint8_t> read_index(const GsLocalMemorySurface& surface,
                                                         std::uint32_t x,
                                                         std::uint32_t y) const noexcept;

    [[nodiscard]] std::span<const std::uint8_t> raw_backing() const noexcept;
    void clear() noexcept;

  private:
    friend class GsSoftwareRasterizer;

    struct MutationTracker;

    class MutationBatch final {
      public:
        explicit MutationBatch(GsLocalMemory& memory) noexcept;
        ~MutationBatch();

        MutationBatch(const MutationBatch&) = delete;
        MutationBatch& operator=(const MutationBatch&) = delete;
        MutationBatch(MutationBatch&& other) noexcept;
        MutationBatch& operator=(MutationBatch&&) = delete;

      private:
        GsLocalMemory* memory_{};
        bool outermost_{};
    };

    struct PixelAddress {
        std::size_t byte{};
        std::uint8_t nibble_shift{};
    };

    [[nodiscard]] static std::optional<PixelAddress>
    pixel_address(const GsLocalMemorySurface& surface, std::uint64_t x, std::uint64_t y) noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    read_transfer_pixel(const GsLocalMemorySurface& surface, std::uint32_t x,
                        std::uint32_t y) const noexcept;
    [[nodiscard]] GsLocalMemoryStatus
    write_transfer_pixel(const GsLocalMemorySurface& surface, std::uint32_t x, std::uint32_t y,
                         std::uint32_t pixel) noexcept;

    // The software rasterizer resolves a depth pixel once, reads it for the
    // test, and reuses the byte address for the later write. Keeping these
    // helpers private prevents callers from constructing unchecked local-
    // memory offsets while avoiding a second full Z swizzle per fragment.
    // kGsLocalMemoryBytes is returned as an invalid-address sentinel; every
    // valid pixel byte is strictly below it.
    [[nodiscard]] static std::size_t depth_pixel_byte(const GsLocalMemorySurface& surface,
                                                      std::uint32_t x, std::uint32_t y) noexcept;
    [[nodiscard]] std::uint32_t read_z_at(std::uint32_t psm, std::size_t byte) const noexcept;
    void write_z_at(std::uint32_t psm, std::size_t byte, std::uint32_t z) noexcept;

    [[nodiscard]] MutationBatch begin_mutation_batch() noexcept;
    [[nodiscard]] bool open_mutation_batch() noexcept;
    void close_mutation_batch() noexcept;
    void record_mutated_byte(std::size_t byte) noexcept;
    void record_all_blocks_mutated() noexcept;

    std::vector<std::uint8_t> raw_backing_{};
    std::unique_ptr<MutationTracker> mutation_tracker_{};
};

} // namespace takedown::guest
