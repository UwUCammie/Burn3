// 0x003620b0 (a0 = object): UpdateTrailRecords. The object keeps a clock (float +508, grown by the global float at
// 0x0052267c on every pass), distance sums (floats +504 and +512, and one float per index at +516 + 4 * index) and a
// ring of 16 records, 16 bytes each from the object's start: count +256, read index +260, write index +264, last
// record +268, head +272, tail +276, a record flag at +280. A record holds its kind at +4 and its length float at +12.
// Each pass measures, with VU0, the distance between the object's position (quad at +320) and the quad its source
// record names (the word at [[object + 552] + 516], plus 48), and copies that source quad to +480. Byte +498 clear is
// the stop path: the copy, then the state (+544) becomes the index (+408). Bytes +281 or +497 set, a different index
// and state, or a nonzero word at +820 is the reset path: the same copy, bytes +497 and +498 cleared, +504 zeroed, then
// the main path. The main path takes the state's kind from a 7-entry switch table (state 0 and states above 6 have
// none) and, with a kind, once the distance sum +504 reaches 10.0, extends the head record of that kind by this pass's
// distance or writes a new record at the write index. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// Object fields, as byte offsets from the object's start.
constexpr GuestAddress kFlag281 = 281U;         // byte: set takes the reset path
constexpr GuestAddress kFlag497 = 497U;         // byte: set takes the reset path; the reset clears it
constexpr GuestAddress kFlag498 = 498U;         // byte: clear takes the stop path; the reset clears it
constexpr GuestAddress kIndex = 408U;           // word: the current index
constexpr GuestAddress kPosition = 320U;        // quad: the object's position (VF2)
constexpr GuestAddress kQuadCopy = 480U;        // quad: copy of the source quad
constexpr GuestAddress kClock = 508U;           // float: the clock
constexpr GuestAddress kDistanceSum = 504U;     // float: distance since the reset
constexpr GuestAddress kTotal = 512U;           // float: distance over all indices
constexpr GuestAddress kIndexDistance = 516U;   // float per index: at 516 + 4 * index
constexpr GuestAddress kExtra = 820U;           // word: nonzero takes the reset path
constexpr GuestAddress kState = 544U;           // word: the state, which selects the record kind
constexpr GuestAddress kSource = 552U;          // word: the source record
constexpr GuestAddress kSourcePointer = 516U;   // word in the source record: the record that holds the quad
constexpr GuestAddress kSourceQuad = 48U;       // the quad sits 48 bytes into that record
constexpr GuestAddress kRecordCount = 256U;     // word: records written so far
constexpr GuestAddress kReadRecord = 260U;      // word: ring index of the oldest record
constexpr GuestAddress kWriteRecord = 264U;     // word: ring index of the next record
constexpr GuestAddress kLastRecord = 268U;      // word: the record last written
constexpr GuestAddress kHeadRecord = 272U;      // word: the head record, which later passes extend
constexpr GuestAddress kTailRecord = 276U;      // word: cleared by the extend and write paths
constexpr GuestAddress kRecordFlag = 280U;      // byte: set by a record write
constexpr std::uint32_t kRecordBytes = 16U;     // ring records are 16 bytes, from the object's start
constexpr GuestAddress kRecordKind = 4U;        // record: kind word
constexpr GuestAddress kRecordLength = 12U;     // record: length float

// Globals and the switch.
constexpr GuestAddress kClockStep = 0x0052267cU;   // float global (lui 0x52, lwc1 9852), added to the clock each pass
constexpr GuestAddress kStateTable = 0x004c76e0U;  // lui 0x4c, addiu 30432: one code address per state value
constexpr std::uint32_t kStateCount = 7U;          // sltiu: states 0..6 go through the table
constexpr std::uint32_t kNoKind = 31U;             // v1 when the test after the switch takes the tail directly
constexpr std::uint32_t kTenBits = 0x41200000U;    // lui 0x4120: 10.0f, the distance sum a record needs
constexpr std::uint32_t kCeiling = 20000U;

// The switch: the table word for each state names one of these targets. kind is what the case leaves in v1, retired
// is its own instructions before the test after the switch (0x3621e0).
struct SwitchCase {
    GuestAddress target;
    std::uint32_t kind;
    std::uint32_t retired;
};
constexpr std::array<SwitchCase, kStateCount> kSwitchCases{{
    {0x003621e0U, kNoKind, 0U},  // state 0: the test itself, with v1 still 31 from the branch's delay slot
    {0x003621b4U, 15U, 2U},
    {0x003621bcU, 14U, 2U},
    {0x003621c4U, 18U, 2U},
    {0x003621ccU, 17U, 2U},
    {0x003621d4U, 16U, 2U},
    {0x003621dcU, 16U, 1U},
}};

// Retired instruction counts of the listing's segments, delay slots included.
constexpr std::uint32_t kTestStop = 3U;        // 0x3620b0..0x3620b8: lbu 498, beq, nop
constexpr std::uint32_t kTestFlag281 = 3U;     // 0x3620bc..0x3620c4
constexpr std::uint32_t kTestFlag497 = 3U;     // 0x3620c8..0x3620d0
constexpr std::uint32_t kTestIndex = 4U;       // 0x3620d4..0x3620e0: index against state
constexpr std::uint32_t kTestExtra = 3U;       // 0x3620e4..0x3620ec: word 820
constexpr std::uint32_t kStopBody = 5U;        // 0x362320..0x362330: source quad copy
constexpr std::uint32_t kResetBody = 8U;       // 0x362248..0x362264: source quad copy and the clears
constexpr std::uint32_t kMainHead = 42U;       // 0x3620f0..0x362194: clock, VU0 distance, stores, state, switch
constexpr std::uint32_t kJump = 7U;            // 0x362198..0x3621b0: table word, jr and its delay slot
constexpr std::uint32_t kSwitchTest = 3U;      // 0x3621e0..0x3621e8
constexpr std::uint32_t kDistanceTest = 7U;    // 0x3621ec..0x362204: distance sum against 10.0
constexpr std::uint32_t kStateTest = 3U;       // 0x362208..0x362210: index against state
constexpr std::uint32_t kHeadTest = 3U;        // 0x362214..0x36221c: head pointer
constexpr std::uint32_t kKindTest = 3U;        // 0x362220..0x362228: head kind against the kind
constexpr std::uint32_t kExtend = 3U;         // 0x36222c..0x362234: head length += distance
constexpr std::uint32_t kClearTail = 1U;       // 0x362238
constexpr std::uint32_t kStoreState = 3U;      // 0x36223c..0x362244
constexpr std::uint32_t kReturn = 2U;          // 0x362334..0x362338
constexpr std::uint32_t kTail = kStoreState + kReturn;  // the state store and the return
constexpr std::uint32_t kPushHead = 10U;       // 0x362268..0x36228c: count, write slot, write index step
constexpr std::uint32_t kPushIndex = 5U;       // 0x36229c..0x3622ac: write index, read index compare
constexpr std::uint32_t kHeadFind = 3U;        // 0x3622b0..0x3622b8
constexpr std::uint32_t kHeadClear = 2U;       // 0x362310..0x362314
constexpr std::uint32_t kTailFind = 3U;        // 0x3622bc..0x3622c4
constexpr std::uint32_t kTailClear = 2U;       // 0x362318..0x36231c
constexpr std::uint32_t kRecordWrite = 10U;    // 0x3622c8..0x3622ec

// The outcome of the entry tests: the instructions retired through the test that decided, and whether it was a reset.
struct Entry {
    std::uint32_t retired;
    bool reset;
};

// One step of a ring index: the index it moves to, and the instructions the path takes on it (see write_step and
// read_step for what each counts).
struct Step {
    std::uint32_t index;
    std::uint32_t retired;
};

// The write index moves to (index + 1) & 15, and moves back by 16 when that sum is negative and the step is nonzero.
// The common instructions are in kPushHead; this step adds only what its negative branch retires.
Step write_step(const std::uint32_t index) {
    const std::uint32_t sum = index + 1U;
    const std::uint32_t masked = sum & 0xfU;
    if (static_cast<std::int32_t>(sum) >= 0) {
        return {masked, 0U};
    }
    if (masked == 0U) {
        return {0U, 2U};
    }
    return {masked - 16U, 3U};
}

// The read index moves the same way when the write index caught up with it. This step is the whole path (its
// retired count is the block's own, the branch taken or not).
Step read_step(const std::uint32_t index) {
    const std::uint32_t sum = index + 1U;
    const std::uint32_t masked = sum & 0xfU;
    if (static_cast<std::int32_t>(sum) >= 0) {
        return {masked, 5U};
    }
    if (masked == 0U) {
        return {0U, 7U};
    }
    return {masked - 16U, 8U};
}

// The entry tests after the stop test (0x3620bc..0x3620ec): the first one that fires sends the call to the reset
// path. The stop test itself has already decided that flag 498 is set.
template <class Memory>
Entry entry_tests(const Memory& m, const GuestAddress object) {
    std::uint32_t retired = kTestStop;
    if (m.read_u8(object + kFlag281) != 0U) {
        return {retired + kTestFlag281, true};
    }
    retired += kTestFlag281;
    if (m.read_u8(object + kFlag497) != 0U) {
        return {retired + kTestFlag497, true};
    }
    retired += kTestFlag497;
    if (m.read_u32_le(object + kIndex) != m.read_u32_le(object + kState)) {
        return {retired + kTestIndex, true};
    }
    retired += kTestIndex;
    if (m.read_u32_le(object + kExtra) != 0U) {
        return {retired + kTestExtra, true};
    }
    return {retired + kTestExtra, false};
}

// The source quad: the word at +552 names a record whose word at +516 names the record that holds the quad at +48.
template <class Memory>
GuestAddress source_quad(const Memory& m, const GuestAddress object) {
    const GuestAddress source = m.read_u32_le(object + kSource);
    return m.read_u32_le(source + kSourcePointer) + kSourceQuad;
}

// lq and sq ignore the low four address bits and fault outside guest memory (ee_interpreter.cpp).
template <class Memory>
bool quad_ok(const Memory& m, const GuestAddress address) {
    return m.contains(address & ~15U, 16U);
}

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
template <class Memory>
bool vector_ok(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

// The lq of the source quad and the sq to +480 (0x362248..0x362254 and 0x362320..0x362330).
template <class Memory>
void copy_quad(Memory& m, const GuestAddress object, const GuestAddress source) {
    n3::store_quad_any(m, object + kQuadCopy, n3::load_quad_any(m, source));
}

// The state store (0x36223c..0x362244): the state becomes the index.
template <class Memory>
void store_state(Memory& m, const GuestAddress object) {
    m.write_u32_le(object + kState, m.read_u32_le(object + kIndex));
}

// The write path (0x362268 on): counts the record, places it at the write index, and links it as head and tail.
// Returns the instructions it retires, the clear of the tail and the state store and return included.
template <class Memory>
std::uint32_t write_record(Memory& m, const GuestAddress object, const std::uint32_t kind, const std::uint32_t length) {
    std::uint32_t retired = kPushHead;
    const std::uint32_t count = m.read_u32_le(object + kRecordCount);
    m.write_u32_le(object + kRecordCount, count + 1U);
    const std::uint32_t write_index = m.read_u32_le(object + kWriteRecord);
    const GuestAddress slot = object + write_index * kRecordBytes;
    const Step next = write_step(write_index);
    retired += next.retired;
    m.write_u32_le(object + kWriteRecord, next.index);
    retired += kPushIndex;
    const std::uint32_t read_index = m.read_u32_le(object + kReadRecord);
    if (next.index == read_index) {
        const Step read = read_step(read_index);
        retired += read.retired;
        m.write_u32_le(object + kReadRecord, read.index);
    }
    retired += kHeadFind;
    if (m.read_u32_le(object + kHeadRecord) == slot) {
        m.write_u32_le(object + kHeadRecord, 0U);
        retired += kHeadClear;
    }
    retired += kTailFind;
    if (m.read_u32_le(object + kTailRecord) == slot) {
        m.write_u32_le(object + kTailRecord, 0U);
        retired += kTailClear;
    }
    retired += kRecordWrite;
    m.write_u8(object + kRecordFlag, std::uint8_t{1U});
    m.write_u32_le(object + kLastRecord, slot);
    m.write_u32_le(slot, 0U);
    m.write_u32_le(slot + kRecordKind, kind);
    m.write_u32_le(slot + 8U, 0U);
    m.write_u32_le(slot + kRecordLength, length);
    m.write_u32_le(object + kHeadRecord, m.read_u32_le(object + kLastRecord));
    m.write_u32_le(object + kTailRecord, 0U);
    store_state(m, object);
    return retired + kClearTail + kTail;
}

template <class Context>
n3::Run update_trail_records(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    // The stop path: a clear byte 498 copies the source quad and stores the state.
    if (m.read_u8(object + kFlag498) == 0U) {
        const GuestAddress source = source_quad(m, object);
        if (!quad_ok(m, source) || !quad_ok(m, object + kQuadCopy)) {
            return call.decline();
        }
        copy_quad(m, object, source);
        store_state(m, object);
        return call.finish(kTestStop + kStopBody + kTail);
    }

    const Entry entry = entry_tests(m, object);
    std::uint32_t retired = entry.retired;
    if (entry.reset) {
        const GuestAddress source = source_quad(m, object);
        if (!quad_ok(m, source) || !quad_ok(m, object + kQuadCopy)) {
            return call.decline();
        }
        copy_quad(m, object, source);
        m.write_u8(object + kFlag497, std::uint8_t{0U});
        m.write_u8(object + kFlag498, std::uint8_t{0U});
        m.write_u32_le(object + kDistanceSum, 0U);
        retired += kResetBody;
    }

    // The clock, then the VU0 distance from the position to the source quad (|P - Q| over x, y and z).
    const std::uint32_t clock_before = m.read_u32_le(object + kClock);
    const std::uint32_t clock_step = m.read_u32_le(kClockStep);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);           // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);               // qmfc2 a1,vf1
    m.write_u32_le(object + kClock, fpu::add(clock_before, clock_step));
    if (!vector_ok(m, object + kPosition)) {
        return call.decline();
    }
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, object + kPosition));  // lqc2 vf2,320(a0)
    const GuestAddress source = source_quad(m, object);
    if (!vector_ok(m, source)) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, source));              // lqc2 vf1,48(v1)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                  // vsub.xyz vf1,vf2,vf1
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                               // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                     // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved);                                    // qmtc2 a1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                        // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);           // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);        // vmulq.x vf1,vf1,q
    const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];   // qmfc2 v1,vf1; mtc1 v1,f2

    // The distance goes into the current index's float, the total and the distance sum.
    const std::uint32_t current_index = m.read_u32_le(object + kIndex);
    const GuestAddress index_distance = object + (current_index << 2U) + kIndexDistance;
    m.write_u32_le(index_distance, fpu::add(m.read_u32_le(index_distance), distance));
    m.write_u32_le(object + kTotal, fpu::add(m.read_u32_le(object + kTotal), distance));
    const std::uint32_t length = fpu::add(m.read_u32_le(object + kDistanceSum), distance);
    m.write_u32_le(object + kDistanceSum, length);
    retired += kMainHead;

    // The switch on the state. State 0 and states above 6 leave v1 at 31 (no kind).
    const std::uint32_t state = m.read_u32_le(object + kState);
    std::uint32_t kind = kNoKind;
    if (state < kStateCount) {
        const SwitchCase& switch_case = kSwitchCases[state];
        if (m.read_u32_le(kStateTable + 4U * state) != switch_case.target) {
            return call.decline();
        }
        kind = switch_case.kind;
        retired += kJump + switch_case.retired;
    }
    retired += kSwitchTest;
    if (kind == kNoKind) {
        store_state(m, object);
        return call.finish(retired + kTail);
    }
    retired += kDistanceTest;
    if (fpu::lt(length, kTenBits)) {
        store_state(m, object);
        return call.finish(retired + kTail);
    }

    // A record of this kind is due: write one, unless the head record of the same kind is there to be extended.
    retired += kStateTest;
    if (current_index != state) {
        return call.finish(retired + write_record(m, object, kind, length));
    }
    retired += kHeadTest;
    const GuestAddress head = m.read_u32_le(object + kHeadRecord);
    if (head == 0U) {
        return call.finish(retired + write_record(m, object, kind, length));
    }
    retired += kKindTest;
    if (m.read_u32_le(head + kRecordKind) != kind) {
        return call.finish(retired + write_record(m, object, kind, length));
    }
    m.write_u32_le(head + kRecordLength, fpu::add(m.read_u32_le(head + kRecordLength), distance));
    retired += kExtend;
    m.write_u32_le(object + kTailRecord, 0U);
    store_state(m, object);
    return call.finish(retired + kClearTail + kTail);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(003620b0, 652U, 0x131cee5a124212abULL, "UpdateTrailRecords", kCeiling, update_trail_records,
         EngineReturn::Nothing)
