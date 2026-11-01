// 0x0038abe0 (a0 = placement object O): HudPlacementUpdate, a void leaf. O holds four pointers: +12 P, a 64-byte
// placement block (its words at P+0 and P+4 are the floats x and y), +16 D (a 64-byte copy of P goes there unless D is
// 0), +20 Q (a vec2, read only) and +24 R (a vec2, written). The mode word at 0x01e90424 picks a block through the jump
// table at 0x004c9730: modes 0 and 1 run block Z, modes 2 to 4 block X, mode 5 block Y, and modes 6 and up skip the
// blocks. Block X, with the flag byte at 0x01e89792 set, stores P.x = Q.x - 0.0 and P.y = Q.y - 32.0 * scale (the float
// at 0x01e90420). Block Y with the flag set stores P.y = Q.y - 32.0; with the flag clear it compares P.y with Q.y and,
// when they differ, stores P.x back and P.y = Q.y. Block Z stores R.x = 0 and R.y = (0.0 + 96.0) + (-96.0) * scale.
// Every block then runs the copy. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kMode = 0x01e90424U;         // lui 0x1e9, lw 1060
constexpr GuestAddress kScale = 0x01e90420U;        // lui 0x1e9, lwc1 1056
constexpr GuestAddress kFlag = 0x01e89792U;         // lui 0x1e9, lbu -26734 (a byte)
constexpr GuestAddress kJumpTable = 0x004c9730U;    // lui 0x4d, addiu -26832: one word per mode, modes 0..5
constexpr std::uint32_t kModeCount = 6U;            // sltiu $at, $v1, 6: unsigned, so modes 6 and up skip the table
// The table words for modes 0..5, verified from the release ELF.
constexpr GuestAddress kBlockX = 0x0038ac14U;       // modes 2, 3 and 4
constexpr GuestAddress kBlockY = 0x0038ac68U;       // mode 5
constexpr GuestAddress kBlockZ = 0x0038aca8U;       // modes 0 and 1

constexpr GuestAddress kPlacement = 12U;            // P: lw 12($a0)
constexpr GuestAddress kDestination = 16U;          // D: lw 16($a0)
constexpr GuestAddress kPoint = 20U;                // Q: lw 20($a0)
constexpr GuestAddress kResult = 24U;               // R: lw 24($a0)

constexpr std::uint32_t kThirtyTwo = 0x42000000U;       // 32.0
constexpr std::uint32_t kNinetySix = 0x42c00000U;       // 96.0
constexpr std::uint32_t kMinusNinetySix = 0xc2c00000U;  // -96.0

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 6U;            // addiu $sp; lui, lw, sltiu, the branch and its delay slot
constexpr std::uint32_t kDispatchCost = 7U;         // lui, sll, addiu, addu, lw, jr and its delay slot
constexpr std::uint32_t kFlagCost = 4U;             // lui, lbu, the branch and its delay slot (blocks X and Y)
constexpr std::uint32_t kBlockXSetCost = 17U;       // flag set: the loads, the two stores, the jump to the copy
constexpr std::uint32_t kBlockYSetCost = 12U;       // flag set in block Y: the loads, two stores, the jump to Z
constexpr std::uint32_t kBlockYEqualCost = 7U;      // flag clear: the compare, its branch and the delay slot
constexpr std::uint32_t kBlockYDifferCost = 5U;     // flag clear, P.y != Q.y: the two rewrites and their loads
constexpr std::uint32_t kBlockZCost = 14U;          // 0x38aca8..0x38acdc: R.x = 0 and R.y, then falls into the copy
constexpr std::uint32_t kCopyNoneCost = 6U;         // D is 0: the test and its delay slot, then the return
constexpr std::uint32_t kCopyCost = 43U;            // D is set: the copy, then the return
// The longest path: mode 5 (either flag case) through block Z and the copy. No path retires more.
constexpr std::uint32_t kLongestPath =
    kEntryCost + kDispatchCost + kFlagCost + kBlockYSetCost + kBlockZCost + kCopyCost;

// What one run leaves besides memory: the retired count, whether the jump-table word named one of the blocks, and the
// ACC and C.EQ.S results the run sets.
struct Outcome {
    std::uint32_t retired{};
    bool covered{true};
    bool sets_accumulator{};
    std::uint32_t accumulator{};
    bool sets_condition{};
    bool condition{};
};

// The common copy at 0x0038ace0: when D (at O+16) is set, the 64 bytes of P (at O+12) are copied to D. Both quadwords
// at P+0 and P+16 are loaded before either is stored. Then the words at 32..55 move in three pairs, each pair read
// before it is written (the loop runs exactly three times: a1 = 3 is set just before it). The words at 56 and 60 come
// last. Returns the retired count, return included.
template <class Memory>
std::uint32_t copy_placement(Memory& m, const GuestAddress object) {
    const GuestAddress destination = m.read_u32_le(object + kDestination);
    if (destination == 0U) {
        return kCopyNoneCost;
    }
    const GuestAddress placement = m.read_u32_le(object + kPlacement);
    const auto first = n3::load_quad_any(m, placement);
    const auto second = n3::load_quad_any(m, placement + 16U);
    n3::store_quad_any(m, destination, first);
    n3::store_quad_any(m, destination + 16U, second);
    GuestAddress from = placement + 32U;
    GuestAddress to = destination + 32U;
    for (std::uint32_t pass = 0U; pass < 3U; ++pass) {
        const std::uint32_t low = m.read_u32_le(from);
        const std::uint32_t high = m.read_u32_le(from + 4U);
        m.write_u32_le(to, low);
        m.write_u32_le(to + 4U, high);
        from += 8U;
        to += 8U;
    }
    const std::uint32_t word_56 = m.read_u32_le(placement + 56U);
    m.write_u32_le(destination + 56U, word_56);
    const std::uint32_t word_60 = m.read_u32_le(placement + 60U);
    m.write_u32_le(destination + 60U, word_60);
    return kCopyCost;
}

// C.EQ.S sets the FCR31 condition bit. The paths without a compare leave it alone.
void set_condition(guest::R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                             : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
}

// Block X (modes 2 to 4). With the flag set, P.x = Q.x - 0.0 and P.y = Q.y - 32.0 * scale: both loads come before
// both stores. Returns the retired count of the block.
template <class Memory>
std::uint32_t block_x(Memory& m, const GuestAddress object) {
    if (m.read_u8(kFlag) == 0U) {
        return kFlagCost;
    }
    const GuestAddress point = m.read_u32_le(object + kPoint);
    const std::uint32_t scale = m.read_u32_le(kScale);
    const GuestAddress placement = m.read_u32_le(object + kPlacement);
    const std::uint32_t point_x = m.read_u32_le(point);
    const std::uint32_t point_y = m.read_u32_le(point + 4U);
    const std::uint32_t x = fpu::sub(point_x, 0U);
    const std::uint32_t y = fpu::sub(point_y, fpu::mul(kThirtyTwo, scale));
    m.write_u32_le(placement, x);
    m.write_u32_le(placement + 4U, y);
    return kFlagCost + kBlockXSetCost;
}

// Block Y (mode 5). Returns the retired count of the block. Block Y always continues into block Z, which the caller runs.
template <class Memory>
std::uint32_t block_y(Memory& m, const GuestAddress object, Outcome& out) {
    if (m.read_u8(kFlag) != 0U) {
        const GuestAddress placement = m.read_u32_le(object + kPlacement);
        const std::uint32_t placement_x = m.read_u32_le(placement);
        const GuestAddress point = m.read_u32_le(object + kPoint);
        const std::uint32_t y = fpu::sub(m.read_u32_le(point + 4U), kThirtyTwo);
        m.write_u32_le(placement, placement_x);
        m.write_u32_le(placement + 4U, y);
        return kFlagCost + kBlockYSetCost;
    }
    const GuestAddress point = m.read_u32_le(object + kPoint);
    const GuestAddress placement = m.read_u32_le(object + kPlacement);
    const std::uint32_t point_y = m.read_u32_le(point + 4U);
    const bool equal = fpu::eq(m.read_u32_le(placement + 4U), point_y);
    out.sets_condition = true;
    out.condition = equal;
    if (equal) {
        return kFlagCost + kBlockYEqualCost;
    }
    const std::uint32_t placement_x = m.read_u32_le(placement);
    m.write_u32_le(placement, placement_x);
    m.write_u32_le(placement + 4U, point_y);
    return kFlagCost + kBlockYEqualCost + kBlockYDifferCost;
}

// Block Z (modes 0 and 1, and after block Y). ADDA.S gives ACC = 0.0 + 96.0; MADD.S then gives R.y = ACC + (-96.0) *
// scale, and R.x is stored as the word 0 before it. Returns the retired count of the block.
template <class Memory>
std::uint32_t block_z(Memory& m, const GuestAddress object, Outcome& out) {
    const std::uint32_t scale = m.read_u32_le(kScale);
    const std::uint32_t accumulator = fpu::add(0U, kNinetySix);
    const std::uint32_t y = fpu::madd(accumulator, kMinusNinetySix, scale);
    const GuestAddress result = m.read_u32_le(object + kResult);
    out.sets_accumulator = true;
    out.accumulator = accumulator;
    m.write_u32_le(result, 0U);
    m.write_u32_le(result + 4U, y);
    return kBlockZCost;
}

// The whole function on a memory: the EeMemory itself, or the checked view call_cost uses. The steps follow the
// listing, so every access happens where the original makes it.
template <class Memory>
Outcome placement_outcome(Memory& m, const GuestAddress object) {
    Outcome out{};
    const std::uint32_t mode = m.read_u32_le(kMode);
    if (mode >= kModeCount) {
        // sltiu/beq: modes 6 and up go straight to the copy.
        const std::uint32_t copy_cost = copy_placement(m, object);
        out.retired = kEntryCost + copy_cost;
        return out;
    }
    // The jr target is the table word. The release image's table names only the three blocks below.
    const GuestAddress block = m.read_u32_le(kJumpTable + 4U * mode);
    out.retired = kEntryCost + kDispatchCost;
    switch (block) {
    case kBlockX: {
        const std::uint32_t x_cost = block_x(m, object);
        out.retired += x_cost;
        break;
    }
    case kBlockY: {
        // Block Y falls into block Z on every path.
        const std::uint32_t y_cost = block_y(m, object, out);
        out.retired += y_cost;
        const std::uint32_t z_cost = block_z(m, object, out);
        out.retired += z_cost;
        break;
    }
    case kBlockZ: {
        const std::uint32_t z_cost = block_z(m, object, out);
        out.retired += z_cost;
        break;
    }
    default:
        out.covered = false;
        return out;
    }
    const std::uint32_t copy_cost = copy_placement(m, object);
    out.retired += copy_cost;
    return out;
}

// A DryMemory view for call_cost that checks each access the way the interpreter does: a word needs a 4-byte aligned
// address inside guest memory (lw and sw fault otherwise), a quadword needs its 16-byte block inside guest memory, a
// byte needs guest memory. A faulting access is not performed and marks the view, so the call is declined and the
// original runs and faults itself.
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
        if (!word_accessible(address)) {
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (word_accessible(address)) {
            dry_.write_u32_le(address, value);
        }
    }
    void write_u128_le(const GuestAddress address, const std::array<std::uint32_t, 4>& words) {
        const GuestAddress aligned = address & ~15U;
        if (!memory_.contains(aligned, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(aligned, words);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool word_accessible(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return false;
        }
        return true;
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// call_cost: the exact count, or 0 when the original would fault on this input or the table word names no block. A
// declined call runs the original code, which takes the same path (and faults) itself.
template <class Context>
std::uint32_t hud_placement_update_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Outcome outcome = placement_outcome(dry, context.cpu.read_gpr_u32(4U));
    if (dry.faulted() || !outcome.covered) {
        return 0U;
    }
    return outcome.retired;
}

template <class Context>
n3::Run hud_placement_update(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const Outcome outcome = placement_outcome(call.memory(), call.arg(0));
    if (!outcome.covered) {
        throw std::logic_error("HudPlacementUpdate: jump-table target has no block");
    }
    if (outcome.sets_accumulator) {
        cpu.set_fpu_accumulator_bits(outcome.accumulator);
    }
    if (outcome.sets_condition) {
        set_condition(cpu, outcome.condition);
    }
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_0038abe0{0x0038abe0U, 416U, 0x336fdc1afb752604ULL, "HudPlacementUpdate",
                                                  kLongestPath, n3::live<&hud_placement_update<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &hud_placement_update_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0038abe0, hud_placement_update, hud_placement_update_cost)
