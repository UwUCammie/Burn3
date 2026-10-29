#include "takedown/guest/gs_diagnostic_vram.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace takedown::guest {

const char* to_string(const GsDiagnosticVramStatus status) noexcept {
    switch (status) {
    case GsDiagnosticVramStatus::Applied:
        return "applied";
    case GsDiagnosticVramStatus::UnsupportedPixelStorageMode:
        return "unsupported pixel storage mode";
    case GsDiagnosticVramStatus::ZeroDestinationBufferWidth:
        return "zero destination buffer width";
    case GsDiagnosticVramStatus::InvalidGeometry:
        return "invalid transfer geometry";
    case GsDiagnosticVramStatus::InvalidPayload:
        return "invalid transfer payload";
    case GsDiagnosticVramStatus::AddressOutOfBounds:
        return "diagnostic linear address out of bounds";
    }
    return "unknown diagnostic VRAM status";
}

GsDiagnosticVram::GsDiagnosticVram()
    : raw_backing_(kGsDiagnosticVramRawBytes, std::uint8_t{}), layouts_{}, diagnostics_{} {
    latest_layout_index_ = std::optional<std::size_t>{};
}

GsDiagnosticVramApplyResult GsDiagnosticVram::apply_host_to_local(const GsImageTransfer& transfer) {
    const GsDiagnosticVramLayoutKey key{
        .destination_base_pointer = transfer.destination_base_pointer,
        .destination_buffer_width = transfer.destination_buffer_width,
        .destination_psm = transfer.destination_psm,
    };
    // The diagnostic history is bounded; once full, rejections are still reported in the result.
    auto reject = [this, &transfer, &key](GsDiagnosticVramStatus status, std::uint64_t detail) {
        if (diagnostics_.size() <= 5080U) {
            GsDiagnosticVramDiagnostic diagnostic;
            diagnostic.status = status;
            diagnostic.sequence = transfer.sequence;
            diagnostic.key = key;
            diagnostic.detail = detail;
            diagnostics_.push_back(diagnostic);
        }
        return GsDiagnosticVramApplyResult{.status = status, .bytes_written = 0, .detail = detail};
    };

    std::optional<std::size_t> pixel_bytes;
    switch (transfer.destination_psm) {
        case 0:
            pixel_bytes.emplace(4);
            break;
        case 2:
        case 10:
            pixel_bytes.emplace(2);
            break;
        default:
            pixel_bytes = {};
            break;
    }
    if (!pixel_bytes) {
        return reject(GsDiagnosticVramStatus::UnsupportedPixelStorageMode, 0);
    }
    if (transfer.destination_buffer_width == 0) {
        return reject(GsDiagnosticVramStatus::ZeroDestinationBufferWidth, 0);
    }

    std::optional<std::size_t> expected_bytes;
    if (transfer.width != 0 && transfer.height != 0) {
        const std::size_t pixel_count = static_cast<std::size_t>(transfer.width) * transfer.height;
        const std::size_t maximum_pixel_count = kGsDiagnosticVramRawBytes / *pixel_bytes;
        if (pixel_count <= maximum_pixel_count) {
            expected_bytes.emplace(*pixel_bytes * pixel_count);
        }
    }
    if (!expected_bytes) {
        return reject(GsDiagnosticVramStatus::InvalidGeometry, 0);
    }
    if (*expected_bytes != transfer.pixels.size()) {
        return reject(GsDiagnosticVramStatus::InvalidPayload, 0);
    }

    const std::size_t bytes = *pixel_bytes;
    std::optional<std::uint64_t> invalid_address;
    for (std::uint32_t row = 0; row < transfer.height && !invalid_address; ++row) {
        const std::uint32_t y = (transfer.destination_y + row) & (kGsDiagnosticCoordinateLimit - 1U);
        for (std::uint32_t column = 0; column < transfer.width && !invalid_address; ++column) {
            const std::uint32_t x = (transfer.destination_x + column) & (kGsDiagnosticCoordinateLimit - 1U);
            const std::uint64_t base = static_cast<std::uint64_t>(key.destination_base_pointer) * 256U;
            const std::uint64_t pixel_offset =
                (static_cast<std::uint64_t>(key.destination_buffer_width) * y) * 64U + x;
            const std::uint64_t address = base + bytes * pixel_offset;
            if (bytes > kGsDiagnosticVramRawBytes || address > kGsDiagnosticVramRawBytes - bytes) {
                invalid_address = address;
            }
        }
    }
    if (invalid_address) {
        return reject(GsDiagnosticVramStatus::AddressOutOfBounds, *invalid_address);
    }

    std::size_t source_offset = 0;
    for (std::uint32_t row = 0; row < transfer.height; ++row) {
        const std::uint32_t y = (transfer.destination_y + row) & (kGsDiagnosticCoordinateLimit - 1U);
        for (std::uint32_t column = 0; column < transfer.width; ++column) {
            const std::uint32_t x = (transfer.destination_x + column) & (kGsDiagnosticCoordinateLimit - 1U);
            const std::uint64_t base = static_cast<std::uint64_t>(key.destination_base_pointer) * 256U;
            const std::uint64_t pixel_offset =
                (static_cast<std::uint64_t>(key.destination_buffer_width) * y) * 64U + x;
            const std::uint64_t address = base + bytes * pixel_offset;
            std::copy(transfer.pixels.data() + source_offset, transfer.pixels.data() + source_offset + bytes,
                      raw_backing_.data() + address);
            source_offset += bytes;
        }
    }

    // The dirty rectangle wraps at the coordinate seam; a transfer that crosses it covers the whole axis.
    const std::uint32_t wrapped_x = transfer.destination_x & (kGsDiagnosticCoordinateLimit - 1U);
    std::uint32_t dirty_x_begin = 0U;
    std::uint32_t dirty_x_end = kGsDiagnosticCoordinateLimit - 1U;
    if (transfer.width <= kGsDiagnosticCoordinateLimit - 1U) {
        const std::uint64_t end = static_cast<std::uint64_t>(wrapped_x) + transfer.width - 1U;
        if (end <= kGsDiagnosticCoordinateLimit - 1U) {
            dirty_x_begin = wrapped_x;
            dirty_x_end = static_cast<std::uint32_t>(end);
        }
    }
    const std::uint32_t wrapped_y = transfer.destination_y & (kGsDiagnosticCoordinateLimit - 1U);
    std::uint32_t dirty_y_begin = 0U;
    std::uint32_t dirty_y_end = kGsDiagnosticCoordinateLimit - 1U;
    if (transfer.height <= kGsDiagnosticCoordinateLimit - 1U) {
        const std::uint64_t end = static_cast<std::uint64_t>(wrapped_y) + transfer.height - 1U;
        if (end <= kGsDiagnosticCoordinateLimit - 1U) {
            dirty_y_begin = wrapped_y;
            dirty_y_end = static_cast<std::uint32_t>(end);
        }
    }

    const auto found = std::ranges::find_if(layouts_, [&key](const GsDiagnosticVramLayout& candidate) {
        return candidate.key.destination_base_pointer == key.destination_base_pointer &&
               candidate.key.destination_buffer_width == key.destination_buffer_width &&
               candidate.key.destination_psm == key.destination_psm;
    });
    if (found == layouts_.end()) {
        GsDiagnosticVramLayout layout;
        layout.key = key;
        layout.dirty_x = dirty_x_begin;
        layout.dirty_y = dirty_y_begin;
        layout.dirty_width = dirty_x_end - dirty_x_begin + 1U;
        layout.dirty_height = dirty_y_end - dirty_y_begin + 1U;
        layout.last_sequence = transfer.sequence;
        layout.apply_count = 1;
        layouts_.push_back(layout);
        latest_layout_index_ = layouts_.size() - 1U;
    } else {
        GsDiagnosticVramLayout& layout = *found;
        const std::uint32_t old_x_end = layout.dirty_x + layout.dirty_width - 1U;
        const std::uint32_t old_y_end = layout.dirty_y + layout.dirty_height - 1U;
        layout.dirty_x = std::min(layout.dirty_x, dirty_x_begin);
        layout.dirty_y = std::min(layout.dirty_y, dirty_y_begin);
        layout.dirty_width = std::max(old_x_end, dirty_x_end) - layout.dirty_x + 1U;
        layout.dirty_height = std::max(old_y_end, dirty_y_end) - layout.dirty_y + 1U;
        layout.last_sequence = transfer.sequence;
        layout.apply_count += 1;
        latest_layout_index_ = static_cast<std::size_t>(found - layouts_.begin());
    }

    return GsDiagnosticVramApplyResult{
        .status = GsDiagnosticVramStatus::Applied,
        .bytes_written = *expected_bytes,
        .detail = 0,
    };
}

std::span<const std::uint8_t> GsDiagnosticVram::raw_backing() const noexcept {
    return raw_backing_;
}

std::span<const GsDiagnosticVramLayout> GsDiagnosticVram::layouts() const noexcept {
    return layouts_;
}

std::span<const GsDiagnosticVramDiagnostic> GsDiagnosticVram::diagnostics() const noexcept {
    return diagnostics_;
}

const GsDiagnosticVramLayout* GsDiagnosticVram::latest_layout() const noexcept {
    if (!latest_layout_index_) {
        return nullptr;
    }
    if (*latest_layout_index_ >= layouts_.size()) {
        return nullptr;
    }
    return &layouts_[*latest_layout_index_];
}

std::optional<GsDiagnosticVramSnapshot> GsDiagnosticVram::snapshot_for(const GsDiagnosticVramLayoutKey& key) const {
    const auto found = std::ranges::find_if(layouts_, [&key](const GsDiagnosticVramLayout& candidate) {
        return candidate.key.destination_base_pointer == key.destination_base_pointer &&
               candidate.key.destination_buffer_width == key.destination_buffer_width &&
               candidate.key.destination_psm == key.destination_psm;
    });
    if (found == layouts_.end()) {
        return {};
    }
    const GsDiagnosticVramLayout& layout = *found;
    const std::span<const std::uint8_t> raw{raw_backing_};
    if (layout.dirty_width == 0 || layout.dirty_height == 0 ||
        layout.dirty_x >= kGsDiagnosticCoordinateLimit || layout.dirty_y >= kGsDiagnosticCoordinateLimit ||
        static_cast<std::uint64_t>(layout.dirty_x) + layout.dirty_width > kGsDiagnosticCoordinateLimit ||
        static_cast<std::uint64_t>(layout.dirty_y) + layout.dirty_height > kGsDiagnosticCoordinateLimit) {
        return {};
    }

    std::optional<std::size_t> pixel_bytes;
    switch (layout.key.destination_psm) {
        case 0:
            pixel_bytes.emplace(4);
            break;
        case 2:
        case 10:
            pixel_bytes.emplace(2);
            break;
        default:
            pixel_bytes = {};
            break;
    }
    if (!pixel_bytes) {
        return {};
    }

    const std::size_t pixel_count = static_cast<std::size_t>(layout.dirty_width) * layout.dirty_height;
    GsDiagnosticVramSnapshot snapshot{
        .layout = layout,
        .x = layout.dirty_x,
        .y = layout.dirty_y,
        .width = layout.dirty_width,
        .height = layout.dirty_height,
        .rgba = std::vector<std::uint8_t>(pixel_count * 4U),
    };

    for (std::uint32_t row = 0; row < snapshot.height; ++row) {
        const std::uint32_t y = snapshot.y + row;
        for (std::uint32_t column = 0; column < snapshot.width; ++column) {
            const std::uint32_t x = snapshot.x + column;
            const std::size_t pixel_size = *pixel_bytes;
            const std::uint64_t base = static_cast<std::uint64_t>(snapshot.layout.key.destination_base_pointer) * 256U;
            const std::uint64_t pixel_offset =
                (static_cast<std::uint64_t>(snapshot.layout.key.destination_buffer_width) * y) * 64U + x;
            const std::uint64_t address = base + pixel_size * pixel_offset;
            if (pixel_size > kGsDiagnosticVramRawBytes || address > kGsDiagnosticVramRawBytes - pixel_size) {
                continue;
            }
            const std::uint64_t destination =
                (static_cast<std::uint64_t>(row) * snapshot.width + column) * 4U;
            if (snapshot.layout.key.destination_psm == 0) {
                snapshot.rgba[destination] = raw[address];
                snapshot.rgba[destination + 1] = raw[address + 1];
                snapshot.rgba[destination + 2] = raw[address + 2];
                snapshot.rgba[destination + 3] = raw[address + 3];
            } else {
                const int pixel = raw[address] | (raw[address + 1] << 8);
                const auto r5 = static_cast<std::uint16_t>(pixel & 31);
                snapshot.rgba[destination] = static_cast<std::uint8_t>((r5 >> 2) | (r5 << 3));
                const auto g5 = static_cast<std::uint16_t>((pixel >> 5) & 31);
                snapshot.rgba[destination + 1] = static_cast<std::uint8_t>((g5 >> 2) | (g5 << 3));
                const auto b5 = static_cast<std::uint16_t>((pixel >> 10) & 31);
                snapshot.rgba[destination + 2] = static_cast<std::uint8_t>((b5 >> 2) | (b5 << 3));
                snapshot.rgba[destination + 3] = static_cast<std::uint8_t>((pixel & 32768) != 0 ? 255 : 0);
            }
        }
    }
    return snapshot;
}


std::optional<GsDiagnosticVramSnapshot> GsDiagnosticVram::latest_snapshot() const {
    const GsDiagnosticVramLayout* latest = latest_layout();
    if (latest == nullptr) {
        return {};
    }
    const GsDiagnosticVramLayout& layout = *latest;
    const std::span<const std::uint8_t> raw{raw_backing_};
    if (layout.dirty_width == 0 || layout.dirty_height == 0 ||
        layout.dirty_x >= kGsDiagnosticCoordinateLimit || layout.dirty_y >= kGsDiagnosticCoordinateLimit ||
        static_cast<std::uint64_t>(layout.dirty_x) + layout.dirty_width > kGsDiagnosticCoordinateLimit ||
        static_cast<std::uint64_t>(layout.dirty_y) + layout.dirty_height > kGsDiagnosticCoordinateLimit) {
        return {};
    }

    std::optional<std::size_t> pixel_bytes;
    switch (layout.key.destination_psm) {
        case 0:
            pixel_bytes.emplace(4);
            break;
        case 2:
        case 10:
            pixel_bytes.emplace(2);
            break;
        default:
            pixel_bytes = {};
            break;
    }
    if (!pixel_bytes) {
        return {};
    }

    const std::size_t pixel_count = static_cast<std::size_t>(layout.dirty_width) * layout.dirty_height;
    GsDiagnosticVramSnapshot snapshot{
        .layout = layout,
        .x = layout.dirty_x,
        .y = layout.dirty_y,
        .width = layout.dirty_width,
        .height = layout.dirty_height,
        .rgba = std::vector<std::uint8_t>(pixel_count * 4U),
    };

    for (std::uint32_t row = 0; row < snapshot.height; ++row) {
        const std::uint32_t y = snapshot.y + row;
        for (std::uint32_t column = 0; column < snapshot.width; ++column) {
            const std::uint32_t x = snapshot.x + column;
            const std::size_t pixel_size = *pixel_bytes;
            const std::uint64_t base = static_cast<std::uint64_t>(snapshot.layout.key.destination_base_pointer) * 256U;
            const std::uint64_t pixel_offset =
                (static_cast<std::uint64_t>(snapshot.layout.key.destination_buffer_width) * y) * 64U + x;
            const std::uint64_t address = base + pixel_size * pixel_offset;
            if (pixel_size > kGsDiagnosticVramRawBytes || address > kGsDiagnosticVramRawBytes - pixel_size) {
                continue;
            }
            const std::uint64_t destination =
                (static_cast<std::uint64_t>(row) * snapshot.width + column) * 4U;
            if (snapshot.layout.key.destination_psm == 0) {
                snapshot.rgba[destination] = raw[address];
                snapshot.rgba[destination + 1] = raw[address + 1];
                snapshot.rgba[destination + 2] = raw[address + 2];
                snapshot.rgba[destination + 3] = raw[address + 3];
            } else {
                const int pixel = raw[address] | (raw[address + 1] << 8);
                const auto r5 = static_cast<std::uint16_t>(pixel & 31);
                snapshot.rgba[destination] = static_cast<std::uint8_t>((r5 >> 2) | (r5 << 3));
                const auto g5 = static_cast<std::uint16_t>((pixel >> 5) & 31);
                snapshot.rgba[destination + 1] = static_cast<std::uint8_t>((g5 >> 2) | (g5 << 3));
                const auto b5 = static_cast<std::uint16_t>((pixel >> 10) & 31);
                snapshot.rgba[destination + 2] = static_cast<std::uint8_t>((b5 >> 2) | (b5 << 3));
                snapshot.rgba[destination + 3] = static_cast<std::uint8_t>((pixel & 32768) != 0 ? 255 : 0);
            }
        }
    }
    return snapshot;
}


void GsDiagnosticVram::clear() noexcept {
    std::fill(raw_backing_.begin(), raw_backing_.end(), std::uint8_t{});
    layouts_.clear();
    diagnostics_.clear();
    latest_layout_index_.reset();
}

} // namespace takedown::guest
