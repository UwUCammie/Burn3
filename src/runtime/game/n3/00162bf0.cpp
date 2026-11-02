// 0x00162bf0 (a0 = node N): RepositionChainNode. N is one 80-byte record of the node table (node(id) = 0x00551910 +
// 80 * id; id 255 = none) and sits on the chain of its list. The list record is 0x00522660 + 76 * id + 556 (none when
// N's list id is 255). Its byte map is at B + the list's halfword at +4, B being the word at 0x0055d7dc. The slot is
// int(N.key) (+48), clamped below the map's length (the list's word at +0 points to a table whose word at +16 is the
// length). The byte there, cur, names the record R that owns the slot. N then goes where its key belongs:
//   - cur is N's own id (+73): nothing to do;
//   - cur is free (255): the free slots from the slot down take N's id;
//   - R comes after N on the chain (the walk from N's id through the next ids at +68 reaches cur): unless
//     N.key <= R.key, R is taken out, N is repositioned by this function again, and R is inserted again with its
//     scaled key (+44) kept. Otherwise the slots from the slot down that name R take N's id;
//   - R does not come after N: if N.key < R.key, N is taken out and inserted again, its scaled key (+44) kept.
// Always returns 1 (v0). The walk and the fills run over data, so the count comes from a dry run.

#include "takedown/runtime/game/n3_support.hpp"

// The callees (their callable bodies, see n3_call.hpp) and this function: the recursion calls it again.
N3_CALLEE(001613b0)  // UnlinkNodeFromList
N3_CALLEE(00161590)  // InsertIntoSortedChain
N3_CALLEE(00162bf0)  // RepositionChainNode

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kPoolPointer = 0x0055d7dcU;  // word: B, the base of the list byte maps
constexpr GuestAddress kListTable = 0x00522660U;    // list(id) = kListTable + 76 * id + 556
constexpr std::uint32_t kListStride = 76U;
constexpr std::uint32_t kListBias = 556U;
constexpr GuestAddress kNodeTable = 0x00551910U;    // node(id) = kNodeTable + 80 * id
constexpr std::uint32_t kNodeStride = 80U;
constexpr std::uint8_t kNone = 255U;
constexpr std::uint32_t kFrameBytes = 80U;          // this function's frame: addiu $sp, $sp, -80
constexpr std::uint32_t kCeiling = 20000U;
// Path A calls this function again, once per record it moves out. A chain that never settles would nest without end,
// so past this depth the call declines and the original runs it.
constexpr std::uint32_t kMaxNesting = 256U;

// Node record fields.
constexpr GuestAddress kScaledKey = 44U;   // float: kept across the moves
constexpr GuestAddress kKey = 48U;         // float
constexpr GuestAddress kClamp = 52U;       // float: the insert's f13
constexpr GuestAddress kListId = 65U;      // byte: the list (255 = none)
constexpr GuestAddress kNext = 68U;        // byte: the next record on the chain (255 = none)
constexpr GuestAddress kIndexByte = 70U;   // byte: the insert's a2 argument
constexpr GuestAddress kOwnId = 73U;       // byte: the record's own id

// List record fields.
constexpr GuestAddress kTable = 0U;        // word: pointer to the table
constexpr GuestAddress kMapOffset = 4U;    // halfword: the map's offset from B
constexpr GuestAddress kTableLength = 16U; // table word: the map's length

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 15U;       // 0x00162bf0..0x00162c28: N's key and list id, B
constexpr std::uint32_t kNoListCost = 2U;      // 0x00162c2c..0x00162c30: no list, L = 0
constexpr std::uint32_t kListCost = 9U;        // 0x00162c34..0x00162c54: L = the list record
constexpr std::uint32_t kLengthCost = 5U;      // 0x00162c58..0x00162c68: the map length and the slot test
constexpr std::uint32_t kClampCost = 1U;       // 0x00162c6c: the slot is length - 1
constexpr std::uint32_t kSlotCost = 7U;        // 0x00162c70..0x00162c88: the slot's byte and the own-id test
constexpr std::uint32_t kFreeCost = 3U;        // 0x00162c8c..0x00162c94: the free test (both outcomes)
constexpr std::uint32_t kNamedCost = 11U;      // 0x00162cd8..0x00162d08: R = node(cur)
constexpr std::uint32_t kWalkSetupCost = 6U;   // 0x00162d0c..0x00162d20
constexpr std::uint32_t kWalkStepCost = 16U;   // 0x00162d24..0x00162d6c: one per record on the walk
constexpr std::uint32_t kWalkMatchCost = 1U;   // 0x00162d64: the record on the walk is R
constexpr std::uint32_t kWalkEndCost = 3U;     // 0x00162d24..0x00162d2c: the walk reaches 255
constexpr std::uint32_t kFlagCost = 2U;        // 0x00162d70..0x00162d74
constexpr std::uint32_t kOrderCost = 4U;       // 0x00162d78..0x00162d84 or 0x00162dbc..0x00162dc8
constexpr std::uint32_t kFreeStepCost = 12U;   // 0x00162c98..0x00162cc4: a free slot takes N's id
constexpr std::uint32_t kFreeStopCost = 8U;    // 0x00162c98..0x00162cb0: the slot is not free
constexpr std::uint32_t kFreeEndCost = 2U;     // 0x00162c98..0x00162c9c: the index is below 0
constexpr std::uint32_t kFillStepCost = 13U;   // 0x00162d88..0x00162db8: a slot that names R takes N's id
constexpr std::uint32_t kFillStopCost = 9U;    // 0x00162d88..0x00162da8: the slot does not name R
constexpr std::uint32_t kFillEndCost = 2U;     // 0x00162d88..0x00162d8c: the index is below 0
constexpr std::uint32_t kMoveNCost = 11U;      // 0x00162dcc..0x00162df4: N unlinked and inserted, N+44 kept
constexpr std::uint32_t kMoveRCost = 9U;       // 0x00162df8..0x00162e18: R's scaled key, R unlinked, N again
constexpr std::uint32_t kRelinkCost = 7U;      // 0x00162e48..0x00162e60: R inserted, R+44 kept
constexpr std::uint32_t kExitCost = 9U;        // 0x00162cc8..0x00162ccc (v0 = 1) and the epilogue

// Nesting of reposition_chain_node on this thread (path A calls it from inside itself).
thread_local std::uint32_t nesting = 0U;

class NestingScope {
public:
    NestingScope() noexcept { ++nesting; }
    ~NestingScope() { --nesting; }
    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;
};

constexpr GuestAddress node_address(const std::uint32_t id) noexcept { return kNodeTable + kNodeStride * id; }
constexpr GuestAddress list_address(const std::uint32_t id) noexcept {
    return kListTable + kListStride * id + kListBias;
}

// The record after `id` on its chain (255 at the end).
template <class Memory>
std::uint8_t next_of(Memory& m, const std::uint8_t id) {
    return m.read_u8(node_address(id) + kNext);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Every loop checks the count
// against kCeiling and returns as soon as it passes it, so a chain that never ends cannot hang the dry run.
template <class Context>
n3::Run reposition_chain_node(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const NestingScope nesting_scope;
    const GuestAddress node = call.arg(0);
    const auto exit_with = [&call](const std::uint32_t count) {
        call.return_value(1U);
        return call.finish(count);
    };

    // N's key, its list and the map base.
    const std::uint32_t key = m.read_u32_le(node + kKey);
    const std::uint8_t list_id = m.read_u8(node + kListId);
    const GuestAddress list = list_id == kNone ? 0U : list_address(list_id);
    const GuestAddress map_base = m.read_u32_le(kPoolPointer);
    std::uint32_t retired = kHeadCost + (list_id == kNone ? kNoListCost : kListCost);

    // The slot: int(key), clamped below the map's length.
    const GuestAddress table = m.read_u32_le(list + kTable);
    const std::uint32_t length = m.read_u32_le(table + kTableLength);
    retired += kLengthCost;
    std::int32_t index = fpu::to_int(key);
    if (!(index < static_cast<std::int32_t>(length))) {
        index = static_cast<std::int32_t>(length - 1U);
        retired += kClampCost;
    }
    // The map byte at index i: B + the list's offset + i. The offset is read at each use, as the loops read it.
    const auto slot = [&m, list, map_base](const std::int32_t i) {
        return map_base + m.read_u16_le(list + kMapOffset) + static_cast<std::uint32_t>(i);
    };

    // cur is the byte the slot holds.
    retired += kSlotCost;
    const std::uint8_t own = m.read_u8(node + kOwnId);
    const std::uint8_t cur = m.read_u8(slot(index));
    if (cur == own) {
        return exit_with(retired + kExitCost);
    }
    retired += kFreeCost;
    if (cur == kNone) {
        // The free slots from the index down take N's id, for as long as they are free.
        for (;;) {
            if (retired > kCeiling) {
                return exit_with(retired);
            }
            if (index < 0) {
                retired += kFreeEndCost;
                break;
            }
            const GuestAddress at = slot(index);
            if (m.read_u8(at) != kNone) {
                retired += kFreeStopCost;
                break;
            }
            retired += kFreeStepCost;
            m.write_u8(at, m.read_u8(node + kOwnId));
            index -= 1;
        }
        return exit_with(retired + kExitCost);
    }

    // cur names another record R. Does the walk from N's own id reach R before the chain ends?
    const GuestAddress record = node_address(cur);
    retired += kNamedCost + kWalkSetupCost;
    bool reaches_record = false;
    std::uint8_t id = own;
    for (;;) {
        if (retired > kCeiling) {
            return exit_with(retired);
        }
        if (id == kNone) {
            break;
        }
        retired += kWalkStepCost;
        if (id == cur) {
            reaches_record = true;
            retired += kWalkMatchCost;
        }
        id = next_of(m, id);
    }
    retired += kWalkEndCost + kFlagCost + kOrderCost;
    const std::uint32_t record_key = m.read_u32_le(record + kKey);

    if (!reaches_record) {
        // R does not come after N: when N's key is below R's, N is taken out and inserted again.
        if (!fpu::lt(key, record_key)) {
            return exit_with(retired + kExitCost);
        }
        retired += kMoveNCost;
        const std::uint32_t scaled = m.read_u32_le(node + kScaledKey);
        call.set_arg(0, node);
        const n3::Run unlink = call.invoke([](auto& mem) { return n3::fn_001613b0(mem); }, kFrameBytes);
        if (unlink.declined) {
            return call.decline();
        }
        retired += unlink.retired;
        call.set_arg(0, node);
        call.set_arg(1, list);
        call.set_arg(2, m.read_u8(node + kIndexByte));
        call.set_float_arg(0, m.read_u32_le(node + kKey));
        call.set_float_arg(1, m.read_u32_le(node + kClamp));
        const n3::Run insert = call.invoke([](auto& mem) { return n3::fn_00161590(mem); }, kFrameBytes);
        if (insert.declined) {
            return call.decline();
        }
        retired += insert.retired;
        m.write_u32_le(node + kScaledKey, scaled);
        return exit_with(retired + kExitCost);
    }

    if (fpu::le(key, record_key)) {
        // N's key is not above R's: the slots from the index down that name R take N's id, for as long as they do.
        for (;;) {
            if (retired > kCeiling) {
                return exit_with(retired);
            }
            if (index < 0) {
                retired += kFillEndCost;
                break;
            }
            const GuestAddress at = slot(index);
            if (m.read_u8(at) != m.read_u8(record + kOwnId)) {
                retired += kFillStopCost;
                break;
            }
            retired += kFillStepCost;
            m.write_u8(at, m.read_u8(node + kOwnId));
            index -= 1;
        }
        return exit_with(retired + kExitCost);
    }

    // N's key is above R's: R comes out, N is repositioned by this function, then R goes back in.
    if (retired > kCeiling || nesting >= kMaxNesting) {
        return call.decline();
    }
    retired += kMoveRCost;
    const std::uint32_t record_scaled = m.read_u32_le(record + kScaledKey);
    call.set_arg(0, record);
    const n3::Run unlink = call.invoke([](auto& mem) { return n3::fn_001613b0(mem); }, kFrameBytes);
    if (unlink.declined) {
        return call.decline();
    }
    retired += unlink.retired;
    call.set_arg(0, node);
    const n3::Run again = call.invoke([](auto& mem) { return n3::fn_00162bf0(mem); }, kFrameBytes);
    if (again.declined) {
        return call.decline();
    }
    retired += again.retired;

    // R's list, index byte, key and clamp are read after the calls, as the original reads them.
    const std::uint8_t record_list = m.read_u8(record + kListId);
    retired += record_list == kNone ? kNoListCost : kListCost;
    call.set_arg(0, record);
    call.set_arg(1, record_list == kNone ? 0U : list_address(record_list));
    call.set_arg(2, m.read_u8(record + kIndexByte));
    call.set_float_arg(0, m.read_u32_le(record + kKey));
    call.set_float_arg(1, m.read_u32_le(record + kClamp));
    const n3::Run insert = call.invoke([](auto& mem) { return n3::fn_00161590(mem); }, kFrameBytes);
    if (insert.declined) {
        return call.decline();
    }
    retired += insert.retired + kRelinkCost;
    m.write_u32_le(record + kScaledKey, record_scaled);
    return exit_with(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00162bf0, 656U, 0xe1cd254efeac761aULL, "RepositionChainNode", kCeiling, reposition_chain_node,
         EngineReturn::Integer)
