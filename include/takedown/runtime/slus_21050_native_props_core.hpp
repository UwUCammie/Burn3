#pragma once

// P2 WP5: the arithmetic of the prop instance cull + prepare, in plain C++ (no guest machine, no microprogram).
//
// What it replaces. The guest feeds every prop instance of a section through VU0 microprogram 0x27 (distance test, the eight corners of
// the section's box through the instance transform, a classification against the planes of each registered view, one prepared record
// of five quadwords appended to that view's buffer) and polls each view's buffer length with microprogram 0x124 after the instance;
// a view whose buffer holds 70 quadwords (14 records) is flushed at once (0x26a240), and the sections's last records go out when
// the section is drained (0x26a1c0). The microprograms keep all their state in the 4 KiB VU0 data memory:
//
//     qw   0 + 71 * slot            buffer of a registered view: word 0 = quadwords of records held (5 per instance), the records
//                                   start at qw 1 (at most 14 are ever held: the 14th triggers the flush)
//     qw 213 .. 220                 the eight corners of the section's box, in the order the setup (microprogram 0) makes them:
//                                   origin, +x, +z, +x+z, +y, +x+y, +y+z, +x+y+z (xyz; w unused)
//     qw 229 + 6 * slot             the six planes of the view in slot `slot`: four planes transposed (qw +0 = their X, +1 = Y,
//                                   +2 = Z, +3 = W) and two as plain (X, Y, Z, W) quadwords (+4 is the far plane with the section's
//                                   far term applied, +5); a point is inside a plane when W - X x - Y y - Z z >= 0
//     qw 247                        tint vector: xyz = light direction (unused for kind 4), w = 255.0 / 128.0 / 4.0 by material flags
//     qw 248                        distance reference: xyz = scene point (only y and z are used), w = threshold (squared distance)
//   plus the VU0 integer registers vi1 (registered views), vi15 (distance mode: 0 none, 1 keep near, else keep far), vi8 (1 = lit
//   props, kind 5) and vi4 (1 = compose with the retained transform).
//
// This code reads and writes exactly that image, so a native call leaves the VU0 state a guest call would have left and any later call
// of the section may still run the original code (see slus_21050_native_props.hpp). The arithmetic is the VU's (clamp-mode-3 operands,
// products truncated, sums chopped toward zero, every step through the helpers of vu0_simd_arithmetic.hpp): the prepared records are bit
// for bit those of the microprogram (tests/test_native_props.cpp checks that over recorded sections).
//
// Supported: kind 4 (packed-scale output), no retained transform. Everything else is `Unsupported` and must run the original.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050::native_props {

using Quad = std::array<std::uint32_t, 4>;
using InstanceWords = std::array<std::uint32_t, 16>; // 64 bytes of an instance: basis columns x y z (xyz + w), translation (xyz + w)

inline constexpr std::uint32_t kImageWords = 1024U;          // 4 KiB of VU0 data memory
inline constexpr std::uint32_t kBufferStrideQw = 71U;
inline constexpr std::uint32_t kCornersQw = 213U;
inline constexpr std::uint32_t kPlanesQw = 229U;
inline constexpr std::uint32_t kPlaneStrideQw = 6U;
inline constexpr std::uint32_t kTintQw = 247U;
inline constexpr std::uint32_t kVectorQw = 248U;
inline constexpr std::uint32_t kMaxSlots = 3U;
inline constexpr std::uint32_t kRecordQw = 5U;
inline constexpr std::uint32_t kFlushQw = 70U;                // a view buffer holding this many quadwords is flushed
inline constexpr std::uint32_t kFlushRecords = kFlushQw / kRecordQw;

// One prepared instance as the microprogram leaves it in a view buffer (and as the flush sends it to VU1 qw 934 + 5k):
//   qw 0..2  basis columns: x y z unchanged, w = z
//   qw 3     translation: x y unchanged, z = visibility (0 inside every plane, 1 crosses one), w = z (the depth)
//   qw 4     parameters: ftoi(128.5 * column.w) x 3, ftoi(tint.w)
struct Record {
    std::array<Quad, 5> qw{};
    bool operator==(const Record&) const = default;
};

// A copy of the VU0 data memory.
struct Vu0Image {
    std::array<std::uint32_t, kImageWords> words{};
    [[nodiscard]] Quad quad(const std::uint32_t index) const noexcept {
        const std::size_t at = 4U * static_cast<std::size_t>(index);
        return Quad{words[at], words[at + 1U], words[at + 2U], words[at + 3U]};
    }
    void set_quad(const std::uint32_t index, const Quad& q) noexcept {
        const std::size_t at = 4U * static_cast<std::size_t>(index);
        words[at] = q[0];
        words[at + 1U] = q[1];
        words[at + 2U] = q[2];
        words[at + 3U] = q[3];
    }
};

// The VU0 / EE state besides the data memory that the call depends on.
struct Regs {
    std::uint32_t slots{};    // vi1: registered views (1..3)
    std::uint32_t mode{};     // vi15
    std::uint32_t lighting{}; // vi8
    std::uint32_t retained{}; // vi4 (ctx byte 138)
};

// A batch the guest would have flushed: the records of one view buffer, in buffer order.
struct Flush {
    std::uint32_t slot{};
    std::vector<Record> records;
};

struct CellStats {
    std::uint32_t instances{};
    std::uint32_t distance_rejected{};
    std::uint32_t outside_all{};  // inside the distance limit but outside every registered view
    std::uint32_t records{};      // records appended (sum over views)
};

enum class Status : std::uint8_t {
    kOk,
    kUnsupported, // retained transform, lit props, no or too many views, an unknown mode: run the original
    kBadBuffer,   // a view buffer that is not what the guest leaves (count not a multiple of 5 or at the flush size)
};

// microprogram 0x27 + the polling and flushing of 0x26a610 for `instances` instances: appends the prepared records to the view buffers
// of `image` and moves every buffer that reaches 14 records to `flushes` (in the order the guest flushes them: after each instance,
// view by view). The buffers in `image` are updated (count word, remaining records; a flushed buffer's count word is zeroed).
[[nodiscard]] Status process_cell(Vu0Image& image, const Regs& regs, std::span<const InstanceWords> instances, std::vector<Flush>& flushes,
                                  CellStats* stats = nullptr);

// 0x26a1c0: every non-empty buffer goes to `flushes` in slot order and its count word is zeroed.
[[nodiscard]] Status drain(Vu0Image& image, const Regs& regs, std::vector<Flush>& flushes);

// What the emitter's read_prop_instances makes of a record (native_cmd_decode.hpp): the 3x4 row-major transform of (x y w) columns
// and the translation, and the tint words / 255.
struct InstanceView {
    float xform[12]{};
    float tint[4]{};
};
[[nodiscard]] InstanceView instance_view(const Record& record) noexcept;
// A tint outside 0..255 (lit props; the emitter's lit_instance()).
[[nodiscard]] bool is_lit(const InstanceView& view) noexcept;


// ---- one native call: what it reads from the guest, what it decides ----------------------------------------------------------------
// The glue (slus_21050_native_props.cpp) gathers CallInputs from guest memory, calls plan_call() and carries the plan out; everything
// that decides (eligibility, batches, view and primitive selection, space in the staging buffer) is here and tested offline.
inline constexpr std::uint32_t kViewBlockWords = 40U;     // VU1 qw 1014..1023, copied by 0x26baa0 from view struct + 288
inline constexpr std::uint32_t kViewFarWord = 4U * (1020U - 1014U) + 3U;
inline constexpr std::uint32_t kViewNearWord = 4U * (1021U - 1014U) + 3U;
inline constexpr std::uint32_t kMaxViews = 8U;
inline constexpr std::uint32_t kStageLimitQw = 160U;      // staging output (scratchpad 0x70002d50..) a native call may fill
inline constexpr std::uint32_t kViewSwitchQw = 17U;       // what 0x26baa0 queues
inline constexpr std::uint32_t kStubQw = 3U;              // DMA CNT, VIF DIRECT 1, marker
inline constexpr std::uint32_t kPrimBase = 0x4CU;         // triangle strip, shaded, fog (what the prologue puts in the GIF tag template)
inline constexpr std::uint32_t kPrimTexture = 0x10U;

struct ViewBlock {
    bool valid{};
    std::array<std::uint32_t, kViewBlockWords> words{};
};

struct CallInputs {
    bool drain{};
    bool check_stage{true};                           // false in shadow mode: nothing is written to the staging buffer
    // ctx (0x01e32a10)
    std::int32_t slots_byte{};                        // byte 132: registered views
    std::array<std::int32_t, kMaxSlots> view_of_slot{}; // bytes 135..137
    std::int32_t current_view{};                      // byte 153
    bool masks_unchanged{};                           // dwords 160 == 168 and 176 == 184
    // staging buffer
    std::uint32_t queue_qw{};                         // quadwords queued by 0x26baa0 and not yet consumed
    std::uint32_t stage_used_qw{};                    // output quadwords already in the buffer
    // material (ctx word 148)
    std::uint32_t material_kind{}, material_flags{};
    bool texture{};                                   // the group prologue binds a texture (descriptor word non-zero)
    // VU0
    Regs regs;
    Vu0Image image;
    std::array<ViewBlock, kMaxViews> blocks{};        // by view index; the views of the registered slots
    std::vector<InstanceWords> instances;
};

enum class Decline : std::uint8_t {
    kNone,
    kRegs,        // vi1 / ctx byte 132 disagree, no or too many views, retained transform, lit program (vi8)
    kMaterial,    // not kind 4, or scrolling (flags & 0x200)
    kBuffer,      // a view buffer the guest cannot have left
    kView,        // a registered view that is not a view struct, or one the emitter would refuse (near / far)
    kLit,         // an instance whose tint is outside 0..255 (the emitter drops the whole batch; the original keeps it as a legacy strip)
    kStage,       // the staging buffer cannot take the output of the call
};

struct PlannedFlush {
    std::uint32_t slot{};
    std::uint32_t view{};         // view struct index
    bool select_target{};         // 0x26baa0 runs first (the view or the FRAME / ZBUF masks changed)
    std::uint32_t queue_qw{};     // quadwords queued when the stub is written (0 = no FLUSHE + queue copy)
};

struct CallPlan {
    Decline decline{Decline::kNone};
    std::vector<Flush> flushes;       // same order as `planned`
    std::vector<PlannedFlush> planned;
    Vu0Image image;                   // the VU0 data memory after the call
    std::uint32_t prim{};             // token PRIM (11 bits) | PRE << 11, as the emitter's trusted_prim reads qw 1006
    std::uint32_t stage_qw{};         // staging output the call adds (stubs, queues, FLUSHE)
    CellStats stats;
};

// The decision. `kNone` when the native path may carry the call out; otherwise nothing has been touched and the original runs.
[[nodiscard]] CallPlan plan_call(const CallInputs& in);

// True when the emitter would refuse the view block (a trusted launch declines such a view and leaves the strip to the GS).
[[nodiscard]] bool view_block_usable(const ViewBlock& block) noexcept;

} // namespace takedown::runtime::slus_21050::native_props
