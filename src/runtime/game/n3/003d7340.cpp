// 0x003d7340 (a0 = P0, a1 = P1, a2 = u16 key stream ending at a zero key, a3 = S): a leaf that returns its count in
// v0; the caller at 0x003d7970 reads it (daddu $s5, $v0 ; blez $s5).
// The table pointer at 0x00665ed0 gives float scales at +8 and +12, the chain end pointer at +28, and 128 bucket
// heads at +32 chosen by key & 0x7f. Entries are 32 bytes: floats at +0..+24 and the u16 key at +28. Each key's
// chain is searched (its end counts as found). When the entry's float at +0 is not negative, one quad is written:
// 10 words at the stream at 0x0066eb20 and 10 at 0x0066fb00, both streams moving on by 40 bytes. Each later key
// first moves both streams on by 8 bytes (repeating their last two words) and advances S136 (the stack word that
// starts as P1[0]). v0 counts 5 per quad and 1 per later key, and the loop stops before a quad would take it to 509
// or more. f0 ends as P1[4]. Temporaries (a0-a3, t0-t2, v1, at) are not reproduced; no caller can rely on them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTableGlobal = 0x00665ed0U;   // lui 0x66, lw +24272: the table pointer
constexpr GuestAddress kQuadsA = 0x0066eb20U;        // lui 0x67, addiu -5344
constexpr GuestAddress kQuadsB = 0x0066fb00U;        // lui 0x67, addiu -1280
constexpr std::int32_t kGlobalFloatOffset = -31812;  // lwc1 from $gp
constexpr std::uint32_t kTwentyFive = 0x41c80000U;   // 25.0f (lui 0x41c8, then mtc1 into f2)
constexpr std::uint32_t kZero = 0U;                  // f2 from 0x003d7418 on
constexpr std::uint32_t kCountLimit = 509U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of each block of the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 13U;      // 0x003d7340-0x003d7370
constexpr std::uint32_t kKeyTestCost = 4U;     // 0x003d7384-0x003d7390, 0x003d7694-0x003d76a0
constexpr std::uint32_t kEndTestCost = 3U;     // 0x003d7374-0x003d737c, 0x003d7684-0x003d768c
constexpr std::uint32_t kStepCost = 1U;        // 0x003d7380, 0x003d7690
constexpr std::uint32_t kSetupCost = 39U;      // 0x003d7394-0x003d742c
constexpr std::uint32_t kSampleCost = 1U;      // 0x003d7430
constexpr std::uint32_t kSignCost = 3U;        // 0x003d7434-0x003d743c
constexpr std::uint32_t kNegativeCost = 1U;    // 0x003d7440, only when the sample is negative
constexpr std::uint32_t kSignBranchCost = 3U;  // 0x003d7444-0x003d744c
constexpr std::uint32_t kLimitCost = 4U;       // 0x003d7450-0x003d745c
constexpr std::uint32_t kQuadCost = 112U;      // 0x003d7460-0x003d761c
constexpr std::uint32_t kKeyAdvanceCost = 4U;  // 0x003d7620-0x003d762c
constexpr std::uint32_t kShadowCost = 21U;     // 0x003d7630-0x003d7680
constexpr std::uint32_t kMatchCost = 2U;       // 0x003d76a4-0x003d76a8
constexpr std::uint32_t kReturnCost = 2U;      // 0x003d76ac-0x003d76b0

// The per-call float terms: f0, f1, f3, f4 and f5 of the listing.
struct Terms {
    std::uint32_t y1{};       // f0: P1[4]
    std::uint32_t scale_x{};  // f1: (TBL[8] * P0[0]) * 25
    std::uint32_t scale_y{};  // f3: (TBL[12] * P0[4]) * 25
    std::uint32_t drop{};     // f4: gp float * P0[0]
    std::uint32_t top{};      // f5: S[4] + S[12]
};

struct Emission {
    std::uint32_t retired{};
    std::uint32_t count{};  // v0 at the return
    std::uint32_t f0{};     // f0 at the return: P1[4]
};

// 0x003d7598-0x003d75f4: word = ACC + drop * (top - y), with ACC = 0 + word (ADDA.S, then MADD.S).
template <class Memory>
void blend_word(Memory& m, const GuestAddress word, const GuestAddress y_word, const Terms& k) {
    const std::uint32_t d = fpu::sub(k.top, m.read_u32_le(y_word));
    const std::uint32_t acc = fpu::add(kZero, m.read_u32_le(word));
    m.write_u32_le(word, fpu::madd(acc, k.drop, d));
}

// 0x003d7460-0x003d761c: one quad, 10 words at the A stream (a) and 10 at the B stream (b). Every read and store
// keeps the listing's order, so overlapping streams behave as they do in the original.
template <class Memory>
void emit_quad(Memory& m, const GuestAddress entry, const GuestAddress a, const GuestAddress b,
               const std::uint32_t s136, const Terms& k) {
    const std::uint32_t pa = fpu::add(s136, fpu::mul(m.read_u32_le(entry + 16U), k.scale_x));
    const std::uint32_t qa = fpu::add(k.y1, fpu::mul(m.read_u32_le(entry + 20U), k.scale_y));
    m.write_u32_le(a + 24U, pa);
    m.write_u32_le(a + 28U, qa);
    const std::uint32_t e0 = m.read_u32_le(entry + 0U);
    const std::uint32_t e4 = m.read_u32_le(entry + 4U);
    m.write_u32_le(b + 24U, e0);
    m.write_u32_le(b + 28U, e4);
    const std::uint32_t e8 = m.read_u32_le(entry + 8U);
    const std::uint32_t e12 = m.read_u32_le(entry + 12U);
    const std::uint32_t pb = fpu::add(m.read_u32_le(a + 24U), fpu::mul(k.scale_x, e8));
    const std::uint32_t qb = fpu::add(m.read_u32_le(a + 28U), fpu::mul(k.scale_y, e12));
    m.write_u32_le(a + 16U, pb);
    m.write_u32_le(a + 20U, qb);
    const std::uint32_t sum_a = fpu::add(m.read_u32_le(entry + 0U), m.read_u32_le(entry + 8U));
    const std::uint32_t sum_b = fpu::add(m.read_u32_le(entry + 4U), m.read_u32_le(entry + 12U));
    m.write_u32_le(b + 16U, sum_a);
    m.write_u32_le(b + 20U, sum_b);
    const std::uint32_t qb_copy = m.read_u32_le(a + 20U);
    const std::uint32_t pa_copy = m.read_u32_le(a + 24U);
    m.write_u32_le(a + 8U, pa_copy);
    m.write_u32_le(a + 12U, qb_copy);
    const std::uint32_t sum_b_copy = m.read_u32_le(b + 20U);
    const std::uint32_t e0_copy = m.read_u32_le(b + 24U);
    m.write_u32_le(b + 8U, e0_copy);
    m.write_u32_le(b + 12U, sum_b_copy);
    const std::uint32_t qa_copy = m.read_u32_le(a + 28U);
    const std::uint32_t pb_copy = m.read_u32_le(a + 16U);
    m.write_u32_le(a + 32U, pb_copy);
    m.write_u32_le(a + 36U, qa_copy);
    const std::uint32_t e4_copy = m.read_u32_le(b + 28U);
    const std::uint32_t sum_a_copy = m.read_u32_le(b + 16U);
    m.write_u32_le(b + 32U, sum_a_copy);
    m.write_u32_le(b + 36U, e4_copy);
    blend_word(m, a + 8U, a + 12U, k);
    blend_word(m, a + 16U, a + 20U, k);
    blend_word(m, a + 24U, a + 28U, k);
    blend_word(m, a + 32U, a + 36U, k);
    const std::uint32_t a8 = m.read_u32_le(a + 8U);
    const std::uint32_t a12 = m.read_u32_le(a + 12U);
    m.write_u32_le(a + 0U, a8);
    m.write_u32_le(a + 4U, a12);
    const std::uint32_t b8 = m.read_u32_le(b + 8U);
    const std::uint32_t b12 = m.read_u32_le(b + 12U);
    m.write_u32_le(b + 0U, b8);
    m.write_u32_le(b + 4U, b12);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired count;
// it returns early, above kCeiling, as soon as the count passes it, so a long walk cannot hang the dry run.
template <class Memory>
Emission emit_keyed_quads(Memory& m, const GuestAddress p0, const GuestAddress p1, GuestAddress keys,
                          const GuestAddress s, const GuestAddress gp) {
    std::uint32_t retired = kEntryCost;
    std::uint32_t s136 = m.read_u32_le(p1);
    const std::uint32_t y1 = m.read_u32_le(p1 + 4U);
    const GuestAddress table = m.read_u32_le(kTableGlobal);
    const std::uint16_t key = m.read_u16_le(keys);
    GuestAddress entry = m.read_u32_le(table + 32U + 4U * (key & 0x7fU));
    for (;;) {
        retired += kKeyTestCost;
        if (m.read_u16_le(entry + 28U) == key) {
            break;
        }
        retired += kEndTestCost;
        if (entry == m.read_u32_le(table + 28U)) {
            break;
        }
        retired += kStepCost;
        entry += 32U;
        if (retired > kCeiling) {
            return {retired, 0U, y1};
        }
    }

    // 0x003d7394: the scales, the drop and the top, from the table, P0, the gp float and S.
    retired += kSetupCost;
    const std::uint32_t scale8 = m.read_u32_le(table + 8U);
    const std::uint32_t scale12 = m.read_u32_le(table + 12U);
    const std::uint32_t p0x = m.read_u32_le(p0);
    const std::uint32_t p0y = m.read_u32_le(p0 + 4U);
    const std::uint32_t gp_float = m.read_u32_le(gp + static_cast<GuestAddress>(kGlobalFloatOffset));
    const std::uint32_t s4 = m.read_u32_le(s + 4U);
    const std::uint32_t s12 = m.read_u32_le(s + 12U);
    Terms k{};
    k.y1 = y1;
    k.scale_x = fpu::mul(fpu::mul(scale8, p0x), kTwentyFive);
    k.scale_y = fpu::mul(fpu::mul(scale12, p0y), kTwentyFive);
    k.drop = fpu::mul(gp_float, p0x);
    k.top = fpu::add(s4, s12);

    GuestAddress rec_a = kQuadsA;
    GuestAddress rec_b = kQuadsB;
    std::uint32_t count = 0U;
    retired += kSampleCost;
    std::uint32_t sample = m.read_u32_le(entry);
    for (;;) {
        if (retired > kCeiling) {
            return {retired, count, y1};
        }
        // 0x003d7434: a quad is written only when the entry's float at +0 is not negative.
        retired += kSignCost;
        const bool negative = fpu::lt(sample, kZero);
        if (negative) {
            retired += kNegativeCost;
        }
        retired += kSignBranchCost;
        if (!negative) {
            retired += kLimitCost;
            if (count + 5U >= kCountLimit) {
                retired += kReturnCost;
                return {retired, count, y1};
            }
            retired += kQuadCost;
            count += 5U;
            emit_quad(m, entry, rec_a, rec_b, s136, k);
            rec_a += 40U;
            rec_b += 40U;
        }

        // 0x003d7620: the next key; a zero key ends the call.
        retired += kKeyAdvanceCost;
        keys += 2U;
        const std::uint16_t next_key = m.read_u16_le(keys);
        if (next_key == 0U) {
            retired += kReturnCost;
            return {retired, count, y1};
        }

        // 0x003d7630: the shadow copies, S136 and the next key's bucket head.
        retired += kShadowCost;
        const std::uint32_t shadow_a0 = m.read_u32_le(rec_a - 8U);
        const std::uint32_t shadow_a4 = m.read_u32_le(rec_a - 4U);
        count += 1U;
        m.write_u32_le(rec_a + 0U, shadow_a0);
        m.write_u32_le(rec_a + 4U, shadow_a4);
        const std::uint32_t acc = fpu::add(kZero, s136);
        rec_a += 8U;
        const std::uint32_t shadow_b0 = m.read_u32_le(rec_b - 8U);
        const std::uint32_t shadow_b4 = m.read_u32_le(rec_b - 4U);
        m.write_u32_le(rec_b + 0U, shadow_b0);
        m.write_u32_le(rec_b + 4U, shadow_b4);
        const std::uint32_t tail = m.read_u32_le(entry + 24U);
        rec_b += 8U;
        entry = m.read_u32_le(table + 32U + 4U * (next_key & 0x7fU));
        s136 = fpu::madd(acc, tail, k.scale_x);

        // 0x003d7694-0x003d76a8 and 0x003d7684-0x003d7690: search the chain for the next key.
        for (;;) {
            if (retired > kCeiling) {
                return {retired, count, y1};
            }
            retired += kKeyTestCost;
            if (m.read_u16_le(entry + 28U) == next_key) {
                retired += kMatchCost;
                break;
            }
            retired += kEndTestCost;
            if (entry == m.read_u32_le(table + 28U)) {
                retired += kSampleCost;
                break;
            }
            retired += kStepCost;
            entry += 32U;
        }
        sample = m.read_u32_le(entry);
    }
}

// The implementation: the body on the live CPU and memory, then v0 (the count) and f0 as the original leaves them.
template <class Context>
n3::Run emit_keyed_quads_run(Context& context) {
    auto call = n3::guest_call(context);
    const Emission out = emit_keyed_quads(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3),
                                          call.cpu().read_gpr_u32(n3::kGlobalPointer));
    call.return_value(out.count);
    call.cpu().write_fpr_bits(0U, out.f0);
    return call.finish(out.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003d7340, 884U, 0x90bed4fd42721a27ULL, "EmitKeyedQuadStrip", kCeiling, emit_keyed_quads_run,
         EngineReturn::Integer)
