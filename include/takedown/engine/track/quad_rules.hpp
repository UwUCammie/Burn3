#pragma once

// Which track strip quads a driver may enter. Every quad has a kind (three
// bits of its record): kind 5 is closed to everyone, and kinds 4, 1 and 3
// are closed or only discouraged depending on the driver's rules. Quads also
// name their neighbour across each side, which is how a driver finds a lane
// to move into.

#include <cstdint>

namespace takedown::engine {

enum QuadEntry : std::uint32_t {
    kQuadEntryDiscouraged = 1U,
    kQuadEntryClosed = 4U,
};

constexpr std::uint8_t kQuadKindClosed = 5U;
constexpr std::uint8_t kQuadKindRestricted = 4U;  // closed or discouraged by the rules
constexpr std::uint8_t kQuadKindSideA = 1U;       // kinds 1 and 3 share a rule
constexpr std::uint8_t kQuadKindSideB = 3U;

struct QuadRules {
    // Off: only kind 4 matters, and it is merely discouraged.
    bool apply{};
    bool restricted_allowed{};
    bool restricted_discouraged_only{};
    bool side_kinds_allowed{};
    bool side_kinds_active{};
    bool side_kinds_discouraged_only{};
};

// The QuadEntry bits of a quad of `kind` under `rules`.
[[nodiscard]] constexpr std::uint32_t quad_entry(const std::uint8_t kind, const QuadRules& rules) noexcept {
    if (kind == kQuadKindClosed) {
        return kQuadEntryClosed;
    }
    if (!rules.apply) {
        return kind == kQuadKindRestricted ? kQuadEntryDiscouraged : 0U;
    }
    std::uint32_t entry = 0U;
    if (!rules.restricted_allowed && kind == kQuadKindRestricted) {
        entry |= rules.restricted_discouraged_only ? kQuadEntryDiscouraged : kQuadEntryClosed;
    }
    if (!rules.side_kinds_allowed && (kind == kQuadKindSideA || kind == kQuadKindSideB) && rules.side_kinds_active) {
        entry |= rules.side_kinds_discouraged_only ? kQuadEntryDiscouraged : kQuadEntryClosed;
    }
    return entry;
}

struct RouteAhead {
    bool open{};
    std::int32_t quads_tested{};
};

// Whether none of the `count` quads from `quad` on (wrapping round a closed
// strip of `quad_count` quads; an open strip ends the test early, open) is
// of the closed kind. `kind(quad)` reads a quad's kind.
template <class Kind>
[[nodiscard]] RouteAhead route_open_ahead(Kind&& kind, const std::int32_t quad_count, const bool closed_loop,
                                          std::uint16_t quad, std::uint16_t count) {
    RouteAhead ahead{true, 0};
    while (count != 0U) {
        --count;
        ++ahead.quads_tested;
        if (kind(quad) == kQuadKindClosed) {
            ahead.open = false;
            return ahead;
        }
        quad = static_cast<std::uint16_t>(quad + 1U);
        if (quad >= quad_count) {
            if (!closed_loop) {
                return ahead;
            }
            quad = 0U;
        }
    }
    return ahead;
}

} // namespace takedown::engine
