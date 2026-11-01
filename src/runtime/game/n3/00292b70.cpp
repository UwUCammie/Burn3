// 0x00292b70 (a0 = list object, a1 = owner, a2 = flags, a3 = offset from the object to its pool
// buffer): initialises a list object. The owner's descriptor pointer (at owner+76) gives the count of
// 12-byte nodes (u16 at +12) and of 8-byte nodes (u16 at +14). Flags bit 0 builds both node pools
// under a header, in the buffer (at the offset held by owner+64) or inside the object when the buffer
// pointer is null. Flags bit 1 selects the alternate layout: the header moves 4 bytes further on and
// a zero end word is written. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint32_t kBuildPoolsBit = 1U;
constexpr std::uint32_t kAlternateBit = 2U;
constexpr GuestAddress kDescriptorOffset = 76U;   // owner + 76: the owner's descriptor pointer
constexpr GuestAddress kSizeWordOffset = 64U;     // owner + 64: the header's offset in the buffer
constexpr std::uint32_t kSizeMask = 0x0fffffffU;  // only the low 28 bits of that word count

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns
// the retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t init_list(Memory& m, const GuestAddress self, const GuestAddress owner,
                        const std::uint32_t flags_arg, const std::uint32_t offset) {
    std::uint32_t retired = 0U;

    // 0x292b70..0x292b8c: store the owner, the flags and the pool pointer, then test bit 0.
    m.write_u32_le(self + 0U, owner);
    const GuestAddress pool = self + offset;
    m.write_u32_le(self + 28U, flags_arg);
    m.write_u32_le(self + 32U, pool);
    const std::uint32_t flags = m.read_u32_le(self + 28U);
    retired += 8U;

    if ((flags & kBuildPoolsBit) != 0U) {
        // 0x292b90..0x292b98: the buffer pointer, and the test for a null buffer.
        const std::uint32_t buffer = m.read_u32_le(self + 32U);
        retired += 3U;
        GuestAddress header = 0U;
        if (buffer == 0U) {
            // 0x292cd4..0x292ce8: the header is inside the object, at +40 (+44 when alternate).
            retired += 4U;
            header = self + ((flags & kAlternateBit) == 0U ? 40U : 44U);
        } else {
            // 0x292b9c..0x292bc0 or 0x292bc4..0x292bd4: the header follows the buffer's offset word.
            retired += 3U;
            const std::uint32_t owner_seen = m.read_u32_le(self + 0U);
            const std::uint32_t size = m.read_u32_le(owner_seen + kSizeWordOffset) & kSizeMask;
            if ((flags & kAlternateBit) == 0U) {
                retired += 5U;
                header = buffer + size;
            } else {
                retired += 7U;
                header = buffer + size + 4U;
            }
        }
        // 0x292bd8..0x292c10: the first header word points past itself, at the 12-byte nodes; the
        // second points at the 8-byte nodes after them. The descriptor and its counts are read again
        // on the way, exactly where the listing reads them.
        m.write_u32_le(header, header + 8U);
        const std::uint32_t desc = m.read_u32_le(owner + kDescriptorOffset);
        const std::uint32_t first = m.read_u32_le(header);
        const std::uint32_t count12 = m.read_u16_le(desc + 12U);
        m.write_u32_le(header + 4U, first + 12U * count12);
        const std::uint32_t desc_again = m.read_u32_le(owner + kDescriptorOffset);
        const GuestAddress nodes12 = m.read_u32_le(header);
        const std::uint32_t count12_again = m.read_u16_le(desc_again + 12U);
        retired += 15U;

        // 0x292c14..0x292c3c: the 12-byte nodes. Each points at itself twice, then copies its first word.
        std::uint32_t i12 = 0U;
        for (;;) {
            retired += 4U;  // 0x292c3c..0x292c48: the index test, its delay slot included
            if (retired > kCeiling) {
                return retired;
            }
            if (!(i12 < count12_again)) {
                break;
            }
            retired += 10U;  // 0x292c14..0x292c38
            const GuestAddress node = nodes12 + 12U * i12;
            m.write_u32_le(node, node);
            m.write_u32_le(node + 4U, node);
            const std::uint32_t copied = m.read_u32_le(node);
            m.write_u32_le(node + 8U, copied);
            i12 = (i12 + 1U) & 0xffffU;
        }

        // 0x292c4c..0x292c5c: the 8-byte nodes start at the second header word; their count is read here.
        retired += 5U;
        const GuestAddress nodes8 = m.read_u32_le(header + 4U);
        const std::uint32_t desc_end = m.read_u32_le(owner + kDescriptorOffset);
        const std::uint32_t count8 = m.read_u16_le(desc_end + 14U);
        std::uint32_t i8 = 0U;
        for (;;) {
            retired += 4U;  // 0x292c78..0x292c84: the index test, its delay slot included
            if (retired > kCeiling) {
                return retired;
            }
            if (!(i8 < count8)) {
                break;
            }
            retired += 6U;  // 0x292c60..0x292c74
            const GuestAddress node = nodes8 + 8U * i8;
            m.write_u32_le(node, node);
            m.write_u32_le(node + 4U, node);
            i8 = (i8 + 1U) & 0xffffU;
        }
    }

    // 0x292c88..0x292c94: flags bit 1 is read again from the object. With it set, a zero end word goes
    // at the buffer's end (buffer + size), or at +40 when there is no buffer.
    retired += 4U;
    const std::uint32_t flags_end = m.read_u32_le(self + 28U);
    if ((flags_end & kAlternateBit) != 0U) {
        const std::uint32_t buffer_end = m.read_u32_le(self + 32U);
        retired += 3U;
        GuestAddress end_word = self + 40U;
        if (buffer_end != 0U) {
            const std::uint32_t owner_end = m.read_u32_le(self + 0U);
            const std::uint32_t size_end = m.read_u32_le(owner_end + kSizeWordOffset) & kSizeMask;
            retired += 5U;
            end_word = buffer_end + size_end;
        }
        m.write_u32_le(end_word, 0U);
        retired += 2U;
    }

    // 0x292cc0..0x292cd0 and 0x292cec..0x292cf0 (jr $ra and its delay slot): the words at +12 and +16
    // hold the head's own address, and the words at +20 and +24 are cleared.
    const GuestAddress head = self + 12U;
    m.write_u32_le(self + 12U, head);
    m.write_u32_le(self + 16U, head);
    m.write_u32_le(self + 24U, 0U);
    m.write_u32_le(self + 20U, 0U);
    retired += 5U + 2U;
    return retired;
}

template <class Context>
std::uint32_t init_list_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = init_list(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                            context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run init_list_native(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired = init_list(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3));
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00292b70{0x00292b70U, 388U, 0x593fcf041ad88bbeULL, "InitNodePoolLists",
                                                  kCeiling, n3::live<&init_list_native<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &init_list_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00292b70, init_list_native, init_list_cost)
