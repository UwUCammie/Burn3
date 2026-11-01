// 0x001c0f00 (a0 = the car's control record): UpdateVehicleControls.
//
// One per-frame step of a car's control state: it rescales the travel the
// object accumulated, optionally re-probes the two angle tests of the car's
// control block, then decides this frame's steering value and the display state
// that goes with it. Every store goes to the record itself (nothing is returned).
//
// The record (floats are raw R5900 words; the two quadwords keep a direction in
// xyz):
//   +176   quadword: xyz a direction, w the travel scalar the 130-degree curve
//          scales.              +192 quadword: a second direction used by the
//          two cornering phases.
//   +516   pointer to the orientation record (basis quads at +0 and +32).
//   +530   byte: skip the second probe.   +533 signed byte: drive mode, 3 = driving.
//   +2556/+2748 the two input angles the cornering test looks at.
//   +4452  the steering output written every frame.
//   +4984/+4988 the clamp window of the distance term; +4992/+4996 its baseline
//          and gain; +5000 the steering band; +5008/+5012 the curve band;
//          +5024 the angle bias; +5056/+5060/+5064 the 130-degree curve;
//          +5072 the dot normaliser; +5076 the speed limit scale;
//          +5088/+5092/+5096 the three probe angles; +5100/+5104 saved copies
//          of +4996/+5056.
//   +5108  pointer to the car's control block P1: its entry record starts at
//          +4288, +4924 its drive phase, +4928 a timestamp, +5512/+5760 the two
//          probe readings, +4300 the probe limit, +6028 the probe gate.
//   +5112  pointer to the input block P2 (+332/+340/+400 angles).
//   +5116  byte flags; +5120/+5124 the two input angles; +5128 the steering
//          value; +5140 an angle reference; +5148/+5152/+5156 last frame's
//          copies; +5164/+5168/+5176/+5184 the timers and the smoothed angle;
//          +5188/+5190 and +5416..+5419 mode bytes; +5280/+5284/+5288 the event
//          stamp, flag and reset request; +5304/+5312/+5316 the shift arming
//          flags and latches; +5320/+5324 the step index and its limit;
//          +5408 the probe result; +5412 the cornering phase; +5436 a sign flag.
//
// Paths: 34 instructions of prologue and 13 of epilogue; each optional test
// and update below adds its named block cost, including tests whose updates
// are skipped. There are no data-dependent loops. The caller alone has a
// conservative longest path of 565; add at most three 16-instruction probes
// and the 381-instruction ramp for a total bound of 994. Captures retire
// 429..571 including callees. The cold call at 0x001c1a28 declines because
// 0x00156e20 has no native replacement.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>

N3_CALLEE(001c6f90)
N3_CALLEE(00156170)
N3_CALLEE(001560d0)

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;
using n3::GuestAddress;

// Drive mode at +533 and the two cornering phases of the control block.
constexpr std::int32_t kDrivingMode = 3;
constexpr std::uint32_t kCorneringPhase = 3U;

// Record offsets.
constexpr GuestAddress kMotionQuad = 176U;
constexpr GuestAddress kAxisQuad = 192U;
constexpr GuestAddress kOrientationPointer = 516U;
constexpr GuestAddress kProbeSkipByte = 530U;
constexpr GuestAddress kModeByte = 533U;
constexpr GuestAddress kRateAngleLow = 2556U;    // lower candidate for the ramp's rate scale
constexpr GuestAddress kRateAngleHigh = 2748U;   // higher candidate
constexpr GuestAddress kSteerOutput = 4452U;
constexpr GuestAddress kDistanceLow = 4984U;
constexpr GuestAddress kDistanceHigh = 4988U;
constexpr GuestAddress kDistanceBaseline = 4992U;
constexpr GuestAddress kDistanceGain = 4996U;
constexpr GuestAddress kSteerBand = 5000U;
constexpr GuestAddress kCurveHigh = 5008U;
constexpr GuestAddress kCurveLow = 5012U;
constexpr GuestAddress kAngleBias = 5024U;
constexpr GuestAddress kCurveOutputLow = 5056U;
constexpr GuestAddress kCurveCut = 5060U;
constexpr GuestAddress kCurveFull = 5064U;
constexpr GuestAddress kDotNormaliser = 5072U;
constexpr GuestAddress kSpeedLimitScale = 5076U;
constexpr GuestAddress kProbeAngleNear = 5088U;
constexpr GuestAddress kProbeAngleFar = 5092U;
constexpr GuestAddress kProbeAngleLimit = 5096U;
constexpr GuestAddress kSavedDistanceGain = 5100U;
constexpr GuestAddress kSavedCurveOutputLow = 5104U;
constexpr GuestAddress kControlPointer = 5108U;
constexpr GuestAddress kInputPointer = 5112U;
constexpr GuestAddress kFlagBytes = 5116U;
constexpr GuestAddress kYaw = 5120U;
constexpr GuestAddress kPitch = 5124U;
constexpr GuestAddress kSteer = 5128U;
constexpr GuestAddress kSteerReference = 5140U;
constexpr GuestAddress kLastYaw = 5148U;
constexpr GuestAddress kLastPitch = 5152U;
constexpr GuestAddress kLastSteer = 5156U;
constexpr GuestAddress kHoldTimer = 5164U;
constexpr GuestAddress kHeldAngle = 5168U;
constexpr GuestAddress kOverTimer = 5176U;
constexpr GuestAddress kSmoothedAngle = 5184U;
constexpr GuestAddress kReverseByte = 5188U;
constexpr GuestAddress kEngagedByte = 5190U;
constexpr GuestAddress kDriveStateRecord = 5192U;
constexpr GuestAddress kEventStamp = 5280U;
constexpr GuestAddress kEventFlag = 5284U;
constexpr GuestAddress kResetRequest = 5288U;
constexpr GuestAddress kShiftArmed = 5304U;
constexpr GuestAddress kShiftUpLatch = 5312U;
constexpr GuestAddress kShiftDownLatch = 5316U;
constexpr GuestAddress kStep = 5320U;
constexpr GuestAddress kStepLimit = 5324U;
constexpr GuestAddress kProbeResult = 5408U;
constexpr GuestAddress kCorneringFlag = 5412U;
constexpr GuestAddress kArrowByte1 = 5416U;
constexpr GuestAddress kArrowByte2 = 5417U;
constexpr GuestAddress kArrowByte3 = 5418U;
constexpr GuestAddress kArrowByte4 = 5419U;
constexpr GuestAddress kSignByte = 5436U;

// The control block P1's own fields (a0 = P1 + kEntryRecord for the calls).
constexpr GuestAddress kEntryRecord = 4288U;
constexpr GuestAddress kDrivePhase = 4924U;
constexpr GuestAddress kBlockTimestamp = 4928U;
constexpr GuestAddress kProbeLimit = 4300U;
constexpr GuestAddress kProbeNearReading = 5512U;
constexpr GuestAddress kProbeFarReading = 5760U;
constexpr GuestAddress kProbeGate = 6028U;

// The input block P2's angles.
constexpr GuestAddress kInputYaw = 332U;
constexpr GuestAddress kInputPitch = 340U;
constexpr GuestAddress kInputRoll = 400U;

// Frame globals the listing reads by absolute address.
constexpr GuestAddress kNow = 0x00522680U;         // lui 0x52, lwc1 9856
constexpr GuestAddress kFrameBlend = 0x0052267cU;  // lui 0x52, lwc1 9852

// $gp-relative floats (address = $gp - offset, as the listing addresses them).
constexpr std::uint32_t kGpEventStamp = 32456U;     // stamped into +5280
constexpr std::uint32_t kGpProbeFarScale = 31792U;  // far angle scale and floor
constexpr std::uint32_t kGpSmallAngle = 31320U;     // the small-angle floor
constexpr std::uint32_t kGpProbeWideScale = 31144U; // probe retry scale
constexpr std::uint32_t kGpAngleFloor = 31024U;     // angle floor
constexpr std::uint32_t kGpTravelScale = 30968U;    // travel -> curve multiplier
constexpr std::uint32_t kGpDistanceGain = 30964U;   // distance term gain
constexpr std::uint32_t kGpSpeedThreshold = 30940U; // speed limit scale
constexpr std::uint32_t kGpSteerSmall = 30936U;     // steering dead band
constexpr std::uint32_t kGpSteerKill = 30932U;      // steering cut-off

// Raw float words the listing builds with lui/mtc1.
constexpr std::uint32_t kZero = 0x00000000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kMinusHalf = 0xbf000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;
constexpr std::uint32_t kThree = 0x40400000U;
constexpr std::uint32_t kMinusTenAndHalf = 0xc1280000U;
constexpr std::uint32_t kTwentyFive = 0x41c80000U;
constexpr std::uint32_t kThirty = 0x41f00000U;
constexpr std::uint32_t kFortyThree = 0x422c0000U;
constexpr std::uint32_t kOneThirty = 0x43020000U;

// Retired-instruction counts of the original's straight runs (brief.py).
constexpr std::uint32_t kPrologue = 34U;             // 0x0f00..0x0f84
constexpr std::uint32_t kLoadInputAngles = 9U;       // 0x0f88..0x0fa8
constexpr std::uint32_t kProbeGateTest = 5U;         // 0x0fac..0x0fbc
constexpr std::uint32_t kEngagedTest = 3U;           // 0x0fc0..0x0fc8
constexpr std::uint32_t kStepRangeTest = 4U;         // 0x0fcc..0x0fd8
constexpr std::uint32_t kStepPendingTest = 3U;       // 0x0fdc..0x0fe4
constexpr std::uint32_t kStepDot = 14U;              // 0x0fe8..0x101c
constexpr std::uint32_t kCheckBackStep = 3U;         // 0x1020..0x1028
constexpr std::uint32_t kCheckIdleStep = 2U;         // 0x102c..0x1030
constexpr std::uint32_t kIdleStepMotion = 8U;        // 0x1034..0x1050
constexpr std::uint32_t kIdleStepRaise = 2U;         // 0x1054..0x1058
constexpr std::uint32_t kBackStepHandler = 5U;       // 0x1878..0x1888
constexpr std::uint32_t kBackStepSmall = 4U;         // 0x188c..0x1898
constexpr std::uint32_t kBackStepDot = 6U;           // 0x189c..0x18b0
constexpr std::uint32_t kBackStepEngaged = 3U;       // 0x18b4..0x18bc
constexpr std::uint32_t kBackStepStore = 6U;         // 0x18c0..0x18d4
constexpr std::uint32_t kForwardStepHandler = 5U;    // 0x18d8..0x18e8
constexpr std::uint32_t kForwardStepSmall = 4U;      // 0x18ec..0x18f8
constexpr std::uint32_t kForwardStepDot = 6U;        // 0x18fc..0x1910
constexpr std::uint32_t kForwardStepStore = 5U;      // 0x1914..0x1924
constexpr std::uint32_t kShiftFlagsTest = 3U;        // 0x105c..0x1064
constexpr std::uint32_t kShiftUpFlags = 4U;          // 0x1068..0x1074
constexpr std::uint32_t kShiftUpLatchClear = 2U;     // 0x1928..0x192c
constexpr std::uint32_t kShiftUpLatchTest = 3U;      // 0x1078..0x1080
constexpr std::uint32_t kShiftUpRange = 3U;          // 0x1084..0x108c
constexpr std::uint32_t kShiftUpLimit = 4U;          // 0x1090..0x109c
constexpr std::uint32_t kShiftUpStore = 6U;          // 0x10a0..0x10b4
constexpr std::uint32_t kShiftUpLatchSet = 2U;       // 0x10b8..0x10bc
constexpr std::uint32_t kShiftDownFlags = 4U;        // 0x10c0..0x10cc
constexpr std::uint32_t kShiftDownLatchClear = 2U;   // 0x194c..0x1950
constexpr std::uint32_t kShiftDownLatchTest = 3U;    // 0x10d0..0x10d8
constexpr std::uint32_t kShiftDownLimit = 4U;        // 0x10dc..0x10e8
constexpr std::uint32_t kShiftDownStore = 7U;        // 0x1930..0x1948
constexpr std::uint32_t kShiftDownLatchSet = 3U;     // 0x10ec..0x10f4
constexpr std::uint32_t kFallbackTest = 4U;          // 0x10f8..0x1104
constexpr std::uint32_t kProbeAge = 10U;             // 0x1108..0x112c
constexpr std::uint32_t kProbeAgeZero = 2U;          // 0x1954..0x1958
constexpr std::uint32_t kProbeAgeByte = 3U;          // 0x1130..0x1138
constexpr std::uint32_t kProbeAgeBand = 7U;          // 0x113c..0x1154
constexpr std::uint32_t kProbeAgeClear = 4U;         // 0x1158..0x1164
constexpr std::uint32_t kProbeAgeCleared = 1U;       // 0x1168
constexpr std::uint32_t kArrowTest = 6U;             // 0x116c..0x1180
constexpr std::uint32_t kArrowBandTest = 6U;         // 0x1184..0x1198
constexpr std::uint32_t kArrowByteTest = 3U;         // 0x119c..0x11a4
constexpr std::uint32_t kArrowSecondByte = 3U;       // 0x11a8..0x11b0
constexpr std::uint32_t kArrowSet = 3U;              // 0x11b4..0x11bc
constexpr std::uint32_t kArrowClear = 2U;            // 0x11c0..0x11c4
constexpr std::uint32_t kArrowClearByte = 2U;        // 0x195c..0x1960
constexpr std::uint32_t kArrowEnter = 6U;            // 0x19d8..0x19ec
constexpr std::uint32_t kArrowReadByte3 = 2U;        // 0x19f0..0x19f4
constexpr std::uint32_t kArrowCheckByte3 = 2U;       // 0x1964..0x1968
constexpr std::uint32_t kArrowCheckByte4 = 2U;       // 0x196c..0x1970
constexpr std::uint32_t kArrowFirstByte4 = 3U;       // 0x1974..0x197c
constexpr std::uint32_t kArrowAngleTest = 5U;        // 0x1980..0x1990
constexpr std::uint32_t kArrowEngagedTest = 3U;      // 0x1994..0x199c
constexpr std::uint32_t kArrowEngagedSet = 1U;       // 0x19a0
constexpr std::uint32_t kArrowEngagedStore = 1U;     // 0x19a4
constexpr std::uint32_t kArrowLateByte4 = 3U;        // 0x19f8..0x1a00
constexpr std::uint32_t kArrowLateAngle = 5U;        // 0x1a04..0x1a14
constexpr std::uint32_t kArrowLateSet = 3U;          // 0x1a18..0x1a20
constexpr std::uint32_t kArrowByteCopy = 3U;         // 0x19a8..0x19b0
constexpr std::uint32_t kArrowCopyOne = 3U;          // 0x19b4..0x19bc
constexpr std::uint32_t kArrowCopyZero = 1U;         // 0x19c0
constexpr std::uint32_t kArrowFinish = 5U;           // 0x19c4..0x19d4
constexpr std::uint32_t kStepJoin = 3U;              // 0x11c8..0x11d0
constexpr std::uint32_t kSwapAngles = 5U;            // 0x1a38..0x1a48
constexpr std::uint32_t kDistanceTerm = 11U;         // 0x11d4..0x11fc
constexpr std::uint32_t kDistanceFloor = 1U;         // 0x1200
constexpr std::uint32_t kDistanceHighTest = 4U;      // 0x1204..0x1210
constexpr std::uint32_t kDistanceHighSet = 2U;       // 0x1214..0x1218
constexpr std::uint32_t kSteerBandTest = 5U;         // 0x1a4c..0x1a5c
constexpr std::uint32_t kSteerBandPicked = 2U;       // 0x1a60..0x1a64
constexpr std::uint32_t kSteerOverBand = 4U;         // 0x121c..0x1228
constexpr std::uint32_t kSteerClampHigh = 3U;        // 0x122c..0x1234
constexpr std::uint32_t kSteerUnderBand = 6U;        // 0x1238..0x124c
constexpr std::uint32_t kSteerClampLow = 2U;         // 0x1250..0x1254
constexpr std::uint32_t kProbeResultTest = 2U;       // 0x1258..0x125c
constexpr std::uint32_t kSecondProbeTest = 2U;       // 0x1260..0x1264
constexpr std::uint32_t kSteerSignByte = 3U;         // 0x1268..0x1270
constexpr std::uint32_t kSteerSignSet = 3U;          // 0x1274..0x127c
constexpr std::uint32_t kSteerSignClear = 2U;        // 0x1280..0x1284
constexpr std::uint32_t kSteerOutputBlock = 9U;      // 0x1288..0x12a8
constexpr std::uint32_t kSteerCall = 13U;            // 0x12ac..0x12dc or 0x12e0..0x1310
constexpr std::uint32_t kReverseFlags = 4U;          // 0x1314..0x1320
constexpr std::uint32_t kReverseSet = 3U;            // 0x1324..0x132c
constexpr std::uint32_t kReverseClear = 1U;          // 0x1330
constexpr std::uint32_t kReverseByteTest = 3U;       // 0x1334..0x133c
constexpr std::uint32_t kProbeSignTest = 6U;         // 0x1340..0x1354
constexpr std::uint32_t kStepMatchTest = 6U;         // 0x1358..0x136c
constexpr std::uint32_t kSpeedSignTest = 6U;         // 0x1370..0x1384
constexpr std::uint32_t kProbeHalve = 5U;            // 0x1388..0x1398
constexpr std::uint32_t kSpeedTest = 6U;             // 0x139c..0x13b0
constexpr std::uint32_t kSpeedZero = 2U;             // 0x1a68..0x1a6c
constexpr std::uint32_t kCurveValue = 22U;           // 0x13b4..0x1408
constexpr std::uint32_t kCurveGate = 4U;             // 0x140c..0x1418
constexpr std::uint32_t kCurveGateBias = 5U;         // 0x141c..0x142c
constexpr std::uint32_t kCurveGatePitch = 3U;        // 0x1430..0x1438
constexpr std::uint32_t kCorneringEnter = 2U;        // 0x143c..0x1440
constexpr std::uint32_t kPhaseTest = 5U;             // 0x1a70..0x1a80
constexpr std::uint32_t kPhasePrepare = 2U;          // 0x1a84..0x1a88
constexpr std::uint32_t kCorneringBlend = 38U;       // 0x1444..0x14d8
constexpr std::uint32_t kCorneringShift = 5U;        // 0x1a8c..0x1a9c
constexpr std::uint32_t kSmoothedLowTest = 5U;       // 0x14dc..0x14ec
constexpr std::uint32_t kSmoothedLowSet = 2U;        // 0x1aa0..0x1aa4
constexpr std::uint32_t kSmoothedHighTest = 5U;      // 0x14f0..0x1500
constexpr std::uint32_t kSmoothedHighSet = 2U;       // 0x1aa8..0x1aac
constexpr std::uint32_t kSmoothedBlendCurve = 10U;   // 0x1504..0x1528
constexpr std::uint32_t kSmoothedBlendBand = 9U;     // 0x152c..0x154c
constexpr std::uint32_t kCorneringStateTest = 3U;    // 0x1550..0x1558
constexpr std::uint32_t kHoldTimerReset = 7U;        // 0x1ad0..0x1ae8
constexpr std::uint32_t kSteerDeadBand = 6U;         // 0x155c..0x1570
constexpr std::uint32_t kOverTimerClear = 2U;        // 0x1ab0..0x1ab4
constexpr std::uint32_t kOverSpeedTest = 9U;         // 0x1574..0x1594
constexpr std::uint32_t kOverTimerAdd = 5U;          // 0x1598..0x15a8
constexpr std::uint32_t kAngleBiasBlock = 18U;       // 0x15ac..0x15f0
constexpr std::uint32_t kAngleBiasFloor = 1U;        // 0x15f4
constexpr std::uint32_t kOverFast = 6U;              // 0x15f8..0x160c
constexpr std::uint32_t kOverFastLow = 1U;           // 0x1610
constexpr std::uint32_t kOverTimerTest = 2U;         // 0x1614..0x1618
constexpr std::uint32_t kOverTimerCompare = 4U;      // 0x161c..0x1628
constexpr std::uint32_t kOverTimerLow = 1U;          // 0x162c
constexpr std::uint32_t kHoldDecision = 2U;          // 0x1630..0x1634
constexpr std::uint32_t kHoldSkip = 2U;              // 0x1638..0x163c
constexpr std::uint32_t kSteerSignTest = 5U;         // 0x1640..0x1650
constexpr std::uint32_t kHoldTimerSet = 6U;          // 0x1ab8..0x1acc
constexpr std::uint32_t kSteerKillTest = 4U;         // 0x1654..0x1660
constexpr std::uint32_t kHoldTimerStart = 5U;        // 0x1664..0x1674
constexpr std::uint32_t kLastYawTest = 3U;           // 0x1678..0x1680
constexpr std::uint32_t kLastYawClear = 2U;          // 0x1684..0x1688
constexpr std::uint32_t kLastYawStore = 2U;          // 0x168c..0x1690
constexpr std::uint32_t kSaveAngles = 7U;            // 0x1694..0x16ac
constexpr std::uint32_t kEpilogue = 13U;             // 0x1aec..0x1b1c
constexpr std::uint32_t kProbeStart = 7U;            // 0x16b0..0x16c8
constexpr std::uint32_t kProbeRetry = 10U;           // 0x17bc..0x17e0
constexpr std::uint32_t kProbeRetryShift = 4U;       // 0x17e4..0x17f0
constexpr std::uint32_t kSecondProbe = 3U;           // 0x16cc..0x16d4
constexpr std::uint32_t kSecondProbePick = 2U;       // 0x16d8..0x16dc
constexpr std::uint32_t kSecondProbeDirect = 6U;     // 0x16e0..0x16f4
constexpr std::uint32_t kSecondProbeShifted = 5U;    // 0x16f8..0x1708
constexpr std::uint32_t kProbeResultGate = 2U;       // 0x170c..0x1710
constexpr std::uint32_t kProbeSkipTest = 3U;         // 0x1714..0x171c
constexpr std::uint32_t kProbeChoice = 2U;           // 0x1720..0x1724
constexpr std::uint32_t kProbeShiftChoice = 2U;      // 0x1728..0x172c
constexpr std::uint32_t kProbeBlendNear = 13U;       // 0x1730..0x1760
constexpr std::uint32_t kProbeBlendFar = 10U;        // 0x1764..0x1788
constexpr std::uint32_t kProbeBlend = 7U;            // 0x178c..0x17a4
constexpr std::uint32_t kProbeSavedAngles = 5U;      // 0x17a8..0x17b8
constexpr std::uint32_t kProbeReading = 3U;          // 0x17f4..0x17fc
constexpr std::uint32_t kProbeReload = 10U;          // 0x1800..0x1824
constexpr std::uint32_t kEngagedReset = 12U;         // 0x1828..0x1854
constexpr std::uint32_t kEngagedKindTest = 6U;       // 0x1858..0x186c
constexpr std::uint32_t kEngagedClear = 2U;          // 0x1870..0x1874

// The frame the original pushes (addiu $sp, $sp, -352) and the calls inherit.
constexpr std::uint32_t kFrameBytes = 352U;

// A call into one of the two probe tests: v0 & 0xff, or "needs the original".
struct ProbeResult {
    bool declined;
    std::uint32_t value;
};

// The VU0 lane operations the original uses for the two dot products and for
// the travel scaling. The VU multiplies by truncating the significand and
// clamps its operands, which COP1 does not, so the guest macro helpers run
// these three steps.
std::uint32_t vu_scale_w(guest::R5900State& cpu, const n3::Gpr128& motion, const std::uint32_t scale) {
    n3::Gpr128 scalar{};
    scalar.words[0] = scale;
    cpu.write_vu0_vector(1U, motion);
    cpu.write_vu0_vector(2U, scalar);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskW, 0U); // vmulx.w
    return cpu.read_vu0_vector(1U).words[3];
}

// vmul.xyz, then vadday.x and vmaddz.x add the three products (the 1.0 the
// original parks in VF1.x makes the last step a plain add).
std::uint32_t vu_dot3(guest::R5900State& cpu, const n3::Gpr128& a, const n3::Gpr128& b) {
    n3::Gpr128 one{};
    one.words[0] = kOne;
    cpu.write_vu0_vector(1U, b);
    cpu.write_vu0_vector(2U, a);
    ee_cop2::vmul(cpu, 2U, 2U, 1U, n3::kMaskXyz);              // vf2 = a * b
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 0x08U, 1U);          // ACC.x = vf2.x + vf2.y
    cpu.write_vu0_vector(1U, one);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 0x08U, 2U, false, false, false); // vf1.x = ACC.x + 1*vf2.z
    return cpu.read_vu0_vector(1U).words[0];
}

// MAX.S: the R5900 compares the raw words as signed integers unless both are
// negative (then it takes the larger magnitude).
std::uint32_t max_s(const std::uint32_t a, const std::uint32_t b) {
    const auto x = std::bit_cast<std::int32_t>(a);
    const auto y = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(x < 0 && y < 0 ? std::min(x, y) : std::max(x, y));
}

template <class Context>
n3::Run update_vehicle_controls(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress o = call.arg(0U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto gp_float = [&](const std::uint32_t offset) { return m.read_u32_le(gp - offset); };

    std::uint32_t retired = kPrologue;

    // The motion quad's w drives the update; its scaled copy is the last term of
    // the 130-degree curve.
    const std::uint32_t travel = m.read_u32_le(o + kMotionQuad + 12U);
    const std::uint32_t travel_scaled =
        vu_scale_w(cpu, n3::load_quad_any(m, o + kMotionQuad), gp_float(kGpTravelScale));
    std::uint32_t far_angle = m.read_u32_le(o + kProbeAngleFar);
    std::uint32_t near_angle = m.read_u32_le(o + kProbeAngleNear);
    std::uint32_t limit_angle = m.read_u32_le(o + kProbeAngleLimit);
    const std::int32_t mode = static_cast<std::int8_t>(m.read_u8(o + kModeByte));

    // The two probe verdicts (s1, s2), whether the retry widened the angles (s0)
    // and whether the cornering branch was taken (s3).
    std::uint32_t probe_far = 0U;
    std::uint32_t probe_near = 0U;
    bool widened = false;
    bool cornering = false;

    // The car control block's two probe tests: a0 is its entry record, f12 the
    // probe angle, and the caller counts its own instructions.
    const auto remaining_range = [&](const std::uint32_t angle) {
        call.set_arg(0U, m.read_u32_le(o + kControlPointer) + kEntryRecord);
        call.set_float_arg(0U, angle);
        const n3::Run run = call.invoke([](auto& machine) { return n3::fn_00156170(machine); }, kFrameBytes);
        if (!run.declined) retired += run.retired;
        return ProbeResult{run.declined, run.declined ? 0U : call.result() & 0xffU};
    };
    const auto threshold_reached = [&](const std::uint32_t angle) {
        call.set_arg(0U, m.read_u32_le(o + kControlPointer) + kEntryRecord);
        call.set_float_arg(0U, angle);
        const n3::Run run = call.invoke([](auto& machine) { return n3::fn_001560d0(machine); }, kFrameBytes);
        if (!run.declined) retired += run.retired;
        return ProbeResult{run.declined, run.declined ? 0U : call.result() & 0xffU};
    };

    // ---- the modes that are not plain driving re-probe the block -----------
    bool reload_angles = true;
    std::uint32_t reload_cost = kLoadInputAngles;
    if (mode != kDrivingMode) {
        // A blocked probe is retried once with both angles and the limit widened.
        retired += kProbeStart;
        const ProbeResult first = remaining_range(far_angle);
        if (first.declined) return call.decline();
        probe_far = first.value;
        if (probe_far == 0U) {
            retired += kProbeRetry;
            far_angle = fpu::mul(far_angle, gp_float(kGpProbeWideScale));
            near_angle = fpu::mul(near_angle, gp_float(kGpProbeWideScale));
            const ProbeResult retry = threshold_reached(far_angle);
            if (retry.declined) return call.decline();
            probe_far = retry.value;
            if (probe_far != 0U) {
                retired += kProbeRetryShift;
                widened = true;
                limit_angle = fpu::mul(limit_angle, gp_float(kGpProbeFarScale));
            }
        }
        // The second probe is skipped when the control block asks for it.
        retired += kSecondProbe;
        if (m.read_u8(o + kProbeSkipByte) == 0U) {
            ProbeResult second{false, 0U};
            retired += kSecondProbePick; // 0x16d8: 2
            if (widened) {
                retired += kSecondProbeShifted;
                second = threshold_reached(near_angle);
            } else {
                retired += kSecondProbeDirect;
                second = remaining_range(near_angle);
            }
            if (second.declined) return call.decline();
            probe_near = second.value;
        }
        reload_cost = kProbeReload;
        retired += kProbeResultGate; // 0x170c: 2
        if (probe_far != 0U) {
            retired += kProbeSkipTest;
            if (m.read_u8(o + kProbeSkipByte) == 0U) {
                // The probes decided the distance term themselves.
                reload_angles = false;
                retired += kProbeChoice;
                if (probe_near != 0U) {
                    retired += kProbeReading;
                    m.write_u32_le(o + kDistanceHigh, m.read_u32_le(o + kProbeAngleLimit));
                } else {
                    // The angle between the two probe readings sets the mix
                    // between the block's own reading and the limit.
                    retired += kProbeShiftChoice; // 0x1728: 2
                    retired += widened ? kProbeBlendFar : kProbeBlendNear;
                    const std::uint32_t control = m.read_u32_le(o + kControlPointer);
                    const std::uint32_t reading =
                        m.read_u32_le(control + (widened ? kProbeFarReading : kProbeNearReading));
                    const std::uint32_t span = fpu::sub(far_angle, near_angle);
                    const std::uint32_t ratio = fpu::div(
                        fpu::sub(m.read_u32_le(control + kProbeLimit), fpu::add(reading, near_angle)), span);
                    const std::uint32_t weight = fpu::sub(kOne, ratio);
                    retired += kProbeBlend;
                    const std::uint32_t input = m.read_u32_le(m.read_u32_le(o + kInputPointer) + kInputYaw);
                    m.write_u32_le(o + kDistanceHigh,
                                   fpu::madd(fpu::add(kZero, input), weight,
                                             fpu::sub(limit_angle, input)));
                }
                retired += kProbeSavedAngles;
                m.write_u32_le(o + kDistanceGain, m.read_u32_le(o + kSavedDistanceGain));
                m.write_u32_le(o + kCurveOutputLow, m.read_u32_le(o + kSavedCurveOutputLow));
            }
        }
    }
    if (reload_angles) {
        retired += reload_cost;
        const GuestAddress input = m.read_u32_le(o + kInputPointer);
        m.write_u32_le(o + kDistanceHigh, m.read_u32_le(input + kInputYaw));
        m.write_u32_le(o + kDistanceGain, m.read_u32_le(input + kInputPitch));
        m.write_u32_le(o + kCurveOutputLow, m.read_u32_le(input + kInputRoll));
    }

    // ---- the steering state ------------------------------------------------
    bool skip_shift_flags = false;
    retired += kProbeGateTest; // 0x0fac: 5
    const GuestAddress control = m.read_u32_le(o + kControlPointer);
    if (m.read_u32_le(control + kProbeGate) != 1U) {
        // Outside the probe gate, non-driving modes update the display bytes
        // from the signed difference between the probe timestamp and now.
        retired += kFallbackTest;
        skip_shift_flags = true;
        if (mode != kDrivingMode) {
            retired += kProbeAge;
            const std::uint32_t age =
                fpu::sub(m.read_u32_le(control + kBlockTimestamp), m.read_u32_le(kNow));
            if (!fpu::le(m.read_u32_le(o + kPitch), kHalf)) {
                retired += kProbeAgeZero;
                m.write_u8(o + kArrowByte1, 1U);
            }
            retired += kProbeAgeByte;
            if (m.read_u8(o + kArrowByte1) != 0U) {
                retired += kProbeAgeBand;
                if (fpu::lt(m.read_u32_le(o + kYaw), kHalf)) {
                    retired += kProbeAgeClear;
                    if (!fpu::le(m.read_u32_le(o + kLastYaw), m.read_u32_le(o + kYaw))) {
                        retired += kProbeAgeCleared;
                        m.write_u8(o + kArrowByte1, 0U);
                    }
                }
            }

            // The indicator state: fresh enough to keep, or reset with the motion
            // quad and the step index.
            bool arrow_declined = false;
            const auto arrow_state = [&](const std::uint32_t probe_age) {
                retired += kArrowEnter;
                if (fpu::lt(probe_age, kHalf)) {
                    retired += kArrowReadByte3 + kArrowCheckByte3;
                    const std::uint32_t third = m.read_u8(o + kArrowByte3);
                    if (third == 0U) {
                        retired += kArrowCheckByte4;
                    } else {
                        retired += kArrowLateByte4;
                        if (m.read_u8(o + kArrowByte4) != 0U) {
                            retired += kArrowCheckByte4 + kArrowFirstByte4 + kArrowAngleTest;
                            if (!fpu::le(m.read_u32_le(o + kYaw), gp_float(kGpProbeFarScale))) {
                                retired += kArrowEngagedTest;
                                if (m.read_u8(o + kEngagedByte) == 0U) {
                                    // 0x1a28 calls 0x00156e20, which has no native.
                                    arrow_declined = true;
                                    return;
                                }
                                retired += kArrowEngagedSet + kArrowEngagedStore;
                                m.write_u8(o + kEngagedByte, 1U);
                            }
                        } else {
                            retired += kArrowLateAngle;
                            if (fpu::lt(m.read_u32_le(o + kYaw), gp_float(kGpAngleFloor))) {
                                retired += kArrowLateSet;
                                m.write_u8(o + kArrowByte4, 1U);
                            }
                        }
                    }
                }
                retired += kArrowByteCopy;
                if (m.read_u8(o + kArrowByte1) != 0U) {
                    retired += kArrowCopyOne;
                    m.write_u32_le(o + kStep, 1U);
                } else {
                    retired += kArrowCopyZero;
                    m.write_u32_le(o + kStep, 0U);
                }
                retired += kArrowFinish;
                m.write_u32_le(o + kResetRequest, 1U);
                n3::store_quad_any(m, o + kMotionQuad, n3::Gpr128{});
            };

            retired += kArrowTest;
            if (!fpu::le(age, kOne)) {
                arrow_state(age);
            } else {
                retired += kArrowBandTest;
                if (fpu::lt(age, kHalf)) {
                    arrow_state(age);
                } else {
                    retired += kArrowByteTest;
                    if (m.read_u8(o + kArrowByte1) == 0U) {
                        retired += kArrowClearByte;
                        m.write_u8(o + kArrowByte3, 0U);
                    } else {
                        retired += kArrowSecondByte;
                        if (m.read_u8(o + kArrowByte2) == 0U) {
                            retired += kArrowClear;
                            m.write_u8(o + kArrowByte3, 0U);
                        } else {
                            retired += kArrowSet;
                            m.write_u8(o + kArrowByte3, 1U);
                        }
                    }
                    arrow_state(age);
                }
            }
            if (arrow_declined) return call.decline();
        }
    }

    if (!skip_shift_flags) {
        // The mode byte at +5190 starts a fresh approach: the input angles are
        // reset and the age of the block's timestamp decides whether it lasts.
        retired += kEngagedTest;
        if (m.read_u8(o + kEngagedByte) != 0U) {
            retired += kEngagedReset;
            m.write_u32_le(o + kPitch, 0U);
            m.write_u32_le(o + kYaw, kOne);
            m.write_u32_le(o + kSteerReference, kOne);
            const std::uint32_t since =
                fpu::sub(m.read_u32_le(kNow), m.read_u32_le(control + kBlockTimestamp));
            if (mode != kDrivingMode) {
                retired += kEngagedKindTest;
                if (!fpu::le(since, kThree)) {
                    retired += kEngagedClear;
                    m.write_u8(o + kEngagedByte, 0U);
                }
            }
        }

        retired += kStepRangeTest;
        const bool step_in_range = static_cast<std::int32_t>(m.read_u32_le(o + kStep)) < 2;
        if (step_in_range) retired += kStepPendingTest;
        if (step_in_range && m.read_u32_le(o + kEventFlag) == 0U) {
            retired += kStepDot;
            const std::uint32_t orientation = m.read_u32_le(o + kOrientationPointer);
            const std::uint32_t step_reading =
                vu_dot3(cpu, n3::load_quad_any(m, o + kMotionQuad),
                        n3::load_quad_any(m, orientation + 32U));
            const std::uint32_t step = m.read_u32_le(o + kStep);
            bool step_settled = false;

            // The listing lays the three step handlers out one after another and
            // each one tests its own step value first, so a step other than -1 or
            // 1 runs the earlier handlers' tests on the way through.
            if (step != 0xffffffffU) {
                retired += kBackStepHandler;
                bool act = !fpu::le(m.read_u32_le(o + kPitch), m.read_u32_le(o + kYaw));
                if (act) {
                    retired += kBackStepSmall;
                    act = !fpu::le(m.read_u32_le(o + kPitch), gp_float(kGpSmallAngle));
                }
                if (act) {
                    retired += kBackStepDot;
                    act = fpu::lt(step_reading, kHalf);
                }
                if (act) {
                    retired += kBackStepEngaged;
                    act = m.read_u8(o + kEngagedByte) == 0U;
                }
                if (act) {
                    retired += kBackStepStore;
                    m.write_u32_le(o + kStep, 0xffffffffU);
                    m.write_u32_le(o + kEventFlag, 1U);
                    m.write_u32_le(o + kEventStamp, gp_float(kGpEventStamp));
                    step_settled = true;
                }
            }
            if (!step_settled) {
                retired += kCheckBackStep;
                if (step != 1U) {
                    retired += kForwardStepHandler;
                    bool act = !fpu::le(m.read_u32_le(o + kYaw), m.read_u32_le(o + kPitch));
                    if (act) {
                        retired += kForwardStepSmall;
                        act = !fpu::le(m.read_u32_le(o + kYaw), gp_float(kGpSmallAngle));
                    }
                    if (act) {
                        retired += kForwardStepDot;
                        act = !fpu::le(step_reading, kMinusTenAndHalf);
                    }
                    if (act) {
                        retired += kForwardStepStore;
                        m.write_u32_le(o + kStep, 1U);
                        m.write_u32_le(o + kEventFlag, 1U);
                        m.write_u32_le(o + kEventStamp, gp_float(kGpEventStamp));
                        step_settled = true;
                    }
                }
                if (!step_settled) {
                    retired += kCheckIdleStep;
                    if (step == 0U) {
                        // Step 0: the travel quad's w arms the yaw if it is large
                        // enough for this frame.
                        retired += kIdleStepMotion;
                        if (!fpu::le(m.read_u32_le(o + kMotionQuad + 12U), gp_float(kGpSmallAngle))) {
                            retired += kIdleStepRaise;
                            m.write_u32_le(o + kPitch, kOne);
                        }
                    }
                }
            }
        }

        // The shift arming flags are checked even when the step is out of range
        // or an earlier step change is still pending.
        retired += kShiftFlagsTest;
        if (m.read_u32_le(o + kShiftArmed) != 0U) {
            retired += kShiftUpFlags;
            if ((m.read_u8(o + kFlagBytes) & 1U) == 0U) {
                retired += kShiftUpLatchClear;
                m.write_u32_le(o + kShiftUpLatch, 0U);
            } else {
                retired += kShiftUpLatchTest;
                if (m.read_u32_le(o + kShiftUpLatch) == 0U) {
                    retired += kShiftUpRange;
                    const std::int32_t up_step = static_cast<std::int32_t>(m.read_u32_le(o + kStep));
                    if (up_step >= 0) {
                        retired += kShiftUpLimit;
                        if (up_step < static_cast<std::int32_t>(m.read_u32_le(o + kStepLimit))) {
                            retired += kShiftUpStore;
                            m.write_u32_le(o + kStep, static_cast<std::uint32_t>(up_step + 1));
                            m.write_u32_le(o + kEventFlag, 1U);
                            m.write_u32_le(o + kEventStamp, gp_float(kGpEventStamp));
                        }
                    }
                    retired += kShiftUpLatchSet;
                    m.write_u32_le(o + kShiftUpLatch, 1U);
                }
            }
            retired += kShiftDownFlags;
            if ((m.read_u8(o + kFlagBytes) & 2U) == 0U) {
                retired += kShiftDownLatchClear;
                m.write_u32_le(o + kShiftDownLatch, 0U);
            } else {
                retired += kShiftDownLatchTest;
                if (m.read_u32_le(o + kShiftDownLatch) == 0U) {
                    retired += kShiftDownLimit;
                    const std::uint32_t down_step = m.read_u32_le(o + kStep);
                    if (!(static_cast<std::int32_t>(down_step) < 2)) {
                        retired += kShiftDownStore;
                        m.write_u32_le(o + kStep, down_step - 1U);
                        m.write_u32_le(o + kEventFlag, 1U);
                        m.write_u32_le(o + kEventStamp, gp_float(kGpEventStamp));
                    }
                    retired += kShiftDownLatchSet;
                    m.write_u32_le(o + kShiftDownLatch, 1U);
                }
            }
        }
    }

    // ---- the steering value ------------------------------------------------
    retired += kStepJoin; // 0x11c8: 3
    if (static_cast<std::int32_t>(m.read_u32_le(o + kStep)) < 0) {
        // A negative step swaps the two input angles for this frame.
        retired += kSwapAngles;
        const std::uint32_t pitch = m.read_u32_le(o + kPitch);
        m.write_u32_le(o + kPitch, m.read_u32_le(o + kYaw));
        m.write_u32_le(o + kYaw, pitch);
    }

    // The distance term: the travel distance folded with the block's gain and
    // clamped into the window the probes set.
    retired += kDistanceTerm; // 0x11d4: 11
    std::uint32_t distance = fpu::msub(fpu::add(kZero, m.read_u32_le(o + kDistanceGain)),
                                       gp_float(kGpDistanceGain),
                                       fpu::sub(travel, m.read_u32_le(o + kDistanceBaseline)));
    if (fpu::lt(distance, m.read_u32_le(o + kDistanceLow))) {
        retired += kDistanceFloor;
        distance = m.read_u32_le(o + kDistanceLow);
    }
    retired += kDistanceHighTest; // 0x1204: 4
    if (!fpu::le(distance, m.read_u32_le(o + kDistanceHigh))) {
        retired += kDistanceHighSet;
        distance = m.read_u32_le(o + kDistanceHigh);
    }

    // The steering value stays inside the band around last frame's value.
    retired += kSteerBandTest; // 0x1a4c: 5
    const std::uint32_t steer_centre = m.read_u32_le(o + kLastSteer);
    if (fpu::le(m.read_u32_le(o + kSteer), steer_centre)) {
        retired += kSteerUnderBand; // 0x1238: 6
        const std::uint32_t band = m.read_u32_le(o + kSteerBand);
        if (fpu::lt(fpu::sub(m.read_u32_le(o + kSteer), steer_centre), fpu::neg(band))) {
            retired += kSteerClampLow;
            m.write_u32_le(o + kSteer, fpu::sub(steer_centre, band));
        }
    } else {
        retired += kSteerBandPicked + kSteerOverBand; // 0x1a60: 2, 0x121c: 4
        const std::uint32_t band = m.read_u32_le(o + kSteerBand);
        if (!fpu::le(fpu::sub(m.read_u32_le(o + kSteer), steer_centre), band)) {
            retired += kSteerClampHigh;
            m.write_u32_le(o + kSteer, fpu::add(steer_centre, band));
        }
    }

    // Both probes reported: the steering value is pinned to full lock.
    retired += kProbeResultTest; // 0x1258: 2
    if (probe_far != 0U) retired += kSecondProbeTest;
    if (probe_far != 0U && probe_near != 0U) {
        retired += kSteerSignByte;   // 0x1268: 3
        if (m.read_u8(o + kSignByte) != 0U) {
            retired += kSteerSignSet;
            m.write_u32_le(o + kSteer, kOne);
        } else {
            retired += kSteerSignClear;
            m.write_u32_le(o + kSteer, kMinusOne);
        }
    }

    // The steering output, then this frame's probe reading.
    retired += kSteerOutputBlock; // 0x1288: 9
    m.write_u32_le(o + kSteerOutput, fpu::mul(fpu::neg(distance), m.read_u32_le(o + kSteer)));

    // Both halves of the 0x1288 fork make the same call with the same arguments
    // (the compiler only parked the intermediate in another register), so the
    // count and the effect are the same either way.
    retired += kSteerCall; // 13
    {
        const GuestAddress block = m.read_u32_le(o + kControlPointer);
        // The rate scale the ramp takes in f13 is the larger of the two input
        // angles; the listing duplicates the whole call block to pick it.
        const std::uint32_t rate_low = m.read_u32_le(o + kRateAngleLow);
        const std::uint32_t rate_high = m.read_u32_le(o + kRateAngleHigh);
        call.set_arg(0U, o + kDriveStateRecord);
        call.set_arg(1U, m.read_u8(o + kFlagBytes) & 4U);
        call.set_arg(2U, m.read_u32_le(block + kProbeGate) == 0U ? 1U : 0U);
        call.set_float_arg(0U, fpu::sub(m.read_u32_le(o + kYaw), m.read_u32_le(o + kPitch)));
        call.set_float_arg(1U, fpu::le(rate_low, rate_high) ? rate_high : rate_low);
        const n3::Run run = call.invoke([](auto& machine) { return n3::fn_001c6f90(machine); }, kFrameBytes);
        if (run.declined) return call.decline();
        retired += run.retired;
        m.write_u32_le(o + kProbeResult, call.float_result());
    }

    retired += kReverseFlags; // 0x1314: 4
    if ((m.read_u8(o + kFlagBytes) & 4U) != 0U) {
        retired += kReverseSet; // 0x1324: 3
        m.write_u8(o + kReverseByte, 1U);
    } else {
        retired += kReverseClear; // 0x1330: 1
        m.write_u8(o + kReverseByte, 0U);
    }

    // A negative probe reading at the last step brakes the car when it is also
    // slow: the reading is halved.
    retired += kReverseByteTest; // 0x1334: 3
    if (m.read_u8(o + kReverseByte) != 0U) {
        retired += kProbeSignTest; // 0x1340: 6
        if (fpu::lt(m.read_u32_le(o + kProbeResult), kZero)) {
            retired += kStepMatchTest; // 0x1358: 6
            if (m.read_u32_le(o + kStep) == m.read_u32_le(o + kStepLimit)) {
                retired += kSpeedSignTest; // 0x1370: 6
                if (fpu::lt(travel, fpu::mul(gp_float(kGpSpeedThreshold),
                                             m.read_u32_le(o + kSpeedLimitScale)))) {
                    retired += kProbeHalve; // 0x1388: 5
                    m.write_u32_le(o + kProbeResult,
                                   fpu::mul(m.read_u32_le(o + kProbeResult), kMinusHalf));
                }
            }
        }
    }

    // Above the speed limit the reading is dropped entirely.
    retired += kSpeedTest; // 0x139c: 6
    if (!fpu::le(travel, fpu::mul(gp_float(kGpSpeedThreshold),
                                  m.read_u32_le(o + kSpeedLimitScale)))) {
        retired += kSpeedZero; // 0x1a68: 2
        m.write_u32_le(o + kProbeResult, 0U);
    }

    // The 130-degree curve the pitch is compared against.
    retired += kCurveValue; // 0x13b4: 22
    m.write_u32_le(o + kResetRequest, 0U);
    const std::uint32_t curve_shape = fpu::add(kZero, kOne);
    const std::uint32_t curve_span = fpu::sub(kOneThirty, m.read_u32_le(o + kCurveCut));
    const std::uint32_t curve_length = fpu::sub(m.read_u32_le(o + kCurveFull), kOne);
    const std::uint32_t curve_ratio = fpu::div(curve_length, curve_span);
    const std::uint32_t curve_base =
        fpu::msub(curve_shape, m.read_u32_le(o + kCurveCut), curve_ratio);
    const std::uint32_t curve =
        max_s(gp_float(kGpSmallAngle), fpu::madd(curve_base, curve_ratio, travel_scaled));

    // The cornering gate: the pitch must outrun the curve, or the two angles must
    // be inside the floor.
    if (m.read_u32_le(m.read_u32_le(o + kControlPointer) + kDrivePhase) != kCorneringPhase) {
        retired += kCurveGate; // 0x140c: 4
        if (!fpu::le(m.read_u32_le(o + kPitch), curve)) {
            retired += kCorneringEnter;
            cornering = true;
        } else {
            retired += kCurveGateBias; // 0x141c: 5
            if (!fpu::le(m.read_u32_le(o + kYaw), gp_float(kGpAngleFloor))) {
                retired += kCurveGatePitch; // 0x1430: 3
                if (!fpu::le(m.read_u32_le(o + kPitch), gp_float(kGpAngleFloor))) {
                    retired += kCorneringEnter;
                    cornering = true;
                }
            }
        }
    }

    retired += kPhaseTest; // 0x1a70: 5
    const std::uint32_t phase = m.read_u32_le(o + kCorneringFlag);
    if (phase == 1U || phase == 2U) {
        retired += kPhasePrepare; // 0x1a84: 2
        retired += kCorneringBlend; // 0x1444: 38
        const std::uint32_t orientation = m.read_u32_le(o + kOrientationPointer);
        const std::uint32_t turn = vu_dot3(cpu, n3::load_quad_any(m, o + kAxisQuad),
                                           n3::load_quad_any(m, orientation));
        std::uint32_t smoothed =
            fpu::sub(kOne, fpu::div(fpu::abs(turn), m.read_u32_le(o + kDotNormaliser)));
        m.write_u32_le(o + kSmoothedAngle, smoothed);
        smoothed = fpu::mul(smoothed, fpu::sub(m.read_u32_le(o + kCurveHigh),
                                               m.read_u32_le(o + kCurveLow)));
        m.write_u32_le(o + kSmoothedAngle, smoothed);
        smoothed = fpu::add(smoothed, m.read_u32_le(o + kCurveLow));
        m.write_u32_le(o + kSmoothedAngle, smoothed);
        smoothed = fpu::add(smoothed, m.read_u32_le(o + kPitch));
        m.write_u32_le(o + kSmoothedAngle, smoothed);
        if (!fpu::le(m.read_u32_le(o + kSteerReference), kHalf)) {
            retired += kCorneringShift; // 0x1a8c: 5
            smoothed = fpu::sub(smoothed, fpu::sub(m.read_u32_le(o + kSteerReference), kHalf));
            m.write_u32_le(o + kSmoothedAngle, smoothed);
        }
        retired += kSmoothedLowTest; // 0x14dc: 5
        if (fpu::lt(smoothed, m.read_u32_le(o + kCurveLow))) {
            retired += kSmoothedLowSet; // 0x1aa0: 2
            smoothed = m.read_u32_le(o + kCurveLow);
            m.write_u32_le(o + kSmoothedAngle, smoothed);
        }
        retired += kSmoothedHighTest; // 0x14f0: 5
        if (!fpu::le(smoothed, m.read_u32_le(o + kCurveHigh))) {
            retired += kSmoothedHighSet; // 0x1aa8: 2
            smoothed = m.read_u32_le(o + kCurveHigh);
            m.write_u32_le(o + kSmoothedAngle, smoothed);
        }
        // Smooth towards the held angle by this frame's blend factor.
        retired += kSmoothedBlendCurve; // 0x1504: 10
        const std::uint32_t held = m.read_u32_le(o + kHeldAngle);
        m.write_u32_le(o + kSmoothedAngle,
                       fpu::madd(fpu::add(kZero, held), fpu::sub(smoothed, held),
                                 m.read_u32_le(kFrameBlend)));
    } else {
        retired += kSmoothedBlendBand; // 0x152c: 9
        const std::uint32_t held = m.read_u32_le(o + kHeldAngle);
        m.write_u32_le(o + kSmoothedAngle,
                       fpu::madd(fpu::add(kZero, held),
                                 fpu::sub(m.read_u32_le(o + kCurveHigh), held),
                                 m.read_u32_le(kFrameBlend)));
    }

    // The over-speed timer runs while the cornering phase holds, otherwise only
    // while the steering value is out of the dead band and the car is slow.
    retired += kCorneringStateTest; // 0x1550: 3
    bool angle_bias_done = false;
    if (m.read_u32_le(o + kCorneringFlag) != 0U) {
        // A cornering phase restarts the hold timer and skips the whole angle
        // bias and hold-search region below (0x1ad0 jumps straight to 0x1678).
        retired += kHoldTimerReset; // 0x1ad0: 7
        m.write_u32_le(o + kOverTimer, 0U);
        m.write_u32_le(o + kHoldTimer,
                       fpu::add(m.read_u32_le(o + kHoldTimer), m.read_u32_le(kFrameBlend)));
        angle_bias_done = true;
    } else {
        retired += kSteerDeadBand; // 0x155c: 6
        if (fpu::le(fpu::abs(m.read_u32_le(o + kSteer)), gp_float(kGpSteerSmall))) {
            retired += kOverTimerClear; // 0x1ab0: 2
            m.write_u32_le(o + kOverTimer, 0U);
        } else {
            retired += kOverSpeedTest; // 0x1574: 9
            if (fpu::le(m.read_u32_le(o + kYaw), fpu::div(kThirty, travel))) {
                retired += kOverTimerClear;
                m.write_u32_le(o + kOverTimer, 0U);
            } else {
                retired += kOverTimerAdd; // 0x1598: 5
                m.write_u32_le(o + kOverTimer, fpu::add(m.read_u32_le(o + kOverTimer),
                                                        m.read_u32_le(kFrameBlend)));
            }
        }
    }

    if (!angle_bias_done) {
        // The angle bias the hold decision compares with the timer.
        retired += kAngleBiasBlock; // 0x15ac: 18
        std::uint32_t bias = fpu::add(
            m.read_u32_le(o + kAngleBias),
            fpu::sub(fpu::sub(kOneAndHalf, m.read_u32_le(o + kYaw)),
                     fpu::div(fpu::sub(travel, kFortyThree), kTwentyFive)));
        if (fpu::lt(bias, gp_float(kGpAngleFloor))) {
            retired += kAngleBiasFloor;
            bias = gp_float(kGpAngleFloor);
        }

        retired += kOverFast; // 0x15f8: 6
        std::uint32_t hold = 1U;
        if (fpu::le(travel, kFortyThree)) {
            retired += kOverFastLow; // 0x1610: 1
            hold = 0U;
        }
        retired += kOverTimerTest; // 0x1614: 2
        if (hold != 0U) {
            retired += kOverTimerCompare; // 0x161c: 4
            if (fpu::le(m.read_u32_le(o + kOverTimer), bias)) {
                retired += kOverTimerLow; // 0x162c: 1
                hold = 0U;
            }
        }

        retired += kHoldDecision; // 0x1630: 2
        if (!cornering) retired += kHoldSkip;
        if (cornering || (hold & 0xffU) != 0U) {
            retired += kSteerSignTest; // 0x1640: 5
            if (!fpu::le(m.read_u32_le(o + kSteer), gp_float(kGpSmallAngle))) {
                retired += kHoldTimerSet; // 0x1ab8: 6
                m.write_u32_le(o + kCorneringFlag, 2U);
                m.write_u32_le(o + kHoldTimer, 0U);
                m.write_u32_le(o + kHeldAngle, m.read_u32_le(o + kCurveHigh));
            } else {
                retired += kSteerKillTest; // 0x1654: 4
                if (fpu::lt(m.read_u32_le(o + kSteer), gp_float(kGpSteerKill))) {
                    retired += kHoldTimerStart; // 0x1664: 5
                    m.write_u32_le(o + kCorneringFlag, 1U);
                    m.write_u32_le(o + kHoldTimer, 0U);
                    m.write_u32_le(o + kHeldAngle, m.read_u32_le(o + kCurveHigh));
                }
            }
        }
    }

    retired += kLastYawTest; // 0x1678: 3
    if (m.read_u32_le(o + kStep) != 0U) {
        retired += kLastYawStore; // 0x168c: 2
        m.write_u32_le(o + kLastYaw, m.read_u32_le(o + kYaw));
    } else {
        retired += kLastYawClear; // 0x1684: 2
        m.write_u32_le(o + kLastYaw, 0U);
    }

    retired += kSaveAngles; // 0x1694: 7
    m.write_u32_le(o + kLastPitch, m.read_u32_le(o + kPitch));
    m.write_u32_le(o + kLastSteer, m.read_u32_le(o + kSteer));
    m.write_u32_le(o + kHeldAngle, m.read_u32_le(o + kSmoothedAngle));

    retired += kEpilogue;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001c0f00, 3104U, 0x22fbfafa5bd13a07ULL, "UpdateVehicleControls", 994U, update_vehicle_controls,
         EngineReturn::Nothing)
