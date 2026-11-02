// 0x002621d0 (a0 = emitter context E, a1 = view object V): EmitEmitterRecords, the dispatcher of the two record emitters.
// A nonzero flags byte at V+88 runs EmitViewSpriteRecords (0x00256690) as (E, V, the byte at V+6384 as a2). Then, when
// the byte at V+6378 is clear (read again after that call, which may change it), EmitTransformedParticleTriples
// (0x00257230) runs as (E, V). Returns nothing.
//
//   002621d0  addiu $sp, $sp, -64           frame of 64 bytes; ra, s0 and s1 are saved and restored (not reproduced)
//   002621e0  daddu $s1, $a0, $zero         s1 = E
//   002621e4  lbu $v1, 88($a1)
//   002621e8  bne $v1, $zero, 0x00262204    [daddu $s0, $a1, $zero]
//   002621f0  lbu $v1, 6378($s0)            byte 88 clear
//   002621f4  beq $v1, $zero, 0x00262214    [nop]
//   002621fc  beq $zero, $zero, 0x00262228  [ld $ra, 32($sp)]
//   00262204  jal 0x00256690                [lbu $a2, 6384($s0)]
//   0026220c  beq $zero, $zero, 0x002621f4  [lbu $v1, 6378($s0)]
//   00262214  daddu $a0, $s1, $zero
//   00262218  jal 0x00257230                [daddu $a1, $s0, $zero]
//   00262220  beq $zero, $zero, 0x002621fc  [nop]
//   00262228  lq $s1, 16($sp); lq $s0, 0($sp); jr $ra   [addiu $sp, $sp, 64]
//
// The native never changes ra, s0 or s1 and stores nothing itself. A call that reaches an unmapped byte is declined, so
// the original runs it and faults where it always did. Retired counts: 17 (byte 88 clear, byte 6378 set); 22 + the
// second callee (both clear); 20 + the first callee (byte 88 set, byte 6378 set); 25 + both callees.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00256690)
N3_CALLEE(00257230)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagsByte = 88U;           // V + 88: nonzero runs EmitViewSpriteRecords
constexpr GuestAddress kModeByte = 6384U;          // V + 6384: its mode argument (a2)
constexpr GuestAddress kParticleSkipByte = 6378U;  // V + 6378: clear runs EmitTransformedParticleTriples
constexpr std::uint32_t kFrameBytes = 64U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;       // 0x002621d0..0x002621ec: frame, saves, the byte 88 test
constexpr std::uint32_t kClearFlagCost = 1U;   // 0x002621f0: the byte 6378 load when the byte 88 is clear
constexpr std::uint32_t kSpriteCost = 4U;      // 0x00262204..0x00262210: jal, its delay slot, beq, the reload of 6378
constexpr std::uint32_t kTestCost = 2U;        // 0x002621f4..0x002621f8: the byte 6378 test and its delay slot
constexpr std::uint32_t kTriplesCost = 5U;     // 0x00262214..0x00262224: daddu, jal, daddu, beq and its delay slot
constexpr std::uint32_t kJoinCost = 2U;        // 0x002621fc..0x00262200: beq and the ld ra in its delay slot
constexpr std::uint32_t kEpilogueCost = 4U;    // 0x00262228..0x00262234: lq, lq, jr and the addiu in its delay slot

// lbu faults on an unmapped address, so a call that reaches one is left to the original, which faults itself.
template <class Memory>
bool mapped_byte(const Memory& memory, const GuestAddress address) {
    return memory.contains(address, 1U);
}

template <class Context>
n3::Run emit_emitter_records(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress view = call.arg(1);
    // The original copies a0 and a1 into s1 and s0 (daddu), and the second call passes them again. The first call may
    // have changed a0 and a1, so they come from these entry values.
    const std::uint64_t emitter = call.arg64(0);
    const std::uint64_t view_value = call.arg64(1);

    if (!mapped_byte(m, view + kFlagsByte)) {
        return call.decline();
    }
    const std::uint8_t flags = m.read_u8(view + kFlagsByte);
    std::uint32_t retired = kEntryCost;
    if (flags != 0U) {
        if (!mapped_byte(m, view + kModeByte)) {
            return call.decline();
        }
        call.set_arg64(2, m.read_u8(view + kModeByte));  // lbu a2: the jal's delay slot
        const n3::Run sprites = call.invoke([](auto& machine) { return n3::fn_00256690(machine); }, kFrameBytes);
        if (sprites.declined) {
            return call.decline();
        }
        retired += kSpriteCost + sprites.retired;
    } else {
        retired += kClearFlagCost;
    }

    // Read again: the sprite records may have changed the byte.
    if (!mapped_byte(m, view + kParticleSkipByte)) {
        return call.decline();
    }
    retired += kTestCost;
    if (m.read_u8(view + kParticleSkipByte) == 0U) {
        call.set_arg64(0, emitter);
        call.set_arg64(1, view_value);
        const n3::Run triples = call.invoke([](auto& machine) { return n3::fn_00257230(machine); }, kFrameBytes);
        if (triples.declined) {
            return call.decline();
        }
        retired += kTriplesCost + triples.retired;
    }
    return call.finish(retired + kJoinCost + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002621d0, 104U, 0x0c7d9ba7b9aac1b9ULL, "EmitEmitterRecords", kCeiling, emit_emitter_records,
         EngineReturn::Nothing)
