#include "takedown/guest/gs_local_memory.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace takedown::guest {

namespace {

// Identities are process-wide so a checkpoint from one GsLocalMemory can never
// authenticate a different tracker. Zero is reserved as the exhausted sentinel.
std::atomic<std::uint64_t> next_mutation_tracker_identity{1U};

[[nodiscard]] std::uint64_t allocate_mutation_tracker_identity() {
    auto current = next_mutation_tracker_identity.load(std::memory_order_relaxed);
    while (current != 0U) {
        const auto next = current == std::numeric_limits<std::uint64_t>::max() ? 0U : current + 1U;
        if (next_mutation_tracker_identity.compare_exchange_weak(current, next,
                                                                 std::memory_order_relaxed,
                                                                 std::memory_order_relaxed)) {
            return current;
        }
    }
    throw std::overflow_error("GS local-memory mutation tracker identity space exhausted");
}

struct Psmz32InPageAddress {
    std::uint8_t block{};
    std::uint8_t byte{};
};

// PSMZ32 keeps a 64x32 page as 32 eight-by-eight 256-byte blocks. The in-page
// layout is shared by the depth fast path and the generic pixel address path.
constexpr std::array<Psmz32InPageAddress, 2048> make_psmz32_in_page_addresses() noexcept {
    std::array<Psmz32InPageAddress, 2048> table{};
    for (std::uint32_t y = 0; y < 32U; ++y) {
        for (std::uint32_t x = 0; x < 64U; ++x) {
            const std::uint32_t block_x = x >> 3U;
            const std::uint32_t block_y = y >> 3U;
            const std::uint32_t local_x = x & 7U;
            const std::uint32_t local_y = y & 7U;
            const std::uint32_t block = (block_x & 1U) | ((block_y & 1U) << 1U) |
                                        ((block_x & 2U) << 1U) | ((block_y & 2U) << 2U) |
                                        ((block_x & 4U) << 2U);
            const std::uint32_t index = (local_x & 1U) | ((local_y & 1U) << 1U) |
                                        ((local_x & 2U) << 1U) | ((local_x & 4U) << 1U) |
                                        ((local_y & 2U) << 3U) | ((local_y & 4U) << 3U);
            table[y * 64U + x] = Psmz32InPageAddress{
                .block = static_cast<std::uint8_t>(block),
                .byte = static_cast<std::uint8_t>(index * 4U),
            };
        }
    }
    return table;
}

constexpr auto kPsmz32InPageAddresses = make_psmz32_in_page_addresses();

GsLocalMemoryStatus surface_status(const GsLocalMemorySurface& surface) noexcept {
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmct24:
    case kGsPsmct16:
    case kGsPsmct16s:
    case kGsPsmt8:
    case kGsPsmt4:
    case kGsPsmt8h:
        break;
    default:
        return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
    }
    if (surface.buffer_width == 0U) {
        return GsLocalMemoryStatus::ZeroBufferWidth;
    }
    if (surface.buffer_width >= 64U || surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
        return GsLocalMemoryStatus::InvalidBasePointer;
    }
    return GsLocalMemoryStatus::Applied;
}

} // namespace

struct GsLocalMemory::MutationTracker {
    MutationTracker(const std::uint64_t tracker_identity, const std::uint64_t maximum)
        : identity(tracker_identity), maximum_epoch(maximum), completed_epoch(0U), active_epoch(0U),
          exhausted(maximum == 0U) {
        for (auto& last_write : block_last_write) {
            last_write.store(0U, std::memory_order_relaxed);
        }
    }

    std::array<std::atomic<std::uint64_t>, kGsLocalMemoryBlocks> block_last_write{};
    std::uint64_t identity;
    std::uint64_t maximum_epoch;
    std::uint64_t completed_epoch;
    std::uint64_t active_epoch;
    bool exhausted;
};

const char* to_string(const GsLocalMemoryStatus status) noexcept {
    switch (status) {
    case GsLocalMemoryStatus::Applied:
        return "applied";
    case GsLocalMemoryStatus::UnsupportedPixelStorageMode:
        return "unsupported pixel storage mode";
    case GsLocalMemoryStatus::ZeroBufferWidth:
        return "zero buffer width";
    case GsLocalMemoryStatus::InvalidBasePointer:
        return "invalid GS base pointer or buffer width";
    case GsLocalMemoryStatus::InvalidGeometry:
        return "invalid transfer geometry";
    case GsLocalMemoryStatus::InvalidPayload:
        return "invalid transfer payload";
    }
    return "unknown GS local-memory status";
}

GsLocalMemory::GsLocalMemory() : raw_backing_(kGsLocalMemoryBytes, std::uint8_t{}) {}

GsLocalMemory::~GsLocalMemory() = default;

GsLocalMemory::GsLocalMemory(const GsLocalMemory& other) : raw_backing_(other.raw_backing_) {}

GsLocalMemory& GsLocalMemory::operator=(const GsLocalMemory& other) {
    if (this != &other) {
        raw_backing_ = other.raw_backing_;
        mutation_tracker_.reset();
    }
    return *this;
}

GsLocalMemory::GsLocalMemory(GsLocalMemory&& other) noexcept
    : raw_backing_(std::move(other.raw_backing_)),
      mutation_tracker_(std::move(other.mutation_tracker_)) {}

GsLocalMemory& GsLocalMemory::operator=(GsLocalMemory&& other) noexcept {
    raw_backing_ = std::move(other.raw_backing_);
    mutation_tracker_ = std::move(other.mutation_tracker_);
    return *this;
}

void GsLocalMemory::enable_mutation_tracking(const std::uint64_t maximum_epoch) {
    if (mutation_tracker_ != nullptr) {
        return;
    }

    mutation_tracker_ =
        std::make_unique<MutationTracker>(allocate_mutation_tracker_identity(), maximum_epoch);
}

void GsLocalMemory::disable_mutation_tracking() noexcept {
    mutation_tracker_.reset();
}

GsLocalMemoryMutationCheckpoint GsLocalMemory::capture_mutation_checkpoint() const noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->exhausted || tracker->active_epoch != 0U) {
        // Untrackable: the caller must fall back to exact comparison.
    } else {
        return GsLocalMemoryMutationCheckpoint{tracker->identity, tracker->completed_epoch};
    }
    return {};
}

GsLocalMemoryMutationStatus
GsLocalMemory::classify_mutations_since(const GsLocalMemoryMutationCheckpoint checkpoint,
                                        const GsLocalMemoryBlockFootprint& footprint) const noexcept {
    const GsLocalMemoryMutationStatus any_mutation = classify_any_mutations_since(checkpoint);
    if (any_mutation != GsLocalMemoryMutationStatus::Changed) {
        return any_mutation;
    }

    for (std::size_t block = 0; block != kGsLocalMemoryBlocks; ++block) {
        if (footprint.test(block)) {
            if (mutation_tracker_->block_last_write[block].load(std::memory_order_relaxed) > checkpoint.completed_epoch_) {
                return GsLocalMemoryMutationStatus::Changed;
            }
        }
    }
    return GsLocalMemoryMutationStatus::Unchanged;
}

GsLocalMemoryMutationStatus
GsLocalMemory::classify_any_mutations_since(const GsLocalMemoryMutationCheckpoint checkpoint) const noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->exhausted || tracker->active_epoch != 0U) {
        return GsLocalMemoryMutationStatus::Untrackable;
    }
    if (checkpoint.tracker_identity_ == 0U) {
        return GsLocalMemoryMutationStatus::Untrackable;
    }
    if (tracker->identity != checkpoint.tracker_identity_) {
        return GsLocalMemoryMutationStatus::Untrackable;
    }
    if (checkpoint.completed_epoch_ > tracker->completed_epoch) {
        return GsLocalMemoryMutationStatus::Untrackable;
    }
    if (checkpoint.completed_epoch_ == tracker->completed_epoch) {
        return GsLocalMemoryMutationStatus::Unchanged;
    }
    return GsLocalMemoryMutationStatus::Changed;
}

GsLocalMemory::MutationBatch::MutationBatch(GsLocalMemory& memory) noexcept
    : memory_(&memory), outermost_(memory.open_mutation_batch()) {}

GsLocalMemory::MutationBatch::~MutationBatch() {
    if (memory_ != nullptr && outermost_) {
        memory_->close_mutation_batch();
    }
}

GsLocalMemory::MutationBatch::MutationBatch(MutationBatch&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr)), outermost_(other.outermost_) {}

GsLocalMemory::MutationBatch GsLocalMemory::begin_mutation_batch() noexcept {
    return MutationBatch(*this);
}

bool GsLocalMemory::open_mutation_batch() noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->active_epoch != 0U || tracker->exhausted) {
        return false;
    }
    if (tracker->completed_epoch >= tracker->maximum_epoch) {
        tracker->exhausted = true;
        return false;
    }
    tracker->active_epoch = tracker->completed_epoch + 1U;
    return true;
}

void GsLocalMemory::close_mutation_batch() noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->active_epoch == 0U) {
        return;
    }
    tracker->completed_epoch = tracker->active_epoch;
    tracker->active_epoch = 0U;
}

void GsLocalMemory::record_mutated_byte(const std::size_t byte) noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->active_epoch == 0U || byte >= kGsLocalMemoryBytes) {
        return;
    }
    if (tracker->block_last_write[byte >> 8U].load(std::memory_order_relaxed) != mutation_tracker_->active_epoch) {
        tracker->block_last_write[byte >> 8U].store(mutation_tracker_->active_epoch, std::memory_order_relaxed);
    }
}

void GsLocalMemory::record_all_blocks_mutated() noexcept {
    MutationTracker* tracker = mutation_tracker_.get();
    if (tracker == nullptr || tracker->active_epoch == 0U) {
        return;
    }
    for (auto& last_write : tracker->block_last_write) {
        last_write.store(mutation_tracker_->active_epoch, std::memory_order_relaxed);
    }
}

// RECOVERY: 8/4/0 tail join and nibble statement order differ from original; semantics match.
GsLocalMemoryApplyResult GsLocalMemory::apply_host_to_local(const GsImageTransfer& transfer) {
    const GsLocalMemorySurface surface{
        .base_pointer = transfer.destination_base_pointer,
        .buffer_width = transfer.destination_buffer_width,
        .pixel_storage_mode = transfer.destination_psm,
    };
    const GsLocalMemoryStatus surface_status = takedown::guest::surface_status(surface);
    if (surface_status != GsLocalMemoryStatus::Applied) {
        return GsLocalMemoryApplyResult{.status = surface_status};
    }
    if (transfer.width == 0U || transfer.height == 0U) {
        return GsLocalMemoryApplyResult{.status = GsLocalMemoryStatus::InvalidGeometry};
    }

    const auto pixel_count = static_cast<std::uint64_t>(transfer.width) * transfer.height;
    const auto expected_bytes = [](const std::uint32_t psm, const std::uint64_t count) noexcept
        -> std::optional<std::size_t> {
        const unsigned int pixel_bits = [](const std::uint32_t p) noexcept -> unsigned int {
            unsigned int bits{};
            if (p != kGsPsmct32 && p != kGsPsmz32) {
                if (p != kGsPsmct24 && p != kGsPsmz24) {
                    if (p != kGsPsmct16 && p != kGsPsmct16s) {
                        if (p != kGsPsmz16 && p != kGsPsmz16s) {
                            bits = [](const std::uint32_t q) noexcept -> unsigned int {
                                if (q != kGsPsmt8 && q != kGsPsmt8h) {
                                    if (q == kGsPsmt4) {
                                        return 4U;
                                    }
                                    return 0U;
                                }
                                return 8U;
                            }(p);
                        } else {
                            bits = 16U;
                        }
                    } else {
                        bits = 16U;
                    }
                } else {
                    bits = 24U;
                }
            } else {
                bits = 32U;
            }
            return bits;
        }(psm);
        if (pixel_bits == 0U || count > (static_cast<std::uint64_t>(kGsLocalMemoryBytes) * 8U) / pixel_bits) {
            return std::nullopt;
        }
        const auto bytes = (count * pixel_bits + 7U) >> 3U;
        return static_cast<std::size_t>(bytes);
    }(transfer.destination_psm, pixel_count);
    if (!expected_bytes) {
        return GsLocalMemoryApplyResult{.status = GsLocalMemoryStatus::InvalidGeometry};
    }
    if (*expected_bytes != transfer.pixels.size()) {
        return GsLocalMemoryApplyResult{.status = GsLocalMemoryStatus::InvalidPayload};
    }

    auto batch = begin_mutation_batch();
    const unsigned int pixel_bits = [](const std::uint32_t p) noexcept -> unsigned int {
        unsigned int bits{};
        if (p != kGsPsmct32 && p != kGsPsmz32) {
            if (p != kGsPsmct24 && p != kGsPsmz24) {
                if (p != kGsPsmct16 && p != kGsPsmct16s) {
                    if (p != kGsPsmz16 && p != kGsPsmz16s) {
                        bits = [](const std::uint32_t q) noexcept -> unsigned int {
                            if (q != kGsPsmt8 && q != kGsPsmt8h) {
                                if (q == kGsPsmt4) {
                                    return 4U;
                                }
                                return 0U;
                            }
                            return 8U;
                        }(p);
                    } else {
                        bits = 16U;
                    }
                } else {
                    bits = 16U;
                }
            } else {
                bits = 24U;
            }
        } else {
            bits = 32U;
        }
        return bits;
    }(transfer.destination_psm);
    std::uint64_t source_pixel{};
    for (std::uint32_t row = 0; row < transfer.height; ++row) {
        const auto y = static_cast<std::uint64_t>(transfer.destination_y) + row;
        for (std::uint32_t column = 0; column < transfer.width; ++column) {
            const auto x = static_cast<std::uint64_t>(transfer.destination_x) + column;
            const auto destination = pixel_address(surface, x, y);
            if (!destination) {
                return GsLocalMemoryApplyResult{.status = GsLocalMemoryStatus::InvalidGeometry};
            }
            if (transfer.destination_psm == kGsPsmt4) {
                const unsigned char packed_source = transfer.pixels[source_pixel >> 1U];
                const unsigned char nibble_mask = static_cast<unsigned char>(15 << destination->nibble_shift);
                raw_backing_[destination->byte] = static_cast<unsigned char>(
                    (raw_backing_[destination->byte] & ~nibble_mask) |
                    (static_cast<unsigned int>((static_cast<int>(packed_source) >> ((source_pixel & 1U) * 4U)) & 15) << destination->nibble_shift));
            } else {
                const auto source_offset = static_cast<std::uint64_t>(pixel_bits >> 3U) * source_pixel;
                std::copy(transfer.pixels.data() + source_offset, transfer.pixels.data() + source_offset + (pixel_bits >> 3U),
                          raw_backing_.data() + destination->byte);
            }
            record_mutated_byte(destination->byte);
            ++source_pixel;
        }
    }
    return GsLocalMemoryApplyResult{.status = GsLocalMemoryStatus::Applied, .bytes_written = *expected_bytes};
}

// RECOVERY: not identical: closest rebuild. read_transfer_pixel/write_transfer_pixel are
// undefined in this copy (their inlined read body differs), and the branch shape of the
// status/pixel-bits checks differs slightly, so the byte-count local 'bytes' is not kept.
GsLocalMemoryApplyResult GsLocalMemory::apply_local_to_local(const GsLocalToLocalTransfer& transfer) noexcept {
    struct GsPageDimensions {
        std::uint32_t width;
        std::uint32_t height;
    };

    // The local-to-local path accepts the depth PSMs (48, 49, 50, 58) as well
    // as the colour PSMs, so it cannot reuse surface_status() unchanged.
    const GsLocalMemoryStatus source_status = [](const GsLocalMemorySurface& surface) noexcept
        -> GsLocalMemoryStatus {
        switch (surface.pixel_storage_mode) {
        case kGsPsmct32:
        case kGsPsmct24:
        case kGsPsmct16:
        case kGsPsmct16s:
        case kGsPsmt8:
        case kGsPsmt4:
        case kGsPsmt8h:
            break;
        default:
            if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
                break;
            }
            return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
        }
        if (surface.buffer_width == 0U) {
            return GsLocalMemoryStatus::ZeroBufferWidth;
        }
        if (surface.buffer_width >= 64U || surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
            return GsLocalMemoryStatus::InvalidBasePointer;
        }
        return GsLocalMemoryStatus::Applied;
    }(transfer.source);
    if (source_status != GsLocalMemoryStatus::Applied) {
        return {source_status};
    }
    const GsLocalMemoryStatus destination_status = [](const GsLocalMemorySurface& surface) noexcept
        -> GsLocalMemoryStatus {
        switch (surface.pixel_storage_mode) {
        case kGsPsmct32:
        case kGsPsmct24:
        case kGsPsmct16:
        case kGsPsmct16s:
        case kGsPsmt8:
        case kGsPsmt4:
        case kGsPsmt8h:
            break;
        default:
            if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
                break;
            }
            return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
        }
        if (surface.buffer_width == 0U) {
            return GsLocalMemoryStatus::ZeroBufferWidth;
        }
        if (surface.buffer_width >= 64U || surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
            return GsLocalMemoryStatus::InvalidBasePointer;
        }
        return GsLocalMemoryStatus::Applied;
    }(transfer.destination);
    if (destination_status != GsLocalMemoryStatus::Applied) {
        return {destination_status};
    }
    if (transfer.width == 0U || transfer.height == 0U) {
        return {GsLocalMemoryStatus::InvalidGeometry};
    }

    const unsigned long long pixel_count = static_cast<unsigned long long>(transfer.width) *
                                           static_cast<unsigned long long>(transfer.height);
    const unsigned int pixel_bits = [&transfer]() noexcept -> unsigned int {
        if (transfer.destination.pixel_storage_mode != kGsPsmct32 &&
            transfer.destination.pixel_storage_mode != kGsPsmz32) {
            if (transfer.destination.pixel_storage_mode != kGsPsmct24 &&
                transfer.destination.pixel_storage_mode != kGsPsmz24) {
                if (transfer.destination.pixel_storage_mode != kGsPsmct16 &&
                    transfer.destination.pixel_storage_mode != kGsPsmct16s) {
                    if (transfer.destination.pixel_storage_mode != kGsPsmz16 &&
                        transfer.destination.pixel_storage_mode != kGsPsmz16s) {
                        if (transfer.destination.pixel_storage_mode != kGsPsmt8 &&
                            transfer.destination.pixel_storage_mode != kGsPsmt8h) {
                            if (transfer.destination.pixel_storage_mode == kGsPsmt4) {
                                return 4U;
                            }
                            return 0U;
                        }
                        return 8U;
                    }
                    return 16U;
                }
                return 16U;
            }
            return 24U;
        }
        return 32U;
    }();
    const std::optional<unsigned long long> destination_bytes =
        [pixel_bits, pixel_count]() noexcept -> std::optional<unsigned long long> {
        if (pixel_bits == 0U || pixel_count > kGsLocalMemoryBytes * 8U / pixel_bits) {
            return std::nullopt;
        }
        const unsigned long long bytes = (pixel_count * pixel_bits + 7U) / 8U;
        return std::optional<unsigned long long>(std::in_place, bytes);
    }();
    if (!destination_bytes.has_value()) {
        return {GsLocalMemoryStatus::InvalidGeometry};
    }

    auto mutation_batch = begin_mutation_batch();

    const long long width = transfer.width;
    const long long height = transfer.height;
    std::int64_t source_x = transfer.source_x;
    std::int64_t source_y = transfer.source_y;
    std::int64_t destination_x = transfer.destination_x;
    std::int64_t destination_y = transfer.destination_y;

    std::int64_t x_increment = 1;
    if (transfer.reverse_x &&
        (!([&transfer, width, height]() noexcept -> bool {
            return transfer.source.base_pointer == transfer.destination.base_pointer &&
                 static_cast<std::int64_t>(transfer.source_x) <
                     static_cast<std::int64_t>(transfer.destination_x) + width &&
                 static_cast<std::int64_t>(transfer.destination_x) <
                     static_cast<std::int64_t>(transfer.source_x) + width &&
                 static_cast<std::int64_t>(transfer.source_y) <
                     static_cast<std::int64_t>(transfer.destination_y) + height &&
                 static_cast<std::int64_t>(transfer.destination_y) <
                     static_cast<std::int64_t>(transfer.source_y) + height;
        }()) ||
         source_x < destination_x)) {
        x_increment = -1;
        source_x += width - 1;
        destination_x += width - 1;
    }
    std::int64_t y_increment = 1;
    if (transfer.reverse_y &&
        (!([&transfer, width, height]() noexcept -> bool {
            return transfer.source.base_pointer == transfer.destination.base_pointer &&
                 static_cast<std::int64_t>(transfer.source_x) <
                     static_cast<std::int64_t>(transfer.destination_x) + width &&
                 static_cast<std::int64_t>(transfer.destination_x) <
                     static_cast<std::int64_t>(transfer.source_x) + width &&
                 static_cast<std::int64_t>(transfer.source_y) <
                     static_cast<std::int64_t>(transfer.destination_y) + height &&
                 static_cast<std::int64_t>(transfer.destination_y) <
                     static_cast<std::int64_t>(transfer.source_y) + height;
        }()) ||
         source_y < destination_y)) {
        y_increment = -1;
        source_y += height - 1;
        destination_y += height - 1;
    }

    const auto copy_pixel = [this, &transfer](const std::int64_t source_x_in,
                                              const std::int64_t source_y_in,
                                              const std::int64_t destination_x_in,
                                              const std::int64_t destination_y_in) noexcept {
        const unsigned int sx = static_cast<unsigned int>(source_x_in) & 2047U;
        const unsigned int sy = static_cast<unsigned int>(source_y_in) & 2047U;
        const unsigned int dx = static_cast<unsigned int>(destination_x_in) & 2047U;
        const unsigned int dy = static_cast<unsigned int>(destination_y_in) & 2047U;
        const auto pixel = read_transfer_pixel(transfer.source, sx, sy);
        if (pixel) {
            write_transfer_pixel(transfer.destination, dx, dy, *pixel);
        }
    };

    if (x_increment == 1) {
        const GsPageDimensions page = [&transfer]() noexcept -> GsPageDimensions {
            switch (transfer.destination.pixel_storage_mode) {
            case kGsPsmct16:
            case kGsPsmct16s:
            case kGsPsmz16:
            case kGsPsmz16s:
                return {64U, 64U};
            case kGsPsmt8:
                return {128U, 64U};
            case kGsPsmt4:
                return {128U, 128U};
            default:
                return {64U, 32U};
            }
        }();
        const long long source_page_x = source_x & ~static_cast<long long>(page.width - 1U);
        const long long source_page_y = source_y & ~static_cast<long long>(page.height - 1U);
        if (y_increment == 1 &&
            ([&transfer, width, height]() noexcept -> bool {
            return transfer.source.base_pointer == transfer.destination.base_pointer &&
                 static_cast<std::int64_t>(transfer.source_x) <
                     static_cast<std::int64_t>(transfer.destination_x) + width &&
                 static_cast<std::int64_t>(transfer.destination_x) <
                     static_cast<std::int64_t>(transfer.source_x) + width &&
                 static_cast<std::int64_t>(transfer.source_y) <
                     static_cast<std::int64_t>(transfer.destination_y) + height &&
                 static_cast<std::int64_t>(transfer.destination_y) <
                     static_cast<std::int64_t>(transfer.source_y) + height;
        }()) &&
            source_y < destination_y &&
            destination_y < source_page_y + static_cast<long long>(page.height)) {
            y_increment = -1;
            source_y += height - 1;
            destination_y += height - 1;
        }

        for (std::int64_t row = 0; height > row; ++row) {
            if (([&transfer, width, height]() noexcept -> bool {
            return transfer.source.base_pointer == transfer.destination.base_pointer &&
                 static_cast<std::int64_t>(transfer.source_x) <
                     static_cast<std::int64_t>(transfer.destination_x) + width &&
                 static_cast<std::int64_t>(transfer.destination_x) <
                     static_cast<std::int64_t>(transfer.source_x) + width &&
                 static_cast<std::int64_t>(transfer.source_y) <
                     static_cast<std::int64_t>(transfer.destination_y) + height &&
                 static_cast<std::int64_t>(transfer.destination_y) <
                     static_cast<std::int64_t>(transfer.source_y) + height;
        }()) &&
                source_x < destination_x &&
                destination_x < source_page_x + static_cast<long long>(page.width)) {
                for (std::int64_t column = width; column-- != 0;) {
                    copy_pixel(source_x + column, source_y, destination_x + column, destination_y);
                }
            } else {
                for (std::int64_t column = 0; width > column; ++column) {
                    copy_pixel(source_x + column, source_y, destination_x + column, destination_y);
                }
            }
            source_y += y_increment;
            destination_y += y_increment;
        }
    } else {
        for (std::int64_t row = 0; height > row; ++row) {
            for (std::int64_t column = 0; width > column; ++column) {
                copy_pixel(source_x - column, source_y, destination_x - column, destination_y);
            }
            source_y += y_increment;
            destination_y += y_increment;
        }
    }

    GsLocalMemoryApplyResult result{.status = GsLocalMemoryStatus::Applied};
    result.bytes_written = *destination_bytes;
    return result;
}

std::optional<std::uint32_t> GsLocalMemory::read_transfer_pixel(const GsLocalMemorySurface& surface, const std::uint32_t x,
                                                                const std::uint32_t y) const noexcept {
    const auto address = pixel_address(surface, x, y);
    if (!address) {
        return std::nullopt;
    }
    const auto byte = address->byte;
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmz32:
        return static_cast<std::uint32_t>(raw_backing_[byte]) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 1]) << 8U) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 2]) << 16U) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 3]) << 24U);
    case kGsPsmct24:
    case kGsPsmz24:
        return static_cast<std::uint32_t>(raw_backing_[byte]) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 1]) << 8U) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 2]) << 16U);
    case kGsPsmct16:
    case kGsPsmct16s:
    case kGsPsmz16:
    case kGsPsmz16s:
        return static_cast<std::uint32_t>(raw_backing_[byte]) |
               (static_cast<std::uint32_t>(raw_backing_[byte + 1]) << 8U);
    case kGsPsmt8:
    case kGsPsmt8h:
        return raw_backing_[byte];
    case kGsPsmt4:
        return static_cast<std::uint32_t>((static_cast<int>(raw_backing_[byte]) >> address->nibble_shift) & 15);
    default:
        return std::nullopt;
    }
}

// RECOVERY: PSMT4 nibble mask/AND statement order differs from original; semantics match.
GsLocalMemoryStatus GsLocalMemory::write_transfer_pixel(const GsLocalMemorySurface& surface, const std::uint32_t x,
                                                        const std::uint32_t y, const std::uint32_t pixel) {
    const auto address = pixel_address(surface, x, y);
    if (!address) {
        return GsLocalMemoryStatus::InvalidGeometry;
    }
    auto batch = begin_mutation_batch();
    const auto byte = address->byte;
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmz32:
        raw_backing_[byte + 3] = static_cast<std::uint8_t>(pixel >> 24U);
        [[fallthrough]];
    case kGsPsmct24:
    case kGsPsmz24:
        raw_backing_[byte + 2] = static_cast<std::uint8_t>(pixel >> 16U);
        [[fallthrough]];
    case kGsPsmct16:
    case kGsPsmct16s:
    case kGsPsmz16:
    case kGsPsmz16s:
        raw_backing_[byte] = static_cast<std::uint8_t>(pixel);
        raw_backing_[byte + 1] = static_cast<std::uint8_t>(pixel >> 8U);
        record_mutated_byte(byte);
        return GsLocalMemoryStatus::Applied;
    case kGsPsmt8:
    case kGsPsmt8h:
        raw_backing_[byte] = static_cast<std::uint8_t>(pixel);
        record_mutated_byte(byte);
        return GsLocalMemoryStatus::Applied;
    case kGsPsmt4: {
        const unsigned char mask = static_cast<unsigned char>(15 << address->nibble_shift);
        raw_backing_[byte] = static_cast<unsigned char>((raw_backing_[byte] & ~mask) |
                                                        static_cast<unsigned char>((pixel & 15U) << address->nibble_shift));
        record_mutated_byte(byte);
        return GsLocalMemoryStatus::Applied;
    }
    default:
        return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
    }
}

bool GsLocalMemory::load_raw_backing(const std::span<const std::uint8_t> backing) noexcept {
    if (backing.size() != raw_backing_.size()) {
        return false;
    }
    auto batch = begin_mutation_batch();
    std::copy(backing.begin(), backing.end(), raw_backing_.begin());
    record_all_blocks_mutated();
    return true;
}

// RECOVERY: inline surface_status return label and branch probabilities differ from original; body otherwise matches.
GsLocalMemoryStatus GsLocalMemory::write_rgba8(const GsLocalMemorySurface& surface,
                                               const std::uint32_t x, const std::uint32_t y,
                                               const GsRgba8 color) noexcept {
    const auto surface_status = ::takedown::guest::surface_status(surface);
    if (surface_status != GsLocalMemoryStatus::Applied) {
        return surface_status;
    }
    const auto address = pixel_address(surface, x, y);
    if (!address) {
        return GsLocalMemoryStatus::InvalidGeometry;
    }
    if (surface.pixel_storage_mode - kGsPsmt8 <= 1U || surface.pixel_storage_mode == kGsPsmt8h) {
        return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
    }
    auto batch = begin_mutation_batch();
    if (surface.pixel_storage_mode <= kGsPsmct24) {
        raw_backing_[address->byte] = color.red;
        raw_backing_[address->byte + 1U] = color.green;
        raw_backing_[address->byte + 2U] = color.blue;
        if (surface.pixel_storage_mode == kGsPsmct32) {
            raw_backing_[address->byte + 3U] = color.alpha;
        }
        record_mutated_byte(address->byte);
    } else {
        const unsigned short pixel = static_cast<unsigned short>(
            static_cast<std::uint16_t>(color.red >> 3U) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(color.green >> 3U) << 5U) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(color.blue >> 3U) << 10U) |
            (static_cast<std::uint16_t>(static_cast<std::int8_t>(color.alpha)) & 0x8000U));
        raw_backing_[address->byte] = static_cast<std::uint8_t>(pixel);
        raw_backing_[address->byte + 1U] = static_cast<std::uint8_t>(pixel >> 8U);
        record_mutated_byte(address->byte);
    }
    return GsLocalMemoryStatus::Applied;
}

// RECOVERY: inline surface_status return label differs from original; body otherwise matches.
std::optional<GsRgba8> GsLocalMemory::read_rgba8(const GsLocalMemorySurface& surface,
                                                                const std::uint32_t x,
                                                                const std::uint32_t y) const noexcept {
    if (surface_status(surface) != GsLocalMemoryStatus::Applied) {
        return {};
    }
    const auto address = pixel_address(surface, x, y);
    if (!address) {
        return {};
    }
    if (surface.pixel_storage_mode - kGsPsmt8 <= 1U || surface.pixel_storage_mode == kGsPsmt8h) {
        return {};
    }
    if (surface.pixel_storage_mode <= kGsPsmct24) {
        return GsRgba8{
            raw_backing_[address->byte],
            raw_backing_[address->byte + 1U],
            raw_backing_[address->byte + 2U],
            surface.pixel_storage_mode == kGsPsmct32 ? raw_backing_[address->byte + 3U]
                                                     : static_cast<std::uint8_t>(255U),
        };
    }
    const auto expand5 = [](const std::uint16_t value) noexcept {
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>(value >> 2) |
                                         static_cast<std::uint8_t>(static_cast<std::uint8_t>(value) << 3));
    };
    const int pixel = static_cast<int>(raw_backing_[address->byte]) |
                      static_cast<int>(static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw_backing_[address->byte + 1U]) << 8));
    return GsRgba8{
        expand5(static_cast<std::uint16_t>(static_cast<std::uint16_t>(pixel) & 31)),
        expand5(static_cast<std::uint16_t>(static_cast<std::uint16_t>(pixel >> 5) & 31)),
        expand5(static_cast<std::uint16_t>(static_cast<std::uint16_t>(pixel >> 10) & 31)),
        (pixel & 0x8000) != 0 ? static_cast<std::uint8_t>(255U) : static_cast<std::uint8_t>(0U),
    };
}

// RECOVERY: texture status is a local lambda (depth PSMs accepted, surface_status does not); hoisted TEXA.AEM test, branch probabilities and status label differ from original.
std::optional<GsRgba8> GsLocalMemory::read_texture_rgba8(const GsLocalMemorySurface& surface,
                                                         const std::uint32_t x,
                                                         const std::uint32_t y,
                                                         const std::uint64_t texa) const noexcept {
    const auto texture_status = [](const GsLocalMemorySurface& texture_surface) noexcept {
        switch (texture_surface.pixel_storage_mode) {
        case kGsPsmct32:
        case kGsPsmct24:
        case kGsPsmct16:
        case kGsPsmct16s:
        case kGsPsmt8:
        case kGsPsmt4:
        case kGsPsmt8h:
            break;
        default:
            if (texture_surface.pixel_storage_mode - kGsPsmz32 <= 2U ||
                texture_surface.pixel_storage_mode == kGsPsmz16s) {
                break;
            }
            return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
        }
        if (texture_surface.buffer_width == 0U) {
            return GsLocalMemoryStatus::ZeroBufferWidth;
        }
        if (texture_surface.buffer_width >= 64U ||
            texture_surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
            return GsLocalMemoryStatus::InvalidBasePointer;
        }
        return GsLocalMemoryStatus::Applied;
    };
    if (texture_status(surface) != GsLocalMemoryStatus::Applied) {
        return {};
    }
    if (surface.pixel_storage_mode - kGsPsmt8 <= 1U || surface.pixel_storage_mode == kGsPsmt8h) {
        return {};
    }
    const auto raw = read_transfer_pixel(surface, x, y);
    if (!raw) {
        return {};
    }
    const unsigned char ta0 = static_cast<unsigned char>(texa);
    const unsigned char ta1 = static_cast<unsigned char>(texa >> 32U);
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmz32:
        return GsRgba8{
            static_cast<unsigned char>(*raw),
            static_cast<unsigned char>(*raw >> 8U),
            static_cast<unsigned char>(*raw >> 16U),
            static_cast<unsigned char>(*raw >> 24U),
        };
    case kGsPsmct24:
    case kGsPsmz24:
        return GsRgba8{
            static_cast<unsigned char>(*raw),
            static_cast<unsigned char>(*raw >> 8U),
            static_cast<unsigned char>(*raw >> 16U),
            ((texa & 0x8000U) == 0U || (*raw & 16777215U) != 0U) ? ta0 : static_cast<unsigned char>(0U),
        };
    case kGsPsmct16:
    case kGsPsmct16s:
    case kGsPsmz16:
    case kGsPsmz16s: {
        const unsigned short pixel = static_cast<unsigned short>(*raw);
        const unsigned char alpha =
            static_cast<signed short>(*raw) >= 0
                ? (((pixel != 0U) || (texa & 0x8000U) == 0U) ? ta0 : static_cast<unsigned char>(0U))
                : ta1;
        return GsRgba8{
            static_cast<unsigned char>(static_cast<unsigned char>(*raw) << 3U),
            static_cast<unsigned char>(static_cast<unsigned char>(pixel >> 5U) << 3U),
            static_cast<unsigned char>(static_cast<unsigned char>(pixel >> 10U) << 3U),
            alpha,
        };
    }
    default:
        return {};
    }
}

GsLocalMemoryStatus GsLocalMemory::write_index(const GsLocalMemorySurface& surface,
                                               const std::uint32_t x, const std::uint32_t y,
                                               const std::uint8_t index) noexcept {
    const auto surface_status = ::takedown::guest::surface_status(surface);
    if (surface_status != GsLocalMemoryStatus::Applied) {
        return surface_status;
    }
    if (surface.pixel_storage_mode - kGsPsmt8 > 1U && surface.pixel_storage_mode != kGsPsmt8h) {
        return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
    }
    const auto address = pixel_address(surface, x, y);
    if (!address) {
        return GsLocalMemoryStatus::InvalidGeometry;
    }
    auto batch = begin_mutation_batch();
    if (surface.pixel_storage_mode == kGsPsmt8 || surface.pixel_storage_mode == kGsPsmt8h) {
        raw_backing_[address->byte] = index;
        record_mutated_byte(address->byte);
    } else {
        const std::uint8_t nibble_mask =
            static_cast<std::uint8_t>(15 << static_cast<int>(address->nibble_shift));
        raw_backing_[address->byte] = static_cast<std::uint8_t>(
            (raw_backing_[address->byte] & ~nibble_mask) |
            ((static_cast<unsigned int>(index & 15U)) << static_cast<int>(address->nibble_shift)));
        record_mutated_byte(address->byte);
    }
    return GsLocalMemoryStatus::Applied;
}

std::optional<std::uint8_t> GsLocalMemory::read_index(const GsLocalMemorySurface& surface,
                                                      const std::uint32_t x,
                                                      const std::uint32_t y) const noexcept {
    if (surface_status(surface) != GsLocalMemoryStatus::Applied) {
        return {};
    }
    if (surface.pixel_storage_mode - kGsPsmt8 <= 1U || surface.pixel_storage_mode == kGsPsmt8h) {
        const auto address = pixel_address(surface, x, y);
        if (address) {
            if (surface.pixel_storage_mode == kGsPsmt8 || surface.pixel_storage_mode == kGsPsmt8h) {
                return raw_backing_[address->byte];
            }
            return static_cast<std::uint8_t>(
                (static_cast<int>(raw_backing_[address->byte]) >>
                 static_cast<int>(address->nibble_shift)) &
                15);
        }
    }
    return {};
}

std::optional<std::uint32_t> GsLocalMemory::read_z(const GsLocalMemorySurface& surface,
                                                   const std::uint32_t x,
                                                   const std::uint32_t y) const noexcept {
    if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
        const auto byte = depth_pixel_byte(surface, x, y);
        if (byte < kGsLocalMemoryBytes) {
            return read_z_at(surface.pixel_storage_mode, byte);
        }
    }
    return {};
}

std::uint32_t GsLocalMemory::read_z_at(const std::uint32_t psm, const std::size_t byte) const noexcept {
    if (psm == kGsPsmz16 || psm == kGsPsmz16s) {
        std::uint16_t value{};
        std::memcpy(&value, raw_backing_.data() + byte, sizeof(value));
        return value;
    }
    std::uint32_t value{};
    std::memcpy(&value, raw_backing_.data() + byte, sizeof(value));
    if (psm == kGsPsmz24) {
        value &= 16777215U;
    }
    return value;
}

std::size_t GsLocalMemory::depth_pixel_byte(const GsLocalMemorySurface& surface, const std::uint32_t x,
                                            const std::uint32_t y) noexcept {
    if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
        if (surface.buffer_width == 0U) {
            return kGsLocalMemoryBytes;
        }
        if (surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
            return kGsLocalMemoryBytes;
        }
        if (surface.pixel_storage_mode - kGsPsmz32 <= 1U) {
            const auto page = static_cast<std::uint64_t>(y >> 5U) * surface.buffer_width + (x >> 6U);
            const auto in_page = kPsmz32InPageAddresses[(y & 31U) * 64U + (x & 63U)];
            const auto unswizzled_block = (static_cast<std::uint64_t>(surface.base_pointer) + page * 32U) + in_page.block;
            const auto block = (unswizzled_block ^ 24U) & (kGsLocalMemoryBlocks - 1U);
            return static_cast<std::size_t>(block * 256U + in_page.byte);
        }
        const auto address = pixel_address(surface, x, y);
        if (address) {
            return address->byte;
        }
    }
    return kGsLocalMemoryBytes;
}

GsLocalMemoryStatus GsLocalMemory::write_z(const GsLocalMemorySurface& surface,
                                           const std::uint32_t x, const std::uint32_t y,
                                           const std::uint32_t z) noexcept {
    if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
        const auto byte = depth_pixel_byte(surface, x, y);
        if (byte < kGsLocalMemoryBytes) {
            auto batch = begin_mutation_batch();
            write_z_at(surface.pixel_storage_mode, byte, z);
            return GsLocalMemoryStatus::Applied;
        }
        return GsLocalMemoryStatus::InvalidBasePointer;
    }
    return GsLocalMemoryStatus::UnsupportedPixelStorageMode;
}

void GsLocalMemory::write_z_at(const std::uint32_t psm, const std::size_t byte, const std::uint32_t z) noexcept {
    if (psm == kGsPsmz16 || psm == kGsPsmz16s) {
        const auto value = static_cast<std::uint16_t>(z);
        std::memcpy(raw_backing_.data() + byte, &value, sizeof(value));
        record_mutated_byte(byte);
        return;
    }
    if (psm == kGsPsmz32) {
        std::memcpy(raw_backing_.data() + byte, &z, sizeof(z));
        record_mutated_byte(byte);
        return;
    }
    const auto low = static_cast<std::uint16_t>(z);
    std::memcpy(raw_backing_.data() + byte, &low, sizeof(low));
    raw_backing_[byte + 2U] = static_cast<std::uint8_t>(z >> 16U);
    record_mutated_byte(byte);
}

std::span<const std::uint8_t> GsLocalMemory::raw_backing() const noexcept {
    return raw_backing_;
}

void GsLocalMemory::clear() noexcept {
    auto batch = begin_mutation_batch();
    std::fill(raw_backing_.begin(), raw_backing_.end(), std::uint8_t{});
    record_all_blocks_mutated();
}

std::optional<GsLocalMemory::PixelAddress> GsLocalMemory::pixel_address(const GsLocalMemorySurface& surface,
                                                                          const std::uint64_t x,
                                                                          const std::uint64_t y) noexcept {
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmct24:
    case kGsPsmct16:
    case kGsPsmct16s:
    case kGsPsmt8:
    case kGsPsmt4:
    case kGsPsmt8h:
        break;
    default:
        if (surface.pixel_storage_mode - kGsPsmz32 <= 2U || surface.pixel_storage_mode == kGsPsmz16s) {
            break;
        }
        return std::nullopt;
    }
    if (surface.buffer_width == 0U || surface.base_pointer > kGsLocalMemoryBlocks - 1U) {
        return std::nullopt;
    }

    std::uint64_t page{};
    std::uint32_t block_in_page{};
    std::uint32_t byte_in_block{};
    std::uint8_t nibble_shift{};
    std::uint64_t block_address_xor{};

    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmct24: {
        page = (y >> 5U) * surface.buffer_width + (x >> 6U);
        const auto block_x = static_cast<unsigned int>(x >> 3U) & 7U;
        const auto block_y = static_cast<unsigned int>(y >> 3U) & 3U;
        block_in_page = (block_x & 1U) | ((block_y << 1U) & 2U) | ((block_x << 1U) & 4U) |
                        ((block_y << 2U) & 8U) | ((block_x << 2U) & 16U);
        const auto local_x = static_cast<unsigned int>(x);
        const auto local_y = static_cast<unsigned int>(y) & 7U;
        byte_in_block = ((local_x & 1U) | ((local_y << 1U) & 2U) |
                                    ((local_x << 1U) & 12U) | ((local_y << 3U) & 48U)) * 4U;
        break;
    }
    case kGsPsmt8h: {
        page = (y >> 5U) * surface.buffer_width + (x >> 6U);
        const auto block_x = static_cast<unsigned int>(x >> 3U) & 7U;
        const auto block_y = static_cast<unsigned int>(y >> 3U) & 3U;
        block_in_page = (block_x & 1U) | ((block_y << 1U) & 2U) | ((block_x << 1U) & 4U) |
                        ((block_y << 2U) & 8U) | ((block_x << 2U) & 16U);
        const auto local_x = static_cast<unsigned int>(x);
        const auto local_y = static_cast<unsigned int>(y) & 7U;
        byte_in_block = ((local_x & 1U) | ((local_y << 1U) & 2U) |
                                    ((local_x << 1U) & 12U) | ((local_y << 3U) & 48U)) * 4U + 3U;
        break;
    }
    case kGsPsmz32:
    case kGsPsmz24: {
        page = (y >> 5U) * surface.buffer_width + (x >> 6U);
        const auto block_x = static_cast<unsigned int>(x >> 3U) & 7U;
        const auto block_y = static_cast<unsigned int>(y >> 3U) & 3U;
        block_in_page = (block_x & 1U) | ((block_y << 1U) & 2U) | ((block_x << 1U) & 4U) |
                        ((block_y << 2U) & 8U) | ((block_x << 2U) & 16U);
        const auto local_x = static_cast<unsigned int>(x);
        const auto local_y = static_cast<unsigned int>(y) & 7U;
        byte_in_block = ((local_x & 1U) | ((local_y << 1U) & 2U) |
                                    ((local_x << 1U) & 12U) | ((local_y << 3U) & 48U)) * 4U;
        block_address_xor = 24U;
        break;
    }
    case kGsPsmct16:
    case kGsPsmct16s: {
        page = (y >> 6U) * surface.buffer_width + (x >> 6U);
        const auto block_x = static_cast<unsigned int>(x >> 4U) & 3U;
        const auto block_y = static_cast<unsigned int>(y >> 3U) & 7U;
        if (surface.pixel_storage_mode == kGsPsmct16s) {
            block_in_page = (block_y & 1U) | ((block_x << 1U) & 2U) | ((block_x << 3U) & 16U) |
                            ((block_y << 2U) & 8U) | (block_y & 4U);
        } else {
            block_in_page = (block_y & 1U) | ((block_x << 1U) & 2U) | ((block_x << 2U) & 8U) |
                            ((block_y << 1U) & 4U) | ((block_y << 2U) & 16U);
        }
        const auto local_x = static_cast<unsigned int>(x) & 15U;
        const auto local_y = static_cast<unsigned int>(y) & 7U;
        byte_in_block = (((local_x << 1U) & 2U) | ((local_x << 2U) & 8U) |
                                    ((local_x << 2U) & 16U) | (local_x >> 3U) |
                                    ((local_y << 2U) & 4U) | ((local_y << 4U) & 32U) |
                                    ((local_y << 4U) & 64U)) * 2U;
        break;
    }
    case kGsPsmz16:
    case kGsPsmz16s: {
        page = (y >> 6U) * surface.buffer_width + (x >> 6U);
        const auto block_x = static_cast<unsigned int>(x >> 4U) & 3U;
        const auto block_y = static_cast<unsigned int>(y >> 3U) & 7U;
        if (surface.pixel_storage_mode == kGsPsmz16s) {
            block_in_page = (block_x << 1U) & 2U | (block_y & 1U) | ((block_x << 3U) & 16U) |
                            ((block_y << 2U) & 8U) | (block_y & 4U);
        } else {
            block_in_page = (block_y & 1U) | ((block_x << 1U) & 2U) | ((block_x << 2U) & 8U) |
                            ((block_y << 1U) & 4U) | ((block_y << 2U) & 16U);
        }
        const auto local_x = static_cast<unsigned int>(x) & 15U;
        const auto local_y = static_cast<unsigned int>(y) & 7U;
        byte_in_block = (((local_x << 1U) & 2U) | ((local_x << 2U) & 8U) |
                                    ((local_x << 2U) & 16U) | (local_x >> 3U) |
                                    ((local_y << 2U) & 4U) | ((local_y << 4U) & 32U) |
                                    ((local_y << 4U) & 64U)) * 2U;
        block_address_xor = 24U;
        break;
    }
    case kGsPsmt8: {
        page = (y >> 6U) * (surface.buffer_width >> 1U) + (x >> 7U);
        const auto block_x = static_cast<unsigned int>(x >> 4U) & 7U;
        const auto block_y = static_cast<unsigned int>(y >> 4U) & 3U;
        block_in_page = (block_x & 1U) | ((block_y << 1U) & 2U) | ((block_x << 1U) & 4U) |
                        ((block_y << 2U) & 8U) | ((block_x << 2U) & 16U);
        const auto local_x = static_cast<unsigned int>(x) & 15U;
        const auto local_y = static_cast<unsigned int>(y) & 15U;
        byte_in_block = ((local_x << 2U) & 4U) | ((local_x << 3U) & 16U) | ((local_x >> 2U) & 2U) |
                        ((local_y << 3U) & 8U) | ((local_y >> 1U) & 1U) | ((local_y << 4U) & 64U) |
                        ((local_y << 4U) & 128U) |
                        ((((local_x >> 2U) ^ (local_y >> 1U)) ^ (local_y >> 2U)) << 5U & 32U);
        break;
    }
    case kGsPsmt4: {
        page = (y >> 7U) * (surface.buffer_width >> 1U) + (x >> 7U);
        const auto block_x = static_cast<unsigned int>(x >> 5U) & 3U;
        const auto block_y = static_cast<unsigned int>(y >> 4U) & 7U;
        block_in_page = (block_y & 1U) | ((block_x << 1U) & 2U) | ((block_x << 2U) & 8U) |
                        ((block_y << 1U) & 4U) | ((block_y << 2U) & 16U);
        const auto local_x = static_cast<unsigned int>(x) & 31U;
        const auto local_y = static_cast<unsigned int>(y) & 15U;
        const auto nibble_in_block = ((local_x << 3U) & 8U) | ((local_x << 4U) & 32U) |
                                     ((local_x >> 2U) & 2U) | ((local_x >> 2U) & 4U) |
                                     ((local_y << 4U) & 16U) | ((local_y >> 1U) & 1U) |
                                     ((local_y << 5U) & 128U) | ((local_y << 5U) & 256U) |
                                     ((((local_x >> 2U) ^ (local_y >> 1U)) ^ (local_y >> 2U)) << 6U & 64U);
        byte_in_block = nibble_in_block >> 1U;
        nibble_shift = static_cast<std::uint8_t>((static_cast<std::uint8_t>(nibble_in_block) << 2U) & 4U);
        break;
    }
    default:
        return std::nullopt;
    }

    const auto unswizzled_block =
        (static_cast<std::uint64_t>(surface.base_pointer) + page * 32U) + block_in_page;
    const auto block = (block_address_xor ^ unswizzled_block) & (kGsLocalMemoryBlocks - 1U);
    return PixelAddress{block * 256U + byte_in_block, nibble_shift};
}


} // namespace takedown::guest
