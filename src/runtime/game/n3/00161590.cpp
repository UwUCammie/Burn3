// 0x00161590 (a0 = the new record N, a1 = the list L, a2 = an int, f12 = N's key float, f13 = a clamp float):
// InsertIntoSortedChain. Links N into the ordered chain of 80-byte records of L and returns 1 (v0). A record is
// rec(id) = 0x00551910 + 80 * id (id 255 = none): +48 key float, +68 next id, +69 previous id, +73 own id. The list
// holds a pointer at +0 to a table whose +4 is an array of 8-byte entries (float at +0), a u16 map offset at +4 and
// the tail id at +70. The byte map base B is the word at 0x0055d7dc, and idx = int(f12) indexes the map at
// B + offset + idx: that byte names the record N goes in front of (its key is compared with N's key). N goes in
// front of that record (FIRST, then FILL1 fills downwards from idx while the entries name it), in front of a later
// record the walk finds (INS, no fill), or at the tail (TAIL, then FILL2 fills downwards from idx while the entries
// are free). The walk and the fills run over data, so the count comes from a dry run on DryMemory, as in 002bd6a0.cpp.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using guest::EeMemory;
using guest::R5900State;
using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kMapBase = 0x0055d7dcU;      // word: the byte map's base B
constexpr GuestAddress kRecordTable = 0x00551910U;  // rec(id) = kRecordTable + kRecordBytes * id
constexpr std::uint32_t kRecordBytes = 80U;
constexpr std::uint32_t kNone = 255U;               // id 255: no record
// gp-relative globals: the MAX.S operand, the MIN.S operand and the scale of the key.
constexpr GuestAddress kMaxFloat = static_cast<GuestAddress>(-30832);
constexpr GuestAddress kMinFloat = static_cast<GuestAddress>(-31792);
constexpr GuestAddress kScale = static_cast<GuestAddress>(-29820);
// Record fields (N and the chain records share them).
constexpr GuestAddress kStep = 32U;       // float: the step into entry idx (the entry itself when idx <= 0, else its difference from entry idx-1)
constexpr GuestAddress kScaledKey = 44U;  // float: the scale times the record's float at +0
constexpr GuestAddress kKey = 48U;        // float: the key
constexpr GuestAddress kClamp = 52U;      // float: f13 clamped by the two globals
constexpr GuestAddress kCopyByte = 65U;   // byte (N only): the list's byte at +71
constexpr GuestAddress kNext = 68U;       // byte: the next record's id
constexpr GuestAddress kPrev = 69U;       // byte: the previous record's id
constexpr GuestAddress kIndexByte = 70U;  // byte (N only): min(idx, 2)
constexpr GuestAddress kOwnId = 73U;      // byte: the record's own id
constexpr GuestAddress kFlags = 75U;      // byte (N only): |= 0x10
// List fields.
constexpr GuestAddress kTablePointer = 0U;  // word: the table
constexpr GuestAddress kTableArray = 4U;    // word at the table: the array of 8-byte entries
constexpr GuestAddress kMapOffset = 4U;     // u16: the offset of the map from B
constexpr GuestAddress kTail = 70U;         // byte: the tail id
constexpr GuestAddress kListCopy = 71U;     // byte: copied into N+65

// Retired instruction counts of the listing's blocks, delay slots included (see brief.py).
constexpr std::uint32_t kEntryCost = 21U;      // 0x161590..0x1615e0
constexpr std::uint32_t kNegCost = 7U;         // 0x1616dc..0x1616f4 (idx <= 0)
constexpr std::uint32_t kPosCost = 8U;         // 0x1615e4..0x161600 (idx > 0)
constexpr std::uint32_t kCostC = 11U;          // 0x161604..0x16162c
constexpr std::uint32_t kCostC2 = 2U;          // 0x161630..0x161634
constexpr std::uint32_t kCostC3 = 9U;          // 0x161640..0x161660
constexpr std::uint32_t kCostK = 2U;           // 0x1618a4..0x1618a8
constexpr std::uint32_t kCostS0 = 5U;          // 0x161664..0x161674
constexpr std::uint32_t kCostW0 = 4U;          // 0x161678..0x161684
constexpr std::uint32_t kCostW = 3U;           // 0x161688..0x161690
constexpr std::uint32_t kCostW1 = 2U;          // 0x161694..0x161698
constexpr std::uint32_t kCostW2 = 3U;          // 0x16169c..0x1616a4
constexpr std::uint32_t kCostW3 = 2U;          // 0x1616a8..0x1616ac
constexpr std::uint32_t kCostW4 = 7U;          // 0x1616b8..0x1616d0
constexpr std::uint32_t kCostW6 = 2U;          // 0x1616d4..0x1616d8
constexpr std::uint32_t kCostNotFound = 1U;    // 0x16182c
constexpr std::uint32_t kCostInsEntry = 2U;    // 0x161830..0x161834
constexpr std::uint32_t kCostIns = 4U;         // 0x161838..0x161844
constexpr std::uint32_t kCostIns2 = 2U;        // 0x161848..0x16184c
constexpr std::uint32_t kCostIns3 = 9U;        // 0x161858..0x161878
constexpr std::uint32_t kCostIns4 = 3U;        // 0x16187c..0x161884
constexpr std::uint32_t kCostInsNone = 2U;     // 0x16189c..0x1618a0
constexpr std::uint32_t kCostInsTail = 5U;     // 0x161888..0x161898
constexpr std::uint32_t kCostFirst = 6U;       // 0x1616f8..0x16170c
constexpr std::uint32_t kCostFirst2 = 2U;      // 0x161710..0x161714
constexpr std::uint32_t kCostFirst3 = 9U;      // 0x161720..0x161740
constexpr std::uint32_t kCostFirst4 = 1U;      // 0x161744
constexpr std::uint32_t kCostFirstSkip = 5U;   // 0x161748..0x161758
constexpr std::uint32_t kCostFill1Head = 2U;   // 0x16175c..0x161760
constexpr std::uint32_t kCostFill1Body = 7U;   // 0x161764..0x16177c
constexpr std::uint32_t kCostFill1Step = 4U;   // 0x161780..0x16178c
constexpr std::uint32_t kCostLock = 2U;        // 0x161790..0x161794
constexpr std::uint32_t kCostTail = 4U;        // 0x161798..0x1617a4
constexpr std::uint32_t kCostTail2 = 2U;       // 0x1617a8..0x1617ac
constexpr std::uint32_t kCostTail3 = 9U;       // 0x1617b8..0x1617d8
constexpr std::uint32_t kCostTail4 = 3U;       // 0x1617dc..0x1617e4
constexpr std::uint32_t kCostTailNone = 3U;    // 0x1617e8..0x1617f0
constexpr std::uint32_t kCostFill2Head = 2U;   // 0x1617f4..0x1617f8
constexpr std::uint32_t kCostFill2Body = 6U;   // 0x1617fc..0x161810
constexpr std::uint32_t kCostFill2Step = 4U;   // 0x161814..0x161820
constexpr std::uint32_t kCostRet = 2U;         // 0x161824..0x161828
constexpr std::uint32_t kCostRet2 = 2U;        // 0x1618ac..0x1618b0

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// MAX.S and MIN.S on the raw patterns, ordered as guest::ee_cop1::max_s and min_s order them.
std::uint32_t max_bits(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}
std::uint32_t min_bits(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}

// The record of a chain id (ids below 255).
GuestAddress record_of(const std::uint32_t id) { return kRecordTable + kRecordBytes * id; }

// Every access the body makes goes through here. The R5900 faults on a misaligned word or halfword and on an
// address outside mapped memory, so the first access that would do either sets the fault: nothing more is read
// or written after it, and call_cost declines the call, which leaves the faulting to the original.
template <class Memory, class Image>
class Access {
  public:
    Access(Memory& memory, const Image& image) noexcept : memory_(memory), image_(image) {}

    [[nodiscard]] bool fault() const noexcept { return fault_; }
    [[nodiscard]] std::uint8_t read8(const GuestAddress address) {
        return check(address, 1U, 0U) ? memory_.read_u8(address) : std::uint8_t{0U};
    }
    [[nodiscard]] std::uint16_t read16(const GuestAddress address) {
        return check(address, 2U, 1U) ? memory_.read_u16_le(address) : std::uint16_t{0U};
    }
    [[nodiscard]] std::uint32_t read32(const GuestAddress address) {
        return check(address, 4U, 3U) ? memory_.read_u32_le(address) : 0U;
    }
    void write8(const GuestAddress address, const std::uint8_t value) {
        if (check(address, 1U, 0U)) {
            memory_.write_u8(address, value);
        }
    }
    void write32(const GuestAddress address, const std::uint32_t value) {
        if (check(address, 4U, 3U)) {
            memory_.write_u32_le(address, value);
        }
    }

  private:
    bool check(const GuestAddress address, const std::size_t length, const GuestAddress align_mask) {
        if (!fault_ && (address & align_mask) == 0U && image_.contains(address, length)) {
            return true;
        }
        fault_ = true;
        return false;
    }

    Memory& memory_;
    const Image& image_;
    bool fault_{false};
};

// The argument registers and the globals the body reads.
struct Inputs {
    GuestAddress record;      // a0: N
    GuestAddress list;        // a1: L
    std::uint32_t index;      // a2: the int (only its low 32 bits count)
    std::uint32_t key;        // f12 bits
    std::uint32_t clamp;      // f13 bits
    GuestAddress gp;          // $gp
};

struct Outcome {
    std::uint32_t retired;    // the retired count (only meaningful without a fault)
    bool fault;
};

Inputs read_inputs(const R5900State& cpu) {
    return Inputs{cpu.read_gpr_u32(4U), cpu.read_gpr_u32(5U), cpu.read_gpr_u32(6U), cpu.read_fpr_bits(12U),
                  cpu.read_fpr_bits(13U), cpu.read_gpr_u32(n3::kGlobalPointer)};
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Each label is the listing
// address of its block; a block's count is added on entry and the stores follow the listing's order exactly.
template <class Memory>
Outcome insert_body(Memory& memory, const auto& image, const Inputs& in) {
    Access<Memory, std::remove_cvref_t<decltype(image)>> m{memory, image};
    const GuestAddress n = in.record;
    const GuestAddress l = in.list;
    const GuestAddress gp = in.gp;
    std::uint32_t retired = 0U;
    // The registers the blocks pass on: v0 is the map base (the implementation writes the return value 1), v1 the
    // not-found flag.
    std::uint32_t base = 0U;
    std::uint32_t flag = 0U;
    std::int32_t idx = 0;
    GuestAddress a3 = 0U;
    std::uint32_t t0 = 0U;
    std::uint32_t t1 = 0U;
    std::uint32_t t2 = 0U;
    std::int32_t t3 = 0;
    std::uint32_t f0 = 0U;
    std::uint32_t f1 = 0U;
    std::int32_t clamped = 0;
    bool not_positive = false;

    // 0x00161590..0x001615e0 (21): the fields of N that do not depend on the chain, stored in listing order.
    retired += kEntryCost;
    base = m.read32(kMapBase);
    clamped = std::min(static_cast<std::int32_t>(in.index), std::int32_t{2});  // pminw: signed word minimum
    m.write8(n + kIndexByte, static_cast<std::uint8_t>(clamped));
    idx = fpu::to_int(in.key);  // cvt.w.s f0, f12 ; mfc1 a2, f0 (sign-extended)
    flag = 0U;
    m.write32(n + kKey, in.key);
    f1 = max_bits(in.clamp, m.read32(gp + kMaxFloat));
    f0 = min_bits(f1, m.read32(gp + kMinFloat));
    m.write32(n + kClamp, f0);
    a3 = m.read8(l + kListCopy);
    m.write8(n + kCopyByte, static_cast<std::uint8_t>(a3));
    a3 = m.read8(n + kFlags) | 0x10U;
    not_positive = idx <= 0;
    m.write8(n + kFlags, static_cast<std::uint8_t>(a3));  // the delay slot of blez: always stored

    if (not_positive) {
        // 0x001616dc..0x001616f4 (7): the step is the array entry at idx itself (idx <= 0).
        retired += kNegCost;
        a3 = m.read32(l + kTablePointer);
        t0 = static_cast<std::uint32_t>(idx) << 3U;
        a3 = m.read32(a3 + kTableArray);
        a3 += t0;
        f0 = m.read32(a3);
        m.write32(n + kStep, f0);
    } else {
        // 0x001615e4..0x00161600 (8): the step from entry idx-1 to entry idx.
        retired += kPosCost;
        a3 = m.read32(l + kTablePointer);
        t0 = static_cast<std::uint32_t>(idx) << 3U;
        a3 = m.read32(a3 + kTableArray);
        a3 += t0;
        f1 = m.read32(a3);
        f0 = m.read32(a3 - 8U);
        f0 = fpu::sub(f1, f0);
        m.write32(n + kStep, f0);
    }

    // 0x00161604..0x0016162c (11): the map byte at B + offset + idx. A free byte (255) takes the tail path.
    retired += kCostC;
    f1 = m.read32(gp + kScale);
    a3 = kNone;
    f0 = m.read32(n);
    f0 = fpu::mul(f1, f0);
    m.write32(n + kScaledKey, f0);
    t0 = m.read16(l + kMapOffset);
    t0 = base + t0;
    t0 += static_cast<std::uint32_t>(idx);
    t1 = m.read8(t0);
    if (t1 == kNone) {
        goto L1618A4;
    }
    // 0x00161630..0x00161634 (2) and 0x00161640..0x00161660 (9): a3 is the record the map names (t1).
    retired += kCostC2 + kCostC3;
    a3 = record_of(t1);
    // 0x00161664..0x00161674 (5): N goes in front of a3 when N's key is not above a3's.
    retired += kCostS0;
    f1 = m.read32(n + kKey);
    f0 = m.read32(a3 + kKey);
    if (fpu::le(f1, f0)) {
        goto L1616F8;
    }
    // 0x00161678..0x00161684 (4): the walk starts from a3 (t1 = 255, f0 = a3's key).
    retired += kCostW0;
    t1 = kNone;
    f0 = m.read32(a3 + kKey);
L161688:
    // 0x00161688..0x00161690 (3): the walk's test of N's key against the key in f0 (record a3).
    if (m.fault() || retired > kCeiling) {
        return Outcome{retired, m.fault()};
    }
    retired += kCostW;
    if (fpu::le(f1, f0)) {
        goto L161830;
    }
    // 0x00161694..0x00161698 (2): a3 == 0 ends the walk; the delay slot reads a3's next id either way.
    retired += kCostW1;
    t2 = m.read8(a3 + kNext);
    if (a3 == 0U) {
        goto L16182C;
    }
    // 0x0016169c..0x001616a4 (3): a next id of 255 ends the walk (andi is a no-op on the byte).
    retired += kCostW2;
    if (t2 == t1) {
        goto L16182C;
    }
    // 0x001616a8..0x001616ac (2): bne taken (the next id is not 255): 0x001616b8.
    retired += kCostW3;
    // 0x001616b8..0x001616d0 (7): a3 = the record of the next id.
    retired += kCostW4;
    a3 = record_of(t2);
    // 0x001616d4..0x001616d8 (2): f0 = the key of the new a3 (the delay slot), back to the test.
    retired += kCostW6;
    f0 = m.read32(a3 + kKey);
    goto L161688;
L1618A4:
    // 0x001618a4..0x001618a8 (2): the map names no record: v1 = 1, the tail path.
    retired += kCostK;
    flag = 1U;
    goto L161790;
L16182C:
    // 0x0016182c (1): the walk found no place: v1 = 1.
    retired += kCostNotFound;
    flag = 1U;
L161830:
    // 0x00161830..0x00161834 (2): with v1 set, the tail path; else N goes in front of a3 (the insert below).
    retired += kCostInsEntry;
    if (flag != 0U) {
        goto L161790;
    }
    // 0x00161838..0x00161844 (4): t2 = a3's previous id.
    retired += kCostIns;
    t2 = m.read8(a3 + kPrev);
    t0 = kNone;
    if (t2 == t0) {
        goto L16189C;
    }
    // 0x00161848..0x0016184c (2): bne taken to 0x00161858.
    retired += kCostIns2;
    // 0x00161858..0x00161878 (9): t1 = the record of t2, which takes N as its next.
    retired += kCostIns3;
    t1 = record_of(t2);
    // 0x0016187c..0x00161884 (3): N's previous is t2; the record of t2 gets N as its next.
    retired += kCostIns4;
    m.write8(n + kPrev, static_cast<std::uint8_t>(t2));
    t0 = m.read8(n + kOwnId);
    m.write8(t1 + kNext, static_cast<std::uint8_t>(t0));
    goto L161888;
L16189C:
    // 0x0016189c..0x001618a0 (2): nothing lies in front of a3: N's previous id is 255 (t0).
    retired += kCostInsNone;
    m.write8(n + kPrev, static_cast<std::uint8_t>(t0));
L161888:
    // 0x00161888..0x00161898 (5): N's next is a3; a3's previous is N.
    retired += kCostInsTail;
    t0 = m.read8(a3 + kOwnId);
    m.write8(n + kNext, static_cast<std::uint8_t>(t0));
    t0 = m.read8(n + kOwnId);
    m.write8(a3 + kPrev, static_cast<std::uint8_t>(t0));
    goto L161790;
L1616F8:
    // 0x001616f8..0x0016170c (6): N takes a3's previous id, read back from N (the listing re-reads it).
    retired += kCostFirst;
    t1 = m.read8(a3 + kPrev);
    t0 = kNone;
    m.write8(n + kPrev, static_cast<std::uint8_t>(t1));
    t3 = static_cast<std::int32_t>(m.read8(n + kPrev));
    if (static_cast<std::uint32_t>(t3) == t0) {
        goto L161748;
    }
    // 0x00161710..0x00161714 (2): bne taken to 0x00161720; its delay slot reads N's own id.
    retired += kCostFirst2;
    t2 = m.read8(n + kOwnId);
    // 0x00161720..0x00161740 (9): the record of that previous id takes N as its next.
    retired += kCostFirst3;
    t0 = record_of(static_cast<std::uint32_t>(t3));
    // 0x00161744 (1)
    retired += kCostFirst4;
    m.write8(t0 + kNext, static_cast<std::uint8_t>(t2));
L161748:
    // 0x00161748..0x00161758 (5): N's next is a3, a3's previous is N; the fill starts at idx.
    retired += kCostFirstSkip;
    t0 = m.read8(a3 + kOwnId);
    t3 = idx;
    m.write8(n + kNext, static_cast<std::uint8_t>(t0));
    t0 = m.read8(n + kOwnId);
    m.write8(a3 + kPrev, static_cast<std::uint8_t>(t0));
L16175C:
    // 0x0016175c..0x00161760 (2): the fill runs while t3 is not negative.
    if (m.fault() || retired > kCeiling) {
        return Outcome{retired, m.fault()};
    }
    retired += kCostFill1Head;
    if (t3 < 0) {
        goto L161790;
    }
    // 0x00161764..0x0016177c (7): the map entry at B + offset + t3 must name a3 (t1 = a3's own id).
    retired += kCostFill1Body;
    t0 = m.read16(l + kMapOffset);
    t1 = m.read8(a3 + kOwnId);
    t0 = base + t0;
    t2 = t0 + static_cast<std::uint32_t>(t3);
    t0 = m.read8(t2);
    if (t1 != t0) {
        goto L161790;
    }
    // 0x00161780..0x0016178c (4): the entry takes N's id; t3 steps down.
    retired += kCostFill1Step;
    t0 = m.read8(n + kOwnId);
    t3 -= 1;
    m.write8(t2, static_cast<std::uint8_t>(t0));
    goto L16175C;
L161790:
    // 0x00161790..0x00161794 (2): v1 clear returns; v1 set takes the tail path.
    retired += kCostLock;
    if (flag == 0U) {
        goto L161824;
    }
    // 0x00161798..0x001617a4 (4): the list's tail id; 255 means the list has no tail.
    retired += kCostTail;
    t0 = m.read8(l + kTail);
    flag = kNone;
    if (t0 == flag) {
        goto L1617E8;
    }
    // 0x001617a8..0x001617ac (2): bne taken to 0x001617b8.
    retired += kCostTail2;
    // 0x001617b8..0x001617d8 (9): a3 = the tail record.
    retired += kCostTail3;
    a3 = record_of(t0);
    // 0x001617dc..0x001617e4 (3): N's previous is the tail; the tail's next is N.
    retired += kCostTail4;
    m.write8(n + kPrev, static_cast<std::uint8_t>(t0));
    flag = m.read8(n + kOwnId);
    m.write8(a3 + kNext, static_cast<std::uint8_t>(flag));
L1617E8:
    // 0x001617e8..0x001617f0 (3): the list's tail becomes N; a3 = 255 for the fill.
    retired += kCostTailNone;
    flag = m.read8(n + kOwnId);
    a3 = kNone;
    m.write8(l + kTail, static_cast<std::uint8_t>(flag));
L1617F4:
    // 0x001617f4..0x001617f8 (2): the fill runs while idx is not negative.
    if (m.fault() || retired > kCeiling) {
        return Outcome{retired, m.fault()};
    }
    retired += kCostFill2Head;
    if (idx < 0) {
        goto L161824;
    }
    // 0x001617fc..0x00161810 (6): the map entry at B + offset + idx must be free (255, the value in a3).
    retired += kCostFill2Body;
    flag = m.read16(l + kMapOffset);
    flag = base + flag;
    t0 = flag + static_cast<std::uint32_t>(idx);
    flag = m.read8(t0);
    if (flag != a3) {
        goto L161824;
    }
    // 0x00161814..0x00161820 (4): the free entry takes N's id; idx steps down.
    retired += kCostFill2Step;
    flag = m.read8(n + kOwnId);
    idx -= 1;
    m.write8(t0, static_cast<std::uint8_t>(flag));
    goto L1617F4;
L161824:
    // 0x00161824..0x00161828 (2) and 0x001618ac..0x001618b0 (2): the return with v0 = 1 (set by the caller).
    retired += kCostRet + kCostRet2;
    return Outcome{retired, m.fault()};
}

template <class Context>
std::uint32_t insert_into_sorted_chain_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Outcome outcome = insert_body(dry, context.memory, read_inputs(context.cpu));
    return outcome.fault || outcome.retired > kCeiling ? 0U : outcome.retired;
}

template <class Context>
n3::Run insert_into_sorted_chain(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const Outcome outcome = insert_body(memory, memory, read_inputs(call.cpu()));
    call.return_value(1U);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00161590{0x00161590U, 804U, 0xae610f4f2c5b45cfULL, "InsertIntoSortedChain",
                                                  kCeiling, n3::live<&insert_into_sorted_chain<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &insert_into_sorted_chain_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00161590, insert_into_sorted_chain, insert_into_sorted_chain_cost)
