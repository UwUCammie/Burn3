#pragma once

// Shared helpers for the one-function-per-file natives in src/runtime/game/n3/.
// The scalar float unit (COP1) on raw words, as the R5900 does it: operands lose
// denormals and infinities, add/subtract/multiply round toward zero, divide rounds
// to nearest, the multiply-accumulate forms (MADD.S/MADDA.S/MSUB.S) round the
// product first. Never use host float arithmetic for guest values.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <array>
#include <cstring>
#include <bit>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace takedown::runtime::n3 {

using guest::GuestAddress;
using guest::Gpr128;

namespace fpu {
using guest::ee_cop1::normalize_cop1_operand;
inline std::uint32_t finish(const std::uint32_t bits) {
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return guest::ee_cop1::is_cop1_denormal(bits) ? bits & 0x80000000U : bits;
}
// ADD.S / ADDA.S
inline std::uint32_t add(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// SUB.S / SUBA.S
inline std::uint32_t sub(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// MUL.S / MULA.S
inline std::uint32_t mul(const std::uint32_t a, const std::uint32_t b) {
    return finish(guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
}
// MADD.S fd = ACC + a*b, and MADDA.S ACC = ACC + a*b
inline std::uint32_t madd(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_add_normal_finite(normalize_cop1_operand(acc), product));
}
// MSUB.S fd = ACC - a*b, and MSUBA.S
inline std::uint32_t msub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    const auto product = normalize_cop1_operand(
        guest::ee_cop1::cop1_mul_normal_finite(normalize_cop1_operand(a), normalize_cop1_operand(b)));
    return finish(guest::ee_cop1::cop1_sub_normal_finite(normalize_cop1_operand(acc), product));
}
// DIV.S
inline std::uint32_t div(const std::uint32_t n, const std::uint32_t d) {
    if ((d & 0x7f800000U) == 0U) {
        return ((n ^ d) & 0x80000000U) | 0x7f7fffffU;
    }
    return normalize_cop1_operand(
        guest::ee_cop1::cop1_div_normal_finite(normalize_cop1_operand(n), normalize_cop1_operand(d)));
}
// SQRT.S
inline std::uint32_t sqrt(const std::uint32_t v) {
    if ((v & 0x7f800000U) == 0U) {
        return v & 0x80000000U;
    }
    return guest::ee_cop1::cop1_sqrt_normal_finite(normalize_cop1_operand(v) & 0x7fffffffU);
}
// NEG.S / ABS.S / MOV.S only touch the sign bit (no normalisation).
inline std::uint32_t neg(const std::uint32_t v) { return v ^ 0x80000000U; }
inline std::uint32_t abs(const std::uint32_t v) { return v & 0x7fffffffU; }
inline float value(const std::uint32_t bits) { return std::bit_cast<float>(normalize_cop1_operand(bits)); }
// C.LT.S / C.LE.S / C.EQ.S
inline bool lt(const std::uint32_t a, const std::uint32_t b) { return value(a) < value(b); }
inline bool le(const std::uint32_t a, const std::uint32_t b) { return value(a) <= value(b); }
inline bool eq(const std::uint32_t a, const std::uint32_t b) { return value(a) == value(b); }
// CVT.S.W
inline std::uint32_t from_int(const std::int32_t v) {
    return guest::ee_cop1::cop1_narrow_chop_zero(static_cast<double>(v));
}
// CVT.W.S (truncates toward zero; out of range saturates)
inline std::int32_t to_int(const std::uint32_t bits) {
    const float f = value(bits);
    if (!(f < 2147483648.0F)) return 0x7fffffff;
    if (!(f >= -2147483648.0F)) return static_cast<std::int32_t>(0x80000000U);
    return static_cast<std::int32_t>(f);
}
} // namespace fpu

// Vector-unit destination masks (x is the high bit, as the instructions encode them).
constexpr std::uint8_t kMaskW = 1U;
constexpr std::uint8_t kMaskXyz = 14U;
constexpr std::uint8_t kMaskXyzw = 15U;
// VF0: (0, 0, 0, 1).
constexpr Gpr128 kVf0{{0U, 0U, 0U, 0x3f800000U}};
constexpr unsigned kGlobalPointer = 28U;

inline Gpr128 load_quad(const guest::EeMemory& memory, const GuestAddress address) {
    const GuestAddress aligned = address & ~15U;
    return Gpr128{{memory.read_u32_le(aligned), memory.read_u32_le(aligned + 4U), memory.read_u32_le(aligned + 8U),
                   memory.read_u32_le(aligned + 12U)}};
}
inline void store_quad(guest::EeMemory& memory, const GuestAddress address, const Gpr128& value) {
    memory.write_u128_le(address & ~15U, value.words);
}

// A read-only view of guest memory that keeps its own writes, for call_cost
// resolvers: run the same templated body as the implementation on a DryMemory
// to learn the exact instruction count without changing guest state (reads see
// the run's own earlier writes, as the original's would). Same method names as
// guest::EeMemory, so a body written as `template <class Memory>` works on both.
class DryMemory {
public:
    explicit DryMemory(const guest::EeMemory& memory) noexcept : memory_(memory) {}

    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (const Line* line = find(address & ~15U); line != nullptr && (line->mask >> (address & 15U) & 1U) != 0U) {
            return line->bytes[address & 15U];
        }
        return memory_.read_u8(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress a) const {
        if (lines_.empty()) return memory_.read_u16_le(a);
        return static_cast<std::uint16_t>(read_u8(a) | (read_u8(a + 1U) << 8U));
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress a) const {
        if (lines_.empty()) return memory_.read_u32_le(a);
        const std::uint32_t offset = a & 15U;
        if (offset <= 12U) {
            const Line* line = find(a & ~15U);
            if (line == nullptr) return memory_.read_u32_le(a);
            if (((line->mask >> offset) & 0xFU) == 0xFU) {
                std::uint32_t value;
                std::memcpy(&value, line->bytes.data() + offset, 4U);
                return value;
            }
        }
        return static_cast<std::uint32_t>(read_u8(a)) | (static_cast<std::uint32_t>(read_u8(a + 1U)) << 8U) |
               (static_cast<std::uint32_t>(read_u8(a + 2U)) << 16U) | (static_cast<std::uint32_t>(read_u8(a + 3U)) << 24U);
    }
    [[nodiscard]] std::uint64_t read_u64_le(const GuestAddress a) const {
        return static_cast<std::uint64_t>(read_u32_le(a)) | (static_cast<std::uint64_t>(read_u32_le(a + 4U)) << 32U);
    }
    void write_u8(const GuestAddress a, const std::uint8_t v) {
        Line& line = slot(a & ~15U);
        line.bytes[a & 15U] = v;
        line.mask = static_cast<std::uint16_t>(line.mask | (1U << (a & 15U)));
    }
    void write_u16_le(const GuestAddress a, const std::uint16_t v) {
        write_u8(a, static_cast<std::uint8_t>(v)); write_u8(a + 1U, static_cast<std::uint8_t>(v >> 8U));
    }
    void write_u32_le(const GuestAddress a, const std::uint32_t v) {
        const std::uint32_t offset = a & 15U;
        if (offset > 12U) {
            for (std::uint32_t i = 0U; i < 4U; ++i) write_u8(a + i, static_cast<std::uint8_t>(v >> (8U * i)));
            return;
        }
        Line& line = slot(a & ~15U);
        std::memcpy(line.bytes.data() + offset, &v, 4U);
        line.mask = static_cast<std::uint16_t>(line.mask | (0xFU << offset));
    }
    void write_u64_le(const GuestAddress a, const std::uint64_t v) {
        write_u32_le(a, static_cast<std::uint32_t>(v)); write_u32_le(a + 4U, static_cast<std::uint32_t>(v >> 32U));
    }
    void write_u128_le(const GuestAddress a, const std::array<std::uint32_t, 4>& words) {
        for (std::size_t i = 0U; i < 4U; ++i) write_u32_le(a + static_cast<GuestAddress>(4U * i), words[i]);
    }
    // Whether guest memory backs [address, address + length) (the same answer as the memory it views).
    [[nodiscard]] bool contains(const GuestAddress address, const std::size_t length) const noexcept {
        return memory_.contains(address, length);
    }
    // Copying a DryMemory gives an independent view with the same writes so far (a dry run inside a dry run).

    // Whether every write lies in plain memory (main RAM at its direct addresses, or the scratchpad), where writing
    // the bytes back one by one has the same effect as the run's own stores.
    [[nodiscard]] bool writes_plain_memory() const noexcept {
        for (const Line& line : lines_) {
            const bool ram = line.base < 0x02000000U;
            const bool scratchpad = line.base >= 0x70000000U && line.base < 0x70004000U;
            if (!ram && !scratchpad) return false;
        }
        return true;
    }
    // Runs VU0 microprogram `entry` for a dry run. A microprogram reads and writes only the VU0 registers (on `cpu`,
    // the dry run's copy) and VU0 data memory, so it runs on the real VU0 data memory with this run's earlier stores
    // there applied; the bytes it changes become this run's writes and the real VU0 data memory is put back.
    // Nothing else runs meanwhile, so guest state is unchanged afterwards.
    guest::Vu0MicroRunResult run_vu0_micro(guest::R5900State& cpu, const std::uint16_t entry) {
        auto& real = const_cast<guest::EeMemory&>(memory_);
        std::byte* const data = real.vu0_data_local_bytes();
        std::array<std::byte, guest::kVu0MemorySize> saved;
        std::memcpy(saved.data(), data, saved.size());
        for (const Line& line : lines_) {
            if (line.base < guest::kVu0DataMemoryStart || line.base - guest::kVu0DataMemoryStart >= guest::kVu0MemorySize) continue;
            for (std::uint32_t i = 0U; i < 16U; ++i) {
                if ((line.mask >> i & 1U) != 0U) data[line.base - guest::kVu0DataMemoryStart + i] = std::byte{line.bytes[i]};
            }
        }
        std::array<std::byte, guest::kVu0MemorySize> before;
        std::memcpy(before.data(), data, before.size());
        guest::Vu0MicroRunResult result;
        try {
            result = guest::run_vu0_micro(cpu, real, entry);
        } catch (...) {
            std::memcpy(data, saved.data(), saved.size());
            throw;
        }
        std::array<std::byte, guest::kVu0MemorySize> after;
        std::memcpy(after.data(), data, after.size());
        std::memcpy(data, saved.data(), saved.size());
        for (std::size_t i = 0U; i < after.size(); ++i) {
            if (after[i] != before[i]) {
                write_u8(guest::kVu0DataMemoryStart + static_cast<GuestAddress>(i), std::to_integer<std::uint8_t>(after[i]));
            }
        }
        return result;
    }
    // Applies the run's writes to guest memory (see writes_plain_memory).
    void commit(guest::EeMemory& memory) const {
        for (const Line& line : lines_) {
            if (line.mask == 0xFFFFU) {
                std::uint64_t low;
                std::uint64_t high;
                std::memcpy(&low, line.bytes.data(), 8U);
                std::memcpy(&high, line.bytes.data() + 8U, 8U);
                memory.write_u64_le(line.base, low);
                memory.write_u64_le(line.base + 8U, high);
                continue;
            }
            for (std::uint32_t i = 0U; i < 16U; ++i) {
                if ((line.mask >> i & 1U) != 0U) memory.write_u8(line.base + i, line.bytes[i]);
            }
        }
    }

private:
    // The run's writes as 16-byte lines (bytes + a mask of the bytes written), found through a small
    // open-addressing index keyed by the line address: a read costs one probe, not a scan of every write.
    struct Line {
        GuestAddress base{};
        std::uint16_t mask{};
        std::array<std::uint8_t, 16> bytes{};
    };
    static std::uint32_t hash(const GuestAddress base) noexcept { return (base >> 4U) * 0x9E3779B1U; }
    [[nodiscard]] const Line* find(const GuestAddress base) const noexcept {
        if (lines_.empty()) return nullptr;
        const std::uint32_t bits = static_cast<std::uint32_t>(index_.size() - 1U);
        for (std::uint32_t i = hash(base) & bits;; i = (i + 1U) & bits) {
            const std::int32_t at = index_[i];
            if (at < 0) return nullptr;
            if (lines_[static_cast<std::size_t>(at)].base == base) return &lines_[static_cast<std::size_t>(at)];
        }
    }
    Line& slot(const GuestAddress base) {
        if (const Line* line = find(base); line != nullptr) return const_cast<Line&>(*line);
        if ((lines_.size() + 1U) * 2U > index_.size()) {
            index_.assign(index_.empty() ? 64U : index_.size() * 2U, -1);
            for (std::size_t n = 0U; n < lines_.size(); ++n) place(lines_[n].base, static_cast<std::int32_t>(n));
        }
        lines_.push_back(Line{base, 0U, {}});
        place(base, static_cast<std::int32_t>(lines_.size() - 1U));
        return lines_.back();
    }
    void place(const GuestAddress base, const std::int32_t at) noexcept {
        const std::uint32_t bits = static_cast<std::uint32_t>(index_.size() - 1U);
        std::uint32_t i = hash(base) & bits;
        while (index_[i] >= 0) i = (i + 1U) & bits;
        index_[i] = at;
    }

    const guest::EeMemory& memory_;
    std::vector<Line> lines_;
    std::vector<std::int32_t> index_;
};

// Quadword helpers for either memory type.
template <class Memory>
Gpr128 load_quad_any(const Memory& memory, const GuestAddress address) {
    const GuestAddress aligned = address & ~15U;
    return Gpr128{{memory.read_u32_le(aligned), memory.read_u32_le(aligned + 4U), memory.read_u32_le(aligned + 8U),
                   memory.read_u32_le(aligned + 12U)}};
}
template <class Memory>
void store_quad_any(Memory& memory, const GuestAddress address, const Gpr128& value) {
    memory.write_u128_le(address & ~15U, value.words);
}

// VCALLMS for a native body: the original's launch on guest memory, DryMemory::run_vu0_micro in a dry run, so a
// dry run gets the same VI/VF results and a caller's dry run can branch on them.
template <class Memory>
guest::Vu0MicroRunResult run_vu0_micro_any(guest::R5900State& cpu, Memory& memory, const std::uint16_t entry) {
    if constexpr (std::is_same_v<std::remove_cvref_t<Memory>, guest::EeMemory>) {
        return guest::run_vu0_micro_call(cpu, memory, entry);
    } else {
        return memory.run_vu0_micro(cpu, entry);
    }
}

} // namespace takedown::runtime::n3

#include "takedown/runtime/game/n3_call.hpp"
