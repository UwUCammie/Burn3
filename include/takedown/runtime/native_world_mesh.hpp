#pragma once

// P2 WP2: native import of world / prop mesh packets from guest RAM.
//
// The guest keeps every world strip and prop mesh as a static DMA packet (record.u32[3], continuation chain record.u32[7],
// section pairs {mesh, continuation}): `STMOD 0, STCYCL CL=3 WL=1, UNPACK V4-32 @0x8000 (pos xyz, w = strip flags),
// UNPACK V2-16 @0x8001 (st / 4096; props: V2-32 floats), UNPACK V4-8 @0xC002 (RGBA) or V3-8 (specular layout) plus a
// V4-8 signed @204 block of s8 normals + intensity (programs 3/4), ITOP n`, one group (<= 80 vertices) per VU1 run.
//
// This file turns such a packet into the StripMesh the emitter builds from VU1 data memory today (native_cmd_decode.hpp:
// group_hash / decode_world3 / decode_prop), with the SAME group_hash words, so a mesh key from here equals the key of the
// trusted-launch path. The VIF1 UNPACK model is a port of EeMemory::expand_vif1_unpack_into / finish_unpack (src/guest/
// ee_memory.cpp), including the V3 lane-3 look-ahead quirk that decides the stale alpha lane of the specular colours.
//
// Threading: reads guest RAM, so it runs on the emulation thread; the result is immutable (shared_ptr<const>) and may be
// handed to the device-model thread.

#include "takedown/platform/native_cmd_decode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace takedown::runtime::world_mesh {

namespace nc = takedown::native_cmd;

// ---- guest memory -------------------------------------------------------------------------------------------------
class QwReader {
  public:
    virtual ~QwReader() = default;
    // One aligned quadword (4 little-endian words); false when the address is not readable main RAM.
    [[nodiscard]] virtual bool read_qw(std::uint32_t address, std::uint32_t out[4]) const noexcept = 0;
    [[nodiscard]] bool read_u32(std::uint32_t address, std::uint32_t& out) const noexcept;
};

// Main RAM as a byte span (EeMemory::bytes()); kuseg / KSEG0 / KSEG1 aliases and the 0x20080000 / 0x30100000 windows map like
// EeMemory::main_ram_offset. Scratchpad, VU and device addresses are not readable.
class MainRamView final : public QwReader {
  public:
    MainRamView(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    [[nodiscard]] bool read_qw(std::uint32_t address, std::uint32_t out[4]) const noexcept override;

  private:
    const std::uint8_t* data_;
    std::size_t size_;
};

// ---- programs -----------------------------------------------------------------------------------------------------
// Material kind / flags -> the VU1 program number the guest's group prologue (0x26b2f0) selects:
// kind 0/6: 10 (flags & 0x20: 11), kind 1: 3 (4), kind 3: 13, kind 7: 12, kinds 4/5: 14. 0 = no native program (kind 2, unknown).
[[nodiscard]] std::uint16_t program_for_material(std::uint32_t kind, std::uint32_t flags) noexcept;
[[nodiscard]] nc::Decoder decoder_for_program(std::uint16_t program) noexcept; // kNone when not a world / prop program
// The emitter's seed for group_hash: program * 0x9E3779B1 + 1.
[[nodiscard]] std::uint64_t mesh_seed(std::uint16_t program) noexcept;

// ---- import -------------------------------------------------------------------------------------------------------
enum class ImportStatus : std::uint8_t {
    kOk,
    kUnreadable, // an address outside main RAM
    kBadTag,     // unknown / unsupported DMA tag or chain too long
    kBadCommand, // a VIF command the world grammar does not use (STMOD != 0, masked UNPACK, ...)
    kBadGroup,   // ITOP count out of range, or a vertex the UNPACKs did not write (stale VU memory would be read)
    kNoGroups,
};

struct ImportOptions {
    // Program 254 (scrolling arrow-board textures) adds an offset to every u / v and the emitter mixes it into the key.
    bool scroll{false};
    float uv_offset[2]{0.0F, 0.0F};
};

struct ImportedGroup {
    std::uint64_t key{};   // MeshEntry::wkey
    std::uint32_t n{};     // vertices (ITOP)
    // Blocks the packet does not carry (untextured meshes have no V2 UNPACK, line meshes no uv at all): the VU1 program never reads
    // them and the trusted-launch path hashes whatever stale words the buffer held, so its key for such a group is unstable. Here
    // the missing block is all zero words (kMissingUv / kMissingColour) and the key is stable.
    static constexpr std::uint8_t kMissingUv = 1U, kMissingColour = 2U;
    std::uint8_t missing{};
    nc::StripMesh mesh;
    float lo[3]{}, hi[3]{}; // bounding box of the vertex positions
};

struct ImportedMesh {
    ImportStatus status{ImportStatus::kNoGroups};
    std::uint16_t program{};
    std::vector<ImportedGroup> groups; // the initial packet's group(s), then the continuation chain's
    std::uint32_t packet{}, cont{};
    std::uint32_t qw_read{};
    [[nodiscard]] std::uint64_t bytes() const noexcept;
};

// Decodes one packet chain (a DMA CALL target; RET / END terminated, nested CALLs followed) into groups. Appends to `out`.
ImportStatus import_packet(const QwReader& ram, std::uint32_t address, std::uint16_t program, const ImportOptions& options,
                           std::vector<ImportedGroup>& out, std::uint32_t* qw_read = nullptr);
// The mesh of one world object / prop section pair: `packet` then `cont` (0 = none).
std::shared_ptr<ImportedMesh> import_mesh(const QwReader& ram, std::uint32_t packet, std::uint32_t cont, std::uint16_t program,
                                          const ImportOptions& options = {});

// ---- validity -----------------------------------------------------------------------------------------------------
// Every streamed cell is a resource block with the header {u32 status (2 = loaded), u32 cell id, u32 size}. There is no unload
// event, so a registry entry remembers the header of the block that contains its packet and is valid only while the header in
// RAM still equals the snapshot. Blocks are listed in the world context (WCTX = 0x64c9a8 + 460 * player): grid dimensions at
// +8 / +12, block pointers at +340 (x) and +380 (y).
struct BlockHeader {
    std::uint32_t status{}, id{}, size{};
    bool operator==(const BlockHeader&) const = default;
};
struct OwnerBlock {
    std::uint32_t base{}, end{};
    BlockHeader header;
};
class OwnerTable {
  public:
    static constexpr std::uint32_t kGridXDimOffset = 8U, kGridYDimOffset = 12U, kGridXOffset = 340U, kGridYOffset = 380U;
    void clear() { blocks_.clear(); }
    // Re-reads the two grids (blocks that are still loading stay listed; registry users check the status). Returns the block count.
    std::size_t rebuild(const QwReader& ram, std::uint32_t wctx);
    void set_blocks(std::vector<OwnerBlock> blocks); // tests / other sources; sorted by base
    // The block containing `address` (base <= address < end) or nullptr.
    [[nodiscard]] const OwnerBlock* find(std::uint32_t address) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return blocks_.size(); }

  private:
    std::vector<OwnerBlock> blocks_;
};

// Registry keyed by packet address; entries are shared_ptr<const ImportedMesh>.
class WorldMeshRegistry {
  public:
    struct Stats {
        std::uint64_t hits{}, imports{}, failures{}, invalidated{}, evicted{};
    };
    struct Config {
        bool verify_every_use{false}; // debug: re-decode on every use and compare the keys
        std::uint64_t max_bytes{256ULL << 20U};
        std::uint32_t idle_frames{1800U}; // entries unused for this many frames are evicted by begin_frame
    };
    WorldMeshRegistry() = default;
    explicit WorldMeshRegistry(const Config& config) : config_(config) {}

    // Entries of an older epoch are dropped on next use (world topology pointer changed / track reload).
    void set_epoch(std::uint64_t epoch) noexcept { epoch_ = epoch; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    OwnerTable& owners() noexcept { return owners_; }
    void begin_frame();

    // Returns the mesh (status kOk) or nullptr when the packet cannot be imported; failures are remembered (until the owner
    // block or the epoch changes) so a declined packet is not decoded again every frame.
    std::shared_ptr<const ImportedMesh> acquire(const QwReader& ram, std::uint32_t packet, std::uint32_t cont, std::uint16_t program,
                                                const ImportOptions& options = {});
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void clear();

  private:
    struct Key {
        std::uint32_t packet{}, cont{};
        std::uint16_t program{};
        std::uint8_t scroll{};
        std::uint32_t uv_bits[2]{};
        bool operator<(const Key& o) const noexcept;
    };
    struct Snapshot {
        std::uint32_t tag[2]{};  // first word of the packet's DMA tag (qwc, id) for packet / cont
        std::uint32_t last[2]{}; // hash of the packet's last payload quadword
        bool has_owner[2]{};
        OwnerBlock owner[2];
    };
    struct Entry {
        std::shared_ptr<ImportedMesh> mesh; // null = remembered failure
        Snapshot snap;
        std::uint64_t epoch{};
        std::uint64_t last_frame{};
    };
    [[nodiscard]] bool snapshot_valid(const QwReader& ram, const Snapshot& snap, std::uint32_t packet, std::uint32_t cont) const noexcept;
    [[nodiscard]] Snapshot take_snapshot(const QwReader& ram, std::uint32_t packet, std::uint32_t cont) const noexcept;

    Config config_;
    OwnerTable owners_;
    std::map<Key, Entry> entries_;
    std::uint64_t epoch_{1};
    std::uint64_t frame_{};
    std::uint64_t bytes_{};
    Stats stats_;
};

} // namespace takedown::runtime::world_mesh
