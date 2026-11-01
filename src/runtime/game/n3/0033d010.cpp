// 0x0033d010 (BuildGlyphStrip, 868 bytes): a0 = P (floats X0, Y0, W, H), a1 = Q (font F, the
// zero-terminated u16 string, float scale S). Lays the glyphs out as a triangle strip in two
// parallel vertex arrays: positions at 0x0066eb20 (A), texture coordinates at 0x0066fb00 (B),
// 8 bytes per entry. Returns v0 = the number of entries written. Calls retiring more than
// kCeiling are left to the original code (call_cost returns 0).

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kVertexA = 0x0066EB20U;  // lui 0x67, -5344
constexpr GuestAddress kVertexB = 0x0066FB00U;  // lui 0x67, -1280
constexpr std::uint32_t kTwo = 0x40000000U;     // 2.0F
constexpr std::uint32_t kEntryLimit = 509U;     // sltiu against 509 (unsigned)
constexpr std::uint32_t kScaleOffset = 32712U;  // float at $gp - 32712

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count. The walks and the outer loop stop as soon as
// the count can no longer stay within it, so a corrupt font or a very long string cannot hang the
// dry run.
constexpr std::uint32_t kCeiling = 20000U;

// One walk down a font's hash bucket.
struct GlyphScan {
    GuestAddress glyph{};
    std::uint32_t steps{};  // glyphs passed over, 32 bytes each
    bool found{};           // false: the walk stopped on the font's sentinel glyph
    bool overrun{};         // the walk stopped early: the call retires more than kCeiling
};

// What the body returns: the instructions the original retires on this path (delay slots
// included) and v0, the number of entries written.
struct Result {
    std::uint32_t retired{};
    std::uint32_t count{};
};

// The body's memory for call_cost. n3::DryMemory answers each read by searching every write made so
// far, and this body writes into A and B for every glyph, so a long string made the dry run quadratic.
// This view keeps the bytes the body has written into A and B in a window (the latest write to a byte
// wins, as in DryMemory). Every other access goes to a DryMemory over guest memory, and a read that
// touches no written byte is the guest's own read.
template <class Base>
class VertexWindowMemory {
public:
    explicit VertexWindowMemory(const Base& memory) noexcept : rest_(memory) {}

    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        const std::size_t offset = window_offset(address);
        return (offset < kWindowBytes && written_[offset]) ? bytes_[offset] : rest_.read_u8(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if (!any_written(address, 2U)) {
            return rest_.read_u16_le(address);
        }
        return static_cast<std::uint16_t>(read_u8(address) | (read_u8(address + 1U) << 8U));
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if (!any_written(address, 4U)) {
            return rest_.read_u32_le(address);
        }
        return static_cast<std::uint32_t>(read_u16_le(address)) |
               (static_cast<std::uint32_t>(read_u16_le(address + 2U)) << 16U);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        const std::size_t offset = window_offset(address);
        if (offset < kWindowBytes) {
            bytes_[offset] = value;
            written_[offset] = true;
        } else {
            rest_.write_u8(address, value);
        }
    }
    void write_u16_le(const GuestAddress address, const std::uint16_t value) {
        write_u8(address, static_cast<std::uint8_t>(value));
        write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        write_u16_le(address, static_cast<std::uint16_t>(value));
        write_u16_le(address + 2U, static_cast<std::uint16_t>(value >> 16U));
    }

private:
    // A (0x0066eb20) through the end of B's largest entry (B + 0x0fdf) and a little more.
    static constexpr std::size_t kWindowBytes = 0x2100U;
    [[nodiscard]] static std::size_t window_offset(const GuestAddress address) {
        return static_cast<std::size_t>(address - kVertexA);
    }
    [[nodiscard]] bool any_written(const GuestAddress address, const unsigned bytes) const {
        for (unsigned i = 0U; i < bytes; ++i) {
            const std::size_t offset = window_offset(address + i);
            if (offset < kWindowBytes && written_[offset]) {
                return true;
            }
        }
        return false;
    }

    n3::DryMemory rest_;
    std::array<std::uint8_t, kWindowBytes> bytes_{};
    std::array<bool, kWindowBytes> written_{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost).
template <class Memory>
Result glyph_strip(Memory& m, const std::uint32_t gp, const GuestAddress position, const GuestAddress request) {
    // The glyph code at +28 is compared before the sentinel test, so a sentinel whose
    // code matches is found. `base` is the count retired before this walk; each glyph
    // passed over adds 8, so the walk stops once the call cannot stay within kCeiling.
    const auto scan = [&m](const GuestAddress table, const std::uint16_t wanted, const std::uint32_t base) {
        const GuestAddress sentinel = m.read_u32_le(table + 28U);
        GlyphScan result{m.read_u32_le(table + 32U + 4U * (wanted & 0x7fU)), 0U, false, false};
        for (;;) {
            if (base + 8U * result.steps > kCeiling) {
                result.overrun = true;
                return result;
            }
            if (m.read_u16_le(result.glyph + 28U) == wanted) {
                result.found = true;
                return result;
            }
            if (result.glyph == sentinel) {
                return result;
            }
            result.glyph += 32U;
            ++result.steps;
        }
    };

    const GuestAddress font = m.read_u32_le(request);
    GuestAddress text = m.read_u32_le(request + 4U);
    std::uint16_t code = m.read_u16_le(text);

    // Setup. The first lookup retires 13 instructions when it finds its glyph (16 when it
    // stops on the sentinel) and 8 per glyph passed over; the setup block 0x0033d054..
    // 0x0033d0e8 retires 38.
    const GlyphScan first = scan(font, code, 38U);
    if (first.overrun) {
        return Result{kCeiling + 1U, 0U};
    }
    std::uint32_t retired = (first.found ? 13U : 16U) + 8U * first.steps + 38U;
    GuestAddress glyph = first.glyph;

    const std::uint32_t global_scale = m.read_u32_le(gp - kScaleOffset);
    const std::uint32_t scale = m.read_u32_le(request + 8U);
    const std::uint32_t x0 = m.read_u32_le(position);
    const std::uint32_t y0 = m.read_u32_le(position + 4U);
    const std::uint32_t width = m.read_u32_le(position + 8U);
    const std::uint32_t height = m.read_u32_le(position + 12U);
    const std::uint32_t font_x = m.read_u32_le(font + 8U);
    const std::uint32_t font_y = m.read_u32_le(font + 12U);

    const std::uint32_t ratio =
        fpu::div(fpu::mul(fpu::mul(global_scale, scale), width), fpu::mul(kTwo, height));
    const std::uint32_t x_extent = fpu::mul(font_x, width);
    const std::uint32_t sx = fpu::mul(scale, x_extent);
    const std::uint32_t y_extent = fpu::mul(font_y, height);
    const std::uint32_t y_top = fpu::add(y0, height);
    // The cursor starts at X0 - GX*SX, GX of the first glyph.
    std::uint32_t cursor = fpu::msub(fpu::add(0U, x0), m.read_u32_le(glyph + 16U), sx);

    GuestAddress pa = kVertexA;
    GuestAddress pb = kVertexB;
    std::uint32_t count = 0U;
    for (;;) {
        // Past the ceiling the rest of the call only adds to the count: stop here.
        if (retired > kCeiling) {
            return Result{retired, count};
        }
        // C.OLT on U0 against 0.0: a glyph with a negative U0 draws nothing.
        if (fpu::lt(m.read_u32_le(glyph), 0U)) {
            retired += 7U;
        } else {
            if (count + 5U >= kEntryLimit) {
                retired += 12U;
                break;
            }
            retired += 121U;
            count += 5U;

            // Quad, accessed in the original's order: several words are stored and then read back
            // (pa + 24, pa + 28, pa + 16 and so on), so every read must see exactly the writes made
            // before it. A stays below B (count is at most 508 entries), so the arrays never overlap.
            const std::uint32_t gx = m.read_u32_le(glyph + 16U);
            const std::uint32_t gy = m.read_u32_le(glyph + 20U);
            m.write_u32_le(pa + 24U, fpu::add(cursor, fpu::mul(gx, sx)));
            m.write_u32_le(pa + 28U, fpu::add(y0, fpu::mul(gy, y_extent)));
            m.write_u32_le(pb + 24U, m.read_u32_le(glyph));
            m.write_u32_le(pb + 28U, m.read_u32_le(glyph + 4U));
            const std::uint32_t uw = m.read_u32_le(glyph + 8U);
            const std::uint32_t vh = m.read_u32_le(glyph + 12U);
            const std::uint32_t x1 = m.read_u32_le(pa + 24U);
            const std::uint32_t y1 = m.read_u32_le(pa + 28U);
            m.write_u32_le(pa + 16U, fpu::add(x1, fpu::mul(sx, uw)));
            m.write_u32_le(pa + 20U, fpu::add(y1, fpu::mul(y_extent, vh)));
            const std::uint32_t u0 = m.read_u32_le(glyph);
            const std::uint32_t uw2 = m.read_u32_le(glyph + 8U);
            const std::uint32_t v0 = m.read_u32_le(glyph + 4U);
            const std::uint32_t vh2 = m.read_u32_le(glyph + 12U);
            m.write_u32_le(pb + 16U, fpu::add(u0, uw2));
            m.write_u32_le(pb + 20U, fpu::add(v0, vh2));

            const std::uint32_t pa20 = m.read_u32_le(pa + 20U);
            const std::uint32_t pa24 = m.read_u32_le(pa + 24U);
            m.write_u32_le(pa + 8U, pa24);
            m.write_u32_le(pa + 12U, pa20);

            const std::uint32_t pb20 = m.read_u32_le(pb + 20U);
            const std::uint32_t pb24 = m.read_u32_le(pb + 24U);
            m.write_u32_le(pb + 8U, pb24);
            m.write_u32_le(pb + 12U, pb20);

            const std::uint32_t pa28 = m.read_u32_le(pa + 28U);
            const std::uint32_t pa16 = m.read_u32_le(pa + 16U);
            m.write_u32_le(pa + 32U, pa16);
            m.write_u32_le(pa + 36U, pa28);

            const std::uint32_t pb28 = m.read_u32_le(pb + 28U);
            const std::uint32_t pb16 = m.read_u32_le(pb + 16U);
            m.write_u32_le(pb + 32U, pb16);
            m.write_u32_le(pb + 36U, pb28);

            // Four vertices (x at pa+lo, y at pa+hi): x += ratio * ((Y0 + H) - y), the
            // accumulator starting from x (ADDA.S with f0 = 0).
            const auto blend = [&m, &pa, y_top, ratio](const GuestAddress hi, const GuestAddress lo) {
                const std::uint32_t t = fpu::sub(y_top, m.read_u32_le(pa + hi));
                const std::uint32_t base = fpu::add(0U, m.read_u32_le(pa + lo));
                m.write_u32_le(pa + lo, fpu::madd(base, ratio, t));
            };
            blend(12U, 8U);
            blend(20U, 16U);
            blend(28U, 24U);
            blend(36U, 32U);

            const std::uint32_t pa8 = m.read_u32_le(pa + 8U);
            const std::uint32_t pa12 = m.read_u32_le(pa + 12U);
            m.write_u32_le(pa, pa8);
            m.write_u32_le(pa + 4U, pa12);

            const std::uint32_t pb8 = m.read_u32_le(pb + 8U);
            const std::uint32_t pb12 = m.read_u32_le(pb + 12U);
            m.write_u32_le(pb, pb8);
            m.write_u32_le(pb + 4U, pb12);
            pa += 40U;
            pb += 40U;
        }

        // ADVANCE: the next character. Its entry repeats the last one written (the join).
        text += 2U;
        code = m.read_u16_le(text);
        if (code == 0U) {
            retired += 6U;
            break;
        }
        if (count + 1U >= kEntryLimit) {
            retired += 10U;
            break;
        }
        count += 1U;
        const std::uint32_t carried = fpu::add(0U, cursor);
        const std::uint32_t pa_lo = m.read_u32_le(pa - 8U);
        const std::uint32_t pa_hi = m.read_u32_le(pa - 4U);
        m.write_u32_le(pa, pa_lo);
        m.write_u32_le(pa + 4U, pa_hi);
        pa += 8U;
        const std::uint32_t pb_lo = m.read_u32_le(pb - 8U);
        const std::uint32_t pb_hi = m.read_u32_le(pb - 4U);
        m.write_u32_le(pb, pb_lo);
        m.write_u32_le(pb + 4U, pb_hi);
        pb += 8U;

        // The cursor moves by the glyph just handled: ADV * SX.
        const std::uint32_t advance = m.read_u32_le(glyph + 24U);
        const GlyphScan next = scan(font, code, retired + 28U);
        if (next.overrun) {
            return Result{kCeiling + 1U, count};
        }
        retired += 28U + (next.found ? 6U : 8U) + 8U * next.steps;
        glyph = next.glyph;
        cursor = fpu::madd(carried, advance, sx);
    }

    return Result{retired, count};
}

template <class Context>
n3::Run build_glyph_strip(Context& context) {
    auto call = n3::guest_call(context);
    const Result r =
        glyph_strip(call.memory(), call.cpu().read_gpr_u32(n3::kGlobalPointer), call.arg(0), call.arg(1));
    call.return_value(r.count);
    return call.finish(r.retired);
}

// The exact count of the original on this path, from a dry run that leaves guest memory alone;
// 0 hands the call to the original code when the count is above kCeiling.
template <class Context>
std::uint32_t build_glyph_strip_cost(const Context& context) {
    VertexWindowMemory dry{context.memory};
    const Result r = glyph_strip(dry, context.cpu.read_gpr_u32(n3::kGlobalPointer), context.cpu.read_gpr_u32(4U),
                                 context.cpu.read_gpr_u32(5U));
    return r.retired <= kCeiling ? r.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_0033d010{0x0033d010U, 868U, 0x58a5c1f25752139aULL, "BuildGlyphStrip",
                                                  kCeiling, n3::live<&build_glyph_strip<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6,
                                                  {}, &build_glyph_strip_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0033d010, build_glyph_strip, build_glyph_strip_cost)
