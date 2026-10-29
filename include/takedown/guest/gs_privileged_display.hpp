#pragma once

#include <array>
#include <cstdint>

namespace takedown::guest {

// GS privileged CRTC register slots in EE physical MMIO space. This observer
// retains their raw bytes for evidence, but it neither emulates GS scanout nor
// changes the existing guest-visible MMIO read/write behavior.
inline constexpr std::uint32_t kGsPmodeAddress = 0x12000000U;
inline constexpr std::uint32_t kGsSmode2Address = 0x12000020U;
inline constexpr std::uint32_t kGsDispfb1Address = 0x12000070U;
inline constexpr std::uint32_t kGsDisplay1Address = 0x12000080U;
inline constexpr std::uint32_t kGsDispfb2Address = 0x12000090U;
inline constexpr std::uint32_t kGsDisplay2Address = 0x120000a0U;
inline constexpr std::uint32_t kGsBgcolorAddress = 0x120000e0U;

struct GsObservedPrivilegedRegister {
    std::uint64_t raw{};
    bool seen{};
    std::uint64_t last_write_sequence{};
};

// These decoded fields are evidence only. They are deliberately not a request
// to compose displays, derive a frame size, or present a scanout.
struct GsPmodeFields {
    bool circuit1_enabled{};
    bool circuit2_enabled{};
    std::uint8_t crt_mode{};
    // Retain the GS register's hardware names rather than assigning host
    // composition semantics before PMODE blending is actually implemented.
    bool mmod{};
    bool amod{};
    bool slbg{};
    std::uint8_t alpha{};
};

struct GsSmode2Fields {
    bool interlaced{};
    bool field_frame_mode{};
    std::uint8_t display_power_management{};
};

struct GsDispfbFields {
    std::uint16_t framebuffer_base_pointer{};
    std::uint8_t framebuffer_width{};
    std::uint8_t pixel_storage_mode{};
    std::uint16_t display_x{};
    std::uint16_t display_y{};
};

struct GsDisplayFields {
    std::uint16_t display_x{};
    std::uint16_t display_y{};
    std::uint8_t horizontal_magnification{};
    std::uint8_t vertical_magnification{};
    // Raw GS display-area values, not inferred pixel dimensions.
    std::uint16_t display_width_raw{};
    std::uint16_t display_height_raw{};
};

struct GsBgcolorFields {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
};

[[nodiscard]] GsPmodeFields decode_gs_pmode(std::uint64_t raw) noexcept;
[[nodiscard]] GsSmode2Fields decode_gs_smode2(std::uint64_t raw) noexcept;
[[nodiscard]] GsDispfbFields decode_gs_dispfb(std::uint64_t raw) noexcept;
[[nodiscard]] GsDisplayFields decode_gs_display(std::uint64_t raw) noexcept;
[[nodiscard]] GsBgcolorFields decode_gs_bgcolor(std::uint64_t raw) noexcept;

struct GsPrivilegedDisplayState {
    std::uint64_t revision{};
    GsObservedPrivilegedRegister pmode{};
    GsObservedPrivilegedRegister smode2{};
    std::array<GsObservedPrivilegedRegister, 2> dispfb{};
    std::array<GsObservedPrivilegedRegister, 2> display{};
    GsObservedPrivilegedRegister bgcolor{};

    [[nodiscard]] bool any_seen() const noexcept;
};

// Byte-granular raw observer for the display-relevant GS privileged MMIO
// slots. EeMemory normalizes cached/uncached aliases before reaching this API.
// It accepts the physical address and exactly the byte the guest stored, so
// partial and unaligned stores remain visible without changing MMIO semantics.
class GsPrivilegedDisplayObserver {
  public:
    void observe_byte_write(std::uint32_t physical_address, std::uint8_t value) noexcept;
    void reset() noexcept;

    [[nodiscard]] const GsPrivilegedDisplayState& state() const noexcept;

  private:
    bool observe_slot(std::uint32_t physical_address, std::uint8_t value,
                      std::uint32_t slot_address,
                      GsObservedPrivilegedRegister& destination) noexcept;

    GsPrivilegedDisplayState state_{};
};

} // namespace takedown::guest
