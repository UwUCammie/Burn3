// 0x0015af80 (a0, a1 both game object pointers): ObjectsAreRelatedEitherWay, a leaf predicate. Returns in v0 1 when
// either object's flag table marks the other object's class, 0 when neither does.
//
// Each object keeps a pointer at +200 to a descriptor whose signed byte at +6572 is the object's class index. Each
// object has a flag table at +1465 indexed by the OTHER object's class: a0's table at a1's class is read first, and
// a1's table at a0's class only when that misses. No stores and no calls. The retired counts are the listing's exit
// counts: 8 (first hit), 14 (second hit), 15 (neither). The temporary v1 ends as the last flag byte read.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kObjectDescriptor = 200U; // lw: descriptor pointer in the object
constexpr std::uint32_t kDescriptorClass = 6572U; // lb: signed class index in the descriptor
constexpr std::uint32_t kFlagTable = 1465U;       // lbu: flag table in the object
constexpr unsigned kV1 = 3U;

// Exit counts from the listing; both delay slots (daddiu v0,1 and nop) are counted where they execute.
constexpr std::uint32_t kFirstHitCost = 8U;
constexpr std::uint32_t kSecondHitCost = 14U;
constexpr std::uint32_t kNeitherCost = 15U;

// lw + lb: the object's class index, sign-extended to 32 bits as the original's addu sees it.
std::uint32_t class_index(const auto& memory, const GuestAddress object) {
    const GuestAddress descriptor = memory.read_u32_le(object + kObjectDescriptor);
    const auto signed_class = static_cast<std::int8_t>(memory.read_u8(descriptor + kDescriptorClass));
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_class));
}

// lbu: the flag in table_owner's table for the class other_class.
std::uint8_t related_flag(const auto& memory, const GuestAddress table_owner,
                          const std::uint32_t other_class) {
    return memory.read_u8(table_owner + other_class + kFlagTable);
}

template <class Context>
n3::Run objects_related_either_way(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress a0 = call.arg(0);
    const GuestAddress a1 = call.arg(1);

    const std::uint8_t first = related_flag(memory, a0, class_index(memory, a1));
    if (first != 0U) {
        call.cpu().write_gpr_u64(kV1, static_cast<std::uint64_t>(first));
        call.return_value(1U);
        return call.finish(kFirstHitCost);
    }
    const std::uint8_t second = related_flag(memory, a1, class_index(memory, a0));
    call.cpu().write_gpr_u64(kV1, static_cast<std::uint64_t>(second));
    if (second != 0U) {
        call.return_value(1U);
        return call.finish(kSecondHitCost);
    }
    call.return_value(0U);
    return call.finish(kNeitherCost);
}

} // namespace

extern constinit const EngineFunction n3_0015af80{0x0015af80U, 60U, 0x21398c803207f1a1ULL, "ObjectsAreRelatedEitherWay",
                                                  kNeitherCost, n3::live<&objects_related_either_way<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015af80, objects_related_either_way)
