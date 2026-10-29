#pragma once

#include <array>
#include <cstdint>

namespace takedown::guest::vu1_detail {

// STATUS is never consumed by a VU1 lower instruction. Preserve its delayed
// history as MAC nibbles, reducing them only when architectural state escapes.
// The high word retains DIV flags and reserved input bits until FMAC clears
// the latter, exactly as the reference's (old & 0x0ff0) update does.
class DeferredVu1Status {
public:
    constexpr DeferredVu1Status() noexcept = default;
    explicit constexpr DeferredVu1Status(std::uint32_t status) noexcept
        : encoded_(spread(status) | (std::uint64_t{spread(status >> 6U)} << 16U) |
                   (std::uint64_t{status & ~0x3cfU} << 32U)) {}

    constexpr void update_fmac(std::uint16_t mac) noexcept {
        encoded_ = (encoded_ & ((std::uint64_t{0xc30U} << 32U) | 0xffff0000ULL)) |
                   mac | (std::uint64_t{mac} << 16U);
    }

    constexpr void merge_fdiv(std::uint32_t status_di) noexcept {
        encoded_ = (encoded_ & ~(std::uint64_t{0x30U} << 32U)) |
                   (std::uint64_t{status_di | (status_di << 6U)} << 32U);
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return reduce(static_cast<std::uint16_t>(encoded_)) |
               (reduce(static_cast<std::uint16_t>(encoded_ >> 16U)) << 6U) |
               static_cast<std::uint32_t>(encoded_ >> 32U);
    }

private:
    static constexpr std::uint32_t spread(std::uint32_t bits) noexcept {
        return (bits & 1U) | ((bits & 2U) << 3U) |
               ((bits & 4U) << 6U) | ((bits & 8U) << 9U);
    }
    static constexpr std::uint32_t reduce(std::uint16_t mac) noexcept {
        const std::uint32_t pairs = mac | (mac >> 1U);
        const std::uint32_t nibbles = (pairs | (pairs >> 2U)) & 0x1111U;
        return ((nibbles * 0x1248U) >> 12U) & 15U;
    }
    std::uint64_t encoded_{};
};

inline std::array<DeferredVu1Status, 4> defer_vu1_status_history(
    const std::array<std::uint32_t, 4>& history) noexcept {
    return {DeferredVu1Status{history[0]}, DeferredVu1Status{history[1]},
            DeferredVu1Status{history[2]}, DeferredVu1Status{history[3]}};
}

} // namespace takedown::guest::vu1_detail
