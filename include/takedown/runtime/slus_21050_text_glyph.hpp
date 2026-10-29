#pragma once

#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050::text_float_detail {

// EE binary32 arithmetic for text layout: denormal operands flush to signed
// zero, and the rounded product or sum is nudged toward the exact double value.
[[nodiscard]] std::uint32_t add_s_bits(std::uint32_t left, std::uint32_t right) noexcept;
[[nodiscard]] std::uint32_t multiply_s_bits(std::uint32_t left, std::uint32_t right) noexcept;

} // namespace takedown::runtime::slus_21050::text_float_detail

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kBuildTextGlyphRectanglesAddress = 0x002f9310U;
inline constexpr guest::GuestAddress kSelectFittingTextSpanAddress = 0x001d7e10U;
inline constexpr guest::GuestAddress kTextGlyphPositionRectanglesAddress = 0x0066eb20U;
inline constexpr guest::GuestAddress kTextGlyphTextureRectanglesAddress = 0x0066fb00U;
// Proven capacity of retail's adjacent fixed position and texture arrays.
// Both occupy exactly 0xfe0 bytes and the downstream packet builder can safely
// represent at most 254 paired records through its 2*n+3 qword requirement.
inline constexpr std::size_t kTextGlyphRectangleCapacity = 254U;
inline constexpr NativeFunctionLimits kTextGlyphOneGlyphCost{222U, 222U};
// Absolute dispatcher ceiling for the largest direct, wholly visible ordinary
// line that fits the proven 254-record output arrays. More expensive decoded
// lines retain generated guest execution so the runner can use this practical
// ceiling during its conservative scheduler preflight.
inline constexpr NativeFunctionLimits kTextGlyphNativeMaximumCost{30329U, 30329U};

// Raw binary32 values retain the EE's ChopZero arithmetic and special-value
// rules. The three values are the exact fields read at A0+0, +4, and +8 by
// BuildTextGlyphRectangles.
struct TextLayoutBoxBits {
    std::uint32_t base_x_bits{};
    std::uint32_t base_y_bits{};
    std::uint32_t horizontal_extent_bits{};

    bool operator==(const TextLayoutBoxBits&) const = default;
};

// Independent X/Y scaling read through A3. The retail routine applies the
// uniform F12 scale before the font's horizontal and vertical scale factors.
struct TextGlyphAxisScaleBits {
    std::uint32_t x_bits{};
    std::uint32_t y_bits{};

    bool operator==(const TextGlyphAxisScaleBits&) const = default;
};

// Exact 32-byte record traversed by the font's 128 low-seven-bit buckets.
// The metric names follow their observed rectangle-building roles; the final
// halfword is retained even though this restricted planner does not interpret
// it.
struct TextGlyphMetricsBits {
    std::uint32_t texture_u_bits{};
    std::uint32_t texture_v_bits{};
    std::uint32_t texture_width_bits{};
    std::uint32_t texture_height_bits{};
    std::uint32_t bearing_x_bits{};
    std::uint32_t bearing_y_bits{};
    std::uint32_t advance_x_bits{};
    char16_t code_unit{};
    std::uint16_t trailing_bits{};

    bool operator==(const TextGlyphMetricsBits&) const = default;
};

static_assert(sizeof(TextGlyphMetricsBits) == 32U);

using TextGlyphBucketView = std::span<const TextGlyphMetricsBits>;

// Pure view of host-owned recovered font fields. The pointed-to records must
// remain alive and unchanged for the duration of a text-layout call. Each
// bucket span must contain the ordered 32-byte records retail probes from that
// bucket head up to, but not including, its sentinel. The span selector and
// ordinary rectangle planner search that chain; the mid-word profile below
// retains its narrower direct-head requirement. Returned values retain no
// borrowed pointers.
struct TextGlyphFontView {
    std::uint32_t horizontal_scale_bits{};
    std::uint32_t vertical_scale_bits{};
    std::array<TextGlyphBucketView, 128U> buckets{};
};

struct TextGlyphRectangleBits {
    // x0, y0, x1, y1
    std::array<std::uint32_t, 4U> position{};
    // u0, v0, u1, v1
    std::array<std::uint32_t, 4U> texture{};

    bool operator==(const TextGlyphRectangleBits&) const = default;
};

// Register-independent inputs corresponding to the retail A0-A3/F12-F14/T0
// values. `text` contains content only: the pure API represents its terminating
// UTF-16 zero with span::end(). Embedded zeroes have no typed representation.
struct TextGlyphBuildInput {
    std::span<const char16_t> text;
    TextGlyphFontView font;
    TextLayoutBoxBits box;
    TextGlyphAxisScaleBits axis_scale;
    std::uint32_t uniform_scale_bits{};
    std::uint32_t maximum_line_width_bits{};
    std::uint32_t horizontal_alignment_bits{};
    std::uint32_t layout_flags{};
};

struct SelectedTextSpan {
    std::size_t begin_code_unit{};
    std::size_t end_code_unit{};
    std::uint32_t width_bits{};

    bool operator==(const SelectedTextSpan&) const = default;
};

enum class TextWordWrapMode : std::uint8_t {
    PreserveWords,
    AllowMidWordBreak,
};

// Register-independent inputs for SelectFittingTextSpan. `text` contains
// content only and span::end() represents the terminating UTF-16 zero. The
// input scan is deliberately independent from the 254-rectangle output limit:
// retail can select one short line from a much longer message.
struct TextSpanSelectionInput {
    std::span<const char16_t> text;
    TextGlyphFontView font;
    std::uint32_t maximum_line_width_bits{};
    TextWordWrapMode word_wrap_mode{TextWordWrapMode::PreserveWords};
};

// Recovered word-wrap operation used by the retail text rectangle builder.
// It skips leading ASCII spaces/newlines, follows complete low-seven-bit font
// collision chains, retains fitting ASCII-space breakpoints, and stops before
// a newline. U+00a0 is an ordinary glyph. Missing glyphs, embedded UTF-16
// zeroes, non-finite arithmetic inputs, and undefined retail edge paths return
// nullopt without side effects.
[[nodiscard]] std::optional<SelectedTextSpan>
select_fitting_text_span(const TextSpanSelectionInput& input);

// Side-effect-free output of the composed SelectFittingTextSpan and
// BuildTextGlyphRectangles path. selected_span identifies the chosen line in
// the input. Visible glyphs occupy compact rectangle slots; negative-U glyphs
// may advance the pen without emitting. Slots after rectangle_count remain zero.
struct TextGlyphRectanglePlan {
    SelectedTextSpan selected_span;
    std::array<TextGlyphRectangleBits, kTextGlyphRectangleCapacity> rectangles{};
    std::uint32_t rectangle_count{};
    NativeFunctionLimits cost{};

    bool operator==(const TextGlyphRectanglePlan&) const = default;
};

// Plans ordinary word-wrapped lines through complete low-seven-bit collision
// chains with exact dynamic retail cost and EE ChopZero geometry. The 254-slot
// limit applies to emitted rectangles rather than scanned input. The narrower
// mid-word-break profile still requires a fitting, direct, wholly visible span.
// Selected CR/LF records, zero-visible results, unsupported metrics, missing
// glyphs, and cost overflow return nullopt without side effects.
[[nodiscard]] std::optional<TextGlyphRectanglePlan>
plan_text_glyph_rectangles(const TextGlyphBuildInput& input);

// Register-independent guest-memory inputs corresponding to A0-A3, F12-F14,
// and T0 at BuildTextGlyphRectangles entry. This public boundary deliberately
// carries no stack or CPU state; the optional EE adapter below layers those
// architectural concerns over a successfully reviewed guest plan.
struct TextGlyphGuestInput {
    guest::GuestAddress layout_box{};
    guest::GuestAddress text_cursor_cell{};
    guest::GuestAddress font{};
    guest::GuestAddress axis_scale{};
    std::uint32_t uniform_scale_bits{};
    std::uint32_t maximum_line_width_bits{};
    std::uint32_t horizontal_alignment_bits{};
    std::uint32_t layout_flags{};

    bool operator==(const TextGlyphGuestInput&) const = default;
};

// One exact guest record traversed while resolving a UTF-16 code unit. General
// line plans own these records so a later commit can detect stale collision
// chains before changing the cursor or either rectangle buffer.
struct TextGlyphRecordSnapshot {
    guest::GuestAddress address{};
    TextGlyphMetricsBits glyph;

    bool operator==(const TextGlyphRecordSnapshot&) const = default;
};

// Canonical snapshot of one low-seven-bit font bucket. The record prefix ends
// at the furthest matching record reached by any helper or outer-loop lookup
// in this line; earlier matches share the same owned prefix.
struct TextGlyphBucketSnapshot {
    std::uint32_t bucket_index{};
    guest::GuestAddress bucket_slot{};
    guest::GuestAddress bucket_head{};
    std::vector<TextGlyphRecordSnapshot> records_through_furthest_match;

    bool operator==(const TextGlyphBucketSnapshot&) const = default;
};

// Immutable guest-memory snapshot for one ordinary wrapped line. Text begins
// at text_cursor and ends at the helper's deciding space/newline or its actual
// NUL terminator. glyph_dependency_end_code_unit is the exclusive text index
// reached by helper lookup. Bucket snapshots include every complete collision
// record through the furthest match used by the helper or outer rectangle loop.
struct TextGlyphLineGuestPlan {
    TextGlyphGuestInput input;
    TextLayoutBoxBits box;
    guest::GuestAddress text_cursor{};
    std::vector<char16_t> text_through_decision;
    std::size_t glyph_dependency_end_code_unit{};
    TextGlyphAxisScaleBits axis_scale;
    std::uint32_t font_horizontal_scale_bits{};
    std::uint32_t font_vertical_scale_bits{};
    guest::GuestAddress font_sentinel{};
    std::vector<TextGlyphBucketSnapshot> font_buckets;
    std::uint64_t outer_function_fingerprint{};
    std::uint64_t helper_function_fingerprint{};
    TextGlyphRectanglePlan rectangle_plan;

    bool operator==(const TextGlyphLineGuestPlan&) const = default;
};

struct TextGlyphLineGuestResult {
    guest::GuestAddress position_rectangles{};
    guest::GuestAddress texture_rectangles{};
    guest::GuestAddress final_text_cursor{};
    TextGlyphRectanglePlan rectangle_plan;

    bool operator==(const TextGlyphLineGuestResult&) const = default;
};

// Builds and atomically commits the currently reviewed ordinary multi-glyph
// profile. Planning has no side effects. Commit reconstructs the complete plan
// and requires exact equality before its first write.
[[nodiscard]] std::optional<TextGlyphLineGuestPlan>
plan_text_glyph_line_from_guest(const guest::EeMemory& memory, const TextGlyphGuestInput& input);

[[nodiscard]] TextGlyphLineGuestResult
commit_text_glyph_line_to_guest(guest::EeMemory& memory, const TextGlyphLineGuestPlan& plan);

// Complete immutable snapshot behind the first guest-memory profile. It keeps
// every value read by the composed retail routines, including fields that do
// not affect the one-glyph semantic rectangle, so commit can reject a stale or
// forged plan before its first guest write. The selected bucket represents a
// direct matching head; no collision-chain shape is inferred.
struct TextGlyphGuestPlan {
    TextGlyphGuestInput input;
    TextLayoutBoxBits box;
    guest::GuestAddress text_cursor{};
    std::array<char16_t, 2U> text_and_terminator{};
    TextGlyphAxisScaleBits axis_scale;
    std::uint32_t font_horizontal_scale_bits{};
    std::uint32_t font_vertical_scale_bits{};
    guest::GuestAddress font_sentinel{};
    guest::GuestAddress selected_bucket_slot{};
    guest::GuestAddress selected_glyph{};
    TextGlyphMetricsBits glyph;
    std::uint64_t outer_function_fingerprint{};
    std::uint64_t helper_function_fingerprint{};
    SelectedTextSpan selected_span;
    TextGlyphRectangleBits rectangle;
    std::uint32_t rectangle_count{};
    NativeFunctionLimits cost{};

    bool operator==(const TextGlyphGuestPlan&) const = default;
};

// Semantic result of committing one admitted glyph. The optional compatibility
// adapter also reproduces the architectural EE register and FPU residue.
struct TextGlyphGuestResult {
    guest::GuestAddress position_rectangle{};
    guest::GuestAddress texture_rectangle{};
    guest::GuestAddress final_text_cursor{};
    SelectedTextSpan selected_span;
    TextGlyphRectangleBits rectangle;
    std::uint32_t rectangle_count{};
    NativeFunctionLimits cost{};

    bool operator==(const TextGlyphGuestResult&) const = default;
};

// Decodes and snapshots the exact one-content-glyph ordinary flag path from
// direct canonical main RAM. It retains the original guest/native profile's
// strict returned-width gate. Both complete retail bodies are fingerprinted.
// Unsupported layouts, unsafe aliases, and unmapped or unaligned pointers
// return nullopt without mutation.
[[nodiscard]] std::optional<TextGlyphGuestPlan>
plan_text_glyph_rectangles_from_guest(const guest::EeMemory& memory,
                                      const TextGlyphGuestInput& input);

// Recomputes and compares the complete guest plan before its first write.
// Throws std::invalid_argument for stale, forged, or out-of-profile plans.
[[nodiscard]] TextGlyphGuestResult
commit_text_glyph_rectangles_to_guest(guest::EeMemory& memory, const TextGlyphGuestPlan& plan);

namespace text_glyph_guest_detail {

// The layout box reaches the decoder either as a direct guest address or as
// the three bits already read by the caller.
using TextGlyphLineLayoutBoxInput = std::variant<guest::GuestAddress, TextLayoutBoxBits>;

// Complete register-independent input for one line decode. The cursor, font
// and scale fields mirror the retail A0-A3/F12-F14/T0 values.
struct TextGlyphLineValueInput {
    TextGlyphLineLayoutBoxInput layout_box{};
    guest::GuestAddress text_cursor{};
    guest::GuestAddress font{};
    TextGlyphAxisScaleBits axis_scale{};
    std::uint32_t uniform_scale_bits{};
    std::uint32_t maximum_line_width_bits{};
    std::uint32_t horizontal_alignment_bits{};
    std::uint32_t layout_flags{};

    bool operator==(const TextGlyphLineValueInput&) const = default;
};

// One direct-main-RAM access observed or planned while decoding a line.
struct DirectGuestMemoryRange {
    guest::GuestAddress address{};
    std::uint64_t length{};

    bool operator==(const DirectGuestMemoryRange&) const = default;
};

// One matched font-bucket record plus how many earlier collision records in
// that bucket were skipped before the match.
struct ResolvedTextGlyph {
    TextGlyphMetricsBits glyph;
    std::size_t collision_mismatches{};
};

// Fully decoded ordinary line. It keeps every value read by the decode so a
// later publication can reject a stale plan before its first guest write.
struct DecodedTextGlyphLine {
    TextGlyphLineValueInput input;
    TextLayoutBoxBits box;
    std::vector<char16_t> text_through_decision;
    std::size_t glyph_dependency_end_code_unit{};
    std::uint32_t font_horizontal_scale_bits{};
    std::uint32_t font_vertical_scale_bits{};
    guest::GuestAddress font_sentinel{};
    std::vector<TextGlyphBucketSnapshot> font_buckets;
    std::uint64_t outer_function_fingerprint{};
    std::uint64_t helper_function_fingerprint{};
    TextGlyphRectanglePlan rectangle_plan;
    std::vector<DirectGuestMemoryRange> publication_ranges;

    bool operator==(const DecodedTextGlyphLine&) const = default;
};

[[nodiscard]] std::optional<DecodedTextGlyphLine>
decode_text_glyph_line_from_guest_values(const guest::EeMemory& memory,
                                         const TextGlyphLineValueInput& input);

[[nodiscard]] bool
text_glyph_line_publication_is_disjoint(const DecodedTextGlyphLine& line,
                                        std::span<const DirectGuestMemoryRange> additional_reads,
                                        std::span<const DirectGuestMemoryRange> additional_writes);

[[nodiscard]] TextGlyphLineGuestResult
make_text_glyph_line_prevalidated_result(const DecodedTextGlyphLine& line);

void write_text_glyph_line_prevalidated(guest::EeMemory& memory,
                                        const TextGlyphLineGuestResult& result,
                                        const std::optional<std::uint32_t> text_cursor_cell);

[[nodiscard]] TextGlyphLineGuestResult
publish_text_glyph_line_prevalidated(guest::EeMemory& memory,
                                     const DecodedTextGlyphLine& line,
                                     const std::optional<std::uint32_t> text_cursor_cell);

} // namespace text_glyph_guest_detail

// Optional whole-function translation for ordinary positive-output lines.
// Registration and every invocation require both complete retail body
// fingerprints. Calls above the practical native cost ceiling or outside the
// reviewed memory, arithmetic, stack, or ABI profile continue through the
// generated guest implementation.
[[nodiscard]] bool
register_slus_21050_text_glyph_native_function(NativeFunctionDispatcher& dispatcher,
                                               const guest::EeMemory& memory);

} // namespace takedown::runtime::slus_21050
