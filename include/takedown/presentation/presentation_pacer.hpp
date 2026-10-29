#pragma once

#include <cstdint>
#include <optional>

namespace takedown::presentation {

// Host-only presentation deadline scheduler. It neither observes nor advances
// guest time: callers continue to pump events and renderer completions before
// asking whether an SDL/window presentation should be emitted.
class PresentationPacer {
  public:
    // nullopt is uncapped. Capped rates use the GraphicsSettings domain of
    // 1..1000 frames per second; invalid direct construction is rejected.
    explicit PresentationPacer(std::optional<std::uint32_t> fps_limit);

    // Returns true when this call may present. A true capped result consumes
    // one deadline and moves directly to the first deadline after `now`, so a
    // late caller drops missed presentation slots instead of issuing a burst.
    // A forced result bypasses the cap without changing its schedule; this is
    // used for the final host-window presentation during shutdown.
    [[nodiscard]] bool should_present(std::uint64_t monotonic_nanoseconds,
                                      bool force = false) noexcept;

    // Establishes a fresh capped epoch at `monotonic_nanoseconds`, as if a
    // frame had just been presented there. The next ordinary presentation is
    // due one exact rational interval later. This is suitable for excluding a
    // host pause or recovering from an explicitly identified clock reset.
    void resync(std::uint64_t monotonic_nanoseconds) noexcept;

    [[nodiscard]] std::optional<std::uint32_t> fps_limit() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_deadline_nanoseconds() const noexcept;

  private:
    [[nodiscard]] std::uint64_t deadline_for_ordinal(std::uint64_t ordinal) const noexcept;
    [[nodiscard]] std::uint64_t first_ordinal_after(std::uint64_t now) const noexcept;

    std::optional<std::uint32_t> fps_limit_;
    std::optional<std::uint64_t> epoch_nanoseconds_;
    std::optional<std::uint64_t> next_deadline_nanoseconds_;
};

} // namespace takedown::presentation
