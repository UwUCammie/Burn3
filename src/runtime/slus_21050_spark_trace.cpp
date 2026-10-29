#include "takedown/runtime/slus_21050_spark_trace.hpp"

#include <array>
#include <cstring>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace takedown::runtime {
namespace {

struct ByteSlice {
    std::uint32_t address{};
    std::size_t requested{};
    bool valid{};
    std::vector<std::uint8_t> bytes;
};

struct SparkSample {
    std::uint64_t sequence{};
    std::uint64_t correlation{};
    Slus21050SparkTraceContext context;
    std::uint32_t pc_before{};
    std::uint32_t next_pc_before{};
    bool delay_slot_before{};
    std::uint32_t instruction_word{};
    std::uint32_t expected_word{};
    std::string label;
    std::uint32_t step_status{};
    std::uint32_t pc_after{};
    std::uint32_t next_pc_after{};
    bool complete{};
    std::array<guest::Gpr128, 7> argument_registers{};
    std::uint32_t f12_bits{};
    std::uint32_t emitter_root{};
    std::uint32_t internal_emitter{};
    std::uint32_t preset{};
    std::int32_t preset_index{-1};
    std::uint32_t variant{};
    std::uint32_t lane{};
    std::uint32_t event_object{};
    std::uint32_t endpoint_a{};
    std::uint32_t endpoint_b{};
    std::uint32_t particle_group{};
    std::uint32_t static_packet_a{};
    std::uint32_t static_packet_b{};
    std::uint32_t packet_start{};
    std::uint32_t packet_end{};
    std::uint64_t packet_size{};
    std::uint32_t builder_allocation{};
    std::uint64_t builder_allocation_size{};
    bool packet_cursor_valid{};
    bool packet_truncated{};
    bool builder_allocation_valid{};
    bool builder_allocation_truncated{};
    ByteSlice event_wrapper;
    ByteSlice event_state;
    ByteSlice endpoint_a_state;
    ByteSlice endpoint_b_state;
    ByteSlice emitter_state;
    ByteSlice internal_emitter_state;
    ByteSlice preset_state;
    ByteSlice particle_group_state;
    ByteSlice static_packet_a_window;
    ByteSlice static_packet_b_window;
    ByteSlice builder_packed_colors_before;
    ByteSlice builder_packed_colors_after;
    ByteSlice builder_allocation_bytes;
    ByteSlice render_packet;
};

struct SparkXgkickSample {
    std::uint64_t sequence{};
    std::uint64_t builder_correlation{};
    std::uint64_t render_correlation{};
    std::uint64_t vblank{};
    std::uint32_t preset{};
    std::int32_t preset_index{-1};
    std::uint32_t variant{};
    guest::Vu1XgkickObservation observation{};
    std::vector<guest::GsQuadword> quadwords;
    std::uint64_t observed_quadwords{};
    bool builder_source_match{};
    bool builder_tag_match{};
    bool render_source_match{};
    bool render_tag_match{};
    bool render_root_fallback{};
    bool complete{};
    bool truncated{};
};

[[nodiscard]] std::optional<std::uint32_t> read_word(const guest::EeMemory& memory,
                                                     const std::optional<guest::GuestAddress> address) {
    if (!address.has_value() || !memory.contains(*address, 4U)) {
        return std::nullopt;
    }
    return memory.read_u32_le(*address);
}

[[nodiscard]] ByteSlice capture_bytes(const guest::EeMemory& memory,
                                      const std::optional<guest::GuestAddress> address,
                                      const std::size_t count) {
    ByteSlice result;
    result.requested = count;
    if (!address.has_value()) {
        return result;
    }
    result.address = *address;
    if (!memory.contains(*address, count)) {
        return result;
    }
    result.valid = true;
    const auto source = memory.view(*address, count);
    result.bytes.reserve(source.size());
    for (const auto value : source) {
        result.bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

void write_hex32(std::ostream& output, const std::uint32_t value) {
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << value << std::dec;
}

void write_slice(std::ostream& output, const ByteSlice& slice) {
    output << (slice.valid ? 1 : 0) << ':';
    write_hex32(output, slice.address);
    output << ':' << slice.requested << ':' << slice.bytes.size() << ':';
    output << std::hex << std::setfill('0');
    for (const auto value : slice.bytes) {
        output << std::setw(2) << static_cast<unsigned>(value);
    }
    output << std::dec;
}


void write_words(std::ostream& output, const guest::Gpr128& value) {
    const auto& words = value.words;
    for (std::size_t index = 0U; index < words.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, words[index]);
    }
}

void write_argument_registers(std::ostream& output,
                              const std::array<guest::Gpr128, 7>& registers) {
    for (std::size_t index = 0U; index < registers.size(); ++index) {
        if (index != 0U) {
            output << '|';
        }
        write_words(output, registers[index]);
    }
}

void write_sample(std::ostream& output, const SparkSample& sample) {
    output << "sample\t" << sample.sequence << '\t' << sample.correlation << '\t'
           << sample.context.committed_steps << '\t' << sample.context.ee_cycles << '\t'
           << sample.context.vblank_starts << '\t' << sample.context.thread_id << '\t';
    write_hex32(output, sample.pc_before);
    output << '\t';
    write_hex32(output, sample.next_pc_before);
    output << '\t' << (sample.delay_slot_before ? 1 : 0) << '\t';
    write_hex32(output, sample.instruction_word);
    output << '\t';
    write_hex32(output, sample.expected_word);
    output << '\t' << (sample.instruction_word == sample.expected_word ? 1 : 0) << '\t'
           << sample.label << '\t' << sample.step_status << '\t';
    write_hex32(output, sample.pc_after);
    output << '\t';
    write_hex32(output, sample.next_pc_after);
    output << '\t' << (sample.complete ? 1 : 0) << '\t';
    write_argument_registers(output, sample.argument_registers);
    output << '\t';
    write_hex32(output, sample.f12_bits);
    output << '\t';
    write_hex32(output, sample.emitter_root);
    output << '\t';
    write_hex32(output, sample.internal_emitter);
    output << '\t';
    write_hex32(output, sample.preset);
    output << '\t' << sample.preset_index << '\t' << sample.variant << '\t' << sample.lane << '\t';
    write_hex32(output, sample.event_object);
    output << '\t';
    write_hex32(output, sample.endpoint_a);
    output << '\t';
    write_hex32(output, sample.endpoint_b);
    output << '\t';
    write_hex32(output, sample.particle_group);
    output << '\t';
    write_hex32(output, sample.static_packet_a);
    output << '\t';
    write_hex32(output, sample.static_packet_b);
    output << '\t';
    write_hex32(output, sample.packet_start);
    output << '\t';
    write_hex32(output, sample.packet_end);
    output << '\t' << sample.packet_size << '\t';
    write_hex32(output, sample.builder_allocation);
    output << '\t' << sample.builder_allocation_size << '\t' << (sample.packet_cursor_valid ? 1 : 0) << '\t'
           << (sample.packet_truncated ? 1 : 0) << '\t' << (sample.builder_allocation_valid ? 1 : 0) << '\t'
           << (sample.builder_allocation_truncated ? 1 : 0) << '\t';
    write_slice(output, sample.event_wrapper);
    output << '\t';
    write_slice(output, sample.event_state);
    output << '\t';
    write_slice(output, sample.endpoint_a_state);
    output << '\t';
    write_slice(output, sample.endpoint_b_state);
    output << '\t';
    write_slice(output, sample.emitter_state);
    output << '\t';
    write_slice(output, sample.internal_emitter_state);
    output << '\t';
    write_slice(output, sample.preset_state);
    output << '\t';
    write_slice(output, sample.particle_group_state);
    output << '\t';
    write_slice(output, sample.static_packet_a_window);
    output << '\t';
    write_slice(output, sample.static_packet_b_window);
    output << '\t';
    write_slice(output, sample.builder_packed_colors_before);
    output << '\t';
    write_slice(output, sample.builder_packed_colors_after);
    output << '\t';
    write_slice(output, sample.builder_allocation_bytes);
    output << '\t';
    write_slice(output, sample.render_packet);
    output << '\n';
}

void write_xgkick_sample(std::ostream& output, const SparkXgkickSample& sample) {
    output << "xgkick\t" << sample.sequence << '\t' << sample.builder_correlation << '\t'
           << sample.render_correlation << '\t' << sample.vblank << '\t';
    write_hex32(output, sample.preset);
    output << '\t' << sample.preset_index << '\t' << sample.variant << '\t'
           << (sample.builder_source_match ? 1 : 0) << '\t' << (sample.builder_tag_match ? 1 : 0)
           << '\t' << (sample.render_source_match ? 1 : 0) << '\t'
           << (sample.render_tag_match ? 1 : 0) << '\t' << (sample.render_root_fallback ? 1 : 0)
           << '\t' << static_cast<unsigned long>(sample.observation.vector_address) << '\t'
           << static_cast<unsigned long>(sample.observation.micro_entry) << '\t'
           << static_cast<unsigned long>(sample.observation.micro_resolved_entry) << '\t'
           << sample.observation.micro_run_sequence << '\t'
           << (sample.observation.vif_command_source_known ? 1 : 0) << '\t';
    write_hex32(output, sample.observation.vif_command_source);
    output << '\t';
    output << (sample.observation.vif_chain_root_known ? 1 : 0) << '\t';
    write_hex32(output, sample.observation.vif_chain_root);
    output << '\t' << (sample.observation.vif_tag_location_known ? 1 : 0) << '\t';
    write_hex32(output, sample.observation.vif_tag_location);
    output << '\t' << std::hex << std::setfill('0');
    for (const auto word : sample.observation.initial_tag) {
        output << std::setw(8) << word;
    }
    output << std::dec << '\t' << (sample.complete ? 1 : 0) << '\t' << (sample.truncated ? 1 : 0)
           << '\t' << sample.observed_quadwords << '\t' << sample.quadwords.size() << '\t';
    output << std::hex << std::setfill('0');
    for (const auto& quadword : sample.quadwords) {
        for (const auto word : quadword) {
            output << std::setw(8) << word;
        }
    }
    output << std::dec << '\n';
}

enum class TargetKind : std::uint8_t {
    BurstEntry,
    BurstEmitCall,
    AppendEntry,
    AppendReturn,
    RenderEntry,
    RenderReturn,
    SelectorEntry,
    SelectorReturn,
    BuilderEntry,
    BuilderPackedColors,
    BuilderAllocation,
    BuilderReturn,
};

struct Target {
    guest::GuestAddress pc{};
    std::uint32_t word{};
    const char* label{};
    TargetKind kind{};
};

constexpr std::array<Target, 12> kTargets{{
    {0x257860U, 0x27BDFD70U, "spark_burst_entry", TargetKind::BurstEntry},
    {0x257C24U, 0x0C098FA0U, "spark_burst_emit_call", TargetKind::BurstEmitCall},
    {0x263E80U, 0x27BDFFA0U, "spark_append_entry", TargetKind::AppendEntry},
    {0x263F68U, 0x03E00008U, "spark_append_return", TargetKind::AppendReturn},
    {0x263420U, 0x27BDFFD0U, "spark_render_entry", TargetKind::RenderEntry},
    {0x2636A8U, 0x03E00008U, "spark_render_return", TargetKind::RenderReturn},
    {0x264040U, 0x27BDFFE0U, "spark_selector_entry", TargetKind::SelectorEntry},
    {0x2640D0U, 0x03E00008U, "spark_selector_return", TargetKind::SelectorReturn},
    {0x264170U, 0x27BDFF20U, "spark_packet_builder_entry", TargetKind::BuilderEntry},
    {0x2644E0U, 0xFFA200D8U, "spark_packet_builder_packed_colors", TargetKind::BuilderPackedColors},
    {0x2645F8U, 0xAE220048U, "spark_packet_builder_allocation", TargetKind::BuilderAllocation},
    {0x264900U, 0x03E00008U, "spark_packet_builder_return", TargetKind::BuilderReturn},
}};

[[nodiscard]] const Target* find_target(const guest::GuestAddress pc) noexcept {
    switch (pc) {
    case 0x257860U:
        return &kTargets[0];
    case 0x257C24U:
        return &kTargets[1];
    case 0x263E80U:
        return &kTargets[2];
    case 0x263F68U:
        return &kTargets[3];
    case 0x263420U:
        return &kTargets[4];
    case 0x2636A8U:
        return &kTargets[5];
    case 0x264040U:
        return &kTargets[6];
    case 0x2640D0U:
        return &kTargets[7];
    case 0x264170U:
        return &kTargets[8];
    case 0x2644E0U:
        return &kTargets[9];
    case 0x2645F8U:
        return &kTargets[10];
    case 0x264900U:
        return &kTargets[11];
    default:
        return nullptr;
    }
}

// Per guest thread state carried from a producer entry to its return.
struct ActiveCall {
    std::uint64_t correlation{};
    guest::GuestAddress root{};
    guest::GuestAddress internal{};
    guest::GuestAddress preset{};
    guest::GuestAddress endpoint_a{};
    guest::GuestAddress endpoint_b{};
    guest::GuestAddress particle_group{};
    guest::GuestAddress packet_start{};
    std::uint32_t variant{};
    std::uint32_t lane{};
    guest::GuestAddress builder_allocation{};
    std::uint32_t builder_allocation_size{};
    bool packet_start_valid{};
    bool builder_allocation_valid{};
};

struct StaticPacketRange {
    guest::GuestAddress begin{};
    std::uint64_t size{};
    std::uint64_t builder_correlation{};
    std::uint32_t preset{};
    std::int32_t preset_index{};
    std::uint32_t variant{};
};

struct RenderPacketRange {
    guest::GuestAddress begin{};
    guest::GuestAddress end{};
    std::uint64_t render_correlation{};
    std::uint64_t vblank{};
    std::uint32_t preset{};
    std::int32_t preset_index{};
    std::uint32_t variant{};
};

} // namespace

struct Slus21050SparkTrace::Impl {
    Impl(std::filesystem::path path, const Slus21050SparkTraceConfig& value)
        : output_path(std::move(path)),
          partial_path(output_path.string() + ".partial"),
          config(value) {
        if (output_path.empty()) {
            throw std::invalid_argument("spark trace output path is empty");
        }
        if (config.first_capacity == 0U && config.tail_capacity == 0U) {
            throw std::invalid_argument("spark trace capacity must not be zero");
        }
        if (config.maximum_packet_bytes == 0U || config.static_packet_window_bytes == 0U ||
            config.maximum_builder_allocation_bytes == 0U || config.maximum_xgkick_quadwords == 0U) {
            throw std::invalid_argument("spark trace packet limits must not be zero");
        }
        if (config.first_xgkick_capacity == 0U && config.tail_xgkick_capacity == 0U) {
            throw std::invalid_argument("spark trace XGKICK capacity must not be zero");
        }
        std::error_code error;
        if (std::filesystem::exists(output_path, error) || error) {
            throw std::invalid_argument("refusing to overwrite spark trace: " + output_path.string());
        }
        error.clear();
        if (std::filesystem::exists(partial_path, error) || error) {
            throw std::invalid_argument("refusing to overwrite partial spark trace: " +
                                        partial_path.string());
        }
        const auto parent = output_path.parent_path();
        if (!parent.empty() && (!std::filesystem::exists(parent, error) || error ||
                                !std::filesystem::is_directory(parent, error) || error)) {
            throw std::invalid_argument("spark trace parent directory is unavailable: " +
                                        parent.string());
        }
        first.reserve(config.first_capacity);
        tail.resize(config.tail_capacity);
        first_xgkicks.resize(config.first_xgkick_capacity);
        tail_xgkicks.resize(config.tail_xgkick_capacity);
        for (auto& sample : first_xgkicks) {
            sample.quadwords.reserve(config.maximum_xgkick_quadwords);
        }
        for (auto& sample : tail_xgkicks) {
            sample.quadwords.reserve(config.maximum_xgkick_quadwords);
        }
    }

    std::filesystem::path output_path;
    std::filesystem::path partial_path;
    Slus21050SparkTraceConfig config;
    std::vector<SparkSample> first;
    std::vector<SparkSample> tail;
    std::size_t tail_count{};
    std::size_t tail_next{};
    std::uint64_t total{};
    std::uint64_t filtered{};
    std::uint64_t revision_mismatches{};
    std::uint64_t render_packets{};
    std::uint64_t render_packet_bytes{};
    std::uint64_t truncated_render_packets{};
    std::uint64_t builder_allocations{};
    std::uint64_t builder_allocation_bytes{};
    std::uint64_t truncated_builder_allocations{};
    std::uint64_t total_xgkicks{};
    std::uint64_t xgkick_quadwords{};
    std::uint64_t truncated_xgkicks{};
    std::uint64_t incomplete_xgkicks{};
    std::uint64_t next_correlation{1U};
    std::unordered_map<int, ActiveCall> active_append;
    std::unordered_map<int, ActiveCall> active_render;
    std::unordered_map<int, ActiveCall> active_selector;
    std::unordered_map<int, ActiveCall> active_builder;
    std::deque<StaticPacketRange> static_packet_ranges;
    std::deque<RenderPacketRange> render_packet_ranges;
    std::vector<SparkXgkickSample> first_xgkicks;
    std::size_t first_xgkick_count{};
    std::vector<SparkXgkickSample> tail_xgkicks;
    std::size_t tail_xgkick_count{};
    std::size_t tail_xgkick_next{};
    SparkXgkickSample* active_xgkick{};
    bool active_xgkick_is_tail{};
    bool finalized{};
};

Slus21050SparkTrace::Slus21050SparkTrace(std::filesystem::path output_path,
                                         const Slus21050SparkTraceConfig config)
    : impl_(std::make_unique<Impl>(std::move(output_path), config)) {}

Slus21050SparkTrace::~Slus21050SparkTrace() {
    if (impl_ == nullptr || impl_->finalized) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(impl_->partial_path, error);
}

Slus21050SparkTrace::Slus21050SparkTrace(Slus21050SparkTrace&&) noexcept = default;
Slus21050SparkTrace& Slus21050SparkTrace::operator=(Slus21050SparkTrace&&) noexcept = default;

std::optional<std::uint32_t>
Slus21050SparkTrace::expected_instruction(const guest::GuestAddress pc) noexcept {
    const Target* target = find_target(pc);
    return target == nullptr ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{target->word};
}

bool Slus21050SparkTrace::is_target(const guest::GuestAddress pc) noexcept {
    return find_target(pc) != nullptr;
}

namespace {

[[nodiscard]] std::optional<guest::GuestAddress> add_offset(const guest::GuestAddress address,
                                                            const std::uint32_t delta) noexcept {
    if (address > 0xFFFFFFFFU - delta) {
        return std::nullopt;
    }
    return address + delta;
}

[[nodiscard]] std::optional<guest::GuestAddress>
subtract_offset(const guest::GuestAddress address, const std::uint32_t delta) noexcept {
    if (address < delta) {
        return std::nullopt;
    }
    return address - delta;
}

[[nodiscard]] std::optional<std::uint32_t> preset_index_for(const std::uint32_t preset) noexcept {
    if (preset < 5011216U) {
        return std::nullopt;
    }
    const auto delta = preset - 5011216U;
    if (delta % 124U != 0U) {
        return std::nullopt;
    }
    const auto group = delta / 124U;
    return group < 6U ? std::optional<std::uint32_t>{group}
                      : std::optional<std::uint32_t>{};
}

} // namespace

std::optional<Slus21050SparkTrace::SampleToken>
Slus21050SparkTrace::begin_sample(const guest::R5900State& state, const guest::EeMemory& memory,
                                  const Slus21050SparkTraceContext& context) {
    if (!impl_ || impl_->finalized) {
        throw std::logic_error("spark trace is not writable");
    }
    const Target* target = find_target(state.pc());
    if (target == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t a0 = state.read_gpr_u32(4U);
    const std::uint32_t a1 = state.read_gpr_u32(5U);
    const std::uint32_t a2 = state.read_gpr_u32(6U);
    const std::uint32_t a3 = state.read_gpr_u32(7U);

    ActiveCall active{};
    bool has_active = false;
    const auto take_active = [&](std::unordered_map<int, ActiveCall>& calls) {
        const auto found = calls.find(context.thread_id);
        if (found == calls.end()) {
            return false;
        }
        active = found->second;
        calls.erase(found);
        return true;
    };
    switch (target->kind) {
    case TargetKind::AppendReturn:
        has_active = take_active(impl_->active_append);
        break;
    case TargetKind::RenderReturn:
        has_active = take_active(impl_->active_render);
        break;
    case TargetKind::SelectorReturn:
        has_active = take_active(impl_->active_selector);
        break;
    case TargetKind::BuilderReturn:
        has_active = take_active(impl_->active_builder);
        break;
    case TargetKind::BuilderPackedColors:
    case TargetKind::BuilderAllocation: {
        const auto found = impl_->active_builder.find(context.thread_id);
        if (found != impl_->active_builder.end()) {
            if (target->kind == TargetKind::BuilderAllocation) {
                found->second.builder_allocation = state.read_gpr_u32(2U);
                found->second.builder_allocation_size = state.read_gpr_u32(16U);
                found->second.builder_allocation_valid =
                    found->second.builder_allocation != 0U &&
                    found->second.builder_allocation_size != 0U;
            }
            active = found->second;
            has_active = true;
        }
        break;
    }
    default:
        break;
    }
    if ((target->kind == TargetKind::AppendReturn || target->kind == TargetKind::RenderReturn ||
         target->kind == TargetKind::SelectorReturn ||
         target->kind == TargetKind::BuilderPackedColors ||
         target->kind == TargetKind::BuilderAllocation ||
         target->kind == TargetKind::BuilderReturn) &&
        !has_active) {
        ++impl_->filtered;
        return std::nullopt;
    }

    SparkSample sample;
    sample.context = context;
    sample.pc_before = state.pc();
    sample.next_pc_before = state.next_pc();
    sample.delay_slot_before = state.is_executing_delay_slot();
    sample.expected_word = target->word;
    sample.label = target->label;
    sample.instruction_word =
        memory.contains(state.pc(), 4U) ? memory.read_u32_le(state.pc()) : 0U;
    if (sample.instruction_word != sample.expected_word) {
        ++impl_->revision_mismatches;
    }
    constexpr std::array<std::size_t, 7> kRegisters{4U, 5U, 6U, 7U, 28U, 29U, 31U};
    for (std::size_t index = 0U; index < kRegisters.size(); ++index) {
        sample.argument_registers[index] = state.read_gpr(kRegisters[index]);
    }
    sample.f12_bits = state.read_fpr_bits(12U);
    if (has_active) {
        sample.correlation = active.correlation;
        sample.emitter_root = active.root;
        sample.internal_emitter = active.internal;
        sample.preset = active.preset;
        sample.variant = active.variant;
        sample.lane = active.lane;
        sample.endpoint_a = active.endpoint_a;
        sample.endpoint_b = active.endpoint_b;
        sample.particle_group = active.particle_group;
        sample.packet_start = active.packet_start;
        sample.packet_cursor_valid = active.packet_start_valid;
        sample.builder_allocation = active.builder_allocation;
        sample.builder_allocation_size = active.builder_allocation_size;
        sample.builder_allocation_valid = active.builder_allocation_valid;
    }

    switch (target->kind) {
    case TargetKind::BurstEntry:
        sample.event_wrapper = capture_bytes(memory, a1, 64U);
        sample.event_object = read_word(memory, a1).value_or(0U);
        sample.event_state = capture_bytes(
            memory,
            sample.event_object == 0U ? std::optional<guest::GuestAddress>{}
                                      : std::optional<guest::GuestAddress>{sample.event_object},
            128U);
        break;
    case TargetKind::BurstEmitCall:
        sample.emitter_root = a0;
        sample.internal_emitter = a0;
        sample.endpoint_a = a1;
        sample.endpoint_b = a2;
        sample.variant = 0U;
        sample.preset = read_word(memory, add_offset(a0, 152U)).value_or(0U);
        break;
    case TargetKind::AppendEntry: {
        sample.correlation = impl_->next_correlation++;
        sample.emitter_root = a0;
        sample.internal_emitter = a3 != 0U ? add_offset(a0, 76U).value_or(0U) : a0;
        sample.endpoint_a = a1;
        sample.endpoint_b = a2;
        sample.variant = a3;
        sample.preset = read_word(memory, add_offset(a0, 152U)).value_or(0U);
        sample.lane = read_word(memory, add_offset(sample.internal_emitter, 64U)).value_or(0U);
        if (sample.lane <= 3U) {
            const auto descriptor = add_offset(sample.internal_emitter, sample.lane * 16U);
            const auto base = read_word(memory, descriptor);
            const auto old_count = read_word(
                memory, descriptor.has_value() ? add_offset(*descriptor, 8U) : std::nullopt);
            if (base.has_value() && old_count.has_value()) {
                const long long quotient = static_cast<std::int32_t>(*old_count) / 4;
                const long long byte_offset = quotient * 144;
                const auto group = *base + static_cast<std::uint32_t>(byte_offset);
                sample.particle_group = group | 0x20000000U;
            }
        }
        impl_->active_append[context.thread_id] = ActiveCall{
            .correlation = sample.correlation,
            .root = sample.emitter_root,
            .internal = sample.internal_emitter,
            .preset = sample.preset,
            .endpoint_a = sample.endpoint_a,
            .endpoint_b = sample.endpoint_b,
            .particle_group = sample.particle_group,
            .variant = sample.variant,
            .lane = sample.lane,
        };
        break;
    }
    case TargetKind::RenderEntry: {
        sample.emitter_root = a0;
        sample.internal_emitter = a0;
        sample.preset = read_word(memory, add_offset(a0, 152U)).value_or(0U);
        const auto preset_index = preset_index_for(sample.preset);
        if (!preset_index.has_value()) {
            impl_->active_render.erase(context.thread_id);
            ++impl_->filtered;
            return std::nullopt;
        }
        sample.preset_index = static_cast<std::int32_t>(*preset_index);
        sample.variant = a1;
        sample.correlation = impl_->next_correlation++;
        const auto cursor_address = subtract_offset(state.read_gpr_u32(28U), 24240U);
        const auto packet_start = read_word(memory, cursor_address);
        sample.packet_cursor_valid = packet_start.has_value();
        sample.packet_start = packet_start.value_or(0U);
        impl_->active_render[context.thread_id] = ActiveCall{
            .correlation = sample.correlation,
            .root = sample.emitter_root,
            .internal = sample.internal_emitter,
            .preset = sample.preset,
            .packet_start = sample.packet_start,
            .variant = sample.variant,
            .packet_start_valid = sample.packet_cursor_valid,
        };
        break;
    }
    case TargetKind::RenderReturn: {
        const auto cursor_address = subtract_offset(state.read_gpr_u32(28U), 24240U);
        const auto packet_end = read_word(memory, cursor_address);
        sample.packet_cursor_valid = sample.packet_cursor_valid && packet_end.has_value();
        sample.packet_end = packet_end.value_or(0U);
        if (sample.packet_cursor_valid && sample.packet_start <= sample.packet_end) {
            sample.packet_size = sample.packet_end - sample.packet_start;
            const auto captured =
                std::min<std::uint64_t>(sample.packet_size, impl_->config.maximum_packet_bytes);
            sample.packet_truncated = sample.packet_size > captured;
            sample.render_packet = capture_bytes(memory, sample.packet_start, captured);
            if (sample.render_packet.valid) {
                ++impl_->render_packets;
                impl_->render_packet_bytes += sample.packet_size;
                if (sample.packet_truncated) {
                    ++impl_->truncated_render_packets;
                }
            }
        } else {
            sample.packet_cursor_valid = false;
        }
        break;
    }
    case TargetKind::SelectorEntry: {
        sample.correlation = impl_->next_correlation++;
        sample.emitter_root = a0;
        sample.internal_emitter = a0;
        sample.variant = a1;
        const std::uint64_t preset = 5011216U + static_cast<std::uint64_t>(a1) * 124U;
        if (preset <= 0xFFFFFFFFU) {
            sample.preset = static_cast<std::uint32_t>(preset);
        }
        impl_->active_selector[context.thread_id] = ActiveCall{
            .correlation = sample.correlation,
            .root = sample.emitter_root,
            .internal = sample.internal_emitter,
            .preset = sample.preset,
            .variant = sample.variant,
        };
        break;
    }
    case TargetKind::BuilderEntry:
        sample.correlation = impl_->next_correlation++;
        sample.internal_emitter = a0;
        sample.preset = a1;
        sample.variant = a2;
        impl_->active_builder[context.thread_id] = ActiveCall{
            .correlation = sample.correlation,
            .internal = sample.internal_emitter,
            .preset = sample.preset,
            .variant = sample.variant,
        };
        break;
    case TargetKind::BuilderPackedColors:
        sample.builder_packed_colors_before =
            capture_bytes(memory, add_offset(state.read_gpr_u32(29U), 192U), 32U);
        sample.builder_packed_colors_after = sample.builder_packed_colors_before;
        break;
    case TargetKind::BuilderReturn:
        if (sample.builder_allocation_valid) {
            const auto captured = std::min<std::uint64_t>(
                sample.builder_allocation_size, impl_->config.maximum_builder_allocation_bytes);
            sample.builder_allocation_truncated = sample.builder_allocation_size > captured;
            sample.builder_allocation_bytes =
                capture_bytes(memory, sample.builder_allocation, captured);
            if (sample.builder_allocation_bytes.valid) {
                ++impl_->builder_allocations;
                impl_->builder_allocation_bytes += sample.builder_allocation_size;
                if (sample.builder_allocation_truncated) {
                    ++impl_->truncated_builder_allocations;
                }
            }
        }
        break;
    default:
        break;
    }

    if (sample.preset_index < 0) {
        const auto index = preset_index_for(sample.preset);
        sample.preset_index =
            static_cast<std::int32_t>(index.has_value() ? *index : 0xFFFFFFFFU);
    }
    if (sample.event_wrapper.bytes.empty() && target->kind != TargetKind::BurstEntry) {
        sample.event_wrapper = ByteSlice{};
    }
    sample.endpoint_a_state = capture_bytes(
        memory,
        sample.endpoint_a == 0U ? std::optional<guest::GuestAddress>{}
                                : std::optional<guest::GuestAddress>{sample.endpoint_a},
        16U);
    sample.endpoint_b_state = capture_bytes(
        memory,
        sample.endpoint_b == 0U ? std::optional<guest::GuestAddress>{}
                                : std::optional<guest::GuestAddress>{sample.endpoint_b},
        16U);
    sample.emitter_state = capture_bytes(
        memory,
        sample.emitter_root == 0U ? std::optional<guest::GuestAddress>{}
                                  : std::optional<guest::GuestAddress>{sample.emitter_root},
        168U);
    sample.internal_emitter_state = capture_bytes(
        memory,
        sample.internal_emitter == 0U ? std::optional<guest::GuestAddress>{}
                                      : std::optional<guest::GuestAddress>{sample.internal_emitter},
        76U);
    sample.preset_state = capture_bytes(
        memory,
        sample.preset == 0U ? std::optional<guest::GuestAddress>{}
                            : std::optional<guest::GuestAddress>{sample.preset},
        124U);
    sample.particle_group_state = capture_bytes(
        memory,
        sample.particle_group == 0U ? std::optional<guest::GuestAddress>{}
                                    : std::optional<guest::GuestAddress>{sample.particle_group},
        144U);
    if (sample.emitter_root != 0U) {
        sample.static_packet_a =
            read_word(memory, add_offset(sample.emitter_root, 72U)).value_or(0U);
        sample.static_packet_b =
            read_word(memory, add_offset(sample.emitter_root, 148U)).value_or(0U);
    } else if (sample.internal_emitter != 0U) {
        sample.static_packet_a =
            read_word(memory, add_offset(sample.internal_emitter, 72U)).value_or(0U);
    }
    sample.static_packet_a_window = capture_bytes(
        memory,
        sample.static_packet_a == 0U ? std::optional<guest::GuestAddress>{}
                                     : std::optional<guest::GuestAddress>{sample.static_packet_a},
        impl_->config.static_packet_window_bytes);
    sample.static_packet_b_window = capture_bytes(
        memory,
        sample.static_packet_b == 0U ? std::optional<guest::GuestAddress>{}
                                     : std::optional<guest::GuestAddress>{sample.static_packet_b},
        impl_->config.static_packet_window_bytes);

    if (target->kind == TargetKind::BuilderReturn && sample.builder_allocation_bytes.valid &&
        !sample.builder_allocation_bytes.bytes.empty()) {
        if (impl_->static_packet_ranges.size() == 1'024U) {
            impl_->static_packet_ranges.pop_front();
        }
        impl_->static_packet_ranges.push_back(StaticPacketRange{
            .begin = sample.builder_allocation,
            .size = sample.builder_allocation_bytes.bytes.size(),
            .builder_correlation = sample.correlation,
            .preset = sample.preset,
            .preset_index = sample.preset_index,
            .variant = sample.variant,
        });
    }
    if (target->kind == TargetKind::RenderReturn && sample.render_packet.valid &&
        !sample.render_packet.bytes.empty()) {
        if (impl_->render_packet_ranges.size() == 1'024U) {
            impl_->render_packet_ranges.pop_front();
        }
        impl_->render_packet_ranges.push_back(RenderPacketRange{
            .begin = sample.packet_start,
            .end = sample.packet_start +
                   static_cast<std::uint32_t>(sample.render_packet.bytes.size()),
            .render_correlation = sample.correlation,
            .vblank = sample.context.vblank_starts,
            .preset = sample.preset,
            .preset_index = sample.preset_index,
            .variant = sample.variant,
        });
    }

    sample.sequence = impl_->total++;
    if (impl_->first.size() < impl_->config.first_capacity) {
        impl_->first.push_back(std::move(sample));
        return SampleToken{
            .sequence = impl_->first.back().sequence,
            .index = impl_->first.size() - 1U,
            .tail = false,
        };
    }
    if (impl_->tail.empty()) {
        return std::nullopt;
    }
    const auto index = impl_->tail_next;
    impl_->tail[index] = std::move(sample);
    impl_->tail_next = (impl_->tail_next + 1U) % impl_->tail.size();
    impl_->tail_count = std::min(impl_->tail_count + 1U, impl_->tail.size());
    return SampleToken{
        .sequence = impl_->tail[index].sequence,
        .index = index,
        .tail = true,
    };
}

namespace {

void refresh_slice(ByteSlice& slice, const guest::EeMemory& memory) {
    if (!slice.valid || slice.bytes.size() != slice.requested ||
        !memory.contains(slice.address, slice.requested)) {
        slice.valid = false;
        slice.bytes.clear();
        return;
    }
    try {
        const auto source = memory.view(slice.address, slice.requested);
        for (std::size_t index = 0U; index < source.size(); ++index) {
            slice.bytes[index] = static_cast<std::uint8_t>(source[index]);
        }
    } catch (...) {
        slice.valid = false;
        slice.bytes.clear();
    }
}

} // namespace

void Slus21050SparkTrace::finish_sample(const SampleToken token, const guest::R5900State& state,
                                        const guest::EeMemory& memory,
                                        const std::uint32_t step_status) noexcept {
    if (!impl_ || impl_->finalized) {
        return;
    }
    SparkSample* sample;
    if (token.tail) {
        if (token.index < impl_->tail.size()) {
            sample = &impl_->tail[token.index];
        } else {
            return;
        }
    } else {
        if (token.index < impl_->first.size()) {
            sample = &impl_->first[token.index];
        } else {
            return;
        }
    }
    if (sample == nullptr) {
        return;
    }
    if (sample->sequence != token.sequence) {
        return;
    }
    sample->step_status = step_status;
    sample->pc_after = state.pc();
    sample->next_pc_after = state.next_pc();
    if (sample->pc_before == 2508000U) {
        refresh_slice(sample->builder_packed_colors_after, memory);
    }
    sample->complete = true;
}

void Slus21050SparkTrace::on_vu1_xgkick_quadword(const guest::GsQuadword& quadword) noexcept {
    if (!impl_ || impl_->active_xgkick == nullptr) {
        return;
    }
    auto& sample = *impl_->active_xgkick;
    ++sample.observed_quadwords;
    ++impl_->xgkick_quadwords;
    if (sample.quadwords.size() < impl_->config.maximum_xgkick_quadwords) {
        sample.quadwords.push_back(quadword);
    } else {
        sample.truncated = true;
    }
}

void Slus21050SparkTrace::on_vu1_xgkick_end(const bool completed) noexcept {
    if (!impl_ || impl_->active_xgkick == nullptr) {
        return;
    }
    auto& sample = *impl_->active_xgkick;
    sample.complete = completed;
    if (sample.truncated) {
        ++impl_->truncated_xgkicks;
    }
    if (!completed) {
        ++impl_->incomplete_xgkicks;
    }
    if (impl_->active_xgkick_is_tail) {
        impl_->tail_xgkick_next = (impl_->tail_xgkick_next + 1U) % impl_->tail_xgkicks.size();
        impl_->tail_xgkick_count =
            std::min(impl_->tail_xgkick_count + 1U, impl_->tail_xgkicks.size());
    } else {
        ++impl_->first_xgkick_count;
    }
    impl_->active_xgkick = nullptr;
    impl_->active_xgkick_is_tail = false;
}

void Slus21050SparkTrace::finalize() {
    if (!impl_) {
        throw std::logic_error("spark trace has no implementation");
    }
    if (impl_->finalized) {
        return;
    }
    if (impl_->active_xgkick != nullptr) {
        on_vu1_xgkick_end(false);
    }
    std::error_code error;
    if (std::filesystem::exists(impl_->output_path, error) || error ||
        std::filesystem::exists(impl_->partial_path, error) || error) {
        throw std::runtime_error("spark trace output appeared while the run was active");
    }
    try {
        std::ofstream output(impl_->partial_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create spark trace partial file: " +
                                     impl_->partial_path.string());
        }
        const auto stats = statistics();
        output << std::string_view{"B3_SPARK_TRACE\t5\n"};
        output << std::string_view{"meta\trevision\tSLUS-21050\n"};
        output << std::string_view{"meta\tpreset_base\t0x004c7710\n"};
        output << std::string_view{"meta\tpreset_stride\t124\n"};
        output << std::string_view{"meta\tpreset_count\t6\n"};
        output << "meta\ttotal\t" << stats.total_samples << '\n'
               << "meta\tretained\t" << stats.retained_samples << '\n'
               << "meta\toverwritten\t" << stats.overwritten_samples << '\n'
               << "meta\truntime_filtered\t" << stats.runtime_filtered_targets << '\n'
               << "meta\trevision_mismatches\t" << stats.revision_mismatches << '\n'
               << "meta\trender_packets\t" << stats.render_packets << '\n'
               << "meta\trender_packet_bytes\t" << stats.render_packet_bytes << '\n'
               << "meta\ttruncated_render_packets\t" << stats.truncated_render_packets << '\n'
               << "meta\tbuilder_allocations\t" << stats.builder_allocations << '\n'
               << "meta\tbuilder_allocation_bytes\t" << stats.builder_allocation_bytes << '\n'
               << "meta\ttruncated_builder_allocations\t" << stats.truncated_builder_allocations
               << '\n'
               << "meta\tmatched_xgkick_packets\t" << stats.matched_xgkick_packets << '\n'
               << "meta\tretained_xgkick_packets\t" << stats.retained_xgkick_packets << '\n'
               << "meta\toverwritten_xgkick_packets\t" << stats.overwritten_xgkick_packets << '\n'
               << "meta\txgkick_quadwords\t" << stats.xgkick_quadwords << '\n'
               << "meta\ttruncated_xgkick_packets\t" << stats.truncated_xgkick_packets << '\n'
               << "meta\tincomplete_xgkick_packets\t" << stats.incomplete_xgkick_packets << '\n'
               << "columns\tsequence\tcorrelation\tcommitted_steps\tee_cycles\tvblank_starts\tthread_id\tpc_before\tnext_pc_before\tdelay_slot\tinstruction\texpected\trevision_match\tlabel\tstep_status\tpc_after\tnext_pc_after\tcomplete\ta0_a1_a2_a3_gp_sp_ra\tf12_bits\temitter_root\tinternal_emitter\tpreset\tpreset_index\tvariant\tlane\tevent_object\tendpoint_a\tendpoint_b\tparticle_group\tstatic_packet_a\tstatic_packet_b\tpacket_start\tpacket_end\tpacket_size\tbuilder_allocation\tbuilder_allocation_size\tpacket_cursor_valid\tpacket_truncated\tbuilder_allocation_valid\tbuilder_allocation_truncated\tevent_wrapper\tevent_state\tendpoint_a_state\tendpoint_b_state\temitter_state\tinternal_emitter_state\tpreset_state\tparticle_group_state\tstatic_packet_a_window\tstatic_packet_b_window\tbuilder_packed_colors_before\tbuilder_packed_colors_after\tbuilder_allocation_bytes\trender_packet\n";
        for (const auto& sample : impl_->first) {
            write_sample(output, sample);
        }
        if (impl_->tail_count != 0U) {
            const auto oldest = impl_->tail_count == impl_->tail.size() ? impl_->tail_next : 0U;
            for (std::size_t offset = 0U; offset < impl_->tail_count; ++offset) {
                write_sample(output, impl_->tail[(oldest + offset) % impl_->tail.size()]);
            }
        }
        output << "xgkick_columns\tsequence\tbuilder_correlation\trender_correlation\tvblank\tpreset\tpreset_index\tvariant\tbuilder_source_match\tbuilder_tag_match\trender_source_match\trender_tag_match\trender_root_fallback\tvector_address\tmicro_entry\tmicro_resolved_entry\tmicro_run_sequence\tvif_command_source_known\tvif_command_source\tvif_chain_root_known\tvif_chain_root\tvif_tag_location_known\tvif_tag_location\tinitial_tag\tcomplete\ttruncated\tobserved_quadwords\tretained_quadwords\tquadwords\n";
        for (std::size_t index = 0U; index < impl_->first_xgkick_count; ++index) {
            write_xgkick_sample(output, impl_->first_xgkicks[index]);
        }
        if (impl_->tail_xgkick_count != 0U) {
            const auto oldest = impl_->tail_xgkick_count == impl_->tail_xgkicks.size()
                                    ? impl_->tail_xgkick_next
                                    : 0U;
            for (std::size_t offset = 0U; offset < impl_->tail_xgkick_count; ++offset) {
                write_xgkick_sample(
                    output, impl_->tail_xgkicks[(oldest + offset) % impl_->tail_xgkicks.size()]);
            }
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("could not finish spark trace partial file: " +
                                     impl_->partial_path.string());
        }
        output.close();
        std::filesystem::rename(impl_->partial_path, impl_->output_path);
        impl_->finalized = true;
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(impl_->partial_path, cleanup_error);
        throw;
    }
}

Slus21050SparkTraceStatistics Slus21050SparkTrace::statistics() const noexcept {
    if (!impl_) {
        return {};
    }
    const auto retained = impl_->first.size() + impl_->tail_count;
    const auto retained_xgkicks = impl_->first_xgkick_count + impl_->tail_xgkick_count;
    return {
        .total_samples = impl_->total,
        .retained_samples = retained,
        .overwritten_samples = impl_->total - retained,
        .runtime_filtered_targets = impl_->filtered,
        .revision_mismatches = impl_->revision_mismatches,
        .render_packets = impl_->render_packets,
        .render_packet_bytes = impl_->render_packet_bytes,
        .truncated_render_packets = impl_->truncated_render_packets,
        .builder_allocations = impl_->builder_allocations,
        .builder_allocation_bytes = impl_->builder_allocation_bytes,
        .truncated_builder_allocations = impl_->truncated_builder_allocations,
        .matched_xgkick_packets = impl_->total_xgkicks,
        .retained_xgkick_packets = retained_xgkicks,
        .overwritten_xgkick_packets = impl_->total_xgkicks - retained_xgkicks,
        .xgkick_quadwords = impl_->xgkick_quadwords,
        .truncated_xgkick_packets = impl_->truncated_xgkicks,
        .incomplete_xgkick_packets = impl_->incomplete_xgkicks,
    };
}

const std::filesystem::path& Slus21050SparkTrace::output_path() const noexcept {
    static const std::filesystem::path kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->output_path;
}

namespace {

[[nodiscard]] bool range_contains(const guest::GuestAddress address,
                                  const guest::GuestAddress begin,
                                  const std::uint64_t size) noexcept {
    if (begin >= 0x70000000U && begin < 0x70004000U) {
        std::uint64_t offset;
        if ((address & 0x80000000U) != 0U) {
            offset = address & 0x3FFFU;
        } else if (address >= 0x70000000U && address < 0x70004000U) {
            offset = address - 0x70000000U;
        } else {
            return false;
        }
        const auto range_offset = static_cast<std::uint64_t>(begin - 0x70000000U);
        return size != 0U && range_offset <= offset && size > offset - range_offset;
    }
    if ((address & 0x80000000U) != 0U) {
        return false;
    }
    const auto canonical_address = static_cast<std::uint64_t>(address & 0x1FFFFFFFU);
    const auto canonical_begin = static_cast<std::uint64_t>(begin & 0x1FFFFFFFU);
    return size != 0U && canonical_address >= canonical_begin &&
           size > canonical_address - canonical_begin;
}

} // namespace

// RECOVERY: GIMPLE not identical to the original for this function. Remaining
// differences are only the block layout/branch polarity of the render/static
// preset selection and the SSA numbering of the reverse deque iterator PHIs.
bool Slus21050SparkTrace::on_vu1_xgkick_begin(
    const guest::Vu1XgkickObservation& observation) noexcept {
    if (!impl_ || impl_->finalized) {
        return false;
    }
    if (impl_->active_xgkick != nullptr) {
        on_vu1_xgkick_end(false);
    }
    const StaticPacketRange* static_range = nullptr;
    bool builder_source_match = false;
    bool builder_tag_match = false;
    if (observation.vif_command_source_known) {
        for (auto it = impl_->static_packet_ranges.rbegin();
             it != impl_->static_packet_ranges.rend(); ++it) {
            if (range_contains(observation.vif_command_source, it->begin, it->size)) {
                static_range = &*it;
                builder_source_match = true;
                break;
            }
        }
    }
    if (static_range == nullptr && observation.vif_tag_location_known) {
        for (auto it = impl_->static_packet_ranges.rbegin();
             it != impl_->static_packet_ranges.rend(); ++it) {
            if (range_contains(observation.vif_tag_location, it->begin, it->size)) {
                static_range = &*it;
                builder_tag_match = true;
                break;
            }
        }
    }
    const RenderPacketRange* render_range = nullptr;
    bool render_source_match = false;
    bool render_tag_match = false;
    bool render_root_fallback = false;
    for (auto it = impl_->render_packet_ranges.rbegin(); it != impl_->render_packet_ranges.rend();
         ++it) {
        const auto size = it->end >= it->begin ? static_cast<std::uint64_t>(it->end - it->begin) : 0U;
        if (observation.vif_command_source_known &&
            range_contains(observation.vif_command_source, it->begin, size)) {
            render_range = &*it;
            render_source_match = true;
            break;
        }
        if (observation.vif_tag_location_known &&
            range_contains(observation.vif_tag_location, it->begin, size)) {
            render_range = &*it;
            render_tag_match = true;
            break;
        }
    }
    if (render_range == nullptr && observation.vif_chain_root_known) {
        for (auto it = impl_->render_packet_ranges.rbegin();
             it != impl_->render_packet_ranges.rend(); ++it) {
            const auto size = it->end >= it->begin ? static_cast<std::uint64_t>(it->end - it->begin) : 0U;
            if (range_contains(observation.vif_chain_root, it->begin, size)) {
                render_range = &*it;
                render_root_fallback = true;
                break;
            }
        }
    }
    if (static_range == nullptr && render_range == nullptr) {
        return false;
    }
    SparkXgkickSample* sample = nullptr;
    bool tail = false;
    if (impl_->first_xgkick_count < impl_->first_xgkicks.size()) {
        sample = &impl_->first_xgkicks[impl_->first_xgkick_count];
    } else if (!impl_->tail_xgkicks.empty()) {
        sample = &impl_->tail_xgkicks[impl_->tail_xgkick_next];
        tail = true;
    }
    const std::uint64_t sequence = impl_->total_xgkicks++;
    if (sample == nullptr) {
        return false;
    }
    sample->render_correlation = 0U;
    sample->vblank = 0U;
    sample->variant = 0U;
    sample->observation = {};
    sample->quadwords.clear();
    sample->observed_quadwords = 0U;
    sample->builder_source_match = false;
    sample->builder_tag_match = false;
    sample->render_source_match = false;
    sample->render_tag_match = false;
    sample->render_root_fallback = false;
    sample->complete = false;
    sample->truncated = false;
    sample->sequence = sequence;
    sample->builder_correlation = static_range != nullptr ? static_range->builder_correlation : 0U;
    if (render_range != nullptr) {
        sample->render_correlation = render_range->render_correlation;
        sample->vblank = render_range->vblank;
    }
    sample->preset = render_range != nullptr ? render_range->preset : static_range->preset;
    sample->preset_index =
        render_range != nullptr ? render_range->preset_index : static_range->preset_index;
    sample->variant = render_range != nullptr ? render_range->variant : static_range->variant;
    sample->observation = observation;
    sample->builder_source_match = builder_source_match;
    sample->builder_tag_match = builder_tag_match;
    sample->render_source_match = render_source_match;
    sample->render_tag_match = render_tag_match;
    sample->render_root_fallback = render_root_fallback;
    impl_->active_xgkick = sample;
    impl_->active_xgkick_is_tail = tail;
    return true;
}
} // namespace takedown::runtime
