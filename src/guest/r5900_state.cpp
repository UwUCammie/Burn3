#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace takedown::guest {

namespace detail {

void throw_r5900_index_error(const char* description) {
    throw std::out_of_range(description);
}

} // namespace detail

namespace {

constexpr std::size_t kCop0StatusIndex = 12U;
constexpr std::size_t kVu0MacFlagControlIndex = 17U;
constexpr std::size_t kVu0ResetControlIndex = 29U;
// Status.ERL | Status.EXL, plus the EDI bit that lets EI/DI work outside
// kernel mode.
constexpr std::uint32_t kStatusEiDiAnyModeMask = 0x00020006U;
constexpr std::uint32_t kStatusKsuMask = 0x00000018U;
constexpr std::uint32_t kStatusEie = 0x00010000U;

// Indirect-call targets in the retail bank-creation code; only used by the
// TAKEDOWN_CFLOW=1 diagnostic tap below.
constexpr std::array<std::uint32_t, 18U> kBankCreationFamily{
    0x002ab880U, 0x002ab4f0U, 0x002ab500U, 0x002ab790U, 0x002abce0U, 0x002abc00U,
    0x002abe50U, 0x002a3b20U, 0x002a46a0U, 0x002a4a70U, 0x002940b0U, 0x002ab9f0U,
    0x002ac610U, 0x002acc70U, 0x002acbd0U, 0x002ac8e0U, 0x002ad6e0U, 0x002ac540U};

bool bank_creation_flow_tap_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TAKEDOWN_CFLOW");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// EI/DI only take effect in kernel mode, or while EXL/ERL/EDI is set.
bool can_change_eie(const std::uint32_t status) noexcept {
    return (status & kStatusEiDiAnyModeMask) != 0U || (status & kStatusKsuMask) == 0U;
}

} // namespace

R5900State::R5900State(const GuestAddress entry_point) noexcept
    : pc_{entry_point}, next_pc_{entry_point + 4U} {}

void R5900State::schedule_delayed_target(const GuestAddress target) noexcept {
    next_pc_ = target;
    executing_delay_slot_ = true;
    if (bank_creation_flow_tap_enabled()) {
        for (const auto family : kBankCreationFamily) {
            if (target == family) {
                std::fprintf(stderr,
                             "[CFLOW] target=0x%08x site=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x "
                             "a3=0x%08x\n",
                             target, pc_, read_gpr_u32(4U), read_gpr_u32(5U), read_gpr_u32(6U),
                             read_gpr_u32(7U));
                break;
            }
        }
    }
}

void R5900State::execute_cop0_di() noexcept {
    const auto status = cop0_[kCop0StatusIndex];
    if (can_change_eie(status)) {
        cop0_[kCop0StatusIndex] = status & ~kStatusEie;
    }
}

void R5900State::execute_cop0_ei() noexcept {
    const auto status = cop0_[kCop0StatusIndex];
    if (can_change_eie(status)) {
        cop0_[kCop0StatusIndex] = status | kStatusEie;
    }
}

const R5900State::Vu0MacFlagHistory& R5900State::vu0_mac_flag_history() const noexcept {
    return vu0_mac_flag_history_;
}

void R5900State::persist_vu0_mac_flag_history(const Vu0MacFlagHistory& history) noexcept {
    vu0_mac_flag_history_ = history;
    vu0_control_[kVu0MacFlagControlIndex] = history[0];
}

void R5900State::reset_vu0() noexcept {
    vu0_control_[kVu0ResetControlIndex] &= ~0xffU;
}

} // namespace takedown::guest
