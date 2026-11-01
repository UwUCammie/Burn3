// 0x004d45e0 (3388 bytes, 847 instructions, one path): InitGlobalRecordTables. a0-a3 are not read; the function
// constructs the game's global record tables and returns nothing the caller uses.
//
// Seven groups of records are built in fixed global memory, each record with a vtable word at its head (and for the
// two 17808-byte blocks also 84 32-byte entries whose w lane is cleared):
//
//   * 2 blocks of 17808 bytes at 0x0057baf0 and 0x00580080 (0x004d4600..0x004d4904 loops back while
//     $s0 != 0x00584610),
//   * 5 records of 5504 bytes from 0x00584610 (0x004d4908..0x004d4a50),
//   * 5 records of 5472 bytes from 0x0058b190 (0x004d4a54..0x004d4b9c),
//   * 64 records of 9264 bytes from 0x00591c70 (0x004d4ba0..0x004d4da0),
//   * 32 records of 1920 bytes from 0x00622870 (0x004d4da4..0x004d4e1c),
//   * 64 records of 1248 bytes from 0x00631870 (0x004d4e20..0x004d4e60),
//   * 5 records of 112 bytes from 0x006482e0 (0x004d4e64..0x004d4ea0).
//
// Then the node records: one absolute head word at 0x0064ce40, 2 of 10192 bytes from 0x0064d3b0, 5 of 9312 bytes
// from 0x00652350 and 5 of 6816 bytes from 0x0065d930, each with three (or two) head words at record + 6416:
// 0x0064d3b0 gets 0x004dde50 / 0x004dddc0 / 0x004dde20, 0x00652350 and 0x0065d930 get 0x004dde50 and 0x004dddc0,
// and 0x0065d930's last one is 0x004dddf0.
//
// Every loop in the listing is a pointer walk with constant bounds (no memory read feeds a branch), so the whole
// function is one path: 89164 retired instructions, delay slots and callees included. The other work is pointer
// bumps with four nops per iteration, counted here but not run.
//
// Unusual: the only `jal` targets are 0x0019b6b0, 0x0019b6c0 and 0x0019b6d0, all three the same two-instruction
// leaf (`jr $ra; daddu $v0, $a0, $zero`): they return their argument and touch no memory, so they are inlined and
// their two instructions counted (542 calls, 1084 instructions). 0x004d47c8 is a VU0 macro loop: load the
// quadword, `vsubw.w vf1,vf0,vf0w` then `vaddx.w vf1,vf1,vf2x` (VF2 was loaded from $f20 = 0 with qmtc2) clear
// the w lane, and store it back.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// Record bases and strides, as the listing builds them (lui + addiu; the constants it compares against).
constexpr GuestAddress kBlockBase = 0x0057baf0U;      // lui 0x0058, addiu -17680
constexpr GuestAddress kBlockStride = 17808U;
constexpr GuestAddress kSceneBase = 0x00584610U;      // lui 0x0058, addiu 17936
constexpr GuestAddress kSceneStride = 5504U;
constexpr GuestAddress kGroupBase = 0x0058b190U;      // lui 0x0059, addiu -20080
constexpr GuestAddress kGroupStride = 5472U;
constexpr GuestAddress kMeshBase = 0x00591c70U;       // lui 0x0059, addiu 7280
constexpr GuestAddress kMeshStride = 9264U;
constexpr GuestAddress kNodeBase = 0x00622870U;       // lui 0x0062, addiu 10352
constexpr GuestAddress kNodeStride = 1920U;
constexpr GuestAddress kLeafBase = 0x00631870U;       // lui 0x0063, addiu 6256
constexpr GuestAddress kLeafStride = 1248U;
constexpr GuestAddress kItemBase = 0x006482e0U;       // lui 0x0065, addiu -32032
constexpr GuestAddress kItemStride = 112U;
constexpr GuestAddress kSlotBase = 0x0064d3b0U;       // lui 0x0065, addiu -11344
constexpr GuestAddress kSlotStride = 10192U;
constexpr GuestAddress kEntryBase = 0x00652350U;      // lui 0x0065, addiu 9040
constexpr GuestAddress kEntryStride = 9312U;
constexpr GuestAddress kChainBase = 0x0065d930U;      // lui 0x0066, addiu -9936
constexpr GuestAddress kChainStride = 6816U;
constexpr GuestAddress kHeadWord = 0x0064ce40U;       // lui 0x0065, sw -12736($v1)
constexpr GuestAddress kSlotTail = 6416U;             // 6416($t1) / 6416($a0) / 6416($v1)

// The vtable words the listing stores at the head of each record (addresses it takes with lui + addiu).
constexpr std::uint32_t kVtableBlockHead = 0x004ddee0U;   // 0x004e0000 - 8480
constexpr std::uint32_t kVtableBlockBody = 0x004ddf50U;   // 0x004e0000 - 8368
constexpr std::uint32_t kVtableBlockTail = 0x004ddf00U;   // 0x004e0000 - 8448
constexpr std::uint32_t kVtableSceneTail = 0x004ddf70U;   // 0x004e0000 - 8336
constexpr std::uint32_t kVtableGroupTail = 0x004de130U;   // 0x004e0000 - 7888
constexpr std::uint32_t kVtableMeshTail = 0x004de110U;    // 0x004e0000 - 7920
constexpr std::uint32_t kVtableNode = 0x004ddf30U;        // 0x004e0000 - 8400
constexpr std::uint32_t kVtableLeaf = 0x004de550U;        // 0x004e0000 - 6832
constexpr std::uint32_t kVtableHead = 0x004dfb60U;        // 0x004e0000 - 1184
constexpr std::uint32_t kVtableSlot = 0x004dde50U;        // 0x004e0000 - 8624
constexpr std::uint32_t kVtableEntry = 0x004dddc0U;       // 0x004e0000 - 8768
constexpr std::uint32_t kVtableSlotAlt = 0x004dde20U;     // 0x004e0000 - 8672
constexpr std::uint32_t kVtableChain = 0x004dddf0U;       // 0x004e0000 - 8720

// Instruction costs. Every walk in the listing is `addiu $r,$r,step`, four nops, `bne $r,$limit` and its delay
// slot: seven instructions an iteration, and nothing outside the registers changes.
constexpr std::uint32_t kPointerLoopCost = 7U;
// The nested form: `daddu $r,$base,$zero` + `addiu $limit,$base,48`, three inner iterations, then
// `addiu $base,$base,64` + `bne` + delay slot.
constexpr std::uint32_t kNestedLoopCost = 2U + 3U * kPointerLoopCost + 3U;   // 26
// A `jal` to one of the two-instruction identity leaves costs its two callee instructions on top of the jal and
// its delay slot, which the block counts already include.
constexpr std::uint32_t kStubCost = 2U;

// The one path of the function, delay slots and inlined callees included.
constexpr std::uint32_t kRetired = 89164U;

template <class Context>
n3::Run init_global_record_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    std::uint32_t retired = 8U;   // 0x004d45e0..0x004d45fc: prologue ($s0 = 0x0057baf0, $f20 = 0)

    // ---- 0x004d4600..0x004d4904: the 17808-byte block, twice (0x004d4900 compares $s0 with 0x00584610) ----
    for (std::uint32_t block = 0U; block < 2U; ++block) {
        const GuestAddress base = kBlockBase + block * kBlockStride;

        // 0x004d4600..0x004d4684 (34): head words and 13 inlined stub calls ($a0 = base + 16, 64, 112, 128, 144,
        // 160, 352, 368, 384, 416, 432, 464, 480).
        m.write_u32_le(base, kVtableBlockHead);              // 0x004d4610  sw $v0, 0($s0)
        m.write_u32_le(base, kVtableBlockBody);              // 0x004d4680  sw $v0, 0($s0)
        retired += 34U + 13U * kStubCost;
        // 0x004d4688..0x004d46a0: base+1344 to base+1696, 16 bytes, 22 iterations.
        retired += 22U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d46a4  addiu $s1,$s0,2080
        // 0x004d46a8..0x004d46c0: base+2080 to base+3232, 192 bytes, one stub call each; 6 iterations.
        retired += 6U * (kPointerLoopCost + kStubCost);
        retired += 1U;                                      // 0x004d46c4  addiu $s1,$s0,3296
        // 0x004d46c8..0x004d46e4 (8): base+3296 to base+4448, 192 bytes, two stub calls; 6 iterations.
        retired += 6U * (8U + 2U * kStubCost);
        retired += 2U;                                      // 0x004d46e8, 0x004d46ec: base+4464, base+4528
        // 0x004d46f0..0x004d4708: base+4464 to base+4528, 32 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d470c, 0x004d4710: base+4560, base+4944
        // 0x004d4714..0x004d4740: 6 records of 64 bytes from base+4560, each 3 entries of 16 bytes.
        retired += 6U * kNestedLoopCost;
        retired += 5U;                                      // 0x004d4744..0x004d4754: $v0 = 0x004ddf00 and the head word at base
        m.write_u32_le(base, kVtableBlockTail);              // 0x004d4754  sw $v0, 0($s0)
        // 0x004d4758..0x004d4784: base+5520 to base+8208, 64 bytes, 42 records.
        retired += 42U * kNestedLoopCost;
        retired += 1U;                                      // 0x004d4788  addiu $a0,$s0,10896
        // 0x004d478c..0x004d47b8: base+8208 to base+10896, 64 bytes, 42 records.
        retired += 42U * kNestedLoopCost;
        // 0x004d47bc..0x004d47c4: mfc1 $v1,$f20, addiu $v0,$s0,13584, qmtc2 $v1,$vf2.
        call.cpu().write_gpr_sign_extended_32(3U, 0U);       // mfc1 $v1,$f20 ($f20 was set to +0.0 at entry)
        call.cpu().write_vu0_vector(2U, call.cpu().read_gpr(3U));   // qmtc2 $v1,$vf2
        retired += 3U;
        // 0x004d47c8..0x004d47e0: base+10896 to base+13584, 32 bytes, 84 entries; clear each quadword's w lane.
        for (std::uint32_t entry = 0U; entry < 84U; ++entry) {
            const GuestAddress address = base + 10896U + entry * 32U;
            call.cpu().write_vu0_vector(1U, n3::load_quad_any(m, address));          // lqc2 $vf1,0($a0)
            ee_cop2::vadd_broadcast(call.cpu(), 1U, 0U, 0U, n3::kMaskW, 3U, true);  // vsubw.w $vf1,$vf0,$vf0w
            ee_cop2::vadd_broadcast(call.cpu(), 1U, 1U, 2U, n3::kMaskW, 0U, false); // vaddx.w $vf1,$vf1,$vf2x
            n3::store_quad_any(m, address, call.cpu().read_vu0_vector(1U));         // sqc2 $vf1,0($a0)
        }
        retired += 84U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d47e4, 0x004d47e8: base+13632, base+14016
        // 0x004d47ec..0x004d4804: base+13632 to base+14016, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4808  addiu $v0,$s0,14400
        // 0x004d480c..0x004d4824: base+14016 to base+14400, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4828, 0x004d482c: base+14480, base+14512
        // 0x004d4830..0x004d4848: base+14480 to base+14512, 16 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d484c, 0x004d4850: base+14528, base+14720
        // 0x004d4854..0x004d486c: base+14528 to base+14720, 48 bytes, 4 iterations.
        retired += 4U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4870, 0x004d4874: base+14912, base+15424
        // 0x004d4878..0x004d4890: base+14912 to base+15424, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4894  addiu $v0,$s0,15936
        // 0x004d4898..0x004d48b0: base+15424 to base+15936, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d48b4  addiu $v1,$s0,16704
        // 0x004d48b8..0x004d48d0: base+15936 to base+16704, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d48d4  addiu $v0,$s0,17472
        // 0x004d48d8..0x004d48f0: base+16704 to base+17472, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 5U;                                      // 0x004d48f4..0x004d4904: $s1 = 0x00584610, next block
    }

    // ---- 0x004d4908..0x004d4a50: 5 records of 5504 bytes from 0x00584610 ----
    for (std::uint32_t record = 0U; record < 5U; ++record) {
        const GuestAddress base = kSceneBase + record * kSceneStride;
        // 0x004d4908..0x004d4954 (20): head words and 6 inlined stub calls ($a0 = base + 16, 32, 48, 64, 80, 96).
        m.write_u32_le(base, kVtableBlockHead);              // 0x004d4918  sw $v0, 0($s1)
        m.write_u32_le(base, kVtableBlockBody);              // 0x004d4950  sw $v0, 0($s1)
        retired += 20U + 6U * kStubCost;
        // 0x004d4958..0x004d4970: base+1344 to base+1696, 16 bytes, 22 iterations.
        retired += 22U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4974  addiu $s0,$s1,2080
        // 0x004d4978..0x004d4990: base+2080 to base+3232, 192 bytes, one stub call each; 6 iterations.
        retired += 6U * (kPointerLoopCost + kStubCost);
        retired += 1U;                                      // 0x004d4994  addiu $s0,$s1,3296
        // 0x004d4998..0x004d49d4 (16): base+3296 to base+4448, 192 bytes, six stub calls; 6 iterations.
        retired += 6U * (16U + 6U * kStubCost);
        retired += 2U;                                      // 0x004d49d8, 0x004d49dc: base+4464, base+4528
        // 0x004d49e0..0x004d49f8: base+4464 to base+4528, 32 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d49fc, 0x004d4a00: base+4560, base+4944
        // 0x004d4a04..0x004d4a30: 6 records of 64 bytes from base+4560, each 3 entries of 16 bytes.
        retired += 6U * kNestedLoopCost;
        retired += 8U;                                      // 0x004d4a34..0x004d4a50: head word, next record
        m.write_u32_le(base, kVtableSceneTail);              // 0x004d4a44  sw $v0, 0($s1)
    }

    // ---- 0x004d4a54..0x004d4b9c: 5 records of 5472 bytes from 0x0058b190 ----
    for (std::uint32_t record = 0U; record < 5U; ++record) {
        const GuestAddress base = kGroupBase + record * kGroupStride;
        // 0x004d4a54..0x004d4aa0 (20): head words and 6 inlined stub calls ($a0 = base + 16, 32, 48, 64, 80, 96).
        m.write_u32_le(base, kVtableBlockHead);              // 0x004d4a64  sw $v0, 0($s0)
        m.write_u32_le(base, kVtableBlockBody);              // 0x004d4a9c  sw $v0, 0($s0)
        retired += 20U + 6U * kStubCost;
        // 0x004d4aa4..0x004d4abc: base+1344 to base+1696, 16 bytes, 22 iterations.
        retired += 22U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4ac0  addiu $s1,$s0,2080
        // 0x004d4ac4..0x004d4adc (7): base+2080 to base+3232, 192 bytes, one stub call each; 6 iterations.
        retired += 6U * (kPointerLoopCost + kStubCost);
        retired += 1U;                                      // 0x004d4ae0  addiu $s1,$s0,3296
        // 0x004d4ae4..0x004d4b20 (16): base+3296 to base+4448, 192 bytes, six stub calls; 6 iterations.
        retired += 6U * (16U + 6U * kStubCost);
        retired += 2U;                                      // 0x004d4b24, 0x004d4b28: base+4464, base+4528
        // 0x004d4b2c..0x004d4b44: base+4464 to base+4528, 32 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4b48, 0x004d4b4c: base+4560, base+4944
        // 0x004d4b50..0x004d4b7c: 6 records of 64 bytes from base+4560, each 3 entries of 16 bytes.
        retired += 6U * kNestedLoopCost;
        retired += 8U;                                      // 0x004d4b80..0x004d4b9c: head word, next record
        m.write_u32_le(base, kVtableGroupTail);              // 0x004d4b90  sw $v1, 0($s0)
    }

    // ---- 0x004d4ba0..0x004d4da0: 64 records of 9264 bytes from 0x00591c70 ----
    retired += 6U;                                          // 0x004d4ba0..0x004d4bb4: $a2, $v1 and $t0, the head word the loop stores
    for (std::uint32_t record = 0U; record < 64U; ++record) {
        const GuestAddress base = kMeshBase + record * kMeshStride;
        // 0x004d4bb8..0x004d4bc0: the head word at base and the pointers of the first pointer walk.
        m.write_u32_le(base, kVtableBlockBody);              // 0x004d4bb8  sw $t0, 0($a0)
        retired += 3U;
        // 0x004d4bc4..0x004d4bdc: base+1344 to base+1696, 16 bytes, 22 iterations.
        retired += 22U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4be0, 0x004d4be4: base+2080, base+3232
        // 0x004d4be8..0x004d4c00: base+2080 to base+3232, 192 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4c04, 0x004d4c08: base+3296, base+4448
        // 0x004d4c0c..0x004d4c24: base+3296 to base+4448, 192 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4c28, 0x004d4c2c: base+4464, base+4528
        // 0x004d4c30..0x004d4c48: base+4464 to base+4528, 32 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4c4c, 0x004d4c50: base+4560, base+4944
        // 0x004d4c54..0x004d4c80: 6 records of 64 bytes from base+4560, each 3 entries of 16 bytes.
        retired += 6U * kNestedLoopCost;
        // 0x004d4c84..0x004d4c8c: the second head word and the next two pointer walks' bounds.
        m.write_u32_le(base, kVtableMeshTail);               // 0x004d4c84  sw $a2, 0($a0)
        retired += 3U;
        // 0x004d4c90..0x004d4ca8: base+5088 to base+5472, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4cac  addiu $a1,$a0,5856
        // 0x004d4cb0..0x004d4cc8: base+5472 to base+5856, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4ccc, 0x004d4cd0: base+5936, base+5968
        // 0x004d4cd4..0x004d4cec: base+5936 to base+5968, 16 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4cf0, 0x004d4cf4: base+5984, base+6176
        // 0x004d4cf8..0x004d4d10: base+5984 to base+6176, 48 bytes, 4 iterations.
        retired += 4U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4d14, 0x004d4d18: base+6368, base+6880
        // 0x004d4d1c..0x004d4d34: base+6368 to base+6880, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4d38  addiu $a1,$a0,7392
        // 0x004d4d3c..0x004d4d54: base+6880 to base+7392, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4d58  addiu $a3,$a0,8160
        // 0x004d4d5c..0x004d4d74: base+7392 to base+8160, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4d78  addiu $a1,$a0,8928
        // 0x004d4d7c..0x004d4d94: base+8160 to base+8928, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 3U;                                      // 0x004d4d98..0x004d4da0: next record
    }

    // ---- 0x004d4da4..0x004d4e1c: 32 records of 1920 bytes from 0x00622870 ----
    retired += 4U;                                          // 0x004d4da4..0x004d4db0: $t1 and $a2, the head word the loop stores
    for (std::uint32_t record = 0U; record < 32U; ++record) {
        const GuestAddress base = kNodeBase + record * kNodeStride;
        // 0x004d4db4..0x004d4dbc: the head word at base and the pointers of the first pointer walk.
        m.write_u32_le(base, kVtableNode);                   // 0x004d4db4  sw $a2, 0($v1)
        retired += 3U;
        // 0x004d4dc0..0x004d4dec: 12 records of 64 bytes from base+576, each 3 entries of 16 bytes.
        retired += 12U * kNestedLoopCost;
        retired += 2U;                                      // 0x004d4df0, 0x004d4df4: base+1648, base+1776
        // 0x004d4df8..0x004d4e10: base+1648 to base+1776, 16 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 3U;                                      // 0x004d4e14..0x004d4e1c: next record
    }

    // ---- 0x004d4e20..0x004d4e60: 64 records of 1248 bytes from 0x00631870 ----
    retired += 4U;                                          // 0x004d4e20..0x004d4e2c: $v1 and $a1, the head word the loop stores
    for (std::uint32_t record = 0U; record < 64U; ++record) {
        const GuestAddress base = kLeafBase + record * kLeafStride;
        // 0x004d4e30..0x004d4e38: the head word at base and the pointers of the pointer walk.
        m.write_u32_le(base, kVtableLeaf);                   // 0x004d4e30  sw $a1, 0($t1)
        retired += 3U;
        // 0x004d4e3c..0x004d4e54: base+960 to base+1088, 16 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 3U;                                      // 0x004d4e58..0x004d4e60: next record
    }

    // ---- 0x004d4e64..0x004d4ea0: 5 records of 112 bytes from 0x006482e0 (pointer walks only) ----
    retired += 4U;                                          // 0x004d4e64..0x004d4e70: $a2, $v1
    for (std::uint32_t record = 0U; record < 5U; ++record) {
        // 0x004d4e74, 0x004d4e78: the walk's start and its bound.
        retired += 2U;
        // 0x004d4e7c..0x004d4e94: record+0 to record+96, 16 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 3U;                                      // 0x004d4e98..0x004d4ea0: next record
    }

    // ---- 0x004d4ea4..0x004d4ee4: one absolute head word, then the first node record ----
    retired += 14U;                                         // 0x004d4ea4..0x004d4ed8
    m.write_u32_le(kHeadWord, kVtableHead);                  // 0x004d4eb4  sw $a0, -12736($v1)

    // ---- 0x004d4edc..0x004d5044: 2 records of 10192 bytes from 0x0064d3b0, head words at record + 6416 ----
    for (std::uint32_t record = 0U; record < 2U; ++record) {
        const GuestAddress base = kSlotBase + record * kSlotStride;
        // 0x004d4edc..0x004d4ee4: the first head word and the pointers of the first pointer walk.
        m.write_u32_le(base + kSlotTail, kVtableSlot);       // 0x004d4edc  sw $a3, 6416($t1)
        retired += 3U;
        // 0x004d4ee8..0x004d4f00: base+128 to base+512, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4f04  addiu $a1,$t1,896
        // 0x004d4f08..0x004d4f20: base+512 to base+896, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4f24, 0x004d4f28: base+976, base+1008
        // 0x004d4f2c..0x004d4f44: base+976 to base+1008, 16 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4f48, 0x004d4f4c: base+1024, base+1216
        // 0x004d4f50..0x004d4f68: base+1024 to base+1216, 48 bytes, 4 iterations.
        retired += 4U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d4f6c, 0x004d4f70: base+1408, base+1920
        // 0x004d4f74..0x004d4f8c: base+1408 to base+1920, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4f90  addiu $a1,$t1,2432
        // 0x004d4f94..0x004d4fac: base+1920 to base+2432, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4fb0  addiu $t0,$t1,3200
        // 0x004d4fb4..0x004d4fcc: base+2432 to base+3200, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d4fd0  addiu $a1,$t1,3968
        // 0x004d4fd4..0x004d4fec: base+3200 to base+3968, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        // 0x004d4ff0..0x004d4ff8: the second head word and the next pointer walks' bounds.
        m.write_u32_le(base + kSlotTail, kVtableEntry);      // 0x004d4ff0  sw $a2, 6416($t1)
        retired += 3U;
        // 0x004d4ffc..0x004d5014: base+6656 to base+6752, 16 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5018  addiu $a1,$t1,6848
        // 0x004d501c..0x004d5034: base+6752 to base+6848, 16 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        // 0x004d5038..0x004d5044: the third head word and the next record.
        m.write_u32_le(base + kSlotTail, kVtableSlotAlt);    // 0x004d5038  sw $v1, 6416($t1)
        retired += 4U;
    }

    // ---- 0x004d5048..0x004d51c4: 5 records of 9312 bytes from 0x00652350, head words at record + 6416 ----
    retired += 6U;                                          // 0x004d5048..0x004d505c
    for (std::uint32_t record = 0U; record < 5U; ++record) {
        const GuestAddress base = kEntryBase + record * kEntryStride;
        // 0x004d5060..0x004d5068: the first head word and the pointers of the first pointer walk.
        m.write_u32_le(base + kSlotTail, kVtableSlot);       // 0x004d5060  sw $a3, 6416($a0)
        retired += 3U;
        // 0x004d506c..0x004d5084: base+128 to base+512, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5088  addiu $a1,$a0,896
        // 0x004d508c..0x004d50a4: base+512 to base+896, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d50a8, 0x004d50ac: base+976, base+1008
        // 0x004d50b0..0x004d50c8: base+976 to base+1008, 16 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d50cc, 0x004d50d0: base+1024, base+1216
        // 0x004d50d4..0x004d50ec: base+1024 to base+1216, 48 bytes, 4 iterations.
        retired += 4U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d50f0, 0x004d50f4: base+1408, base+1920
        // 0x004d50f8..0x004d5110: base+1408 to base+1920, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5114  addiu $a1,$a0,2432
        // 0x004d5118..0x004d5130: base+1920 to base+2432, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5134  addiu $t0,$a0,3200
        // 0x004d5138..0x004d5150: base+2432 to base+3200, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5154  addiu $a1,$a0,3968
        // 0x004d5158..0x004d5170: base+3200 to base+3968, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        // 0x004d5174..0x004d517c: the second head word and the next pointer walks' bounds.
        m.write_u32_le(base + kSlotTail, kVtableEntry);      // 0x004d5174  sw $a2, 6416($a0)
        retired += 3U;
        // 0x004d5180..0x004d5198: base+6656 to base+6752, 16 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d519c  addiu $a1,$a0,6848
        // 0x004d51a0..0x004d51b8: base+6752 to base+6848, 16 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 3U;                                      // 0x004d51bc..0x004d51c4: next record
    }

    // ---- 0x004d51c8..0x004d5300: 5 records of 6816 bytes from 0x0065d930, head words at record + 6416 ----
    retired += 6U;                                          // 0x004d51c8..0x004d51dc
    for (std::uint32_t record = 0U; record < 5U; ++record) {
        const GuestAddress base = kChainBase + record * kChainStride;
        // 0x004d51e0..0x004d51e8: the first head word and the pointers of the first pointer walk.
        m.write_u32_le(base + kSlotTail, kVtableSlot);       // 0x004d51e0  sw $a3, 6416($v1)
        retired += 3U;
        // 0x004d51ec..0x004d5204: base+128 to base+512, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5208  addiu $a2,$v1,896
        // 0x004d520c..0x004d5224: base+512 to base+896, 64 bytes, 6 iterations.
        retired += 6U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d5228, 0x004d522c: base+976, base+1008
        // 0x004d5230..0x004d5248: base+976 to base+1008, 16 bytes, 2 iterations.
        retired += 2U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d524c, 0x004d5250: base+1024, base+1216
        // 0x004d5254..0x004d526c: base+1024 to base+1216, 48 bytes, 4 iterations.
        retired += 4U * kPointerLoopCost;
        retired += 2U;                                      // 0x004d5270, 0x004d5274: base+1408, base+1920
        // 0x004d5278..0x004d5290: base+1408 to base+1920, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d5294  addiu $a2,$v1,2432
        // 0x004d5298..0x004d52b0: base+1920 to base+2432, 64 bytes, 8 iterations.
        retired += 8U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d52b4  addiu $t0,$v1,3200
        // 0x004d52b8..0x004d52d0: base+2432 to base+3200, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        retired += 1U;                                      // 0x004d52d4  addiu $a2,$v1,3968
        // 0x004d52d8..0x004d52f0: base+3200 to base+3968, 64 bytes, 12 iterations.
        retired += 12U * kPointerLoopCost;
        // 0x004d52f4..0x004d5300: the second head word and the next record.
        m.write_u32_le(base + kSlotTail, kVtableChain);      // 0x004d52f4  sw $a1, 6416($v1)
        retired += 4U;
    }

    // 0x004d5304..0x004d5318: epilogue (the frame and $f20 are restored; this native never touched them).
    retired += 6U;

    // $v0 is left by the last inlined stub call: $a0 was the sixth entry of the last group record,
    // 0x0058b190 + 4 * 5472 + 3296 + 5 * 192 + 80 = 0x00591800.
    call.return_value(kGroupBase + 4U * kGroupStride + 3296U + 5U * 192U + 80U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(004d45e0, 3388U, 0xfd54544ddb915fb3ULL, "InitGlobalRecordTables", kRetired,
               init_global_record_tables, EngineReturn::Integer)
