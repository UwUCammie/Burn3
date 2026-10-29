#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/native_function_preflight.hpp"
#include "takedown/runtime/slus_21050_object_packet_capture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace takedown::runtime::slus_21050 {

// Recovery evidence only. This observer never supplies native admission or
// claims a scheduler-transparent replacement. It captures one complete owner
// lifecycle, including RAM pages at every observed call/return so ring reuse
// cannot erase an earlier publication. Device state is described by the
// scheduler snapshot, not serialized as a replayable machine.
inline constexpr std::size_t kInstanceCapturePageSize = 4096;
inline constexpr std::size_t kInstanceCaptureMaximumEvents = 2048;
inline constexpr std::size_t kInstanceCaptureMaximumDeltaBytes = 64U * 1024U * 1024U;
// Coverage selection is opt-in and remains a bounded diagnostic. These totals
// survive a discarded complete lifecycle; the per-lifecycle limits still apply.
inline constexpr std::size_t kInstanceCaptureMaximumSelectionAttempts = 64;
inline constexpr std::size_t kInstanceCaptureMaximumSelectionEvents = 8192;
inline constexpr std::size_t kInstanceCaptureMaximumSelectionDeltaBytes = 256U * 1024U * 1024U;
inline constexpr std::size_t kInstanceCaptureMaximumPrefilterSkips = 4096;

struct InstanceBatchCaptureContext {
    NativeFunctionPreflightState scheduler{}; // Actual, never relaxed polls.
    std::int32_t thread_id{};
    bool guest_callback_active{};
};
struct InstanceBatchCaptureMarker {
    std::uint32_t entry{}, depth{};
    bool returning{}, begin{}, complete{};
};
struct InstanceBatchCapturePage {
    std::uint32_t index{};
    std::array<std::byte, kInstanceCapturePageSize> bytes{};
};
struct InstanceBatchCaptureEvent {
    InstanceBatchCaptureMarker marker;
    InstanceBatchCaptureContext context;
    // Existing field-by-field CPU format; before==after for this boundary.
    // Regions are scratchpad, VU0 code and VU0 data, also before==after.
    ObjectPacketCase boundary;
    std::vector<InstanceBatchCapturePage> ram_pages;
};
struct InstanceBatchCaptureCase {
    std::uint32_t owner{};
    std::vector<std::byte> ram_before, ram_after;
    std::vector<InstanceBatchCaptureEvent> events;
};

// Kept separate from IO so call ordering and rejection can be tested without
// a renderer, guest execution, filesystem, or 32 MiB snapshots.
class InstanceBatchCaptureTracker {
public:
    static constexpr std::uint32_t setup = 0x26a7e0U, view = 0x26a710U,
        append = 0x26a610U, flush = 0x26a240U, drain = 0x26a1c0U;
    static constexpr std::array<std::uint32_t, 10> entries{
        setup, view, append, flush, drain, 0x26b2f0U, 0x26baa0U,
        0x1f3080U, 0x103d20U, 0x1042f0U};
    // Epilogues are defensive static marks; dynamic RA returns still require
    // a one-instruction compiled budget while active.
    static constexpr std::array<std::uint32_t, 15> hook_addresses{
        setup, view, append, flush, drain, 0x26b2f0U, 0x26baa0U,
        0x1f3080U, 0x103d20U, 0x1042f0U,
        0x26ae20U, 0x26a7d4U, 0x26a704U, 0x26a608U, 0x26a234U};

    explicit InstanceBatchCaptureTracker(std::size_t event_limit = kInstanceCaptureMaximumEvents)
        : event_limit_(event_limit) {
        if (!event_limit || event_limit > kInstanceCaptureMaximumEvents)
            throw std::runtime_error("invalid instance capture event limit");
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool completed() const noexcept { return completed_; }
    [[nodiscard]] std::uint32_t owner() const noexcept { return owner_; }
    // Valid immediately after an entry observation pushed the current call.
    // Selection uses the tracked parent as well as the observed return PC.
    [[nodiscard]] std::uint32_t current_call_parent_entry() const noexcept {
        return stack_.size() > 1 ? stack_[stack_.size() - 2].entry : 0U;
    }
    [[nodiscard]] bool wants_pc(std::uint32_t pc) const noexcept {
        if (completed_) return false;
        if (!active_) return pc == setup;
        return std::find(entries.begin(), entries.end(), pc) != entries.end() ||
            std::any_of(stack_.begin(), stack_.end(), [pc](const Frame& f) { return f.ra == pc; });
    }
    std::optional<InstanceBatchCaptureMarker> observe(const guest::R5900State& cpu,
        const InstanceBatchCaptureContext& context, bool measured) {
        if (failed_) throw std::runtime_error("instance capture previously rejected");
        if (completed_) return std::nullopt;
        const auto pc = cpu.pc();
        bool beginning = false;
        if (!active_) {
            if (pc != setup || !measured) return std::nullopt;
            if (context.guest_callback_active) reject("instance setup entered from a guest callback");
            active_ = beginning = true;
            owner_ = cpu.read_gpr_u32(4);
            thread_ = context.thread_id;
            last_step_ = context.scheduler.committed_instructions;
            last_cycles_ = context.scheduler.ee_cycles;
        }
        // Must be called at EVERY dispatch while active, not only wants_pc().
        if (context.thread_id != thread_ || context.guest_callback_active)
            reject("instance capture crossed a guest thread or callback boundary");
        if (context.scheduler.committed_instructions < last_step_ ||
            context.scheduler.ee_cycles < last_cycles_)
            reject("instance capture clock moved backwards");
        if (context.scheduler.executing_delay_slot != cpu.is_executing_delay_slot())
            reject("instance capture scheduler/CPU delay-slot mismatch");
        last_step_ = context.scheduler.committed_instructions;
        last_cycles_ = context.scheduler.ee_cycles;
        if (!wants_pc(pc)) return std::nullopt;
        if (cpu.is_executing_delay_slot() || cpu.next_pc() != pc + 4U)
            reject("instance capture boundary is not sequential");
        for (std::size_t index = 0; index < stack_.size(); ++index) {
            const auto frame = stack_[index];
            if (pc != frame.ra) continue;
            if (index + 1 != stack_.size()) reject("instance capture returned across a pending call");
            if (cpu.read_gpr_u32(29) != frame.sp) reject("instance capture return SP changed");
            if (cpu.read_gpr_u32(31) != frame.ra) reject("instance capture return RA changed");
            stack_.pop_back();
            if (frame.entry == setup) setup_returned_ = true;
            if (frame.entry == view) ++views_;
            if (frame.entry == drain) { active_ = false; completed_ = true; }
            return event({frame.entry, static_cast<std::uint32_t>(stack_.size()),
                          true, false, frame.entry == drain});
        }
        if (std::find(entries.begin(), entries.end(), pc) == entries.end()) return std::nullopt;
        const bool lifecycle = pc == setup || pc == view || pc == append || pc == flush || pc == drain;
        if (lifecycle && cpu.read_gpr_u32(4) != owner_) reject("instance capture owner changed");
        if (pc == setup && !beginning) reject("nested instance setup");
        if (pc == view && (!setup_returned_ || !stack_.empty() || appending_ || views_ >= 3))
            reject("invalid instance view registration order");
        if (pc == append || pc == drain) {
            if (!setup_returned_ || !stack_.empty() || !views_)
                reject("instance append or drain before complete setup and views");
            appending_ = true;
        }
        if (pc == flush && (stack_.empty() ||
            (stack_.back().entry != append && stack_.back().entry != drain)))
            reject("instance flush outside an append or final drain");
        if (!lifecycle && stack_.empty()) reject("instance helper outside a lifecycle operation");
        if (stack_.size() >= 16) reject("instance capture call depth exceeded");
        const auto ra = cpu.read_gpr_u32(31), sp = cpu.read_gpr_u32(29);
        if ((ra & 3U) || !ra || (sp & 15U)) reject("invalid instance capture continuation");
        const auto depth = static_cast<std::uint32_t>(stack_.size());
        stack_.push_back({pc, ra, sp});
        return event({pc, depth, false, beginning, false});
    }
    void finish() const {
        if (failed_ || !completed_ || active_ || !stack_.empty())
            throw std::runtime_error("instance capture has no complete lifecycle");
    }
private:
    struct Frame { std::uint32_t entry, ra, sp; };
    [[noreturn]] void reject(const char* text) { failed_ = true; throw std::runtime_error(text); }
    InstanceBatchCaptureMarker event(InstanceBatchCaptureMarker result) {
        if (++events_ > event_limit_) reject("instance capture event limit exceeded");
        return result;
    }
    std::vector<Frame> stack_;
    std::size_t event_limit_{}, events_{}, last_step_{};
    std::uint64_t last_cycles_{};
    std::uint32_t owner_{}, views_{};
    std::int32_t thread_{};
    bool active_{}, completed_{}, failed_{}, setup_returned_{}, appending_{};
};

namespace instance_capture_detail {
enum class SelectionFlush { none, full, final };

struct SetupPotential {
    std::uint32_t caller_return{}, owner{}, scene{}, type{}, record{}, camera{}, mode{};
    std::uint32_t positive_inputs{}, eligible_lists{};
};

// Capture selection heuristic only: these are the lists at setup entry, not
// a promise that callbacks leave them unchanged or that culling accepts them.
// An unknown caller or malformed ordinary-RAM identity falls back to capture.
inline std::optional<SetupPotential> probe_setup_potential(
    const guest::R5900State& cpu, std::span<const std::byte> ram) {
    if (cpu.pc() != InstanceBatchCaptureTracker::setup || cpu.is_executing_delay_slot() ||
        cpu.next_pc() != cpu.pc() + 4U) return std::nullopt;
    const auto ra = cpu.read_gpr_u32(31);
    unsigned scene_register{}, type_register{}, model_offset{}, parameter_offset{}, flags_mask{};
    std::uint32_t delay_word{};
    switch (ra) {
    case 0x21c214U: scene_register = 17; type_register = 20; model_offset = 0x38;
        parameter_offset = 0x20; flags_mask = 3; delay_word = 0x4600a386U; break;
    case 0x21c2f0U: scene_register = 17; type_register = 20; model_offset = 0x3a;
        parameter_offset = 0x28; flags_mask = 3; delay_word = 0x00433821U; break;
    case 0x21cc4cU: scene_register = 20; type_register = 18; model_offset = 0x3c;
        parameter_offset = 0x30; flags_mask = 0xfc; delay_word = 0x00433821U; break;
    case 0x21cd88U: scene_register = 20; type_register = 18; model_offset = 0x38;
        parameter_offset = 0x20; flags_mask = 3; delay_word = 0x0002400aU; break;
    case 0x21ce64U: scene_register = 20; type_register = 18; model_offset = 0x3a;
        parameter_offset = 0x28; flags_mask = 3; delay_word = 0x00433821U; break;
    default: return std::nullopt;
    }
    const auto contains = [&](std::uint64_t address, std::size_t bytes) {
        return address != 0 && address < ram.size() && bytes <= ram.size() - address;
    };
    const auto word = [&](std::uint64_t address, unsigned bytes = 4U) -> std::optional<std::uint32_t> {
        if (!contains(address, bytes)) return std::nullopt;
        std::uint32_t value{};
        for (unsigned n = 0; n < bytes; ++n)
            value |= std::uint32_t{std::to_integer<unsigned>(ram[static_cast<std::size_t>(address) + n])} << (8U * n);
        return value;
    };
    const auto call = word(ra - 8U), delay = word(ra - 4U), scene_pointer = word(0x64c994U);
    const auto scene = cpu.read_gpr_u32(scene_register), type = cpu.read_gpr_u32(type_register);
    const auto camera = cpu.read_gpr_u32(21), record = cpu.read_gpr_u32(6);
    if (!call || *call != 0x0c09a9f8U || !delay || *delay != delay_word ||
        !scene_pointer || *scene_pointer != scene || !contains(scene, 0x3c) ||
        !contains(camera, 0x150) || cpu.read_gpr_u32(4) != 0x1e32a10U ||
        !contains(cpu.read_gpr_u32(4), 0x90)) return std::nullopt;
    const auto type_count = word(std::uint64_t{scene} + 0x34U, 2);
    const auto records = word(std::uint64_t{scene} + 0x38U);
    const auto model_table = word(std::uint64_t{scene} + 8U);
    if (!type_count || (*type_count & 0x8000U) || type >= *type_count || !records || !*records ||
        !model_table || !*model_table || std::uint64_t{*records} + 80U * type != record ||
        cpu.read_gpr_u32(16) != 80U * type || !contains(record, 80) ||
        std::uint64_t{record} + parameter_offset != cpu.read_gpr_u32(5)) return std::nullopt;
    const auto model = word(std::uint64_t{record} + model_offset, 2);
    const auto flags = word(std::uint64_t{record} + 0x3eU, 2);
    const auto expected_mode = parameter_offset == 0x30U ? 0U : parameter_offset == 0x28U ? 2U :
        (flags && (*flags & 1U) ? 1U : 0U);
    if (!model || (*model & 0x8000U) || !flags ||
        std::uint64_t{*model_table} + 40U * *model != cpu.read_gpr_u32(7) ||
        !contains(cpu.read_gpr_u32(7), 40) || cpu.read_gpr_u32(8) != expected_mode ||
        (parameter_offset == 0x28U && !(*flags & 1U)) ||
        (parameter_offset == 0x30U && !(*flags & 2U))) return std::nullopt;
    SetupPotential result{ra, cpu.read_gpr_u32(4), scene, type, record, camera, expected_mode};
    for (unsigned slot = 0; slot < 17; ++slot) {
        const auto slot_flags = word(std::uint64_t{camera} + 0x58U + 2U * slot, 2);
        if (!slot_flags || !(*slot_flags & flags_mask)) continue;
        const auto descriptor = word(std::uint64_t{camera} + 0x10cU + 4U * slot);
        if (!descriptor || (!*descriptor && flags_mask != 0xfcU)) return std::nullopt;
        if (!*descriptor) continue; // The three-view caller explicitly skips null lists.
        if (!contains(*descriptor, 8)) return std::nullopt;
        const auto counts = word(*descriptor), transforms = word(std::uint64_t{*descriptor} + 4U);
        if (!counts || !*counts || !transforms) return std::nullopt;
        const auto count = word(std::uint64_t{*counts} + type, 1);
        if (!count) return std::nullopt;
        ++result.eligible_lists;
        // The original append count is signed; negative values do no work.
        if (!*count || (*count & 0x80U)) continue;
        if (!*transforms) return std::nullopt;
        const auto source = word(std::uint64_t{*transforms} + 4U * type);
        if (!source || (*source & 15U) || !contains(*source, std::size_t{*count} * 64U))
            return std::nullopt;
        result.positive_inputs += *count;
    }
    return result;
}

class SetupPrefilter {
public:
    // true starts a normal complete capture; false skips only this setup.
    bool capture(std::optional<SetupPotential> candidate) {
        if (candidate && candidate->positive_inputs < 14) {
            if (skipped_ == kInstanceCaptureMaximumPrefilterSkips)
                throw std::runtime_error("instance capture prefilter skip limit exceeded");
            ++skipped_; last_skipped_ = candidate; return false;
        }
        if (candidates_.size() >= kInstanceCaptureMaximumSelectionAttempts)
            throw std::runtime_error("instance capture prefilter attempt limit exceeded");
        if (!candidate) ++unknown_;
        candidates_.push_back(candidate); return true;
    }
    [[nodiscard]] auto skipped() const noexcept { return skipped_; }
    [[nodiscard]] auto unknown() const noexcept { return unknown_; }
    [[nodiscard]] const auto& last_skipped() const noexcept { return last_skipped_; }
    [[nodiscard]] const auto& candidates() const noexcept { return candidates_; }
private:
    std::size_t skipped_{}, unknown_{};
    std::optional<SetupPotential> last_skipped_;
    std::vector<std::optional<SetupPotential>> candidates_;
};

// This only classifies a coverage witness. The standalone verifier still has
// to authenticate the original code and compare every record and flush.
inline SelectionFlush selection_flush(const InstanceBatchCaptureMarker& marker,
    const guest::R5900State& cpu, std::uint32_t parent, std::uint32_t owner,
    std::span<const std::byte> ram, std::span<const std::byte> vu0_data) {
    using Tracker = InstanceBatchCaptureTracker;
    if (marker.returning || marker.entry != Tracker::flush) return SelectionFlush::none;
    const auto ra = cpu.read_gpr_u32(31);
    const bool full = parent == Tracker::append && ra == 0x26a69cU;
    const bool final = parent == Tracker::drain && ra == 0x26a204U;
    if ((!full && !final) || marker.depth != 1 || cpu.read_gpr_u32(4) != owner)
        throw std::runtime_error("instance selection flush has an invalid parent or continuation");
    if (owner >= ram.size() || ram.size() - owner <= 0x86U ||
        vu0_data.size() != guest::kVu0MemorySize)
        throw std::runtime_error("instance selection flush has invalid owner or local memory");
    const auto byte = [&](std::size_t offset) { return std::to_integer<unsigned>(ram[owner + offset]); };
    const auto views = byte(0x84), view = byte(0x85), count = byte(0x86);
    if (!views || views > 3 || view >= views)
        throw std::runtime_error("instance selection flush has an invalid selected view");
    const auto offset = std::size_t{view} * 71U * 16U;
    std::uint32_t header{};
    for (unsigned lane = 0; lane < 4; ++lane)
        header |= std::uint32_t{std::to_integer<unsigned>(vu0_data[offset + lane])} << (lane * 8);
    if (header != count || (full ? count != 70 : (!count || count >= 70 || count % 5 != 0)))
        throw std::runtime_error("instance selection flush has an invalid queue count");
    return full ? SelectionFlush::full : SelectionFlush::final;
}

struct SelectionAttempt {
    std::uint32_t owner{};
    std::size_t events{}, delta_bytes{}, full_flushes{}, final_flushes{};
    bool selected{};
};

// Pure bounded accounting, separate from tracker validation and file IO. A
// caller may complete an attempt only after validating its full capture.
class CaptureSelection {
public:
    explicit CaptureSelection(bool require_full_flush = false) : require_full_flush_(require_full_flush) {}
    [[nodiscard]] bool requires_full_flush() const noexcept { return require_full_flush_; }
    [[nodiscard]] const auto& attempts() const noexcept { return attempts_; }
    [[nodiscard]] std::size_t started() const noexcept { return started_; }
    [[nodiscard]] std::size_t total_events() const noexcept { return total_events_; }
    [[nodiscard]] std::size_t total_delta_bytes() const noexcept { return total_delta_bytes_; }
    void begin(std::uint32_t owner) {
        if (active_ || selected_ || started_ >= kInstanceCaptureMaximumSelectionAttempts)
            throw std::runtime_error("instance capture selection attempt limit or state violated");
        ++started_; active_ = true; current_ = {}; current_.owner = owner;
    }
    void event(SelectionFlush flush) {
        if (!active_ || (require_full_flush_ && total_events_ >= kInstanceCaptureMaximumSelectionEvents))
            throw std::runtime_error("instance capture aggregate event limit or state violated");
        ++total_events_; ++current_.events;
        if (flush == SelectionFlush::full) ++current_.full_flushes;
        if (flush == SelectionFlush::final) ++current_.final_flushes;
    }
    void delta(std::size_t bytes) {
        if (!active_ || (require_full_flush_ &&
            bytes > kInstanceCaptureMaximumSelectionDeltaBytes - total_delta_bytes_))
            throw std::runtime_error("instance capture aggregate RAM delta limit or state violated");
        total_delta_bytes_ += bytes; current_.delta_bytes += bytes;
    }
    [[nodiscard]] bool complete_validated() {
        if (!active_) throw std::runtime_error("instance capture selection has no active attempt");
        current_.selected = !require_full_flush_ || current_.full_flushes != 0;
        attempts_.push_back(current_); active_ = false; selected_ = current_.selected;
        if (!selected_ && started_ == kInstanceCaptureMaximumSelectionAttempts)
            throw std::runtime_error("instance capture exhausted 64 complete lifecycles without a full flush");
        return selected_;
    }
private:
    bool require_full_flush_{}, active_{}, selected_{};
    std::size_t started_{}, total_events_{}, total_delta_bytes_{};
    SelectionAttempt current_;
    std::vector<SelectionAttempt> attempts_;
};

inline void put(std::ostream& out, std::uint64_t value, unsigned bytes = 4) {
    for (unsigned i = 0; i < bytes; ++i) { out.put(static_cast<char>(value & 255U)); value >>= 8U; }
}
inline std::uint64_t get(std::istream& in, unsigned bytes = 4) {
    std::uint64_t result{};
    for (unsigned i = 0; i < bytes; ++i) {
        const auto value = in.get();
        if (value == std::char_traits<char>::eof()) throw std::runtime_error("truncated instance capture");
        result |= std::uint64_t{static_cast<unsigned char>(value)} << (8U * i);
    }
    return result;
}
inline bool boolean(std::istream& in) {
    const auto value = get(in);
    if (value > 1) throw std::runtime_error("invalid instance capture boolean");
    return value != 0;
}
inline void context_out(std::ostream& out, const InstanceBatchCaptureContext& c) {
    const auto& s = c.scheduler;
    put(out, s.committed_instructions, 8); put(out, s.instruction_budget, 8);
    put(out, s.ee_cycles, 8); put(out, s.next_vblank_cycle, 8);
    put(out, s.next_alarm_cycle.has_value()); put(out, s.next_alarm_cycle.value_or(0), 8);
    put(out, s.queued_dmac_completions, 8); put(out, s.gif_dma_in_flight); put(out, s.vif1_dma_in_flight);
    put(out, s.pending_dmac_callbacks, 8); put(out, s.pending_sif_callbacks, 8);
    put(out, s.sif_callback_poll_can_progress); put(out, s.sif_dma_completion_pending);
    put(out, s.competing_ready_thread); put(out, s.pending_intc_causes); put(out, s.executing_delay_slot);
    put(out, static_cast<std::uint32_t>(c.thread_id)); put(out, c.guest_callback_active);
}
inline InstanceBatchCaptureContext context_in(std::istream& in) {
    InstanceBatchCaptureContext c; auto& s = c.scheduler;
    s.committed_instructions = get(in, 8); s.instruction_budget = get(in, 8);
    s.ee_cycles = get(in, 8); s.next_vblank_cycle = get(in, 8);
    const auto alarm = boolean(in); const auto deadline = get(in, 8);
    if (alarm) s.next_alarm_cycle = deadline;
    s.queued_dmac_completions = get(in, 8); s.gif_dma_in_flight = boolean(in); s.vif1_dma_in_flight = boolean(in);
    s.pending_dmac_callbacks = get(in, 8); s.pending_sif_callbacks = get(in, 8);
    s.sif_callback_poll_can_progress = boolean(in); s.sif_dma_completion_pending = boolean(in);
    s.competing_ready_thread = boolean(in); s.pending_intc_causes = static_cast<std::uint32_t>(get(in));
    s.executing_delay_slot = boolean(in); c.thread_id = static_cast<std::int32_t>(get(in));
    c.guest_callback_active = boolean(in); return c;
}
inline void read_bytes(std::istream& in, std::span<std::byte> bytes) {
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw std::runtime_error("truncated instance capture bytes");
}
inline void write_bytes(std::ostream& out, std::span<const std::byte> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
inline void validate(const InstanceBatchCaptureCase& value) {
    if (value.ram_before.size() != guest::kEeMainRamSize || value.ram_after.size() != guest::kEeMainRamSize ||
        value.events.empty() || value.events.size() > kInstanceCaptureMaximumEvents)
        throw std::runtime_error("invalid instance capture extent");
    auto ram = value.ram_before;
    std::size_t delta_bytes{};
    InstanceBatchCaptureTracker tracker;
    for (std::size_t n = 0; n < value.events.size(); ++n) {
        const auto& e = value.events[n];
        if (!same_object_packet_cpu(e.boundary.before, e.boundary.after) ||
            e.boundary.sequence != n || e.boundary.retired != 0)
            throw std::runtime_error("invalid instance CPU boundary record");
        const auto marker = tracker.observe(e.boundary.before, e.context, true);
        if (!marker || marker->entry != e.marker.entry || marker->depth != e.marker.depth ||
            marker->returning != e.marker.returning || marker->begin != e.marker.begin ||
            marker->complete != e.marker.complete || (e.marker.complete && n + 1 != value.events.size()))
            throw std::runtime_error("invalid instance capture event ordering");
        constexpr std::array<std::uint32_t, 3> addresses{0x70000000U, guest::kVu0MicroMemoryStart, guest::kVu0DataMemoryStart};
        constexpr std::array<std::size_t, 3> sizes{guest::kEeScratchpadSize, guest::kVu0MemorySize, guest::kVu0MemorySize};
        if (e.boundary.regions.size() != addresses.size()) throw std::runtime_error("invalid instance local regions");
        for (std::size_t region = 0; region < addresses.size(); ++region) {
            const auto& r = e.boundary.regions[region];
            if (r.address != addresses[region] || r.before.size() != sizes[region] || r.before != r.after)
                throw std::runtime_error("invalid instance local region snapshot");
        }
        std::uint32_t previous{};
        for (std::size_t p = 0; p < e.ram_pages.size(); ++p) {
            const auto& page = e.ram_pages[p];
            if (page.index >= guest::kEeMainRamSize / kInstanceCapturePageSize || (p && page.index <= previous))
                throw std::runtime_error("invalid instance RAM page ordering");
            previous = page.index;
            delta_bytes += kInstanceCapturePageSize;
            if (delta_bytes > kInstanceCaptureMaximumDeltaBytes) throw std::runtime_error("instance RAM delta limit exceeded");
            std::copy(page.bytes.begin(), page.bytes.end(), ram.begin() + page.index * kInstanceCapturePageSize);
        }
        if (n == 0 && !e.ram_pages.empty()) throw std::runtime_error("initial instance boundary has RAM deltas");
    }
    tracker.finish();
    if (tracker.owner() != value.owner || ram != value.ram_after)
        throw std::runtime_error("instance RAM reconstruction or owner mismatch");
}
} // namespace instance_capture_detail

inline void write_instance_batch_capture(std::ostream& out, const InstanceBatchCaptureCase& value) {
    using namespace instance_capture_detail;
    validate(value);
    out.write("B3INST01", 8); put(out, value.owner); put(out, guest::kEeMainRamSize);
    write_bytes(out, value.ram_before); put(out, value.events.size());
    for (const auto& e : value.events) {
        put(out, e.marker.entry); put(out, e.marker.depth); put(out, e.marker.returning);
        put(out, e.marker.begin); put(out, e.marker.complete); context_out(out, e.context);
        std::ostringstream boundary(std::ios::out | std::ios::binary);
        write_object_packet_case(boundary, e.boundary); const auto blob = boundary.str();
        put(out, blob.size()); out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        put(out, e.ram_pages.size());
        for (const auto& page : e.ram_pages) { put(out, page.index); write_bytes(out, page.bytes); }
    }
    write_bytes(out, value.ram_after); out.write("COMPLETE", 8);
    if (!out) throw std::runtime_error("instance capture write failed");
}
inline InstanceBatchCaptureCase read_instance_batch_capture(std::istream& in) {
    using namespace instance_capture_detail;
    char magic[8]{}; in.read(magic, 8);
    if (std::string_view{magic, 8} != "B3INST01") throw std::runtime_error("invalid instance capture magic");
    InstanceBatchCaptureCase value; value.owner = static_cast<std::uint32_t>(get(in));
    if (get(in) != guest::kEeMainRamSize) throw std::runtime_error("invalid instance capture RAM size");
    value.ram_before.resize(guest::kEeMainRamSize); read_bytes(in, value.ram_before);
    const auto count = get(in);
    if (!count || count > kInstanceCaptureMaximumEvents) throw std::runtime_error("invalid instance capture event count");
    std::size_t delta_bytes{};
    for (std::size_t n = 0; n < count; ++n) {
        InstanceBatchCaptureEvent e;
        e.marker.entry = static_cast<std::uint32_t>(get(in)); e.marker.depth = static_cast<std::uint32_t>(get(in));
        e.marker.returning = boolean(in); e.marker.begin = boolean(in); e.marker.complete = boolean(in);
        e.context = context_in(in);
        const auto size = get(in);
        if (size > 65536U) throw std::runtime_error("oversized instance CPU boundary");
        std::string blob(static_cast<std::size_t>(size), '\0'); in.read(blob.data(), static_cast<std::streamsize>(size));
        if (!in) throw std::runtime_error("truncated instance CPU boundary");
        std::istringstream boundary(blob, std::ios::in | std::ios::binary);
        e.boundary = read_object_packet_case(boundary);
        const auto pages = get(in);
        if (pages > guest::kEeMainRamSize / kInstanceCapturePageSize ||
            pages * kInstanceCapturePageSize > kInstanceCaptureMaximumDeltaBytes - delta_bytes)
            throw std::runtime_error("instance RAM delta limit exceeded");
        delta_bytes += pages * kInstanceCapturePageSize;
        e.ram_pages.resize(static_cast<std::size_t>(pages));
        for (auto& page : e.ram_pages) { page.index = static_cast<std::uint32_t>(get(in)); read_bytes(in, page.bytes); }
        value.events.push_back(std::move(e));
    }
    value.ram_after.resize(guest::kEeMainRamSize); read_bytes(in, value.ram_after);
    in.read(magic, 8);
    if (!in || std::string_view{magic, 8} != "COMPLETE" || in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("incomplete or trailing instance capture data");
    validate(value); return value;
}

class InstanceBatchCapture {
public:
    InstanceBatchCapture() : InstanceBatchCapture(environment_directory(), environment_require_full_flush(),
                                                  environment_prefilter()) {}
    // The path-only constructor retains first-complete selection. Requiring a
    // full flush changes diagnostic coverage only, never guest inputs or work.
    explicit InstanceBatchCapture(std::filesystem::path directory, bool require_full_flush = false,
                                  bool prefilter = false)
        : directory_(std::move(directory)), selection_(require_full_flush), prefilter_enabled_(prefilter) {
        if (prefilter && !require_full_flush)
            throw std::runtime_error("instance capture prefilter requires full-flush selection");
    }
    [[nodiscard]] bool enabled() const noexcept { return !directory_.empty(); }
    [[nodiscard]] bool active() const noexcept { return enabled() && tracker_.active(); }
    [[nodiscard]] bool wants_pc(std::uint32_t pc) const noexcept { return enabled() && tracker_.wants_pc(pc); }
    [[nodiscard]] static constexpr auto hook_addresses() noexcept { return InstanceBatchCaptureTracker::hook_addresses; }
    void observe(const guest::R5900State& cpu, const guest::EeMemory& memory,
                 const InstanceBatchCaptureContext& context, bool measured) {
        if (!enabled()) return;
        if (rejected_) throw std::runtime_error("instance capture previously rejected");
        try {
            if (prefilter_enabled_ && !tracker_.active() && !tracker_.completed() && measured &&
                cpu.pc() == InstanceBatchCaptureTracker::setup) {
                // Skips happen before tracker acquisition, scalar-budget restriction,
                // RAM snapshots or event/delta accounting. This is not admission.
                const auto candidate = !context.guest_callback_active &&
                    context.scheduler.executing_delay_slot == cpu.is_executing_delay_slot()
                    ? instance_capture_detail::probe_setup_potential(cpu, memory.bytes()) : std::nullopt;
                claim_directory();
                if (!prefilter_.capture(candidate)) { status("searching"); return; }
            }
            const auto marker = tracker_.observe(cpu, context, measured);
            if (!marker) return;
            const auto ram = memory.bytes(); // Const overload: no mutable escape.
            if (ram.size() != guest::kEeMainRamSize) throw std::runtime_error("instance capture requires complete EE RAM");
            if (marker->begin) {
                claim_directory();
                selection_.begin(tracker_.owner());
                status("incomplete");
                value_.owner = tracker_.owner(); value_.ram_before.assign(ram.begin(), ram.end());
                previous_ram_ = value_.ram_before;
            }
            const auto flush = selection_.requires_full_flush()
                ? instance_capture_detail::selection_flush(*marker, cpu, tracker_.current_call_parent_entry(),
                    tracker_.owner(), ram, memory.view(guest::kVu0DataMemoryStart, guest::kVu0MemorySize))
                : instance_capture_detail::SelectionFlush::none;
            selection_.event(flush);
            InstanceBatchCaptureEvent e; e.marker = *marker; e.context = context;
            e.boundary.sequence = value_.events.size(); e.boundary.before = e.boundary.after = cpu;
            for (const auto [address, size] : std::array<std::pair<std::uint32_t, std::size_t>, 3>{{
                    {0x70000000U, guest::kEeScratchpadSize},
                    {guest::kVu0MicroMemoryStart, guest::kVu0MemorySize},
                    {guest::kVu0DataMemoryStart, guest::kVu0MemorySize}}}) {
                const auto bytes = memory.view(address, size);
                ObjectPacketRegion region; region.address = address;
                region.before.assign(bytes.begin(), bytes.end()); region.after = region.before;
                e.boundary.regions.push_back(std::move(region));
            }
            for (std::size_t offset = 0; offset < ram.size(); offset += kInstanceCapturePageSize) {
                if (std::equal(ram.begin() + offset, ram.begin() + offset + kInstanceCapturePageSize,
                               previous_ram_.begin() + offset)) continue;
                if (delta_bytes_ + kInstanceCapturePageSize > kInstanceCaptureMaximumDeltaBytes)
                    throw std::runtime_error("instance RAM delta limit exceeded");
                selection_.delta(kInstanceCapturePageSize);
                delta_bytes_ += kInstanceCapturePageSize;
                InstanceBatchCapturePage page; page.index = static_cast<std::uint32_t>(offset / kInstanceCapturePageSize);
                std::copy_n(ram.begin() + offset, kInstanceCapturePageSize, page.bytes.begin());
                std::copy(page.bytes.begin(), page.bytes.end(), previous_ram_.begin() + offset);
                e.ram_pages.push_back(std::move(page));
            }
            value_.events.push_back(std::move(e));
            if (marker->complete) {
                value_.ram_after.assign(ram.begin(), ram.end());
                // A miss is discarded only after all ordinary capture gates,
                // including final RAM reconstruction, have passed. Invalid or
                // incomplete attempts always reject the entire diagnostic.
                tracker_.finish();
                if (selection_.requires_full_flush()) {
                    instance_capture_detail::validate(value_);
                    if (!selection_.complete_validated()) {
                        value_ = {}; std::vector<std::byte>{}.swap(previous_ram_); delta_bytes_ = 0;
                        tracker_ = InstanceBatchCaptureTracker{};
                        status("searching");
                        return;
                    }
                }
                const auto partial = directory_ / "lifecycle.b3instance.partial";
                std::ofstream out(partial, std::ios::binary);
                write_instance_batch_capture(out, value_); out.close();
                if (!out) throw std::runtime_error("cannot finish instance capture");
                if (!selection_.requires_full_flush()) static_cast<void>(selection_.complete_validated());
                std::filesystem::rename(partial, directory_ / "lifecycle.b3instance");
                status("complete"); saved_ = true;
                value_ = {}; previous_ram_.clear(); previous_ram_.shrink_to_fit();
            }
        } catch (...) { rejected_ = true; try { if (claimed_) status("rejected"); } catch (...) {} throw; }
    }
    void finish() {
        if (!enabled()) return;
        if (rejected_ || !saved_) {
            rejected_ = true;
            if (claimed_) status("rejected");
            throw std::runtime_error("instance capture did not produce a complete lifecycle");
        }
        tracker_.finish();
    }
private:
    void claim_directory() {
        if (!claimed_) {
            if (std::filesystem::exists(directory_)) throw std::runtime_error("refusing to overwrite instance capture directory");
            if (!std::filesystem::create_directories(directory_))
                throw std::runtime_error("instance capture directory ownership changed");
            claimed_ = true;
        }
    }
    static std::filesystem::path environment_directory() {
        const auto* value = std::getenv("TAKEDOWN_INSTANCE_BATCH_CAPTURE_DIR");
        return value && *value ? std::filesystem::path{value} : std::filesystem::path{};
    }
    static bool environment_require_full_flush() {
        const auto* value = std::getenv("TAKEDOWN_INSTANCE_BATCH_CAPTURE_REQUIRE_FULL_FLUSH");
        return value && std::string_view{value} == "1";
    }
    static bool environment_prefilter() {
        const auto* value = std::getenv("TAKEDOWN_INSTANCE_BATCH_CAPTURE_PREFILTER");
        return value && std::string_view{value} == "1";
    }
    void status(const char* state) const {
        std::ofstream out(directory_ / "status.json");
        out << "{\"schema\":1,\"status\":\"" << state
            << "\",\"diagnostic_only\":true,\"performance_qualified\":false,"
               "\"device_state_replayable\":false,\"events\":" << value_.events.size()
            << ",\"ram_delta_bytes\":" << delta_bytes_;
        // Keep the default status and B3INST01 format unchanged. Extra coverage
        // metadata exists only for an explicitly selected full-flush search.
        if (selection_.requires_full_flush()) {
            out << ",\"require_full_flush\":true,\"attempt_limit\":" << kInstanceCaptureMaximumSelectionAttempts
                << ",\"aggregate_event_limit\":" << kInstanceCaptureMaximumSelectionEvents
                << ",\"aggregate_ram_delta_limit\":" << kInstanceCaptureMaximumSelectionDeltaBytes
                << ",\"attempts_started\":" << selection_.started()
                << ",\"total_events\":" << selection_.total_events()
                << ",\"total_ram_delta_bytes\":" << selection_.total_delta_bytes()
                << ",\"completed_attempts\":[";
            bool first = true;
            for (const auto& attempt : selection_.attempts()) {
                if (!first) out << ',';
                first = false;
                out << "{\"owner\":" << attempt.owner << ",\"events\":" << attempt.events
                    << ",\"ram_delta_bytes\":" << attempt.delta_bytes
                    << ",\"full_flushes\":" << attempt.full_flushes << ",\"final_flushes\":" << attempt.final_flushes
                    << ",\"selected\":" << (attempt.selected ? "true" : "false")
                    << ",\"reason\":\"" << (attempt.selected ? "full_flush" : "no_full_flush") << "\"}";
            }
            out << ']';
        }
        if (prefilter_enabled_) {
            const auto identity = [&](const std::optional<instance_capture_detail::SetupPotential>& value) {
                if (!value) { out << "{\"identified\":false,\"reason\":\"unknown_identity_capture_fallback\"}"; return; }
                out << "{\"identified\":true,\"caller_return\":" << value->caller_return
                    << ",\"owner\":" << value->owner << ",\"scene\":" << value->scene
                    << ",\"type\":" << value->type << ",\"record\":" << value->record
                    << ",\"camera\":" << value->camera << ",\"mode\":" << value->mode
                    << ",\"eligible_lists\":" << value->eligible_lists
                    << ",\"positive_inputs_at_setup\":" << value->positive_inputs << '}';
            };
            out << ",\"prefilter\":{\"heuristic_only\":true,\"skip_limit\":" << kInstanceCaptureMaximumPrefilterSkips
                << ",\"skipped_candidates\":" << prefilter_.skipped()
                << ",\"skip_reason\":\"potential_input_count_below_14_at_setup\""
                << ",\"unknown_capture_fallbacks\":" << prefilter_.unknown();
            if (prefilter_.last_skipped()) { out << ",\"last_skipped_identity\":"; identity(prefilter_.last_skipped()); }
            out << ",\"capture_candidates\":[";
            for (std::size_t index = 0; index < prefilter_.candidates().size(); ++index) {
                if (index) out << ',';
                const auto selected = index < selection_.attempts().size() && selection_.attempts()[index].selected;
                out << "{\"attempt\":" << index + 1 << ",\"selected\":" << (selected ? "true" : "false")
                    << ",\"setup_identity\":"; identity(prefilter_.candidates()[index]); out << '}';
            }
            out << "]}";
        }
        out << "}\n";
        out.close(); if (!out) throw std::runtime_error("cannot write instance capture status");
    }
    std::filesystem::path directory_;
    InstanceBatchCaptureTracker tracker_;
    instance_capture_detail::CaptureSelection selection_;
    instance_capture_detail::SetupPrefilter prefilter_;
    InstanceBatchCaptureCase value_;
    std::vector<std::byte> previous_ram_;
    std::size_t delta_bytes_{};
    bool claimed_{}, saved_{}, rejected_{}, prefilter_enabled_{};
};
} // namespace takedown::runtime::slus_21050
