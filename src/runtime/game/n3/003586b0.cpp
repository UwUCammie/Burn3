// 0x003586b0 (a0 = object, a1 = record): CopyRecordIntoObject. The record's first 128 bytes (eight quadwords) go to
// object+240; the words at record+128 and +132 go to object+368 and +372; the quadwords at record+144 and +160 go to
// object+384 and +400; the eight words at record+176..+207 (five floats, then three words) go to object+416..+444;
// the byte at record+208 goes to object+448; and last, the byte at object+481 is set to 1. Nothing is returned: v0 is
// not written, and the scratch registers the original leaves behind (v1, f0, a1..a3, t0) are not reproduced. Each
// store follows the load it copies, as the original's pairs do, and quadword accesses use the aligned address, as lq
// and sq do. Both block loops make exactly four passes (a2 is set to 4 and decremented once per pass, bgtz is the only
// test), so every call retires the same 91 instructions: 3 + 4 * 7 + 3 + 4 * 7 + 29.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// The record's copied fields sit this far into the object (record +0 is object +240).
constexpr GuestAddress kObjectOffset = 240U;
constexpr std::uint32_t kQuadBytes = 16U;
constexpr std::uint32_t kWordBytes = 4U;
constexpr std::uint32_t kBlockQuads = 8U;       // record +0..+127: the two loops' eight quadwords
constexpr GuestAddress kWordA = 128U;           // words at record +128 and +132
constexpr GuestAddress kWordB = 132U;
constexpr GuestAddress kQuadA = 144U;           // quadwords at record +144 and +160
constexpr GuestAddress kQuadB = 160U;
constexpr GuestAddress kTailWords = 176U;       // eight words at record +176..+207
constexpr std::uint32_t kTailWordCount = 8U;
constexpr GuestAddress kByteField = 208U;       // byte at record +208
constexpr GuestAddress kFlagField = 481U;       // byte at object +481 (not shifted by kObjectOffset)
constexpr std::uint8_t kFlagValue = 1U;

// Retired instructions on every path (the loops always make four passes): the listing's blocks, delay slots included.
constexpr std::uint32_t kRetired = 91U;

template <class Memory>
void copy_quad(Memory& m, const GuestAddress to, const GuestAddress from) {
    n3::store_quad_any(m, to, n3::load_quad_any(m, from));
}

template <class Memory>
void copy_word(Memory& m, const GuestAddress to, const GuestAddress from) {
    const std::uint32_t value = m.read_u32_le(from);
    m.write_u32_le(to, value);
}

template <class Memory>
void copy_byte(Memory& m, const GuestAddress to, const GuestAddress from) {
    const std::uint8_t value = m.read_u8(from);
    m.write_u8(to, value);
}

template <class Context>
n3::Run copy_record_into_object(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress record = call.arg(1);
    const GuestAddress fields = object + kObjectOffset;
    for (std::uint32_t quad = 0U; quad < kBlockQuads; ++quad) {
        copy_quad(m, fields + kQuadBytes * quad, record + kQuadBytes * quad);
    }
    copy_word(m, fields + kWordA, record + kWordA);
    copy_word(m, fields + kWordB, record + kWordB);
    copy_quad(m, fields + kQuadA, record + kQuadA);
    copy_quad(m, fields + kQuadB, record + kQuadB);
    for (std::uint32_t word = 0U; word < kTailWordCount; ++word) {
        copy_word(m, fields + kTailWords + kWordBytes * word, record + kTailWords + kWordBytes * word);
    }
    copy_byte(m, fields + kByteField, record + kByteField);
    m.write_u8(object + kFlagField, kFlagValue);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003586b0, 196U, 0xf17305a3e331577aULL, "CopyRecordIntoObject", 91U, copy_record_into_object,
               EngineReturn::Nothing)
