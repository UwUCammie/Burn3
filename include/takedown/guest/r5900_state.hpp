#pragma once

#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::guest {

namespace detail {
[[noreturn, gnu::cold]] void throw_r5900_index_error(const char* description);
inline void validate_r5900_index(const std::size_t index, const std::size_t count,
                                 const char* description) {
    if (index >= count) [[unlikely]] {
        throw_r5900_index_error(description);
    }
}
} // namespace detail

class R5900State {
  public:
    static constexpr std::size_t kGprCount = 32;
    static constexpr std::size_t kHiLoLaneCount = 2;
    static constexpr std::size_t kFprCount = 32;
    static constexpr std::size_t kCop0RegisterCount = 32;
    static constexpr std::size_t kVu0VectorRegisterCount = 32;
    static constexpr std::size_t kVu0ControlRegisterCount = 32;
    static constexpr std::size_t kVu0MacFlagHistoryDepth = 4;
    using Vu0MacFlagHistory = std::array<std::uint32_t, kVu0MacFlagHistoryDepth>;
    static constexpr std::uint32_t kFcr31WritableMask = 0x0083c078U;
    static constexpr std::uint32_t kFcr31FixedOneMask = 0x01000001U;

    explicit R5900State(GuestAddress entry_point = 0) noexcept;

    [[nodiscard]] GuestAddress pc() const noexcept;
    [[nodiscard]] GuestAddress next_pc() const noexcept;
    [[nodiscard]] bool is_executing_delay_slot() const noexcept;
    // Repositions the pipeline so the following sequential address is value+4.
    void set_pc(GuestAddress value) noexcept;
    // Commits one instruction. A future branch implementation can replace
    // next_pc after this call, leaving exactly one delay-slot instruction.
    void advance_pc() noexcept;
    void schedule_delayed_target(GuestAddress target) noexcept;

    [[nodiscard]] Gpr128 read_gpr(std::size_t index) const;
    void write_gpr(std::size_t index, const Gpr128& value);
    [[nodiscard]] std::uint32_t read_gpr_u32(std::size_t index) const;
    [[nodiscard]] std::uint64_t read_gpr_u64(std::size_t index) const;
    // Scalar EE operations replace only the low 64-bit half of a GPR.
    void write_gpr_u64(std::size_t index, std::uint64_t value);
    void write_gpr_sign_extended_32(std::size_t index, std::uint32_t value);

    [[nodiscard]] std::uint64_t read_hi(std::size_t lane) const;
    [[nodiscard]] std::uint64_t read_lo(std::size_t lane) const;
    void write_hi(std::size_t lane, std::uint64_t value);
    void write_lo(std::size_t lane, std::uint64_t value);

    // The EE SA register encodes a byte count; QFSRV later scales it by eight
    // when applying a bit shift.
    [[nodiscard]] std::uint8_t sa_byte_count() const noexcept;
    void set_sa_byte_count(std::uint8_t value) noexcept;

    [[nodiscard]] std::uint32_t read_fpr_bits(std::size_t index) const;
    void write_fpr_bits(std::size_t index, std::uint32_t value);
    [[nodiscard]] std::uint32_t fpu_accumulator_bits() const noexcept;
    void set_fpu_accumulator_bits(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t fcr31_bits() const noexcept;
    void set_fcr31_bits(std::uint32_t value) noexcept;

    [[nodiscard]] std::uint32_t read_cop0(std::size_t index) const;
    void write_cop0(std::size_t index, std::uint32_t value);
    // Architectural COP0 DI/EI effects. These centralize the EE privilege
    // predicate so interpreted, compiled, and native transaction paths cannot
    // disagree about when Status.EIE may change.
    void execute_cop0_di() noexcept;
    void execute_cop0_ei() noexcept;

    // COP2 exposes VU0's 32 128-bit vector registers to the EE. VF0 is the
    // architectural constant (0, 0, 0, 1) and ignores writes.
    [[nodiscard]] Gpr128 read_vu0_vector(std::size_t index) const;
    void write_vu0_vector(std::size_t index, const Gpr128& value);
    [[nodiscard]] Gpr128 vu0_accumulator() const noexcept;
    void set_vu0_accumulator(const Gpr128& value) noexcept;
    // COP2's VI bank contains 16 integer registers followed by VU control
    // registers. VI0 is the architectural zero register.
    [[nodiscard]] std::uint16_t read_vu0_integer(std::size_t index) const;
    void write_vu0_integer(std::size_t index, std::uint16_t value);
    [[nodiscard]] std::uint32_t read_vu0_control(std::size_t index) const;
    void write_vu0_control(std::size_t index, std::uint32_t value);
    // FMAC flags pass through four visibility stages. A completed VCALLMS
    // retains the drained value in every stage for the following launch,
    // while a bounded/fault exit retains the exact in-flight stages.
    [[nodiscard]] const Vu0MacFlagHistory& vu0_mac_flag_history() const noexcept;
    void persist_vu0_mac_flag_history(const Vu0MacFlagHistory& history) noexcept;
    void reset_vu0() noexcept;

    // Unchecked access for generated guest code (src/guest/ee_aot.cpp), whose
    // register indices are compile-time constants. It never writes GPR 0.
    [[nodiscard]] Gpr128* gpr_data() noexcept { return gpr_.data(); }
    [[nodiscard]] std::uint64_t* hi_data() noexcept { return hi_.data(); }
    [[nodiscard]] std::uint64_t* lo_data() noexcept { return lo_.data(); }
    // The whole fetch state: `pc` runs next and `next` after it; `delay_slot`
    // is set while `pc` is the delay slot of a committed transfer.
    void set_fetch_state(const GuestAddress pc, const GuestAddress next,
                         const bool delay_slot) noexcept {
        pc_ = pc;
        next_pc_ = next;
        executing_delay_slot_ = delay_slot;
    }

  private:
    std::array<Gpr128, kGprCount> gpr_{};
    std::array<std::uint64_t, kHiLoLaneCount> hi_{};
    std::array<std::uint64_t, kHiLoLaneCount> lo_{};
    std::array<std::uint32_t, kFprCount> fpr_bits_{};
    std::array<std::uint32_t, kCop0RegisterCount> cop0_{};
    std::array<Gpr128, kVu0VectorRegisterCount> vu0_vectors_{};
    Gpr128 vu0_accumulator_{};
    std::array<std::uint32_t, kVu0ControlRegisterCount> vu0_control_{};
    Vu0MacFlagHistory vu0_mac_flag_history_{};
    GuestAddress pc_{};
    GuestAddress next_pc_{};
    bool executing_delay_slot_{};
    std::uint32_t fpu_accumulator_bits_{};
    std::uint32_t fcr31_bits_{kFcr31FixedOneMask};
    std::uint8_t sa_byte_count_{};
};

// Register accessors are inline: generated guest code calls them with
// constant indices outside link-time optimization.
inline GuestAddress R5900State::pc() const noexcept { return pc_; }

inline GuestAddress R5900State::next_pc() const noexcept { return next_pc_; }

inline bool R5900State::is_executing_delay_slot() const noexcept { return executing_delay_slot_; }

inline void R5900State::set_pc(const GuestAddress value) noexcept {
    pc_ = value;
    next_pc_ = value + 4U;
    executing_delay_slot_ = false;
}

inline void R5900State::advance_pc() noexcept {
    pc_ = next_pc_;
    next_pc_ += 4U;
    executing_delay_slot_ = false;
}

inline Gpr128 R5900State::read_gpr(const std::size_t index) const {
    detail::validate_r5900_index(index, gpr_.size(), "R5900 GPR index is outside 0..31");
    if (index == 0U) {
        return {};
    }
    return gpr_[index];
}

inline void R5900State::write_gpr(const std::size_t index, const Gpr128& value) {
    detail::validate_r5900_index(index, gpr_.size(), "R5900 GPR index is outside 0..31");
    if (index != 0U) {
        gpr_[index] = value;
    }
}

inline std::uint32_t R5900State::read_gpr_u32(const std::size_t index) const {
    return read_gpr(index).words[0];
}

inline std::uint64_t R5900State::read_gpr_u64(const std::size_t index) const {
    const auto value = read_gpr(index);
    return static_cast<std::uint64_t>(value.words[0]) |
           (static_cast<std::uint64_t>(value.words[1]) << 32U);
}

inline void R5900State::write_gpr_u64(const std::size_t index, const std::uint64_t value) {
    auto result = read_gpr(index);
    result.words[0] = static_cast<std::uint32_t>(value);
    result.words[1] = static_cast<std::uint32_t>(value >> 32U);
    write_gpr(index, result);
}

inline void R5900State::write_gpr_sign_extended_32(const std::size_t index, const std::uint32_t value) {
    const auto extended = (value & 0x80000000U) == 0U
                              ? static_cast<std::uint64_t>(value)
                              : 0xffffffff00000000ULL | static_cast<std::uint64_t>(value);
    write_gpr_u64(index, extended);
}

inline std::uint64_t R5900State::read_hi(const std::size_t lane) const {
    detail::validate_r5900_index(lane, hi_.size(), "R5900 HI lane index is outside 0..1");
    return hi_[lane];
}

inline std::uint64_t R5900State::read_lo(const std::size_t lane) const {
    detail::validate_r5900_index(lane, lo_.size(), "R5900 LO lane index is outside 0..1");
    return lo_[lane];
}

inline void R5900State::write_hi(const std::size_t lane, const std::uint64_t value) {
    detail::validate_r5900_index(lane, hi_.size(), "R5900 HI lane index is outside 0..1");
    hi_[lane] = value;
}

inline void R5900State::write_lo(const std::size_t lane, const std::uint64_t value) {
    detail::validate_r5900_index(lane, lo_.size(), "R5900 LO lane index is outside 0..1");
    lo_[lane] = value;
}

inline std::uint8_t R5900State::sa_byte_count() const noexcept { return sa_byte_count_; }

inline void R5900State::set_sa_byte_count(const std::uint8_t value) noexcept {
    sa_byte_count_ = static_cast<std::uint8_t>(value & 0x0fU);
}

inline std::uint32_t R5900State::read_fpr_bits(const std::size_t index) const {
    detail::validate_r5900_index(index, fpr_bits_.size(), "R5900 FPR index is outside 0..31");
    return fpr_bits_[index];
}

inline void R5900State::write_fpr_bits(const std::size_t index, const std::uint32_t value) {
    detail::validate_r5900_index(index, fpr_bits_.size(), "R5900 FPR index is outside 0..31");
    fpr_bits_[index] = value;
}

inline std::uint32_t R5900State::fpu_accumulator_bits() const noexcept { return fpu_accumulator_bits_; }

inline void R5900State::set_fpu_accumulator_bits(const std::uint32_t value) noexcept {
    fpu_accumulator_bits_ = value;
}

inline std::uint32_t R5900State::fcr31_bits() const noexcept { return fcr31_bits_; }

inline void R5900State::set_fcr31_bits(const std::uint32_t value) noexcept {
    // Reserved fields ignore writes, while the EE's fixed implementation bits
    // always read as one. Keeping state normalized makes later CFC1 behavior
    // unambiguous too.
    fcr31_bits_ = (value & kFcr31WritableMask) | kFcr31FixedOneMask;
}

inline std::uint32_t R5900State::read_cop0(const std::size_t index) const {
    detail::validate_r5900_index(index, cop0_.size(), "R5900 COP0 register index is outside 0..31");
    return cop0_[index];
}

inline void R5900State::write_cop0(const std::size_t index, const std::uint32_t value) {
    detail::validate_r5900_index(index, cop0_.size(), "R5900 COP0 register index is outside 0..31");
    cop0_[index] = value;
}

inline Gpr128 R5900State::read_vu0_vector(const std::size_t index) const {
    detail::validate_r5900_index(index, vu0_vectors_.size(), "VU0 vector register index is outside 0..31");
    if (index == 0U) {
        return {.words = {0U, 0U, 0U, 0x3f800000U}};
    }
    return vu0_vectors_[index];
}

inline void R5900State::write_vu0_vector(const std::size_t index, const Gpr128& value) {
    detail::validate_r5900_index(index, vu0_vectors_.size(), "VU0 vector register index is outside 0..31");
    if (index != 0U) {
        vu0_vectors_[index] = value;
    }
}

inline Gpr128 R5900State::vu0_accumulator() const noexcept { return vu0_accumulator_; }

inline void R5900State::set_vu0_accumulator(const Gpr128& value) noexcept {
    vu0_accumulator_ = value;
}

inline std::uint16_t R5900State::read_vu0_integer(const std::size_t index) const {
    detail::validate_r5900_index(index, 16U, "VU0 integer register index is outside 0..15");
    return index == 0U ? 0U : static_cast<std::uint16_t>(vu0_control_[index]);
}

inline void R5900State::write_vu0_integer(const std::size_t index, const std::uint16_t value) {
    detail::validate_r5900_index(index, 16U, "VU0 integer register index is outside 0..15");
    if (index != 0U) {
        vu0_control_[index] = value;
    }
}

inline std::uint32_t R5900State::read_vu0_control(const std::size_t index) const {
    detail::validate_r5900_index(index, vu0_control_.size(), "VU0 control register index is outside 0..31");
    return index == 0U ? 0U : vu0_control_[index];
}

inline void R5900State::write_vu0_control(const std::size_t index, const std::uint32_t value) {
    detail::validate_r5900_index(index, vu0_control_.size(), "VU0 control register index is outside 0..31");
    if (index != 0U) {
        vu0_control_[index] = value;
        if (index == 17U) {
            // An architectural MAC write establishes the same baseline in
            // every visibility slot for the next micro launch. Micro
            // execution uses the dedicated persistence helper below when its
            // four in-flight values are distinct.
            vu0_mac_flag_history_.fill(value);
        }
    }
}

} // namespace takedown::guest
