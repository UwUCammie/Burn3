#include "takedown/guest/ee_aot.hpp"

#include "ee_aot_internal.hpp"

#include "takedown/guest/ee_interpreter.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace takedown::guest {
namespace {
EeAotStatistics g_statistics{};
// TAKEDOWN_EE_AOT_MISSES: the addresses at which runs stopped for a compiled
// block the caller keeps to itself.
bool g_record_excluded{};
std::unordered_map<GuestAddress, std::uint64_t> g_excluded;
// TAKEDOWN_EE_AOT_MISSES: the hook PCs at which the caller's burst ended.
std::unordered_map<GuestAddress, std::uint64_t> g_hook_stops;
// ... and the native entries it stepped through.
std::unordered_map<GuestAddress, std::uint64_t> g_native_declines;
} // namespace

namespace ee_aot_detail {

namespace {
[[gnu::always_inline]] inline bool stop(EeAotContext& c, const GuestAddress pc,
                                        const GuestAddress last_pc) noexcept {
    c.state->set_pc(pc);
    c.last_pc = last_pc;
    return false;
}

// A block the run did not enter because the budget (the caller's horizon)
// could not hold it: `remaining` is what the budget still had.
void note_budget_refusal(const std::uint64_t remaining, const bool at_entry) noexcept {
    ++g_statistics.stop_budget;
    ++g_statistics.stop_budget_remaining[std::min<std::size_t>(
        std::bit_width(remaining), g_statistics.stop_budget_remaining.size() - 1U)];
    g_statistics.stop_budget_at_entry += at_entry ? 1U : 0U;
}

// The load at pc, which the generated code gave up on because the address is
// not RAM or scratchpad, when it reads EE or GS register storage: performed
// here exactly as the interpreter performs it (the same EeMemory read, which
// changes no device state), with the result written to the register file the
// exit has already updated. False for anything else (other widths, an
// unaligned or non-register address), which the interpreter then takes.
bool serve_register_load(EeAotContext& c, const GuestAddress pc) noexcept {
    std::uint32_t word{};
    std::memcpy(&word, c.ram + (pc & 0x1fffffffU), sizeof(word));
    unsigned width;
    bool sign_extend;
    switch (word >> 26U) {
    case 0x20U: width = 1U; sign_extend = true; break;  // lb
    case 0x24U: width = 1U; sign_extend = false; break; // lbu
    case 0x21U: width = 2U; sign_extend = true; break;  // lh
    case 0x25U: width = 2U; sign_extend = false; break; // lhu
    case 0x23U: width = 4U; sign_extend = true; break;  // lw
    case 0x27U: width = 4U; sign_extend = false; break; // lwu
    default: return false;
    }
    const auto base = static_cast<std::uint32_t>(c.gpr[(word >> 21U) & 31U].words[0]);
    const auto address =
        base + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word)));
    if ((address & (width - 1U)) != 0U || !c.memory->ee_aot_register_read_is_pure(address, width)) {
        return false;
    }
    std::uint64_t value;
    switch (width) {
    case 1U:
        value = c.memory->read_u8(address);
        value = sign_extend ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(value)))
                            : value;
        break;
    case 2U:
        value = c.memory->read_u16_le(address);
        value = sign_extend ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)))
                            : value;
        break;
    default:
        value = c.memory->read_u32_le(address);
        value = sign_extend ? ee_sx(static_cast<std::uint32_t>(value)) : value;
        break;
    }
    if (const auto rt = (word >> 16U) & 31U; rt != 0U) {
        ee_put(c.gpr, rt, value);
    }
    ++g_statistics.register_loads;
    return true;
}

// The word store at pc, which the generated code gave up on, when it only
// latches a DMA channel parameter (EeMemory::ee_aot_store_plain_register).
// False for anything else, which the interpreter then takes.
bool serve_register_store(EeAotContext& c, const GuestAddress pc) noexcept {
    std::uint32_t word{};
    std::memcpy(&word, c.ram + (pc & 0x1fffffffU), sizeof(word));
    if ((word >> 26U) != 0x2bU) { // sw
        return false;
    }
    const auto base = static_cast<std::uint32_t>(c.gpr[(word >> 21U) & 31U].words[0]);
    const auto address =
        base + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word)));
    if ((address & 3U) != 0U || c.memory->ee_aot_store_needs_slow_path() ||
        !c.memory->ee_aot_store_plain_register(address, c.gpr[(word >> 16U) & 31U].words[0])) {
        return false;
    }
    ++g_statistics.register_stores;
    return true;
}
} // namespace

// Statistics only: where the access of the instruction at pc (a load or store
// the run stopped before) went, from the registers the exit has written back.
void note_memory_stop(const EeAotContext& c, const GuestAddress pc) noexcept {
    std::uint32_t word{};
    std::memcpy(&word, c.ram + (pc & 0x1fffffffU), sizeof(word));
    const auto base = static_cast<std::uint32_t>(c.gpr[(word >> 21U) & 31U].words[0]);
    const auto address =
        base + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word)));
    const auto physical = address & 0x1fffffffU;
    auto& stats = g_statistics;
    if (physical >= 0x10000000U && physical < 0x10010000U) {
        ++stats.stop_memory_ee_register_kib[(physical - 0x10000000U) >> 10U];
    } else if (physical >= 0x11000000U && physical < 0x12000000U) {
        ++stats.stop_memory_vu;
    } else if (physical >= 0x12000000U && physical < 0x13000000U) {
        ++stats.stop_memory_gs;
    } else if ((address & 1U) != 0U) {
        ++stats.stop_memory_unaligned;
    } else {
        ++stats.stop_memory_other;
    }
}

bool ee_exit_stop(EeAotContext& c, const std::uint32_t kind, const std::uint32_t j, const GuestAddress pc,
                  const GuestAddress next) noexcept {
    switch (kind) {
    case kEeExitRefused:
        if (c.block_ok[j] != 0U) {
            note_budget_refusal(c.budget - c.executed, c.executed == 0U);
        } else {
            ++g_statistics.stop_excluded;
        }
        if (c.block_ok[j] == 0U && g_record_excluded) [[unlikely]] {
            ++g_excluded[pc];
        }
        return stop(c, pc, c.last_pc);
    case kEeExitAt:
        ++g_statistics.stop_untranslated;
        return stop(c, pc, c.last_pc);
    case kEeExitAfter:
        c.executed += j + 1U;
        ++g_statistics.stop_device;
        return stop(c, next, pc);
    case kEeExitSlotUntranslated:
    case kEeExitSlotMemory:
    case kEeExitSlotFault:
        if (kind == kEeExitSlotMemory) {
            if (serve_register_load(c, pc) || serve_register_store(c, pc)) {
                // The slot instruction is done; continue at the transfer's
                // target, as after any transfer out of a function.
                c.executed += j + 1U;
                c.next_pc = next;
                c.last_pc = pc;
                return true;
            }
            note_memory_stop(c, pc);
        }
        c.executed += j;
        ++(kind == kEeExitSlotUntranslated ? g_statistics.stop_untranslated
           : kind == kEeExitSlotMemory     ? g_statistics.stop_memory
                                           : g_statistics.stop_fault);
        c.state->set_fetch_state(pc, next, true);
        c.last_pc = pc - 4U;
        return false;
    default:
        if (kind == kEeExitBeforeMemory) {
            if (serve_register_load(c, pc) || serve_register_store(c, pc)) {
                // Go on with the next instruction through the dispatcher
                // (generated code has an entry after every load that can
                // leave RAM; where it has none the run ends there).
                c.executed += j + 1U;
                c.next_pc = pc + 4U;
                c.last_pc = pc;
                return true;
            }
            note_memory_stop(c, pc);
        }
        c.executed += j;
        ++(kind == kEeExitBeforeUntranslated ? g_statistics.stop_untranslated
           : kind == kEeExitBeforeMemory     ? g_statistics.stop_memory
                                             : g_statistics.stop_fault);
        // The previous instruction is the one before in the block, unless
        // the block was entered here (by a transfer, or by the dispatcher at
        // an address inside the block with nothing executed since).
        return stop(c, pc, j != 0U && c.executed != c.entry_executed ? pc - 4U : c.last_pc);
    }
}

bool ee_callout(EeAotContext& c, const DecodedR5900Instruction& instruction, const GuestAddress next,
                const bool delay_slot, const std::uint32_t index) {
    c.state->set_fetch_state(instruction.pc, next, delay_slot);
    ee_provenance_at(c, instruction.pc, c.executed + index);
    auto result = execute_r5900_decoded(*c.state, *c.memory, instruction);
    if (result.status != EeStepStatus::Executed || result.vu0_micro.has_value() ||
        ee_device_changed(c)) [[unlikely]] {
        c.callout_last = true;
        *c.callout = std::move(result);
        return false;
    }
    ee_refresh_stores_plain(c);
    return true;
}

// The interpreter's VCALLMS without its decode and dispatch: the same fetch
// state and provenance as a call-out, the same launch, and the same result.
bool ee_vcallms(EeAotContext& c, const GuestAddress pc, const std::uint32_t word, const GuestAddress next,
                const bool delay_slot, const std::uint32_t index) {
    c.state->set_fetch_state(pc, next, delay_slot);
    ee_provenance_at(c, pc, c.executed + index);
    const auto result = run_vu0_micro_call(*c.state, *c.memory, vcallms_entry(word));
    if (result.status == Vu0MicroRunStatus::Completed) [[likely]] {
        c.state->advance_pc();
        if (!ee_device_changed(c)) [[likely]] {
            ee_refresh_stores_plain(c);
            return true;
        }
        c.callout_last = true;
        *c.callout = {.status = EeStepStatus::Executed, .instruction = decode_r5900(pc, word)};
        return false;
    }
    c.callout_last = true;
    *c.callout = {.status = EeStepStatus::Vu0MicroExecutionFault, .instruction = decode_r5900(pc, word),
                  .vu0_micro = result};
    return false;
}

// The interpreter has already left the fetch state exactly as step_r5900
// would (committed, or not for a fault).
bool ee_callout_stop(EeAotContext& c, const std::uint32_t j, const GuestAddress pc) noexcept {
    c.executed += j + 1U;
    ++(c.callout->status == EeStepStatus::Executed ? g_statistics.stop_device
                                                  : g_statistics.stop_fault);
    c.last_pc = pc;
    return false;
}

void ee_store_scratchpad(EeAotContext& c, std::byte* p, const std::uint32_t length) noexcept {
    const auto offset = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) -
                                                 reinterpret_cast<std::uintptr_t>(c.scratchpad));
    c.memory->ee_aot_record_scratchpad_store(offset, length);
}

std::byte* ee_ram_other_out_of_line(const EeAotContext& c, const std::uint32_t address,
                                    const std::uint32_t length) noexcept {
    return ee_ram_other(c, address, length);
}

bool ee_store_fast_out_of_line(EeAotContext& c, std::byte* const p, const void* const source,
                               const std::uint32_t length) noexcept {
    return ee_store_fast_inline(c, p, source, length);
}

// What every generated store used to do inline after a declined fast path:
// the store observer check, the provenance and the ordinary bookkeeping. One
// shared copy keeps that rarely taken route out of the shards.
unsigned ee_store_slow(EeAotContext& c, std::byte* const p, const void* const source, const std::uint32_t length,
                       const GuestAddress pc, const std::uint64_t executed) noexcept {
    if (!ee_store_allowed(c)) {
        return 1U;
    }
    ee_provenance_at(c, pc, executed);
    if (ee_store(c, p, source, length) && ee_device_changed(c)) {
        return 2U;
    }
    ee_refresh_stores_plain(c);
    return 0U;
}

#if defined(TAKEDOWN_LOCAL_EE_AOT_MODULE)
#include TAKEDOWN_LOCAL_EE_AOT_MODULE
constexpr bool kEeAotLinked = true;
#else
constexpr bool kEeAotLinked = false;
constexpr GuestAddress kEeAotCodeBegin = 0U;
constexpr GuestAddress kEeAotCodeEnd = 0U;
constexpr EeAotFunctionInfo kEeAotFunctions[1]{};
constexpr std::uint32_t kEeAotFunctionCount = 0U;
constexpr EeAotEntryInfo kEeAotEntries[1]{};
constexpr std::uint32_t kEeAotEntryCount = 0U;
constexpr EeAotBlockInfo kEeAotBlocks[1]{};
constexpr std::uint32_t kEeAotBlockCount = 0U;
constexpr std::uint32_t kEeAotCodeWords[1]{};
#endif

} // namespace ee_aot_detail

namespace {

using namespace ee_aot_detail;

constexpr std::uint32_t kNoBlock = ~std::uint32_t{0U};

struct EeAotRuntime {
    bool enabled{};
    // Entry index + 1 per code word in [kEeAotCodeBegin, kEeAotCodeEnd).
    std::vector<std::uint32_t> entry_by_word;
    std::vector<std::uint8_t> block_ok;
    // Per block: the highest excluded address in it (0: none; block_ok is set
    // exactly then). An entry after it executes no excluded instruction.
    std::vector<GuestAddress> block_last_excluded;
    // Per block: its only excluded address is its first instruction, and that
    // is an entry the caller offers to a native replacement (declinable).
    std::vector<std::uint8_t> block_native_entry;
    // Code-write epoch each function was last checked at, and its verdict.
    std::vector<std::uint64_t> checked_epoch;
    std::vector<std::uint8_t> identical;
    // The epoch of the last check that found the function identical.
    std::vector<std::uint64_t> verified_epoch;
    // Session stops (ee_aot_set_session_stops): the install-time verdicts of
    // block_ok, block_last_excluded and block_native_entry, which a stop PC
    // inside a block overlays while its set is active.
    std::vector<std::uint8_t> base_block_ok;
    std::vector<GuestAddress> base_last_excluded;
    std::vector<std::uint8_t> base_native_entry;
    // Block indices by start address, to find the block holding a PC.
    std::vector<std::uint32_t> blocks_by_begin;
    struct SessionStop {
        std::uint32_t block{};
        GuestAddress pc{};
    };
    std::vector<SessionStop> session_fixed; // sorted by block
    bool session_active{};
    std::array<GuestAddress, 2> session_extra{};
    std::array<std::uint32_t, 2> session_extra_block{kNoBlock, kNoBlock};
    // TAKEDOWN_EE_AOT_MISSES: where runs found no compiled block.
    std::string misses_path;
    std::unordered_map<GuestAddress, std::uint64_t> misses;
};

EeAotRuntime& runtime() {
    static EeAotRuntime instance;
    return instance;
}

// The compiled block whose words hold pc; kNoBlock when none does.
std::uint32_t block_containing(const EeAotRuntime& rt, const GuestAddress pc) {
    const auto found = std::upper_bound(rt.blocks_by_begin.begin(), rt.blocks_by_begin.end(), pc,
                                        [](const GuestAddress value, const std::uint32_t block) {
                                            return value < kEeAotBlocks[block].begin;
                                        });
    if (found == rt.blocks_by_begin.begin()) {
        return kNoBlock;
    }
    const auto block = *std::prev(found);
    return pc < kEeAotBlocks[block].begin + kEeAotBlocks[block].words * 4U ? block : kNoBlock;
}

// A block's live verdicts: the install-time ones, plus every active session
// stop PC inside it (excluded like a hook PC).
void refresh_block(EeAotRuntime& rt, const std::uint32_t block) {
    auto ok = rt.base_block_ok[block];
    auto last_excluded = rt.base_last_excluded[block];
    auto native_entry = rt.base_native_entry[block];
    const auto exclude = [&](const GuestAddress pc) {
        ok = 0U;
        last_excluded = std::max(last_excluded, pc);
        native_entry = 0U;
    };
    if (rt.session_active) {
        const auto range = std::equal_range(
            rt.session_fixed.begin(), rt.session_fixed.end(), EeAotRuntime::SessionStop{block, 0U},
            [](const auto& left, const auto& right) { return left.block < right.block; });
        for (auto it = range.first; it != range.second; ++it) {
            exclude(it->pc);
        }
        for (std::size_t index = 0U; index < rt.session_extra.size(); ++index) {
            if (rt.session_extra_block[index] == block) {
                exclude(rt.session_extra[index]);
            }
        }
    }
    rt.block_ok[block] = ok;
    rt.block_last_excluded[block] = last_excluded;
    rt.block_native_entry[block] = native_entry;
}

bool function_identical(EeAotRuntime& rt, EeMemory& memory, const std::uint32_t index) {
    const auto epoch = memory.ee_aot_code_write_epoch();
    if (rt.checked_epoch[index] == epoch) [[likely]] {
        return rt.identical[index] != 0U;
    }
    ++g_statistics.identity_checks;
    const auto& info = kEeAotFunctions[index];
    const auto words = (info.end - info.begin) / 4U;
    const bool same = std::memcmp(memory.ee_aot_main_ram() + info.begin, &kEeAotCodeWords[info.first_word],
                                  static_cast<std::size_t>(words) * 4U) == 0;
    g_statistics.identity_failures += same ? 0U : 1U;
    rt.checked_epoch[index] = epoch;
    rt.identical[index] = same ? 1U : 0U;
    rt.verified_epoch[index] = same ? epoch : ~std::uint64_t{0U};
    return same;
}

} // namespace

bool ee_aot_module_linked() noexcept { return kEeAotLinked; }

std::uint64_t ee_aot_compiled_function_count() noexcept { return kEeAotFunctionCount; }

void ee_aot_install(EeMemory& memory, const std::function<bool(GuestAddress)>& excluded,
                    const std::function<bool(GuestAddress)>& declinable_entry) {
    auto& rt = runtime();
    rt = EeAotRuntime{};
    const char* switch_value = std::getenv("TAKEDOWN_EE_AOT");
    if (!kEeAotLinked || (switch_value != nullptr && switch_value[0] == '0') ||
        memory.ee_aot_main_ram_size() != kMainRamBytes) {
        return;
    }
    const auto words = (kEeAotCodeEnd - kEeAotCodeBegin) / 4U;
    rt.entry_by_word.assign(words, 0U);
    for (std::uint32_t index = 0U; index < kEeAotEntryCount; ++index) {
        rt.entry_by_word[(kEeAotEntries[index].pc - kEeAotCodeBegin) / 4U] = index + 1U;
    }
    rt.block_ok.assign(kEeAotBlockCount, 1U);
    rt.block_last_excluded.assign(kEeAotBlockCount, 0U);
    rt.block_native_entry.assign(kEeAotBlockCount, 0U);
    for (std::uint32_t block = 0U; block < kEeAotBlockCount; ++block) {
        const auto& info = kEeAotBlocks[block];
        std::uint32_t excluded_count = 0U;
        bool head_excluded = false;
        for (std::uint32_t word = 0U; word < info.words; ++word) {
            const auto pc = info.begin + word * 4U;
            if (excluded(pc)) {
                rt.block_ok[block] = 0U;
                rt.block_last_excluded[block] = pc;
                head_excluded = head_excluded || word == 0U;
                ++excluded_count;
            }
        }
        rt.block_native_entry[block] =
            excluded_count == 1U && head_excluded && declinable_entry && declinable_entry(info.begin) ? 1U : 0U;
    }
    rt.base_block_ok = rt.block_ok;
    rt.base_last_excluded = rt.block_last_excluded;
    rt.base_native_entry = rt.block_native_entry;
    rt.blocks_by_begin.resize(kEeAotBlockCount);
    std::iota(rt.blocks_by_begin.begin(), rt.blocks_by_begin.end(), 0U);
    std::sort(rt.blocks_by_begin.begin(), rt.blocks_by_begin.end(),
              [](const std::uint32_t left, const std::uint32_t right) {
                  return kEeAotBlocks[left].begin < kEeAotBlocks[right].begin;
              });
    rt.checked_epoch.assign(kEeAotFunctionCount, ~std::uint64_t{0U});
    rt.identical.assign(kEeAotFunctionCount, 0U);
    rt.verified_epoch.assign(kEeAotFunctionCount, ~std::uint64_t{0U});
    std::vector<std::uint64_t> code_words((words + 63U) / 64U, 0U);
    for (std::uint32_t index = 0U; index < kEeAotFunctionCount; ++index) {
        for (auto pc = kEeAotFunctions[index].begin; pc < kEeAotFunctions[index].end; pc += 4U) {
            const auto word = (pc - kEeAotCodeBegin) / 4U;
            code_words[word / 64U] |= std::uint64_t{1U} << (word % 64U);
        }
    }
    memory.set_ee_aot_code_words(kEeAotCodeBegin, std::move(code_words));
    if (const char* path = std::getenv("TAKEDOWN_EE_AOT_MISSES"); path != nullptr && path[0] != '\0') {
        rt.misses_path = path;
        g_record_excluded = true;
        g_excluded.clear();
        g_hook_stops.clear();
        g_native_declines.clear();
    }
    rt.enabled = true;
}

void ee_aot_set_session_stops(const std::span<const GuestAddress> fixed) {
    auto& rt = runtime();
    if (!rt.enabled) {
        return;
    }
    ee_aot_update_session_stops(false);
    rt.session_fixed.clear();
    for (const auto pc : fixed) {
        if (const auto block = block_containing(rt, pc); block != kNoBlock) {
            rt.session_fixed.push_back({block, pc});
        }
    }
    std::sort(rt.session_fixed.begin(), rt.session_fixed.end(), [](const auto& left, const auto& right) {
        return left.block != right.block ? left.block < right.block : left.pc < right.pc;
    });
}

void ee_aot_update_session_stops(const bool active, const GuestAddress extra_a, const GuestAddress extra_b) {
    auto& rt = runtime();
    if (!rt.enabled) {
        return;
    }
    const std::array<GuestAddress, 2> extras{active ? extra_a : 0U, active ? extra_b : 0U};
    if (active == rt.session_active && extras == rt.session_extra) {
        return;
    }
    const auto old_blocks = rt.session_extra_block;
    const bool toggled = active != rt.session_active;
    rt.session_active = active;
    rt.session_extra = extras;
    for (std::size_t index = 0U; index < extras.size(); ++index) {
        rt.session_extra_block[index] = extras[index] != 0U ? block_containing(rt, extras[index]) : kNoBlock;
    }
    if (toggled) {
        for (const auto& stop : rt.session_fixed) {
            refresh_block(rt, stop.block);
        }
    }
    for (std::size_t index = 0U; index < extras.size(); ++index) {
        if (old_blocks[index] != kNoBlock) {
            refresh_block(rt, old_blocks[index]);
        }
        if (rt.session_extra_block[index] != kNoBlock) {
            refresh_block(rt, rt.session_extra_block[index]);
        }
    }
}

EeAotRun run_ee_aot(R5900State& state, EeMemory& memory, const std::uint64_t budget,
                    const std::uint64_t first_step, const GuestAddress declined_native_entry) {
    auto& rt = runtime();
    ++g_statistics.calls;
    if (!rt.enabled || state.is_executing_delay_slot()) {
        return {};
    }
    auto declined = declined_native_entry;
    // The entry the dispatcher takes at `pc` once `executed` instructions of
    // this run are done; nullptr, with the reason counted, when the run ends
    // before it.
    const auto admit = [&](const GuestAddress pc, const std::uint64_t executed) -> const EeAotEntryInfo* {
        const auto slot = pc >= kEeAotCodeBegin && pc < kEeAotCodeEnd && (pc & 3U) == 0U
                              ? rt.entry_by_word[(pc - kEeAotCodeBegin) / 4U]
                              : 0U;
        if (slot == 0U) {
            ++g_statistics.stop_uncompiled;
            if (!rt.misses_path.empty()) [[unlikely]] {
                ++rt.misses[pc];
            }
            return nullptr;
        }
        const auto& entry = kEeAotEntries[slot - 1U];
        // A block holding excluded addresses is entered only at an address
        // after the last of them, or (once) at its first instruction when
        // that is the declined native entry and the only excluded address.
        bool allowed = rt.block_ok[entry.block] != 0U;
        if (!allowed) [[unlikely]] {
            if (pc > rt.block_last_excluded[entry.block]) {
                allowed = true;
            } else if (pc == declined && rt.block_native_entry[entry.block] != 0U &&
                       kEeAotBlocks[entry.block].begin == pc) {
                allowed = true;
                ++g_statistics.native_declined_entries;
            }
        }
        declined = kEeAotNoDeclinedEntry;
        if (budget - executed < entry.maximum || !allowed) {
            if (allowed) {
                note_budget_refusal(budget - executed, executed == 0U);
            } else {
                ++g_statistics.stop_excluded;
                if (g_record_excluded) [[unlikely]] {
                    ++g_excluded[pc];
                }
            }
            return nullptr;
        }
        if (!function_identical(rt, memory, entry.function)) {
            return nullptr;
        }
        ++g_statistics.function_entries;
        return &entry;
    };
    // Most calls end here: the first entry is decided before the context is
    // built.
    auto pc = state.pc();
    const auto* entry = admit(pc, 0U);
    if (entry == nullptr) {
        return {};
    }
    EeAotContext c{};
    EeStepResult callout;
    c.callout = &callout;
    c.state = &state;
    c.memory = &memory;
    c.view = &memory.ee_aot_memory_view();
    c.gpr = state.gpr_data();
    c.hi = state.hi_data();
    c.lo = state.lo_data();
    c.ram = memory.ee_aot_main_ram();
    c.scratchpad = memory.ee_aot_scratchpad();
    c.budget = budget;
    c.first_step = first_step;
    c.hardware_epoch = memory.hardware_activity_epoch();
    c.code_epoch = memory.ee_aot_code_write_epoch();
    c.watch_epoch = memory.ee_aot_watch_epoch();
    c.stores_plain = memory.ee_aot_stores_plain();
    c.block_ok = rt.block_ok.data();
    c.verified_epoch = rt.verified_epoch.data();
    c.code_begin = kEeAotCodeBegin;
    c.code_bytes = kEeAotCodeEnd - kEeAotCodeBegin;
    c.entry_by_word = rt.entry_by_word.data();
    c.entries = kEeAotEntries;
    c.functions = kEeAotFunctions;

    for (;;) {
        if (!kEeAotFunctions[entry->function].function(c, entry->entry)) {
            break;
        }
        pc = c.next_pc;
        entry = admit(pc, c.executed);
        if (entry == nullptr) {
            if (c.executed != 0U) {
                state.set_pc(pc);
            }
            break;
        }
    }
    if (c.executed == 0U) {
        return {};
    }
    ++g_statistics.runs;
    g_statistics.instructions += c.executed;
    EeAotRun run{.executed = c.executed, .last = {}, .last_pc = c.last_pc};
    if (c.callout_last) {
        run.last = std::move(callout);
    } else {
        std::uint32_t word{};
        std::memcpy(&word, c.ram + (c.last_pc & 0x1fffffffU), sizeof(word));
        // Only the address and word: nothing the caller does with an Executed
        // result reads more, and decoding is a fixed cost of every run.
        run.last = {.status = EeStepStatus::Executed, .instruction = {.pc = c.last_pc, .word = word}};
    }
    return run;
}

const EeAotStatistics& ee_aot_statistics() noexcept { return g_statistics; }

namespace {
void write_ranked(const std::string& path, const std::unordered_map<GuestAddress, std::uint64_t>& counts) {
    std::vector<std::pair<std::uint64_t, GuestAddress>> ranked;
    ranked.reserve(counts.size());
    for (const auto& [pc, count] : counts) {
        ranked.emplace_back(count, pc);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    std::ofstream out(path);
    out << "pc\tcount\n";
    char line[32];
    for (const auto& [count, pc] : ranked) {
        std::snprintf(line, sizeof(line), "0x%08x\t", pc);
        out << line << count << '\n';
    }
}
} // namespace

void ee_aot_write_misses() {
    const auto& rt = runtime();
    if (rt.misses_path.empty()) {
        return;
    }
    write_ranked(rt.misses_path, rt.misses);
    write_ranked(rt.misses_path + ".excluded", g_excluded);
    write_ranked(rt.misses_path + ".hooks", g_hook_stops);
    write_ranked(rt.misses_path + ".declines", g_native_declines);
}

bool ee_aot_recording() noexcept { return g_record_excluded; }

void ee_aot_note_native_decline(const GuestAddress pc) {
    if (g_record_excluded) [[unlikely]] {
        ++g_native_declines[pc];
    }
}

void ee_aot_note_hook_stop(const GuestAddress pc) {
    if (g_record_excluded) [[unlikely]] {
        ++g_hook_stops[pc];
    }
}

std::vector<GuestAddress> ee_aot_entry_points() {
    std::vector<GuestAddress> points;
    points.reserve(kEeAotEntryCount);
    for (std::uint32_t index = 0U; index < kEeAotEntryCount; ++index) {
        points.push_back(kEeAotEntries[index].pc);
    }
    return points;
}

} // namespace takedown::guest
