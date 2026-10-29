#include "takedown/guest/ee_kernel.hpp"

#include "takedown/content/mod_overlay.hpp"
#include "takedown/platform/host_wait_stats.hpp"
#include "takedown/save/save_card_device.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace takedown::guest {
namespace {
// Sweep-rate nibble for each |delta| step (0..256) of RWA's volume ramp.
constexpr std::array<std::uint8_t, 257U> kRwaVolumeSweepRates{
    0xffU, 0x43U, 0x3dU, 0x3aU, 0x38U, 0x37U, 0x35U, 0x34U, 0x33U, 0x33U, 0x32U, 0x32U, 0x31U, 0x31U, 0x30U, 0x2fU,
    0x2fU, 0x2fU, 0x2fU, 0x2eU, 0x2eU, 0x2eU, 0x2eU, 0x2eU, 0x2dU, 0x2dU, 0x2cU, 0x2cU, 0x2cU, 0x2cU, 0x2bU, 0x2bU,
    0x2bU, 0x2bU, 0x2bU, 0x2bU, 0x2bU, 0x2bU, 0x2aU, 0x2aU, 0x2aU, 0x2aU, 0x2aU, 0x2aU, 0x2aU, 0x29U, 0x29U, 0x29U,
    0x29U, 0x29U, 0x29U, 0x29U, 0x28U, 0x28U, 0x28U, 0x28U, 0x28U, 0x28U, 0x28U, 0x28U, 0x27U, 0x27U, 0x27U, 0x27U,
    0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x27U, 0x26U, 0x26U, 0x26U, 0x26U, 0x26U,
    0x26U, 0x26U, 0x26U, 0x26U, 0x26U, 0x26U, 0x26U, 0x26U, 0x26U, 0x25U, 0x25U, 0x25U, 0x25U, 0x25U, 0x25U, 0x25U,
    0x25U, 0x25U, 0x25U, 0x25U, 0x25U, 0x25U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U,
    0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x24U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U,
    0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U, 0x23U,
    0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U,
    0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U,
    0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U,
    0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U, 0x21U,
    0x21U, 0x21U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U, 0x20U,
    0x20U, 0x20U, 0x20U, 0x20U, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU,
    0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU, 0x1fU,
    0x1fU,
};

// Magnitude of a signed 16-bit RWA volume register; -32768 saturates.
[[nodiscard]] std::int32_t rwa_volume_magnitude(const std::int32_t value) noexcept {
    if (value == -32768) {
        return 32767;
    }
    return std::min(std::abs(value), 32767);
}

struct RwaVolumeWrites {
    std::array<std::uint16_t, 2U> values{};
    std::size_t count{};
};

[[nodiscard]] RwaVolumeWrites
rwa_volume_update_registers(const std::int32_t current, const std::int32_t desired_target,
                            const bool desired_negative, const bool force_zero,
                            std::int32_t& cached_magnitude, bool& phase_negative,
                            bool& in_progress) {
    RwaVolumeWrites writes{};
    const auto encode = [](const std::int32_t magnitude, const bool negative) {
        const int signed_value = negative ? -magnitude : magnitude;
        return static_cast<std::uint16_t>((signed_value >> 1) & 0x7fff);
    };
    const int target = (phase_negative == desired_negative && !force_zero) ? desired_target : 0;
    if (in_progress) {
        const std::int32_t magnitude = rwa_volume_magnitude(current);
        const bool current_negative = current < 0;
        if (phase_negative != current_negative) {
            writes.values[writes.count++] = encode(magnitude, phase_negative);
        }
        const int difference = target - magnitude;
        if (difference >= -384 && difference <= 384) {
            cached_magnitude = target;
            in_progress = false;
            phase_negative = desired_negative;
            writes.values[writes.count++] = encode(target, desired_negative);
        } else {
            cached_magnitude = magnitude;
        }
    }
    const int difference = target - cached_magnitude;
    const int delta = difference >= 0 ? (difference >> 7) : -((127 - difference) / 128);
    if (delta != 0) {
        const auto magnitude = static_cast<std::size_t>(std::min(std::abs(delta), 256));
        const auto rate = static_cast<std::uint16_t>(kRwaVolumeSweepRates[magnitude]);
        std::uint16_t reg{};
        if (delta < 0) {
            reg = static_cast<std::uint16_t>((rate + 1U) | 0xa000U);
        } else {
            reg = static_cast<std::uint16_t>(rate | 0x8000U);
        }
        if (phase_negative) {
            reg = static_cast<std::uint16_t>(reg | 0x1000U);
        }
        writes.values[writes.count++] = reg;
        in_progress = true;
    } else if (phase_negative != desired_negative) {
        cached_magnitude = target;
        in_progress = false;
        phase_negative = desired_negative;
        writes.values[writes.count++] = encode(target, desired_negative);
    }
    return writes;
}

// RECOVERY: statements match; only the entry profile count (and so block layout) differs,
// which follows from the original's call-graph context.
std::optional<std::string> read_guest_string(const EeMemory& memory,
                                                           const GuestAddress address,
                                                           const std::size_t capacity) {
    if (!memory.contains(address, capacity)) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(capacity);
    for (std::size_t index = 0; index < capacity; ++index) {
        const std::uint8_t byte = memory.read_u8(address + static_cast<GuestAddress>(index));
        if (byte == 0U) {
            if (result.empty()) {
                return std::nullopt;
            }
            return result;
        }
        result.push_back(static_cast<char>(byte));
    }
    return std::nullopt;
}

[[nodiscard]] bool save_card_trace_enabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("TAKEDOWN_SAVE_CARD_TRACE");
        return value != nullptr && *value == '1';
    }();
    return enabled;
}

// Equal-priority threads rotate once the running one has held the CPU this long.
constexpr std::uint64_t kThreadTimeSliceEeCycles = 294912U;

// Queue entry standing for the SIF command callback (which has no thread id)
// among a semaphore's waiters.
constexpr std::int32_t kSifCommandCallbackWaiterId = 0;

// Lowest IOP RAM address the virtual IOPHEAP hands out.
constexpr std::uint32_t kVirtualIopHeapAllocationBase = 0x00100000U;

// True when [address, address + length) is a word-aligned guest range that the
// kernel may read or write directly: RAM or the local memories, never a
// KSEG0/KSEG1/KUSEG address that runs past the end of main RAM.
template <std::uint32_t Length>
[[nodiscard]] bool is_accessible_guest_range(const EeMemory& memory, const GuestAddress address) {
    if (address == 0U || (address & (Length - 1U)) != 0U || !memory.contains(address, Length)) {
        return false;
    }
    const auto segment = address & 0xe0000000U;
    if (segment == 0U || segment == 0x80000000U || segment == 0xa0000000U) {
        const auto physical = static_cast<unsigned long long>(address & 0x1fffffffU);
        return physical <= memory.size() && memory.size() - physical >= Length;
    }
    return true;
}

// Signed 32-bit view of an argument register (a0..a3 are 4..7).
[[nodiscard]] std::int32_t signed_argument(const R5900State& cpu, const std::size_t reg) {
    return static_cast<std::int32_t>(cpu.read_gpr_u32(reg));
}

[[nodiscard]] std::int32_t thread_id_argument(const R5900State& cpu) {
    return static_cast<std::int32_t>(cpu.read_gpr_u32(4U));
}

// base + size as a guest address; false when the sum leaves the 32-bit space.
[[nodiscard]] bool checked_end(const GuestAddress base, const std::uint32_t size,
                               GuestAddress& end) {
    const unsigned long long sum =
        static_cast<unsigned long long>(base) + static_cast<unsigned long long>(size);
    if (sum > 0xffffffffULL) {
        return false;
    }
    end = static_cast<GuestAddress>(sum);
    return true;
}

[[nodiscard]] std::int32_t read_i32(const EeMemory& memory, const GuestAddress address) {
    return static_cast<std::int32_t>(memory.read_u32_le(address));
}

// SPU2 pitch register for a source rate in Hz (0x1000 is 48 kHz).
[[nodiscard]] std::uint16_t spu_pitch_for_rate(const std::uint32_t rate) noexcept {
    const int numerator = static_cast<int>(rate) << 12;
    return static_cast<std::uint16_t>(numerator / 48000);
}

// RWA voice snapshots are little-endian byte records.
template <std::size_t N>
[[nodiscard]] std::uint32_t load_le32(std::array<std::uint8_t, N>& bytes,
                                      const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

template <std::size_t N>
void store_le32(std::array<std::uint8_t, N>& bytes, const std::size_t offset,
                const std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::byte byte_of(const std::uint32_t value, const std::size_t shift) {
    return static_cast<std::byte>(value >> shift);
}

// True when the NUL-terminated guest string at `address` is exactly `expected`
// and fits a buffer of `capacity` bytes.
bool guest_string_equals(const EeMemory& memory, const GuestAddress address,
                         const std::string_view expected, const std::size_t capacity) {
    if (expected.size() >= capacity || !memory.contains(address, expected.size() + 1U)) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (memory.read_u8(address + static_cast<GuestAddress>(index)) !=
            static_cast<std::uint8_t>(expected[index])) {
            return false;
        }
    }
    return memory.read_u8(address + static_cast<GuestAddress>(expected.size())) == 0U;
}

EeSemaphoreState& semaphore_at(std::vector<EeSemaphoreState>& semaphores,
                               const std::optional<std::size_t>& index) {
    return semaphores[*index];
}

template <typename Sockets>
bool socket_is_active(Sockets& sockets, const std::size_t index) {
    return index < sockets.size() && sockets[index].active;
}

template <typename Range, typename Iterator>
int slot_index_or(const Range& range, const Iterator found, const int missing) {
    return found == range.end() ? missing : static_cast<int>(found - range.begin());
}
[[nodiscard]] std::int32_t delete_socket_status(const bool valid) noexcept {
    return valid ? 1 : -22;
}

template <typename Sockets>
[[nodiscard]] std::uint32_t descriptor_type_of(Sockets& sockets, const std::size_t index) noexcept {
    return sockets[index].descriptor_type;
}

[[nodiscard]] std::size_t slot_of(const std::uint32_t socket) noexcept {
    return socket;
}
} // namespace

EeKernelHle::EeKernelHle(std::filesystem::path disc_root)
    : disc_root_(std::move(disc_root)),
      sif_registers_{{2U, 0x000f0000U}, {4U, 0x00070000U}, {0x80000002U, 1U}} {
    mixer_slot_rates_.fill(22050U);
}

EeKernelCallResult EeKernelHle::handle_system_call(R5900State& cpu, EeMemory& memory) {
    const std::uint32_t number = cpu.read_gpr_u32(3U);
    // SetAlarm/ReleaseAlarm are always the built-in ones, in both the plain
    // and the negative (interrupt-safe) numbering.
    if (number == 252U || number == 0xffffff02U) {
        return set_alarm(cpu);
    }
    if (number == 253U || number == 0xffffff01U) {
        return release_alarm(cpu);
    }
    // A game-installed handler hides everything but the three table calls.
    if (registered_syscall_handler(number).has_value()) {
        switch (number) {
        case 90U:
            return copy_words(cpu, memory);
        case 91U:
            return get_entry_address(cpu, memory);
        case 131U:
            return find_address(cpu);
        default:
            return {EeKernelCallStatus::Unsupported, number, ee_kernel_call_name(number)};
        }
    }

    switch (number) {
    case 2U:
        return set_gs_crt(cpu);
    case 60U:
        return setup_thread(cpu, memory);
    case 61U:
        return setup_heap(cpu, memory);
    case 62U:
        return end_of_heap(cpu);
    case 64U:
        return create_semaphore(cpu, memory);
    case 66U:
    case 0xffffffbdU:
        return signal_semaphore_call(cpu);
    case 68U:
        return wait_semaphore(cpu);
    case 69U:
        return poll_semaphore(cpu);
    case 32U:
        return create_thread(cpu, memory);
    case 33U:
        return delete_thread(cpu);
    case 34U:
        return start_thread(cpu);
    case 35U:
        return exit_thread(cpu, false);
    case 36U:
        return exit_thread(cpu, true);
    case 37U:
        return terminate_thread(cpu);
    case 47U:
    case 0xffffffd1U:
        return get_thread_id(cpu);
    case 48U:
        return refer_thread_status(cpu, memory);
    case 41U:
        return change_thread_priority(cpu);
    case 50U:
        return sleep_thread(cpu);
    case 51U:
    case 0xffffffccU:
        return wakeup_thread(cpu);
    case 252U:
    case 0xffffff02U:
        return set_alarm(cpu);
    case 253U:
    case 0xffffff01U:
        return release_alarm(cpu);
    case 65U:
        return delete_semaphore(cpu);
    case 74U:
        return set_osd_config_param(cpu, memory);
    case 75U:
        return get_osd_config_param(cpu, memory);
    case 111U:
        return get_osd_config_param2(cpu, memory);
    case 16U:
        return add_intc_handler(cpu);
    case 17U:
        return remove_intc_handler(cpu);
    case 20U:
        return enable_intc(cpu, memory);
    case 18U:
        return add_dmac_handler(cpu);
    case 19U:
        return remove_dmac_handler(cpu);
    case 22U:
        return enable_dmac(cpu, memory);
    case 23U:
        return disable_dmac(cpu, memory);
    case 100U:
        return flush_cache(cpu);
    case 113U:
        return put_gs_interrupt_mask(cpu, memory);
    case 115U:
        return set_vsync_flag(cpu, memory);
    case 107U:
        return stop_sif_dma(cpu, memory);
    case 120U:
        return set_sif_dma_chain(cpu, memory);
    case 0xffffff89U:
        return sif_dma_stat(cpu, memory);
    case 118U:
        // sceSifInit: nothing to bring up, report failure-free "already initialised".
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 118U, "sceSifInit"};
    case 119U:
        return set_sif_dma(cpu, memory);
    case 121U:
        return set_sif_register(cpu);
    case 122U:
        return get_sif_register(cpu);
    case 124U:
        return deci2_call(cpu, memory);
    case 116U:
        return set_syscall(cpu);
    default:
        return {EeKernelCallStatus::Unsupported, number, ee_kernel_call_name(number)};
    }
}

EeKernelCallResult EeKernelHle::get_sif_register(R5900State& cpu) {
    const std::uint32_t number = cpu.read_gpr_u32(4U);
    cpu.write_gpr_sign_extended_32(2U, sif_register_value(number));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 122U, "sceSifGetReg"};
}

EeKernelCallResult EeKernelHle::set_sif_register(R5900State& cpu) {
    const std::uint32_t number = cpu.read_gpr_u32(4U);
    const std::uint32_t requested = cpu.read_gpr_u32(5U);
    // SMFLAG is write-one-to-set.
    set_sif_register_value(number,
                           number == 4U ? (requested | sif_register_value(4U)) : requested);
    cpu.write_gpr_sign_extended_32(2U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 121U, "sceSifSetReg"};
}

EeKernelCallResult EeKernelHle::sif_dma_stat(R5900State& cpu, EeMemory& memory) {
    const auto is_requested = [id = static_cast<std::int32_t>(cpu.read_gpr_u32(4U))](
                                  const std::int32_t candidate) { return candidate == id; };
    service_sif_dma_completions(memory);
    const auto batch = std::ranges::find_if(incomplete_sif_dma_batches_.begin(),
                                            incomplete_sif_dma_batches_.end(), is_requested,
                                            &IncompleteSifDmaBatch::id);
    // 0: the transfer is still running; -1: finished or never submitted.
    cpu.write_gpr_sign_extended_32(2U, batch == incomplete_sif_dma_batches_.end() ? 0xffffffffU : 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 0xffffff89U, "sceSifDmaStat"};
}

EeKernelCallResult EeKernelHle::put_gs_interrupt_mask(R5900State& cpu, EeMemory& memory) {
    const auto previous = memory.gs_interrupt_mask();
    memory.write_u32_le(0x12001010U, cpu.read_gpr_u32(4U));
    cpu.write_gpr_u64(2U, previous);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 113U, "GsPutIMR"};
}

EeKernelCallResult EeKernelHle::disable_dmac(R5900State& cpu, EeMemory& memory) {
    const std::uint32_t channel = cpu.read_gpr_u32(4U);
    if (channel > 9U) {
        return {EeKernelCallStatus::InvalidArguments, 23U, "DisableDmac"};
    }
    const unsigned bit = 1U << channel;
    const bool was_enabled = (enabled_dmac_channels_ & bit) != 0U;
    enabled_dmac_channels_ &= ~bit;
    const auto status = memory.dmac_status();
    memory.set_dmac_status((status & 0xffffU) | (enabled_dmac_channels_ << 16U));
    cpu.write_gpr_sign_extended_32(2U, was_enabled ? 1U : 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 23U, "DisableDmac"};
}

EeKernelCallResult EeKernelHle::enable_dmac(R5900State& cpu, EeMemory& memory) {
    const std::uint32_t channel = cpu.read_gpr_u32(4U);
    if (channel > 9U) {
        return {EeKernelCallStatus::InvalidArguments, 22U, "EnableDmac"};
    }
    const unsigned bit = 1U << channel;
    const bool newly_enabled = (enabled_dmac_channels_ & bit) == 0U;
    enabled_dmac_channels_ |= bit;
    const auto status = memory.dmac_status();
    memory.set_dmac_status((status & 0xffffU) | (enabled_dmac_channels_ << 16U));
    cpu.write_gpr_sign_extended_32(2U, newly_enabled ? 1U : 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 22U, "EnableDmac"};
}

EeKernelCallResult EeKernelHle::enable_intc(R5900State& cpu, EeMemory& memory) {
    const std::uint32_t cause = cpu.read_gpr_u32(4U);
    if (cause > 31U) {
        return {EeKernelCallStatus::InvalidArguments, 20U, "EnableIntc"};
    }
    const unsigned bit = 1U << cause;
    const auto previous_mask = memory.intc_mask();
    const bool changed = (previous_mask & bit) == 0U;
    if (changed) {
        memory.set_intc_mask(previous_mask | bit);
    }
    cpu.write_gpr_sign_extended_32(2U, changed ? 1U : 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 20U, "EnableIntc"};
}

EeKernelCallResult EeKernelHle::get_osd_config_param(R5900State& cpu, EeMemory& memory) {
    const GuestAddress address = cpu.read_gpr_u32(4U);
    if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
        return {EeKernelCallStatus::InvalidArguments, 75U, "GetOsdConfigParam"};
    }
    memory.write_u32_le(address, osd_config_param_);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 75U, "GetOsdConfigParam"};
}

EeKernelCallResult EeKernelHle::get_thread_id(R5900State& cpu) {
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(current_thread_id_));
    cpu.write_gpr_u64(3U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 47U, "GetThreadId"};
}

EeKernelCallResult EeKernelHle::signal_semaphore_call(R5900State& cpu) {
    if (signal_semaphore(static_cast<std::int32_t>(cpu.read_gpr_u32(4U)))) {
        cpu.write_gpr_u64(2U, 0U);
    } else {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 66U, "SignalSema"};
}

const EeKernelStartupState& EeKernelHle::startup_state() const noexcept { return startup_; }

const EeGsCrtState& EeKernelHle::gs_crt_state() const noexcept { return gs_crt_; }

const EeVsyncFlagState& EeKernelHle::vsync_flag_state() const noexcept { return vsync_flag_; }

std::span<const EeSemaphoreState> EeKernelHle::semaphores() const noexcept { return semaphores_; }

std::span<const EeSyscallHandlerState> EeKernelHle::syscall_handlers() const noexcept {
    return syscall_handlers_;
}

std::span<const EeCacheMaintenanceEvent> EeKernelHle::cache_maintenance_events() const noexcept {
    return cache_maintenance_events_;
}

std::span<const EeIntcHandlerState> EeKernelHle::intc_handlers() const noexcept {
    return intc_handlers_;
}

std::span<const EeDmacHandlerState> EeKernelHle::dmac_handlers() const noexcept {
    return dmac_handlers_;
}

std::span<const EeAlarmState> EeKernelHle::alarms() const noexcept { return alarms_; }

void EeKernelHle::set_ee_cycle_count(const std::uint64_t cycles) noexcept {
    // The kernel clock never runs backwards.
    ee_cycle_count_ = std::max(ee_cycle_count_, cycles);
}

void EeKernelHle::service_sif_dma_completions(EeMemory& memory) {
    // Save-card answers whose latency has elapsed become ordinary SIF command
    // callbacks.
    while (!delayed_save_card_callbacks_.empty() &&
           delayed_save_card_callbacks_.front().ready_ee_cycles <= ee_cycle_count_) {
        pending_sif_command_callbacks_.push_back(delayed_save_card_callbacks_.front().callback);
        if (save_card_trace_enabled()) {
            std::fprintf(stderr, "[save-card] promote ready=%llu now=%llu pending=%zu\n",
                         static_cast<unsigned long long>(
                             delayed_save_card_callbacks_.front().ready_ee_cycles),
                         static_cast<unsigned long long>(ee_cycle_count_),
                         pending_sif_command_callbacks_.size());
        }
        delayed_save_card_callbacks_.pop_front();
    }
    while (!pending_sif_dma_batches_.empty() &&
           pending_sif_dma_batches_.front().deadline_ee_cycles <= ee_cycle_count_) {
        const auto id = pending_sif_dma_batches_.front().id;
        pending_sif_dma_batches_.pop_front();
        complete_sif_dma_batch(memory, id);
    }
}

std::optional<std::uint64_t> EeKernelHle::next_alarm_deadline() const noexcept {
    return next_alarm_deadline_;
}

void EeKernelHle::refresh_next_alarm_deadline() noexcept {
    const auto earliest =
        std::ranges::min_element(alarms_, {}, &EeAlarmState::deadline_ee_cycles);
    next_alarm_deadline_ = earliest == alarms_.end()
                               ? std::nullopt
                               : std::optional<std::uint64_t>{earliest->deadline_ee_cycles};
}

std::span<const EeSifRegisterState> EeKernelHle::sif_registers() const noexcept {
    return sif_registers_;
}

std::span<const EeSifDmaTransferState> EeKernelHle::sif_dma_transfers() const noexcept {
    return sif_dma_transfers_;
}

const EeRwaActivity& EeKernelHle::rwa_activity() const noexcept { return rwa_activity_; }

const EeGtfsActivity& EeKernelHle::gtfs_activity() const noexcept { return gtfs_activity_; }

std::span<const EeGtfsOpenRecord> EeKernelHle::gtfs_open_records() const noexcept {
    return gtfs_open_records_;
}

void EeKernelHle::set_rwa_execution_observers(RwaCommandPacketObserver packet,
                                              RwaIopUploadObserver upload,
                                              RwaShadowResponseProvider response) noexcept {
    rwa_command_packet_observer_ = std::move(packet);
    rwa_iop_upload_observer_ = std::move(upload);
    rwa_shadow_response_provider_ = std::move(response);
}

void EeKernelHle::set_rwa_initialize_observer(RwaInitializeObserver observer) noexcept {
    rwa_initialize_observer_ = std::move(observer);
}

bool EeKernelHle::apply_deferred_rwa_shadow_response(const EeRwaShadowResponse& response,
                                                     EeMemory& memory) {
    constexpr std::size_t kSnapshotBytes = 0x680U;
    if (rwa_activity_.voice_snapshots_deferred_pending == 0U || response.opcode != 0x14U ||
        response.extra_data.size() != kSnapshotBytes ||
        !memory.contains(response.extra_destination, kSnapshotBytes)) {
        return false;
    }
    for (std::size_t byte = 0; byte < kSnapshotBytes; ++byte) {
        memory.write_u8(response.extra_destination + static_cast<GuestAddress>(byte),
                        static_cast<std::uint8_t>(response.extra_data[byte]));
    }
    --rwa_activity_.voice_snapshots_deferred_pending;
    ++rwa_activity_.voice_snapshots_deferred_applied;
    ++rwa_activity_.voice_snapshots_returned;
    rwa_activity_.voice_snapshot_return_bytes += kSnapshotBytes;
    rwa_activity_.last_voice_snapshot_destination = response.extra_destination;
    // The retail command callback (0x2b3b50) acknowledges the 0x14 commit.
    pending_sif_command_callbacks_.push_back({0x002b3b50U, 0U, 0x14U, 0U, 0U, 0U, 0U, 0U});
    return true;
}

void EeKernelHle::set_rwa_stream_journal(RwaStreamJournalObserver observer) noexcept {
    rwa_stream_journal_ = std::move(observer);
}

void EeKernelHle::set_spu_transfer_materialized(SpuTransferMaterializedObserver observer) noexcept {
    spu_transfer_materialized_observer_ = std::move(observer);
}

void EeKernelHle::set_rwa_voice_play(RwaVoicePlayObserver observer) noexcept {
    rwa_voice_play_observer_ = std::move(observer);
}

void EeKernelHle::set_rwa_voice_status(RwaVoiceStatusObserver observer) noexcept {
    rwa_voice_status_observer_ = std::move(observer);
}

void EeKernelHle::set_rwa_voice_volume_status(RwaVoiceVolumeStatusObserver observer) noexcept {
    rwa_voice_volume_status_observer_ = std::move(observer);
}

void EeKernelHle::advance_rwa_voice_volume_ramps() {
    if (!rwa_voice_play_observer_ || !rwa_voice_volume_status_observer_) {
        return;
    }
    for (std::uint32_t slot = 0U; slot != mixer_slot_volume_targets_.size(); ++slot) {
        auto& target = mixer_slot_volume_targets_[slot];
        if (!target.active) {
            continue;
        }
        const auto core = static_cast<std::uint8_t>(slot / 24U);
        const auto voice = static_cast<std::uint8_t>(slot % 24U);
        const EeRwaVoiceVolumeStatus current = rwa_voice_volume_status_observer_(core, voice);
        const RwaVolumeWrites left_writes = rwa_volume_update_registers(
            current.left, target.left.desired_target, target.left.desired_negative,
            target.force_zero, target.left.cached_magnitude, target.left.phase_negative,
            target.left.in_progress);
        for (std::size_t index = 0; index < left_writes.count; ++index) {
            rwa_voice_play_observer_({EeRwaVoiceAction::SetVolumeLeft, core, voice, 0U, 0U, 0U,
                                      left_writes.values[index], 0U, 0U, 0U});
        }
        const RwaVolumeWrites right_writes = rwa_volume_update_registers(
            current.right, target.right.desired_target, target.right.desired_negative,
            target.force_zero, target.right.cached_magnitude, target.right.phase_negative,
            target.right.in_progress);
        for (std::size_t index = 0; index < right_writes.count; ++index) {
            rwa_voice_play_observer_({EeRwaVoiceAction::SetVolumeRight, core, voice, 0U, 0U, 0U, 0U,
                                      right_writes.values[index], 0U, 0U});
        }
    }
}

void EeKernelHle::set_rwa_voice_play_probe(std::function<void(std::uint32_t)> probe) noexcept {
    rwa_voice_play_probe_ = std::move(probe);
}

std::span<const EeThreadState> EeKernelHle::threads() const noexcept { return threads_; }

std::vector<EeThreadRuntimeSnapshot>
EeKernelHle::thread_runtime_snapshots(const R5900State& live_cpu) const {
    std::vector<EeThreadRuntimeSnapshot> snapshots;
    snapshots.reserve(threads_.size());
    for (std::size_t index = 0; index < threads_.size(); ++index) {
        const auto& thread = threads_[index];
        const auto& runtime = thread_runtime_[index];
        const R5900State* context = runtime.cpu ? &*runtime.cpu : nullptr;
        // The running thread's registers live in the CPU, or in the saved
        // interrupt context while a guest callback is executing.
        if (thread.id == current_thread_id_ && thread.status == EeThreadStatus::Running) {
            context = interrupted_cpu_state_ ? &*interrupted_cpu_state_ : &live_cpu;
        }
        std::uint32_t wait_type = 0U;
        if (runtime.wait_reason == ThreadWaitReason::Sleep) {
            wait_type = 1U;
        } else if (runtime.wait_reason == ThreadWaitReason::Semaphore) {
            wait_type = 2U;
        }
        snapshots.push_back({
            .id = thread.id,
            .program_counter = context != nullptr ? context->pc() : 0U,
            .stack_pointer = context != nullptr ? context->read_gpr_u32(29U) : 0U,
            .return_address = context != nullptr ? context->read_gpr_u32(31U) : 0U,
            .wait_type = wait_type,
            .wait_id = runtime.wait_reason == ThreadWaitReason::Semaphore ? runtime.wait_id : 0,
            .wakeup_count = runtime.wakeup_count,
            .context_saved = context != nullptr,
        });
    }
    return snapshots;
}

std::int32_t EeKernelHle::current_thread_id() const noexcept { return current_thread_id_; }

std::uint32_t EeKernelHle::osd_config_param() const noexcept { return osd_config_param_; }

bool EeKernelHle::sif0_dma_chain_enabled() const noexcept { return sif0_dma_chain_enabled_; }

std::uint32_t EeKernelHle::enabled_dmac_channels() const noexcept {
    return enabled_dmac_channels_;
}

const std::array<std::uint64_t, 10>& EeKernelHle::dmac_callback_counts() const noexcept {
    return dmac_callback_counts_;
}

std::size_t EeKernelHle::pending_dmac_callback_count() const noexcept {
    return pending_dmac_channels_.size();
}

std::uint32_t EeKernelHle::pending_dmac_callback_mask() const noexcept {
    return pending_dmac_channel_mask_;
}

bool EeKernelHle::pending_dmac_callback_can_dispatch(const R5900State& cpu) const noexcept {
    if (interrupted_cpu_state_ || (pending_dmac_channel_mask_ & enabled_dmac_channels_) == 0U) {
        return false;
    }
    const auto status = cpu.read_cop0(12U);
    if ((status & 0x10000U) == 0U || (status & 7U) != 1U) {
        return false;
    }
    return std::ranges::any_of(dmac_handlers_, [this](const EeDmacHandlerState& handler) {
        return (((pending_dmac_channel_mask_ & enabled_dmac_channels_) >> handler.channel) & 1U) != 0U;
    });
}

bool EeKernelHle::sif_dma_completion_pending() const noexcept {
    return !pending_sif_dma_batches_.empty() || !delayed_save_card_callbacks_.empty();
}

std::optional<std::uint64_t> EeKernelHle::next_sif_dma_completion_deadline() const noexcept {
    std::optional<std::uint64_t> deadline;
    if (!pending_sif_dma_batches_.empty()) {
        deadline = pending_sif_dma_batches_.front().deadline_ee_cycles;
    }
    if (!delayed_save_card_callbacks_.empty()) {
        const auto ready = delayed_save_card_callbacks_.front().ready_ee_cycles;
        deadline = deadline ? std::min(*deadline, ready) : ready;
    }
    return deadline;
}

bool EeKernelHle::has_competing_ready_thread() const noexcept {
    if (interrupted_cpu_state_ || !highest_ready_priority_) {
        return false;
    }
    const auto running = thread_index(current_thread_id_);
    return !running || threads_[*running].status != EeThreadStatus::Running ||
           static_cast<std::int32_t>(*highest_ready_priority_) <=
               threads_[*running].current_priority;
}

std::optional<std::uint64_t> EeKernelHle::next_thread_time_slice_deadline() const noexcept {
    if (interrupted_cpu_state_ || threads_.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> candidate;
    if (highest_ready_priority_) {
        for (unsigned long long priority = *highest_ready_priority_;
             priority < ready_thread_queues_.size() && !candidate; ++priority) {
            for (const int id : ready_thread_queues_[priority]) {
                const auto index = thread_index(id);
                if (index && threads_[*index].status == EeThreadStatus::Ready &&
                    threads_[*index].current_priority == static_cast<std::int32_t>(priority)) {
                    candidate = index;
                    break;
                }
            }
        }
    }
    std::optional<std::size_t> running;
    if (current_thread_id_ != 0) {
        running = thread_index(current_thread_id_);
        if (running && threads_[*running].status != EeThreadStatus::Running) {
            running.reset();
        }
    }
    if (!candidate) {
        return running ? std::nullopt : std::optional<std::uint64_t>{0U};
    }
    if (!running || threads_[*candidate].current_priority < threads_[*running].current_priority) {
        return 0U;
    }
    if (threads_[*candidate].current_priority > threads_[*running].current_priority) {
        return std::nullopt;
    }
    return last_thread_switch_ee_cycles_ + kThreadTimeSliceEeCycles;
}

bool EeKernelHle::pending_intc_callback_can_dispatch(const R5900State& cpu,
                                                     const EeMemory& memory) const noexcept {
    if (interrupted_cpu_state_) {
        return false;
    }
    const auto status = cpu.read_cop0(12U);
    if ((status & 0x10000U) == 0U || (status & 7U) != 1U) {
        return false;
    }
    const auto pending = memory.pending_intc_causes();
    if (pending == 0U) {
        return false;
    }
    return std::ranges::any_of(intc_handlers_, [pending](const EeIntcHandlerState& handler) {
        return handler.cause < 32U && ((pending >> handler.cause) & 1U) != 0U;
    });
}

std::size_t EeKernelHle::pending_sif_command_callback_count() const noexcept {
    return pending_sif_command_callbacks_.size() +
           static_cast<std::size_t>(suspended_sif_command_callback_.has_value());
}

bool EeKernelHle::sif_command_callback_poll_can_progress() const noexcept {
    if (suspended_sif_command_callback_ && suspended_sif_command_callback_->ready) {
        return !interrupted_cpu_state_;
    }
    if (pending_sif_command_callbacks_.empty()) {
        return false;
    }
    if (pending_sif_command_callbacks_.front().dispatch_delay != 0U) {
        return true;
    }
    return !interrupted_cpu_state_ && !suspended_sif_command_callback_;
}

void EeKernelHle::note_guest_callback_activation() noexcept {
    if (guest_callback_activation_generation_ != std::numeric_limits<std::uint64_t>::max()) {
        ++guest_callback_activation_generation_;
    }
}

void EeKernelHle::begin_new_guest_callback_identity() noexcept {
    note_guest_callback_activation();
    active_guest_callback_identity_ = next_guest_callback_identity_;
    ++next_guest_callback_identity_;
    if (next_guest_callback_identity_ == 0U) {
        next_guest_callback_identity_ = 1U;
    }
}

bool EeKernelHle::dispatch_pending_alarm_callback(R5900State& cpu) {
    if (interrupted_cpu_state_) {
        return false;
    }
    if (alarms_.empty()) {
        return false;
    }
    if (!next_alarm_deadline_ || *next_alarm_deadline_ > ee_cycle_count_) {
        return false;
    }
    const auto earliest = std::ranges::min_element(alarms_, {}, &EeAlarmState::deadline_ee_cycles);
    if (earliest == alarms_.end() || ee_cycle_count_ < earliest->deadline_ee_cycles) {
        return false;
    }
    const EeAlarmState alarm = *earliest;
    alarms_.erase(earliest);
    refresh_next_alarm_deadline();

    interrupted_cpu_state_ = cpu;
    active_guest_callback_kind_ = GuestCallbackKind::Alarm;
    begin_new_guest_callback_identity();
    cpu.write_cop0(12U, cpu.read_cop0(12U) & ~0x10000U);
    cpu.set_pc(alarm.handler);
    cpu.write_gpr_u64(4U, static_cast<std::uint32_t>(alarm.id));
    cpu.write_gpr_u64(5U, alarm.requested_hsync_ticks);
    cpu.write_gpr_u64(6U, alarm.common);
    cpu.write_gpr_u64(28U, alarm.global_pointer);
    cpu.write_gpr_u64(31U, kGuestCallbackReturnAddress);
    return true;
}

bool EeKernelHle::dispatch_pending_dmac_callback(R5900State& cpu, EeMemory& memory) {
    if (interrupted_cpu_state_) {
        return false;
    }
    while (const auto channel = memory.take_completed_dmac_channel()) {
        if (*channel >= dmac_callback_counts_.size()) {
            continue;
        }
        const auto bit = 1U << *channel;
        if ((pending_dmac_channel_mask_ & bit) == 0U) {
            pending_dmac_channels_.push_back(*channel);
            pending_dmac_channel_mask_ |= bit;
        }
    }
    const auto status = cpu.read_cop0(12U);
    if ((status & 0x10000U) == 0U || (status & 7U) != 1U) {
        return false;
    }
    for (auto pending = pending_dmac_channels_.begin(); pending != pending_dmac_channels_.end();
         ++pending) {
        const auto channel = *pending;
        if (((enabled_dmac_channels_ >> channel) & 1U) == 0U) {
            continue;
        }
        const auto handler = std::ranges::find_if(dmac_handlers_, [channel](const EeDmacHandlerState& candidate) {
            return candidate.channel == channel;
        });
        if (handler == dmac_handlers_.end()) {
            continue;
        }
        pending_dmac_channels_.erase(pending);
        pending_dmac_channel_mask_ &= ~(1U << channel);
        memory.set_dmac_status(memory.dmac_status() & ~(1U << channel));

        interrupted_cpu_state_ = cpu;
        active_guest_callback_kind_ = GuestCallbackKind::Dmac;
        begin_new_guest_callback_identity();
        cpu.write_cop0(12U, cpu.read_cop0(12U) & ~0x10000U);
        cpu.set_pc(handler->handler);
        cpu.write_gpr_u64(4U, channel);
        cpu.write_gpr_u64(5U, handler->argument);
        cpu.write_gpr_u64(6U, interrupted_cpu_state_->pc());
        cpu.write_gpr_u64(28U, handler->global_pointer);
        cpu.write_gpr_u64(31U, kGuestCallbackReturnAddress);
        ++dmac_callback_counts_[channel];
        return true;
    }
    return false;
}

bool EeKernelHle::dispatch_pending_intc_callback(R5900State& cpu, EeMemory& memory) {
    if (interrupted_cpu_state_) {
        return false;
    }
    const auto status = cpu.read_cop0(12U);
    if ((status & 0x10000U) == 0U || (status & 7U) != 1U) {
        return false;
    }
    const auto pending = memory.pending_intc_causes();
    if (pending == 0U) {
        return false;
    }
    for (std::uint32_t cause = 0U; cause != 32U; ++cause) {
        const auto bit = 1U << cause;
        if ((pending & bit) == 0U) {
            continue;
        }
        std::vector<EeIntcHandlerState> handlers;
        std::ranges::copy_if(intc_handlers_, std::back_inserter(handlers),
                             [cause](const EeIntcHandlerState& handler) { return cause == handler.cause; });
        if (handlers.empty()) {
            continue;
        }
        if (cause == 2U) {
            notify_vsync_flag(memory);
        }
        memory.set_intc_status(memory.intc_status() & ~bit);
        interrupted_cpu_state_ = cpu;
        active_guest_callback_kind_ = GuestCallbackKind::Intc;
        begin_new_guest_callback_identity();
        cpu.write_cop0(12U, cpu.read_cop0(12U) & ~0x10000U);
        active_intc_dispatch_ = ActiveIntcDispatch{
            .cause = cause,
            .interrupted_pc = cpu.pc(),
            .handlers = std::move(handlers),
            .next_handler = 0U,
        };
        enter_next_intc_handler(cpu);
        return true;
    }
    return false;
}

bool EeKernelHle::dispatch_pending_sif_command_callback(R5900State& cpu) {
    if (suspended_sif_command_callback_ && suspended_sif_command_callback_->ready) {
        if (interrupted_cpu_state_) {
            return false;
        }
        R5900State continuation = suspended_sif_command_callback_->continuation_cpu;
        copy_cop0_state(cpu, continuation);
        interrupted_cpu_state_ = cpu;
        active_guest_callback_kind_ = GuestCallbackKind::SifCommand;
        active_guest_callback_identity_ = suspended_sif_command_callback_->callback_identity;
        note_guest_callback_activation();
        cpu = continuation;
        suspended_sif_command_callback_.reset();
        return true;
    }
    if (pending_sif_command_callbacks_.empty()) {
        return false;
    }
    auto& next = pending_sif_command_callbacks_.front();
    if (next.dispatch_delay != 0U) {
        --next.dispatch_delay;
        return false;
    }
    if (interrupted_cpu_state_ || suspended_sif_command_callback_) {
        return false;
    }
    if (save_card_trace_enabled() && next.handler == 0x248518U) {
        std::fprintf(stderr, "[save-card] dispatch now=%llu\n",
                     static_cast<unsigned long long>(ee_cycle_count_));
    }
    const PendingSifCommandCallback response = pending_sif_command_callbacks_.front();
    pending_sif_command_callbacks_.erase(pending_sif_command_callbacks_.begin());
    if (rwa_stream_journal_ &&
        (response.command == 0x33U || response.command == 0x47U || response.command == 0x31U)) {
        // The retail stream callback answered a request that was dropped.
        rwa_stream_journal_({
            .sequence = rwa_activity_.command_packets,
            .slot = response.a3,
            .state = response.command,
            .file_offset = response.a2,
            .phase = EeRwaStreamJournalEvent::Phase::Completed,
            .detail = 97U,
        });
    }
    interrupted_cpu_state_ = cpu;
    active_guest_callback_kind_ = GuestCallbackKind::SifCommand;
    begin_new_guest_callback_identity();
    cpu.set_pc(response.handler);
    cpu.write_gpr_u64(4U, response.context);
    cpu.write_gpr_u64(5U, response.command);
    cpu.write_gpr_u64(6U, response.a2);
    cpu.write_gpr_u64(7U, response.a3);
    cpu.write_gpr_u64(8U, response.t0);
    cpu.write_gpr_u64(28U, response.global_pointer != 0U ? response.global_pointer
                                                         : startup_.global_pointer);
    cpu.write_gpr_u64(31U, kGuestCallbackReturnAddress);
    return true;
}

void EeKernelHle::enter_next_intc_handler(R5900State& cpu) {
    auto& dispatch = *active_intc_dispatch_;
    const auto& handler = dispatch.handlers[dispatch.next_handler++];
    cpu.set_pc(handler.handler);
    cpu.write_gpr_u64(4U, dispatch.cause);
    cpu.write_gpr_u64(5U, handler.argument);
    cpu.write_gpr_u64(6U, dispatch.interrupted_pc);
    cpu.write_gpr_u64(28U, handler.global_pointer);
    cpu.write_gpr_u64(31U, kGuestCallbackReturnAddress);
}

bool EeKernelHle::finish_guest_callback(R5900State& cpu) noexcept {
    if (!interrupted_cpu_state_ || cpu.pc() != kGuestCallbackReturnAddress) {
        return false;
    }
    if (active_intc_dispatch_ &&
        active_intc_dispatch_->next_handler < active_intc_dispatch_->handlers.size()) {
        enter_next_intc_handler(cpu);
        return true;
    }
    cpu = *interrupted_cpu_state_;
    active_intc_dispatch_.reset();
    interrupted_cpu_state_.reset();
    active_guest_callback_kind_ = GuestCallbackKind::None;
    active_guest_callback_identity_ = 0U;
    return true;
}

bool EeKernelHle::guest_callback_active() const noexcept {
    return interrupted_cpu_state_.has_value();
}

EeKernelHle::GuestCallbackKind EeKernelHle::guest_callback_kind() const noexcept {
    return interrupted_cpu_state_ ? active_guest_callback_kind_ : GuestCallbackKind::None;
}

std::uint64_t EeKernelHle::guest_callback_identity() const noexcept {
    return interrupted_cpu_state_ ? active_guest_callback_identity_ : 0U;
}

std::uint64_t EeKernelHle::guest_callback_activation_generation() const noexcept {
    return guest_callback_activation_generation_;
}

void EeKernelHle::copy_cop0_state(const R5900State& source, R5900State& destination) {
    for (std::size_t index = 0U; index != R5900State::kCop0RegisterCount; ++index) {
        destination.write_cop0(index, source.read_cop0(index));
    }
}

void EeKernelHle::enqueue_ready_thread(const std::size_t index, const bool front) {
    auto& thread = threads_[index];
    if (static_cast<std::uint32_t>(thread.current_priority) >= ready_thread_queues_.size()) {
        return;
    }
    thread.status = EeThreadStatus::Ready;
    const auto priority = static_cast<std::size_t>(thread.current_priority);
    auto& queue = ready_thread_queues_[priority];
    if (front) {
        queue.push_front(thread.id);
    } else {
        queue.push_back(thread.id);
    }
    if (!highest_ready_priority_ || priority < *highest_ready_priority_) {
        highest_ready_priority_ = priority;
    }
}

// RECOVERY: only the retvalptr.N numbering differs (LTO decl uid counter shifts with the
// functions still missing from the full file: path move ctor, push_back, shared_count).
void EeKernelHle::remove_ready_thread(const std::size_t index) noexcept {
    const auto id = threads_[index].id;
    for (std::size_t priority = 0U; priority != ready_thread_queues_.size(); ++priority) {
        auto& queue = ready_thread_queues_[priority];
        const auto entry = std::ranges::find(queue, id);
        if (entry != queue.end()) {
            queue.erase(entry);
            if (queue.empty() && highest_ready_priority_ == priority) {
                refresh_highest_ready_priority();
            }
            return;
        }
    }
}

void EeKernelHle::refresh_highest_ready_priority() noexcept {
    highest_ready_priority_.reset();
    for (std::size_t priority = 0U; priority != ready_thread_queues_.size(); ++priority) {
        if (!ready_thread_queues_[priority].empty()) {
            highest_ready_priority_ = priority;
            return;
        }
    }
}

std::optional<std::size_t> EeKernelHle::take_next_ready_thread() noexcept {
    while (highest_ready_priority_) {
        const auto priority = *highest_ready_priority_;
        auto& queue = ready_thread_queues_[priority];
        while (!queue.empty()) {
            const auto id = queue.front();
            queue.pop_front();
            const auto index = thread_index(id);
            if (index && threads_[*index].status == EeThreadStatus::Ready &&
                threads_[*index].current_priority == static_cast<std::int32_t>(priority)) {
                if (queue.empty()) {
                    refresh_highest_ready_priority();
                }
                return index;
            }
        }
        refresh_highest_ready_priority();
    }
    return std::nullopt;
}

// RECOVERY: only the retvalptr.N numbering differs (LTO decl uid counter shifts with the
// functions still missing from the full file: path move ctor, push_back, shared_count).
void EeKernelHle::remove_thread_wait(const std::size_t index) noexcept {
    auto& runtime = thread_runtime_[index];
    if (runtime.wait_reason == ThreadWaitReason::Semaphore) {
        const auto semaphore = semaphore_index(runtime.wait_id);
        if (semaphore && *semaphore < semaphore_waiters_.size()) {
            auto& waiters = semaphore_waiters_[*semaphore];
            const auto waiter = std::ranges::find(waiters, threads_[index].id);
            if (waiter != waiters.end()) {
                waiters.erase(waiter);
                if (semaphores_[*semaphore].wait_threads > 0) {
                    --semaphores_[*semaphore].wait_threads;
                }
            }
        }
    }
    runtime.wait_reason = ThreadWaitReason::None;
    runtime.wait_id = 0;
}

// RECOVERY: only the retvalptr.N numbering differs (LTO decl uid counter shifts with the
// functions still missing from the full file: path move ctor, push_back, shared_count).
void EeKernelHle::make_thread_dormant(const std::size_t index) noexcept {
    remove_ready_thread(index);
    remove_thread_wait(index);
    auto& thread = threads_[index];
    auto& runtime = thread_runtime_[index];
    thread.status = EeThreadStatus::Dormant;
    thread.current_priority = thread.initial_priority;
    thread.argument = 0U;
    runtime.cpu.reset();
    runtime.wakeup_count = 0U;
}

EeThreadDispatchResult EeKernelHle::dispatch_guest_threads(R5900State& cpu) {
    if (interrupted_cpu_state_ || threads_.empty()) {
        return EeThreadDispatchResult::Continued;
    }
    std::optional<std::size_t> candidate;
    while (highest_ready_priority_) {
        const auto priority = *highest_ready_priority_;
        auto& queue = ready_thread_queues_[priority];
        while (!queue.empty()) {
            const auto index = thread_index(queue.front());
            if (index && threads_[*index].status == EeThreadStatus::Ready &&
                threads_[*index].current_priority == static_cast<std::int32_t>(priority)) {
                candidate = index;
                break;
            }
            queue.pop_front();
        }
        if (candidate) {
            break;
        }
        refresh_highest_ready_priority();
    }
    std::optional<std::size_t> running;
    if (current_thread_id_ != 0) {
        running = thread_index(current_thread_id_);
        if (!running || threads_[*running].status != EeThreadStatus::Running) {
            current_thread_id_ = 0;
            running.reset();
        }
    }
    if (!candidate) {
        return running ? EeThreadDispatchResult::Continued : EeThreadDispatchResult::NoRunnable;
    }
    const bool time_slice_expired =
        running && ee_cycle_count_ >= last_thread_switch_ee_cycles_ + kThreadTimeSliceEeCycles;
    if (running) {
        if (threads_[*candidate].current_priority > threads_[*running].current_priority ||
            (threads_[*candidate].current_priority == threads_[*running].current_priority &&
             !time_slice_expired)) {
            return EeThreadDispatchResult::Continued;
        }
    }
    if (running) {
        thread_runtime_[*running].cpu = cpu;
        enqueue_ready_thread(*running, true);
    }
    last_thread_switch_ee_cycles_ = ee_cycle_count_;
    const auto next = take_next_ready_thread();
    if (!next || !thread_runtime_[*next].cpu) {
        return EeThreadDispatchResult::NoRunnable;
    }
    R5900State next_cpu = *thread_runtime_[*next].cpu;
    copy_cop0_state(cpu, next_cpu);
    cpu = next_cpu;
    threads_[*next].status = EeThreadStatus::Running;
    current_thread_id_ = threads_[*next].id;
    return EeThreadDispatchResult::Switched;
}

bool EeKernelHle::finish_guest_thread(R5900State& cpu) noexcept {
    if (interrupted_cpu_state_ || cpu.pc() != 0U || current_thread_id_ == 0) {
        return false;
    }
    const auto current = thread_index(current_thread_id_);
    if (!current || threads_[*current].status != EeThreadStatus::Running) {
        return false;
    }
    make_thread_dormant(*current);
    current_thread_id_ = 0;
    return true;
}

EeKernelCallResult EeKernelHle::set_gs_crt(R5900State& cpu) {
    gs_crt_ = EeGsCrtState{
        .configured = true,
        .interlace = static_cast<std::int16_t>(cpu.read_gpr_u32(4U)),
        .mode = static_cast<std::int16_t>(cpu.read_gpr_u32(5U)),
        .field_mode = static_cast<std::int16_t>(cpu.read_gpr_u32(6U)),
    };
    cpu.write_cop0(12U, cpu.read_cop0(12U) | 1U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 2U, "SetGsCrt"};
}

EeKernelCallResult EeKernelHle::set_vsync_flag(R5900State& cpu, EeMemory& memory) {
    const GuestAddress flag_address = cpu.read_gpr_u32(4U);
    const GuestAddress csr_address = cpu.read_gpr_u32(5U);
    if (!is_accessible_guest_range<4U>(memory, flag_address) ||
        !is_accessible_guest_range<8U>(memory, csr_address)) {
        return {EeKernelCallStatus::InvalidArguments, 115U, "SetVSyncFlag"};
    }
    vsync_flag_ = EeVsyncFlagState{
        .registered = true,
        .flag_address = flag_address,
        .csr_address = csr_address,
        .registration_count = vsync_flag_.registration_count + 1U,
        .notification_count = vsync_flag_.notification_count,
    };
    // Seed the CSR word with the live GS CSR so a poll before the first VBLANK
    // sees the current field/vblank state.
    memory.write_u64_le(csr_address, memory.read_u64_le(0x12001000U));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 115U, "SetVSyncFlag"};
}

void EeKernelHle::notify_vsync_flag(EeMemory& memory) {
    if (!vsync_flag_.registered ||
        !is_accessible_guest_range<4U>(memory, vsync_flag_.flag_address) ||
        !is_accessible_guest_range<8U>(memory, vsync_flag_.csr_address)) {
        return;
    }
    memory.write_u64_le(vsync_flag_.csr_address, memory.read_u64_le(0x12001000U));
    memory.write_u32_le(vsync_flag_.flag_address, 1U);
    ++vsync_flag_.notification_count;
}

EeKernelCallResult EeKernelHle::setup_thread(R5900State& cpu, const EeMemory& memory) {
    const GuestAddress global_pointer = cpu.read_gpr_u32(4U);
    const GuestAddress stack_base = cpu.read_gpr_u32(5U);
    const std::uint32_t stack_size = cpu.read_gpr_u32(6U);
    const GuestAddress argument_block = cpu.read_gpr_u32(7U);
    const GuestAddress root_function = cpu.read_gpr_u32(8U);
    // A stack base of -1 asks for the top of RAM.
    GuestAddress stack_top{};
    if (stack_base == 0xffffffffU) {
        if (memory.size() > 0xffffffffULL) {
            return {EeKernelCallStatus::InvalidArguments, 60U, "SetupThread"};
        }
        stack_top = static_cast<GuestAddress>(memory.size());
    } else if (!checked_end(stack_base, stack_size, stack_top)) {
        return {EeKernelCallStatus::InvalidArguments, 60U, "SetupThread"};
    }
    if (static_cast<std::size_t>(stack_top) > memory.size() || stack_top < 0x2a0U) {
        return {EeKernelCallStatus::InvalidArguments, 60U, "SetupThread"};
    }
    const auto initial_stack_pointer = stack_top - 0x2a0U;
    if ((stack_top & 15U) != 0U) {
        return {EeKernelCallStatus::InvalidArguments, 60U, "SetupThread"};
    }

    startup_.heap_initialized = false;
    startup_.heap_start = 0U;
    startup_.heap_end = 0U;
    startup_.main_thread_initialized = true;
    startup_.global_pointer = global_pointer;
    startup_.stack_base = stack_base;
    startup_.stack_size = stack_size;
    startup_.initial_stack_pointer = initial_stack_pointer;
    startup_.argument_block = argument_block;
    startup_.root_function = root_function;

    threads_.clear();
    thread_runtime_.clear();
    ready_thread_queues_ = {};
    highest_ready_priority_.reset();
    for (auto& waiters : semaphore_waiters_) {
        waiters.clear();
    }
    for (auto& semaphore : semaphores_) {
        semaphore.wait_threads = 0;
    }
    threads_.push_back({
        .id = 1,
        .status = EeThreadStatus::Running,
        .entry_point = root_function,
        .stack = stack_base,
        .stack_size = stack_size,
        .global_pointer = global_pointer,
        .initial_priority = 0,
        .current_priority = 0,
        .attributes = 0U,
        .option = 0U,
        .argument = 0U,
    });
    thread_runtime_.push_back({});
    current_thread_id_ = 1;
    cpu.write_gpr_u64(2U, initial_stack_pointer);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 60U, "SetupThread"};
}

EeKernelCallResult EeKernelHle::setup_heap(R5900State& cpu, const EeMemory& memory) {
    const GuestAddress heap_start = cpu.read_gpr_u32(4U);
    const std::uint32_t heap_size = cpu.read_gpr_u32(5U);
    if (!startup_.main_thread_initialized ||
        static_cast<std::size_t>(heap_start) > memory.size()) {
        return {EeKernelCallStatus::InvalidArguments, 61U, "SetupHeap"};
    }
    GuestAddress heap_end{};
    // A heap size of -1 extends to the main thread's initial stack pointer.
    if (heap_size == 0xffffffffU) {
        heap_end = startup_.initial_stack_pointer;
    } else if (!checked_end(heap_start, heap_size, heap_end)) {
        return {EeKernelCallStatus::InvalidArguments, 61U, "SetupHeap"};
    }
    if (heap_end < heap_start || static_cast<std::size_t>(heap_end) > memory.size()) {
        return {EeKernelCallStatus::InvalidArguments, 61U, "SetupHeap"};
    }
    startup_.heap_initialized = true;
    startup_.heap_start = heap_start;
    startup_.heap_end = heap_end;
    cpu.write_gpr_u64(2U, heap_end);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 61U, "SetupHeap"};
}

EeKernelCallResult EeKernelHle::end_of_heap(R5900State& cpu) {
    if (!startup_.heap_initialized) {
        return {EeKernelCallStatus::InvalidArguments, 62U, "EndOfHeap"};
    }
    cpu.write_gpr_u64(2U, startup_.heap_end);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 62U, "EndOfHeap"};
}

std::optional<std::int32_t> EeKernelHle::lowest_free_semaphore_id() const noexcept {
    for (std::int32_t candidate = 0; candidate != 256; ++candidate) {
        const bool used = std::ranges::any_of(semaphores_, [candidate](const EeSemaphoreState& semaphore) {
            return semaphore.id == candidate;
        });
        if (!used) {
            return candidate;
        }
    }
    return std::nullopt;
}

EeKernelCallResult EeKernelHle::create_semaphore(R5900State& cpu, const EeMemory& memory) {
    const GuestAddress parameter = cpu.read_gpr_u32(4U);
    const auto id = lowest_free_semaphore_id();
    if ((parameter & 3U) != 0U || !memory.contains(parameter, 24U) || !id) {
        return {EeKernelCallStatus::InvalidArguments, 64U, "CreateSema"};
    }
    // ee_sema_t: init_count, max_count, current count, wait threads, attr, option.
    const std::int32_t max_count = read_i32(memory, parameter + 4U);
    const std::int32_t initial_count = read_i32(memory, parameter + 8U);
    semaphores_.push_back({
        .id = *id,
        .count = initial_count,
        .max_count = max_count,
        .wait_threads = 0,
        .attributes = memory.read_u32_le(parameter + 16U),
        .option = memory.read_u32_le(parameter + 20U),
    });
    semaphore_waiters_.emplace_back();
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(*id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 64U, "CreateSema"};
}

EeKernelCallResult EeKernelHle::wait_semaphore(R5900State& cpu) {
    const std::int32_t semaphore_id = signed_argument(cpu, 4U);
    const auto index = semaphore_index(semaphore_id);
    if (!index) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 68U, "WaitSema"};
    }
    auto& semaphore = semaphores_[*index];
    if (semaphore.count <= 0) {
        if (active_guest_callback_kind_ == GuestCallbackKind::SifCommand && interrupted_cpu_state_) {
            // A SIF command callback blocks by parking its continuation; the
            // interrupted thread resumes meanwhile.
            if (suspended_sif_command_callback_ || *index >= semaphore_waiters_.size()) {
                return {EeKernelCallStatus::Blocked, 68U, "WaitSema"};
            }
            cpu.advance_pc();
            R5900State continuation = cpu;
            R5900State interrupted = *interrupted_cpu_state_;
            copy_cop0_state(cpu, interrupted);
            suspended_sif_command_callback_ = SuspendedSifCommandCallback{
                .continuation_cpu = continuation,
                .semaphore_id = semaphore_id,
                .callback_identity = active_guest_callback_identity_,
                .ready = false,
            };
            semaphore_waiters_[*index].push_back(kSifCommandCallbackWaiterId);
            ++semaphore.wait_threads;
            cpu = interrupted;
            interrupted_cpu_state_.reset();
            active_guest_callback_kind_ = GuestCallbackKind::None;
            active_guest_callback_identity_ = 0U;
            return {EeKernelCallStatus::Handled, 68U, "WaitSema"};
        }
        const auto current = thread_index(current_thread_id_);
        if (!current || threads_[*current].status != EeThreadStatus::Running ||
            *index >= semaphore_waiters_.size()) {
            return {EeKernelCallStatus::Blocked, 68U, "WaitSema"};
        }
        cpu.advance_pc();
        auto& runtime = thread_runtime_[*current];
        runtime.cpu = cpu;
        runtime.wait_reason = ThreadWaitReason::Semaphore;
        runtime.wait_id = semaphore_id;
        threads_[*current].status = EeThreadStatus::Waiting;
        semaphore_waiters_[*index].push_back(current_thread_id_);
        ++semaphore.wait_threads;
        current_thread_id_ = 0;
        return {EeKernelCallStatus::Handled, 68U, "WaitSema"};
    }
    --semaphore.count;
    cpu.write_gpr_u64(2U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 68U, "WaitSema"};
}

// RECOVERY: only the operand order of the inlined semaphore_index id compare differs (SSA numbering).
EeKernelCallResult EeKernelHle::poll_semaphore(R5900State& cpu) {
    const std::int32_t semaphore_id = signed_argument(cpu, 4U);
    const auto index = semaphore_index(semaphore_id);
    if (!index || semaphores_[*index].count <= 0) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        --semaphores_[*index].count;
        cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(semaphore_id));
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 69U, "PollSema"};
}

std::optional<std::size_t> EeKernelHle::thread_index(const std::int32_t id) const noexcept {
    const auto found = std::ranges::find_if(threads_, [id](const EeThreadState& thread) { return id == thread.id; });
    if (found == threads_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - threads_.begin());
}

std::optional<std::size_t> EeKernelHle::semaphore_index(const std::int32_t id) const noexcept {
    const auto found = std::ranges::find_if(semaphores_, [id](const EeSemaphoreState& semaphore) { return id == semaphore.id; });
    if (found == semaphores_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - semaphores_.begin());
}

bool EeKernelHle::signal_semaphore(const std::int32_t id) {
    const auto index = semaphore_index(id);
    if (!index) {
        return false;
    }
    auto& semaphore = semaphores_[*index];
    if (*index < semaphore_waiters_.size()) {
        auto& waiters = semaphore_waiters_[*index];
        while (!waiters.empty()) {
            const int waiter_id = waiters.front();
            waiters.pop_front();
            if (waiter_id == kSifCommandCallbackWaiterId) {
                if (!suspended_sif_command_callback_ || suspended_sif_command_callback_->ready ||
                    suspended_sif_command_callback_->semaphore_id != id) {
                    continue;
                }
                suspended_sif_command_callback_->continuation_cpu.write_gpr_u64(2U, 0U);
                suspended_sif_command_callback_->ready = true;
                if (semaphore.wait_threads > 0) {
                    --semaphore.wait_threads;
                }
                return true;
            }
            const auto waiter = thread_index(waiter_id);
            if (!waiter || threads_[*waiter].status != EeThreadStatus::Waiting ||
                thread_runtime_[*waiter].wait_reason != ThreadWaitReason::Semaphore ||
                thread_runtime_[*waiter].wait_id != id) {
                continue;
            }
            auto& runtime = thread_runtime_[*waiter];
            runtime.wait_reason = ThreadWaitReason::None;
            runtime.wait_id = 0;
            if (runtime.cpu) {
                runtime.cpu->write_gpr_u64(2U, 0U);
            }
            if (semaphore.wait_threads > 0) {
                --semaphore.wait_threads;
            }
            enqueue_ready_thread(*waiter);
            return true;
        }
    }
    if (semaphore.count >= semaphore.max_count) {
        return false;
    }
    ++semaphore.count;
    return true;
}

std::optional<std::int32_t> EeKernelHle::lowest_free_thread_id() const noexcept {
    for (std::int32_t candidate = 1; candidate != 256; ++candidate) {
        if (!thread_index(candidate).has_value()) {
            return candidate;
        }
    }
    return std::nullopt;
}

EeKernelCallResult EeKernelHle::create_thread(R5900State& cpu, const EeMemory& memory) {
    const auto parameter = cpu.read_gpr_u32(4U);
    const auto id = lowest_free_thread_id();
    if ((parameter & 3U) != 0U || !memory.contains(parameter, 36U) || !id.has_value()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 32U, "CreateThread"};
    }

    const auto priority = read_i32(memory, parameter + 20U);
    threads_.push_back({
        .id = *id,
        .status = EeThreadStatus::Dormant,
        .entry_point = memory.read_u32_le(parameter + 4U),
        .stack = memory.read_u32_le(parameter + 8U),
        .stack_size = memory.read_u32_le(parameter + 12U),
        .global_pointer = memory.read_u32_le(parameter + 16U),
        .initial_priority = priority,
        .current_priority = priority,
    });
    thread_runtime_.push_back({});
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(*id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 32U, "CreateThread"};
}

// RECOVERY: statements match; the original lays the overflow-failure return out after the
// thread setup code (block order only) and the thread-id compare has swapped operands.
EeKernelCallResult EeKernelHle::start_thread(R5900State& cpu) {
    const auto thread_id = thread_id_argument(cpu);
    const auto index = thread_index(thread_id);
    if (!index || threads_[*index].status != EeThreadStatus::Dormant ||
        threads_[*index].current_priority < 0 || threads_[*index].current_priority > 127) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        auto& thread = threads_[*index];
        thread.argument = cpu.read_gpr_u32(5U);
        GuestAddress stack_top = cpu.read_gpr_u32(29U);
        if (thread.stack != 0U) {
            if (checked_end(thread.stack, thread.stack_size, stack_top)) {
                // fall through to the thread setup below
            } else {
                cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
                cpu.advance_pc();
                return {EeKernelCallStatus::Handled, 34U, "StartThread"};
            }
        }

        R5900State initial{thread.entry_point};
        copy_cop0_state(cpu, initial);
        initial.write_gpr_u64(4U, thread.argument);
        initial.write_gpr_u64(28U, thread.global_pointer != 0U ? thread.global_pointer
                                                               : cpu.read_gpr_u32(28U));
        initial.write_gpr_u64(29U, stack_top & 0xfffffff0U);
        initial.write_gpr_u64(31U, 0U);
        auto& runtime = thread_runtime_[*index];
        runtime = ThreadRuntimeState{};
        runtime.cpu = initial;
        enqueue_ready_thread(*index);
        cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(thread_id));
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 34U, "StartThread"};
}


// RECOVERY: statements match; the original loads current_thread_id_ after `current` is declared and
// the thread-id compare has swapped operands (SSA numbering).
EeKernelCallResult EeKernelHle::exit_thread(R5900State&, const bool delete_record) {
    const auto number = delete_record ? 36U : 35U;
    const auto thread_id = current_thread_id();
    const auto current = thread_index(thread_id);
    if (!current || threads_[*current].status != EeThreadStatus::Running) {
        return {EeKernelCallStatus::InvalidArguments, number,
                delete_record ? "ExitDeleteThread" : "ExitThread"};
    }

    make_thread_dormant(*current);
    current_thread_id_ = 0;
    if (delete_record && thread_id != 1) {
        threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(*current));
        thread_runtime_.erase(thread_runtime_.begin() + static_cast<std::ptrdiff_t>(*current));
    }
    return {EeKernelCallStatus::Handled, number, delete_record ? "ExitDeleteThread" : "ExitThread"};
}


// RECOVERY: statements match; only the operand order of one compare differs (SSA numbering of the
// inlined id conversion).
EeKernelCallResult EeKernelHle::terminate_thread(R5900State& cpu) {
    const auto thread_id = thread_id_argument(cpu);
    const auto index = thread_index(thread_id);
    if (!index || current_thread_id_ == thread_id ||
        threads_[*index].status == EeThreadStatus::Dormant) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        make_thread_dormant(*index);
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 37U, "TerminateThread"};
}

EeKernelCallResult EeKernelHle::delete_thread(R5900State& cpu) {
    const auto thread_id = thread_id_argument(cpu);
    const auto index = thread_index(thread_id);
    if (!index || current_thread_id_ == thread_id ||
        threads_[*index].status != EeThreadStatus::Dormant) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        remove_ready_thread(*index);
        remove_thread_wait(*index);
        threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(*index));
        thread_runtime_.erase(thread_runtime_.begin() + static_cast<std::ptrdiff_t>(*index));
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 33U, "DeleteThread"};
}

EeKernelCallResult EeKernelHle::refer_thread_status(R5900State& cpu, EeMemory& memory) {
    auto id = thread_id_argument(cpu);
    if (id == 0) {
        id = current_thread_id_;
    }
    const auto destination = cpu.read_gpr_u32(5U);
    const auto index = thread_index(id);
    if (!index || (destination & 3U) != 0U || !memory.contains(destination, 48U)) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 48U, "ReferThreadStatus"};
    }
    const auto& thread = threads_[*index];
    memory.write_u32_le(destination, static_cast<std::uint32_t>(thread.status));
    memory.write_u32_le(destination + 4U, thread.entry_point);
    memory.write_u32_le(destination + 8U, thread.stack);
    memory.write_u32_le(destination + 12U, thread.stack_size);
    memory.write_u32_le(destination + 16U, thread.global_pointer);
    memory.write_u32_le(destination + 20U, static_cast<std::uint32_t>(thread.initial_priority));
    memory.write_u32_le(destination + 24U, static_cast<std::uint32_t>(thread.current_priority));
    memory.write_u32_le(destination + 28U, thread.attributes);
    memory.write_u32_le(destination + 32U, thread.option);
    const auto& runtime = thread_runtime_[*index];
    std::uint32_t wait_type = 0U;
    if (runtime.wait_reason == ThreadWaitReason::Sleep) {
        wait_type = 1U;
    } else if (runtime.wait_reason == ThreadWaitReason::Semaphore) {
        wait_type = 2U;
    }
    memory.write_u32_le(destination + 36U, wait_type);
    memory.write_u32_le(destination + 40U,
                        static_cast<std::uint32_t>(
                            runtime.wait_reason == ThreadWaitReason::Semaphore
                                ? runtime.wait_id
                                : 0));
    memory.write_u32_le(destination + 44U, runtime.wakeup_count);
    cpu.write_gpr_u64(2U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 48U, "ReferThreadStatus"};
}

// RECOVERY: statements match; only the operand order of one compare differs (SSA numbering of the
// inlined id conversion).
EeKernelCallResult EeKernelHle::change_thread_priority(R5900State& cpu) {
    auto id = thread_id_argument(cpu);
    if (id == 0) {
        id = current_thread_id_;
    }
    const auto priority = signed_argument(cpu, 5U);
    const auto index = thread_index(id);
    if (!index || cpu.read_gpr_u32(5U) > 127U || threads_[*index].status == EeThreadStatus::Dormant) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        auto& thread = threads_[*index];
        const auto previous_priority = thread.current_priority;
        const bool was_ready = thread.status == EeThreadStatus::Ready;
        if (was_ready && previous_priority != priority) {
            remove_ready_thread(*index);
        }
        thread.current_priority = priority;
        if (was_ready && previous_priority != priority) {
            enqueue_ready_thread(*index);
        }
        cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(previous_priority));
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 41U, "ChangeThreadPriority"};
}

EeKernelCallResult EeKernelHle::sleep_thread(R5900State& cpu) {
    const auto current = thread_index(current_thread_id_);
    if (!current || threads_[*current].status != EeThreadStatus::Running) {
        return {EeKernelCallStatus::InvalidArguments, 50U, "SleepThread"};
    }

    auto& runtime = thread_runtime_[*current];
    cpu.write_gpr_u64(2U, 0U);
    cpu.advance_pc();
    if (runtime.wakeup_count != 0U) {
        // A wakeup arrived first: consume it instead of blocking.
        --runtime.wakeup_count;
    } else {
        runtime.cpu = cpu;
        runtime.wait_reason = ThreadWaitReason::Sleep;
        runtime.wait_id = 0;
        threads_[*current].status = EeThreadStatus::Waiting;
        current_thread_id_ = 0;
    }
    return {EeKernelCallStatus::Handled, 50U, "SleepThread"};
}

// RECOVERY: statements match; only the operand order of one compare differs (SSA numbering of the
// inlined id conversion).
EeKernelCallResult EeKernelHle::wakeup_thread(R5900State& cpu) {
    const auto thread_id = thread_id_argument(cpu);
    const auto index = thread_index(thread_id);
    if (!index || threads_[*index].status == EeThreadStatus::Dormant) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        auto& runtime = thread_runtime_[*index];
        if (threads_[*index].status == EeThreadStatus::Waiting &&
            runtime.wait_reason == ThreadWaitReason::Sleep) {
            runtime.wait_reason = ThreadWaitReason::None;
            runtime.wait_id = 0;
            if (runtime.cpu) {
                runtime.cpu->write_gpr_u64(2U, 0U);
            }
            enqueue_ready_thread(*index);
        } else if (runtime.wakeup_count != 0xffffffffU) {
            ++runtime.wakeup_count;
        }
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 51U, "WakeupThread"};
}

// RECOVERY: statements match; only the operand order of one compare differs (SSA numbering of the
// inlined id conversion).
EeKernelCallResult EeKernelHle::delete_semaphore(R5900State& cpu) {
    const auto id = signed_argument(cpu, 4U);
    const auto index = semaphore_index(id);
    if (!index) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        if (*index < semaphore_waiters_.size()) {
            for (const auto waiter_id : semaphore_waiters_[*index]) {
                if (waiter_id == 0) {
                    // Waiter 0 stands for a SIF command callback parked on this semaphore.
                    if (suspended_sif_command_callback_ && !suspended_sif_command_callback_->ready &&
                        suspended_sif_command_callback_->semaphore_id == id) {
                        suspended_sif_command_callback_->continuation_cpu.write_gpr_sign_extended_32(
                            2U, 0xffffffffU);
                        suspended_sif_command_callback_->ready = true;
                    }
                    continue;
                }
                const auto waiter = thread_index(waiter_id);
                if (!waiter || threads_[*waiter].status != EeThreadStatus::Waiting ||
                    thread_runtime_[*waiter].wait_reason != ThreadWaitReason::Semaphore ||
                    thread_runtime_[*waiter].wait_id != id) {
                    continue;
                }
                auto& runtime = thread_runtime_[*waiter];
                runtime.wait_reason = ThreadWaitReason::None;
                runtime.wait_id = 0;
                if (runtime.cpu) {
                    runtime.cpu->write_gpr_sign_extended_32(2U, 0xffffffffU);
                }
                enqueue_ready_thread(*waiter);
            }
            semaphore_waiters_.erase(semaphore_waiters_.begin() +
                                     static_cast<std::ptrdiff_t>(*index));
        }
        semaphores_.erase(semaphores_.begin() + static_cast<std::ptrdiff_t>(*index));
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 65U, "DeleteSema"};
}

EeKernelCallResult EeKernelHle::set_osd_config_param(R5900State& cpu, const EeMemory& memory) {
    const auto address = cpu.read_gpr_u32(4U);
    if ((address & 3U) != 0U || !memory.contains(address, 4U)) {
        return {EeKernelCallStatus::InvalidArguments, 74U, "SetOsdConfigParam"};
    }
    osd_config_param_ = memory.read_u32_le(address);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 74U, "SetOsdConfigParam"};
}

EeKernelCallResult EeKernelHle::get_osd_config_param2(R5900State& cpu, EeMemory& memory) {
    const auto destination = cpu.read_gpr_u32(4U);
    const auto size = cpu.read_gpr_u32(5U);
    if (size > 256U || !memory.contains(destination, size)) {
        return {EeKernelCallStatus::InvalidArguments, 111U, "GetOsdConfigParam2"};
    }
    for (std::uint32_t index = 0U; index < size; ++index) {
        memory.write_u8(destination + index, 0U);
    }
    cpu.write_gpr_u64(2U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 111U, "GetOsdConfigParam2"};
}

std::optional<std::int32_t> EeKernelHle::lowest_free_intc_handler_id() const noexcept {
    for (std::int32_t candidate = 1; candidate != 129; ++candidate) {
        const bool used =
            std::ranges::any_of(intc_handlers_, [candidate](const EeIntcHandlerState& handler) {
                return handler.id == candidate;
            });
        if (!used) {
            return candidate;
        }
    }
    return std::nullopt;
}

EeKernelCallResult EeKernelHle::add_intc_handler(R5900State& cpu) {
    const auto id = lowest_free_intc_handler_id();
    if (!id) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 16U, "AddIntcHandler2"};
    }

    const auto cause = cpu.read_gpr_u32(4U);
    const auto next = cpu.read_gpr_u32(6U);
    const EeIntcHandlerState handler{
        .id = *id,
        .cause = cause,
        .handler = cpu.read_gpr_u32(5U),
        .argument = cpu.read_gpr_u32(7U),
        .global_pointer = cpu.read_gpr_u32(28U),
    };
    // next == 0 inserts before the first handler of the cause, -1 after the last
    // one, any other value before the handler with that id.
    auto insertion = intc_handlers_.end();
    if (next == 0U) {
        insertion = std::find_if(intc_handlers_.begin(), intc_handlers_.end(),
                                 [cause](const EeIntcHandlerState& existing) {
                                     return existing.cause == cause;
                                 });
    } else if (next == 0xffffffffU) {
        for (auto current = intc_handlers_.begin(); current != intc_handlers_.end(); ++current) {
            if (current->cause == cause) {
                insertion = current + 1;
            }
        }
    } else {
        insertion = std::find_if(intc_handlers_.begin(), intc_handlers_.end(),
                                 [next](const EeIntcHandlerState& existing) {
                                     return existing.id == static_cast<std::int32_t>(next);
                                 });
    }
    intc_handlers_.insert(insertion, handler);
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(*id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 16U, "AddIntcHandler2"};
}

std::optional<std::int32_t> EeKernelHle::lowest_free_dmac_handler_id() const noexcept {
    for (std::int32_t candidate = 1; candidate != 129; ++candidate) {
        const bool used =
            std::ranges::any_of(dmac_handlers_, [candidate](const EeDmacHandlerState& handler) {
                return handler.id == candidate;
            });
        if (!used) {
            return candidate;
        }
    }
    return std::nullopt;
}

EeKernelCallResult EeKernelHle::add_dmac_handler(R5900State& cpu) {
    const auto channel = cpu.read_gpr_u32(4U);
    if (channel > 9U) {
        return {EeKernelCallStatus::InvalidArguments, 18U, "AddDmacHandler2"};
    }
    const auto id = lowest_free_dmac_handler_id();
    if (!id) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 18U, "AddDmacHandler2"};
    }

    const auto next = cpu.read_gpr_u32(6U);
    const EeDmacHandlerState handler{
        .id = *id,
        .channel = channel,
        .handler = cpu.read_gpr_u32(5U),
        .argument = cpu.read_gpr_u32(7U),
        .global_pointer = cpu.read_gpr_u32(28U),
    };
    // Same placement rules as AddIntcHandler2, keyed by DMAC channel.
    auto insertion = dmac_handlers_.end();
    if (next == 0U) {
        insertion = std::find_if(dmac_handlers_.begin(), dmac_handlers_.end(),
                                 [channel](const EeDmacHandlerState& existing) {
                                     return existing.channel == channel;
                                 });
    } else if (next == 0xffffffffU) {
        for (auto current = dmac_handlers_.begin(); current != dmac_handlers_.end(); ++current) {
            if (current->channel == channel) {
                insertion = current + 1;
            }
        }
    } else {
        insertion = std::find_if(dmac_handlers_.begin(), dmac_handlers_.end(),
                                 [next](const EeDmacHandlerState& existing) {
                                     return existing.id == static_cast<std::int32_t>(next);
                                 });
    }
    dmac_handlers_.insert(insertion, handler);
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(*id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 18U, "AddDmacHandler2"};
}

// RECOVERY: only the operand order of the `handler.id == id` compare differs (SSA numbering).
EeKernelCallResult EeKernelHle::remove_intc_handler(R5900State& cpu) {
    const auto found = std::ranges::find_if(
        intc_handlers_,
        [cause = cpu.read_gpr_u32(4U), id = signed_argument(cpu, 5U)](
            const EeIntcHandlerState& handler) {
            return handler.cause == cause && handler.id == id;
        });
    if (found == intc_handlers_.end()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        intc_handlers_.erase(found);
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 17U, "RemoveIntcHandler"};
}

// RECOVERY: only the operand order of the `handler.id == id` compare differs (SSA numbering).
EeKernelCallResult EeKernelHle::remove_dmac_handler(R5900State& cpu) {
    const auto channel = cpu.read_gpr_u32(4U);
    const auto id = signed_argument(cpu, 5U);
    const auto found = std::ranges::find_if(
        dmac_handlers_, [channel, id](const EeDmacHandlerState& handler) {
            return handler.channel == channel && handler.id == id;
        });
    if (channel > 9U || found == dmac_handlers_.end()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        dmac_handlers_.erase(found);
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 19U, "RemoveDmacHandler"};
}

std::optional<std::int32_t> EeKernelHle::lowest_free_alarm_id() const noexcept {
    for (std::int32_t candidate = 1; candidate != 65; ++candidate) {
        const bool used = std::ranges::any_of(
            alarms_, [candidate](const EeAlarmState& alarm) { return alarm.id == candidate; });
        if (!used) {
            return candidate;
        }
    }
    return std::nullopt;
}

EeKernelCallResult EeKernelHle::set_alarm(R5900State& cpu) {
    const auto number = cpu.read_gpr_u32(3U);
    const auto handler = cpu.read_gpr_u32(5U);
    const auto id = lowest_free_alarm_id();
    if (handler == 0U || !id.has_value()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, number, "SetAlarm"};
    }
    const auto ticks = static_cast<std::uint16_t>(cpu.read_gpr_u32(4U));
    const unsigned long long scheduled_ticks = std::max<unsigned long long>(ticks, 1U);
    const unsigned long long delay = scheduled_ticks * 18750U;
    const std::uint64_t deadline = ~std::uint64_t{0} - ee_cycle_count_ >= delay
                                       ? ee_cycle_count_ + delay
                                       : ~std::uint64_t{0};
    alarms_.push_back({*id, ticks, handler, cpu.read_gpr_u32(6U), cpu.read_gpr_u32(28U), deadline});
    if (!next_alarm_deadline_.has_value() || *next_alarm_deadline_ > deadline) {
        next_alarm_deadline_ = deadline;
    }
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(*id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, number, "SetAlarm"};
}

// RECOVERY: matches except the order of the id compare in the search loop (the
// original hoists the int conversion of a0 out of the loop with a higher SSA number).
EeKernelCallResult EeKernelHle::release_alarm(R5900State& cpu) {
    const auto number = cpu.read_gpr_u32(3U);
    const auto requested = cpu.read_gpr_u32(4U);
    const auto found = std::ranges::find_if(alarms_, [requested](const EeAlarmState& alarm) {
        return alarm.id == static_cast<std::int32_t>(requested);
    });
    if (found == alarms_.end()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
    } else {
        const bool was_next =
            next_alarm_deadline_.has_value() && found->deadline_ee_cycles == *next_alarm_deadline_;
        alarms_.erase(found);
        if (was_next) {
            refresh_next_alarm_deadline();
        }
        cpu.write_gpr_u64(2U, 0U);
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, number, "ReleaseAlarm"};
}

EeKernelCallResult EeKernelHle::flush_cache(R5900State& cpu) {
    cache_maintenance_events_.push_back({cpu.read_gpr_u32(4U)});
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 100U, "FlushCache"};
}


EeKernelCallResult EeKernelHle::stop_sif_dma(R5900State& cpu, EeMemory& memory) {
    // Clear STR on SIF0's channel control register.
    memory.write_u32_le(0x1000c000U, memory.read_u32_le(0x1000c000U) & ~0x100U);
    sif0_dma_chain_enabled_ = false;
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 107U, "sceSifStopDma"};
}

EeKernelCallResult EeKernelHle::set_sif_dma_chain(R5900State& cpu, EeMemory& memory) {
    sif0_dma_chain_enabled_ = true;
    // Start SIF0 in chain mode with an empty quadword count.
    memory.write_u32_le(0x1000c000U, 0x184U);
    memory.write_u32_le(0x1000c020U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 120U, "sceSifSetDChain"};
}

std::uint32_t EeKernelHle::sif_register_value(const std::uint32_t number) const noexcept {
    const auto found = std::ranges::find_if(
        sif_registers_, [number](const EeSifRegisterState& state) { return state.number == number; });
    return found == sif_registers_.end() ? 0U : found->value;
}

void EeKernelHle::set_sif_register_value(const std::uint32_t number, const std::uint32_t value) {
    const auto found = std::ranges::find_if(
        sif_registers_, [number](const EeSifRegisterState& state) { return state.number == number; });
    if (found == sif_registers_.end()) {
        sif_registers_.push_back({number, value});
    } else {
        found->value = value;
    }
}

void EeKernelHle::complete_virtual_sif_rpc_init(const EeSifDmaTransferState& transfer,
                                                EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 16U ||
        !memory.contains(transfer.source, 16U)) {
        return;
    }
    if (memory.read_u32_le(transfer.source + 8U) != 0x80000002U ||
        memory.read_u32_le(transfer.source + 12U) != 1U) {
        return;
    }
    const auto bind_buffer = sif_register_value(0x80000001U);
    if ((bind_buffer & 3U) != 0U || !memory.contains(bind_buffer, 32U)) {
        return;
    }
    const auto reply = memory.read_u32_le(bind_buffer + 28U);
    if ((reply & 3U) != 0U || !memory.contains(reply, 4U)) {
        return;
    }
    memory.write_u32_le(reply, 1U);
}

// RECOVERY: statement-level match expected; the diff is dominated by EeMemory
// write_u32_le inlining decisions that depend on the rest of the file.
void EeKernelHle::complete_virtual_rpc_bind(const EeSifDmaTransferState& transfer,
                                            EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 36U ||
        !memory.contains(transfer.source, 36U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x80000009U) {
        return;
    }
    // Defaults describe the first service (0x80000592); the others override them.
    std::uint32_t server = 0x000f1000U;
    std::uint32_t buffer = 0x000f1100U;
    std::uint32_t callback_buffer = 0x000f1200U;
    switch (memory.read_u32_le(transfer.source + 32U)) {
    case 0x80000593U:
        server = 0x000f1300U;
        buffer = 0x000f1400U;
        callback_buffer = 0x000f1500U;
        break;
    case 0x8000059cU:
        server = 0x000f1600U;
        buffer = 0x000f1700U;
        callback_buffer = 0x00000000U;
        break;
    case 0x80000001U:
        server = 0x000f1800U;
        buffer = 0x000f1900U;
        callback_buffer = 0x00000000U;
        break;
    case 0x80000003U:
        server = 0x000f1a00U;
        buffer = 0x000f1b00U;
        callback_buffer = 0x00000000U;
        break;
    case 0x80000006U:
        server = 0x000f1c00U;
        buffer = 0x000f1d00U;
        callback_buffer = 0x00000000U;
        break;
    case 0x80001300U:
        server = 0x000f1e00U;
        buffer = 0x000f1f00U;
        callback_buffer = 0x00000000U;
        break;
    case 0x8000131bU:
        server = 0x000f2000U;
        buffer = 0x000f2100U;
        callback_buffer = 0x00000000U;
        break;
    case 0x8000131cU:
        server = 0x000f2200U;
        buffer = 0x000f2300U;
        callback_buffer = 0x00000000U;
        break;
    case 0x046d046dU:
        server = 0x000f2400U;
        buffer = 0x000f2500U;
        callback_buffer = 0x00000000U;
        break;
    case 0x00150276U:
        server = 0x000f2600U;
        buffer = 0x000f2700U;
        callback_buffer = 0x00000000U;
        break;
    case 0x00475453U:
        server = 0x000f2800U;
        buffer = 0x000f2900U;
        callback_buffer = 0x00000000U;
        break;
    case 0x80001101U:
        server = 0x000f2a00U;
        buffer = 0x000f2b00U;
        callback_buffer = 0x00000000U;
        break;
    default:
        return;
    case 0x80000592U:
        break;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    if ((client & 3U) != 0U || !memory.contains(client, 40U)) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    memory.write_u32_le(client + 20U, buffer);
    memory.write_u32_le(client + 24U, callback_buffer);
    memory.write_u32_le(client + 36U, server);
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_network_config_lifecycle_call(
    const EeSifDmaTransferState& transfer, EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U)) {
        return;
    }
    if (memory.read_u32_le(transfer.source + 8U) != 0x8000000aU) {
        return;
    }
    const auto command = memory.read_u32_le(transfer.source + 32U);
    if ((command != 11U && command != 15U) ||
        memory.read_u32_le(transfer.source + 36U) != 0x1000U ||
        memory.read_u32_le(transfer.source + 44U) != 0x1000U ||
        !pending_network_config_payload_.has_value() ||
        pending_network_config_payload_->destination != 0x000f2b00U ||
        pending_network_config_payload_->size != 0x1000U ||
        !memory.contains(pending_network_config_payload_->source, 0x1000U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto result = memory.read_u32_le(transfer.source + 40U);
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2a00U ||
        (result & 15U) != 0U || !memory.contains(result, 0x1000U)) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    const auto request = pending_network_config_payload_->source;
    if (result != request) {
        for (std::size_t byte = 0U; byte != 0x1000U; ++byte) {
            memory.write_u8(result + byte, memory.read_u8(request + byte));
        }
    }
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_network_config_payload_.reset();
}

void EeKernelHle::complete_virtual_b3_route_init_call(const EeSifDmaTransferState& transfer,
                                                      EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x00150276U ||
        memory.read_u32_le(transfer.source + 36U) != 0x400U ||
        memory.read_u32_le(transfer.source + 40U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) != 0U ||
        !pending_b3_route_payload_.has_value() ||
        pending_b3_route_payload_->destination != 0x000f2700U ||
        pending_b3_route_payload_->size != 0x400U ||
        !memory.contains(pending_b3_route_payload_->source, 0x400U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto payload = pending_b3_route_payload_->source;
    const auto target = memory.read_u32_le(payload + 8U);
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2600U ||
        memory.read_u32_le(payload) != 1U || memory.read_u32_le(payload + 4U) != 0x8000U ||
        (target & 15U) != 0U || !memory.contains(target, 4U) ||
        memory.read_u32_le(payload + 12U) != 0U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    virtual_b3_route_target_ = target;
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_b3_route_payload_.reset();
}

void EeKernelHle::complete_virtual_b3_route_routing_call(const EeSifDmaTransferState& transfer,
                                                         EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x00150276U ||
        memory.read_u32_le(transfer.source + 36U) != 0x400U ||
        memory.read_u32_le(transfer.source + 40U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) != 0U ||
        !pending_b3_route_payload_.has_value() ||
        pending_b3_route_payload_->destination != 0x000f2700U ||
        pending_b3_route_payload_->size != 0x400U ||
        !memory.contains(pending_b3_route_payload_->source, 0x400U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto payload = pending_b3_route_payload_->source;
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2600U ||
        memory.read_u32_le(payload) != 2U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    virtual_b3_route_routing_word0_ = memory.read_u32_le(payload + 4U);
    virtual_b3_route_routing_word1_ = memory.read_u32_le(payload + 8U);
    virtual_b3_route_routing_word2_ = memory.read_u32_le(payload + 12U);
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_b3_route_payload_.reset();
}

void EeKernelHle::complete_virtual_gtfs_cdvd_init_call(const EeSifDmaTransferState& transfer,
                                                       EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 1U ||
        memory.read_u32_le(transfer.source + 36U) != 48U ||
        memory.read_u32_le(transfer.source + 44U) != 4U ||
        !pending_gtfs_cdvd_payload_.has_value() ||
        pending_gtfs_cdvd_payload_->destination != 0x000f2900U ||
        pending_gtfs_cdvd_payload_->size != 48U ||
        !memory.contains(pending_gtfs_cdvd_payload_->source, 48U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto result = memory.read_u32_le(transfer.source + 40U);
    const auto request = pending_gtfs_cdvd_payload_->source;
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2800U ||
        (result & 3U) != 0U || !memory.contains(result, 4U) ||
        memory.read_u32_le(request) != 1U || memory.read_u32_le(request + 4U) != 3U ||
        memory.read_u32_le(request + 8U) != 32U || memory.read_u32_le(request + 12U) != 16U) {
        return;
    }
    for (std::uint32_t offset = 16U; offset != 48U; offset += 4U) {
        if (memory.read_u32_le(request + offset) != 0U) {
            return;
        }
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    memory.write_u32_le(result, gtfs_cdvd_interface_address_);
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_gtfs_cdvd_payload_.reset();
}

// RECOVERY: statements and control flow match; only the inlining of the noexcept
// path move constructor (out-of-line `path::path(path&&)` in the original) differs.
void EeKernelHle::complete_virtual_gtfs_cdvd_open_call(const EeSifDmaTransferState& transfer,
                                                       EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 3U ||
        memory.read_u32_le(transfer.source + 36U) != 64U ||
        memory.read_u32_le(transfer.source + 44U) != 16U ||
        !pending_gtfs_cdvd_payload_.has_value() ||
        pending_gtfs_cdvd_payload_->destination != 0x000f2900U ||
        pending_gtfs_cdvd_payload_->size != 64U ||
        !memory.contains(pending_gtfs_cdvd_payload_->source, 64U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto result = memory.read_u32_le(transfer.source + 40U);
    const auto request = pending_gtfs_cdvd_payload_->source;
    const auto guest_path = read_guest_string(memory, request, 64U);
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2800U ||
        (result & 3U) != 0U || !memory.contains(result, 16U) || !guest_path.has_value()) {
        return;
    }
    std::filesystem::path host_path{};
    std::error_code path_error{};
    std::uint32_t file_size{};
    std::optional<std::size_t> file_index{};
    if (!disc_root_.empty()) {
        const auto resolved =
            content::resolve_game_path(disc_root_, mod_overlay_, *guest_path, path_error);
        if (resolved.has_value() && std::filesystem::is_regular_file(*resolved, path_error)) {
            const auto host_size = std::filesystem::file_size(*resolved, path_error);
            if (!path_error && host_size <= 0xffffffffULL) {
                for (std::size_t index = 0; index < virtual_gtfs_files_.size(); ++index) {
                    if (!virtual_gtfs_files_[index].open) {
                        host_path = *resolved;
                        file_size = static_cast<std::uint32_t>(host_size);
                        file_index = index;
                        break;
                    }
                }
            }
        }
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    ++gtfs_activity_.open_requests;
    const auto record_index = gtfs_open_records_.size();
    gtfs_open_records_.push_back({.guest_path = *guest_path,
                                  .host_path = host_path,
                                  .handle = !file_index.has_value() ? 0xffffffffU : 0U,
                                  .file_size = file_size,
                                  .opened = file_index.has_value()});
    std::ranges::fill(memory.view(result, 16U), std::byte{0});
    if (file_index.has_value()) {
        ++gtfs_activity_.successful_opens;
        virtual_gtfs_files_[*file_index] = {.path = std::move(host_path),
                                            .size = file_size,
                                            .activity_index = record_index,
                                            .open = true};
        memory.write_u32_le(result, 0U);
        memory.write_u32_le(result + 4U, file_size);
        memory.write_u32_le(result + 8U, (file_size + 0x7ffU) >> 11U);
    } else {
        memory.write_u32_le(result, 0xffffffffU);
    }
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_gtfs_cdvd_payload_.reset();
}

void EeKernelHle::complete_virtual_gtfs_cdvd_close_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 4U ||
        memory.read_u32_le(transfer.source + 36U) != 16U ||
        memory.read_u32_le(transfer.source + 40U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) != 0U ||
        !pending_gtfs_cdvd_payload_.has_value() ||
        pending_gtfs_cdvd_payload_->destination != 0x000f2900U ||
        pending_gtfs_cdvd_payload_->size != 16U ||
        !memory.contains(pending_gtfs_cdvd_payload_->source, 16U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto request = pending_gtfs_cdvd_payload_->source;
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2800U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    ++gtfs_activity_.close_requests;
    const auto handle = memory.read_u32_le(request);
    if (handle < virtual_gtfs_files_.size()) {
        auto& file = virtual_gtfs_files_[handle];
        if (file.open) {
            ++gtfs_activity_.successful_closes;
            if (file.activity_index.has_value() &&
                *file.activity_index < gtfs_open_records_.size()) {
                gtfs_open_records_[*file.activity_index].closed = true;
            }
        }
        file = {};
    }
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_gtfs_cdvd_payload_.reset();
}

// RECOVERY: the original does not thread the `accepted` PHI after the host read (the
// first `if (accepted)` stays a separate branch) and prints `idx < size` for the first
// activity-index bounds test; make_shared<HostFileReader> stays out of line there
// (shared __shared_count clone), which depends on whole-file inlining.
void EeKernelHle::complete_virtual_gtfs_cdvd_read_call(const EeSifDmaTransferState& transfer,
                                                       EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 5U ||
        memory.read_u32_le(transfer.source + 36U) != 16U ||
        memory.read_u32_le(transfer.source + 40U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) != 0U ||
        !pending_gtfs_cdvd_payload_.has_value() ||
        pending_gtfs_cdvd_payload_->destination != 0x000f2900U ||
        pending_gtfs_cdvd_payload_->size != 16U ||
        !memory.contains(pending_gtfs_cdvd_payload_->source, 16U)) {
        return;
    }
    const auto client = memory.read_u32_le(transfer.source + 28U);
    const auto request = pending_gtfs_cdvd_payload_->source;
    if ((client & 3U) != 0U || !memory.contains(client, 40U) ||
        memory.read_u32_le(client) != transfer.source ||
        memory.read_u32_le(client + 36U) != 0x000f2800U) {
        return;
    }
    const auto handle = memory.read_u32_le(request);
    const auto sector_count = memory.read_u32_le(request + 4U);
    const auto destination = memory.read_u32_le(request + 8U);
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(client + 8U)))) {
        return;
    }
    const unsigned long long byte_count = static_cast<unsigned long long>(sector_count) * 2048U;
    // TAKEDOWN_GTFS_READ_TRACE=<n> logs the first n (default 48) reads.
    if (const char* v = std::getenv("TAKEDOWN_GTFS_READ_TRACE"); v != nullptr) {
        static std::size_t logged = std::strtoul(v, nullptr, 10) > 1UL
                                        ? static_cast<std::size_t>(std::strtoul(v, nullptr, 10))
                                        : 48U;
        if (logged != 0U) {
            std::string path = "?";
            std::uint32_t file_sector = 0U;
            if (handle < virtual_gtfs_files_.size() && virtual_gtfs_files_[handle].open) {
                path = virtual_gtfs_files_[handle].path.generic_string();
                file_sector = virtual_gtfs_files_[handle].sector;
            }
            std::fprintf(stderr, "gtfs-read: dest=%08x file-sector=%u bytes=%llu file=%s\n",
                         destination, file_sector, byte_count, path.c_str());
            --logged;
        }
    }
    ++gtfs_activity_.read_requests;
    gtfs_activity_.sectors_requested += sector_count;
    std::optional<std::size_t> activity_index{};
    if (handle < virtual_gtfs_files_.size() && virtual_gtfs_files_[handle].open) {
        activity_index = virtual_gtfs_files_[handle].activity_index;
        if (activity_index.has_value() && *activity_index < gtfs_open_records_.size()) {
            auto& record = gtfs_open_records_[*activity_index];
            ++record.read_requests;
            record.sectors_requested += sector_count;
        }
    }
    bool accepted = false;
    if (handle < virtual_gtfs_files_.size() && virtual_gtfs_files_[handle].open &&
        memory.contains(destination, byte_count)) {
        auto& file = virtual_gtfs_files_[handle];
        const unsigned long long file_offset = static_cast<unsigned long long>(file.sector) * 2048U;
        const unsigned long long remaining =
            file.size > file_offset ? file.size - file_offset : 0U;
        const auto host_bytes = std::min<unsigned long long>(byte_count, remaining);
        {
            auto destination_bytes = memory.view(destination, byte_count);
            std::ranges::fill(destination_bytes, std::byte{0});
            accepted = true;
            if (host_bytes != 0U) {
                if (host_file_reader_ == nullptr) {
                    host_file_reader_ = std::make_shared<platform::HostFileReader>();
                }
                const platform::ScopedHostWait host_wait{platform::HostWaitSite::CdvdRead};
                accepted = host_file_reader_->read_at(file.path, file_offset,
                                                      destination_bytes.first(host_bytes)) ==
                           platform::HostFileReadStatus::Ok;
            }
            if (accepted) {
                const unsigned long long total_sectors =
                    (static_cast<unsigned long long>(file.size) + 0x7ffU) >> 11U;
                file.sector = static_cast<std::uint32_t>(
                    std::min<unsigned long long>(total_sectors, sector_count + file.sector));
            }
        }
        if (accepted) {
            const unsigned long long padding_bytes = byte_count - host_bytes;
            ++gtfs_activity_.successful_reads;
            gtfs_activity_.host_bytes_read += host_bytes;
            gtfs_activity_.zero_padding_bytes += padding_bytes;
            if (activity_index.has_value() && *activity_index < gtfs_open_records_.size()) {
                auto& record = gtfs_open_records_[*activity_index];
                ++record.successful_reads;
                record.host_bytes_read += host_bytes;
                record.zero_padding_bytes += padding_bytes;
            }
        }
    }
    memory.write_u32_le(client, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & 0xfffffffeU);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_gtfs_cdvd_payload_.reset();
    if (accepted) {
        // Reply to the guest's GTFS read callback with the completed handle.
        memory.write_u32_le(transfer.source, 16U);
        memory.write_u32_le(transfer.source + 4U, 0U);
        memory.write_u32_le(transfer.source + 8U, 4U);
        memory.write_u32_le(transfer.source + 12U, handle & 0xffU);
        pending_sif_command_callbacks_.push_back(
            {.handler = 0x001d2f50U, .context = transfer.source, .command = 0x0066e100U});
    }
}

// RECOVERY: identical except the order of the edges feeding the `accepted` PHI (73/76 swapped).
void EeKernelHle::complete_virtual_gtfs_cdvd_seek_call(const EeSifDmaTransferState& transfer,
                                                       EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 6U ||
        memory.read_u32_le(transfer.source + 36U) != 16U ||
        memory.read_u32_le(transfer.source + 40U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) != 0U ||
        !pending_gtfs_cdvd_payload_.has_value() ||
        pending_gtfs_cdvd_payload_->destination != 0x000f2900U ||
        pending_gtfs_cdvd_payload_->size != 16U ||
        !memory.contains(pending_gtfs_cdvd_payload_->source, 16U)) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto request = pending_gtfs_cdvd_payload_->source;
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) ||
        memory.read_u32_le(payload) != transfer.source ||
        memory.read_u32_le(payload + 36U) != 0x000f2800U) {
        return;
    }
    const auto handle = memory.read_u32_le(request);
    const auto position = std::bit_cast<std::int32_t>(memory.read_u32_le(request + 4U));
    const auto whence = memory.read_u32_le(request + 8U);
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    ++gtfs_activity_.seek_requests;
    std::optional<std::size_t> activity_index{};
    if (handle < virtual_gtfs_files_.size() && virtual_gtfs_files_[handle].open) {
        activity_index = virtual_gtfs_files_[handle].activity_index;
        if (activity_index.has_value() && *activity_index < gtfs_open_records_.size()) {
            ++gtfs_open_records_[*activity_index].seek_requests;
        }
    }
    bool accepted = false;
    if (handle < virtual_gtfs_files_.size() && virtual_gtfs_files_[handle].open && whence <= 2U) {
        auto& file = virtual_gtfs_files_[handle];
        const auto total_sectors = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(file.size) + 2047U) / 2048U);
        std::int64_t target = position;
        if (whence == 1U) {
            target = position + (static_cast<std::int64_t>(file.sector) - 1);
        } else if (whence == 2U) {
            target = total_sectors - position;
        }
        file.sector =
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(target, 0, total_sectors));
        ++gtfs_activity_.successful_seeks;
        if (activity_index.has_value() && *activity_index < gtfs_open_records_.size()) {
            ++gtfs_open_records_[*activity_index].successful_seeks;
        }
        accepted = true;
    }
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_gtfs_cdvd_payload_.reset();
    if (accepted) {
        memory.write_u32_le(transfer.source, 16U);
        memory.write_u32_le(transfer.source + 4U, 0U);
        memory.write_u32_le(transfer.source + 8U, 4U);
        memory.write_u32_le(transfer.source + 12U, handle & 0xffU);
        pending_sif_command_callbacks_.push_back(
            {.handler = 0x001d2f50U, .context = transfer.source, .command = 0x0066e100U});
    }
}

void EeKernelHle::complete_virtual_cdvd_init_call(const EeSifDmaTransferState& transfer,
                                                  EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0U ||
        memory.read_u32_le(transfer.source + 36U) != 4U ||
        memory.read_u32_le(transfer.source + 44U) < 16U) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 16U) || memory.read_u32_le(payload + 36U) != 0x000f1000U) {
        return;
    }
    const auto semaphore = std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U));
    memory.write_u32_le(response, 1U);
    memory.write_u32_le(response + 4U, 0U);
    memory.write_u32_le(response + 8U, 0U);
    memory.write_u32_le(response + 12U, 0U);
    if (!signal_semaphore(semaphore)) {
        return;
    }
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_cdvd_scmd_call(const EeSifDmaTransferState& transfer,
                                                  EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU) {
        return;
    }
    const auto command = memory.read_u32_le(transfer.source + 32U);
    const auto selector = memory.read_u32_le(transfer.source + 36U);
    const auto input_size = memory.read_u32_le(transfer.source + 44U);
    std::size_t required_reply_size;
    if (command == 34U && selector == 4U && input_size >= 4U) {
        required_reply_size = 4U;
    } else if (command == 1U && selector == 0U && input_size == 16U) {
        required_reply_size = 16U;
    } else {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, required_reply_size) ||
        memory.read_u32_le(payload + 36U) != 0x000f1300U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    if (command == 1U) {
        std::ranges::fill(memory.view(response, 16U), std::byte{0});
        memory.write_u32_le(response, 1U);
        memory.write_u8(response + 7U, 0x12U);
        memory.write_u8(response + 9U, 0x07U);
        memory.write_u8(response + 10U, 0x09U);
        memory.write_u8(response + 11U, 0x04U);
    } else {
        memory.write_u32_le(response, 1U);
    }
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_cdvd_disk_ready_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0U ||
        memory.read_u32_le(transfer.source + 36U) != 4U ||
        memory.read_u32_le(transfer.source + 44U) < 4U) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 4U) || memory.read_u32_le(payload + 36U) != 0x000f1600U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 2U);
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_file_io_version_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0xffU ||
        memory.read_u32_le(transfer.source + 36U) != 8U ||
        memory.read_u32_le(transfer.source + 44U) < 8U) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 8U) || memory.read_u32_le(payload + 36U) != 0x000f1800U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 0x30303832U);
    memory.write_u32_le(response + 4U, 2U);
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_file_io_payload_.reset();
}

void EeKernelHle::complete_virtual_load_file_version_call(const EeSifDmaTransferState& transfer,
                                                          EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0xffU ||
        memory.read_u32_le(transfer.source + 36U) != 0U ||
        memory.read_u32_le(transfer.source + 44U) < 4U) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 4U) || memory.read_u32_le(payload + 36U) != 0x000f1c00U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 0x30303832U);
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

// RECOVERY: identical except for a one-unit rounding of the final profile count (bb1 1073741823 vs 1073741824).
void EeKernelHle::complete_virtual_iop_heap_call(const EeSifDmaTransferState& transfer,
                                                 EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 44U) != 4U) {
        return;
    }
    if (!pending_iop_heap_payload_.has_value() ||
        pending_iop_heap_payload_->destination != 0x000f1b00U ||
        pending_iop_heap_payload_->size < 4U) {
        return;
    }
    const auto command = memory.read_u32_le(transfer.source + 32U);
    const unsigned long long request_size = command == 4U ? 12U : 4U;
    if (command != 1U && command != 2U && command != 4U) {
        return;
    }
    if (memory.read_u32_le(transfer.source + 36U) != request_size ||
        pending_iop_heap_payload_->size < request_size ||
        !memory.contains(pending_iop_heap_payload_->source, request_size)) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 4U) || memory.read_u32_le(payload + 36U) != 0x000f1a00U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    const auto argument = memory.read_u32_le(pending_iop_heap_payload_->source);
    if (command == 1U || command == 4U) {
        // Allocate: first fit above the 1 MiB base of the IOP heap, 256-byte granules.
        std::uint32_t allocated_address = 0U;
        const bool valid = command == 1U ||
                           (memory.read_u32_le(pending_iop_heap_payload_->source + 4U) == 0U &&
                            memory.read_u32_le(pending_iop_heap_payload_->source + 8U) == 0U);
        if (valid && argument != 0U) {
            if (argument <= 0x100000U - 255U) {
                const auto aligned_size = (argument + 255U) & ~255U;
                auto candidate = 0x100000U;
                auto insertion = virtual_iop_heap_blocks_.begin();
                for (; insertion != virtual_iop_heap_blocks_.end(); ++insertion) {
                    if (insertion->address - candidate >= aligned_size) {
                        break;
                    }
                    candidate =
                        insertion->address + static_cast<std::uint32_t>(insertion->bytes.size());
                }
                if (candidate <= 0x200000U - aligned_size) {
                    virtual_iop_heap_blocks_.insert(
                        insertion, VirtualIopHeapBlock{candidate,
                                                       std::vector<std::uint8_t>(aligned_size)});
                    allocated_address = candidate;
                }
            }
        }
        memory.write_u32_le(response, allocated_address);
    } else {
        const auto block = std::ranges::find_if(
            virtual_iop_heap_blocks_,
            [argument](const VirtualIopHeapBlock& candidate) { return candidate.address == argument; });
        const int result = block == virtual_iop_heap_blocks_.end() ? -1 : 0;
        if (block != virtual_iop_heap_blocks_.end()) {
            virtual_iop_heap_blocks_.erase(block);
        }
        memory.write_u32_le(response, static_cast<std::uint32_t>(result));
    }
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_iop_heap_payload_.reset();
}

void EeKernelHle::complete_virtual_load_file_module_call(const EeSifDmaTransferState& transfer,
                                                         EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0U ||
        memory.read_u32_le(transfer.source + 36U) != 512U ||
        memory.read_u32_le(transfer.source + 44U) < 8U) {
        return;
    }
    if (!pending_load_file_payload_.has_value() ||
        pending_load_file_payload_->destination != 0x000f1d00U ||
        pending_load_file_payload_->size != 512U ||
        !memory.contains(pending_load_file_payload_->source, 512U)) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 512U) || pending_load_file_payload_->source != response ||
        memory.read_u32_le(payload + 36U) != 0x000f1c00U) {
        return;
    }
    // The module path is the NUL-terminated string at response + 8.
    bool found_path_terminator = false;
    for (std::size_t index = 0U; index < 252U; ++index) {
        if (memory.read_u8(response + static_cast<GuestAddress>(index) + 8U) == 0U) {
            found_path_terminator = index != 0U;
            break;
        }
    }
    if (!found_path_terminator) {
        return;
    }
    if (next_virtual_iop_module_id_ <= 0) {
        return;
    }
    if (next_virtual_iop_module_id_ == std::numeric_limits<std::int32_t>::max()) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    memory.write_u32_le(response, static_cast<std::uint32_t>(next_virtual_iop_module_id_++));
    memory.write_u32_le(response + 4U, 0U);
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_load_file_payload_.reset();
}

void EeKernelHle::complete_virtual_dbc_man_version_call(const EeSifDmaTransferState& transfer,
                                                        EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 56U ||
        !memory.contains(transfer.source, 56U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x80001363U ||
        memory.read_u32_le(transfer.source + 36U) != 640U ||
        memory.read_u32_le(transfer.source + 44U) != 640U) {
        return;
    }
    const auto payload = memory.read_u32_le(transfer.source + 28U);
    const auto response = memory.read_u32_le(transfer.source + 40U);
    if ((payload & 3U) != 0U || !memory.contains(payload, 40U) || (response & 3U) != 0U ||
        !memory.contains(response, 640U) || memory.read_u32_le(payload + 36U) != 0x000f1e00U) {
        return;
    }
    if (!signal_semaphore(std::bit_cast<std::int32_t>(memory.read_u32_le(payload + 8U)))) {
        return;
    }
    for (std::size_t offset = 0U; offset < 16U; offset += 4U) {
        memory.write_u32_le(response + static_cast<GuestAddress>(offset), 800U);
    }
    memory.write_u32_le(payload, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_dbc_man_set_work_address_call(
    const EeSifDmaTransferState& transfer, EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x80001304U ||
        memory.read_u32_le(transfer.source + 36U) != 0x280U ||
        memory.read_u32_le(transfer.source + 44U) != 0x280U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 15U) != 0U ||
        !memory.contains(response, 640U) || memory.read_u32_le(request + 36U) != 0xf1e00U) {
        return;
    }
    const std::uint32_t work_address = memory.read_u32_le(response + 4U);
    if ((work_address & 15U) != 0U || !memory.contains(work_address, 16U)) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    dbc_work_address_ = work_address;
    memory.write_u32_le(response, 1U);
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_dbc_man_create_socket_call(
    const EeSifDmaTransferState& transfer, EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x80001301U ||
        memory.read_u32_le(transfer.source + 36U) != 0x280U ||
        memory.read_u32_le(transfer.source + 44U) != 0x280U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 15U) != 0U ||
        !memory.contains(response, 640U) || memory.read_u32_le(request + 36U) != 0xf1e00U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    const auto free_socket = std::ranges::find_if(
        virtual_dbc_sockets_, [](const VirtualDbcSocket& socket) { return !socket.active; });
    const int socket_id = slot_index_or(virtual_dbc_sockets_, free_socket, -21);
    if (free_socket != virtual_dbc_sockets_.end()) {
        auto& socket = *free_socket;
        socket = VirtualDbcSocket{};
        socket.response_buffers = {memory.read_u32_le(response + 40U),
                                   memory.read_u32_le(response + 44U)};
        socket.active = true;
        socket.descriptor_type = memory.read_u32_le(response + 4U);
        publish_save_card_socket_status(memory, static_cast<std::size_t>(socket_id));
    }
    memory.write_u32_le(response + 36U, static_cast<std::uint32_t>(socket_id));
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}


void EeKernelHle::publish_save_card_socket_status(EeMemory& memory, const std::size_t socket) {
    const auto status_word =
        static_cast<std::uint64_t>(dbc_work_address_) + (socket & 0xffffffffU) * 4U;
    const auto& state = virtual_dbc_sockets_[socket];
    if (!state.active || state.descriptor_type != 2U || dbc_work_address_ == 0U ||
        !memory.contains(static_cast<GuestAddress>(status_word), 4U)) {
        return;
    }
    const bool connected = save_card_present_ && save_card_device_ &&
                           save_card_device_->attached_slot().has_value();
    memory.write_u32_le(static_cast<GuestAddress>(status_word), connected ? 1U : 0U);
}

void EeKernelHle::set_save_card_present(EeMemory& memory, const bool present) {
    save_card_present_ = present;
    for (std::size_t socket = 0; socket < virtual_dbc_sockets_.size(); ++socket) {
        publish_save_card_socket_status(memory, socket);
    }
}


// RECOVERY: only differs in two named locals (status, descriptor_type) that are
// anonymous in the original; control flow and operations are identical.
void EeKernelHle::complete_virtual_dbc_man_delete_socket_call(
    const EeSifDmaTransferState& transfer, EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x80001302U ||
        memory.read_u32_le(transfer.source + 36U) != 0x280U ||
        memory.read_u32_le(transfer.source + 44U) != 0x280U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 15U) != 0U ||
        !memory.contains(response, 640U) || memory.read_u32_le(request + 36U) != 0xf1e00U) {
        return;
    }
    const std::uint32_t socket = memory.read_u32_le(response);
    const bool valid = static_cast<std::int32_t>(socket) >= 0 && socket_is_active(virtual_dbc_sockets_, socket);
    const auto status = delete_socket_status(valid);
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    if (valid) {
        const auto descriptor_type = descriptor_type_of(
            virtual_dbc_sockets_, static_cast<std::size_t>(static_cast<std::int32_t>(socket)));
        virtual_dbc_sockets_[static_cast<std::size_t>(static_cast<std::int32_t>(socket))] = VirtualDbcSocket{};
        const auto status_word = static_cast<std::uint64_t>(dbc_work_address_) +
                                 static_cast<std::uint64_t>(socket) * 4U;
        if (descriptor_type == 2U && dbc_work_address_ != 0U &&
            memory.contains(static_cast<GuestAddress>(status_word), 4U)) {
            memory.write_u32_le(static_cast<GuestAddress>(status_word), 0U);
        }
    }
    memory.write_u32_le(response + 4U, static_cast<std::uint32_t>(status));
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}


void EeKernelHle::complete_virtual_dbc_man_get_dependency_call(
    const EeSifDmaTransferState& transfer, EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x80001303U ||
        memory.read_u32_le(transfer.source + 36U) != 0x280U ||
        memory.read_u32_le(transfer.source + 44U) != 0x280U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 15U) != 0U ||
        !memory.contains(response, 640U) || memory.read_u32_le(request + 36U) != 0xf1e00U) {
        return;
    }
    [[maybe_unused]] const std::uint32_t socket = memory.read_u32_le(response);
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    // Reported status for every socket: -12.
    memory.write_u32_le(response + 4U, 0xfffffff4U);
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}


// RECOVERY: statements match, but GCC inlined fewer EeMemory::read_u32_le calls
// in the original (the trace block's reads stay calls); the original's trace
// block structure that causes that was not found.
void EeKernelHle::complete_virtual_dbc_service_1c_call(const EeSifDmaTransferState& transfer,
                                                       EeMemory& memory) {
    if (save_card_trace_enabled() && transfer.destination == 0x000f0000U &&
        transfer.size >= 0x38U && memory.contains(transfer.source, 0x38U) &&
        memory.read_u32_le(transfer.source + 8U) == 0x8000000aU) {
        const std::uint32_t rpc = memory.read_u32_le(transfer.source + 32U);
        if ((rpc & 0xffffff00U) == 0x80001300U) {
            const bool has_payload = pending_dbc_service_1c_payload_.has_value();
            const unsigned request =
                has_payload ? pending_dbc_service_1c_payload_->source : 0U;
            std::fprintf(stderr,
                         "[save-card] rpc=0x%08x send=0x%x recv=0x%x payload=%d op=%u "
                         "fields=%08x %08x %08x %08x\n",
                         rpc, memory.read_u32_le(transfer.source + 36U),
                         memory.read_u32_le(transfer.source + 44U), has_payload ? 1 : 0,
                         has_payload ? memory.read_u32_le(request + 4U) : 0U,
                         has_payload ? memory.read_u32_le(request + 12U) : 0U,
                         has_payload ? memory.read_u32_le(request + 16U) : 0U,
                         has_payload ? memory.read_u32_le(request + 20U) : 0U,
                         has_payload ? memory.read_u32_le(request + 44U) : 0U);
            const std::uint32_t packet = memory.read_u32_le(transfer.source + 40U);
            if (rpc != 0x8000131cU && memory.contains(packet, 48U)) {
                std::fprintf(stderr, "[save-card]   packet=0x%08x:", packet);
                for (std::uint32_t word = 0U; word < 12U; ++word) {
                    std::fprintf(stderr, " %08x", memory.read_u32_le(packet + word * 4U));
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x8000131cU ||
        memory.read_u32_le(transfer.source + 36U) != 0x2090U ||
        memory.read_u32_le(transfer.source + 44U) != 0x2090U ||
        memory.read_u32_le(transfer.source + 48U) != 1U) {
        return;
    }
    if (!pending_dbc_service_1c_payload_ || pending_dbc_service_1c_payload_->destination != 0xf2300U ||
        pending_dbc_service_1c_payload_->size != 0x2090U ||
        !memory.contains(pending_dbc_service_1c_payload_->source, 0x2090U)) {
        return;
    }
    const std::uint32_t packet = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t reply = memory.read_u32_le(transfer.source + 40U);
    const unsigned request = pending_dbc_service_1c_payload_->source;
    if ((packet & 3U) != 0U || !memory.contains(packet, 40U) ||
        memory.read_u32_le(packet) != transfer.source ||
        memory.read_u32_le(packet + 36U) != 0xf2200U ||
        memory.read_u32_le(packet + 8U) != 0xffffffffU || (reply & 15U) != 0U ||
        reply != request || !memory.contains(reply, 0x2090U) ||
        memory.read_u32_le(request + 8U) != 0x2080U) {
        return;
    }
    const std::uint32_t socket = memory.read_u32_le(request);
    if (static_cast<std::int32_t>(socket) < 0 ||
        slot_of(socket) >= virtual_dbc_sockets_.size()) {
        return;
    }
    auto& state = virtual_dbc_sockets_[slot_of(socket)];
    if (!state.active || state.next_response_buffer > 1U) {
        return;
    }
    const unsigned response = state.response_buffers
        [state.next_response_buffer];
    const std::uint32_t handler = memory.read_u32_le(packet + 28U);
    const std::uint32_t context = memory.read_u32_le(packet + 32U);
    const std::uint32_t global_pointer = memory.read_u32_le(packet + 24U);
    if ((response & 15U) != 0U || !memory.contains(response, 128U) || handler == 0U ||
        (handler & 3U) != 0U) {
        return;
    }
    const std::uint32_t operation = memory.read_u32_le(request + 4U);
    if (!save_card_device_ && operation != 4U) {
        return;
    }
    const unsigned header = save_card_device_ && operation == 1U
                                     ? state.response_buffers[0]
                                     : response;
    const GuestAddress read_data = state.response_buffers[0] + 128U;
    if ((header & 15U) != 0U || !memory.contains(header, 128U)) {
        return;
    }
    std::array<std::byte, 116> data{};
    std::ranges::fill(data, std::byte{0xff});
    std::uint32_t result = 0U;
    if (save_card_device_) {
        result = serve_save_card_operation(operation, request, read_data, data, memory);
    }
    if (save_card_trace_enabled()) {
        std::fprintf(stderr, "[save-card] op=%u result=%u data=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                     operation, result, static_cast<unsigned>(data[0]),
                     static_cast<unsigned>(data[1]), static_cast<unsigned>(data[2]),
                     static_cast<unsigned>(data[3]), static_cast<unsigned>(data[4]),
                     static_cast<unsigned>(data[5]), static_cast<unsigned>(data[6]),
                     static_cast<unsigned>(data[7]));
    }
    std::ranges::fill(memory.view(header, 128U), std::byte{0});
    memory.write_u32_le(header, state.next_sequence);
    memory.write_u32_le(header + 4U, socket);
    memory.write_u32_le(header + 8U, result);
    if (result == 0U || !save_card_device_) {
        memory.write_u32_le(header + 12U, 0xffffffffU);
    } else {
        for (std::size_t index = 0U; index < data.size(); ++index) {
            memory.write_u8(header + static_cast<std::uint32_t>(index) + 12U,
                            static_cast<std::uint8_t>(data[index]));
        }
    }
    state.next_sequence = state.next_sequence == 0x7fffffffU ? 1U : state.next_sequence + 1U;
    state.next_response_buffer = (state.next_response_buffer + 1U) & 1U;
    memory.write_u32_le(packet, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    pending_dbc_service_1c_payload_.reset();
    const unsigned pages =
        operation == 1U || operation == 2U ? std::min(memory.read_u32_le(request + 16U), 16U) : 0U;
    // A real card spends ~295000 EE cycles per page plus a fixed command cost.
    const std::uint64_t latency =
        static_cast<std::uint64_t>(pages) * 295000U + (operation == 9U ? 590000U : 0U) + 90000U;
    const auto ready =
        !save_card_device_
            ? ee_cycle_count_
            : std::max(ee_cycle_count_ + latency,
                       delayed_save_card_callbacks_.empty()
                           ? std::uint64_t{0}
                           : delayed_save_card_callbacks_.back().ready_ee_cycles);
    auto& queue_target = delayed_save_card_callbacks_;
    if (!save_card_device_) {
        pending_sif_command_callbacks_.push_back(
            {handler, context, 0U, 0U, 0U, 0U, global_pointer, 0U});
        return;
    }
    queue_target.push_back(
        {ready, {handler, context, 0U, 0U, 0U, 0U, global_pointer, 0U}});
}


// RECOVERY: identical except for two unnamed temporaries in the declaration list (one unsigned int
// instead of one unsigned long long); no statement differs.
std::uint32_t EeKernelHle::serve_save_card_operation(std::uint32_t operation,
                                                     GuestAddress request, GuestAddress read_data,
                                                     std::span<std::byte> response_data,
                                                     EeMemory& memory) {
    std::ranges::fill(response_data, std::byte{0});
    save::SaveCardDevice* const device = save_card_device_.get();
    const std::uint32_t first_page = memory.read_u32_le(request + 12U);
    const std::uint32_t page_count = memory.read_u32_le(request + 16U);
    const std::uint32_t page_size = memory.read_u32_le(request + 20U);
    const bool valid_pages = page_size == 512U && page_count != 0U && page_count <= 16U;
    switch (operation) {
    case 4U:
        return device->probe();
    case 3U:
        response_data[0] = std::byte{0};
        response_data[1] = std::byte{2};
        response_data[2] = std::byte{16};
        for (std::size_t index = 0U; index < 4U; ++index) {
            response_data[4U + index] = byte_of(16384U, index * 8U);
        }
        return device->attached_slot().has_value() ? 1U : 0U;
    case 1U: {
        const std::uint32_t bytes = page_count * 512U;
        if (!valid_pages || !memory.contains(read_data, bytes)) {
            return 0U;
        }
        std::vector<std::byte> pages(bytes);
        if (!device->read_pages(first_page, page_count, pages)) {
            return 0U;
        }
        if (memory.is_main_ram_range(read_data, bytes)) {
            memory.write_main_ram_bytes(read_data, pages);
        } else {
            for (std::uint32_t index = 0U; index < bytes; ++index) {
                memory.write_u8(read_data + index, static_cast<std::uint8_t>(pages[index]));
            }
        }
        return 1U;
    }
    case 2U: {
        const std::uint32_t bytes = page_count * 512U;
        if (!valid_pages || bytes + 52U > 0x2090U) {
            return 0U;
        }
        return device->write_pages(first_page, std::as_const(memory).view(request + 52U, bytes)) ? 1U : 0U;
    }
    case 9U:
        return device->erase_block(memory.read_u32_le(request + 44U)) ? 1U : 0U;
    case 10U: {
        const auto clock = device->clock_bcd();
        std::ranges::copy(clock, response_data.begin());
        break;
    }
    case 6U:
    case 12U:
        return device->attached_slot().has_value() ? 1U : 0U;
    default:
        return 0U;
    }
    return 1U;
}


void EeKernelHle::complete_virtual_lg_dev_version_call(const EeSifDmaTransferState& transfer,
                                                   EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0xcU ||
        memory.read_u32_le(transfer.source + 36U) != 0x240U ||
        memory.read_u32_le(transfer.source + 44U) != 0x240U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 63U) != 0U ||
        !memory.contains(response, 576U) ||
        memory.read_u32_le(request + 36U) != 0xf2400U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 0U);
    memory.write_u32_le(response + 4U, 0x10b1b00U);
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_lg_dev_enumerate_call(const EeSifDmaTransferState& transfer,
                                                   EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 1U ||
        memory.read_u32_le(transfer.source + 36U) != 0x240U ||
        memory.read_u32_le(transfer.source + 44U) != 0x240U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 63U) != 0U ||
        !memory.contains(response, 576U) ||
        !memory.contains(response + 112U, 24U) ||
        memory.read_u32_le(request + 36U) != 0xf2400U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 0x80000002U);
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}

void EeKernelHle::complete_virtual_lg_dev_no_device_call(const EeSifDmaTransferState& transfer,
                                                   EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0x12U ||
        memory.read_u32_le(transfer.source + 36U) != 0x240U ||
        memory.read_u32_le(transfer.source + 44U) != 0x240U) {
        return;
    }
    const std::uint32_t request = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t response = memory.read_u32_le(transfer.source + 40U);
    if ((request & 3U) != 0U || !memory.contains(request, 40U) || (response & 63U) != 0U ||
        !memory.contains(response, 576U) ||
        memory.read_u32_le(request + 36U) != 0xf2400U) {
        return;
    }
    if (!signal_semaphore(static_cast<std::int32_t>(memory.read_u32_le(request + 8U)))) {
        return;
    }
    memory.write_u32_le(response, 0U);
    std::ranges::fill(memory.view(response + 112U, 352U), std::byte{0});
    memory.write_u32_le(request, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
}


// RECOVERY: the original's inlined semaphore lookups compare `it != end` first and `end != it` second
// (reverse of this build) and reload the semaphore indices from the optionals instead of reusing
// them; statements are otherwise identical.
void EeKernelHle::complete_virtual_file_io_open_call(const EeSifDmaTransferState& transfer,
                                                     EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 0U ||
        memory.read_u32_le(transfer.source + 36U) != 0x418U ||
        memory.read_u32_le(transfer.source + 44U) < 4U) {
        return;
    }
    if (!pending_file_io_payload_ || pending_file_io_payload_->destination != 0xf1900U ||
        pending_file_io_payload_->size != 0x418U ||
        !memory.contains(pending_file_io_payload_->source, 0x418U)) {
        return;
    }
    const std::uint32_t packet = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t reply = memory.read_u32_le(transfer.source + 40U);
    const auto request = pending_file_io_payload_->source;
    const std::uint32_t result_address = memory.read_u32_le(request + 4U);
    if ((packet & 3U) != 0U || !memory.contains(packet, 40U) || (reply & 3U) != 0U ||
        !memory.contains(reply, 4U) || memory.read_u32_le(packet + 36U) != 0xf1800U ||
        memory.read_u32_le(request + 8U) != 4U || (result_address & 3U) != 0U ||
        !memory.contains(result_address, 4U) || memory.read_u32_le(request + 12U) != 1U ||
        memory.read_u32_le(request + 16U) != 24U ||
        memory.read_u32_le(request + 1044U) > 31U ||
        !guest_string_equals(memory, request + 20U, "rom0:ROMVER", 1024U)) {
        return;
    }
    const auto operation_id = read_i32(memory, request);
    const auto client_id = read_i32(memory, packet + 8U);
    const auto operation_semaphore = semaphore_index(operation_id);
    const auto client_semaphore = semaphore_index(client_id);
    if (!operation_semaphore || !client_semaphore || *operation_semaphore == *client_semaphore) {
        return;
    }
    if (semaphore_at(semaphores_, operation_semaphore).count >=
            semaphore_at(semaphores_, operation_semaphore).max_count ||
        semaphore_at(semaphores_, client_semaphore).count >=
            semaphore_at(semaphores_, client_semaphore).max_count) {
        return;
    }
    ++semaphore_at(semaphores_, operation_semaphore).count;
    ++semaphore_at(semaphores_, client_semaphore).count;
    memory.write_u32_le(reply, 1U);
    memory.write_u32_le(result_address, 2U);
    memory.write_u32_le(packet, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    virtual_romver_open_ = true;
    virtual_romver_offset_ = 0U;
    pending_file_io_payload_.reset();
}


// RECOVERY: same lookup operand-order and optional-reload differences as file_io_open_call.
void EeKernelHle::complete_virtual_file_io_read_call(const EeSifDmaTransferState& transfer,
                                                     EeMemory& memory) {
    // The ROM version string "rom0:ROMVER" reads back (16 bytes, ends in NUL).
    constexpr std::string_view kRomver("0200AC20040614\n\0", 16U);
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 2U ||
        memory.read_u32_le(transfer.source + 36U) != 32U ||
        memory.read_u32_le(transfer.source + 44U) < 4U) {
        return;
    }
    if (!pending_file_io_payload_ || pending_file_io_payload_->destination != 0xf1900U ||
        pending_file_io_payload_->size != 32U ||
        !memory.contains(pending_file_io_payload_->source, 32U) || !virtual_romver_open_ ||
        virtual_romver_offset_ > kRomver.size()) {
        return;
    }
    const std::uint32_t packet = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t reply = memory.read_u32_le(transfer.source + 40U);
    const auto request = pending_file_io_payload_->source;
    const std::uint32_t result_address = memory.read_u32_le(request + 4U);
    const std::uint32_t destination = memory.read_u32_le(request + 16U);
    const std::uint32_t size = memory.read_u32_le(request + 20U);
    const std::size_t count = std::min(kRomver.size() - virtual_romver_offset_, static_cast<std::size_t>(size));
    if ((packet & 3U) != 0U || !memory.contains(packet, 40U) || (reply & 3U) != 0U ||
        !memory.contains(reply, 4U) || memory.read_u32_le(packet + 36U) != 0xf1800U ||
        memory.read_u32_le(request + 8U) != 4U || (result_address & 3U) != 0U ||
        !memory.contains(result_address, 4U) || memory.read_u32_le(request + 12U) != 2U ||
        memory.read_u32_le(request + 28U) > 31U || !memory.contains(destination, count)) {
        return;
    }
    const auto operation_id = read_i32(memory, request);
    const auto client_id = read_i32(memory, packet + 8U);
    const auto operation_semaphore = semaphore_index(operation_id);
    const auto client_semaphore = semaphore_index(client_id);
    if (!operation_semaphore || !client_semaphore || *operation_semaphore == *client_semaphore) {
        return;
    }
    if (semaphore_at(semaphores_, operation_semaphore).count >=
            semaphore_at(semaphores_, operation_semaphore).max_count ||
        semaphore_at(semaphores_, client_semaphore).count >=
            semaphore_at(semaphores_, client_semaphore).max_count) {
        return;
    }
    ++semaphore_at(semaphores_, operation_semaphore).count;
    ++semaphore_at(semaphores_, client_semaphore).count;
    for (std::size_t index = 0U; index < count; ++index) {
        memory.write_u8(destination + static_cast<std::uint32_t>(index),
                        static_cast<std::uint8_t>(kRomver[virtual_romver_offset_ + index]));
    }
    memory.write_u32_le(reply, 1U);
    memory.write_u32_le(result_address, static_cast<std::uint32_t>(count));
    memory.write_u32_le(packet, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    virtual_romver_offset_ += count;
    pending_file_io_payload_.reset();
}


// RECOVERY: same lookup operand-order and optional-reload differences as file_io_open_call.
void EeKernelHle::complete_virtual_file_io_close_call(const EeSifDmaTransferState& transfer,
                                                      EeMemory& memory) {
    if (transfer.destination != 0x000f0000U || transfer.size < 0x38U ||
        !memory.contains(transfer.source, 0x38U) ||
        memory.read_u32_le(transfer.source + 8U) != 0x8000000aU ||
        memory.read_u32_le(transfer.source + 32U) != 1U ||
        memory.read_u32_le(transfer.source + 36U) != 20U ||
        memory.read_u32_le(transfer.source + 44U) < 4U) {
        return;
    }
    if (!pending_file_io_payload_ || pending_file_io_payload_->destination != 0xf1900U ||
        pending_file_io_payload_->size != 20U ||
        !memory.contains(pending_file_io_payload_->source, 20U) || !virtual_romver_open_) {
        return;
    }
    const std::uint32_t packet = memory.read_u32_le(transfer.source + 28U);
    const std::uint32_t reply = memory.read_u32_le(transfer.source + 40U);
    const auto request = pending_file_io_payload_->source;
    const std::uint32_t result_address = memory.read_u32_le(request + 4U);
    if ((packet & 3U) != 0U || !memory.contains(packet, 40U) || (reply & 3U) != 0U ||
        !memory.contains(reply, 4U) || memory.read_u32_le(packet + 36U) != 0xf1800U ||
        memory.read_u32_le(request + 8U) != 4U || (result_address & 3U) != 0U ||
        !memory.contains(result_address, 4U) || memory.read_u32_le(request + 12U) != 2U ||
        memory.read_u32_le(request + 16U) > 31U) {
        return;
    }
    const auto operation_id = read_i32(memory, request);
    const auto client_id = read_i32(memory, packet + 8U);
    const auto operation_semaphore = semaphore_index(operation_id);
    const auto client_semaphore = semaphore_index(client_id);
    if (!operation_semaphore || !client_semaphore || *operation_semaphore == *client_semaphore) {
        return;
    }
    if (semaphore_at(semaphores_, operation_semaphore).count >=
            semaphore_at(semaphores_, operation_semaphore).max_count ||
        semaphore_at(semaphores_, client_semaphore).count >=
            semaphore_at(semaphores_, client_semaphore).max_count) {
        return;
    }
    ++semaphore_at(semaphores_, operation_semaphore).count;
    ++semaphore_at(semaphores_, client_semaphore).count;
    memory.write_u32_le(reply, 1U);
    memory.write_u32_le(result_address, 0U);
    memory.write_u32_le(packet, 0U);
    memory.write_u32_le(transfer.source + 16U, memory.read_u32_le(transfer.source + 16U) & ~1U);
    memory.write_u32_le(transfer.source + 24U, 0U);
    virtual_romver_open_ = false;
    virtual_romver_offset_ = 0U;
    pending_file_io_payload_.reset();
}

// Bank-loader node probe (docs/archive/AUDIO_HISTORY.md increment 18). Scans
// guest RAM for the fe.awd bank pump method-table word, then snapshots the
// loader node's FSM state when it changes. Observation only.
// RECOVERY: statements match; the original keeps the second `if (trace)` test (before the
// [SIF-TBL] block) as a separate branch where GCC threads it here, and has a real `continue`
// where this has a goto.
void EeKernelHle::probe_bank_loader_node(EeMemory& memory) {
    constexpr std::size_t kBankProbeHistory = 64U;
    ++rwa_activity_.bank_probe_scans;

    const std::array<std::byte, 4> kPumpWord{std::byte{0x00U}, std::byte{0xb5U}, std::byte{0x2aU},
                                             std::byte{0x00U}};
    const auto safe_read = [&memory](const GuestAddress address) -> std::uint32_t {
        return memory.contains(address, 4U) ? memory.read_u32_le(address) : 0U;
    };
    struct ScanWindow {
        GuestAddress base;
        std::size_t span;
    };
    const std::array<ScanWindow, 1> kWindows{{{0U, 0x2000000U}}};

    const bool trace = std::getenv("TAKEDOWN_RWA_OPS") != nullptr;

    // The record tag (0x3080f) marks every bank record; the lowest and highest
    // tagged addresses bound where bank containers can sit.
    const std::span<const std::byte> census_view =
        memory.contains(0x1e00000U, 0x100000U)
            ? std::as_const(memory).view(0x1e00000U, 0x100000U)
            : std::span<const std::byte>{};
    std::uint32_t records_lo = 0xffffffffU;
    std::uint32_t records_hi = 0U;
    for (std::size_t offset = 0U; offset + 4U <= census_view.size(); offset += 4U) {
        if ((std::to_integer<int>(census_view[offset]) |
             (std::to_integer<int>(census_view[offset + 1U]) << 8) |
             (std::to_integer<int>(census_view[offset + 2U]) << 16) |
             (std::to_integer<int>(census_view[offset + 3U]) << 24)) == 0x3080f) {
            records_lo = std::min(records_lo, static_cast<std::uint32_t>(offset) + 0x1e00000U);
            records_hi = std::max(records_hi, static_cast<std::uint32_t>(offset) + 0x1e00080U);
        }
    }

    for (const ScanWindow& window : kWindows) {
        if (!memory.contains(window.base, window.span)) {
            continue;
        }
        const std::span<const std::byte> heap =
            std::as_const(memory).view(window.base, window.span);
        auto cursor = heap.begin();
        std::size_t scanned = 0U;
        while (scanned != 16U) {
            const auto hit = std::search(cursor, heap.end(), kPumpWord.begin(), kPumpWord.end());
            if (hit == heap.end()) {
                break;
            }
            const auto node = window.base + static_cast<std::uint32_t>(hit - heap.begin()) - 48U;
            cursor = hit + 1;
            ++scanned;

            if (trace) {
                const std::span<const std::byte> heap_words =
                    std::as_const(memory).view(0x1e00000U, 0x100000U);
                std::array<std::uint32_t, 6> record_tag_addrs{};
                std::size_t record_tags = 0U;
                std::size_t dict_tags = 0U;
                for (std::size_t offset = 0U; offset + 4U <= heap_words.size(); offset += 4U) {
                    const std::uint32_t word = static_cast<std::uint32_t>(
                        std::to_integer<int>(heap_words[offset]) |
                        (std::to_integer<int>(heap_words[offset + 1U]) << 8) |
                        (std::to_integer<int>(heap_words[offset + 2U]) << 16) |
                        (std::to_integer<int>(heap_words[offset + 3U]) << 24));
                    if (word == 0x3080fU) {
                        if (record_tags <= 5U) {
                            record_tag_addrs[record_tags] = static_cast<std::uint32_t>(offset) + 0x1e00000U;
                        }
                        ++record_tags;
                    }
                    if (word == 0x30809U) {
                        ++dict_tags;
                    }
                }
                std::fprintf(stderr,
                             "[RWA-BANK-SCAN] cmd=%llu node=0x%06x tags{%zu records@[%08x %08x %08x %08x], "
                             "%zu dict}\n",
                             rwa_activity_.command_packets, node, record_tags, record_tag_addrs[0],
                             record_tag_addrs[1], record_tag_addrs[2], record_tag_addrs[3], dict_tags);
            }

            EeRwaActivity::BankProbe probe{};
            probe.at_command = rwa_activity_.command_packets;
            probe.node_address = node;
            const auto records_lo_local = static_cast<unsigned int>(records_lo);
            const auto records_hi_local =
                records_hi != 0U ? records_hi : records_lo_local + 128U;
            const std::span<const std::byte> heap_scan =
                memory.contains(0x1e00000U, 0x100000U)
                    ? std::as_const(memory).view(0x1e00000U, 0x100000U)
                    : std::span<const std::byte>{};
            GuestAddress bank = 0U;
            for (std::size_t offset = 0U; offset + 4U <= heap_scan.size(); offset += 4U) {
                const std::uint32_t word = static_cast<std::uint32_t>(
                    std::to_integer<int>(heap_scan[offset]) |
                    (std::to_integer<int>(heap_scan[offset + 1U]) << 8) |
                    (std::to_integer<int>(heap_scan[offset + 2U]) << 16) |
                    (std::to_integer<int>(heap_scan[offset + 3U]) << 24));
                if (records_lo_local <= word && records_hi_local > word) {
                    const auto container = static_cast<std::uint32_t>(offset) + 0x1e00000U - 152U;
                    if (memory.contains(container, 176U)) {
                        bank = container;
                        break;
                    }
                }
            }

            if (bank != 0U) {
                probe.bank_state = safe_read(bank + 104U);
                probe.flags = safe_read(bank + 140U);
                probe.records = safe_read(bank + 152U);
                probe.chunk_count = safe_read(bank + 156U);
                probe.chunk_size = safe_read(bank + 160U);
                probe.resume = safe_read(bank + 168U);
                const GuestAddress spu = safe_read(bank + 56U);
                if (memory.contains(spu, 8U)) {
                    probe.spu_base = safe_read(spu + 4U);
                }
            } else {
                probe.bank_state = safe_read(node + 104U);
            }

            if (trace) {
                if (rwa_activity_.bank_probe_history_count == 0U && bank != 0U) {
                    std::fprintf(stderr, "[RWA-BANK-DUMP] bank=0x%08x\n", bank);
                    for (std::uint32_t offset = 0U; offset != 192U; offset += 4U) {
                        std::fprintf(stderr, "[RWA-BANK-DUMP] +%03x=0x%08x\n", offset,
                                     safe_read(bank + offset));
                    }
                    if (probe.records != 0U) {
                        for (std::uint32_t offset = 16U; offset != 112U; offset += 4U) {
                            std::fprintf(stderr, "[RWA-BANK-DUMP] rec-%02x=0x%08x\n", offset,
                                         safe_read(probe.records - offset));
                        }
                    }
                }
                const std::uint32_t reg = safe_read(0x1e828d0U);
                const std::uint32_t xport_node = safe_read(reg);
                const std::uint32_t node30 = safe_read(xport_node + 48U);
                std::fprintf(stderr,
                             "[RWA-XPORT] reg=0x%08x entry=0x%08x node=0x%08x node30=0x%08x\n", reg,
                             reg, xport_node, node30);
            }

            if (trace) {
                for (std::uint32_t index = 0U; index != 6U; ++index) {
                    const auto entry = 0x49fe60U + index * 112U;
                    std::fprintf(stderr, "[SIF-TBL] e%u +0=0x%08x +1c=0x%08x\n", index,
                                 safe_read(entry), safe_read(entry + 28U));
                }
                std::fprintf(stderr,
                             "[RWA-GATE] 0x4E2C38=0x%08x cursors{r=0x%08x w=0x%08x} sema=0x%08x "
                             "latch=0x%08x\n",
                             safe_read(0x4e2c38U), safe_read(0x4e2c3cU), safe_read(0x4e2c40U),
                             safe_read(0x4e2c54U), safe_read(0x4e2c28U));
                const GuestAddress bank2 = 0x1eefe80U;
                std::fprintf(stderr,
                             "[RWA-BANK2] bank{+100=0x%08x +104=0x%08x +96=0x%08x +64=0x%08x}\n",
                             safe_read(bank2 + 100U), safe_read(bank2 + 104U), safe_read(bank2 + 96U),
                             safe_read(bank2 + 64U));
            }

            probe.loader_ptr = bank;
            probe.window_ptr = safe_read(0x1e82a00U);
            probe.window_log2 = safe_read(0x1e83218U);
            probe.window_base = safe_read(0x1e8321cU);
            probe.window_stride = safe_read(0x1e83224U);
            probe.in_flight = safe_read(0x4e2cf4U);

            // The newest earlier snapshot of this node decides: unchanged
            // state is skipped, any change is recorded.
            const std::size_t recent =
                std::min<std::size_t>(rwa_activity_.bank_probe_history_count, kBankProbeHistory);
            for (std::size_t back = 0U; back < recent; ++back) {
                const auto& prior = rwa_activity_.bank_probe_history
                    [(rwa_activity_.bank_probe_history_count + kBankProbeHistory - 1U - back) %
                     kBankProbeHistory];
                if (prior.node_address != node) {
                    continue;
                }
                if (prior.bank_state == probe.bank_state && prior.flags == probe.flags &&
                    prior.records == probe.records && prior.chunk_count == probe.chunk_count &&
                    prior.spu_base == probe.spu_base && prior.in_flight == probe.in_flight) {
                    goto next_hit;
                }
                break;
            }
            if (rwa_activity_.bank_probe_history_count >= 256U) {
                break;
            }
            rwa_activity_.bank_probe_history[rwa_activity_.bank_probe_history_count %
                                             kBankProbeHistory] = probe;
            ++rwa_activity_.bank_probe_history_count;
            if (std::getenv("TAKEDOWN_RWA_OPS") != nullptr) {
                std::fprintf(stderr,
                             "[RWA-BANK] cmd=%llu node=0x%06x state=%u loader=0x%06x flags=0x%x "
                             "records=0x%06x count=%u size=0x%x spu_base=0x%06x win{ptr=0x%06x log2=%u "
                             "base=0x%06x stride=0x%x} inflight=%u\n",
                             probe.at_command, node, probe.bank_state, bank, probe.flags, probe.records,
                             probe.chunk_count, probe.chunk_size, probe.spu_base, probe.window_ptr,
                             probe.window_log2, probe.window_base, probe.window_stride, probe.in_flight);
            }
        next_hit:;
        }
    }
}

void EeKernelHle::complete_virtual_rwa_command(const EeSifDmaTransferState& transfer,
                                               EeMemory& memory, const R5900State& cpu) {
    const auto packet_word = [&](const std::uint32_t offset) {
        return memory.read_u32_le(transfer.source + offset);
    };
    if (transfer.destination != 0xf0000U || transfer.size != 0x30U ||
        transfer.attributes != 0x44U || !memory.contains(transfer.source, 0x30U) ||
        (memory.read_u32_le(transfer.source) & 0xffU) != 0x30U ||
        packet_word(8U) != 0U) {
        return;
    }
    const auto header = memory.read_u32_le(transfer.source);
    const auto opcode = memory.read_u32_le(transfer.source + 16U);

    // Earlier SIF DMA batches complete before this command is observed.
    std::vector<std::int32_t> earlier_batches;
    for (const auto& batch : incomplete_sif_dma_batches_) {
        if (batch.id != transfer.id) {
            earlier_batches.push_back(batch.id);
        }
    }
    for (const int id : earlier_batches) {
        complete_sif_dma_batch(memory, id);
    }

    ++rwa_activity_.command_packets;
    ++rwa_activity_.opcode_counts[opcode & 0xffU];
    for (std::size_t word = 0; word < 12U; ++word) {
        rwa_activity_.last_packet_words[opcode & 0xffU][word] =
            memory.read_u32_le(transfer.source + static_cast<std::uint32_t>(word) * 4U);
    }
    rwa_activity_.last_opcode = opcode;
    static const bool bank_probe_enabled = std::getenv("TAKEDOWN_RWA_OPS") != nullptr;
    if (bank_probe_enabled && ((rwa_activity_.command_packets & 15U) == 0U ||
                               (opcode & 0xffU) == 5U || (opcode & 0xffU) == 0x46U)) {
        probe_bank_loader_node(memory);
    }
    if (std::getenv("TAKEDOWN_RWA_OPS") != nullptr) {
        std::fprintf(stderr, "[RWA-OP %.3f] op=0x%02x",
                     std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count(),
                     opcode & 0xffU);
        for (std::size_t word = 0; word < 12U; ++word) {
            if (rwa_activity_.last_packet_words[opcode & 0xffU][word] != 0U) {
                std::fprintf(stderr, " +%02zx=%08x", word * 4U,
                             rwa_activity_.last_packet_words[opcode & 0xffU][word]);
            }
        }
        std::fprintf(stderr, "\n");
    }
    if (rwa_stream_journal_ && (opcode & 0xffU) != 0x32U) {
        rwa_stream_journal_({
            .sequence = rwa_activity_.command_packets, .slot = opcode, .detail = 98U});
    }

    EeRwaCommandRecord recent{};
    recent.sequence = rwa_activity_.command_packets;
    recent.sif_dma_id = transfer.id;
    recent.packet_address = transfer.source;
    recent.syscall_pc = cpu.pc();
    recent.syscall_ra = cpu.read_gpr_u32(31U);
    recent.packet_words = rwa_activity_.last_packet_words[opcode & 0xffU];
    // The retail sceSifSetDma wrapper (0x10c1e4, called from 0x10e5e4) keeps
    // its caller's return address in the frame above the stack pointer.
    const std::uint32_t stack_pointer = cpu.read_gpr_u32(29U);
    if (recent.syscall_pc == 0x10c1e4U && recent.syscall_ra == 0x10e5e4U &&
        stack_pointer <= 0xffffff7fU && memory.contains(stack_pointer + 128U, 4U)) {
        const std::uint32_t return_address = memory.read_u32_le(stack_pointer + 128U);
        if (return_address > 7U && (return_address & 3U) == 0U) {
            recent.sender_callsite = return_address - 8U;
        }
    }
    if (rwa_activity_.recent_commands.size() == 32U) {
        rwa_activity_.recent_commands.erase(rwa_activity_.recent_commands.begin());
    }
    rwa_activity_.recent_commands.push_back(recent);

    bool staging_pair_rejected = false;
    bool native_voice_snapshot_dma_valid = false;
    const auto inbound_extra_size = header >> 8U;
    if (inbound_extra_size != 0U && sif_dma_transfers_.size() > 1U) {
        const EeSifDmaTransferState& packet_transfer = sif_dma_transfers_.back();
        const EeSifDmaTransferState& extra_transfer =
            sif_dma_transfers_[sif_dma_transfers_.size() - 2U];
        auto& words = rwa_activity_.last_packet_words[opcode & 0xffU];
        const auto extra_source = words[6];
        const auto semantic_destination = words[7];
        const auto physical_destination = opcode == 7U ? words[1] : semantic_destination;
        const bool declared_size_matches = opcode == 7U
                                               ? std::min<std::uint32_t>(words[8], 0x200000U) ==
                                                     inbound_extra_size
                                               : words[8] == inbound_extra_size;
        const bool staging_pair_valid =
            packet_transfer.id == transfer.id && packet_transfer.source == transfer.source &&
            extra_transfer.id == transfer.id && extra_transfer.source == extra_source &&
            extra_transfer.destination == physical_destination &&
            extra_transfer.size == inbound_extra_size && extra_transfer.attributes == 0U &&
            declared_size_matches && memory.contains(extra_source, inbound_extra_size) &&
            inbound_extra_size <= 0x200000U &&
            0x200000U - inbound_extra_size >= physical_destination &&
            virtual_iop_ram_.size() == 0x200000U && virtual_iop_ram_valid_.size() == 0x200000U &&
            std::all_of(virtual_iop_ram_valid_.begin() + physical_destination,
                        virtual_iop_ram_valid_.begin() + physical_destination + inbound_extra_size,
                        [](const std::uint8_t valid) { return valid != 0U; });
        if (staging_pair_valid) {
            staging_pair_rejected = true;
            if (opcode == 7U && transfer.attributes == 0x44U &&
                0x200000U - inbound_extra_size >= semantic_destination &&
                ~inbound_extra_size >= extra_source) {
                PendingRwaStagingTransfer pending{transfer.id,        physical_destination,
                                                  semantic_destination, extra_source,
                                                  inbound_extra_size,   words[8]};
                for (std::size_t byte = 0; byte < pending.response_record.size(); ++byte) {
                    pending.response_record[byte] =
                        memory.read_u8(transfer.source + static_cast<GuestAddress>(byte));
                }
                pending.bytes.assign(virtual_iop_ram_.begin() + physical_destination,
                                     virtual_iop_ram_.begin() + physical_destination +
                                         inbound_extra_size);
                rwa_activity_.last_op07_staging_address = physical_destination;
                rwa_activity_.last_op07_spu_destination = semantic_destination;
                rwa_activity_.last_op07_chunk_size = inbound_extra_size;
                rwa_activity_.last_op07_remaining_size = words[8];
                rwa_activity_.last_op07_fnv1a64 = 0U;
                if (rwa_activity_.recent_op07_staging_transfers.size() == 512U) {
                    rwa_activity_.recent_op07_staging_transfers.erase(
                        rwa_activity_.recent_op07_staging_transfers.begin());
                }
                rwa_activity_.recent_op07_staging_transfers.push_back(recent);
                if (rwa_stream_journal_) {
                    rwa_stream_journal_({
                        .sequence = rwa_activity_.command_packets,
                        .slot = 7U,
                        .byte_count = inbound_extra_size,
                        .destination = semantic_destination,
                        .detail = 94U});
                }
                pending_rwa_staging_transfers_.emplace_back(std::move(pending));
                staging_pair_rejected = false;
            }
            if (opcode == 20U && packet_transfer.destination == transfer.destination &&
                packet_transfer.size == transfer.size && packet_transfer.attributes == 0x44U) {
                native_voice_snapshot_dma_valid = true;
            }
        } else if (opcode == 7U) {
            staging_pair_rejected = true;
        }
    }
    if (staging_pair_rejected) {
        ++rwa_activity_.op07_staging_transfer_rejected;
    }

    if (rwa_command_packet_observer_ && (opcode != 20U || native_voice_snapshot_dma_valid)) {
        std::array<std::byte, 0x30U> packet{};
        for (std::size_t byte = 0; byte < packet.size(); ++byte) {
            packet[byte] = static_cast<std::byte>(
                memory.read_u8(transfer.source + static_cast<GuestAddress>(byte)));
        }
        rwa_command_packet_observer_(packet);
    }
    const std::optional<EeRwaShadowResponse> shadow_response =
        rwa_shadow_response_provider_ && (opcode != 20U || native_voice_snapshot_dma_valid)
            ? rwa_shadow_response_provider_(opcode)
            : std::nullopt;

    switch (opcode) {
    case 0x14U: {
        const auto shadow_source = rwa_activity_.last_packet_words[opcode & 0xffU][6];
        const auto shadow_size = rwa_activity_.last_packet_words[opcode & 0xffU][8];
        constexpr std::size_t kSnapshotBytes = 0x680U;
        if (native_voice_snapshot_dma_valid && shadow_response && shadow_response->opcode == 20U &&
            shadow_response->extra_data.size() == kSnapshotBytes) {
            if (memory.contains(shadow_response->extra_destination, kSnapshotBytes)) {
                for (std::size_t byte = 0; byte < kSnapshotBytes; ++byte) {
                    memory.write_u8(shadow_response->extra_destination +
                                        static_cast<GuestAddress>(byte),
                                    static_cast<std::uint8_t>(shadow_response->extra_data[byte]));
                }
                ++rwa_activity_.voice_snapshots_returned;
                rwa_activity_.voice_snapshot_return_bytes += kSnapshotBytes;
                rwa_activity_.last_voice_snapshot_destination = shadow_response->extra_destination;
                pending_sif_command_callbacks_.push_back(
                    {0x002b3b50U, 0U, 0x14U, 0U, 0U, 0U, 0U, 0U});
            }
            break;
        }
        if (shadow_size == kSnapshotBytes && native_voice_snapshot_dma_valid &&
            memory.contains(shadow_source, kSnapshotBytes) &&
            rwa_voice_snapshot_destination_.has_value() &&
            memory.contains(*rwa_voice_snapshot_destination_, kSnapshotBytes)) {
            std::array<std::uint8_t, kSnapshotBytes> response{};
            for (std::size_t byte = 0; byte < kSnapshotBytes; ++byte) {
                response[byte] = memory.read_u8(shadow_source + static_cast<GuestAddress>(byte));
            }
            std::vector<EeRwaVoicePlay> parameter_updates;
            std::vector<EeRwaVoicePlay> key_offs;
            std::vector<EeRwaVoicePlay> key_off_loop_resets;
            std::vector<EeRwaVoicePlay> key_ons;
            std::uint32_t pending_this_commit = 0U;
            for (std::uint32_t slot = 0U; slot != 48U; ++slot) {
                const auto offset = static_cast<std::size_t>(slot) * 32U;
                unsigned int state = load_le32(response, offset);
                if (state <= 7U) {
                    ++rwa_activity_.voice_state_histogram[state];
                    if (state == 2U) {
                        ++pending_this_commit;
                        rwa_activity_.voice_pending_max =
                            std::max(pending_this_commit, rwa_activity_.voice_pending_max);
                    }
                }
                const auto core = static_cast<std::uint8_t>(slot / 24U);
                const auto voice = static_cast<std::uint8_t>(slot % 24U);
                const auto ssa = load_le32(response, offset + 4U);
                const auto loop_address = load_le32(response, offset + 8U);
                const auto source_rate = load_le32(response, offset + 12U);
                const auto packed_volumes = load_le32(response, offset + 16U);
                const auto requested_position = load_le32(response, offset + 24U);
                const auto flags = load_le32(response, offset + 28U);
                const int raw_left = static_cast<std::int16_t>(packed_volumes >> 16U);
                const int raw_right = static_cast<int>(packed_volumes & 0xffffU);
                int requested_left = raw_left;
                if ((flags & 4U) != 0U) {
                    requested_left = -raw_left;
                }
                int requested_right = raw_right;
                if ((flags & 2U) != 0U) {
                    requested_right = -raw_right;
                }
                auto& volume_target = mixer_slot_volume_targets_[slot];
                if (state != 2U && state != 4U) {
                    volume_target.active = false;
                }
                if (ssa == 0U && state == 2U) {
                    state = 1U;
                    store_le32(response, offset, 1U);
                }
                const bool rate_changed = mixer_slot_rates_[slot] != source_rate;
                mixer_slot_rates_[slot] = source_rate;
                const bool can_arm = state == 2U && ssa > 0x500fU;
                if (state == 2U && !can_arm) {
                    volume_target.active = false;
                }
                if (state != 5U && rate_changed && !can_arm) {
                    parameter_updates.push_back({EeRwaVoiceAction::SetPitch, core, voice,
                                                 spu_pitch_for_rate(source_rate), 0U, 0U, 0U, 0U,
                                                 0U, 0U});
                }
                if (state == 1U || state == 2U) {
                    key_offs.push_back({EeRwaVoiceAction::KeyOff, core, voice, 0U, 0U, 0U, 0U, 0U,
                                        0U, 0U});
                }
                std::optional<EeRwaVoiceStatus> status;
                if (state != 2U && state != 3U && rwa_voice_status_observer_) {
                    status = rwa_voice_status_observer_(core, voice);
                }
                if (can_arm) {
                    const auto start_offset = (requested_position << 4U) / 28U;
                    const auto effective_start = start_offset + ssa;
                    volume_target.left = {raw_left, raw_left, (flags & 4U) != 0U,
                                          (flags & 4U) != 0U, false};
                    volume_target.right = {raw_right, raw_right, (flags & 2U) != 0U,
                                           (flags & 2U) != 0U, false};
                    volume_target.force_zero = false;
                    volume_target.active = true;
                    key_ons.push_back(
                        {EeRwaVoiceAction::KeyOn, core, voice, spu_pitch_for_rate(source_rate),
                         effective_start,
                         (flags & 1U) != 0U ? loop_address : 0x5010U,
                         static_cast<std::uint16_t>((requested_left >> 1) & 0x7fff),
                         static_cast<std::uint16_t>((requested_right >> 1) & 0x7fff),
                         static_cast<std::uint16_t>(requested_position == 0U ? 0xffU : 0x1bffU),
                         7U});
                    if (rwa_voice_play_probe_) {
                        rwa_voice_play_probe_(effective_start);
                    }
                    if ((flags & 1U) == 0U) {
                        store_le32(response, offset + 8U, 0x5000U);
                    }
                } else if (state == 4U && status.has_value()) {
                    volume_target.left.desired_target = raw_left;
                    volume_target.left.desired_negative = (flags & 4U) != 0U;
                    volume_target.right.desired_target = raw_right;
                    volume_target.right.desired_negative = (flags & 2U) != 0U;
                    volume_target.force_zero = false;
                    volume_target.active = true;
                    if (status->envelope == 0U) {
                        key_offs.push_back({EeRwaVoiceAction::KeyOff, core, voice, 0U, 0U, 0U, 0U,
                                            0U, 0U, 0U});
                        state = 1U;
                        store_le32(response, offset, 1U);
                        volume_target.active = false;
                    } else if ((flags & 1U) == 0U) {
                        parameter_updates.push_back({EeRwaVoiceAction::SetLoopAddress, core, voice,
                                                     0U, 0U, 0x5010U, 0U, 0U, 0U, 0U});
                    }
                }
                if ((state == 1U || state == 2U) && !can_arm) {
                    store_le32(response, offset + 8U, 0x5000U);
                    key_off_loop_resets.push_back({EeRwaVoiceAction::SetLoopAddress, core, voice,
                                                   0U, 0U, 0x5000U, 0U, 0U, 0U, 0U});
                }
                if (state != 3U) {
                    if (state == 2U) {
                        store_le32(response, offset + 20U, requested_position);
                    } else if (status.has_value()) {
                        int byte_delta = static_cast<int>(status->current_address - ssa);
                        const int scaled = static_cast<int>(
                            static_cast<std::uint32_t>(byte_delta > 0 ? byte_delta : 0) * 28U);
                        store_le32(response, offset + 20U, static_cast<std::uint32_t>(scaled / 16));
                    }
                    if (state == 2U) {
                        store_le32(response, offset, 4U);
                    }
                }
            }
            if (rwa_voice_play_observer_) {
                for (const EeRwaVoicePlay& play : parameter_updates) {
                    rwa_voice_play_observer_(play);
                }
                for (const EeRwaVoicePlay& play : key_offs) {
                    rwa_voice_play_observer_(play);
                }
                for (const EeRwaVoicePlay& play : key_off_loop_resets) {
                    rwa_voice_play_observer_(play);
                }
                for (const EeRwaVoicePlay& play : key_ons) {
                    rwa_voice_play_observer_(play);
                }
            }
            const auto response_destination = *rwa_voice_snapshot_destination_;
            for (std::size_t byte = 0; byte < kSnapshotBytes; ++byte) {
                memory.write_u8(response_destination + static_cast<GuestAddress>(byte),
                                response[byte]);
            }
            ++rwa_activity_.voice_snapshots_returned;
            rwa_activity_.voice_snapshot_return_bytes += kSnapshotBytes;
            rwa_activity_.last_voice_snapshot_destination = response_destination;
            pending_sif_command_callbacks_.push_back(
                {0x002b3b50U, 0U, 0x14U, 0U, 0U, 0U, 0U, 0U});
            if (pending_this_commit != 0U) {
                ++rwa_activity_.voice_pending_commits;
            }
            break;
        }
        if (native_voice_snapshot_dma_valid && rwa_shadow_response_provider_) {
            ++rwa_activity_.voice_snapshots_deferred_pending;
        }
        break;
    }
    case 0x05U: {
        if (rwa_activity_.recent_op05_voice_transfers.size() == 512U) {
            rwa_activity_.recent_op05_voice_transfers.erase(
                rwa_activity_.recent_op05_voice_transfers.begin());
        }
        rwa_activity_.recent_op05_voice_transfers.push_back(recent);
        const auto iop_source = rwa_activity_.last_packet_words[opcode & 0xffU][6];
        const auto spu_destination = rwa_activity_.last_packet_words[opcode & 0xffU][7];
        const auto size = rwa_activity_.last_packet_words[opcode & 0xffU][8];
        rwa_activity_.last_op05_iop_source = iop_source;
        rwa_activity_.last_op05_spu_destination = spu_destination;
        rwa_activity_.last_op05_size = size;
        rwa_activity_.last_op05_fnv1a64 = 0U;
        PendingRwaOp05VoiceTransfer pending{transfer.id,
                                            iop_source,
                                            spu_destination,
                                            size,
                                            rwa_activity_.last_packet_words[opcode & 0xffU][5],
                                            rwa_activity_.last_packet_words[opcode & 0xffU][9],
                                            rwa_activity_.last_packet_words[opcode & 0xffU][10]};
        for (std::size_t byte = 0; byte < pending.response_record.size(); ++byte) {
            pending.response_record[byte] =
                memory.read_u8(transfer.source + static_cast<GuestAddress>(byte));
        }
        pending_rwa_op05_voice_transfers_.emplace_back(pending);
        break;
    }
    case 0x32U: {
        // Stream-interface request: a 112-byte stream object travels with the
        // command packet as its extra SIF DMA.
        if (inbound_extra_size != 112U || sif_dma_transfers_.size() <= 1U) {
            if (rwa_stream_journal_) {
                rwa_stream_journal_({
                    .sequence = rwa_activity_.command_packets,
                    .byte_count = inbound_extra_size,
                    .phase = EeRwaStreamJournalEvent::Phase::Dropped,
                    .detail = 11U});
            }
            break;
        }
        const EeSifDmaTransferState& packet_transfer = sif_dma_transfers_.back();
        const EeSifDmaTransferState& extra_transfer =
            sif_dma_transfers_[sif_dma_transfers_.size() - 2U];
        const std::uint32_t stream_object = packet_word(24U);
        const std::uint32_t stream_destination = packet_word(28U);
        const std::uint32_t stream_size = packet_word(32U);
        const unsigned int slot = packet_word(36U);
        const unsigned int stream_state = memory.read_u32_le(stream_object + 16U);
        if (packet_transfer.id != transfer.id || packet_transfer.source != transfer.source ||
            packet_transfer.destination != transfer.destination ||
            packet_transfer.size != transfer.size || extra_transfer.id != transfer.id ||
            extra_transfer.source != stream_object ||
            extra_transfer.destination != stream_destination || extra_transfer.size != 112U ||
            stream_size != 112U || extra_transfer.attributes != 0U ||
            !memory.contains(stream_object, 112U) || slot > 2U ||
            slot != memory.read_u32_le(stream_object) || stream_state - 1U > 3U) {
            if (rwa_stream_journal_) {
                rwa_stream_journal_({
                    .sequence = rwa_activity_.command_packets,
                    .slot = slot,
                    .state = stream_state,
                    .byte_count = inbound_extra_size,
                    .phase = EeRwaStreamJournalEvent::Phase::Dropped,
                    .detail = 12U});
            }
            break;
        }
        // Stream events carry the sequence number, the slot and the stream state.
        const auto journal = [&](EeRwaStreamJournalEvent event) {
            if (!rwa_stream_journal_) {
                return;
            }
            event.sequence = rwa_activity_.command_packets;
            event.slot = slot;
            event.state = stream_state;
            rwa_stream_journal_(event);
        };
        if (rwa_stream_journal_) {
            std::optional<std::string> journal_path =
                read_guest_string(memory, stream_object + 36U, 76U);
            journal(EeRwaStreamJournalEvent{.flags = memory.read_u32_le(stream_object + 28U),
                                            .seek = memory.read_u32_le(stream_object + 12U),
                                            .byte_count = memory.read_u32_le(stream_object + 8U),
                                            .destination = memory.read_u32_le(stream_object + 4U),
                                            .path = journal_path.value_or("")});
        }
        const std::uint32_t destination = memory.read_u32_le(stream_object + 4U);
        const std::uint32_t requested_bytes = memory.read_u32_le(stream_object + 8U);
        const std::uint32_t completion_token = memory.read_u32_le(stream_object + 20U);
        VirtualRwaFile& stream_file = virtual_rwa_files_[slot];
        if (stream_state == 1U) {
            // Close.
            if (completion_token == 0U || !stream_file.open) {
                journal(EeRwaStreamJournalEvent{.phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 1U});
                break;
            }
            const auto closed_guest_path = stream_file.guest_path;
            ++gtfs_activity_.close_requests;
            ++gtfs_activity_.successful_closes;
            if (stream_file.activity_index.has_value() &&
                *stream_file.activity_index < gtfs_open_records_.size()) {
                gtfs_open_records_[*stream_file.activity_index].closed = true;
            }
            stream_file = VirtualRwaFile{};
            pending_sif_command_callbacks_.push_back(
                {0x002b3b50U, 0U, 0x33U, 0U, slot, completion_token, 0U, 32U});
            journal(EeRwaStreamJournalEvent{.path = closed_guest_path, .phase = EeRwaStreamJournalEvent::Phase::Completed, .detail = 1U});
            break;
        }
        if (stream_state == 4U) {
            // Seek.
            if (completion_token == 0U || !stream_file.open) {
                journal(EeRwaStreamJournalEvent{.phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 2U});
                break;
            }
            const std::uint32_t seek_offset = memory.read_u32_le(stream_object + 12U);
            ++gtfs_activity_.seek_requests;
            if (!stream_file.activity_index.has_value() ||
                *stream_file.activity_index >= gtfs_open_records_.size()) {
                journal(EeRwaStreamJournalEvent{.phase = EeRwaStreamJournalEvent::Phase::Dropped,
                                                .detail = 3U});
                break;
            }
            auto& record = gtfs_open_records_[*stream_file.activity_index];
            ++record.seek_requests;
            stream_file.offset = seek_offset;
            ++gtfs_activity_.successful_seeks;
            ++record.successful_seeks;
            pending_sif_command_callbacks_.push_back(
                {0x002b3b50U, 0U, 0x33U, seek_offset, slot, completion_token, 0U, 32U});
            journal(EeRwaStreamJournalEvent{.file_offset = seek_offset,
                                            .phase = EeRwaStreamJournalEvent::Phase::Completed,
                                            .detail = 1U});
            break;
        }
        // Read (state 2: into EE memory, state 3: sector reads into IOP RAM).
        const std::uint32_t flags = memory.read_u32_le(stream_object + 28U);
        if (stream_state == 3U && (flags & 8U) != 0U) {
            stream_file.offset = memory.read_u32_le(stream_object + 12U);
            if (stream_file.offset >= stream_file.size) {
                journal(EeRwaStreamJournalEvent{.flags = flags, .seek = memory.read_u32_le(stream_object + 12U), .phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 13U});
                break;
            }
        }
        unsigned int byte_count = requested_bytes;
        if (stream_state == 3U) {
            if (byte_count > 0xfffff800U) {
                journal(EeRwaStreamJournalEvent{.byte_count = requested_bytes, .phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 5U});
                break;
            }
            byte_count = (requested_bytes + 2047U) & 0xfffff800U;
        }
        const auto guest_path = read_guest_string(memory, stream_object + 36U, 76U);
        const bool destination_valid =
            stream_state == 2U
                ? memory.contains(destination, byte_count)
                : (destination > 0xfffffU && byte_count <= 0x100000U &&
                   0x200000U - byte_count >= destination);
        if (byte_count == 0U || completion_token == 0U || !destination_valid ||
            (!stream_file.open && (!guest_path.has_value() || disc_root_.empty()))) {
            journal(EeRwaStreamJournalEvent{.byte_count = byte_count, .path = guest_path.value_or(""), .phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 6U});
            break;
        }
        if (!stream_file.open) {
            ++gtfs_activity_.open_requests;
            std::filesystem::path host_path;
            std::error_code path_error;
            const std::optional<std::filesystem::path> resolved =
                content::resolve_game_path(disc_root_, mod_overlay_, *guest_path, path_error);
            std::uint32_t file_size = 0U;
            if (resolved.has_value() && !path_error) {
                if (std::filesystem::is_regular_file(*resolved, path_error)) {
                    const unsigned long long host_size = std::filesystem::file_size(*resolved, path_error);
                    if (!path_error && host_size <= 0xffffffffU) {
                        host_path = *resolved;
                        file_size = static_cast<std::uint32_t>(host_size);
                    }
                }
            }
            const std::size_t record_index = gtfs_open_records_.size();
            gtfs_open_records_.push_back({.guest_path = *guest_path,
                                         .host_path = host_path,
                                         .handle = host_path.empty() ? 0xffffffffU : slot,
                                         .file_size = file_size,
                                         .opened = !host_path.empty()});
            if (host_path.empty()) {
                journal(EeRwaStreamJournalEvent{.path = *guest_path, .phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = 7U});
                break;
            }
            stream_file = VirtualRwaFile{*guest_path, host_path, file_size, 0U, record_index, true};
            ++gtfs_activity_.successful_opens;
        }
        ++gtfs_activity_.read_requests;
        const unsigned long long sector_count = (static_cast<unsigned long long>(byte_count) + 2047U) >> 11U;
        gtfs_activity_.sectors_requested += sector_count;
        if (!stream_file.activity_index.has_value() ||
            *stream_file.activity_index >= gtfs_open_records_.size()) {
            break;
        }
        auto& record = gtfs_open_records_[*stream_file.activity_index];
        ++record.read_requests;
        record.sectors_requested += sector_count;
        const unsigned long long read_start = stream_file.offset;
        const unsigned long long remaining =
            stream_file.size > read_start ? stream_file.size - read_start : 0U;
        const unsigned long long host_bytes =
            std::min<unsigned long long>(byte_count, remaining);
        std::vector<std::byte> bytes(byte_count);
        if (host_file_reader_ == nullptr) {
            host_file_reader_ = std::make_shared<platform::HostFileReader>();
        }
        {
            const platform::ScopedHostWait host_wait{platform::HostWaitSite::RwaStreamRead};
            const auto read_status = host_file_reader_->read_at(
                stream_file.path, read_start, std::span<std::byte>(bytes).first(host_bytes));
            if (read_status != platform::HostFileReadStatus::Ok) {
                const std::uint32_t detail =
                    read_status == platform::HostFileReadStatus::OpenFailed
                        ? 8U
                        : (read_status == platform::HostFileReadStatus::SeekFailed ? 9U : 10U);
                journal(EeRwaStreamJournalEvent{.byte_count = byte_count, .file_offset = static_cast<std::uint32_t>(read_start), .path = stream_file.guest_path, .phase = EeRwaStreamJournalEvent::Phase::Dropped, .detail = detail});
                break;
            }
        }
        if (stream_state == 2U) {
            std::ranges::copy(bytes, memory.view(destination, byte_count).begin());
        } else {
            if (virtual_iop_ram_.empty()) {
                virtual_iop_ram_.resize(0x200000U);
                virtual_iop_ram_valid_.resize(0x200000U);
            }
            for (std::uint32_t byte = 0; byte < byte_count; ++byte) {
                virtual_iop_ram_[byte + destination] = static_cast<std::uint8_t>(bytes[byte]);
                virtual_iop_ram_valid_[byte + destination] = 1U;
            }
            if (rwa_iop_upload_observer_) {
                rwa_iop_upload_observer_(
                    destination,
                    std::as_bytes(std::span<const std::uint8_t>(
                        virtual_iop_ram_.data() + destination, byte_count)));
            }
            for (VirtualIopHeapBlock& block : virtual_iop_heap_blocks_) {
                const auto block_size = static_cast<std::uint32_t>(block.bytes.size());
                if (block.address > destination || byte_count > block_size ||
                    destination - block.address > block_size - byte_count) {
                    continue;
                }
                const std::uint32_t block_offset = destination - block.address;
                for (std::uint32_t byte = 0; byte < byte_count; ++byte) {
                    block.bytes[block_offset + byte] = static_cast<std::uint8_t>(bytes[byte]);
                }
                break;
            }
        }
        const unsigned long long padding_bytes = byte_count - host_bytes;
        ++gtfs_activity_.successful_reads;
        gtfs_activity_.host_bytes_read += host_bytes;
        gtfs_activity_.zero_padding_bytes += padding_bytes;
        ++record.successful_reads;
        record.host_bytes_read += host_bytes;
        record.zero_padding_bytes += padding_bytes;
        stream_file.offset = std::min<unsigned long long>(stream_file.size, read_start + byte_count);
        pending_sif_command_callbacks_.push_back({0x002b3b50U, stream_state == 2U ? destination : 0U,
                                                  0x33U, requested_bytes, slot, completion_token,
                                                  0U, 32U});
        journal(EeRwaStreamJournalEvent{.byte_count = byte_count, .file_offset = static_cast<std::uint32_t>(read_start), .destination = destination, .path = stream_file.guest_path, .phase = EeRwaStreamJournalEvent::Phase::Completed});
        break;
    }
    case 0x46U: {
        // Explicit SPU2 VoiceTrans descriptor (five words) with its own 20-byte DMA.
        if (inbound_extra_size != 20U || sif_dma_transfers_.size() <= 1U) {
            if (rwa_stream_journal_) {
                rwa_stream_journal_({
                    .sequence = rwa_activity_.command_packets,
                    .slot = opcode,
                    .byte_count = inbound_extra_size,
                    .phase = EeRwaStreamJournalEvent::Phase::Dropped,
                    .detail = 21U});
            }
            break;
        }
        const EeSifDmaTransferState& packet_transfer = sif_dma_transfers_.back();
        const EeSifDmaTransferState& extra_transfer =
            sif_dma_transfers_[sif_dma_transfers_.size() - 2U];
        const std::uint32_t descriptor_address = packet_word(24U);
        const std::uint32_t descriptor_destination = packet_word(28U);
        const std::uint32_t descriptor_size = packet_word(32U);
        const std::uint32_t completion_token = packet_word(20U);
        if (packet_transfer.id != transfer.id || packet_transfer.source != transfer.source ||
            packet_transfer.destination != transfer.destination ||
            packet_transfer.size != transfer.size || extra_transfer.id != transfer.id ||
            extra_transfer.source != descriptor_address ||
            extra_transfer.destination != descriptor_destination || extra_transfer.size != 20U ||
            descriptor_size != 20U || extra_transfer.attributes != 0U ||
            !memory.contains(descriptor_address, 20U) || completion_token == 0U) {
            if (rwa_stream_journal_) {
                rwa_stream_journal_({
                    .sequence = rwa_activity_.command_packets,
                    .slot = opcode,
                    .byte_count = inbound_extra_size,
                    .phase = EeRwaStreamJournalEvent::Phase::Dropped,
                    .detail = 22U});
            }
            break;
        }
        for (std::size_t word = 0; word < 5U; ++word) {
            rwa_activity_.last_spu_descriptor[word] = memory.read_u32_le(
                descriptor_address + static_cast<GuestAddress>(word) * 4U);
        }
        auto& history_entry = rwa_activity_.spu_descriptor_history
            [rwa_activity_.spu_descriptor_history_count &
             (EeRwaActivity::kSpuDescriptorHistory - 1U)];
        history_entry[0] = static_cast<std::uint32_t>(rwa_activity_.command_packets);
        for (std::size_t word = 0; word < 5U; ++word) {
            history_entry[word + 1U] = rwa_activity_.last_spu_descriptor[word];
        }
        ++rwa_activity_.spu_descriptor_history_count;
        ++rwa_activity_.spu_transfer_count;
        const auto window_offset = rwa_activity_.last_spu_descriptor[0];
        const auto spu_destination = rwa_activity_.last_spu_descriptor[1];
        const auto iop_source = rwa_activity_.last_spu_descriptor[2];
        const auto byte_count = rwa_activity_.last_spu_descriptor[3];
        const auto window_size = rwa_activity_.last_spu_descriptor[4];
        const auto transfer_size = (byte_count + 63U) & ~63U;
        rwa_activity_.last_spu_iop_source = iop_source;
        rwa_activity_.last_spu_destination = spu_destination;
        rwa_activity_.last_spu_size = transfer_size;
        rwa_activity_.last_spu_fnv1a64 = 0U;
        if (byte_count - 16U > 0x1ffff0U || 0x200000U - transfer_size < spu_destination) {
            ++rwa_activity_.spu_transfer_bounds_rejected;
        } else if (virtual_iop_ram_.size() == 0x200000U &&
                   iop_source <= 0x200000U - transfer_size &&
                   std::all_of(virtual_iop_ram_valid_.begin() + iop_source,
                               virtual_iop_ram_valid_.begin() + iop_source + transfer_size,
                               [](const std::uint8_t valid) { return valid != 0U; })) {
            // The retail module re-terminates the window with markers the
            // upload did not carry: patch them into the IOP image first.
            const unsigned char original_flag = virtual_iop_ram_[iop_source + 1U];
            {
                const auto window_end = window_offset + byte_count;
                const bool patch_second = window_offset <= 1U && window_end != 0U && original_flag == 0U;
                virtual_iop_ram_[iop_source + 1U] = 0U;
                virtual_iop_ram_[iop_source + byte_count - 15U] = 0U;
                if (window_offset <= 1U) {
                    virtual_iop_ram_[iop_source - window_offset + 1U] = 6U;
                }
                const auto patch_terminal = [&](const std::uint32_t marker) {
                    if (marker < window_offset || marker > window_end) {
                        return;
                    }
                    const auto relative = marker - window_offset;
                    if (byte_count >= relative && iop_source <= 0x1fffffU &&
                        0x200000U - iop_source > relative) {
                        virtual_iop_ram_[iop_source + relative] = 3U;
                        virtual_iop_ram_valid_[iop_source + relative] = 1U;
                    }
                };
                patch_terminal(window_size - 15U);
                if (patch_second) {
                    patch_terminal(window_size - 31U);
                }
            }
            if (virtual_spu2_ram_.empty()) {
                virtual_spu2_ram_.resize(0x200000U);
            }
            unsigned long long hash = 14695981039346656037ULL;
            for (std::uint32_t byte = 0; byte < transfer_size; ++byte) {
                const unsigned char value = virtual_iop_ram_[iop_source + byte];
                virtual_spu2_ram_[byte + spu_destination] = value;
                hash ^= value;
                hash *= 1099511628211ULL;
            }
            rwa_activity_.last_spu_fnv1a64 = hash;
            ++rwa_activity_.spu_transfer_materialized;
            rwa_activity_.spu_transfer_bytes += transfer_size;
            if (spu_transfer_materialized_observer_) {
                spu_transfer_materialized_observer_({
                    iop_source, spu_destination, transfer_size,
                    std::vector<std::uint8_t>(virtual_iop_ram_.begin() + iop_source,
                                              virtual_iop_ram_.begin() + iop_source +
                                                  transfer_size)});
            }
        } else {
            ++rwa_activity_.spu_transfer_source_missing;
        }
        pending_sif_command_callbacks_.push_back(
            {0x002b3b50U, 0U, 0x47U, 0U, 0U, completion_token, 0U, 64U});
        break;
    }
    case 0x49U: {
        const std::uint32_t address = packet_word(36U);
        const std::uint32_t length = packet_word(40U);
        for (VirtualIopHeapBlock& block : virtual_iop_heap_blocks_) {
            const auto block_size = static_cast<std::uint32_t>(block.bytes.size());
            if (block.address > address || length > block_size ||
                address - block.address > block_size - length) {
                continue;
            }
            std::fill_n(block.bytes.begin() + (address - block.address), length, std::uint8_t{0});
            break;
        }
        break;
    }
    case 0x11U:
        if (rwa_initialize_observer_ && !rwa_initialize_observer_()) {
            break;
        }
        pending_sif_command_callbacks_.push_back(
            {0x002b3b50U, 0U, 0x1aU, 0x00130030U, 0U, 0U, 0U, 0U});
        break;
    case 0x13U: {
        constexpr std::uint32_t kHeapEnd = 0x200000U;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> occupied_ranges;
        occupied_ranges.reserve(virtual_iop_heap_blocks_.size());
        for (const VirtualIopHeapBlock& block : virtual_iop_heap_blocks_) {
            const auto size = static_cast<std::uint32_t>(block.bytes.size());
            if (block.address >= kHeapEnd || size == 0U) {
                continue;
            }
            const auto end = std::min(block.address + size, kHeapEnd);
            occupied_ranges.emplace_back(std::max(kVirtualIopHeapAllocationBase, block.address),
                                         end);
        }
        std::ranges::sort(occupied_ranges);
        std::uint32_t cursor = kVirtualIopHeapAllocationBase;
        std::uint32_t largest_free = 0U;
        for (const auto [range_begin, range_end] : occupied_ranges) {
            if (range_begin > cursor) {
                largest_free = std::max(range_begin - cursor, largest_free);
            }
            cursor = std::max(cursor, range_end);
        }
        pending_sif_command_callbacks_.push_back(
            {0x002b3b50U, 0U, 0x1bU, 0x00096200U, 0x00100000U, 0U, 0U, 0U});
        break;
    }
    case 0x15U: {
        const std::uint32_t snapshot_address = packet_word(36U);
        if (header != 0x30U || transfer.attributes != 0x44U || snapshot_address <= 0xcffU ||
            !memory.contains(snapshot_address, 0x680U)) {
            break;
        }
        rwa_voice_snapshot_destination_ = snapshot_address;
        pending_sif_command_callbacks_.push_back({0x002b3b50U, snapshot_address - 0xd00U,
                                                  0x1cU, rwa_iop_work_area_, 0U, 0U, 0U, 0U});
        break;
    }
    case 0x17U:
        pending_sif_command_callbacks_.push_back({0x002b3b50U, 0U, 0x1dU, 0U, 0U, 0U, 0U, 0U});
        break;
    case 0x30U: {
        const std::uint32_t completion_token = packet_word(20U);
        const std::uint32_t storage_interface = packet_word(36U);
        if (completion_token == 0U || gtfs_cdvd_interface_address_ != storage_interface) {
            break;
        }
        pending_sif_command_callbacks_.push_back(
            {0x002b3b50U, 0U, 0x31U, 0U, completion_token, completion_token, 0U, 0U});
        break;
    }
    case 0x01U:
    case 0x02U: {
        const std::uint32_t record_index = rwa_stream_completion_sequence_++;
        const auto record_address = (record_index % 24U) * 48U + 0x01e81c40U;
        if (!memory.contains(record_address, 48U)) {
            break;
        }
        for (std::uint32_t word = 0U; word != 48U; word += 4U) {
            memory.write_u32_le(word + record_address, 0U);
        }
        if (opcode == 1U) {
            memory.write_u32_le(record_address + 16U, 9U);
            memory.write_u32_le(record_address + 36U, 277U);
        } else {
            memory.write_u32_le(record_address + 16U, 10U);
            memory.write_u32_le(record_address + 36U, rwa_iop_work_area_ + 9424U);
            memory.write_u32_le(record_address + 40U, rwa_iop_work_area_ - 12128U);
        }
        memory.write_u32_le(record_address + 44U, 1U);
        pending_sif_command_callbacks_.push_back({0x002af340U, record_address, 0U, 0U, 0U, 0U, 0U, 0U});
        break;
    }
    case 0x4eU: {
        const std::uint32_t completion_token = packet_word(20U);
        static_cast<void>(packet_word(36U));
        pending_sif_command_callbacks_.push_back(
            {0x002b3b50U, 0U, 0x4fU, 0U, 0U, completion_token, 0U, 0U});
        break;
    }
    default:
        break;
    }
}



// RECOVERY: statements match; only the operand order of the `session.socket == socket`
// compares (SSA version ordering inside the inlined find_if) still differs.
EeKernelCallResult EeKernelHle::deci2_call(R5900State& cpu, EeMemory& memory) {
    const auto operation = signed_argument(cpu, 4U);
    const auto block = cpu.read_gpr_u32(5U);
    if ((block & 3U) != 0U || !memory.contains(block, 16U)) {
        return {EeKernelCallStatus::InvalidArguments, 124U, "Deci2Call"};
    }
    std::array<std::uint32_t, 4> arguments{};
    for (std::size_t index = 0U; index < arguments.size(); ++index) {
        arguments[index] = memory.read_u32_le(block + static_cast<std::uint32_t>(index) * 4U);
    }
    const auto finish = [&cpu](const std::int32_t value) {
        cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(value));
        cpu.advance_pc();
        return EeKernelCallResult{EeKernelCallStatus::Handled, 124U, "Deci2Call"};
    };
    const auto find_session = [this](const unsigned int& word) {
        return std::find_if(deci2_sessions_.begin(), deci2_sessions_.end(),
                            [socket = static_cast<std::int32_t>(word)](const Deci2SessionState& session) {
                                return session.socket == socket;
                            });
    };
    switch (operation) {
    case 1: {
        const auto protocol = static_cast<unsigned short>(arguments[0]);
        const auto option = arguments[1];
        const auto handler = arguments[2];
        const auto user_area = arguments[3];
        if (protocol == 0U) {
            return finish(-1);
        }
        if (std::ranges::any_of(deci2_sessions_, [protocol](const Deci2SessionState& session) {
                return protocol == session.protocol;
            })) {
            return finish(-3);
        }
        if (deci2_sessions_.size() > 31U || next_deci2_socket_id_ <= 0 ||
            next_deci2_socket_id_ == std::numeric_limits<std::int32_t>::max()) {
            return finish(-4);
        }
        const std::int32_t socket = next_deci2_socket_id_++;
        deci2_sessions_.push_back({socket, protocol, option, handler, user_area, false});
        return finish(socket);
    }
    case 2: {
        const auto session = find_session(arguments[0]);
        if (session == deci2_sessions_.end()) {
            return finish(-2);
        }
        deci2_sessions_.erase(session);
        return finish(0);
    }
    case 3: {
        const auto session = find_session(arguments[0]);
        if (session == deci2_sessions_.end()) {
            return finish(-2);
        }
        if (session->protocol == 0x210U) {
            if (!memory.contains(session->option, 28U)) {
                return finish(-5);
            }
            const std::uint32_t size = memory.read_u32_le(session->option + 4U);
            const std::uint32_t destination = memory.read_u32_le(session->option + 16U);
            const unsigned long long write_end =
                static_cast<unsigned long long>(destination) + size;
            if (write_end > 0xffffffffULL || (size != 0U && !memory.contains(destination, size))) {
                return finish(-5);
            }
            memory.write_u32_le(session->option + 16U, size + destination);
            memory.write_u32_le(session->option + 4U, 0U);
            memory.write_u32_le(session->option + 12U, 0U);
        }
        return finish(0);
    }
    case 4:
    case -7: {
        return finish(find_session(arguments[0]) == deci2_sessions_.end() ? -2 : 0);
    }
    case -6:
    case -5: {
        if (find_session(arguments[0]) == deci2_sessions_.end()) {
            return finish(-2);
        }
        const unsigned long long length = arguments[2] & 0xffffU;
        if (length != 0U && !memory.contains(arguments[1], length)) {
            return finish(-5);
        }
        return finish(operation == -5
                          ? 0
                          : static_cast<std::int32_t>(length));
    }
    case -8: {
        const auto session = find_session(arguments[0]);
        if (session == deci2_sessions_.end()) {
            return finish(-2);
        }
        if (session->locked) {
            return finish(-8);
        }
        session->locked = true;
        return finish(0);
    }
    case -9: {
        const auto session = find_session(arguments[0]);
        if (session == deci2_sessions_.end()) {
            return finish(-2);
        }
        if (!session->locked) {
            return finish(-9);
        }
        session->locked = false;
        return finish(0);
    }
    case 16: {
        const auto text_address = arguments[0];
        std::size_t length = 0U;
        while (length != 4096U && memory.contains(text_address + static_cast<GuestAddress>(length), 1U) &&
               memory.read_u8(text_address + static_cast<GuestAddress>(length)) != 0U) {
            ++length;
        }
        if (length == 0U && !memory.contains(text_address, 1U)) {
            return finish(-5);
        }
        return finish(static_cast<std::int32_t>(length));
    }
    default:
        return finish(-1);
    }
}

EeKernelCallResult EeKernelHle::set_sif_dma(R5900State& cpu, EeMemory& memory) {
    const auto address = cpu.read_gpr_u32(4U);
    const auto entry_count = cpu.read_gpr_u32(5U);
    const unsigned long long byte_count = static_cast<unsigned long long>(entry_count) * 16U;
    if (entry_count == 0U || byte_count > 0xffffffffULL || !memory.contains(address, byte_count) ||
        next_sif_dma_id_ <= 0 || next_sif_dma_id_ == std::numeric_limits<std::int32_t>::max()) {
        cpu.write_gpr_sign_extended_32(2U, 0xffffffffU);
        cpu.advance_pc();
        return {EeKernelCallStatus::Handled, 119U, "sceSifSetDma"};
    }
    const std::int32_t id = next_sif_dma_id_++;
    incomplete_sif_dma_batches_.push_back(
        {id, sif_dma_transfers_.size(), sif_dma_transfers_.size() + entry_count});
    for (std::uint32_t index = 0U; index < entry_count; ++index) {
        const auto offset = index * 16U;
        const EeSifDmaTransferState transfer{
            .id = id,
            .source = memory.read_u32_le(address + offset),
            .destination = memory.read_u32_le(address + offset + 4U),
            .size = memory.read_u32_le(address + offset + 8U),
            .attributes = memory.read_u32_le(address + offset + 12U),
            .submitted_ee_cycles = ee_cycle_count_,
        };
        const auto source = transfer.source;
        const auto destination = transfer.destination;
        const auto size = transfer.size;
        const auto attributes = transfer.attributes;
        sif_dma_transfers_.push_back(transfer);
        // Attribute 0 transfers carry plain EE -> IOP uploads: keep the physical
        // IOP RAM image (and which bytes of it are valid) current.
        if (attributes == 0U) {
            if (size != 0U && memory.contains(source, size) && size <= 0x200000U &&
                0x200000U - size >= destination) {
                if (virtual_iop_ram_.empty()) {
                    virtual_iop_ram_.resize(0x200000U);
                    virtual_iop_ram_valid_.resize(0x200000U);
                }
                for (std::uint32_t byte = 0U; byte < size; ++byte) {
                    const std::uint8_t value = memory.read_u8(source + byte);
                    virtual_iop_ram_[destination + byte] = value;
                    virtual_iop_ram_valid_[destination + byte] = 1U;
                }
                if (rwa_iop_upload_observer_) {
                    rwa_iop_upload_observer_(
                        destination,
                        std::as_bytes(std::span<const std::uint8_t>(
                            virtual_iop_ram_.data() + destination, size)));
                }
            }
        }
        if (destination == 0xf1900U && memory.contains(source, size)) {
            pending_file_io_payload_ = transfer;
        }
        if (destination == 0xf1d00U && memory.contains(source, size)) {
            pending_load_file_payload_ = transfer;
        }
        if (destination == 0xf1b00U && memory.contains(source, size)) {
            pending_iop_heap_payload_ = transfer;
        }
        if (destination == 0xf2700U && memory.contains(source, size)) {
            pending_b3_route_payload_ = transfer;
        }
        if (destination == 0xf2900U && memory.contains(source, size)) {
            pending_gtfs_cdvd_payload_ = transfer;
        }
        if (destination == 0xf2b00U && memory.contains(source, size)) {
            pending_network_config_payload_ = transfer;
        }
        if (destination == 0xf2300U && memory.contains(source, size)) {
            pending_dbc_service_1c_payload_ = transfer;
        }
        for (auto& block : virtual_iop_heap_blocks_) {
            const unsigned int block_size = static_cast<unsigned int>(block.bytes.size());
            if (block.address > destination || size > block_size ||
                destination - block.address > block_size - size ||
                !memory.contains(source, size)) {
                continue;
            }
            const unsigned int block_offset = destination - block.address;
            for (std::uint32_t byte = 0U; byte < size; ++byte) {
                block.bytes[byte + block_offset] = memory.read_u8(source + byte);
            }
            break;
        }
        complete_virtual_sif_rpc_init(transfer, memory);
        complete_virtual_rpc_bind(transfer, memory);
        complete_virtual_network_config_lifecycle_call(transfer, memory);
        complete_virtual_b3_route_init_call(transfer, memory);
        complete_virtual_b3_route_routing_call(transfer, memory);
        complete_virtual_gtfs_cdvd_init_call(transfer, memory);
        complete_virtual_gtfs_cdvd_open_call(transfer, memory);
        complete_virtual_gtfs_cdvd_close_call(transfer, memory);
        complete_virtual_gtfs_cdvd_read_call(transfer, memory);
        complete_virtual_gtfs_cdvd_seek_call(transfer, memory);
        complete_virtual_cdvd_init_call(transfer, memory);
        complete_virtual_cdvd_scmd_call(transfer, memory);
        complete_virtual_cdvd_disk_ready_call(transfer, memory);
        complete_virtual_file_io_version_call(transfer, memory);
        complete_virtual_iop_heap_call(transfer, memory);
        complete_virtual_load_file_version_call(transfer, memory);
        complete_virtual_load_file_module_call(transfer, memory);
        complete_virtual_dbc_man_version_call(transfer, memory);
        complete_virtual_dbc_man_set_work_address_call(transfer, memory);
        complete_virtual_dbc_man_create_socket_call(transfer, memory);
        complete_virtual_dbc_man_delete_socket_call(transfer, memory);
        complete_virtual_dbc_man_get_dependency_call(transfer, memory);
        complete_virtual_dbc_service_1c_call(transfer, memory);
        complete_virtual_lg_dev_version_call(transfer, memory);
        complete_virtual_lg_dev_enumerate_call(transfer, memory);
        complete_virtual_lg_dev_no_device_call(transfer, memory);
        complete_virtual_file_io_open_call(transfer, memory);
        complete_virtual_file_io_read_call(transfer, memory);
        complete_virtual_file_io_close_call(transfer, memory);
        complete_virtual_rwa_command(transfer, memory, cpu);
    }
    // Retail SIF DMAs complete asynchronously: age the batch before it is reported.
    constexpr std::uint64_t kCompletionDelayEeCycles = 50000U;
    const auto completion_deadline =
        (ee_cycle_count_ > std::numeric_limits<std::uint64_t>::max() - kCompletionDelayEeCycles
             ? std::numeric_limits<std::uint64_t>::max() - kCompletionDelayEeCycles
             : ee_cycle_count_) +
        kCompletionDelayEeCycles;
    pending_sif_dma_batches_.push_back({id, completion_deadline});
    cpu.write_gpr_sign_extended_32(2U, static_cast<std::uint32_t>(id));
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 119U, "sceSifSetDma"};
}

namespace {
// RWA publishes its completion ring through two EE words; the ring is usable
// once the descriptor it points at carries the module's magic.
[[nodiscard]] bool rwa_completion_ring_ready(const EeMemory& memory) {
    if (!memory.is_main_ram_range(0x4e3facU, 8U)) {
        return false;
    }
    const auto ring = memory.read_u32_le(0x4e3facU);
    const auto count = memory.read_u32_le(0x4e3fb0U);
    if (count <= 1U || ring == 0U || (ring & 3U) != 0U ||
        static_cast<unsigned long long>(ring) + 24U > memory.size()) {
        return false;
    }
    const auto entry = ring + 12U;
    return memory.read_u32_le(entry) == 0x2af340U;
}
} // namespace

// RECOVERY: statements match; GCC folds the `read_index + 23 < write_index` ring-distance
// test here but the original keeps the three long long temporaries.
bool EeKernelHle::enqueue_rwa_sif_completion(
    EeMemory& memory, const std::array<std::uint8_t, 0x30U>& source_record, const std::uint32_t type,
    const std::uint32_t t0, const std::uint32_t extra_source, const std::uint32_t extra_destination,
    const std::uint32_t remaining_size, const std::uint32_t a2, const std::uint32_t a3,
    const bool terminal) {
    // RWA keeps a 24-entry ring of 0x30-byte completion records in EE RAM; the
    // game consumes it from its own SIF command callback.
    const int completion_semaphore =
        memory.contains(0x4e2c54U, 4U) ? read_i32(memory, 0x4e2c54U) : -1;
    if (!rwa_completion_ring_ready(memory)) {
        return false;
    }
    if (completion_semaphore < 0 || !semaphore_index(completion_semaphore)) {
        return false;
    }
    if (!memory.contains(0x4e2c3cU, 4U) || !memory.contains(0x4e2c40U, 4U)) {
        return false;
    }
    const auto read_index = read_i32(memory, 0x4e2c3cU);
    const auto write_index = memory.read_u32_le(0x4e2c40U);
    const std::int32_t signed_write_index = static_cast<std::int32_t>(write_index);
    if (read_index < 0 || signed_write_index < 0 ||
        static_cast<long long>(write_index) > static_cast<long long>(read_index) + 23) {
        return false;
    }
    const unsigned int write_slot = write_index % 24U;
    const auto destination = write_slot * 48U + 0x1e81c40U;
    if (!memory.contains(destination, 48U)) {
        return false;
    }
    for (std::size_t byte = 0U; byte < source_record.size(); ++byte) {
        memory.write_u8(destination + static_cast<std::uint32_t>(byte), source_record[byte]);
    }
    memory.write_u32_le(destination, 48U);
    memory.write_u32_le(destination + 4U, 0U);
    memory.write_u32_le(destination + 8U, 1U);
    memory.write_u32_le(destination + 16U, type);
    memory.write_u32_le(destination + 20U, t0);
    memory.write_u32_le(destination + 24U, extra_source);
    memory.write_u32_le(destination + 28U, extra_destination);
    memory.write_u32_le(destination + 32U, remaining_size);
    memory.write_u32_le(destination + 36U, a2);
    memory.write_u32_le(0x4e2c40U, write_index + 1U);
    static_cast<void>(signal_semaphore(completion_semaphore));
    return true;
}

// RECOVERY: statements and control flow match the original; only the profile counts inside the
// chunk loop differ by a few units in the 9th digit (GCC branch-probability rounding).
void EeKernelHle::complete_virtual_rwa_op05_voice_transfer(EeMemory& memory, const std::int32_t id) {
    const auto found = std::ranges::find_if(
        pending_rwa_op05_voice_transfers_,
        [id](const PendingRwaOp05VoiceTransfer& pending) { return pending.sif_dma_id == id; });
    if (found == pending_rwa_op05_voice_transfers_.end()) {
        return;
    }
    PendingRwaOp05VoiceTransfer transfer = std::move(*found);
    pending_rwa_op05_voice_transfers_.erase(found);
    unsigned int iop_source = transfer.iop_source;
    unsigned int spu_destination = transfer.spu_destination;
    unsigned int remaining = transfer.size;
    unsigned long long command_hash = 14695981039346656037ULL;
    bool rejected = transfer.size == 0U;
    bool command_fully_materialized = transfer.size != 0U;
    while (remaining != 0U) {
        const std::uint32_t chunk = std::min<std::uint32_t>(remaining, 0x200000U);
        ++rwa_activity_.spu_transfer_count;
        rwa_activity_.last_spu_iop_source = iop_source;
        rwa_activity_.last_spu_destination = spu_destination;
        rwa_activity_.last_spu_size = chunk;
        rwa_activity_.last_spu_fnv1a64 = 0U;
        const auto iop_end = static_cast<unsigned long long>(iop_source) + chunk;
        const auto spu_end = static_cast<unsigned long long>(spu_destination) + chunk;
        if (iop_end > 0x200000U || spu_end > 0x200000U) {
            ++rwa_activity_.spu_transfer_bounds_rejected;
            rejected = true;
            command_fully_materialized = false;
        } else if (virtual_iop_ram_.size() == 0x200000U &&
                   virtual_iop_ram_valid_.size() == 0x200000U &&
                   std::all_of(virtual_iop_ram_valid_.begin() + iop_source,
                               virtual_iop_ram_valid_.begin() + static_cast<std::ptrdiff_t>(iop_end),
                               [](const std::uint8_t valid) { return valid != 0U; })) {
            if (virtual_spu2_ram_.empty()) {
                virtual_spu2_ram_.resize(0x200000U);
            }
            unsigned long long chunk_hash = 14695981039346656037ULL;
            for (std::uint32_t byte = 0U; byte < chunk; ++byte) {
                const unsigned char value = virtual_iop_ram_[iop_source + byte];
                virtual_spu2_ram_[spu_destination + byte] = value;
                chunk_hash ^= value;
                chunk_hash *= 1099511628211ULL;
                command_hash ^= value;
                command_hash *= 1099511628211ULL;
            }
            ++rwa_activity_.spu_transfer_materialized;
            rwa_activity_.spu_transfer_bytes += chunk;
            rwa_activity_.op05_voice_transfer_bytes += chunk;
            rwa_activity_.last_spu_fnv1a64 = chunk_hash;
            if (spu_transfer_materialized_observer_) {
                spu_transfer_materialized_observer_(
                    {iop_source, spu_destination, chunk,
                     std::vector<std::uint8_t>(
                         virtual_iop_ram_.begin() + iop_source,
                         virtual_iop_ram_.begin() + static_cast<std::ptrdiff_t>(iop_end))});
            }
        } else {
            ++rwa_activity_.spu_transfer_source_missing;
            rejected = true;
            command_fully_materialized = false;
        }
        iop_source += chunk;
        spu_destination += chunk;
        remaining -= chunk;
    }
    ++rwa_activity_.op05_voice_transfer_completed;
    if (rejected) {
        ++rwa_activity_.op05_voice_transfer_rejected;
    }
    rwa_activity_.last_op05_fnv1a64 = command_fully_materialized ? command_hash : 0U;
    const bool enqueued = enqueue_rwa_sif_completion(memory, transfer.response_record, 13U,
                                                     transfer.t0, 0U, 0U, 0U, transfer.a2,
                                                     transfer.a3, true);
    if (std::getenv("TAKEDOWN_RWA_OPS") != nullptr) {
        std::fprintf(stderr,
                     "[RWA-OP05-DONE] id=%d iop=0x%06x spu=0x%06x size=0x%x copied=%u cid1=%u\n", id,
                     transfer.iop_source, transfer.spu_destination, transfer.size,
                     command_fully_materialized ? 1U : 0U, enqueued ? 1U : 0U);
    }
}

void EeKernelHle::complete_virtual_rwa_staging_transfer(EeMemory& memory, const std::int32_t id) {
    const auto found = std::find_if(
        pending_rwa_staging_transfers_.begin(), pending_rwa_staging_transfers_.end(),
        [id](const PendingRwaStagingTransfer& pending) { return id == pending.sif_dma_id; });
    if (found == pending_rwa_staging_transfers_.end()) {
        return;
    }
    PendingRwaStagingTransfer transfer = std::move(*found);
    pending_rwa_staging_transfers_.erase(found);
    if (transfer.size == 0U || transfer.bytes.size() != transfer.size ||
        transfer.size > transfer.remaining_size ||
        0x200000U - transfer.size < transfer.spu_destination) {
        ++rwa_activity_.op07_staging_transfer_rejected;
        return;
    }
    std::vector<std::uint8_t> staged = std::move(transfer.bytes);
    if (virtual_spu2_ram_.empty()) {
        virtual_spu2_ram_.resize(0x200000U);
    }
    unsigned long long hash = 14695981039346656037ULL;
    for (std::uint32_t byte = 0U; byte < transfer.size; ++byte) {
        virtual_spu2_ram_[transfer.spu_destination + byte] = staged[byte];
        hash ^= staged[byte];
        hash *= 1099511628211ULL;
    }
    ++rwa_activity_.spu_transfer_count;
    ++rwa_activity_.spu_transfer_materialized;
    rwa_activity_.spu_transfer_bytes += transfer.size;
    rwa_activity_.last_spu_iop_source = transfer.staging_destination;
    rwa_activity_.last_spu_destination = transfer.spu_destination;
    rwa_activity_.last_spu_size = transfer.size;
    rwa_activity_.last_spu_fnv1a64 = hash;
    if (spu_transfer_materialized_observer_) {
        spu_transfer_materialized_observer_({transfer.staging_destination, transfer.spu_destination,
                                             transfer.size, staged});
    }
    const auto word_at = [&record = transfer.response_record](const std::size_t offset) {
        return static_cast<std::uint32_t>(record[offset]) |
               (static_cast<std::uint32_t>(record[offset + 1U]) << 8U) |
               (static_cast<std::uint32_t>(record[offset + 2U]) << 16U) |
               (static_cast<std::uint32_t>(record[offset + 3U]) << 24U);
    };
    const bool enqueued = enqueue_rwa_sif_completion(
        memory, transfer.response_record, 15U, word_at(20U), transfer.ee_source + transfer.size,
        transfer.spu_destination + transfer.size, transfer.remaining_size - transfer.size,
        word_at(36U), word_at(40U), transfer.size == transfer.remaining_size);
    if (enqueued) {
        ++rwa_activity_.op07_staging_responses_enqueued;
    } else {
        ++rwa_activity_.op07_staging_responses_dropped;
    }
    ++rwa_activity_.op07_staging_transfer_completed;
    rwa_activity_.op07_staging_transfer_bytes += transfer.size;
    rwa_activity_.last_op07_staging_address = transfer.staging_destination;
    rwa_activity_.last_op07_spu_destination = transfer.spu_destination;
    rwa_activity_.last_op07_chunk_size = transfer.size;
    rwa_activity_.last_op07_remaining_size = transfer.remaining_size - transfer.size;
    rwa_activity_.last_op07_fnv1a64 = hash;
    if (rwa_stream_journal_) {
        rwa_stream_journal_({.sequence = rwa_activity_.command_packets,
                             .slot = 7U,
                             .byte_count = transfer.size,
                             .file_offset = transfer.staging_destination,
                             .destination = transfer.spu_destination,
                             .phase = EeRwaStreamJournalEvent::Phase::Completed,
                             .detail = 95U});
    }
    if (std::getenv("TAKEDOWN_RWA_OPS") != nullptr) {
        std::fprintf(stderr, "[RWA-OP07-DONE] id=%d stage=0x%06x dst=0x%06x size=0x%x cid1=%u\n", id,
                     transfer.staging_destination, transfer.spu_destination, transfer.size,
                     enqueued ? 1U : 0U);
    }
}

void EeKernelHle::complete_sif_dma_batch(EeMemory& memory, const std::int32_t id) {
    const auto found = std::ranges::find(incomplete_sif_dma_batches_, id, &IncompleteSifDmaBatch::id);
    if (found == incomplete_sif_dma_batches_.end()) {
        return;
    }
    const unsigned long long first = found->first;
    const auto end = std::min<unsigned long long>(found->end, sif_dma_transfers_.size());
    incomplete_sif_dma_batches_.erase(found);
    complete_virtual_rwa_staging_transfer(memory, id);
    complete_virtual_rwa_op05_voice_transfer(memory, id);
    for (std::size_t index = first; index < end; ++index) {
        sif_dma_transfers_[index].completed = true;
    }
}
std::optional<GuestAddress> EeKernelHle::registered_syscall_handler(const std::uint32_t number) const noexcept {
    const auto found = std::ranges::find_if(
        syscall_handlers_, [number](const EeSyscallHandlerState& state) { return state.number == number; });
    if (found == syscall_handlers_.end()) {
        return std::nullopt;
    }
    return found->handler;
}

EeKernelCallResult EeKernelHle::set_syscall(R5900State& cpu) {
    const auto number = cpu.read_gpr_u32(4U);
    const auto handler = cpu.read_gpr_u32(5U);
    const auto found = std::ranges::find_if(
        syscall_handlers_, [number](const EeSyscallHandlerState& state) { return state.number == number; });
    if (handler == 0U) {
        if (found != syscall_handlers_.end()) {
            syscall_handlers_.erase(found);
        }
    } else if (found == syscall_handlers_.end()) {
        syscall_handlers_.push_back({number, handler});
    } else {
        found->handler = handler;
    }
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 116U, "SetSyscall"};
}

EeKernelCallResult EeKernelHle::find_address(R5900State& cpu) const {
    const auto lower = cpu.read_gpr_u32(4U);
    const auto upper = cpu.read_gpr_u32(5U);
    const auto handler = cpu.read_gpr_u32(6U);
    GuestAddress result = 0U;
    for (const auto& state : syscall_handlers_) {
        if (state.number > 255U || state.handler != handler) {
            continue;
        }
        const auto address = kCompatibleSyscallTableBase + state.number * 4U;
        if (address >= lower && address < upper && (result == 0U || address < result)) {
            result = address;
        }
    }
    cpu.write_gpr_sign_extended_32(2U, result);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 131U, "FindAddress"};
}

EeKernelCallResult EeKernelHle::copy_words(R5900State& cpu, EeMemory& memory) const {
    const auto destination = cpu.read_gpr_u32(4U);
    const auto source = cpu.read_gpr_u32(5U);
    const auto size = cpu.read_gpr_u32(6U);
    const auto word_count = size >> 2U;
    const GuestAddress byte_count = word_count * 4U;
    if (!memory.contains(destination, byte_count) || !memory.contains(source, byte_count)) {
        return {EeKernelCallStatus::InvalidArguments, 90U, "Copy"};
    }
    std::uint32_t last_word = cpu.read_gpr_u32(3U);
    for (std::uint32_t index = 0U; index < word_count; ++index) {
        const GuestAddress offset = index * 4U;
        last_word = memory.read_u32_le(source + offset);
        memory.write_u32_le(destination + offset, last_word);
    }
    cpu.write_gpr_sign_extended_32(4U, destination + byte_count);
    cpu.write_gpr_sign_extended_32(5U, source + byte_count);
    cpu.write_gpr_u64(6U, word_count);
    cpu.write_gpr_u64(7U, word_count);
    if (word_count != 0U) {
        cpu.write_gpr_sign_extended_32(3U, last_word);
    }
    cpu.write_gpr_u64(2U, 0U);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 90U, "Copy"};
}

EeKernelCallResult EeKernelHle::get_entry_address(R5900State& cpu, const EeMemory& memory) const {
    const auto handler = registered_syscall_handler(91U);
    if (!handler) {
        return {EeKernelCallStatus::Unsupported, 91U, "GetEntryAddress"};
    }
    GuestAddress table_offset;
    if (*handler == 0x80076000U) {
        table_offset = 0x710U;
    } else if (*handler == 0x80075000U) {
        table_offset = 0x300U;
    } else {
        return {EeKernelCallStatus::Unsupported, 91U, "GetEntryAddress"};
    }
    const auto table = *handler + table_offset;
    if (!memory.contains(table, 48U)) {
        return {EeKernelCallStatus::InvalidArguments, 91U, "GetEntryAddress"};
    }
    const auto target = cpu.read_gpr_u32(4U);
    GuestAddress result = 0U;
    for (std::size_t index = 0U; index < 6U; ++index) {
        const auto offset = static_cast<std::uint32_t>(index) * 8U;
        if (memory.read_u32_le(table + offset) == target) {
            result = memory.read_u32_le(table + offset + 4U);
            break;
        }
    }
    cpu.write_gpr_sign_extended_32(2U, result);
    cpu.advance_pc();
    return {EeKernelCallStatus::Handled, 91U, "GetEntryAddress"};
}

std::string_view to_string(const EeKernelCallStatus status) noexcept {
    switch (status) {
    case EeKernelCallStatus::Handled:
        return "handled";
    case EeKernelCallStatus::Blocked:
        return "kernel call would block";
    case EeKernelCallStatus::Unsupported:
        return "unsupported kernel call";
    case EeKernelCallStatus::InvalidArguments:
        return "invalid kernel call arguments";
    }
    return "unknown kernel call status";
}
std::string_view ee_kernel_call_name(const std::uint32_t number) noexcept {
    switch (number) {
    case 2U:
        return "SetGsCrt";
    case 115U:
        return "SetVSyncFlag";
    case 35U:
        return "ExitThread";
    case 36U:
        return "ExitDeleteThread";
    case 16U:
        return "AddIntcHandler2";
    case 17U:
        return "RemoveIntcHandler";
    case 20U:
        return "EnableIntc";
    case 18U:
        return "AddDmacHandler2";
    case 19U:
        return "RemoveDmacHandler";
    case 22U:
        return "EnableDmac";
    case 23U:
        return "DisableDmac";
    case 60U:
        return "SetupThread";
    case 61U:
        return "SetupHeap";
    case 62U:
        return "EndOfHeap";
    case 64U:
        return "CreateSema";
    case 32U:
        return "CreateThread";
    case 33U:
        return "DeleteThread";
    case 34U:
        return "StartThread";
    case 37U:
        return "TerminateThread";
    case 47U:
        return "GetThreadId";
    case 48U:
        return "ReferThreadStatus";
    case 41U:
        return "ChangeThreadPriority";
    case 50U:
        return "SleepThread";
    case 51U:
        return "WakeupThread";
    case 252U:
    case 0xffffff02U:
        return "SetAlarm";
    case 253U:
    case 0xffffff01U:
        return "ReleaseAlarm";
    case 65U:
        return "DeleteSema";
    case 66U:
        return "SignalSema";
    case 68U:
        return "WaitSema";
    case 69U:
        return "PollSema";
    case 74U:
        return "SetOsdConfigParam";
    case 75U:
        return "GetOsdConfigParam";
    case 111U:
        return "GetOsdConfigParam2";
    case 90U:
        return "Copy";
    case 91U:
        return "GetEntryAddress";
    case 100U:
        return "FlushCache";
    case 113U:
        return "GsPutIMR";
    case 107U:
        return "sceSifStopDma";
    case 116U:
        return "SetSyscall";
    case 120U:
        return "sceSifSetDChain";
    case 0xffffff89U:
        return "sceSifDmaStat";
    case 119U:
        return "sceSifSetDma";
    case 121U:
        return "sceSifSetReg";
    case 122U:
        return "sceSifGetReg";
    case 124U:
        return "Deci2Call";
    case 131U:
        return "FindAddress";
    default:
        return "unknown";
    }
}
} // namespace takedown::guest
