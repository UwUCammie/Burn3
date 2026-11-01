// 0x00157410 (a0 = player controller object A, a1 = pointer R, may be 0): UpdateCarProximityRecords. A leaf that
// returns nothing. It clears A's word +1292 and bytes +1286 and +1287 and sets vf2.x to 1.0. Then the car list at
// 0x0064d388 (its count is the signed word at 0x0064d37c, read at every test) is scanned: a car that passes the float
// and flag tests, and lies within 25600.0 of A's own record (the word at A+200) by the VU0 squared distance of the quads
// at +48, gets a record for the player slot idx (the signed byte at own+6572) in its own arrays: the float word at
// +5528+4*idx, and bytes at +5552+idx and +5558+idx. Part B does the same for the car at A+1516 (and stores own at its
// +5576), and Part C does it for the object at R+12 (byte 1 at +5552+idx). The VU0 registers are the real ones: the
// sequence runs on them, so vf1, vf2 and the accumulator end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <type_traits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kMaskX = 8U;                  // the x lane of a VU destination mask
constexpr std::uint32_t kLimitBits = 0x46c80000U;    // 25600.0f (lui $t0, 0x46c8)
constexpr std::uint32_t kMinusOneBits = 0xbf800000U; // -1.0f (lui $v1, 0xbf80)
constexpr GuestAddress kCarCount = 0x0064d37cU;      // signed car count (lw -11396 from $a3 = 0x650000)
constexpr GuestAddress kCarList = 0x0064d388U;       // car pointers: pass i reads the word at kCarList + 4 i
constexpr GuestAddress kRecordLimit = 0x00645170U;   // signed limit for the Part C table (lw 20848 from 0x640000)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 12U;       // 0x157410..0x15743c: the prologue and the jump to the loop test
constexpr std::uint32_t kLoopTestCost = 4U;     // 0x1574fc..0x157508: lw, slt, bne and the nop
constexpr std::uint32_t kPassCost = 10U;        // 0x157440..0x157464: the car pointer and the float test
constexpr std::uint32_t kFlagTestCost = 3U;     // 0x157468..0x157470: the byte at +1464
constexpr std::uint32_t kOwnTestCost = 5U;      // 0x157474..0x157484: the car's word +5748 against the byte at own+6572
constexpr std::uint32_t kDistanceCost = 13U;    // 0x157488..0x1574b8: the squared distance and its compare
constexpr std::uint32_t kLiveTestCost = 3U;     // 0x1574bc..0x1574c4: the car's word +4924
constexpr std::uint32_t kQueueTestCost = 4U;    // 0x1574c8..0x1574d4: the byte at (the car's word +4488) + 6378
constexpr std::uint32_t kRecordCost = 8U;       // 0x1574d8..0x1574f4: the record
constexpr std::uint32_t kStepCost = 1U;         // 0x1574f8: i += 1

constexpr std::uint32_t kPartBHeadCost = 7U;       // 0x15750c..0x157524: the float at +1520 and the -1.0 test
constexpr std::uint32_t kPartBNegOneCost = 2U;     // 0x15768c..0x157690: t0 = 0 when the float is -1.0
constexpr std::uint32_t kPartBSumCost = 5U;        // 0x157528..0x157538: the sum against the float at +12
constexpr std::uint32_t kPartBSumTakenCost = 2U;   // 0x157694..0x157698: t0 = 0 when the sum is below it
constexpr std::uint32_t kPartBLoadCost = 1U;       // 0x15753c: t0 = the word at +1516
constexpr std::uint32_t kPartBTestCost = 2U;       // 0x157540..0x157544: t0 == 0 goes to Part C
constexpr std::uint32_t kPartBDistanceCost = 18U;  // 0x157548..0x15758c: the squared distance and its compare
constexpr std::uint32_t kPartBStoreCost = 5U;      // 0x157590..0x1575a0: own stored at +5576, the state test
constexpr std::uint32_t kPartBQueueCost = 4U;      // 0x1575a4..0x1575b0: the flag test
constexpr std::uint32_t kPartBRecordCost = 8U;     // 0x1575b4..0x1575d0: the record

constexpr std::uint32_t kPartCZeroCost = 2U;       // 0x1575d4..0x1575d8: R == 0
constexpr std::uint32_t kPartCObjectCost = 3U;     // 0x1575dc..0x1575e4: R+12 == 0
constexpr std::uint32_t kPartCFlagCost = 3U;       // 0x1575e8..0x1575f0: the byte at +6378
constexpr std::uint32_t kPartCDoneCost = 3U;       // 0x1575f4..0x1575fc: the word at +6420
constexpr std::uint32_t kPartCCountCost = 6U;      // 0x157600..0x157614: the count and its signed compare
constexpr std::uint32_t kPartCBaseCost = 9U;       // 0x157618..0x157638: the table base when the count is below
constexpr std::uint32_t kPartCZeroBaseCost = 1U;   // 0x15763c: the base 0
constexpr std::uint32_t kPartCTableCost = 3U;      // 0x157640..0x157648: the byte at base + 17797
constexpr std::uint32_t kPartCStateCost = 5U;      // 0x15764c..0x15765c: own, the state test
constexpr std::uint32_t kPartCRecordCost = 11U;    // 0x157660..0x157688: the record
constexpr std::uint32_t kReturnCost = 2U;          // 0x15769c..0x1576a0: jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The inputs the original reads on entry: a0, a1 (its doubleword is tested against zero) and $gp.
struct Inputs {
    GuestAddress object{};      // a0: A
    GuestAddress record{};      // a1: R (low word)
    bool record_is_zero{};      // a1 == 0 as a doubleword
    GuestAddress gp{};          // $gp
};

// A DryMemory for call_cost that records any access the original could not make (outside guest memory, or a word
// or quadword address the interpreter rejects), so the call is left to the original, which faults the same way.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(address, value);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    // lqc2 needs a 16-byte aligned address inside guest memory.
    void check_quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// lb: the byte at the address, sign-extended to 32 bits. The compares and addresses below use only the low 32 bits.
template <class Memory>
std::uint32_t signed_byte(const Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(address))));
}

// lqc2: the dry run records a misaligned or unmapped quadword load, see CheckedDryMemory.
template <class Memory>
Gpr128 vu0_quad(Memory& m, const GuestAddress address) {
    if constexpr (requires(const Memory& x) { x.check_quad(0U); }) {
        m.check_quad(address);
    }
    return n3::load_quad_any(m, address);
}

// One pass of the car loop (0x157440..0x1574f8) for car i. Returns the retired count of the pass, its increment
// included.
template <class Memory>
std::uint32_t car_pass(Memory& m, guest::R5900State& vu, const Inputs& in, const std::uint32_t i) {
    const GuestAddress object = in.object;
    const std::uint32_t slot = 4U * i;
    std::uint32_t retired = kPassCost;
    const GuestAddress car = m.read_u32_le(kCarList + slot);
    const std::uint32_t near_float = m.read_u32_le(object + 1320U + slot);
    const std::uint32_t limit = m.read_u32_le(in.gp - 29080U);
    if (fpu::le(near_float, limit)) {
        retired += kFlagTestCost;
        if (m.read_u8(object + 1464U) == 0U) {
            return retired + kStepCost;
        }
        retired += kOwnTestCost;
        const GuestAddress own_flag_record = m.read_u32_le(object + 200U);
        if (m.read_u32_le(car + 5748U) != signed_byte(m, own_flag_record + 6572U)) {
            return retired + kStepCost;
        }
    }
    retired += kDistanceCost;
    const GuestAddress own = m.read_u32_le(object + 200U);
    vu.write_vu0_vector(3U, vu0_quad(m, car + 48U));                              // lqc2 vf3, 48(t4)
    vu.write_vu0_vector(1U, vu0_quad(m, own + 48U));                              // lqc2 vf1, 48(t1)
    ee_cop2::vadd_vector(vu, 1U, 3U, 1U, n3::kMaskXyz, true);                     // vsub.xyz vf1, vf3, vf1
    ee_cop2::vmul(vu, 1U, 1U, 1U, n3::kMaskXyz);                                  // vmul.xyz vf1, vf1, vf1
    ee_cop2::vadda_broadcast(vu, 1U, 1U, kMaskX, 1U);                             // vadday.x acc, vf1, vf1y
    ee_cop2::vmadd_broadcast(vu, 1U, 2U, 1U, kMaskX, 2U, false, false, false);    // vmaddz.x vf1, vf2, vf1z
    const std::uint32_t squared = vu.read_vu0_vector(1U).words[0];                // qmfc2 t0, vf1 ; mtc1 t0, f0
    if (!fpu::lt(squared, kLimitBits)) {
        return retired + kStepCost;
    }
    retired += kLiveTestCost;
    const std::uint32_t state = m.read_u32_le(car + 4924U);
    const std::uint8_t flag = m.read_u8(object + 1464U);                          // lbu t3 (delay slot: always)
    if (state == 3U) {
        return retired + kStepCost;
    }
    retired += kQueueTestCost;
    const std::uint32_t queue = m.read_u32_le(car + 4488U);
    if (m.read_u8(queue + 6378U) != 0U) {
        return retired + kStepCost;
    }
    retired += kRecordCost;
    const std::uint32_t idx = signed_byte(m, own + 6572U);
    const std::uint32_t value = m.read_u32_le(car + 4300U);
    m.write_u32_le(car + 5528U + 4U * idx, value);
    m.write_u8(car + 5552U + idx, 0U);
    m.write_u8(car + 5558U + idx, flag);
    return retired + kStepCost;
}

// Part B (0x15750c..0x1575d0): the car whose word is at A+1516. Returns the retired count of the part, its record
// included. Part C runs after it in every case.
template <class Memory>
std::uint32_t object_record(Memory& m, guest::R5900State& vu, const Inputs& in) {
    const GuestAddress object = in.object;
    std::uint32_t retired = kPartBHeadCost;
    const std::uint32_t f1 = m.read_u32_le(object + 1520U);
    const std::uint32_t f2 = m.read_u32_le(in.gp - 29088U);
    GuestAddress target = 0U;
    if (fpu::eq(kMinusOneBits, f1)) {
        retired += kPartBNegOneCost;
    } else {
        retired += kPartBSumCost;
        const std::uint32_t bound = m.read_u32_le(object + 12U);
        if (fpu::lt(fpu::add(f1, f2), bound)) {
            retired += kPartBSumTakenCost;
        } else {
            retired += kPartBLoadCost;
            target = m.read_u32_le(object + 1516U);
        }
    }
    retired += kPartBTestCost;
    if (target == 0U) {
        return retired;
    }
    retired += kPartBDistanceCost;
    const GuestAddress own = m.read_u32_le(object + 200U);
    ee_cop2::vadd_broadcast(vu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1, vf0, vf0w
    const Gpr128 saved = vu.read_vu0_vector(1U);                                 // qmfc2 v1, vf1
    vu.write_vu0_vector(1U, vu0_quad(m, own + 48U));                             // lqc2 vf1, 48(a2)
    vu.write_vu0_vector(2U, vu0_quad(m, target + 48U));                          // lqc2 vf2, 48(t0)
    ee_cop2::vadd_vector(vu, 1U, 2U, 1U, n3::kMaskXyz, true);                    // vsub.xyz vf1, vf2, vf1
    ee_cop2::vmul(vu, 2U, 1U, 1U, n3::kMaskXyz);                                 // vmul.xyz vf2, vf1, vf1
    ee_cop2::vadda_broadcast(vu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc, vf2, vf2y
    vu.write_vu0_vector(1U, saved);                                              // qmtc2 v1, vf1
    ee_cop2::vmadd_broadcast(vu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1, vf1, vf2z
    const std::uint32_t squared = vu.read_vu0_vector(1U).words[0];               // qmfc2 v1, vf1 ; mtc1 v1, f1
    if (!fpu::lt(squared, kLimitBits)) {
        return retired;
    }
    retired += kPartBStoreCost;
    m.write_u32_le(target + 5576U, own);                                         // sw a2, 5576(t0)
    const std::uint32_t state = m.read_u32_le(target + 4924U);                   // lw a2, 4924(t0)
    const GuestAddress own_again = m.read_u32_le(object + 200U);                 // lw a3, 200(a0) (delay slot)
    if (state == 3U) {
        return retired;
    }
    retired += kPartBQueueCost;
    const std::uint32_t queue = m.read_u32_le(target + 4488U);                   // lw v1, 4488(t0)
    if (m.read_u8(queue + 6378U) != 0U) {
        return retired;
    }
    retired += kPartBRecordCost;
    const std::uint32_t idx = signed_byte(m, own_again + 6572U);                 // lb v1, 6572(a3)
    const std::uint32_t value = m.read_u32_le(target + 4300U);                   // lwc1 f0, 4300(t0)
    m.write_u32_le(target + 5528U + 4U * idx, value);                            // swc1 f0, 5528(a2)
    m.write_u8(target + 5552U + idx, 0U);                                        // sb $zero, 5552(v1)
    m.write_u8(target + 5558U + idx, 0U);                                        // sb $zero, 5558(v1)
    return retired;
}

// Part C (0x1575d4..0x1576a0): the object at R+12, when R is not zero and the object qualifies. Returns the retired
// count of the part, the return included.
template <class Memory>
std::uint32_t record_object(Memory& m, const Inputs& in) {
    std::uint32_t retired = kPartCZeroCost;
    if (in.record_is_zero) {
        return retired + kReturnCost;
    }
    retired += kPartCObjectCost;
    const GuestAddress holder = m.read_u32_le(in.record + 12U);
    if (holder == 0U) {
        return retired + kReturnCost;
    }
    retired += kPartCFlagCost;
    if (m.read_u8(holder + 6378U) == 0U) {
        return retired + kReturnCost;
    }
    retired += kPartCDoneCost;
    if (m.read_u32_le(holder + 6420U) != 0U) {
        return retired + kReturnCost;
    }
    retired += kPartCCountCost;
    const std::uint32_t count = m.read_u32_le(holder + 10176U);
    const std::uint32_t limit = m.read_u32_le(kRecordLimit);
    GuestAddress base = 0U;
    if (static_cast<std::int32_t>(count) < static_cast<std::int32_t>(limit)) {
        retired += kPartCBaseCost;
        base = 0x0055ef70U + 0x0001cb80U + count * 17808U;
    } else {
        retired += kPartCZeroBaseCost;
    }
    retired += kPartCTableCost;
    if (m.read_u8(base + 17797U) == 0U) {
        return retired + kReturnCost;
    }
    retired += kPartCStateCost;
    const GuestAddress own = m.read_u32_le(in.object + 200U);
    if (m.read_u32_le(holder + 4924U) == 3U) {
        return retired + kReturnCost;
    }
    retired += kPartCRecordCost;
    const std::uint32_t idx = signed_byte(m, own + 6572U);
    const std::uint32_t value = m.read_u32_le(holder + 4300U);
    m.write_u32_le(holder + 5528U + 4U * idx, value);
    m.write_u8(holder + 5552U + idx, 1U);
    m.write_u8(holder + 5558U + idx, 0U);
    return retired + kReturnCost;
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). `vu` is the VU0 state
// the original sees. Returns the retired count of the path taken.
template <class Memory>
std::uint32_t update_body(Memory& m, guest::R5900State& vu, const Inputs& in) {
    std::uint32_t retired = kEntryCost;
    ee_cop2::vadd_broadcast(vu, 2U, 0U, 0U, kMaskX, 3U, false);                   // vaddw.x vf2, vf0, vf0w
    m.write_u32_le(in.object + 1292U, 0U);                                        // sw $zero, 1292($a0)
    m.write_u8(in.object + 1286U, 0U);                                            // sb $zero, 1286($a0)
    m.write_u8(in.object + 1287U, 0U);                                            // sb $zero, 1287($a0)
    for (std::uint32_t i = 0U;; ++i) {
        retired += kLoopTestCost;
        if (static_cast<std::int32_t>(i) >= static_cast<std::int32_t>(m.read_u32_le(kCarCount))) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += car_pass(m, vu, in, i);
    }
    retired += object_record(m, vu, in);
    retired += record_object(m, in);
    return retired;
}

Inputs read_inputs(const guest::R5900State& cpu) {
    const Gpr128 a1 = cpu.read_gpr(5U);
    return Inputs{
        .object = cpu.read_gpr_u32(4U),
        .record = cpu.read_gpr_u32(5U),
        .record_is_zero = a1.words[0] == 0U && a1.words[1] == 0U,
        .gp = cpu.read_gpr_u32(n3::kGlobalPointer),
    };
}

template <class Context>
std::uint32_t update_car_proximity_records_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    guest::R5900State vu{context.cpu};
    const std::uint32_t retired = update_body(dry, vu, read_inputs(context.cpu));
    if (dry.faulted() || retired > kCeiling) {
        return 0U;
    }
    return retired;
}

template <class Context>
n3::Run update_car_proximity_records(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    return call.finish(update_body(call.memory(), cpu, read_inputs(cpu)));
}

} // namespace

extern constinit const EngineFunction n3_00157410{0x00157410U, 660U, 0x06008a157bc232d4ULL,
                                                  "UpdateCarProximityRecords", kCeiling,
                                                  n3::live<&update_car_proximity_records<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &update_car_proximity_records_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00157410, update_car_proximity_records, update_car_proximity_records_cost)
