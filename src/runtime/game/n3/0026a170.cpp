// 0x0026a170 (a0 = blob header B): RelocateOffsetTable. The word at B+12 is an offset to a table of 32-bit
// offsets (zero means no table, and nothing is written). It is rebased to the address B + offset, then every
// table entry i below the signed byte count at B+17 is rebased in place to B + entry. The table pointer at B+12
// and the count at B+17 are re-read before every entry, so a table entry that aliases either word changes what
// the next pass sees, as in the original. Returns nothing: the caller (0x0015e930) reloads v0 and recomputes v1
// before it reads either, so neither is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kTableWord = 12U;       // B+12: table offset, rebased to an address
constexpr std::uint32_t kCountByte = 17U;       // B+17: signed (lb) entry count

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEmptyTableCost = 5U;   // lw, beq, nop, jr, nop
constexpr std::uint32_t kSetupCost = 7U;        // lw, beq, nop, addu, daddu, beq, sw
constexpr std::uint32_t kTestCost = 4U;         // lb, slt, bne, nop
constexpr std::uint32_t kEntryCost = 7U;        // lw, sll, addiu, addu, lw, addu, sw
constexpr std::uint32_t kReturnCost = 2U;       // jr $ra, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count. The byte count is signed and the index only grows while it is below the count,
// so at most 127 entries are processed (1410 retired instructions): this ceiling is never reached.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate(Memory& m, const GuestAddress blob) {
    const std::uint32_t offset = m.read_u32_le(blob + kTableWord);
    if (offset == 0U) {
        return kEmptyTableCost;
    }
    m.write_u32_le(blob + kTableWord, offset + blob);
    std::uint32_t retired = kSetupCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        const auto count = static_cast<std::int8_t>(m.read_u8(blob + kCountByte));
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(count)) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kEntryCost;
        const GuestAddress entry = m.read_u32_le(blob + kTableWord) + (index << 2U);
        ++index;
        const std::uint32_t value = m.read_u32_le(entry);
        m.write_u32_le(entry, value + blob);
    }
    return retired + kReturnCost;
}

// A load or store the original would fault on (misaligned, or outside the mapped memory) is not performed by the
// dry run: it is recorded and call_cost declines, so the original runs and faults itself.
template <class Base>
class CheckedDry {
public:
    explicit CheckedDry(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
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
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t relocate_offset_table_cost(const Context& context) {
    CheckedDry dry{context.memory};
    const std::uint32_t retired = relocate(dry, context.cpu.read_gpr_u32(4U));
    if (dry.faulted() || retired > kCeiling) {
        return 0U;
    }
    return retired;
}

template <class Context>
n3::Run relocate_offset_table(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_0026a170{0x0026a170U, 80U, 0x7e5f4318ab987ef0ULL, "RelocateOffsetTable",
                                                  kCeiling, n3::live<&relocate_offset_table<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &relocate_offset_table_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0026a170, relocate_offset_table, relocate_offset_table_cost)
