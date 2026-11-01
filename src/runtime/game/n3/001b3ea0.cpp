// 0x001b3ea0 (a0 = manager base): BuildNearbyObjectLists. For each slot i below the signed word N at 0x0064d37c,
// the slot's list of nearby secondary objects is rebuilt: the count byte at a0+96+i is cleared, then every secondary
// object Y (the u32 at a0+0xe6100+4*j, for j below the signed word M at a0+0xe620c, re-read on every test) is tested
// against the slot's object X (the u32 at a0+48*i+116, re-read every time). Z is the u32 at Y+516. The 16-byte
// positions at +48 of X and Z are differenced, squared and summed on VU0 (vf1..vf3 and ACC), and the sum is compared
// with 256.0 (C.OLT.S, so a NaN distance fails). Below 256.0, j is stored as a byte at a0+16*i+count while the count
// byte is below 16, and the count byte is then incremented. Returns nothing. Those list and count bytes can alias the
// count bytes and object pointers of other slots, so every access stays in the original order and goes through
// guest memory.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kSlotCount = 0x0064d37cU;       // lui 0x65, -11396: N (signed word)
constexpr GuestAddress kSecondaryList = 0x000e6100U;   // a0 + 0xe6100: u32 pointers, indexed 4*j
constexpr GuestAddress kSecondaryCount = 0x000e620cU;  // a0 + 0xe620c: M (signed word), re-read on every test
constexpr GuestAddress kSlotStride = 48U;              // slot record: a0 + 48*i
constexpr GuestAddress kSlotObject = 116U;             // slot record + 116: the slot's object X
constexpr GuestAddress kPartner = 516U;                // Y + 516: Z, the object that Y is tested against X with
constexpr GuestAddress kPosition = 48U;                // object + 48: 16-byte position (xyz used)
constexpr GuestAddress kListStride = 16U;              // a0 + 16*i: the slot's list, one byte per entry
constexpr GuestAddress kCount = 96U;                   // a0 + 96 + i: the slot's entry count, one byte
constexpr std::int32_t kListCapacity = 16;             // an entry is added only while the count is below this
constexpr std::uint32_t kLimit = 0x43800000U;          // 256.0 (f0): the squared-distance limit
constexpr std::uint8_t kMaskX = 8U;                    // VU0 destination field x
constexpr std::uint8_t kMaskXyz = 14U;                 // VU0 destination fields x, y and z

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 13U;           // 0x001b3ea0..0x001b3ed0: setup, then the jump to the slot test
constexpr std::uint32_t kSlotTestCost = 3U;            // 0x001b3f7c..0x001b3f84: slt, bne and its nop
constexpr std::uint32_t kSlotSetupCost = 10U;          // 0x001b3ed4..0x001b3ef8: count reset, jump to the entry test
constexpr std::uint32_t kSlotStepCost = 1U;            // 0x001b3f78: addiu i
constexpr std::uint32_t kExitCost = 2U;                // 0x001b3f88..0x001b3f8c: jr $ra and its nop
constexpr std::uint32_t kEntryTestCost = 4U;           // 0x001b3f68..0x001b3f74: lw M, slt, bne and its nop
constexpr std::uint32_t kEntryDistanceCost = 17U;      // 0x001b3efc..0x001b3f3c: loads, VU0 distance, compare, bc1f
constexpr std::uint32_t kEntryInRangeCost = 4U;        // 0x001b3f40..0x001b3f4c: count byte, the < 16 test
constexpr std::uint32_t kEntryAddCost = 5U;            // 0x001b3f50..0x001b3f60: list byte store, count bump
constexpr std::uint32_t kEntryStepCost = 1U;           // 0x001b3f64: addiu j

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// A read-only view of guest memory that keeps its own writes, for call_cost. Like the interpreter, it treats a load
// the original would fault on (lw unaligned or outside guest memory, lqc2 unaligned or outside it, sb outside it) as
// a fault: the value read is zero, nothing is written, and the call is declined so the original runs and faults itself.
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
    [[nodiscard]] guest::Gpr128 quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            faulted_ = true;
            return guest::Gpr128{};
        }
        return n3::load_quad_any(dry_, address);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// lqc2 on the implementation's memory: the call_cost run has already declined any address that is not a loadable
// quadword, so the aligned block is the whole load.
template <class Memory>
guest::Gpr128 quad_at(const Memory& memory, const GuestAddress address) {
    return n3::load_quad_any(memory, address);
}
template <class B>
guest::Gpr128 quad_at(const CheckedDryMemory<B>& memory, const GuestAddress address) { return memory.quad(address); }

// The whole function on guest memory (the implementation) or on a CheckedDryMemory with a scratch copy of the CPU
// (call_cost). Returns the retired count; above kCeiling it stops early and the caller discards that count.
template <class Memory>
std::uint32_t build_lists(Memory& memory, guest::R5900State& cpu, const GuestAddress a0) {
    std::uint32_t retired = kPrologueCost;
    const auto slot_count = static_cast<std::int32_t>(memory.read_u32_le(kSlotCount));
    cpu.write_fpr_bits(0U, kLimit);                                  // mtc1 $a1,$f0
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, kMaskX, 3U, false);    // vaddw.x vf3,vf0,vf0w: vf3.x = 1.0
    std::uint32_t slot = 0U;
    for (;;) {
        retired += kSlotTestCost;
        if (static_cast<std::int32_t>(slot) >= slot_count) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kSlotSetupCost;
        const GuestAddress count_byte = a0 + kCount + slot;
        const GuestAddress list = a0 + kListStride * slot;
        const GuestAddress record = a0 + kSlotStride * slot;
        memory.write_u8(count_byte, static_cast<std::uint8_t>(0U));  // sb $zero,96($a1)
        std::uint32_t entry = 0U;
        for (;;) {
            retired += kEntryTestCost;
            if (static_cast<std::int32_t>(entry) >=
                static_cast<std::int32_t>(memory.read_u32_le(a0 + kSecondaryCount))) {
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            retired += kEntryDistanceCost;
            const GuestAddress object = memory.read_u32_le(record + kSlotObject);
            const GuestAddress secondary = memory.read_u32_le(a0 + kSecondaryList + 4U * entry);
            cpu.write_vu0_vector(2U, quad_at(memory, object + kPosition));  // lqc2 vf2,48(X)
            const GuestAddress partner = memory.read_u32_le(secondary + kPartner);
            cpu.write_vu0_vector(1U, quad_at(memory, partner + kPosition));  // lqc2 vf1,48(Z)
            ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kMaskXyz, true);                       // vsub.xyz vf1,vf2,vf1
            ee_cop2::vmul(cpu, 1U, 1U, 1U, kMaskXyz);                                    // vmul.xyz vf1,vf1,vf1
            ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                           // vadday.x acc,vf1,vf1y
            ee_cop2::vmadd_broadcast(cpu, 1U, 3U, 1U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf3,vf1z
            const std::uint32_t squared = cpu.read_vu0_vector(1U).words[0];              // qmfc2 $t1,$vf1; mtc1 f1
            cpu.write_fpr_bits(1U, squared);
            if (fpu::lt(squared, kLimit)) {                                               // c.olt.s f1,f0
                retired += kEntryInRangeCost;
                const std::uint32_t count = memory.read_u8(count_byte);                  // lbu, slti 16
                if (static_cast<std::int32_t>(count) < kListCapacity) {
                    retired += kEntryAddCost;
                    memory.write_u8(list + count, static_cast<std::uint8_t>(entry));    // sb j
                    memory.write_u8(count_byte,                                           // lbu, addiu 1, sb
                                    static_cast<std::uint8_t>(memory.read_u8(count_byte) + 1U));
                }
            }
            retired += kEntryStepCost;
            ++entry;
        }
        retired += kSlotStepCost;
        ++slot;
    }
    return retired + kExitCost;
}

template <class Context>
std::uint32_t build_nearby_object_lists_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    guest::R5900State scratch{context.cpu};
    const std::uint32_t retired = build_lists(dry, scratch, context.cpu.read_gpr_u32(4U));
    return (dry.faulted() || retired > kCeiling) ? 0U : retired;
}

template <class Context>
n3::Run build_nearby_object_lists(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(build_lists(call.memory(), call.cpu(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_001b3ea0{0x001b3ea0U, 240U, 0xc4f6c99ce02db4faULL, "BuildNearbyObjectLists",
                                                  kCeiling, n3::live<&build_nearby_object_lists<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &build_nearby_object_lists_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001b3ea0, build_nearby_object_lists, build_nearby_object_lists_cost)
