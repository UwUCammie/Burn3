#include "takedown/guest/gs_privileged_display.hpp"

namespace takedown::guest {

namespace {

// Extracts a -bit register field starting at bit .
[[nodiscard]] constexpr std::uint64_t field(const std::uint64_t raw, const unsigned shift,
                                            const unsigned width) noexcept {
    return (raw >> shift) & ((std::uint64_t{1} << width) - 1U);
}

} // namespace

GsPmodeFields decode_gs_pmode(const std::uint64_t raw) noexcept {
    return GsPmodeFields{
        .circuit1_enabled = (raw & 0x1U) != 0U,
        .circuit2_enabled = (raw & 0x2U) != 0U,
        .crt_mode = static_cast<std::uint8_t>(field(raw, 2, 3)),
        .mmod = (raw & 0x20U) != 0U,
        .amod = (raw & 0x40U) != 0U,
        .slbg = (raw & 0x80U) != 0U,
        .alpha = static_cast<std::uint8_t>(field(raw, 8, 8)),
    };
}

GsSmode2Fields decode_gs_smode2(const std::uint64_t raw) noexcept {
    return GsSmode2Fields{
        .interlaced = (raw & 0x1U) != 0U,
        .field_frame_mode = (raw & 0x2U) != 0U,
        .display_power_management = static_cast<std::uint8_t>(field(raw, 2, 2)),
    };
}

GsDispfbFields decode_gs_dispfb(const std::uint64_t raw) noexcept {
    return GsDispfbFields{
        .framebuffer_base_pointer = static_cast<std::uint16_t>(field(raw, 0, 9)),
        .framebuffer_width = static_cast<std::uint8_t>(field(raw, 9, 6)),
        .pixel_storage_mode = static_cast<std::uint8_t>(field(raw, 15, 5)),
        .display_x = static_cast<std::uint16_t>(field(raw, 32, 11)),
        .display_y = static_cast<std::uint16_t>(field(raw, 43, 11)),
    };
}

GsDisplayFields decode_gs_display(const std::uint64_t raw) noexcept {
    return GsDisplayFields{
        .display_x = static_cast<std::uint16_t>(field(raw, 0, 12)),
        .display_y = static_cast<std::uint16_t>(field(raw, 12, 11)),
        .horizontal_magnification = static_cast<std::uint8_t>(field(raw, 23, 4)),
        .vertical_magnification = static_cast<std::uint8_t>(field(raw, 27, 2)),
        .display_width_raw = static_cast<std::uint16_t>(field(raw, 32, 12)),
        .display_height_raw = static_cast<std::uint16_t>(field(raw, 44, 11)),
    };
}

GsBgcolorFields decode_gs_bgcolor(const std::uint64_t raw) noexcept {
    return GsBgcolorFields{
        .red = static_cast<std::uint8_t>(field(raw, 0, 8)),
        .green = static_cast<std::uint8_t>(field(raw, 8, 8)),
        .blue = static_cast<std::uint8_t>(field(raw, 16, 8)),
    };
}

bool GsPrivilegedDisplayState::any_seen() const noexcept {
    return pmode.seen || smode2.seen || dispfb[0].seen || dispfb[1].seen || display[0].seen ||
           display[1].seen || bgcolor.seen;
}

void GsPrivilegedDisplayObserver::observe_byte_write(const std::uint32_t physical_address,
                                                     const std::uint8_t value) noexcept {
    if (observe_slot(physical_address, value, kGsPmodeAddress, state_.pmode) ||
        observe_slot(physical_address, value, kGsSmode2Address, state_.smode2) ||
        observe_slot(physical_address, value, kGsDispfb1Address, state_.dispfb[0]) ||
        observe_slot(physical_address, value, kGsDisplay1Address, state_.display[0]) ||
        observe_slot(physical_address, value, kGsDispfb2Address, state_.dispfb[1]) ||
        observe_slot(physical_address, value, kGsDisplay2Address, state_.display[1])) {
        return;
    }
    static_cast<void>(observe_slot(physical_address, value, kGsBgcolorAddress, state_.bgcolor));
}

void GsPrivilegedDisplayObserver::reset() noexcept {
    state_ = {};
}

const GsPrivilegedDisplayState& GsPrivilegedDisplayObserver::state() const noexcept {
    return state_;
}

bool GsPrivilegedDisplayObserver::observe_slot(const std::uint32_t physical_address,
                                               const std::uint8_t value,
                                               const std::uint32_t slot_address,
                                               GsObservedPrivilegedRegister& destination) noexcept {
    if (physical_address < slot_address || physical_address >= slot_address + 8U) {
        return false;
    }
    const auto shift = (physical_address - slot_address) * 8U;
    const auto mask = std::uint64_t{0xffU} << shift;
    destination.raw = (destination.raw & ~mask) | (static_cast<std::uint64_t>(value) << shift);
    destination.seen = true;
    destination.last_write_sequence = ++state_.revision;
    return true;
}

} // namespace takedown::guest
