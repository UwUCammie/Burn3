// 0x003827b0 (a0 = u16 id list, ended by a zero halfword; a1 low byte = flags): SumLinkListSpan.
// T = [0x00665ed0] is a record table: u32 bucket heads at T+32+4*(id&0x7f), the end sentinel record pointer
// at T+28, the linear list head at T+160 and the float scale at T+8. A record is 32 bytes: floats at +0, +8,
// +16 and +24, and its id as a halfword at +28. Flag 0x20 sets f2 to the float at gp-32268 times the +24 float
// of the record with id 32 (found on the linear list), and f2 is added to each record's +24 term. Flag 0x40
// selects the first and last terms. The result is the sum times T+8, or +0 unless some looked-up record has a
// float at +0 that is not negative. The original's only stores are to its own stack frame, which is not
// reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTablePointer = 0x00665ed0U;  // lui 0x66, lw 24272: the record table T
constexpr std::uint32_t kSentinelOffset = 28U;       // T+28: the end sentinel record pointer
constexpr std::uint32_t kBucketOffset = 32U;         // T+32+4*(id&0x7f): bucket heads
constexpr std::uint32_t kLinearHeadOffset = 160U;    // T+160: linear list head
constexpr std::uint32_t kScaleOffset = 8U;           // T+8: the float the sum is multiplied by
constexpr std::uint32_t kSpilledFloatOffset = 12U;   // T+12: loaded, then spilled to the frame, by the original
constexpr std::uint32_t kGpScaleOffset = 32268U;     // gp-32268: the float of the flag 0x20 term
constexpr std::uint32_t kRecordBytes = 32U;
constexpr std::uint32_t kIdOffset = 28U;             // record: id halfword
constexpr std::uint32_t kScanFlag = 0x20U;
constexpr std::uint32_t kAltFlag = 0x40U;
constexpr std::uint32_t kScanId = 32U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;      // 0x3827b0..0x3827c8: frame, flags, table pointer, scan branch
constexpr std::uint32_t kScanStartCost = 3U;  // 0x3827cc..0x3827d4: linear list head, wanted id 32
constexpr std::uint32_t kScanEndCost = 3U;    // 0x3827f8..0x382800: the flag 0x20 term
constexpr std::uint32_t kTestCost = 4U;       // a chain walk's id test: lhu, nop, bne, nop
constexpr std::uint32_t kSentinelCost = 3U;   // a chain walk's end test: lw, beq, nop
constexpr std::uint32_t kStepCost = 1U;       // a chain walk's step to the next record
constexpr std::uint32_t kBucketCost = 6U;     // 0x382804..0x382818: the first id and its bucket head
constexpr std::uint32_t kFoundCost = 6U;      // 0x38283c..0x382850: the first record's valid test
constexpr std::uint32_t kClearCost = 1U;      // daddu: the valid flag cleared by a negative float
constexpr std::uint32_t kFlagsCost = 4U;      // 0x382858..0x382864: the flag 0x40 test
constexpr std::uint32_t kNegateCost = 3U;     // 0x382868..0x382870: the first term without flag 0x40
constexpr std::uint32_t kNextCost = 4U;       // 0x3828e4..0x3828f0: step to the next id, zero test
constexpr std::uint32_t kLoopHeadCost = 9U;   // 0x382874..0x382894: one loop term and its bucket head
constexpr std::uint32_t kLoopFoundCost = 3U;  // 0x3828b8..0x3828c0: the valid flag test
constexpr std::uint32_t kRecheckCost = 6U;    // 0x3828c4..0x3828d8: the valid test while still clear
constexpr std::uint32_t kLoopEndCost = 1U;    // 0x3828e0
constexpr std::uint32_t kEndCheckCost = 2U;   // 0x3828f4..0x3828f8 and 0x3828fc..0x382900: two 2-word tests
constexpr std::uint32_t kPairTermCost = 5U;   // 0x382904..0x382914: the last term without flag 0x40
constexpr std::uint32_t kLastTermCost = 2U;   // 0x382918..0x38291c: the last term with flag 0x40
constexpr std::uint32_t kScaleCost = 6U;      // 0x382920..0x382934: the scale and the product
constexpr std::uint32_t kZeroCost = 1U;       // 0x382938: the result when no record is valid
constexpr std::uint32_t kReturnCost = 2U;     // 0x38293c..0x382940: jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// One chain walk (the linear scan and every bucket lookup): test the record's id and stop on a match, else
// test the end sentinel and stop on it (returning the sentinel), else step to the next record. The id test
// comes first, so the sentinel is not tested before the first record. Returns early once the count passes
// kCeiling, so a cyclic chain cannot hang the dry run.
template <class Memory>
GuestAddress walk_records(const Memory& m, const GuestAddress table, GuestAddress rec, const std::uint32_t wanted,
                          std::uint32_t& retired) {
    for (;;) {
        retired += kTestCost;
        if (m.read_u16_le(rec + kIdOffset) == wanted) {
            return rec;
        }
        if (retired > kCeiling) {
            return rec;
        }
        retired += kSentinelCost;
        if (rec == m.read_u32_le(table + kSentinelOffset)) {
            return rec;
        }
        retired += kStepCost;
        rec += kRecordBytes;
    }
}

// The walk's loads, checked the way the interpreter checks them: lhu needs a 2-byte aligned address, lw and
// lwc1 a 4-byte one, and each must lie in guest memory. A load the original would fault on is not performed:
// the wrapper records it, so call_cost declines the call and the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

struct SpanResult {
    std::uint32_t retired;
    std::uint32_t f0;
};

// The whole function on guest memory (the implementation) or on a read-only view (call_cost): returns the
// retired count and the f0 bits. Once the count passes kCeiling the walks stop early, so the count is above it.
template <class Memory>
SpanResult sum_span(const Memory& m, const GuestAddress list, const std::uint32_t flags_word,
                    const std::uint32_t global_pointer) {
    const std::uint32_t flags = flags_word & 0xffU;
    // The table pointer's lw is in the delay slot of the flag 0x20 branch, so it runs on both paths.
    const GuestAddress table = m.read_u32_le(kTablePointer);
    std::uint32_t retired = kEntryCost;
    std::uint32_t f2 = 0U;  // mtc1 $zero, $f2
    if ((flags & kScanFlag) != 0U) {
        retired += kScanStartCost;
        const GuestAddress head = m.read_u32_le(table + kLinearHeadOffset);
        const GuestAddress scanned = walk_records(m, table, head, kScanId, retired);
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kScanEndCost;
        const std::uint32_t scanned_24 = m.read_u32_le(scanned + 24U);
        const std::uint32_t gp_scale = m.read_u32_le(global_pointer - kGpScaleOffset);
        f2 = fpu::mul(gp_scale, scanned_24);
    }

    // The first id is used without a terminator test, as the original does.
    std::uint32_t id = m.read_u16_le(list);
    retired += kBucketCost;
    GuestAddress rec = m.read_u32_le(table + kBucketOffset + 4U * (id & 0x7fU));
    rec = walk_records(m, table, rec, id, retired);
    if (retired > kCeiling) {
        return {retired, 0U};
    }
    retired += kFoundCost;
    const bool first_negative = fpu::lt(m.read_u32_le(rec), 0U);
    std::uint32_t valid = first_negative ? 0U : 1U;
    if (first_negative) {
        retired += kClearCost;
    }
    retired += kFlagsCost;
    std::uint32_t sum = 0U;  // mtc1 $zero, $f3
    if ((flags & kAltFlag) == 0U) {
        retired += kNegateCost;
        sum = fpu::neg(m.read_u32_le(rec + 16U));
    }

    GuestAddress cursor = list;
    for (;;) {
        retired += kNextCost;
        cursor += 2U;
        id = m.read_u16_le(cursor);
        if (id == 0U) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kLoopHeadCost;
        // The previous record's +24 term (before this id's record replaces it) plus f2 goes into the sum.
        const std::uint32_t previous = fpu::add(m.read_u32_le(rec + 24U), f2);
        sum = fpu::add(sum, previous);
        rec = m.read_u32_le(table + kBucketOffset + 4U * (id & 0x7fU));
        rec = walk_records(m, table, rec, id, retired);
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kLoopFoundCost;
        if (valid == 0U) {
            retired += kRecheckCost;
            const bool negative = fpu::lt(m.read_u32_le(rec), 0U);
            valid = negative ? 0U : 1U;
            if (negative) {
                retired += kClearCost;
            }
        }
        retired += kLoopEndCost;
    }

    retired += kEndCheckCost;
    if (valid == 0U) {
        return {retired + kZeroCost + kReturnCost, 0U};
    }
    retired += kEndCheckCost;
    if ((flags & kAltFlag) != 0U) {
        retired += kLastTermCost;
        sum = fpu::add(sum, m.read_u32_le(rec + 24U));
    } else {
        retired += kPairTermCost;
        const std::uint32_t low = m.read_u32_le(rec + 16U);
        const std::uint32_t high = m.read_u32_le(rec + 8U);
        sum = fpu::add(sum, fpu::add(low, high));
    }
    retired += kScaleCost;
    const std::uint32_t scale = m.read_u32_le(table + kScaleOffset);
    static_cast<void>(m.read_u32_le(table + kSpilledFloatOffset));
    return {retired + kReturnCost, fpu::mul(sum, scale)};
}

template <class Context>
std::uint32_t sum_link_list_span_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const SpanResult result = sum_span(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                       context.cpu.read_gpr_u32(n3::kGlobalPointer));
    if (result.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return result.retired;
}

template <class Context>
n3::Run sum_link_list_span(Context& context) {
    auto call = n3::guest_call(context);
    const SpanResult result = sum_span(call.memory(), call.arg(0), call.arg(1),
                                       call.cpu().read_gpr_u32(n3::kGlobalPointer));
    call.cpu().write_fpr_bits(0U, result.f0);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_003827b0{0x003827b0U, 404U, 0x81ed55f2477f2fc4ULL, "SumLinkListSpan",
                                                  kCeiling, n3::live<&sum_link_list_span<NativeFunctionContext>>, EngineReturn::Float, {}, 1e-4, 1e-6,
                                                  {}, &sum_link_list_span_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003827b0, sum_link_list_span, sum_link_list_span_cost)
