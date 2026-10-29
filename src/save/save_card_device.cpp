#include "takedown/save/save_card_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace takedown::save {

namespace {

// Two-digit packed BCD, as the console's clock reports it.
[[nodiscard]] std::byte to_bcd(unsigned value) noexcept {
    return static_cast<std::byte>((((value / 10U) % 10U) << 4U) | (value % 10U));
}

} // namespace

SaveCardDevice::SaveCardDevice(std::filesystem::path saves_root)
    : saves_root_(std::move(saves_root)) {}

SaveCardDevice::~SaveCardDevice() {
    std::error_code ignored;
    {
        std::lock_guard lock(mutex_);
        flush_locked(ignored);
    }
}

bool SaveCardDevice::attach_slot(std::size_t slot, std::error_code& error) {
    error.clear();
    std::lock_guard lock(mutex_);
    std::error_code flush_error;
    if (!flush_locked(flush_error)) {
        std::cerr << "Save data: could not write profile slot " << slot_.value_or(0U) << " ("
                  << flush_error.message() << ")\n";
    }
    slot_.reset();
    image_.clear();
    last_written_.clear();
    dirty_ = false;

    auto directories = read_profile_slot(saves_root_, slot, error);
    if (!directories) {
        return false;
    }
    auto image = build_ps2_card_image(*directories, error);
    if (!image) {
        return false;
    }
    image_ = std::move(*image);
    last_written_ = std::move(*directories);
    slot_ = slot;
    new_card_pending_ = true;
    return true;
}

void SaveCardDevice::detach() {
    std::lock_guard lock(mutex_);
    std::error_code error;
    if (!flush_locked(error)) {
        std::cerr << "Save data: could not write profile slot " << slot_.value_or(0U) << " ("
                  << error.message() << ")\n";
    }
    slot_.reset();
    image_.clear();
    last_written_.clear();
    dirty_ = false;
}

std::optional<std::size_t> SaveCardDevice::attached_slot() const {
    std::lock_guard lock(mutex_);
    return slot_;
}

void SaveCardDevice::disable_persistence_for_session() {
    std::lock_guard lock(mutex_);
    persistence_disabled_ = true;
    dirty_ = false;
}

bool SaveCardDevice::persistence_disabled() const {
    std::lock_guard lock(mutex_);
    return persistence_disabled_;
}

std::uint32_t SaveCardDevice::probe() {
    std::lock_guard lock(mutex_);
    ++statistics_.probes;
    if (!slot_) {
        return kProbeAbsent;
    }
    if (dirty_) {
        // libmc2 probes before each operation, so the filesystem is consistent here.
        std::error_code error;
        if (!flush_locked(error)) {
            std::cerr << "Save data: could not write profile slot " << *slot_ << " ("
                      << error.message() << ")\n";
        }
    }
    if (new_card_pending_) {
        new_card_pending_ = false;
        return kProbeNewCard;
    }
    return kProbeSameCard;
}

bool SaveCardDevice::read_pages(std::uint32_t first_page, std::uint32_t page_count,
                                std::span<std::byte> out) {
    std::lock_guard lock(mutex_);
    const std::size_t bytes = static_cast<std::size_t>(page_count) * kCardPageSize;
    if (!slot_ || first_page >= kCardPageCount ||
        page_count > kCardPageCount - static_cast<std::size_t>(first_page) ||
        out.size() < bytes) {
        return false;
    }
    std::memcpy(out.data(), image_.data() + static_cast<std::size_t>(first_page) * kCardPageSize,
                bytes);
    statistics_.page_reads += page_count;
    return true;
}

bool SaveCardDevice::write_pages(std::uint32_t first_page, std::span<const std::byte> data) {
    std::lock_guard lock(mutex_);
    if (!slot_ || data.size() % kCardPageSize != 0U || first_page >= kCardPageCount) {
        return false;
    }
    if (data.size() / kCardPageSize > kCardPageCount - static_cast<std::size_t>(first_page)) {
        return false;
    }
    std::memcpy(image_.data() + static_cast<std::size_t>(first_page) * kCardPageSize, data.data(),
                data.size());
    statistics_.page_writes += data.size() / kCardPageSize;
    dirty_ = true;
    last_write_ = Clock::now();
    return true;
}

bool SaveCardDevice::erase_block(std::uint32_t block) {
    std::lock_guard lock(mutex_);
    if (!slot_ || block >= kCardBlockCount) {
        return false;
    }
    constexpr std::size_t kBlockBytes = kCardPagesPerBlock * kCardPageSize;
    std::fill_n(image_.data() + static_cast<std::size_t>(block) * kBlockBytes, kBlockBytes,
                std::byte{0xFFU});
    ++statistics_.block_erases;
    dirty_ = true;
    last_write_ = Clock::now();
    return true;
}

std::array<std::byte, 8U> SaveCardDevice::clock_bcd() const {
    const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    const CardTimestamp time = card_timestamp_from_unix(unix_seconds);
    return {
        std::byte{0U},
        to_bcd(time.second),
        to_bcd(time.minute),
        to_bcd(time.hour),
        std::byte{0U},
        to_bcd(time.day),
        to_bcd(time.month),
        to_bcd(static_cast<unsigned>(time.year % 100U)),
    };
}

bool SaveCardDevice::flush(std::error_code& error) {
    std::lock_guard lock(mutex_);
    return flush_locked(error);
}

void SaveCardDevice::flush_if_idle(Clock::time_point now, Clock::duration quiet_period) {
    std::lock_guard lock(mutex_);
    if (!dirty_ || now - last_write_ < quiet_period) {
        return;
    }
    std::error_code error;
    if (!flush_locked(error)) {
        // Back off for another quiet period instead of retrying on every tick.
        last_write_ = now;
    }
}

bool SaveCardDevice::dirty() const {
    std::lock_guard lock(mutex_);
    return dirty_;
}

SaveCardDevice::Statistics SaveCardDevice::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

std::vector<std::byte> SaveCardDevice::image_copy() const {
    std::lock_guard lock(mutex_);
    return image_;
}

bool SaveCardDevice::flush_locked(std::error_code& error) {
    error.clear();
    if (persistence_disabled_) {
        dirty_ = false;
        return true;
    }
    if (!slot_ || !dirty_) {
        return true;
    }
    auto directories = read_ps2_card_directories(image_, error);
    if (!directories) {
        ++statistics_.failed_flushes;
        return false;
    }
    if (*directories != last_written_ &&
        !write_profile_slot(saves_root_, *slot_, *directories, error)) {
        ++statistics_.failed_flushes;
        return false;
    }
    last_written_ = std::move(*directories);
    dirty_ = false;
    ++statistics_.flushes;
    return true;
}

} // namespace takedown::save
