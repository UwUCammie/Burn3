// 0x002b6ef0 (a0 = pool header, a1 = node count, a2 = payload size, a3 = alignment (0 means 1), t0 = optional
// template (a2 bytes copied into every node's payload), t1 = storage buffer): builds a doubly linked free list of
// a1 fixed-size nodes. Header: head +0, stride +4, count +8, alignment +12, copy of the head +16. Node: next +0,
// prev +4, payload +8. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kHead = 0U;          // header +0
constexpr GuestAddress kStride = 4U;        // header +4
constexpr GuestAddress kCount = 8U;         // header +8
constexpr GuestAddress kAlignment = 12U;    // header +12
constexpr GuestAddress kHeadCopy = 16U;     // header +16
constexpr GuestAddress kNext = 0U;          // node +0
constexpr GuestAddress kPrev = 4U;          // node +4
constexpr GuestAddress kPayload = 8U;       // node +8

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t build_pool(Memory& m, const GuestAddress header, const std::uint32_t node_count,
                         const std::uint32_t payload_size, const std::uint32_t alignment_arg,
                         const GuestAddress template_ptr, const GuestAddress storage) {
    std::uint32_t retired = 0U;

    // Branch on a3: the branch and its delay slot, then the store of a3 or of 1 (the zero path also runs
    // addiu, beq and the delay-slot store).
    if (alignment_arg == 0U) {
        retired += 5U;
        m.write_u32_le(header + kAlignment, 1U);
    } else {
        retired += 3U;
        m.write_u32_le(header + kAlignment, alignment_arg);
    }

    // Branch on a1: the branch and its delay slot.
    retired += 2U;
    if (node_count == 0U) {
        retired += 6U;  // four header stores, jr $ra and its delay slot
        m.write_u32_le(header + kHead, 0U);
        m.write_u32_le(header + kStride, payload_size);
        m.write_u32_le(header + kCount, 0U);
        m.write_u32_le(header + kHeadCopy, 0U);
        return retired;
    }

    // Head and first-node setup (20 instructions up to the delay slot of the branch to the link test).
    retired += 20U;
    const std::uint32_t alignment = m.read_u32_le(header + kAlignment);
    const GuestAddress head = (((storage + 7U) + alignment) & ~(alignment - 1U)) - 8U;
    m.write_u32_le(header + kHead, head);
    m.write_u32_le(header + kHeadCopy, head);
    const std::uint32_t alignment_again = m.read_u32_le(header + kAlignment);
    const GuestAddress first = m.read_u32_le(header + kHeadCopy);
    const std::uint32_t stride = ((payload_size + 8U + alignment_again) - 1U) & ~(alignment_again - 1U);
    m.write_u32_le(first + kPrev, 0U);

    // Link loop: each node links forward to the next node and that node links back to it.
    GuestAddress cur = first;
    std::uint32_t i = 1U;
    for (;;) {
        retired += 3U;  // sltu, bne and its delay slot: i < node_count, unsigned
        if (!(i < node_count)) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += 5U;
        const GuestAddress prev = cur;
        ++i;
        cur += stride;
        m.write_u32_le(prev + kNext, cur);
        m.write_u32_le(cur + kPrev, prev);
    }

    // The store in the delay slot of the template branch: it runs on every path.
    m.write_u32_le(cur + kNext, 0U);
    retired += 2U;
    if (template_ptr != 0U) {
        retired += 1U;
        GuestAddress node = m.read_u32_le(header + kHeadCopy);
        do {
            if (retired > kCeiling) {
                return retired;
            }
            retired += 4U;  // addiu, daddu, beq and its delay slot
            GuestAddress destination = node + kPayload;
            GuestAddress source = template_ptr;
            std::uint32_t remaining = payload_size;
            retired += 2U;  // the first byte-count test
            while (remaining != 0U) {
                if (retired > kCeiling) {
                    return retired;
                }
                const std::uint8_t value = m.read_u8(source);
                m.write_u8(destination, value);
                --remaining;
                ++source;
                ++destination;
                retired += 7U;  // five instructions of the copy, then the byte-count test
            }
            node = m.read_u32_le(node + kNext);
            retired += 3U;  // lw, bne and its delay slot
        } while (node != 0U);
    }

    m.write_u32_le(header + kStride, stride);
    m.write_u32_le(header + kCount, node_count);
    retired += 5U;  // the two header stores (one in a delay slot), jr $ra and its delay slot
    return retired;
}

template <class Context>
n3::Run init_free_list_pool(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(build_pool(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3), call.arg(4),
                                  call.arg(5)));
}

template <class Context>
std::uint32_t init_free_list_pool_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = build_pool(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                             context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U),
                                             context.cpu.read_gpr_u32(8U), context.cpu.read_gpr_u32(9U));
    return retired <= kCeiling ? retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_002b6ef0{0x002b6ef0U, 248U, 0x4015b9b7738163cfULL, "InitFreeListPool",
                                                  kCeiling, n3::live<&init_free_list_pool<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &init_free_list_pool_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b6ef0, init_free_list_pool, init_free_list_pool_cost)
