// 0x001a6000 (a0 = origin {x, y}, a1 = scale {sx, sy}, a2 = P): LayoutTextGlyphQuads. The glyph ids are the
// halfwords from the string at word(word(P) + 4) up to the first zero. Each id is looked up in the glyph table T
// (the word at 0x00665ed0): the bucket T + 32 + 4 * (id & 0x7f) starts a run of 32-byte glyph records, walked
// until a record holds the id or the sentinel record (the word at T + 28) is reached. A glyph whose u is below
// zero is skipped. Any other glyph writes one quad to the position buffer (0x0066eb20) and one to the UV buffer
// (0x0066fb00), 16 bytes each. Returns the number of quads in v0.
//
// Glyph record: float u, v, w, h at +0..+12 (the UV rectangle), float x and y offsets at +16 and +20, float
// advance at +24, u16 id at +28.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kGlyphTable = 0x00665ed0U;      // lw 24272($a3) with a3 = 0x00660000
constexpr GuestAddress kPositionBuffer = 0x0066eb20U;  // a2 at 0x1a603c: lui 0x67, addiu -5344
constexpr GuestAddress kUvBuffer = 0x0066fb00U;        // v1 at 0x1a6014: lui 0x67, addiu -1280
constexpr std::uint32_t kTwentySix = 0x41d00000U;      // 26.0f (lui 0x41d0)
constexpr GuestAddress kBucketBase = 32U;              // bucket word: T + 32 + 4 * (id & 0x7f)
constexpr GuestAddress kSentinelSlot = 28U;            // T + 28: the sentinel record pointer
constexpr GuestAddress kFontX = 8U;                    // T + 8: font scale x
constexpr GuestAddress kFontY = 12U;                   // T + 12: font scale y
constexpr GuestAddress kRecordBytes = 32U;
constexpr GuestAddress kGlyphU = 0U;
constexpr GuestAddress kGlyphV = 4U;
constexpr GuestAddress kGlyphW = 8U;
constexpr GuestAddress kGlyphH = 12U;
constexpr GuestAddress kGlyphX = 16U;
constexpr GuestAddress kGlyphY = 20U;
constexpr GuestAddress kGlyphAdvance = 24U;
constexpr GuestAddress kGlyphId = 28U;

// Retired instruction counts of the listing's blocks (brief.py), delay slots included.
constexpr std::uint32_t kEntryCost = 16U;    // 0x1a6000..0x1a603c: up to the first lookup
constexpr std::uint32_t kTestCost = 4U;      // 0x1a6050..0x1a605c, 0x1a6164..0x1a6170: the id compare
constexpr std::uint32_t kSentinelCost = 3U;  // 0x1a6040..0x1a6048, 0x1a6154..0x1a615c: the sentinel compare
constexpr std::uint32_t kStepCost = 1U;      // 0x1a604c, 0x1a6160: g += 32
constexpr std::uint32_t kSetupCost = 39U;    // 0x1a6060..0x1a60f8: scales, origin and the first cursor
constexpr std::uint32_t kLoadUCost = 1U;     // 0x1a60fc: lwc1 u, after a sentinel exit or the first lookup
constexpr std::uint32_t kFoundCost = 2U;     // 0x1a6174..0x1a6178: found exit, its delay slot loads u
constexpr std::uint32_t kDecideCost = 3U;    // 0x1a6100..0x1a6108: c.olt, bc1f and its delay slot
constexpr std::uint32_t kSkipCost = 1U;      // 0x1a610c: executed only when u < 0
constexpr std::uint32_t kSelectCost = 3U;    // 0x1a6110..0x1a6118: xori, beq and its delay slot
constexpr std::uint32_t kEmitCost = 66U;     // 0x1a617c..0x1a6280: the quads
constexpr std::uint32_t kNextCost = 4U;      // 0x1a611c..0x1a6128: t0 += 2, the next id and its zero test
constexpr std::uint32_t kAdvanceCost = 10U;  // 0x1a612c..0x1a6150: cursorX and the next bucket word
constexpr std::uint32_t kReturnCost = 2U;    // 0x1a6284..0x1a6288

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Layout {
    std::uint32_t retired;      // the original's retired instruction count
    std::uint32_t count;        // v0: quads written
    std::uint32_t accumulator;  // the FPU accumulator the original leaves (when advanced)
    bool advanced;              // an ADDA.S ran, so the accumulator changed
};

// call_cost's view of guest memory. Nothing is written: a store is only noted, and a read of bytes an
// earlier store of the same call wrote (or one the original would fault on) declines the call. Every
// other read sees what the original sees at that point, so the count is exact when the call is not
// declined. Stores are noted as spans, which is cheaper than logging each one.
template <class Base>
class LayoutDryMemory {
public:
    explicit LayoutDryMemory(const Base& memory) noexcept : memory_(memory) {}

    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U) || stored(address, 2U)) {
            declined_ = true;
            return 0U;
        }
        return memory_.read_u16_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U) || stored(address, 4U)) {
            declined_ = true;
            return 0U;
        }
        return memory_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t) { note_store(address, 4U); }

    [[nodiscard]] bool declined() const noexcept { return declined_; }

private:
    struct Span {
        GuestAddress begin;
        GuestAddress end;
    };

    [[nodiscard]] bool stored(const GuestAddress address, const std::uint32_t size) const noexcept {
        for (std::size_t i = 0U; i < span_count_; ++i) {
            if (address < spans_[i].end && spans_[i].begin < address + size) {
                return true;
            }
        }
        return false;
    }
    void note_store(const GuestAddress address, const std::uint32_t size) {
        const GuestAddress end = address + size;
        for (std::size_t i = 0U; i < span_count_; ++i) {
            if (address <= spans_[i].end && spans_[i].begin <= end) {
                spans_[i].begin = std::min(spans_[i].begin, address);
                spans_[i].end = std::max(spans_[i].end, end);
                return;
            }
        }
        if (span_count_ == spans_.size()) {
            declined_ = true;
            return;
        }
        spans_[span_count_++] = Span{address, end};
    }

    const Base& memory_;
    std::array<Span, 4> spans_{};
    std::size_t span_count_ = 0U;
    mutable bool declined_ = false;
};

// The whole function on guest memory (the implementation) or on a LayoutDryMemory (call_cost). Every
// guest load and store happens in the original's order, so reads see the original's stores.
template <class Memory>
Layout layout_quads(Memory& m, const GuestAddress origin, const GuestAddress scale, const GuestAddress pointer) {
    std::uint32_t retired = kEntryCost;
    std::uint32_t count = 0U;
    std::uint32_t accumulator = 0U;
    bool advanced = false;

    // The first id is looked up even when it is zero; the zero test only applies to the ids after it.
    const GuestAddress header = m.read_u32_le(pointer);
    const GuestAddress table = m.read_u32_le(kGlyphTable);
    GuestAddress text = m.read_u32_le(header + 4U);
    const std::uint32_t first_id = m.read_u16_le(text);
    GuestAddress glyph = m.read_u32_le(table + kBucketBase + ((first_id & 0x7fU) << 2U));
    for (;;) {
        if (retired > kCeiling) {
            return {retired, count, accumulator, advanced};
        }
        retired += kTestCost;
        if (m.read_u16_le(glyph + kGlyphId) == first_id) {
            break;
        }
        retired += kSentinelCost;
        if (glyph == m.read_u32_le(table + kSentinelSlot)) {
            break;
        }
        retired += kStepCost;
        glyph += kRecordBytes;
    }
    retired += kSetupCost + kLoadUCost;

    const std::uint32_t font_x = m.read_u32_le(table + kFontX);
    const std::uint32_t font_y = m.read_u32_le(table + kFontY);
    const std::uint32_t scale_x = m.read_u32_le(scale + 0U);
    const std::uint32_t scale_y = m.read_u32_le(scale + 4U);
    const std::uint32_t origin_x = m.read_u32_le(origin + 0U);
    const std::uint32_t origin_y = m.read_u32_le(origin + 4U);
    const std::uint32_t first_x = m.read_u32_le(glyph + kGlyphX);
    const GuestAddress table_again = m.read_u32_le(kGlyphTable);

    const std::uint32_t size_x = fpu::mul(font_x, scale_x);
    const std::uint32_t size_y = fpu::mul(font_y, scale_y);
    const std::uint32_t cursor_y = fpu::sub(origin_y, 0U);
    const std::uint32_t s1 = fpu::mul(size_x, kTwentySix);
    const std::uint32_t s2 = fpu::mul(size_y, kTwentySix);
    std::uint32_t cursor_x = fpu::sub(origin_x, fpu::mul(first_x, s1));
    GuestAddress position = kPositionBuffer;
    GuestAddress uv = kUvBuffer;

    for (;;) {
        if (retired > kCeiling) {
            return {retired, count, accumulator, advanced};
        }
        // u < 0 skips the glyph; anything else, NaN included, emits it.
        const std::uint32_t u = m.read_u32_le(glyph + kGlyphU);
        retired += kDecideCost;
        if (fpu::lt(u, 0U)) {
            retired += kSkipCost + kSelectCost;
        } else {
            retired += kSelectCost + kEmitCost;
            ++count;
            const std::uint32_t emit_x = m.read_u32_le(glyph + kGlyphX);
            const std::uint32_t emit_y = m.read_u32_le(glyph + kGlyphY);
            const std::uint32_t quad_x = fpu::add(cursor_x, fpu::mul(emit_x, s1));
            const std::uint32_t quad_y = fpu::add(cursor_y, fpu::mul(emit_y, s2));
            m.write_u32_le(position + 0U, quad_x);
            m.write_u32_le(position + 4U, quad_y);
            const std::uint32_t tex_u = m.read_u32_le(glyph + kGlyphU);
            const std::uint32_t tex_v = m.read_u32_le(glyph + kGlyphV);
            m.write_u32_le(uv + 0U, tex_u);
            m.write_u32_le(uv + 4U, tex_v);
            const std::uint32_t width = m.read_u32_le(glyph + kGlyphW);
            const std::uint32_t height = m.read_u32_le(glyph + kGlyphH);
            const std::uint32_t far_x = fpu::add(quad_x, fpu::mul(s1, width));
            const std::uint32_t far_y = fpu::add(quad_y, fpu::mul(s2, height));
            m.write_u32_le(position + 8U, far_x);
            m.write_u32_le(position + 12U, far_y);
            const std::uint32_t uv_u = m.read_u32_le(glyph + kGlyphU);
            const std::uint32_t uv_w = m.read_u32_le(glyph + kGlyphW);
            const std::uint32_t uv_v = m.read_u32_le(glyph + kGlyphV);
            const std::uint32_t uv_h = m.read_u32_le(glyph + kGlyphH);
            m.write_u32_le(uv + 8U, fpu::add(uv_u, uv_w));
            m.write_u32_le(uv + 12U, fpu::add(uv_v, uv_h));
            position += 16U;
            uv += 16U;
        }

        // The next id. A zero ends the string, and the advance of the glyph just processed never runs then.
        text += 2U;
        const std::uint32_t next_id = m.read_u16_le(text);
        retired += kNextCost;
        if (next_id == 0U) {
            retired += kReturnCost;
            return {retired, count, accumulator, advanced};
        }

        // ADDA.S ACC = 0 + cursorX, MADD.S cursorX = ACC + advance * S1, then the next id's bucket.
        retired += kAdvanceCost;
        const std::uint32_t advance = m.read_u32_le(glyph + kGlyphAdvance);
        accumulator = fpu::add(0U, cursor_x);
        advanced = true;
        cursor_x = fpu::madd(accumulator, advance, s1);
        glyph = m.read_u32_le(table_again + kBucketBase + ((next_id & 0x7fU) << 2U));

        // The in-loop lookup of next_id. A found record leaves through F, a sentinel through the load of u.
        for (;;) {
            if (retired > kCeiling) {
                return {retired, count, accumulator, advanced};
            }
            retired += kTestCost;
            if (m.read_u16_le(glyph + kGlyphId) == next_id) {
                retired += kFoundCost;
                break;
            }
            retired += kSentinelCost;
            if (glyph == m.read_u32_le(table_again + kSentinelSlot)) {
                retired += kLoadUCost;
                break;
            }
            retired += kStepCost;
            glyph += kRecordBytes;
        }
    }
}

template <class Context>
std::uint32_t layout_text_glyph_quads_cost(const Context& context) {
    LayoutDryMemory dry{context.memory};
    const Layout layout = layout_quads(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                       context.cpu.read_gpr_u32(6U));
    if (layout.retired > kCeiling || dry.declined()) {
        return 0U;
    }
    return layout.retired;
}

template <class Context>
n3::Run layout_text_glyph_quads(Context& context) {
    auto call = n3::guest_call(context);
    const Layout layout = layout_quads(call.memory(), call.arg(0), call.arg(1), call.arg(2));
    if (layout.advanced) {
        call.cpu().set_fpu_accumulator_bits(layout.accumulator);
    }
    call.return_value(layout.count);
    return call.finish(layout.retired);
}

} // namespace

extern constinit const EngineFunction n3_001a6000{0x001a6000U, 652U, 0x23a21b31d2746b07ULL, "LayoutTextGlyphQuads",
                                                  kCeiling, n3::live<&layout_text_glyph_quads<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &layout_text_glyph_quads_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001a6000, layout_text_glyph_quads, layout_text_glyph_quads_cost)
