#pragma once

// The page device behind the game's save/load: what the IOP's MC2_S1 module
// offers the EE save library (libmc2), backed by the active profile slot.
//
// How the game's save/load reaches the host: Burnout 3 links libmc2, whose
// filesystem code runs on the EE ("Sony PS2 Memory Card Format" is checked
// and written by EE code). The library sends page-level requests through the
// DBCMAN companion service (SIF RPC 0x8000131c, 0x2090-byte transactions):
//   op 4  probe (0 absent, 1 same card, 2 new card)    op 3  spec
//   op 6  geometry handshake after mounting             op 1  read pages
//   op 2  write pages                                   op 9  erase block
//   op 10 real-time clock (BCD)                         op 12 completion check
// takedown::guest::EeKernelHle answers those requests from this device; see
// docs/SAVE_DATA.md for the request/response layouts.
//
// The device keeps one in-memory card image (512-byte pages, no spare area):
// attaching a slot formats a fresh image holding the slot's save
// directories, and flushing reads the save directories back out of the image
// and writes them into the slot folder (write_profile_slot). A flush runs at
// the next probe after a write (libmc2 probes before each operation, so the
// filesystem is consistent there), after a quiet period, and on detach.
//
// Thread safety: all members lock one mutex; the guest thread issues page
// operations while the presentation thread may flush.

#include "takedown/save/profile_slots.hpp"
#include "takedown/save/ps2_card_filesystem.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::save {

class SaveCardDevice {
  public:
    // Probe answers, as libmc2 reads them.
    static constexpr std::uint32_t kProbeAbsent = 0U;
    static constexpr std::uint32_t kProbeSameCard = 1U;
    static constexpr std::uint32_t kProbeNewCard = 2U;

    using Clock = std::chrono::steady_clock;

    explicit SaveCardDevice(std::filesystem::path saves_root);
    ~SaveCardDevice();
    SaveCardDevice(const SaveCardDevice&) = delete;
    SaveCardDevice& operator=(const SaveCardDevice&) = delete;

    [[nodiscard]] const std::filesystem::path& saves_root() const noexcept { return saves_root_; }

    // Flushes any current slot, then presents the given slot (1-based) as a
    // freshly formatted card holding its save directories. The next probe
    // reports a new card. On failure the device is left without a card.
    bool attach_slot(std::size_t slot, std::error_code& error);
    // Flushes and removes the card (probes then report it absent).
    void detach();
    [[nodiscard]] std::optional<std::size_t> attached_slot() const;

    // One-way for this device's lifetime. Guest saves continue in the volatile
    // card image, but no probe, flush, slot change or destructor can persist
    // them. Enable before applying any session-only developer unlocks.
    void disable_persistence_for_session();
    [[nodiscard]] bool persistence_disabled() const;

    // MC2_S1 page operations. Pages are kCardPageSize bytes.
    [[nodiscard]] std::uint32_t probe();
    bool read_pages(std::uint32_t first_page, std::uint32_t page_count, std::span<std::byte> out);
    bool write_pages(std::uint32_t first_page, std::span<const std::byte> data);
    bool erase_block(std::uint32_t block);
    // The PS2 real-time clock reply: status, second, minute, hour, pad, day,
    // month, year (two digits), BCD, in Japan Standard Time like the console.
    [[nodiscard]] std::array<std::byte, 8U> clock_bcd() const;

    // Writes the card's save directories into the slot folder when the game
    // wrote since the last flush. Returns false (dirty state kept) when the
    // image does not parse or the slot cannot be written.
    bool flush(std::error_code& error);
    // flush() when the last write is at least quiet_period old.
    void flush_if_idle(Clock::time_point now, Clock::duration quiet_period);
    [[nodiscard]] bool dirty() const;

    struct Statistics {
        std::uint64_t probes{};
        std::uint64_t page_reads{};
        std::uint64_t page_writes{};
        std::uint64_t block_erases{};
        std::uint64_t flushes{};
        std::uint64_t failed_flushes{};
    };
    [[nodiscard]] Statistics statistics() const;

    // The current card image (for tests and diagnostics).
    [[nodiscard]] std::vector<std::byte> image_copy() const;

  private:
    bool flush_locked(std::error_code& error);

    std::filesystem::path saves_root_;
    mutable std::mutex mutex_;
    std::optional<std::size_t> slot_;
    std::vector<std::byte> image_;
    std::vector<CardSaveDirectory> last_written_;
    bool new_card_pending_{};
    bool dirty_{};
    bool persistence_disabled_{};
    Clock::time_point last_write_{};
    Statistics statistics_{};
};

} // namespace takedown::save
