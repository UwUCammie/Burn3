// 0x0019d4d0 (a0 = object P): UpdateTargetCandidate. Clears the word at P+1936 first. Then, when the flag word at
// 0x0064d3a0 and the byte at P+401 are both set, R = ReadField72(P+368) is the candidate, and a null R returns.
// R's signed byte at +6572 is a class index a3 into 48-byte records at 0x0055efec, whose pointer leads to a quadword
// at +176 (word 3 is w). Unless R's byte at +4574 is set while the manager's word at +9280 is zero (the manager is the
// pointer at P+1888), the candidate is accepted when the manager's word at +5696 equals a3, or the manager's signed
// byte at +6572 equals R's word at +5696, or the manager's word at +9280 is set, or when X * w is not below 100.0
// (X = the float at gp-30980). An accepted candidate is stored at P+1940, and the word at P+1936 becomes 5 when the
// byte at P+402 is set, otherwise 4. The quadword at +176 is spilled to the frame at sp+32 and its word 3 is read back
// from sp+44, as the original does.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0032f350)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFlagWord = 0x0064d3a0U;       // lui 0x65 + lw -11360
constexpr GuestAddress kRecordTable = 0x0055efecU;    // lui 0x56 + addiu -4116
constexpr std::uint32_t kRecordStride = 48U;          // (3 * a3) << 4
constexpr GuestAddress kScaleBelowGp = 30980U;        // lwc1 f1,-30980(gp)
constexpr std::uint32_t kLimitBits = 0x42c80000U;     // 100.0f, lui 0x42c8 into f0
constexpr GuestAddress kFrameBytes = 64U;             // addiu sp,sp,-64

constexpr GuestAddress kGateByte = 401U;              // P+401
constexpr GuestAddress kCandidateOffset = 368U;       // ReadField72 is called with P+368
constexpr GuestAddress kManagerOffset = 1888U;        // P+1888: the manager pointer
constexpr GuestAddress kModeByte = 402U;              // P+402
constexpr GuestAddress kStateWord = 1936U;            // P+1936
constexpr GuestAddress kTargetWord = 1940U;           // P+1940
constexpr GuestAddress kManagerListWord = 9280U;      // manager+9280
constexpr GuestAddress kManagerClassWord = 5696U;     // manager+5696
constexpr GuestAddress kManagerClassByte = 6572U;     // manager+6572 (signed)
constexpr GuestAddress kClassByte = 6572U;            // R+6572 (signed): a3
constexpr GuestAddress kTargetWordOffset = 5696U;     // R+5696
constexpr GuestAddress kTargetBlockedByte = 4574U;    // R+4574
constexpr GuestAddress kRecordQuad = 176U;            // record+176
constexpr GuestAddress kSpillOffset = 32U;            // sq a1,0(a0) with a0 = sp+32
constexpr GuestAddress kSpillWordOffset = 44U;        // lwc1 f2,44(sp): word 3 of the spilled quadword

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;              // 0x19d4d0..0x19d4ec: frame, clear, flag test, daddu
constexpr std::uint32_t kGateCost = 3U;               // 0x19d4f0..0x19d4f8: lbu, bne, nop
constexpr std::uint32_t kGateExitCost = 5U;           // 0x19d4fc..0x19d500 (beq, ld ra) and the epilogue
constexpr std::uint32_t kCallCost = 2U;               // 0x19d504..0x19d508: jal and its delay slot
constexpr std::uint32_t kNullCost = 2U;               // 0x19d50c..0x19d510: beq v0 and its delay slot
constexpr std::uint32_t kRecordCost = 11U;            // 0x19d514..0x19d53c: class index, manager list, record
constexpr std::uint32_t kBlockedCost = 3U;            // 0x19d540..0x19d548
constexpr std::uint32_t kClassCost = 3U;              // 0x19d54c..0x19d554
constexpr std::uint32_t kWordCost = 4U;               // 0x19d558..0x19d564
constexpr std::uint32_t kListCost = 2U;               // 0x19d568..0x19d56c
constexpr std::uint32_t kRangeCost = 11U;             // 0x19d570..0x19d598: spill, scale test, bc1f and delay slot
constexpr std::uint32_t kRangeExitCost = 2U;          // 0x19d59c..0x19d5a0: beq and its delay slot
constexpr std::uint32_t kStoreCost = 4U;              // 0x19d5a4..0x19d5b0: sw, lbu, beq, nop
constexpr std::uint32_t kStateFourCost = 2U;          // 0x19d5c0..0x19d5c4
constexpr std::uint32_t kStateFiveCost = 3U;          // 0x19d5b4..0x19d5bc
constexpr std::uint32_t kReturnCost = 4U;             // 0x19d5c8..0x19d5d4: ld ra, lq, jr, addiu
constexpr std::uint32_t kCeiling = 20000U;

// lb: the byte sign-extended to the 32-bit register value.
inline std::uint32_t sign_extend_byte(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

// 0x19d5a4..0x19d5d4: the candidate is stored, then the state word becomes 4, or 5 when the mode byte is set.
template <class Memory>
std::uint32_t store_candidate(Memory& memory, const GuestAddress object, const std::uint32_t target,
                              const std::uint32_t retired) {
    memory.write_u32_le(object + kTargetWord, target);                                // sw v0,1940(s0)
    if (memory.read_u8(object + kModeByte) == 0U) {
        memory.write_u32_le(object + kStateWord, 4U);                                 // addiu v1,zero,4; sw
        return retired + kStoreCost + kStateFourCost + kReturnCost;
    }
    memory.write_u32_le(object + kStateWord, 5U);                                     // addiu v1,zero,5; sw
    return retired + kStoreCost + kStateFiveCost + kReturnCost;
}

template <class Context>
n3::Run update_target_candidate(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0U);
    memory.write_u32_le(object + kStateWord, 0U);                                     // sw $zero,1936($a0)
    if (memory.read_u32_le(kFlagWord) == 0U) {
        return call.finish(kEntryCost + kReturnCost);
    }
    std::uint32_t retired = kEntryCost + kGateCost;
    if (memory.read_u8(object + kGateByte) == 0U) {
        return call.finish(retired + kGateExitCost);
    }

    call.set_arg(0U, object + kCandidateOffset);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0032f350(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    retired += kCallCost + callee.retired + kNullCost;
    const GuestAddress target = call.result();
    if (target == 0U) {
        return call.finish(retired + kReturnCost);
    }

    const std::uint32_t class_index = sign_extend_byte(memory.read_u8(target + kClassByte));
    const GuestAddress manager = memory.read_u32_le(object + kManagerOffset);
    const std::uint32_t list = memory.read_u32_le(manager + kManagerListWord);
    const GuestAddress record = memory.read_u32_le(kRecordTable + kRecordStride * class_index);
    retired += kRecordCost;

    if (list == 0U) {
        retired += kBlockedCost;
        if (memory.read_u8(target + kTargetBlockedByte) != 0U) {
            return call.finish(retired + kReturnCost);
        }
    }
    retired += kClassCost;
    if (memory.read_u32_le(manager + kManagerClassWord) == class_index) {
        return call.finish(store_candidate(memory, object, target, retired));
    }
    retired += kWordCost;
    const std::uint32_t manager_class = sign_extend_byte(memory.read_u8(manager + kManagerClassByte));
    if (memory.read_u32_le(target + kTargetWordOffset) == manager_class) {
        return call.finish(store_candidate(memory, object, target, retired));
    }
    retired += kListCost;
    if (list != 0U) {
        return call.finish(store_candidate(memory, object, target, retired));
    }

    retired += kRangeCost;
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    n3::store_quad_any(memory, frame + kSpillOffset, n3::load_quad_any(memory, record + kRecordQuad));
    const std::uint32_t word3 = memory.read_u32_le(frame + kSpillWordOffset);
    const std::uint32_t scale = memory.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) - kScaleBelowGp);
    if (fpu::lt(fpu::mul(scale, word3), kLimitBits)) {
        return call.finish(retired + kRangeExitCost + kReturnCost);
    }
    return call.finish(store_candidate(memory, object, target, retired));
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0019d4d0, 264U, 0x75c686288220c9cbULL, "UpdateTargetCandidate", kCeiling, update_target_candidate,
         EngineReturn::Nothing)
