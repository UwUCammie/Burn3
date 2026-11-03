// 0x0019ebf0 InitEffectGroup (a0 = the group object o): resets an effect group. Four named assets are looked up in
// the global asset list (its pointer is at 0x00518688) and their objects are stored at o+64, +68, +72 and +76.
// Two sub-objects (o+464 and o+624) get constants, a VU0 quad with its x, y and z lanes cleared, and one more asset
// lookup (002fad90 in the listing). Two tables (o+560 and o+720) get constants and a VU0 xyz clear (00426030). The
// words of the global block 0x0067d490 that hold -1 are cleared, and so is its flag byte (00220fd0). Then the flag
// bytes are set, o+136 = 23, and the function returns 1.
//
// 002fad90, 00265c30, 00426030 and 00220fd0 are not native functions, so their bodies are written here with the
// instruction counts of their listings. The asset lookups are calls to strcasecmp (0x00126df0), which is native.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Guest addresses. The asset list pointer is `lui 0x52, -31096`, the sentinel block `lui 0x68, -11120`.
constexpr GuestAddress kAssetList = 0x00518688U;
constexpr GuestAddress kAssetNameA = 0x004b7328U;     // the names of the four asset fields, o+64 .. o+76
constexpr GuestAddress kAssetNameB = 0x004b7338U;
constexpr GuestAddress kAssetNameC = 0x004b7348U;
constexpr GuestAddress kAssetNameD = 0x004b7360U;
constexpr GuestAddress kSubAssetName = 0x004c4540U;   // the name 002fad90 looks up
constexpr GuestAddress kSentinels = 0x0067d490U;      // six words, then a flag byte at +24
constexpr GuestAddress kCurveFloat = 0x004f50a0U;     // `lui 0x4f, 20640`: the float 00426030 leaves in f0
constexpr std::int32_t kGpFloatSub = -32704;          // `lwc1 $f0, -32704($gp)` in 002fad90
constexpr std::int32_t kGpFloatTable = -29920;        // `lwc1 $f0, -29920($gp)` in 00426030

constexpr std::uint32_t kHalfBits = 0x3f000000U;      // 0.5f
constexpr std::uint32_t kOneBits = 0x3f800000U;       // 1.0f
constexpr std::uint8_t kLaneX = 8U;                   // destination masks of vaddx.x, vaddx.y and vaddx.z
constexpr std::uint8_t kLaneY = 4U;
constexpr std::uint8_t kLaneZ = 2U;
constexpr std::size_t kV0 = 2U;

// The strcasecmp frame bytes: 80 for a lookup made by InitEffectGroup, 80 more for one made inside 002fad90.
constexpr std::uint32_t kGroupFrame = 80U;
constexpr std::uint32_t kSubFrame = 160U;

// Retired instruction counts of the listing's blocks, delay slots included. A path adds the blocks it runs; each
// strcasecmp call adds its own count.
constexpr std::uint32_t kEntryCost = 12U;           // 0x0019ebf0..0x0019ec1c: prologue, o+132 = 0, first list pointer
constexpr std::uint32_t kTestCost = 4U;             // a loop test: lw count, slt, bne and its delay slot
constexpr std::uint32_t kBodyCost = 11U;            // a loop body with its strcasecmp call and the beq on the result
constexpr std::uint32_t kStepCost = 1U;             // the index step after an entry that does not match
constexpr std::uint32_t kMissCost = 1U;             // found = 0 once the list is exhausted
constexpr std::uint32_t kStoreAndReloadCost = 5U;   // sw of a field, then lui, lw (list pointer), beq and its delay slot
constexpr std::uint32_t kLastStoreCost = 3U;        // addiu a0, jal 002fad90, and the sw of o+76 in its delay slot
constexpr std::uint32_t kSubFixedCost = 39U;        // 002fad90 outside its lookup: 30 before the loop, 9 after it
constexpr std::uint32_t kLeafCallCost = 4U;         // jal 00265c30 and its delay slot, then jr and its delay slot
constexpr std::uint32_t kTableCallCost = 16U;       // jal 00426030 and its delay slot, then its 14 instructions
constexpr std::uint32_t kSentinelSetupCost = 6U;    // sw o+608, lui, sw o+612, addiu, jal 002fad90, sw o+616 (delay)
constexpr std::uint32_t kTailSetupCost = 7U;        // sw o+768, lui, lui, sw o+772, addiu, jal 00220fd0, sw o+776
constexpr std::uint32_t kSentinelBaseCost = 29U;    // 00220fd0 with no word at -1: six tests of 4, then 5
constexpr std::uint32_t kSentinelHitCost = 2U;      // per word at -1: the beq to its stub, and the stub's beq and sw
constexpr std::uint32_t kEpilogueCost = 29U;        // sb o+80, addiu, sw o+84, addiu, 17 sb, sw o+136, the epilogue

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is always
// the original's exact count. Every loop checks it.
constexpr std::uint32_t kCeiling = 262144U;

// One asset lookup: the object it found (0 when none matched), the count so far, and whether a strcasecmp declined.
struct Lookup {
    GuestAddress object{};
    std::uint32_t retired{};
    bool declined{};
};

// The VU0 macro ops as the interpreter runs them: the masked lanes of `dest` take the result and the MAC/STATUS flags
// are published (as ee_cop2::vadd_vector and vadd_broadcast do).

// VADDx.<mask>: FD.<mask> = FS + FT.<lane>
void vu0_add_lane(guest::R5900State& cpu, const unsigned dest, const unsigned left, const unsigned right,
                  const std::uint8_t mask, const std::size_t lane) {
    const guest::Vu0MacroFmacResult result = guest::execute_vu0_macro_broadcast_add_subtract(
        cpu.read_vu0_vector(dest), cpu.read_vu0_vector(left), cpu.read_vu0_vector(right), mask, lane, false);
    cpu.write_vu0_vector(dest, result.value);
    guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
}

// VADD.<mask>: FD.<mask> = FS + FT
void vu0_add(guest::R5900State& cpu, const unsigned dest, const unsigned left, const unsigned right,
             const std::uint8_t mask) {
    const guest::Vu0MacroFmacResult result = guest::execute_vu0_macro_vector_add_subtract(
        cpu.read_vu0_vector(dest), cpu.read_vu0_vector(left), cpu.read_vu0_vector(right), mask, false);
    cpu.write_vu0_vector(dest, result.value);
    guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
}

// The lookup the listing makes six times. The global list has a count at +8 and an entry array at +12 of 16-byte
// entries whose object is at +8; each object's name is at +168 and is compared with `name` by strcasecmp. Returns the
// first matching object. `retired` is the count before the lookup.
template <class Memory>
Lookup find_asset(n3::Call<Memory>& call, const GuestAddress name, const std::uint32_t frame,
                  const std::uint32_t retired) {
    auto& m = call.memory();
    const GuestAddress list = m.read_u32_le(kAssetList);
    const std::uint32_t count = m.read_u32_le(list + 8U);
    const GuestAddress entries = m.read_u32_le(list + 12U);
    Lookup result{0U, retired + kTestCost, false};
    for (std::uint32_t index = 0U; static_cast<std::int32_t>(index) < static_cast<std::int32_t>(count); ++index) {
        if (result.retired > kCeiling) {
            return result;
        }
        const GuestAddress object = m.read_u32_le(entries + 16U * index + 8U);
        call.set_arg(0, object + 168U);
        call.set_arg(1, name);
        const n3::Run compared = call.invoke([](auto& machine) { return n3::fn_00126df0(machine); }, frame);
        if (compared.declined) {
            return Lookup{0U, result.retired, true};
        }
        result.retired += kBodyCost + compared.retired;
        if (call.result() == 0U) {
            result.object = object;
            return result;
        }
        result.retired += kStepCost + kTestCost;
    }
    result.retired += kMissCost;
    return result;
}

// 002fad90 on the sub-object `sub`: 0.5f in +0..+12, the gp float in +16 and +20, 1.0f in +24, zero in +28. VF2 takes
// v0 with its low doubleword cleared (qmtc2 after daddu $v0, $zero, $zero). The quad at +32 goes through VF1, its x,
// y and z lanes become VF0 + VF2.x (one vaddx per lane, so zero), and the quad is stored back, its w lane unchanged.
// Then the zero byte at +48 and the lookup, whose object goes to +64.
template <class Memory>
Lookup sub_object_init(n3::Call<Memory>& call, const GuestAddress sub, const std::uint32_t retired) {
    auto& m = call.memory();
    guest::R5900State& cpu = call.cpu();
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    for (GuestAddress at = 0U; at < 16U; at += 4U) {
        m.write_u32_le(sub + at, kHalfBits);
    }
    const std::uint32_t gp_float = m.read_u32_le(gp + static_cast<GuestAddress>(kGpFloatSub));
    m.write_u32_le(sub + 16U, gp_float);
    m.write_u32_le(sub + 20U, gp_float);
    m.write_u32_le(sub + 24U, kOneBits);
    m.write_u32_le(sub + 28U, 0U);

    n3::Gpr128 zero_source = cpu.read_gpr(kV0);
    zero_source.words[0] = 0U;
    zero_source.words[1] = 0U;
    cpu.write_vu0_vector(2U, zero_source);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, sub + 32U));
    vu0_add_lane(cpu, 1U, 0U, 2U, kLaneX, 0U);
    vu0_add_lane(cpu, 1U, 0U, 2U, kLaneY, 0U);
    vu0_add_lane(cpu, 1U, 0U, 2U, kLaneZ, 0U);
    n3::store_quad_any(m, sub + 32U, cpu.read_vu0_vector(1U));

    m.write_u8(sub + 48U, 0U);
    const Lookup found = find_asset(call, kSubAssetName, kSubFrame, retired + kSubFixedCost);
    if (!found.declined) {
        m.write_u32_le(sub + 64U, found.object);
    }
    return found;
}

// 00426030 on the table `table`: VF1.xyz = VF0 + VF0 (w keeps VF1's value), VF1 is stored as the quad, then 0.5f twice,
// the gp float twice, zero, and the float at 0x004f50a0, which stays in f0.
template <class Memory>
void table_init(n3::Call<Memory>& call, const GuestAddress table) {
    auto& m = call.memory();
    guest::R5900State& cpu = call.cpu();
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    vu0_add(cpu, 1U, 0U, 0U, n3::kMaskXyz);
    const std::uint32_t gp_float = m.read_u32_le(gp + static_cast<GuestAddress>(kGpFloatTable));
    n3::store_quad_any(m, table, cpu.read_vu0_vector(1U));
    m.write_u32_le(table + 16U, kHalfBits);
    m.write_u32_le(table + 20U, kHalfBits);
    m.write_u32_le(table + 24U, gp_float);
    m.write_u32_le(table + 28U, gp_float);
    m.write_u32_le(table + 32U, 0U);
    const std::uint32_t curve = m.read_u32_le(kCurveFloat);
    m.write_u32_le(table + 36U, curve);
    cpu.write_fpr_bits(0U, curve);
}

// 00220fd0 on the sentinel block: each of its six words that holds -1 is cleared, then the flag byte at +24 is cleared.
// Returns how many words were cleared.
template <class Memory>
std::uint32_t clear_sentinels(Memory& m) {
    std::uint32_t cleared = 0U;
    for (GuestAddress offset = 0U; offset < 24U; offset += 4U) {
        if (m.read_u32_le(kSentinels + offset) == 0xffffffffU) {
            m.write_u32_le(kSentinels + offset, 0U);
            ++cleared;
        }
    }
    m.write_u8(kSentinels + 24U, 0U);
    return cleared;
}

template <class Context>
n3::Run init_effect_group(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress group = call.arg(0);
    m.write_u32_le(group + 132U, 0U);

    Lookup found = find_asset(call, kAssetNameA, kGroupFrame, kEntryCost);
    if (found.declined) {
        return call.decline();
    }
    m.write_u32_le(group + 64U, found.object);
    found = find_asset(call, kAssetNameB, kGroupFrame, found.retired + kStoreAndReloadCost);
    if (found.declined) {
        return call.decline();
    }
    m.write_u32_le(group + 68U, found.object);
    found = find_asset(call, kAssetNameC, kGroupFrame, found.retired + kStoreAndReloadCost);
    if (found.declined) {
        return call.decline();
    }
    m.write_u32_le(group + 72U, found.object);
    found = find_asset(call, kAssetNameD, kGroupFrame, found.retired + kStoreAndReloadCost);
    if (found.declined) {
        return call.decline();
    }
    m.write_u32_le(group + 76U, found.object);
    std::uint32_t retired = found.retired + kLastStoreCost;
    if (retired > kCeiling) {
        return call.finish(retired);
    }

    Lookup sub = sub_object_init(call, group + 464U, retired);
    if (sub.declined) {
        return call.decline();
    }
    retired = sub.retired + kLeafCallCost;
    table_init(call, group + 560U);
    retired += kTableCallCost;
    m.write_u32_le(group + 608U, 0U);
    m.write_u32_le(group + 612U, 0U);
    m.write_u32_le(group + 616U, kOneBits);
    retired += kSentinelSetupCost;

    sub = sub_object_init(call, group + 624U, retired);
    if (sub.declined) {
        return call.decline();
    }
    retired = sub.retired + kLeafCallCost;
    table_init(call, group + 720U);
    retired += kTableCallCost;
    m.write_u32_le(group + 768U, 0U);
    m.write_u32_le(group + 772U, 0U);
    m.write_u32_le(group + 776U, kOneBits);
    retired += kTailSetupCost;

    const std::uint32_t cleared = clear_sentinels(m);
    retired += kSentinelBaseCost + kSentinelHitCost * cleared;

    m.write_u8(group + 80U, 0U);
    m.write_u32_le(group + 84U, 0U);
    for (GuestAddress flag = 93U; flag <= 99U; ++flag) {
        m.write_u8(group + flag, 1U);
    }
    m.write_u8(group + 100U, 0U);
    for (GuestAddress flag = 101U; flag <= 108U; ++flag) {
        m.write_u8(group + flag, 1U);
    }
    m.write_u8(group + 92U, 1U);
    m.write_u32_le(group + 136U, 23U);
    call.return_value(1U);
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0019ebf0, 592U, 0xbe5af4c05b975567ULL, "InitEffectGroup", kCeiling, init_effect_group,
         EngineReturn::Integer)
