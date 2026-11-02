// 0x0013f550 ProjectRouteIntoProfile (a0 = vehicle A, a1 = profile id, a2 = world point Pt, a3 = Q3, t0 = Q4,
// t1 = Q5 (pointers to quads), t2 = box (two quads), f12 = dist, f13 = second level, f14 = first level).
//
// A's physics record P (u32 at A+1108) holds the vehicle's own road strip object (u32 at P+6324) and quad index
// (u16 at P+6328). A strip object U (0x00522660 + 44 + 8*id) points at its road R (u32 at U+4): pair table (+0),
// arc-length table (+4, a float per 8 bytes: the cumulative length at quad k), 10-byte quad records (+8), quad
// count (u16 +12) and the closed flag (+14). A quad record has its kind in +3 (5 = blocked), the forward road id and
// quad in +4/+6 and the backward ones in +5/+8.
//
//   1. The quad index advances by dist/6 (when dist > 0) and is clamped into R.
//   2. A walk of up to 128 steps moves across quads and linked strips until ClassifyPointInStripQuad says that the
//      quad contains Pt; the strip is then related to the vehicle's own strip (same strip, or found by following
//      the forward and then the backward links).
//   3. Not related: up to 16 quads consume dist along the arc lengths (path X, geometry of the quad's step vectors
//      and RoadLinkEntryDistance, a near and a far variant); related: path Y (geometry of the edge lengths of the
//      vehicle's strip and a chain sum). Both feed the same VU0 core: the step and its perpendicular become unit
//      vectors, their dots with the axes Q4 and Q5 and the box give the extents f22 and f6, and the centres
//      f23 and f1.
//   4. [lo, hi] is rasterised into three 256-column arrays of A: u8 A[64+i] (the profile id), u8 A[320+i] and
//      s16 A[576+2i], each only where the new value is smaller than the stored one scaled by two globals.
//
// Nothing is returned. Memory written: the point copy at frame+2288 (which ClassifyPointInStripQuad reads) and the
// three arrays; the VU0 registers, the COP1 accumulator and the condition flag end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <type_traits>

N3_CALLEE(00148440)
N3_CALLEE(00148ca0)
N3_CALLEE(00148c20)
N3_CALLEE(00146880)
N3_CALLEE(00147e90)
N3_CALLEE(00147d00)
N3_CALLEE(00147cb0)
N3_CALLEE(00148140)
N3_CALLEE(00147c10)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 2304U;

// Vehicle A, its physics record P, strip objects, roads and quad records.
constexpr GuestAddress kProfileIds = 64U;       // A: u8[256]
constexpr GuestAddress kProfileFirst = 320U;    // A: u8[256]
constexpr GuestAddress kProfileSecond = 576U;   // A: s16[256]
constexpr GuestAddress kWidth = 1092U;          // A: f32
constexpr GuestAddress kFarBias = 1096U;        // A: f32
constexpr GuestAddress kNearBias = 1100U;       // A: f32
constexpr GuestAddress kBlend = 1104U;          // A: f32
constexpr GuestAddress kPhysics = 1108U;        // A: P
constexpr GuestAddress kOwnStrip = 6324U;       // P: strip object
constexpr GuestAddress kOwnQuad = 6328U;        // P: u16 quad index
constexpr GuestAddress kStripObjects = 0x00522660U + 44U;   // object(id) = base + 8*id
constexpr GuestAddress kVertexTable = 0x0064d354U;          // u32 -> 16-byte vertices
constexpr GuestAddress kRoadPairs = 0U;
constexpr GuestAddress kRoadArc = 4U;
constexpr GuestAddress kRoadRecords = 8U;
constexpr GuestAddress kRoadCount = 12U;
constexpr GuestAddress kRoadClosed = 14U;
constexpr GuestAddress kRecordBytes = 10U;
constexpr GuestAddress kGpLevelScale = 0xffff843cU;      // $gp - 31684: g1
constexpr GuestAddress kGpFirstScale = 0xffff8828U;      // $gp - 30680: g2
constexpr GuestAddress kGpSecondScale = 0xffff882cU;     // $gp - 30676: g3

// Offsets in the original's frame (the callees' out buffers and the point they read).
constexpr GuestAddress kStartQuad = 2160U;
constexpr GuestAddress kNormalQuad = 2224U;
constexpr GuestAddress kFarStartQuad = 2128U;
constexpr GuestAddress kFarStepQuad = 2208U;
constexpr GuestAddress kStepQuad = 2192U;
constexpr GuestAddress kUpQuad = 2176U;
constexpr GuestAddress kPointQuad = 2288U;

// Walk move codes (the last move made, so that it is not undone at once).
constexpr std::uint32_t kMoveStart = 16U;
constexpr std::uint32_t kMoveForward = 4U;
constexpr std::uint32_t kMoveBackward = 8U;
constexpr std::uint32_t kMoveSideForward = 1U;
constexpr std::uint32_t kMoveSideBackward = 2U;
constexpr std::uint32_t kMaskBeyondEnd = 4U;
constexpr std::uint32_t kMaskBeforeStart = 8U;
constexpr std::uint32_t kMaskOutsideFirst = 1U;
constexpr std::uint32_t kMaskOutsideSecond = 2U;
constexpr std::uint32_t kMaxWalkSteps = 128U;
constexpr std::uint32_t kMaxScanQuads = 16U;
constexpr std::uint32_t kNoRoad = 255U;

// Float constants.
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kFive = 0x40a00000U;
constexpr std::uint32_t kSix = 0x40c00000U;
constexpr std::uint32_t kEight = 0x41000000U;
constexpr std::uint32_t kOneAndAHalf = 0x3fc00000U;
constexpr std::uint32_t k255 = 0x437f0000U;
constexpr std::uint32_t kFiveHundred = 0x43fa0000U;
constexpr std::uint32_t kMinusFiveHundred = 0xc3fa0000U;
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kTinyBelow = 0x37800001U;   // exponent field 111 or less
constexpr std::int32_t kInt16Max = 32767;

// Retired counts of the listing's blocks, delay slots included (callees excluded: they add their own).
constexpr std::uint32_t kEntryCost = 43U;          // 0x13f550..0x13f5f8
constexpr std::uint32_t kAdvanceCost = 9U;         // 0x13f5fc..0x13f61c: dist > 0
constexpr std::uint32_t kClampCost = 6U;           // 0x13f620..0x13f634
constexpr std::uint32_t kClampOpenCost = 5U;       // 0x13f638 + 0x13f650
constexpr std::uint32_t kClampClosedCost = 6U;     // 0x13f638 + 0x13f644
constexpr std::uint32_t kFirstClassifyCost = 5U;   // 0x13f658
constexpr std::uint32_t kWalkTestCost = 2U;        // 0x13f720
constexpr std::uint32_t kWalkStepCost = 4U;        // 0x13f66c
constexpr std::uint32_t kMaskTestCost = 3U;        // each of 0x13f67c, 0x13f688, 0x13f694, 0x13f6a0
constexpr std::uint32_t kReclassifyCost = 4U;      // 0x13f710
constexpr std::uint32_t kForwardGuardCost = 3U;    // 0x13feb0
constexpr std::uint32_t kForwardStepCost = 7U;     // 0x13febc
constexpr std::uint32_t kForwardInCost = 2U;       // 0x13feec
constexpr std::uint32_t kForwardOpenEndCost = 6U;  // 0x13fed8 + 0x13fef4
constexpr std::uint32_t kForwardWrapCost = 7U;     // 0x13fed8 + 0x13fee4 + 0x13feec
constexpr std::uint32_t kBackwardGuardCost = 3U;   // 0x13ff00
constexpr std::uint32_t kBackwardTestCost = 3U;    // 0x13ff0c
constexpr std::uint32_t kBackwardStepCost = 4U;    // 0x13ff18 + 0x13ff20
constexpr std::uint32_t kBackwardOpenEndCost = 4U; // 0x13ff28 (taken)
constexpr std::uint32_t kBackwardWrapCost = 10U;   // 0x13ff28 + 0x13ff38 + 0x13ff20
constexpr std::uint32_t kSideGuardCost = 3U;       // 0x13ff48, 0x13f6ac
constexpr std::uint32_t kSideLinkCost = 15U;       // 0x13ff54
constexpr std::uint32_t kSideFirstDoneCost = 9U;   // 0x13ff90
constexpr std::uint32_t kSideSecondLinkCost = 14U; // 0x13f6b8
constexpr std::uint32_t kSideSecondDoneCost = 8U;  // 0x13f6f0
constexpr std::uint32_t kFinalClassifyCost = 8U;   // 0x13f728
constexpr std::uint32_t kRelateCost = 5U;          // 0x13f748
constexpr std::uint32_t kRelatedSameCost = 7U;     // 0x13ffb4 + 0x13f75c + 0x13f764
constexpr std::uint32_t kRelateOtherCost = 2U;     // 0x13f75c (taken)
constexpr std::uint32_t kForwardSearchCost = 10U;  // 0x13ffc0
constexpr std::uint32_t kSearchTestCost = 6U;      // 0x14002c, 0x13f7e0
constexpr std::uint32_t kSearchMissCost = 2U;      // 0x140044
constexpr std::uint32_t kSearchObjectCost = 8U;    // 0x13ffe8, 0x13f79c
constexpr std::uint32_t kSearchHitCost = 3U;       // 0x14004c, 0x140058
constexpr std::uint32_t kSearchNextCost = 9U;      // 0x140008, 0x13f7bc
constexpr std::uint32_t kBackwardSearchCost = 12U; // 0x13f76c
constexpr std::uint32_t kRelatedTestCost = 2U;     // 0x13f764 (not related) and 0x13f7f8
constexpr std::uint32_t kScanStartCost = 7U;       // 0x13f800
constexpr std::uint32_t kScanSetupCost = 2U;       // 0x13f81c
constexpr std::uint32_t kScanTestCost = 3U;        // 0x13f894
constexpr std::uint32_t kScanDistCost = 5U;        // 0x13f824
constexpr std::uint32_t kScanLengthCost = 3U;      // 0x13f838
constexpr std::uint32_t kScanFirstQuadCost = 7U;   // 0x1404fc
constexpr std::uint32_t kScanQuadCost = 8U;        // 0x13f844
constexpr std::uint32_t kScanCountCost = 6U;       // 0x13f864
constexpr std::uint32_t kScanEndTestCost = 3U;     // 0x13f87c
constexpr std::uint32_t kScanWrapCost = 2U;        // 0x13f888
constexpr std::uint32_t kScanLastCost = 16U;       // 0x140518
constexpr std::uint32_t kScanLastHitCost = 8U;     // 0x140558
constexpr std::uint32_t kScanMovedTestCost = 2U;   // 0x140578
constexpr std::uint32_t kScanBackCost = 15U;       // 0x140580
constexpr std::uint32_t kScanBackHitCost = 8U;     // 0x1405bc
constexpr std::uint32_t kScanStepDoneCost = 1U;    // 0x13f890
constexpr std::uint32_t kXFrontCost = 52U;         // 0x13f8a0
constexpr std::uint32_t kXNearCost = 13U;          // 0x13f970
constexpr std::uint32_t kXTinyCost = 1U;           // 0x13f9a4, 0x1406f4, 0x13fb74, 0x140444
constexpr std::uint32_t kXNearRatioCost = 12U;     // 0x140734, 0x140704
constexpr std::uint32_t kXNearBiasCost = 2U;       // 0x13f9a8
constexpr std::uint32_t kXFarCost = 70U;           // 0x1405dc
constexpr std::uint32_t kXFarBiasCost = 3U;        // 0x1406f8
constexpr std::uint32_t kXCoreCost = 113U;         // 0x13f9b0
constexpr std::uint32_t kXCoreRatioCost = 12U;     // 0x140764
constexpr std::uint32_t kXTailCost = 44U;          // 0x13fb78
constexpr std::uint32_t kYFrontCost = 65U;         // 0x140064
constexpr std::uint32_t kYEdgeTinyCost = 7U;       // 0x140168, 0x140184, 0x14040c, 0x140428
constexpr std::uint32_t kYMidCost = 11U;           // 0x1401a4
constexpr std::uint32_t kYChainTestCost = 6U;      // 0x14021c
constexpr std::uint32_t kYChainStepCost = 19U;     // 0x1401d0
constexpr std::uint32_t kYCoreCost = 118U;         // 0x140234
constexpr std::uint32_t kYTailCost = 45U;          // 0x140448
constexpr std::uint32_t kRasterCost = 15U;         // 0x13fc28
constexpr std::uint32_t kLowExitCost = 12U;        // 0x13fc64
constexpr std::uint32_t kHighExitCost = 8U;        // 0x13fc94
constexpr std::uint32_t kLowAltExitCost = 11U;     // 0x13fd8c
constexpr std::uint32_t kHighAltExitCost = 8U;     // 0x13fdb8
constexpr std::uint32_t kRangeCost = 3U;           // 0x13fcb4, 0x13fdd8
constexpr std::uint32_t kRangeLowCost = 2U;        // 0x140794, 0x1407c8
constexpr std::uint32_t kRangeHighTestCost = 3U;   // 0x13fcc0, 0x13fde4
constexpr std::uint32_t kRangeHighCost = 1U;       // 0x13fccc, 0x13fdf0
constexpr std::uint32_t kRasterSetupCost = 21U;    // 0x13fcd0, 0x13fdf4
constexpr std::uint32_t kColumnTestCost = 3U;      // 0x13fd78, 0x13fe9c
constexpr std::uint32_t kColumnFirstCost = 9U;     // 0x13fd24
constexpr std::uint32_t kColumnFirstStoreCost = 3U;   // 0x14079c
constexpr std::uint32_t kColumnSecondCost = 11U;   // 0x13fd48
constexpr std::uint32_t kColumnSecondStoreCost = 8U;  // 0x1407a8
constexpr std::uint32_t kColumnNextCost = 1U;      // 0x13fd74
constexpr std::uint32_t kRasterJumpCost = 2U;      // 0x13fd84 (the first form only)
constexpr std::uint32_t kLeaveCost = 2U;           // 0x13fea8
constexpr std::uint32_t kEarlyLeaveCost = 1U;      // 0x1407fc
constexpr std::uint32_t kEpilogueCost = 20U;       // 0x140800

constexpr std::uint32_t kCeiling = 20000U;

// --- scalar helpers -------------------------------------------------------------------------------------------

// The listing's tiny test: the exponent field alone, compared unsigned.
bool is_tiny(const std::uint32_t bits) { return (bits & kExponentMask) < kTinyBelow; }

// mfc1 sign-extends the word into the low doubleword of a GPR.
std::uint32_t sign_word(const std::uint32_t bits) { return static_cast<std::int32_t>(bits) < 0 ? 0xffffffffU : 0U; }

// A GPR written by mfc1 and then moved to a vector register (qmtc2): the float in the low doubleword, the upper
// doubleword whatever the register held (`upper`'s z and w).
Gpr128 float_register(const std::uint32_t bits, const Gpr128& upper) {
    return Gpr128{{bits, sign_word(bits), upper.words[2], upper.words[3]}};
}

GuestAddress strip_object(const std::uint32_t id) { return kStripObjects + ((id & 0xffU) << 3U); }

// --- VU0 macro instructions of the listing (vf0 is the constant (0, 0, 0, 1)) ----------------------------------

constexpr std::uint8_t kLaneX = 8U;
constexpr std::uint8_t kLaneY = 4U;
constexpr std::uint8_t kLaneZ = 2U;

void vaddw_x(R5900State& cpu, const unsigned fd) {          // vaddw.x fd,vf0,vf0w: fd.x = 1.0
    ee_cop2::vadd_broadcast(cpu, fd, 0U, 0U, kLaneX, 3U, false);
}
void vaddx_y(R5900State& cpu, const unsigned fd, const unsigned ft) {  // vaddx.y fd,vf0,ftx
    ee_cop2::vadd_broadcast(cpu, fd, 0U, ft, kLaneY, 0U, false);
}
void vmul_xyz(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {
    ee_cop2::vmul(cpu, fd, fs, ft, n3::kMaskXyz);
}
void vsub_xyz(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {
    ee_cop2::vadd_vector(cpu, fd, fs, ft, n3::kMaskXyz, true);
}
void vadday_x(R5900State& cpu, const unsigned fs) {         // vadday.x acc,fs,fsy
    ee_cop2::vadda_broadcast(cpu, fs, fs, kLaneX, 1U);
}
void vmaddz_x(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {  // fd.x = acc.x + fs.x*ft.z
    ee_cop2::vmadd_broadcast(cpu, fd, fs, ft, kLaneX, 2U, false, false, false);
}
void vsubz_z(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {   // fd.z = fs.z - ft.z
    ee_cop2::vadd_broadcast(cpu, fd, fs, ft, kLaneZ, 2U, true);
}
void vsubx_x(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {   // fd.x = fs.x - ft.x
    ee_cop2::vadd_broadcast(cpu, fd, fs, ft, kLaneX, 0U, true);
}
void vmulx_z(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {   // fd.z = fs.z * ft.x
    ee_cop2::vmul_broadcast(cpu, fd, fs, ft, kLaneZ, 0U);
}
void vmulx_x(R5900State& cpu, const unsigned fd, const unsigned fs, const unsigned ft) {   // fd.x = fs.x * ft.x
    ee_cop2::vmul_broadcast(cpu, fd, fs, ft, kLaneX, 0U);
}
void vrsqrt_q(R5900State& cpu) {                            // vrsqrt q,vf0w,vf1x
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
}
void vmulq_xyz(R5900State& cpu, const unsigned fd, const unsigned fs) {
    ee_cop2::vmul_control(cpu, fd, fs, n3::kMaskXyz, ee_cop2::kControlQ);
}
std::uint32_t lane_x(const R5900State& cpu, const unsigned vf) { return cpu.read_vu0_vector(vf).words[0]; }

// The tail every dot product of the listing shares: vf1.x = acc.x + 1.0 * z, where vf1 was saved (qmfc2) just
// after vaddw.x had put 1.0 in its x lane and is written back (qmtc2) before the multiply-add. `fs` is the product
// vector (vf2 or vf3).
void finish_dot(R5900State& cpu, const Gpr128& saved_one, const unsigned fs) {
    vadday_x(cpu, fs);
    cpu.write_vu0_vector(1U, saved_one);
    vmaddz_x(cpu, 1U, 1U, fs);
}

// vmul.xyz vf2,vf3,vf3 .. vmulq.xyz vf3,vf3,q: vf3 becomes unit length in x and z (its y lane was cleared by the
// caller). `saved_one` is vf1 as vaddw.x left it.
void normalise_vf3(R5900State& cpu, const Gpr128& saved_one) {
    vmul_xyz(cpu, 2U, 3U, 3U);
    finish_dot(cpu, saved_one, 2U);
    vrsqrt_q(cpu);
    vmulq_xyz(cpu, 3U, 3U);
}

// --- the projection -----------------------------------------------------------------------------------------------

struct Along {
    Gpr128 unit;           // the direction made unit length (y forced to zero)
    std::uint32_t along;   // dot(Pt - origin, unit)
};

template <class Memory>
class Projector {
public:
    explicit Projector(n3::Call<Memory>& call)
        : call_(call), cpu_(call.cpu()), m_(call.memory()), frame_(call.stack_pointer() - kFrameBytes),
          vehicle_(call.arg(0)), profile_id_(call.arg(1)), box_(call.arg(6)) {
        // The original loads the four quads first, then stores them in its frame.
        q5_ = n3::load_quad_any(m_, call.arg(5));
        q3_ = n3::load_quad_any(m_, call.arg(3));
        q4_ = n3::load_quad_any(m_, call.arg(4));
        point_ = n3::load_quad_any(m_, call.arg(2));
        n3::store_quad_any(m_, frame_ + kPointQuad, point_);
        dist_ = cpu_.read_fpr_bits(12U);
        second_level_ = cpu_.read_fpr_bits(13U);
        first_level_ = cpu_.read_fpr_bits(14U);
    }

    n3::Run run();

private:
    // --- callees (each is the original's jal with its arguments set as the listing sets them) ---

    template <class Fn>
    bool invoke(Fn&& callee) {
        const n3::Run run = call_.invoke(callee, kFrameBytes);
        if (run.declined) {
            return false;
        }
        retired_ += run.retired;
        return true;
    }
    // 0x00148440: the side of the quad on which Pt lies (0 = inside).
    bool classify(const GuestAddress strip, const std::uint32_t quad, std::uint32_t& mask) {
        call_.set_arg(0U, strip);
        call_.set_arg(1U, frame_ + kPointQuad);
        call_.set_arg(2U, quad);
        if (!invoke([](auto& machine) { return n3::fn_00148440(machine); })) {
            return false;
        }
        mask = call_.result();
        return true;
    }
    // 0x00148ca0: one quad from `quad` of road `id` is not blocked.
    bool route_open(const std::uint32_t id, const std::uint32_t quad, bool& open) {
        call_.set_arg(0U, id);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, 1U);
        if (!invoke([](auto& machine) { return n3::fn_00148ca0(machine); })) {
            return false;
        }
        open = call_.result() != 0U;
        return true;
    }
    // 0x00148c20: road R's entry at `quad` is not blocked.
    bool entry_open(const GuestAddress road, const std::uint32_t quad, bool& open) {
        call_.set_arg(0U, road);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, 1U);
        if (!invoke([](auto& machine) { return n3::fn_00148c20(machine); })) {
            return false;
        }
        open = call_.result() != 0U;
        return true;
    }
    // 0x00146880: the 16-byte vertex of the chain at `quad` (direction 2 or 1) written to `out`.
    bool link_vertex(const GuestAddress strip, const std::uint32_t quad, const std::uint32_t direction,
                     const GuestAddress out) {
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, direction);
        call_.set_arg(3U, frame_ + out);
        return invoke([](auto& machine) { return n3::fn_00146880(machine); });
    }
    bool step_cross(const GuestAddress strip, const std::uint32_t quad, const GuestAddress out) {  // 0x00147e90
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, frame_ + out);
        return invoke([](auto& machine) { return n3::fn_00147e90(machine); });
    }
    bool step_vector(const GuestAddress strip, const std::uint32_t quad, const GuestAddress out) {  // 0x00147d00
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, frame_ + out);
        return invoke([](auto& machine) { return n3::fn_00147d00(machine); });
    }
    bool step_up(const GuestAddress strip, const std::uint32_t quad, const GuestAddress out) {  // 0x00147cb0
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, frame_ + out);
        return invoke([](auto& machine) { return n3::fn_00147cb0(machine); });
    }
    // 0x00148140 with a2 = a1 (every call site passes the same quad twice).
    bool entry_distance(const GuestAddress strip, const std::uint32_t quad, std::uint32_t& bits) {
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        call_.set_arg(2U, quad);
        if (!invoke([](auto& machine) { return n3::fn_00148140(machine); })) {
            return false;
        }
        bits = call_.float_result();
        return true;
    }
    bool edge_length(const GuestAddress strip, const std::uint32_t quad, std::uint32_t& bits) {  // 0x00147c10
        call_.set_arg(0U, strip);
        call_.set_arg(1U, quad);
        if (!invoke([](auto& machine) { return n3::fn_00147c10(machine); })) {
            return false;
        }
        bits = call_.float_result();
        return true;
    }

    // --- road records ---

    [[nodiscard]] GuestAddress road_of(const GuestAddress strip) const { return m_.read_u32_le(strip + 4U); }
    [[nodiscard]] GuestAddress record_of(const GuestAddress strip, const std::uint32_t quad) const {
        return m_.read_u32_le(road_of(strip) + kRoadRecords) + (quad & 0xffffU) * kRecordBytes;
    }
    [[nodiscard]] std::uint32_t quad_count(const GuestAddress road) const { return m_.read_u16_le(road + kRoadCount); }
    [[nodiscard]] bool road_closed(const GuestAddress road) const { return m_.read_u8(road + kRoadClosed) != 0U; }

    // --- the stages (each returns false when a callee declined) ---

    bool locate_quad();
    bool relate_to_own_strip();
    bool scan_forward();
    Along along_unit(const Gpr128& direction, const Gpr128& origin, bool with_length);
    bool path_x();
    bool path_y();
    bool core(bool y_path);
    bool scaled_by_own_distance(std::uint32_t numerator, std::uint32_t denominator, std::uint32_t& scaled);
    void core_first_half(std::uint32_t& along);
    void core_second_half(std::uint32_t ratio);
    bool rasterise();

    n3::Call<Memory>& call_;
    R5900State& cpu_;
    Memory& m_;
    GuestAddress frame_;
    GuestAddress vehicle_;
    std::uint32_t profile_id_;
    GuestAddress box_;
    Gpr128 q3_{};
    Gpr128 q4_{};
    Gpr128 q5_{};
    Gpr128 point_{};
    std::uint32_t retired_ = 0U;

    // The original's float registers that live across stages.
    std::uint32_t dist_ = 0U;           // f20 at first
    std::uint32_t second_level_ = 0U;   // f26
    std::uint32_t first_level_ = 0U;    // f25
    std::uint32_t along_ = 0U;          // f21
    std::uint32_t centre_a_ = 0U;       // f23
    std::uint32_t extent_a_ = 0U;       // f22
    std::uint32_t centre_b_ = 0U;       // f1
    std::uint32_t extent_b_ = 0U;       // f6

    // The position.
    GuestAddress strip_ = 0U;           // s1
    std::uint32_t quad_ = 0U;           // s0 / s4
    std::uint32_t own_quad_ = 0U;       // s5
    bool found_ = false;               // the walk ended inside a quad
    bool related_ = false;
    Gpr128 start_{};                    // the road point the point is measured from (2160)
    Gpr128 normal_{};                   // its unit direction (2224)
    Gpr128 step_unit_{};                // 2192 after the core's first half
    Gpr128 up_unit_{};                  // 2176
    std::uint32_t level_scale_ = 0U;    // f20 in the core: g1
    std::uint32_t chain_sum_ = 0U;      // f24 on path Y
};

// Stage 1 and 2: the quad index (advanced by dist/6 and clamped into R) and the walk across neighbouring quads and
// linked strips until the quad that contains Pt (0x13f550..0x13f728).
template <class Memory>
bool Projector<Memory>::locate_quad() {
    const GuestAddress physics = m_.read_u32_le(vehicle_ + kPhysics);
    strip_ = m_.read_u32_le(physics + kOwnStrip);
    quad_ = m_.read_u16_le(physics + kOwnQuad);
    retired_ += kEntryCost;
    if (!fpu::le(dist_, 0U)) {
        quad_ = (quad_ + static_cast<std::uint32_t>(fpu::to_int(fpu::div(dist_, kSix)))) & 0xffffU;
        retired_ += kAdvanceCost;
    }
    {
        const GuestAddress road = road_of(strip_);
        const std::uint32_t count = quad_count(road);
        retired_ += kClampCost;
        if (static_cast<std::int32_t>(quad_) >= static_cast<std::int32_t>(count)) {
            if (!road_closed(road)) {
                quad_ = (count - 1U) & 0xffffU;
                retired_ += kClampOpenCost;
            } else {
                quad_ = (quad_ - count) & 0xffffU;
                retired_ += kClampClosedCost;
            }
        }
    }

    std::uint32_t mask = 0U;
    if (!classify(strip_, quad_, mask)) {
        return false;
    }
    retired_ += kFirstClassifyCost;
    std::uint32_t move = kMoveStart;
    std::uint32_t steps = 0U;
    for (;;) {
        retired_ += kWalkTestCost;
        if (mask == 0U) {
            break;
        }
        if (retired_ > kCeiling) {
            return false;
        }
        retired_ += kWalkStepCost;
        if (++steps == kMaxWalkSteps) {
            break;
        }
        retired_ += kMaskTestCost;
        if ((mask & kMaskBeyondEnd) != 0U) {
            // Past the end edge: the next quad (wrapping on a closed road, stopping at the end of an open one).
            retired_ += kForwardGuardCost;
            if (move == kMoveBackward) {
                break;
            }
            const GuestAddress road = road_of(strip_);
            quad_ = (quad_ + 1U) & 0xffffU;
            const std::uint32_t count = quad_count(road);
            retired_ += kForwardStepCost;
            if (static_cast<std::int32_t>(quad_) < static_cast<std::int32_t>(count)) {
                retired_ += kForwardInCost;
            } else if (!road_closed(road)) {
                retired_ += kForwardOpenEndCost;
                quad_ = (count - 1U) & 0xffffU;
                break;
            } else {
                retired_ += kForwardWrapCost;
                quad_ = (quad_ - count) & 0xffffU;
            }
            move = kMoveForward;
        } else {
            retired_ += kMaskTestCost;
            if ((mask & kMaskBeforeStart) != 0U) {
                // Before the start edge: the previous quad.
                retired_ += kBackwardGuardCost;
                if (move == kMoveForward) {
                    break;
                }
                retired_ += kBackwardTestCost;
                if (quad_ != 0U) {
                    quad_ = (quad_ - 1U) & 0xffffU;
                    retired_ += kBackwardStepCost;
                } else {
                    const GuestAddress road = road_of(strip_);
                    if (!road_closed(road)) {
                        retired_ += kBackwardOpenEndCost;
                        break;
                    }
                    quad_ = (quad_count(road) - 1U) & 0xffffU;
                    retired_ += kBackwardWrapCost;
                }
                move = kMoveBackward;
            } else {
                retired_ += kMaskTestCost;
                if ((mask & kMaskOutsideFirst) != 0U) {
                    // Outside the first side: the forward link of the quad record.
                    retired_ += kSideGuardCost;
                    if (move == kMoveSideBackward) {
                        break;
                    }
                    retired_ += kSideLinkCost;
                    const GuestAddress record = record_of(strip_, quad_);
                    const std::uint32_t id = m_.read_u8(record + 4U);
                    const std::uint32_t next = m_.read_u16_le(record + 6U);
                    bool open = false;
                    if (!route_open(id, next, open)) {
                        return false;
                    }
                    if (!open) {
                        break;
                    }
                    retired_ += kSideFirstDoneCost;
                    quad_ = next;
                    strip_ = strip_object(id);
                    move = kMoveSideForward;
                } else {
                    retired_ += kMaskTestCost;
                    if ((mask & kMaskOutsideSecond) != 0U) {
                        // Outside the second side: the backward link.
                        retired_ += kSideGuardCost;
                        if (move == kMoveSideForward) {
                            break;
                        }
                        retired_ += kSideSecondLinkCost;
                        const GuestAddress record = record_of(strip_, quad_);
                        const std::uint32_t id = m_.read_u8(record + 5U);
                        const std::uint32_t next = m_.read_u16_le(record + 8U);
                        bool open = false;
                        if (!route_open(id, next, open)) {
                            return false;
                        }
                        if (!open) {
                            break;
                        }
                        retired_ += kSideSecondDoneCost;
                        quad_ = next;
                        strip_ = strip_object(id);
                        move = kMoveSideBackward;
                    }
                }
            }
        }
        retired_ += kReclassifyCost;
        if (!classify(strip_, quad_, mask)) {
            return false;
        }
    }

    retired_ += kFinalClassifyCost;
    if (!classify(strip_, quad_, mask)) {
        return false;
    }
    found_ = mask == 0U;
    return true;
}

// Stage 3: the found strip against the vehicle's own: the same strip, or reached from the own quad by following
// the forward links and then the backward links (0x13f748..0x13f7f4). On success own_quad_ is the quad there.
template <class Memory>
bool Projector<Memory>::relate_to_own_strip() {
    related_ = false;
    retired_ += kRelateCost;
    const GuestAddress physics = m_.read_u32_le(vehicle_ + kPhysics);
    const GuestAddress own_strip = m_.read_u32_le(physics + kOwnStrip);
    const std::uint32_t own_quad = m_.read_u16_le(physics + kOwnQuad);
    if (own_strip == strip_) {
        related_ = true;
        own_quad_ = own_quad;
        retired_ += kRelatedSameCost;
        return true;
    }
    retired_ += kRelateOtherCost;

    // The two searches differ only in the link they follow (id at +4 and quad at +6, or +5 and +8).
    for (int backward = 0; backward < 2 && !related_; ++backward) {
        const GuestAddress id_offset = backward != 0 ? 5U : 4U;
        const GuestAddress quad_offset = backward != 0 ? 8U : 6U;
        retired_ += backward != 0 ? kBackwardSearchCost : kForwardSearchCost;
        GuestAddress record = record_of(own_strip, own_quad);
        std::uint32_t id = m_.read_u8(record + id_offset);
        std::uint32_t next = m_.read_u16_le(record + quad_offset);
        for (;;) {
            retired_ += kSearchTestCost;
            bool open = false;
            if (!route_open(id, next, open)) {
                return false;
            }
            if (!open) {
                retired_ += backward != 0 ? 0U : kSearchMissCost;
                break;
            }
            retired_ += kSearchObjectCost;
            const GuestAddress other = strip_object(id);
            if (other == strip_) {
                retired_ += kSearchHitCost;
                own_quad_ = next;
                related_ = true;
                break;
            }
            retired_ += kSearchNextCost;
            record = record_of(other, next);
            id = m_.read_u8(record + id_offset);
            next = m_.read_u16_le(record + quad_offset);
            if (retired_ > kCeiling) {
                return false;
            }
        }
        if (backward == 0) {
            retired_ += kRelatedTestCost;   // 0x13f764
        }
    }
    return true;
}

// Stage 4: the quad has no relation to the vehicle's strip. If its entry in R is not blocked it is used as it is;
// otherwise up to 16 quads consume dist along the arc lengths, moving on to the linked strip at the end of an open
// road (0x13f800..0x13f89c, 0x1404fc..0x1405d8).
template <class Memory>
bool Projector<Memory>::scan_forward() {
    retired_ += kScanStartCost;
    std::uint32_t quad = quad_ & 0xffffU;
    bool open = false;
    if (!entry_open(road_of(strip_), quad_, open)) {
        return false;
    }
    if (open) {
        quad_ = quad;
        return true;
    }
    retired_ += kScanSetupCost;
    std::uint32_t pass = 0U;
    for (;;) {
        retired_ += kScanTestCost;
        if (pass >= kMaxScanQuads) {
            break;
        }
        if (retired_ > kCeiling) {
            return false;
        }
        retired_ += kScanDistCost;
        if (fpu::lt(dist_, 0U)) {
            break;
        }
        retired_ += kScanLengthCost;
        const GuestAddress road = road_of(strip_);
        const GuestAddress arc = m_.read_u32_le(road + kRoadArc);
        if (quad == 0U) {
            retired_ += kScanFirstQuadCost;
            dist_ = fpu::sub(dist_, m_.read_u32_le(arc));
        } else {
            retired_ += kScanQuadCost;
            const GuestAddress entry = arc + (quad << 3U);
            dist_ = fpu::sub(dist_, fpu::sub(m_.read_u32_le(entry), m_.read_u32_le(entry - 8U)));
        }
        retired_ += kScanCountCost;
        const std::uint32_t count = quad_count(road);
        quad = (quad + 1U) & 0xffffU;
        if (static_cast<std::int32_t>(quad) >= static_cast<std::int32_t>(count)) {
            retired_ += kScanEndTestCost;
            if (road_closed(road)) {
                retired_ += kScanWrapCost;
                quad = (quad - count) & 0xffffU;
            } else {
                // The open end: the last quad's forward link, else the backward link of that quad.
                retired_ += kScanLastCost;
                quad = (count - 1U) & 0xffffU;
                GuestAddress record = m_.read_u32_le(road + kRoadRecords) + quad * kRecordBytes;
                const std::uint32_t id = m_.read_u8(record + 4U);
                const std::uint32_t next = m_.read_u16_le(record + 6U);
                if (!route_open(id, next, open)) {
                    return false;
                }
                if (open) {
                    retired_ += kScanLastHitCost;
                    quad = next;
                    strip_ = strip_object(id);
                }
                retired_ += kScanMovedTestCost;
                if (!open) {
                    retired_ += kScanBackCost;
                    record = record_of(strip_, quad);
                    const std::uint32_t back_id = m_.read_u8(record + 5U);
                    const std::uint32_t back_next = m_.read_u16_le(record + 8U);
                    if (!route_open(back_id, back_next, open)) {
                        return false;
                    }
                    if (open) {
                        retired_ += kScanBackHitCost;
                        quad = back_next;
                        strip_ = strip_object(back_id);
                    }
                }
            }
        }
        retired_ += kScanStepDoneCost;
        ++pass;
    }
    quad_ = quad;
    return true;
}

// The point's position along a direction: the direction is made unit length in x and z (its y lane is forced to
// zero by vaddx.y against a zero lane), and dot(Pt - origin, unit) comes back as the x lane. `with_length` adds the
// FAR variant's extra square root of |unit|^2 (0x140680..0x1406bc), which only leaves its mark in Q, the flags and
// vf1.
template <class Memory>
Along Projector<Memory>::along_unit(const Gpr128& direction, const Gpr128& origin, const bool with_length) {
    cpu_.write_vu0_vector(3U, direction);                 // lqc2 vf3
    cpu_.write_vu0_vector(2U, Gpr128{});                  // qmtc2 of the cleared v0: x = 0 (z, w are rewritten below)
    vaddw_x(cpu_, 1U);
    vaddx_y(cpu_, 3U, 2U);
    normalise_vf3(cpu_, cpu_.read_vu0_vector(1U));
    Along result{cpu_.read_vu0_vector(3U), 0U};
    const Gpr128 dotted = cpu_.read_vu0_vector(1U);       // qmfc2 v0,vf1: its z and w stay in v0
    cpu_.write_vu0_vector(2U, point_);                    // lqc2 vf2,2288
    cpu_.write_vu0_vector(1U, origin);                    // lqc2 vf1,2160
    cpu_.write_vu0_vector(5U, Gpr128{{0U, 0U, dotted.words[2], dotted.words[3]}});   // mfc1 v0,f0 (f0 = 0); qmtc2
    vsub_xyz(cpu_, 1U, 2U, 1U);
    vaddw_x(cpu_, 3U);
    cpu_.write_vu0_vector(4U, result.unit);
    vaddx_y(cpu_, 1U, 5U);
    const Gpr128 saved_one = cpu_.read_vu0_vector(3U);    // qmfc2 v0,vf3
    vmul_xyz(cpu_, 2U, 1U, 4U);
    finish_dot(cpu_, saved_one, 2U);
    result.along = lane_x(cpu_, 1U);
    if (with_length) {
        vaddw_x(cpu_, 1U);
        vmul_xyz(cpu_, 2U, 4U, 4U);
        finish_dot(cpu_, cpu_.read_vu0_vector(1U), 2U);
        ee_cop2::vsqrt(cpu_, 1U, 0U);
        vaddw_x(cpu_, 1U);
        ee_cop2::vmul_control(cpu_, 1U, 1U, kLaneX, ee_cop2::kControlQ);
    }
    return result;
}

// (numerator / denominator) * the entry distance of the vehicle's own strip and quad (0x140734, 0x140704,
// 0x140764 and the divide that follows).
template <class Memory>
bool Projector<Memory>::scaled_by_own_distance(const std::uint32_t numerator, const std::uint32_t denominator,
                                               std::uint32_t& scaled) {
    const GuestAddress physics = m_.read_u32_le(vehicle_ + kPhysics);
    const std::uint32_t own_quad = m_.read_u16_le(physics + kOwnQuad);
    std::uint32_t own_distance = 0U;
    if (!entry_distance(m_.read_u32_le(physics + kOwnStrip), own_quad, own_distance)) {
        return false;
    }
    scaled = fpu::mul(fpu::div(numerator, denominator), own_distance);
    return true;
}

// Path X (0x13f8a0..0x13f9ac, 0x1405dc..0x140730): the geometry of the located quad. Near the quad's start
// (along <= half the vehicle's width) the centre is the ratio of the along distance to the quad's entry distance,
// scaled by the vehicle's own; far from it the same from the step-up direction, measured from the other end.
template <class Memory>
bool Projector<Memory>::path_x() {
    retired_ += kXFrontCost;
    if (!link_vertex(strip_, quad_, 2U, kStartQuad) || !step_cross(strip_, quad_, kNormalQuad)) {
        return false;
    }
    start_ = n3::load_quad_any(m_, frame_ + kStartQuad);
    const Along front = along_unit(n3::load_quad_any(m_, frame_ + kNormalQuad), start_, false);
    normal_ = front.unit;
    along_ = front.along;
    const std::uint32_t half_width = fpu::div(m_.read_u32_le(vehicle_ + kWidth), kTwo);
    std::uint32_t arc_length = 0U;
    std::uint32_t ratio = 0U;
    if (fpu::le(along_, half_width)) {
        retired_ += kXNearCost;
        if (!entry_distance(strip_, quad_, arc_length)) {
            return false;
        }
        if (is_tiny(arc_length)) {
            retired_ += kXTinyCost;
        } else {
            retired_ += kXNearRatioCost;
            if (!scaled_by_own_distance(along_, arc_length, ratio)) {
                return false;
            }
        }
        retired_ += kXNearBiasCost;
        centre_a_ = fpu::sub(ratio, m_.read_u32_le(vehicle_ + kNearBias));
    } else {
        retired_ += kXFarCost;
        if (!link_vertex(strip_, quad_, 1U, kFarStartQuad) || !step_up(strip_, quad_, kFarStepQuad)) {
            return false;
        }
        const Along far = along_unit(n3::load_quad_any(m_, frame_ + kFarStepQuad),
                                     n3::load_quad_any(m_, frame_ + kFarStartQuad), true);
        along_ = far.along;
        if (!entry_distance(strip_, quad_, arc_length)) {
            return false;
        }
        if (is_tiny(arc_length)) {
            retired_ += kXTinyCost;
        } else {
            retired_ += kXNearRatioCost;
            if (!scaled_by_own_distance(along_, arc_length, ratio)) {
                return false;
            }
        }
        retired_ += kXFarBiasCost;
        centre_a_ = fpu::sub(m_.read_u32_le(vehicle_ + kFarBias), ratio);
    }
    return core(false);
}

// Path Y (0x140064..0x140230): the quad is on the vehicle's own strip (reached through own_quad_). The start is the
// quad's pair vertex, the centre comes from the edge lengths and the sum of the lengths along the backward links
// from the own quad.
template <class Memory>
bool Projector<Memory>::path_y() {
    retired_ += kYFrontCost;
    const GuestAddress pairs = m_.read_u32_le(road_of(strip_) + kRoadPairs);
    const GuestAddress vertices = m_.read_u32_le(kVertexTable);
    const std::uint32_t vertex = m_.read_u16_le(pairs + 2U + (quad_ << 2U));
    start_ = n3::load_quad_any(m_, vertices + (vertex << 4U));
    if (!step_cross(strip_, quad_, kNormalQuad)) {
        return false;
    }
    const Along front = along_unit(n3::load_quad_any(m_, frame_ + kNormalQuad), start_, false);
    normal_ = front.unit;
    along_ = front.along;
    std::uint32_t quad_length = 0U;
    std::uint32_t own_length = 0U;
    if (!edge_length(strip_, quad_, quad_length) || !edge_length(strip_, own_quad_, own_length)) {
        return false;
    }
    std::uint32_t scaled = 0U;
    if (is_tiny(quad_length)) {
        retired_ += kXTinyCost;
    } else {
        retired_ += kYEdgeTinyCost;
        if (is_tiny(own_length)) {
            retired_ += kXTinyCost;
        } else {
            retired_ += kYEdgeTinyCost;
            scaled = fpu::mul(own_length, fpu::div(along_, quad_length));
        }
    }

    retired_ += kYMidCost;
    chain_sum_ = 0U;
    GuestAddress record = record_of(strip_, own_quad_);
    std::uint32_t id = m_.read_u8(record + 5U);
    std::uint32_t next = m_.read_u16_le(record + 8U);
    for (;;) {
        retired_ += kYChainTestCost;
        bool open = false;
        if (!route_open(id, next, open)) {
            return false;
        }
        if (!open) {
            break;
        }
        if (retired_ > kCeiling) {
            return false;
        }
        retired_ += kYChainStepCost;
        const GuestAddress other = strip_object(id);
        std::uint32_t length = 0U;
        if (!edge_length(other, next, length)) {
            return false;
        }
        chain_sum_ = fpu::add(chain_sum_, length);
        record = record_of(other, next);
        id = m_.read_u8(record + 5U);
        next = m_.read_u16_le(record + 8U);
    }
    centre_a_ = fpu::sub(fpu::add(scaled, chain_sum_), m_.read_u32_le(vehicle_ + kNearBias));
    return core(true);
}

// The VU0 core both paths share, first half (0x13f9d0..0x13fb3c): the step and its perpendicular become unit
// vectors; their dots with the axis Q4 and the box give extent_a_ (f22), and the dot of (Q3 - start) with the
// normal comes back as `along`. The general registers the original passes values through are rebuilt where their
// upper halves reach a vector register: mfc1 writes only the low doubleword, so the rest is what the register held
// (the unit step in a0, the unit perpendicular in v1, Q3 in a1).
template <class Memory>
void Projector<Memory>::core_first_half(std::uint32_t& along) {
    const Gpr128 step = n3::load_quad_any(m_, frame_ + kStepQuad);
    const Gpr128 up = n3::load_quad_any(m_, frame_ + kUpQuad);
    cpu_.write_vu0_vector(2U, up);
    cpu_.write_vu0_vector(4U, Gpr128{});         // qmtc2 of the cleared v0: x = 0 (vf4 is loaded in full below)
    cpu_.write_vu0_vector(3U, step);
    vaddx_y(cpu_, 2U, 4U);
    vaddw_x(cpu_, 1U);
    vaddx_y(cpu_, 3U, 4U);
    const Gpr128 flat_up = cpu_.read_vu0_vector(2U);
    normalise_vf3(cpu_, cpu_.read_vu0_vector(1U));
    step_unit_ = cpu_.read_vu0_vector(3U);
    cpu_.write_vu0_vector(3U, flat_up);
    vaddw_x(cpu_, 1U);
    normalise_vf3(cpu_, cpu_.read_vu0_vector(1U));
    up_unit_ = cpu_.read_vu0_vector(3U);

    // Dots of both unit vectors with Q4.
    cpu_.write_vu0_vector(2U, q4_);
    cpu_.write_vu0_vector(3U, step_unit_);
    vaddw_x(cpu_, 1U);
    Gpr128 saved_one = cpu_.read_vu0_vector(1U);
    vmul_xyz(cpu_, 3U, 3U, 2U);
    finish_dot(cpu_, saved_one, 3U);
    const std::uint32_t dot_step = lane_x(cpu_, 1U);
    cpu_.write_vu0_vector(1U, up_unit_);
    vmul_xyz(cpu_, 2U, 1U, 2U);
    const std::uint32_t half_step = fpu::mul(kHalf, fpu::abs(dot_step));
    vaddw_x(cpu_, 1U);
    saved_one = cpu_.read_vu0_vector(1U);
    finish_dot(cpu_, saved_one, 2U);
    const std::uint32_t dot_up = lane_x(cpu_, 1U);

    // The box and the extent: vf7 = g1*|dot_up| and vf6 = 0.5*|dot_step| scale the box's z and x differences.
    cpu_.write_vu0_vector(2U, q3_);
    const std::uint32_t gp = cpu_.read_gpr_u32(n3::kGlobalPointer);
    level_scale_ = m_.read_u32_le(gp + kGpLevelScale);
    const std::uint32_t scaled_up = fpu::mul(level_scale_, fpu::abs(dot_up));
    cpu_.write_vu0_vector(1U, start_);
    cpu_.write_vu0_vector(5U, Gpr128{{0U, 0U, up_unit_.words[2], up_unit_.words[3]}});   // mfc1 v1,f1 (f1 = 0); qmtc2
    vsub_xyz(cpu_, 1U, 2U, 1U);
    cpu_.write_vu0_vector(4U, normal_);
    vaddx_y(cpu_, 1U, 5U);
    cpu_.write_vu0_vector(9U, n3::load_quad_any(m_, box_));
    cpu_.write_vu0_vector(8U, n3::load_quad_any(m_, box_ + 16U));
    vmul_xyz(cpu_, 2U, 1U, 4U);
    cpu_.write_vu0_vector(7U, float_register(scaled_up, up_unit_));
    vsubz_z(cpu_, 1U, 9U, 8U);
    vmulx_z(cpu_, 1U, 1U, 7U);
    const std::uint32_t box_depth = cpu_.read_vu0_vector(1U).words[2];     // sqc2 vf1 / lwc1 f0: the z lane
    vaddw_x(cpu_, 3U);
    cpu_.write_vu0_vector(6U, float_register(half_step, step_unit_));
    vsubx_x(cpu_, 1U, 9U, 8U);
    vmulx_x(cpu_, 1U, 1U, 6U);
    const std::uint32_t box_width = lane_x(cpu_, 1U);
    saved_one = cpu_.read_vu0_vector(3U);
    extent_a_ = fpu::add(box_depth, box_width);
    vadday_x(cpu_, 2U);
    cpu_.write_vu0_vector(1U, saved_one);
    vmaddz_x(cpu_, 1U, 1U, 2U);
    along = lane_x(cpu_, 1U);
}

// Second half (0x13fb78..0x13fc24): the same with Q5, giving centre_b_ (f1) and extent_b_ (f6).
template <class Memory>
void Projector<Memory>::core_second_half(const std::uint32_t ratio) {
    centre_b_ = fpu::sub(ratio, m_.read_u32_le(vehicle_ + kNearBias));
    cpu_.write_vu0_vector(2U, step_unit_);
    cpu_.write_vu0_vector(3U, q5_);
    vaddw_x(cpu_, 1U);
    Gpr128 saved_one = cpu_.read_vu0_vector(1U);
    vmul_xyz(cpu_, 2U, 2U, 3U);
    finish_dot(cpu_, saved_one, 2U);
    const std::uint32_t dot_step = lane_x(cpu_, 1U);
    cpu_.write_vu0_vector(2U, up_unit_);
    const std::uint32_t half_step = fpu::mul(kHalf, fpu::abs(dot_step));
    vaddw_x(cpu_, 1U);
    saved_one = cpu_.read_vu0_vector(1U);
    vmul_xyz(cpu_, 2U, 2U, 3U);
    finish_dot(cpu_, saved_one, 2U);
    const std::uint32_t dot_up = lane_x(cpu_, 1U);
    cpu_.write_vu0_vector(4U, n3::load_quad_any(m_, box_));
    const std::uint32_t scaled_up = fpu::mul(level_scale_, fpu::abs(dot_up));
    cpu_.write_vu0_vector(3U, n3::load_quad_any(m_, box_ + 16U));
    cpu_.write_vu0_vector(1U, float_register(half_step, step_unit_));
    cpu_.write_vu0_vector(2U, float_register(scaled_up, q3_));
    vsubz_z(cpu_, 5U, 4U, 3U);
    vmulx_z(cpu_, 2U, 5U, 2U);
    const std::uint32_t box_depth = cpu_.read_vu0_vector(2U).words[2];
    vsubx_x(cpu_, 2U, 4U, 3U);
    vmulx_x(cpu_, 1U, 2U, 1U);
    extent_b_ = fpu::add(box_depth, lane_x(cpu_, 1U));
}

// Both paths: the step vectors, the first half, the centre from the arc (X) or edge (Y) lengths, the second half.
template <class Memory>
bool Projector<Memory>::core(const bool y_path) {
    retired_ += y_path ? kYCoreCost : kXCoreCost;
    if (!step_vector(strip_, quad_, kStepQuad) || !step_up(strip_, quad_, kUpQuad)) {
        return false;
    }
    std::uint32_t along = 0U;
    core_first_half(along);
    std::uint32_t ratio = 0U;
    if (!y_path) {
        std::uint32_t arc_length = 0U;
        if (!entry_distance(strip_, quad_, arc_length)) {
            return false;
        }
        if (is_tiny(arc_length)) {
            retired_ += kXTinyCost;
        } else {
            retired_ += kXCoreRatioCost;
            if (!scaled_by_own_distance(along, arc_length, ratio)) {
                return false;
            }
        }
        retired_ += kXTailCost;
    } else {
        std::uint32_t quad_length = 0U;
        std::uint32_t own_length = 0U;
        if (!edge_length(strip_, quad_, quad_length) || !edge_length(strip_, own_quad_, own_length)) {
            return false;
        }
        std::uint32_t scaled = 0U;
        if (is_tiny(quad_length)) {
            retired_ += kXTinyCost;
        } else {
            retired_ += kYEdgeTinyCost;
            if (is_tiny(own_length)) {
                retired_ += kXTinyCost;
            } else {
                retired_ += kYEdgeTinyCost;
                scaled = fpu::mul(own_length, fpu::div(along, quad_length));
            }
        }
        ratio = fpu::add(scaled, chain_sum_);
        retired_ += kYTailCost;
    }
    core_second_half(ratio);
    return true;
}

// MAX.S / MIN.S on raw words: the EE's sign/magnitude ordering (ee_cop1::max_s and min_s).
std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}
std::uint32_t min_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

// The column of a scaled extent: cvt.w.s, then 128 (the middle of the 256 columns).
std::int32_t column_of(const std::uint32_t scaled) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(fpu::to_int(scaled)) + 128U);
}

// The FPU condition flag, as the last c.ole.s / c.olt.s left it.
void set_condition(R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition
                             : cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition);
}

// Stage 6 (0x13fc28..0x140850): the three profile arrays. The two levels are blended towards 8.0 and 500.0 by the
// vehicle's blend factor (the COP1 accumulator ends as the second sum's start), the interval [lo, hi] of columns is
// found from the centres and extents, and each column takes the new first byte / profile id / second value where the
// stored one, scaled by a global, is larger than the new level.
template <class Memory>
bool Projector<Memory>::rasterise() {
    const std::uint32_t blend = m_.read_u32_le(vehicle_ + kBlend);
    const std::uint32_t first_gap = fpu::sub(kEight, first_level_);
    const std::uint32_t second_gap = fpu::sub(kFiveHundred, second_level_);
    const std::uint32_t first_blended = fpu::madd(fpu::add(0U, first_level_), blend, first_gap);    // f25
    const std::uint32_t accumulator = fpu::add(0U, second_level_);
    const std::uint32_t second_blended = fpu::madd(accumulator, blend, second_gap);                // f5
    cpu_.set_fpu_accumulator_bits(accumulator);
    const bool swapped = fpu::le(centre_a_, centre_b_);
    bool condition = swapped;
    set_condition(cpu_, condition);

    // Outside the 256 columns the profile is left alone.
    retired_ += kRasterCost;
    std::int32_t high = 0;
    std::int32_t low = 0;
    if (!swapped) {
        retired_ += kLowExitCost;
        high = column_of(fpu::mul(kFive, fpu::add(centre_a_, extent_a_)));
        if (high < 0) {
            retired_ += kEarlyLeaveCost + kEpilogueCost;
            return true;
        }
        retired_ += kHighExitCost;
        low = column_of(fpu::mul(kFive, fpu::sub(centre_b_, extent_b_)));
    } else {
        retired_ += kLowAltExitCost;
        high = column_of(fpu::mul(kFive, fpu::add(centre_b_, extent_b_)));
        if (high < 0) {
            retired_ += kEarlyLeaveCost + kEpilogueCost;
            return true;
        }
        retired_ += kHighAltExitCost;
        low = column_of(fpu::mul(kFive, fpu::sub(centre_a_, extent_a_)));
    }
    if (!(low < 256)) {
        retired_ += kEarlyLeaveCost + kEpilogueCost;
        return true;
    }
    retired_ += kRangeCost;
    std::int32_t column = low - 1;
    std::int32_t last = high + 1;
    if (column < 0) {
        retired_ += kRangeLowCost;
        column = 0;
    }
    retired_ += kRangeHighTestCost;
    if (!(last < 256)) {
        retired_ += kRangeHighCost;
        last = 255;
    }
    retired_ += kRasterSetupCost;

    const std::uint32_t level_byte = static_cast<std::uint32_t>(fpu::to_int(fpu::div(fpu::mul(k255, first_blended), kEight)));
    const std::uint32_t second_scaled = fpu::mul(kOneAndAHalf, second_blended);   // f7
    const std::uint32_t gp = cpu_.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t first_scale = m_.read_u32_le(gp + kGpFirstScale);
    const std::uint32_t second_scale = m_.read_u32_le(gp + kGpSecondScale);
    const std::uint32_t int16_max = fpu::from_int(kInt16Max);
    for (;;) {
        retired_ += kColumnTestCost;
        if (last < column) {
            break;
        }
        if (retired_ > kCeiling) {
            return false;
        }
        const GuestAddress at = vehicle_ + static_cast<GuestAddress>(column);
        retired_ += kColumnFirstCost;
        const std::uint32_t first_old = fpu::mul(first_scale, fpu::from_int(m_.read_u8(at + kProfileFirst)));
        if (fpu::lt(first_blended, first_old)) {
            retired_ += kColumnFirstStoreCost;
            m_.write_u8(at + kProfileFirst, static_cast<std::uint8_t>(level_byte));
            m_.write_u8(at + kProfileIds, static_cast<std::uint8_t>(profile_id_));
        }
        retired_ += kColumnSecondCost;
        const GuestAddress cell = vehicle_ + (static_cast<GuestAddress>(column) << 1U) + kProfileSecond;
        const std::uint32_t second_old =
            fpu::mul(second_scale, fpu::from_int(static_cast<std::int16_t>(m_.read_u16_le(cell))));
        condition = fpu::lt(second_scaled, second_old);
        if (condition) {
            retired_ += kColumnSecondStoreCost;
            const std::uint32_t clamped = min_word(max_word(second_scaled, kMinusFiveHundred), kFiveHundred);
            const std::int32_t value = fpu::to_int(fpu::div(fpu::mul(int16_max, clamped), kFiveHundred));
            m_.write_u16_le(cell, static_cast<std::uint16_t>(value));
            cpu_.set_fcr31_bits(cpu_.fcr31_bits() &
                                ~(guest::ee_cop1::kFcr31CauseOverflow | guest::ee_cop1::kFcr31CauseUnderflow));
        }
        retired_ += kColumnNextCost;
        ++column;
    }
    retired_ += (swapped ? 0U : kRasterJumpCost) + kLeaveCost + kEpilogueCost;
    set_condition(cpu_, condition);
    return true;
}

template <class Memory>
n3::Run Projector<Memory>::run() {
    if (!locate_quad()) {
        return n3::kDeclined;
    }
    if (found_ && !relate_to_own_strip()) {
        return n3::kDeclined;
    }
    retired_ += kRelatedTestCost;   // 0x13f7f8
    if (related_) {
        if (!path_y()) {
            return n3::kDeclined;
        }
    } else if (!scan_forward() || !path_x()) {
        return n3::kDeclined;
    }
    if (!rasterise()) {
        return n3::kDeclined;
    }
    return call_.finish(retired_);
}

template <class Context>
n3::Run project_route_into_profile(Context& context) {
    auto call = n3::guest_call(context);
    Projector<std::remove_cvref_t<decltype(context.memory)>> projector{call};
    return projector.run();
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013f550, 4864U, 0xa7cb71b5cf6bdbb2ULL, "ProjectRouteIntoProfile", kCeiling, project_route_into_profile,
         EngineReturn::Nothing)
