#pragma once

// P2 WP3: the producer core of the native SubmitWorldObject (original 0x0026af50), free of the dispatcher and of EeMemory so the offline
// test can drive it over captured memory (tests/test_native_world_submit_producer.cpp). The dispatcher glue is
// slus_21050_native_world_submit.{hpp,cpp}.
//
// What the guest routine does (recon-p2-world.md 1.3, 4) and what the core keeps of it:
//   * unless a2 == 1 the object's OBB {corner record+48, edges +0/+16/+32} is classified against the 6 planes of the view ctx.s8[153]
//     at 0x01e327d0 + 96 * view (engine::classify, packet-exact with the VU0 program 0x143). Outside -> v0 = 0 and nothing else changes.
//     Inside -> a2 := 1, intersecting -> a2 unchanged.
//   * ctx.u8[155] = 1, ctx.s8[152] = a2, staging (pointer at gp-23648 = 0x004e2a10) +0 := 0 and +4 := the new write cursor.
//   * staging output: [ctx+0 qw (CNT 0 | FLUSHE | NOP)] [the Q queued view-select quadwords] [the 1013 pair when
//     (Q | (a2 & ~ctx152)) > 0: ctx+16 qw, {cmd, 0, a3 & 0xff, flags}] and then, in the original, [CALL record.mesh] in front and the
//     tail [CALL record.cont | MSCNT] / ctx+32 behind. The native writes the stub of a world marker (3 quadwords,
//     world_submit::write_stub) where the original had the CALL and the tail, and pushes a WorldSubmit with everything the emitter needs
//     to make the draws the VU1 launches of those packets would have produced. The queued state and the 1013 pair stay in the stream
//     exactly as the guest writes them (GS registers, VU1 qw 1013/1014..1023 for the guest-path objects that follow).
//   * never touched: ring pointer gp-24240, gp-28060 / gp-24080 / gp-22808, VU0 and VU1 state (recon section 4).
// Everything uncertain declines (the dispatcher then lets the guest run the original code unchanged).

#include "takedown/platform/native_world_submit.hpp"
#include "takedown/runtime/native_world_mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>

namespace takedown::runtime::slus_21050::world_submit_core {

namespace ws = platform::world_submit;
namespace nc = takedown::native_cmd;

// ---- addresses and layouts (US disc SLUS_210.50) ---------------------------------------------------------------------------------------
inline constexpr std::uint32_t kEntry = 0x0026AF50U;
inline constexpr std::uint32_t kEntryNext = kEntry + 4U;           // the original continues here after `addiu sp, sp, -16`
inline constexpr std::uint32_t kCodeBytes = 612U;                  // 153 words
inline constexpr std::uint32_t kGlobalPointer = 0x004E8670U;
inline constexpr std::uint32_t kWorldCtx = 0x01E32A10U;            // the world / prop state record (a0)
inline constexpr std::uint32_t kStagingPointerSlot = 0x004E2A10U;  // gp-23648: pointer to the staging record in the scratchpad
inline constexpr std::uint32_t kPlanes = 0x01E327D0U;              // 6 x 16 B per view, stride 96
inline constexpr std::uint32_t kPlaneStride = 96U;
inline constexpr std::uint32_t kViewStruct = 0x00666330U;          // + 560 * view; the 10 qw view block at +288
inline constexpr std::uint32_t kViewStride = 560U;
inline constexpr std::uint32_t kViewBlockOffset = 288U;
inline constexpr std::uint32_t kViews = 8U;      // view structs / plane sets 0..7 exist (the front-end model scenes draw into 0..5)
inline constexpr std::uint32_t kWorldRoot = 0x0064C994U;           // topology pointer T (the registry epoch)
inline constexpr std::uint32_t kWorldContext0 = 0x0064C9A8U;       // WCTX of player 0 (+460 per player)
inline constexpr std::uint32_t kScratchpadBase = 0x70000000U;
inline constexpr std::uint32_t kScratchpadEnd = 0x70004000U;

// ctx offsets
inline constexpr std::uint32_t kCtxHeader = 0U;       // [CNT 0 | FLUSHE | NOP]
inline constexpr std::uint32_t kCtxPairHead = 16U;    // [CNT 1 | STCYCL 4,4 | UNPACK V4-32 1 qw @0x3f5]
inline constexpr std::uint32_t kCtxTail = 32U;        // [CNT 0 | MSCNT]
inline constexpr std::uint32_t kCtxSpecDirection = 48U;
inline constexpr std::uint32_t kCtxSpecCamera = 64U;
inline constexpr std::uint32_t kCtxMaterial = 148U;
inline constexpr std::uint32_t kCtxClip = 152U;
inline constexpr std::uint32_t kCtxView = 153U;
inline constexpr std::uint32_t kCtxPending = 155U;
inline constexpr std::uint32_t kCtxMirror = 204U;     // float: half the mirror plane height of a kind 7 material
inline constexpr std::uint32_t kCtxBytes = 208U;

// staging record: +0 queued quadwords, +4 write cursor, +8 base, queue from +16, output from +336
inline constexpr std::uint32_t kStagingQueue = 16U;
inline constexpr std::uint32_t kStagingOutput = 336U;
inline constexpr std::uint32_t kMaxSlots = 4U;              // WorldSubmits of one call: an object with more than 16 groups is split (64 groups at most)
inline constexpr std::uint32_t kMaxQueued = (kStagingOutput - kStagingQueue) / 16U;   // 20 quadwords

// record (64 B): edges +0/+16/+32, origin +48, mesh packet +12, continuation +28
inline constexpr std::uint32_t kRecordMesh = 12U;
inline constexpr std::uint32_t kRecordCont = 28U;

// material (40 B)
inline constexpr std::uint32_t kMatKind = 0U;
inline constexpr std::uint32_t kMatParamA = 4U;       // f32 (kind 1: gain)
inline constexpr std::uint32_t kMatParamB = 8U;       // f32 (kind 1: specular power)
inline constexpr std::uint32_t kMatTextures = 12U;    // pointer to the texture descriptor table
inline constexpr std::uint32_t kMatSlot = 16U;        // s8 slot
inline constexpr std::uint32_t kMatFlags = 36U;       // u16
inline constexpr std::uint32_t kMatBytes = 40U;
inline constexpr std::uint32_t kFlagAlternate = 0x20U;
inline constexpr std::uint32_t kFlagScroll = 0x200U;

// ---- guest memory as the core sees it ---------------------------------------------------------------------------------------------------
class ReadView : public world_mesh::QwReader {
  public:
    // The whole range [address, address + bytes) is readable (and, for GuestMemory, writable) main RAM or scratchpad.
    // Reads of unmapped addresses return 0 (the core checks `mapped` for everything it depends on).
    [[nodiscard]] virtual bool mapped(std::uint32_t address, std::uint32_t bytes) const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t read_u8(std::uint32_t address) const = 0;
    [[nodiscard]] virtual std::uint16_t read_u16(std::uint32_t address) const = 0;
    [[nodiscard]] virtual std::uint32_t read_u32(std::uint32_t address) const = 0;
    using world_mesh::QwReader::read_u32; // the importer's checked overload
    // Any quadword main RAM or scratchpad: four read_u32.
    void read_quad(std::uint32_t address, std::uint32_t (&out)[4]) const {
        for (std::uint32_t i = 0; i != 4U; ++i) {
            out[i] = read_u32(address + 4U * i);
        }
    }
};
class GuestMemory : public ReadView {
  public:
    virtual void write_u8(std::uint32_t address, std::uint8_t value) = 0;
    virtual void write_u32(std::uint32_t address, std::uint32_t value) = 0;
    virtual void write_quad(std::uint32_t address, const std::uint32_t (&words)[4]) = 0;
};

// ---- the call and the plan --------------------------------------------------------------------------------------------------------------
struct Call {
    std::uint32_t ctx{};      // a0
    std::uint32_t record{};   // a1
    std::uint64_t clip{};     // a2 (64-bit register)
    std::uint64_t pass{};     // a3
    std::uint64_t gp{};
};

enum class Outcome : std::uint8_t {
    kDecline,  // the guest runs the original code
    kOutside,  // culled: v0 = 0, nothing else changes (replace mode only; shadow mode declines)
    kAccept,   // visible and supported: write the stub and push the submission
};
enum class Reason : std::uint8_t {
    kNone, kState, kStaging, kMaterial, kMesh, kView, kRing, kSpace, kEnv, kGroups,
    kCount
};
[[nodiscard]] const char* reason_name(Reason r) noexcept;

struct Plan {
    Outcome outcome{Outcome::kDecline};
    Reason reason{Reason::kNone};
    // guest state decisions
    std::uint32_t ctx{}, record{}, staging{}, cursor{}, queued{};
    std::uint64_t clip_after{};   // a2 after the cull (1 = inside), stored to ctx.s8[152]
    std::uint64_t pass{};         // a3
    bool pair{};                  // the 1013 pair is written
    std::uint32_t pair_cmd{};     // 0 / 2 / 10
    std::uint32_t pair_flags{};   // low word of the flags word (qw 1013.w)
    std::uint32_t slots{};        // WorldSubmits (ring slots) the call needs: ceil(groups / ws::kMaxGroups)
    std::uint32_t out_qw{};       // quadwords written at the cursor
    std::uint32_t retired{};      // nominal cost
    // the submission
    std::uint32_t material{}, mat_kind{}, mat_flags{};
    std::uint16_t program{};
    std::uint32_t prim{};
    float view[ws::kViewFloats]{};
    nc::SpecularLight spec{};
    float mirror_y0{};
    std::shared_ptr<const world_mesh::ImportedMesh> mesh;
};

struct Stats {
    std::uint64_t calls{}, outside{}, accepted{}, declined{};
    std::uint64_t reasons[static_cast<std::size_t>(Reason::kCount)]{};
    std::uint64_t kinds[8]{};         // accepted by material kind
    std::uint64_t groups{}, owner_refreshes{};
};

// ---- the producer -------------------------------------------------------------------------------------------------------------------
class Producer {
  public:
    Producer();

    // Decides one call from guest memory (no guest state is changed; the mesh registry and the owner table are host caches).
    // `shadow`: the guest keeps its own packets, so a culled object is a decline (nothing to compare).
    Outcome plan(const ReadView& mem, const Call& call, bool shadow, Plan& out);

    // Submission `index` (0 .. plan.slots - 1) of an accepted plan, into a slot opened with Ring::begin_push (kind, seq, generation are
    // set). Slot k carries groups [16k, 16k + 16) of the object, all other fields are the same.
    static void fill(const Plan& plan, std::uint32_t index, ws::Submit& slot);

    // Replace mode: the staging output, ctx.s8[152], ctx.u8[155], staging +0 / +4. `markers` are the plan.slots marker quadwords
    // (Ring::marker_of of the pushed slots, in order); each gets a stub.
    static void apply(GuestMemory& mem, const Plan& plan, const nc::Quad* markers);

    [[nodiscard]] Stats& stats() noexcept { return stats_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] world_mesh::WorldMeshRegistry& registry() noexcept { return registry_; }
    void clear();

  private:
    Outcome decline(Plan& out, Reason r) noexcept;
    bool refresh_owners(const ReadView& mem);

    // The view block the VU1 holds (qw 1014..1023): the last view-select packet (UNPACK V4-32 10 qw @0x3f6, queued by 0x26baa0) the guest
    // consumed. The view struct changes during a pass (the camera is updated after the list is built), so an object's view is the block
    // that was copied at the switch, not the struct's current content.
    float tracked_view_[ws::kViewFloats]{};
    bool tracked_valid_{false};

    world_mesh::WorldMeshRegistry registry_;
    std::unordered_set<std::uint64_t> known_;   // meshes acquired since the last owner refresh / epoch change
    std::uint64_t owner_signature_{0};
    std::uint64_t epoch_{0};
    std::uint32_t since_refresh_{0};
    Stats stats_;
};

} // namespace takedown::runtime::slus_21050::world_submit_core
