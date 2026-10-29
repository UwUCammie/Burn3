#include "takedown/runtime/slus_21050_render_state_packet.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

namespace takedown::runtime::slus_21050 {

namespace {

// Object state words are OR-ed into the low doubleword of a template quadword.
RenderStateQuadword merge_object_state(const RenderStateQuadword& base, const std::uint64_t state,
                                       const std::uint32_t command) noexcept {
    return {base[0] | static_cast<std::uint32_t>(state), base[1] | static_cast<std::uint32_t>(state >> 32U),
            command, 0U};
}

} // namespace

RenderStatePacket build_render_state_packet(const RenderStatePacketInput& input) noexcept {
    RenderStatePacket packet{};
    packet[0] = {0x1000000AU, 0U, 0x01000404U, 0x6C0A03F6U};
    std::memcpy(packet.data() + 1, input.state_template.data(), 10U * sizeof(RenderStateQuadword));
    packet[11] = {0x10000005U, 0U, 0U, 0x50000005U};
    packet[12] = {0x8004U, 0x10000000U, 14U, 0U};
    packet[13] = merge_object_state(input.state_template[10], input.object_state_a, 76U);
    packet[14] = merge_object_state(input.state_template[11], input.object_state_b, 78U);
    packet[15] = input.state_template[12];
    packet[16] = input.state_template[13];
    return packet;
}

namespace {

constexpr guest::GuestAddress kSelectionAddress = 0x00667050U;
constexpr guest::GuestAddress kTemplateBase = 0x00666450U;
constexpr std::uint32_t kTemplateStride = 560U;
constexpr std::size_t kEntryBytes = 448U;
constexpr std::uint64_t kEntryCodeHash = 6623210478386708784ULL;

struct Range {
    guest::GuestAddress address{};
    std::uint32_t bytes{};
};

bool direct_ram(const guest::EeMemory& memory, const Range range) {
    return static_cast<std::uint64_t>(range.address) + range.bytes <= memory.size() &&
           memory.is_main_ram_range(range.address, range.bytes);
}

guest::GuestAddress quadword_address(const guest::GuestAddress base, const std::size_t slot) {
    return base + static_cast<std::uint32_t>(slot) * 16U;
}

bool overlaps(const Range a, const Range b) {
    return static_cast<std::uint64_t>(a.address) + a.bytes > b.address &&
           a.address < static_cast<std::uint64_t>(b.address) + b.bytes;
}

RenderStateQuadword read_quadword(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return {memory.read_u32_le(address), memory.read_u32_le(address + 4U), memory.read_u32_le(address + 8U),
            memory.read_u32_le(address + 12U)};
}

struct Invocation {
    guest::GuestAddress object{};
    guest::GuestAddress source{};
    guest::GuestAddress output{};
    int selection{};
};

[[nodiscard]] std::optional<Invocation> parse_invocation(const NativeFunctionPrepareContext& context) {
    const guest::R5900State& cpu = context.cpu;
    const guest::EeMemory& memory = context.memory;
    if (cpu.pc() != kBuildRenderStatePacketAddress || cpu.is_executing_delay_slot()) {
        return std::nullopt;
    }
    const Range object{cpu.read_gpr_u32(4U), 184U};
    const Range pointer{cpu.read_gpr_u32(28U) - 0x5C60U, 4U};
    if ((object.address & 7U) != 0U || (cpu.read_gpr_u32(28U) & 3U) != 0U || !direct_ram(memory, object) || !direct_ram(memory, pointer) ||
        !direct_ram(memory, Range{kSelectionAddress, 4U})) {
        return std::nullopt;
    }
    const int selection = static_cast<std::int8_t>(memory.read_u8(object.address + 153U));
    const Range source{static_cast<std::uint32_t>(selection * 560 + 6710352), 224U};
    const Range output{memory.read_u32_le(pointer.address), 288U};
    const bool scratchpad =
        output.address > 0x6FFFFFFFU && static_cast<std::uint64_t>(output.address) + 288U <= 0x70004000ULL;
    if ((output.address & 15U) != 0U || !direct_ram(memory, source) ||
        (!scratchpad && !direct_ram(memory, output))) {
        return std::nullopt;
    }
    const std::array<Range, 6U> ranges{
        object, pointer, source, output, Range{kSelectionAddress, 4U}, Range{kBuildRenderStatePacketAddress, kEntryBytes}};
    for (std::size_t i = 0U; i < ranges.size(); ++i) {
        for (std::size_t j = i + 1U; j < ranges.size(); ++j) {
            if (overlaps(ranges[i], ranges[j])) {
                return std::nullopt;
            }
        }
    }
    return Invocation{object.address, source.address, output.address, selection};
}

NativeFunctionExecution execute(NativeFunctionContext& context) {
    const auto invocation = parse_invocation({context.cpu, context.memory, context.entry_address});
    if (!invocation) {
        throw std::logic_error("render-state packet admission changed");
    }
    guest::R5900State& cpu = context.cpu;
    guest::EeMemory& memory = context.memory;

    RenderStatePacketInput input{};
    for (std::size_t i = 0U; i < input.state_template.size(); ++i) {
        input.state_template[i] = read_quadword(memory, (*invocation).source + static_cast<std::uint32_t>(i) * 16U);
    }
    input.object_state_a = memory.read_u64_le((*invocation).object + 160U);
    input.object_state_b = memory.read_u64_le((*invocation).object + 176U);
    const auto packet = build_render_state_packet(input);

    memory.write_u32_le(kSelectionAddress, static_cast<std::uint32_t>((*invocation).selection));
    memory.write_u32_le((*invocation).output, 17U);
    for (std::size_t i = 0U; i < packet.size(); ++i) {
        memory.write_u128_le(quadword_address((*invocation).output, i + 1U), packet[i]);
    }

    cpu.write_gpr_sign_extended_32(3U, (*invocation).output + 256U);
    cpu.write_gpr_sign_extended_32(4U, 17U);
    cpu.write_gpr_sign_extended_32(5U, (*invocation).output);
    cpu.write_gpr(6U, {input.state_template[13]});
    cpu.write_gpr(7U, {input.state_template[11]});
    cpu.write_gpr_sign_extended_32(7U, (*invocation).output);
    cpu.write_gpr(8U, {{(*invocation).output, 0U, 76U, 0U}});
    cpu.write_gpr(9U, {input.state_template[9]});
    cpu.write_gpr_sign_extended_32(9U, (*invocation).output);
    cpu.write_gpr(10U, {input.state_template[10]});
    cpu.write_gpr(11U, {{(*invocation).output, 0U, 0U, 0x50000005U}});
    cpu.set_pc(cpu.read_gpr_u32(31U));
    return {229U, 229U};
}

// Promote only the shipped routine: its code bytes are hashed (FNV-1a).
bool matches_entry(const guest::EeMemory& ee) {
    if (!ee.is_main_ram_range(kBuildRenderStatePacketAddress, kEntryBytes)) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : ee.view(kBuildRenderStatePacketAddress, kEntryBytes)) {
        hash = (hash ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
    }
    return hash == kEntryCodeHash;
}

} // namespace

bool register_render_state_packet_native_function(NativeFunctionDispatcher& dispatcher,
                                                  const guest::EeMemory& memory) {
    if (!matches_entry(memory)) {
        return false;
    }
    auto page = memory.capture_main_ram_code_page(kBuildRenderStatePacketAddress);
    return dispatcher
        .register_function(
            kBuildRenderStatePacketAddress, "BuildRenderStatePacket", NativeFunctionLimits{229U, 229U},
            NativeFunctionTraits{.scheduler_state_transparent = true},
            [page](const NativeFunctionPrepareContext& context) mutable
                -> std::optional<NativeFunctionLimits> {
                if (!context.memory.validate_main_ram_code_page(page.token)) {
                    if (!matches_entry(context.memory)) {
                        return std::nullopt;
                    }
                    page = context.memory.capture_main_ram_code_page(kBuildRenderStatePacketAddress);
                }
                return parse_invocation(context) ? std::optional<NativeFunctionLimits>{NativeFunctionLimits{229U, 229U}}
                                                 : std::nullopt;
            },
            &execute)
        .registered();
}

} // namespace takedown::runtime::slus_21050
