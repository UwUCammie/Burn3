// 0x00103e28 (a0 = config record C): ConfigureDmaController. Returns v0 (Integer).
// C holds bytes b0..b3 (+0..+3), halfwords h4 (+4), h6 (+6), h8 (+8) and h10 (+10), and words w12 (+12) and w16 (+16).
// b0, b1 and b2 must each be below 10 and b3 below 7, checked in that order; otherwise v0 is -1, -2, -3 or -4 and
// nothing is stored. Otherwise the DMA control word (D_CTRL, 0x1000e000) is rebuilt from its own old value: the byte
// tables at 0x004b1ae0 + b0, 0x004b1af0 + b1 and 0x004b1b00 + b2 are shifted left 4, 6 and 2 and merged in by mask,
// one after another; then b3 == 0 clears bit 1, and b3 != 0 sets bit 1 and ORs in (b3 - 1) << 8. The registers are
// then written in order: D_CTRL, D_PCR (0x1000e020) = h4 << 16 | h6, D_SQWC (0x1000e030) = h10 << 16 | h8, D_RBOR
// (0x1000e050) = w12 and D_RBSR (0x1000e040) = w16. Last, the 20 bytes of C are copied to the shadow at 0x004e3008.
// No loops and no calls: each exit's retired count is fixed by the listing. A record that is not 4-aligned, or whose
// 24-byte aligned block is not mapped, makes the original's lhu/lw or loads fault, so call_cost declines it and the
// original runs and faults itself.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kDmaControl = 0x1000e000U;   // D_CTRL (lui 0x1000, ori 0xe000)
constexpr GuestAddress kDmaPcr = 0x1000e020U;       // D_PCR
constexpr GuestAddress kDmaSqwc = 0x1000e030U;      // D_SQWC
constexpr GuestAddress kDmaRbsr = 0x1000e040U;      // D_RBSR
constexpr GuestAddress kDmaRbor = 0x1000e050U;      // D_RBOR
constexpr GuestAddress kShadow = 0x004e3008U;       // lui 0x4e, addiu 12296: the shadow copy of C
constexpr GuestAddress kTableB0 = 0x004b1ae0U;      // byte table indexed by b0 (lui 0x4b, addiu 6880)
constexpr GuestAddress kTableB1 = 0x004b1af0U;      // byte table indexed by b1 (addiu 6896)
constexpr GuestAddress kTableB2 = 0x004b1b00U;      // byte table indexed by b2 (addiu 6912)

// Retired instruction counts of each exit, delay slots included (brief.py's table).
constexpr std::uint32_t kInvalidB0Cost = 21U;       // v0 = -1
constexpr std::uint32_t kInvalidB1Cost = 24U;       // v0 = -2
constexpr std::uint32_t kInvalidB2Cost = 27U;       // v0 = -3
constexpr std::uint32_t kInvalidB3Cost = 31U;       // v0 = -4
constexpr std::uint32_t kZeroB3Cost = 101U;         // b3 == 0: the beq is taken to 0x00103f58
constexpr std::uint32_t kNonZeroB3Cost = 107U;      // 1 <= b3 <= 6: the longest path, the declared cost

// The result for the four config bytes, checked in the original's order. The first byte out of range gives v0 = -1,
// -2, -3 or -4 and no stores. Otherwise v0 = 0 and the stores run; their retired count depends on b3 alone.
struct ConfigureResult {
    std::uint32_t v0{};
    std::uint32_t retired{};
    bool stores{};
};

ConfigureResult result_for_bytes(const std::uint32_t b0, const std::uint32_t b1, const std::uint32_t b2,
                                 const std::uint32_t b3) {
    if (b0 >= 10U) {
        return {static_cast<std::uint32_t>(-1), kInvalidB0Cost, false};
    }
    if (b1 >= 10U) {
        return {static_cast<std::uint32_t>(-2), kInvalidB1Cost, false};
    }
    if (b2 >= 10U) {
        return {static_cast<std::uint32_t>(-3), kInvalidB2Cost, false};
    }
    if (b3 >= 7U) {
        return {static_cast<std::uint32_t>(-4), kInvalidB3Cost, false};
    }
    return {0U, b3 == 0U ? kZeroB3Cost : kNonZeroB3Cost, true};
}

// The whole function on guest memory.
ConfigureResult configure_dma_controller_on(auto& m, const GuestAddress record) {
    // The original reads all five DMA registers first. Only D_CTRL feeds the result, so the other four are not read.
    std::uint32_t control = m.read_u32_le(kDmaControl);
    const std::uint32_t b0 = m.read_u8(record);
    const std::uint32_t b1 = m.read_u8(record + 1U);
    const std::uint32_t b2 = m.read_u8(record + 2U);
    const std::uint32_t b3 = m.read_u8(record + 3U);
    const ConfigureResult result = result_for_bytes(b0, b1, b2, b3);
    if (!result.stores) {
        return result;
    }
    // Each merge uses the control word the one before produced. The table bytes are not masked, as in the original.
    control = (control & 0xffffffcfU) | (static_cast<std::uint32_t>(m.read_u8(kTableB0 + b0)) << 4U);
    control = (control & 0xffffff3fU) | (static_cast<std::uint32_t>(m.read_u8(kTableB1 + b1)) << 6U);
    control = (control & 0xfffffff3U) | (static_cast<std::uint32_t>(m.read_u8(kTableB2 + b2)) << 2U);
    if (b3 == 0U) {
        control &= 0xfffffffdU;
    } else {
        control = ((control | 2U) & 0xfffffcffU) | ((b3 - 1U) << 8U);
    }
    // The original reads h4, h6, h8, h10 and w16 before its first store, and w12 between the SQWC and RBOR stores.
    const std::uint32_t h4 = m.read_u16_le(record + 4U);
    const std::uint32_t h6 = m.read_u16_le(record + 6U);
    const std::uint32_t h8 = m.read_u16_le(record + 8U);
    const std::uint32_t h10 = m.read_u16_le(record + 10U);
    const std::uint32_t w16 = m.read_u32_le(record + 16U);
    m.write_u32_le(kDmaControl, control);
    m.write_u32_le(kDmaPcr, (h4 << 16U) | h6);
    m.write_u32_le(kDmaSqwc, (h10 << 16U) | h8);
    const std::uint32_t w12 = m.read_u32_le(record + 12U);
    m.write_u32_le(kDmaRbor, w12);
    m.write_u32_le(kDmaRbsr, w16);
    // The copy reads all of C before it writes, as the original's loads come before its stores.
    const std::uint64_t head = m.read_u64_le(record);
    const std::uint64_t middle = m.read_u64_le(record + 8U);
    const std::uint32_t tail = m.read_u32_le(record + 16U);
    m.write_u64_le(kShadow, head);
    m.write_u64_le(kShadow + 8U, middle);
    m.write_u32_le(kShadow + 16U, tail);
    return result;
}

// call_cost: the retired count for the record's bytes, or 0 when the original would fault on it (a record that is not
// 4-aligned, or whose 24-byte aligned block is not mapped). The original then runs and faults itself.
template <class Context>
std::uint32_t configure_dma_controller_cost(const Context& context) {
    const GuestAddress record = context.cpu.read_gpr_u32(4U);
    if ((record & 3U) != 0U || !context.memory.contains(record & ~7U, 24U)) {
        return 0U;
    }
    const std::uint32_t b0 = context.memory.read_u8(record);
    const std::uint32_t b1 = context.memory.read_u8(record + 1U);
    const std::uint32_t b2 = context.memory.read_u8(record + 2U);
    const std::uint32_t b3 = context.memory.read_u8(record + 3U);
    return result_for_bytes(b0, b1, b2, b3).retired;
}

template <class Context>
n3::Run configure_dma_controller(Context& context) {
    auto call = n3::guest_call(context);
    const ConfigureResult result = configure_dma_controller_on(call.memory(), call.arg(0));
    call.return_value(result.v0);
    return call.finish(result.retired);
}

} // namespace

// Float tolerances are zero: the function is integer-only, and the checker's default tolerance would accept a RAM word
// that differs in its low mantissa bits whenever it decodes as a normal float.
extern constinit const EngineFunction n3_00103e28{0x00103e28U, 472U, 0xecf1067bd2931ce2ULL, "ConfigureDmaController",
                                                  107U, n3::live<&configure_dma_controller<NativeFunctionContext>>, EngineReturn::Integer, {}, 0.0,
                                                  0.0, {}, &configure_dma_controller_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00103e28, configure_dma_controller, configure_dma_controller_cost)
