#include "takedown/runtime/slus_21050_world_object_submit.hpp"

#include "takedown/engine/world/frustum_culling.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr guest::GuestAddress kSubmitWorldObjectAddress = 0x0026AF50U;
constexpr std::size_t kSubmitWorldObjectWordCount = 153U;
constexpr std::uint64_t kGlobalPointer = 0x004E8670U;
// Pointer to the frame's draw-list header.
constexpr guest::GuestAddress kDrawListPointerAddress = 0x004E2A10U;
constexpr guest::GuestAddress kViewTableAddress = 0x01E327D0U;
constexpr std::uint32_t kViewStride = 96U;
constexpr std::size_t kViewRows = 6U;
constexpr std::uint32_t kCullMicroroutineFirstWord = 544U;
constexpr std::uint32_t kMaximumQueued = 4096U;
constexpr std::uint32_t kSubmitRetiredLimit = 17084U;

// Retail code of the routine (words in EE memory order), matched before the
// native version replaces it.
constexpr std::array<std::uint32_t, kSubmitWorldObjectWordCount> kSubmitWorldObjectWords{{
    0x27BDFFF0U, 0x240A0001U, 0x7FB00000U, 0x8F82A3A0U, 0x10CA0020U, 0x8C480000U, 0x80890099U,
    0x3C0201E3U, 0x244227D0U, 0x00091840U, 0x00691821U, 0x00031940U, 0x00431021U, 0xD8A10030U,
    0xD8A60020U, 0xD8A70000U, 0xD8A80010U, 0xD8490020U, 0xD84A0030U, 0xD84B0040U, 0xD84C0050U,
    0xD84D0000U, 0xD84E0010U, 0x4A0050F8U, 0x48420801U, 0x3043FFFFU, 0x14600003U, 0x00000000U,
    0x10000079U, 0x0000102DU, 0x24020002U, 0x14620003U, 0x00000000U, 0x10000003U, 0x0140302DU,
    0x00C0502DU, 0x0140302DU, 0x24020001U, 0x9CA3000CU, 0xA082009BU, 0x0000582DU, 0x8F89A3A0U,
    0x3C025000U, 0x0000502DU, 0x0003183CU, 0x00621025U, 0x8D230004U, 0x71621389U, 0x7C620000U,
    0x24620010U, 0x24490010U, 0x78820000U, 0x7C620010U, 0x8F82A3A0U, 0x1000001CU, 0x24430010U,
    0x000A1100U, 0x252B0010U, 0x00621021U, 0x256B0010U, 0x78500000U, 0x256B0010U, 0x78590010U,
    0x256B0010U, 0x78580020U, 0x256B0010U, 0x784F0030U, 0x256B0010U, 0x784E0040U, 0x256B0010U,
    0x784D0050U, 0x254A0008U, 0x784C0060U, 0x78420070U, 0x7D300000U, 0x7D390010U, 0x7D380020U,
    0x7D2F0030U, 0x7D2E0040U, 0x7D2D0050U, 0x7D2C0060U, 0x7D220070U, 0x25690010U, 0x8F82A3A0U,
    0x8C420000U, 0x2442FFF9U, 0x0142102AU, 0x1440FFE0U, 0x00000000U, 0x10000008U, 0x8F8BA3A0U,
    0x000A1100U, 0x00621021U, 0x254A0001U, 0x78420000U, 0x7D220000U, 0x25290010U, 0x8F8BA3A0U,
    0x8D620000U, 0x0142102AU, 0x1440FFF6U, 0x00000000U, 0xAD600000U, 0x80820098U, 0x00401027U,
    0x00C21024U, 0x01021825U, 0x18600019U, 0xA0860098U, 0x24020002U, 0x10C20009U, 0x00000000U,
    0x24020003U, 0x10C20003U, 0x00000000U, 0x10000004U, 0x0000102DU, 0x10000002U, 0x2402000AU,
    0x0000102DU, 0x0002303CU, 0x0003103CU, 0x0006303EU, 0x0002103FU, 0x0002183CU, 0x30E200FFU,
    0x00621025U, 0x78830010U, 0x70463389U, 0x25220010U, 0x7D230000U, 0x7D260010U, 0x24490010U,
    0x8CA2001CU, 0x10400009U, 0x00000000U, 0x0002103CU, 0x3C035000U, 0x0002103EU, 0x0002203CU,
    0x3C021700U, 0x00831825U, 0x10000002U, 0x70431389U, 0x78820020U, 0x7D220000U, 0x25240010U,
    0x8F83A3A0U, 0x24020001U, 0xAC640004U, 0x7BB00000U, 0x03E00008U, 0x27BD0010U,
}};

// The VU0 microroutine the cull test depends on.
constexpr std::array<std::uint32_t, 116U> kCullMicroroutineWords{{
    0x100200F0U, 0x000002FFU, 0x10060002U, 0x000002FFU, 0x80000170U, 0x000002FFU, 0x80001130U,
    0x01E061BFU, 0x12063001U, 0x01E148FCU, 0x8000033CU, 0x01E150FDU, 0x8000033CU, 0x01E1580EU,
    0x8000033CU, 0x01E061BFU, 0x8000033CU, 0x01E248FCU, 0x8000033CU, 0x01E250FDU, 0x34031000U,
    0x01E2580EU, 0x80032134U, 0x01E061BFU, 0x80032975U, 0x01E348FCU, 0x8000033CU, 0x01E350FDU,
    0x34031000U, 0x01E3580EU, 0x80032134U, 0x01E061BFU, 0x80032975U, 0x01E448FCU, 0x8000033CU,
    0x01E450FDU, 0x34031000U, 0x01E4580EU, 0x80032134U, 0x01E061BFU, 0x80032975U, 0x01E548FCU,
    0x8000033CU, 0x01E550FDU, 0x34031000U, 0x01E5580EU, 0x80032134U, 0x01E061BFU, 0x80032975U,
    0x01E648FCU, 0x8000033CU, 0x01E650FDU, 0x34031000U, 0x01E6580EU, 0x80032134U, 0x01E061BFU,
    0x80032975U, 0x01E748FCU, 0x8000033CU, 0x01E750FDU, 0x34031000U, 0x01E7580EU, 0x80032134U,
    0x01E061BFU, 0x80032975U, 0x01E848FCU, 0x8000033CU, 0x01E850FDU, 0x34031000U, 0x01E8580EU,
    0x80032134U, 0x010D0303U, 0x80032975U, 0x010D0240U, 0x8000033CU, 0x010D0281U, 0x34031000U,
    0x010D02C2U, 0x80032134U, 0x008E0303U, 0x80032975U, 0x008E0240U, 0x52040006U, 0x008E0281U,
    0x10010000U, 0x008E02C2U, 0x520607D7U, 0x000002FFU, 0x100200C0U, 0x000002FFU, 0x50050002U,
    0x000002FFU, 0x10010002U, 0x000002FFU, 0x10010001U, 0x000002FFU, 0x8000033CU, 0x000002FFU,
    0x8000033CU, 0x600002FFU, 0x8000033CU, 0x200002FFU, 0x8000033CU, 0x01C608A8U, 0x8000033CU,
    0x01C70928U, 0x8000033CU, 0x01C80968U, 0x8000033CU, 0x01C710E8U, 0x8000033CU, 0x01C811A8U,
    0x400007C7U, 0x01C819E8U, 0x8000033CU, 0x01C82228U,
}};

[[nodiscard]] bool body_matches(const guest::EeMemory& memory) {
    for (std::size_t index = 0U; index != kSubmitWorldObjectWords.size(); ++index) {
        if (memory.read_u32_le((kSubmitWorldObjectAddress / 4U + static_cast<guest::GuestAddress>(index)) * 4U) !=
            kSubmitWorldObjectWords[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_retail_body(const guest::EeMemory& memory) {
    struct Cache {
        const guest::EeMemory* memory{};
        guest::EeMainRamCodePageToken tokens[2]{};
        bool result{};
    };
    static Cache cache;
    if (cache.memory == &memory && memory.validate_main_ram_code_page(cache.tokens[0]) &&
        memory.validate_main_ram_code_page(cache.tokens[1])) {
        return cache.result;
    }
    const auto head = memory.capture_main_ram_code_page(kSubmitWorldObjectAddress);
    const auto tail = memory.capture_main_ram_code_page(
        kSubmitWorldObjectAddress + static_cast<guest::GuestAddress>(kSubmitWorldObjectWordCount * 4U - 1U));
    const bool result = body_matches(memory);
    if (head.status == guest::EeMainRamCodePageCaptureStatus::Captured &&
        tail.status == guest::EeMainRamCodePageCaptureStatus::Captured) {
        cache = Cache{&memory, {head.token, tail.token}, result};
    } else {
        cache = Cache{};
    }
    return result;
}

bool has_cull_microroutine(const guest::EeMemory& memory) {
    struct Cache {
        const guest::EeMemory* memory{};
        std::uint64_t generation{};
        bool result{};
    };
    static Cache cache;
    const std::uint64_t generation = memory.vu0_micro_generation();
    if (cache.memory == &memory && cache.generation == generation) {
        return cache.result;
    }
    const auto image = memory.vu0_micro_image();
    bool result = true;
    for (std::size_t index = 0U; index != kCullMicroroutineWords.size() && result; ++index) {
        const std::size_t offset = (index + kCullMicroroutineFirstWord) * 4U;
        std::uint32_t word = 0U;
        for (std::size_t byte = 0U; byte != 4U; ++byte) {
            word |= static_cast<std::uint32_t>(image[offset + byte]) << (byte * 8U);
        }
        result = word == kCullMicroroutineWords[index];
    }
    cache.memory = &memory;
    cache.generation = generation;
    cache.result = result;
    return result;
}

[[nodiscard]] guest::Gpr128 read_qword(const guest::EeMemory& memory, const guest::GuestAddress address) {
    guest::Gpr128 value{};
    for (std::size_t word = 0U; word != 4U; ++word) {
        value.words[word] = memory.read_u32_le(static_cast<guest::GuestAddress>(word) * 4U + address);
    }
    return value;
}

engine::Vec4 load_vec4(const guest::Gpr128& qword) {
    __m128i raw;
    std::memcpy(&raw, qword.words.data(), sizeof raw);
    return _mm_castsi128_ps(raw);
}

struct Range {
    unsigned long long begin;
    unsigned long long end;
};

Range range_of(const guest::GuestAddress address, const std::uint64_t bytes) {
    const unsigned long long begin = address;
    const unsigned long long end = begin + bytes;
    return {begin, end};
}

// True when the whole range lies in main RAM or in the scratchpad.
bool addressable(const guest::EeMemory& memory, const Range range) {
    return range.end <= memory.size() || (range.begin > 0x6FFFFFFFULL && range.end <= 0x70004000ULL);
}

bool overlaps(const Range a, const Range b) {
    return a.end > b.begin && a.begin < b.end;
}

// Everything the routine decides from the call state, before it changes any.
struct CullInputs {
    std::array<guest::Gpr128, 4U> box{};
    std::array<guest::Gpr128, kViewRows> frustum{};
};

struct SubmitPlan {
    engine::Visibility visibility{engine::Visibility::Inside};
    bool tested;
    bool sends_clip_change;
    std::uint64_t clip_mode{};
    std::uint32_t object{};
    std::uint32_t record{};
    std::uint32_t stack_slot{};
    std::uint32_t draw_list{};
    std::uint32_t retired{};
    CullInputs cull_inputs{};
    std::uint32_t write_cursor{};
    int queued{};
    std::uint32_t tail_program{};
    std::uint64_t clip_change_flags{};
};

[[nodiscard]] std::optional<SubmitPlan> plan_submit(const guest::R5900State& cpu, const guest::EeMemory& memory) {
    if (!has_retail_body(memory) || cpu.read_gpr_u64(28U) != kGlobalPointer) {
        return std::nullopt;
    }
    if (memory.vu0_micro_call_observer() != nullptr) {
        return std::nullopt;
    }
    SubmitPlan plan{};
    plan.object = cpu.read_gpr_u32(4U);
    plan.record = cpu.read_gpr_u32(5U);
    plan.stack_slot = cpu.read_gpr_u32(29U) - 16U;
    if (cpu.read_gpr_u64(4U) != plan.object || cpu.read_gpr_u64(5U) != plan.record ||
        cpu.read_gpr_u64(29U) != cpu.read_gpr_u32(29U) ||
        (plan.object & 15U) != 0U || (plan.record & 15U) != 0U ||
        (cpu.read_gpr_u32(29U) & 15U) != 0U) {
        return std::nullopt;
    }
    if (!addressable(memory, range_of(plan.object, 160U)) || !addressable(memory, range_of(plan.record, 64U)) ||
        !addressable(memory, range_of(plan.stack_slot, 16U))) {
        return std::nullopt;
    }
    if (memory.size() <= kDrawListPointerAddress + 3U) {
        return std::nullopt;
    }
    plan.draw_list = memory.read_u32_le(kDrawListPointerAddress);
    if ((plan.draw_list & 15U) != 0U || !addressable(memory, range_of(plan.draw_list, 16U))) {
        return std::nullopt;
    }
    const std::uint32_t entry_word = memory.read_u32_le(plan.draw_list);
    plan.clip_mode = cpu.read_gpr_u64(6U);
    plan.tested = plan.clip_mode != 1U;
    std::uint32_t retired = 6U;
    if (plan.tested) {
        if (!has_cull_microroutine(memory)) {
            return std::nullopt;
        }
        const auto view_index = static_cast<std::int8_t>(memory.read_u8(plan.object + 153U));
        const auto view = static_cast<guest::GuestAddress>(view_index) * kViewStride + kViewTableAddress;
        if (static_cast<std::uint64_t>(view + kViewStride) > memory.size()) {
            return std::nullopt;
        }
        plan.cull_inputs.box = {read_qword(memory, plan.record), read_qword(memory, plan.record + 16U),
                                read_qword(memory, plan.record + 32U),
                                read_qword(memory, plan.record + 48U)};
        for (std::size_t row = 0U; row != kViewRows; ++row) {
            plan.cull_inputs.frustum[row] =
                read_qword(memory, view + static_cast<guest::GuestAddress>(row) * 16U);
        }
        const engine::OrientedBox box{load_vec4(plan.cull_inputs.box[3]), load_vec4(plan.cull_inputs.box[0]),
                                      load_vec4(plan.cull_inputs.box[1]), load_vec4(plan.cull_inputs.box[2])};
        const engine::Frustum frustum{load_vec4(plan.cull_inputs.frustum[0]), load_vec4(plan.cull_inputs.frustum[1]),
                                      load_vec4(plan.cull_inputs.frustum[2]), load_vec4(plan.cull_inputs.frustum[3]),
                                      load_vec4(plan.cull_inputs.frustum[4]), load_vec4(plan.cull_inputs.frustum[5])};
        retired = 33U;
        plan.visibility = engine::classify(box, frustum);
        switch (plan.visibility) {
        case engine::Visibility::Outside:
            plan.retired = retired;
            return plan;
        case engine::Visibility::Inside:
            plan.clip_mode = 1U;
            break;
        default:
            break;
        }
    }
    plan.write_cursor = memory.read_u32_le(plan.draw_list + 4U);
    plan.queued = static_cast<int>(memory.read_u32_le(plan.draw_list));
    if (plan.queued > static_cast<int>(kMaximumQueued) || (plan.write_cursor & 15U) != 0U) {
        return std::nullopt;
    }
    const auto flushed = static_cast<std::uint32_t>(std::max(plan.queued, 0));
    const std::uint64_t queue_bytes = static_cast<std::uint64_t>(std::max(plan.queued, 0) + 1) * 16U;
    const std::uint64_t written_bytes = static_cast<std::uint64_t>(flushed + 5U) * 16U;
    if (!addressable(memory, range_of(plan.draw_list, queue_bytes)) ||
        !addressable(memory, range_of(plan.write_cursor, written_bytes))) {
        return std::nullopt;
    }
    const Range object_range = range_of(plan.object, 160U);
    const Range record_range = range_of(plan.record, 64U);
    const Range stack_range = range_of(plan.stack_slot, 16U);
    const Range queue_range = range_of(plan.draw_list, queue_bytes);
    const Range write_range = range_of(plan.write_cursor, written_bytes);
    const Range pointer_range = range_of(kDrawListPointerAddress, 4U);
    const Range code_range = range_of(kSubmitWorldObjectAddress, kSubmitWorldObjectWordCount * 4U);
    if (overlaps(queue_range, write_range) || overlaps(object_range, write_range) ||
        overlaps(record_range, write_range) || overlaps(stack_range, write_range) ||
        overlaps(pointer_range, write_range) || overlaps(code_range, write_range) ||
        overlaps(object_range, queue_range) || overlaps(object_range, stack_range) ||
        overlaps(queue_range, stack_range)) {
        return std::nullopt;
    }
    retired += 19U;
    const std::uint32_t batches = flushed / 8U;
    const std::uint32_t singles = flushed % 8U;
    retired += batches * 33U + singles * 11U;
    retired += 19U;
    plan.clip_change_flags =
        static_cast<std::uint64_t>(static_cast<std::int32_t>(entry_word)) |
        (plan.clip_mode & ~static_cast<std::uint64_t>(static_cast<std::int8_t>(memory.read_u8(plan.object + 152U))));
    plan.sends_clip_change = static_cast<std::int64_t>(plan.clip_change_flags) > 0;
    if (plan.sends_clip_change) {
        retired += plan.clip_mode == 2U ? 16U : 21U;
    }
    plan.tail_program = memory.read_u32_le(plan.record + 28U);
    retired += plan.tail_program == 0U ? 12U : 19U;
    plan.retired = retired;
    return plan;
}

struct Prepared {
    const guest::R5900State* cpu{};
    std::optional<SubmitPlan> plan;
};

thread_local Prepared t_prepared;

std::optional<NativeFunctionLimits> prepare_submit_world_object(const NativeFunctionPrepareContext& context) {
    t_prepared = {&context.cpu, plan_submit(context.cpu, context.memory)};
    if (!t_prepared.plan) {
        return std::nullopt;
    }
    return NativeFunctionLimits{t_prepared.plan->retired, t_prepared.plan->retired};
}

void write_u64(guest::R5900State& cpu, const std::size_t index, const std::uint64_t value) {
    cpu.write_gpr_u64(index, value);
}

std::uint64_t clip_change_command(const std::uint64_t mode) {
    return mode == 2U ? 2U : (mode == 3U ? 10U : 0U);
}

guest::Gpr128 tail_call_command(const std::uint32_t program) {
    return guest::Gpr128{{0x50000000U, program, 0x17000000U, 0U}};
}


NativeFunctionExecution submit_world_object(NativeFunctionContext& context) {
    guest::R5900State& cpu = context.cpu;
    guest::EeMemory& memory = context.memory;
    if (t_prepared.cpu != &cpu || !t_prepared.plan) {
        t_prepared = {&cpu, plan_submit(cpu, memory)};
        if (!t_prepared.plan) {
            throw std::logic_error("unprepared world object submission reached native execution");
        }
    }
    const SubmitPlan plan = *t_prepared.plan;
    t_prepared = {};

    const guest::Gpr128 entry_s0 = cpu.read_gpr(16U);
    memory.write_u128_le(plan.stack_slot, entry_s0.words);
    const std::uint32_t first_word = memory.read_u32_le(plan.draw_list);
    write_u64(cpu, 10U, 1U);
    write_u64(cpu, 2U, static_cast<std::uint64_t>(static_cast<std::int32_t>(plan.draw_list)));
    write_u64(cpu, 8U, static_cast<std::uint64_t>(static_cast<std::int32_t>(first_word)));
    if (plan.tested) {
        const auto view_index = static_cast<std::int8_t>(memory.read_u8(plan.object + 153U));
        write_u64(cpu, 9U, static_cast<std::uint64_t>(view_index));
        write_u64(cpu, 3U, static_cast<std::uint64_t>(plan.visibility));
        if (plan.visibility == engine::Visibility::Outside) {
            write_u64(cpu, 2U, 0U);
            cpu.set_pc(cpu.read_gpr_u32(31U));
            return {plan.retired, plan.retired};
        }
        write_u64(cpu, 2U, 2U);
        if (plan.visibility == engine::Visibility::Intersecting) {
            write_u64(cpu, 10U, plan.clip_mode);
        }
        write_u64(cpu, 6U, plan.clip_mode);
    }
    const std::uint32_t program = memory.read_u32_le(plan.record + 12U);
    memory.write_u8(plan.object + 155U, 1U);
    memory.write_u128_le(plan.write_cursor, guest::Gpr128{{0x50000000U, program, 0U, 0U}}.words);
    const guest::Gpr128 header = read_qword(memory, plan.object);
    memory.write_u128_le(plan.write_cursor + 16U, header.words);
    cpu.write_gpr(2U, header);
    guest::GuestAddress out = plan.write_cursor + 32U;
    const auto source = plan.draw_list + 16U;
    const auto flushed = static_cast<std::uint32_t>(plan.queued > 0 ? plan.queued : 0);
    std::uint32_t copied = 0U;
    while (copied + 8U <= flushed) {
        std::array<guest::Gpr128, 8U> batch{};
        for (std::size_t i = 0U; i != 8U; ++i) {
            batch[i] = read_qword(memory, (copied + i) * 16U + source);
            memory.write_u128_le(static_cast<std::uint32_t>(i) * 16U + out, batch[i].words);
        }
        cpu.write_gpr(16U, batch[0]);
        cpu.write_gpr(25U, batch[1]);
        cpu.write_gpr(24U, batch[2]);
        cpu.write_gpr(15U, batch[3]);
        cpu.write_gpr(14U, batch[4]);
        cpu.write_gpr(13U, batch[5]);
        cpu.write_gpr(12U, batch[6]);
        cpu.write_gpr(2U, batch[7]);
        write_u64(cpu, 11U, static_cast<std::uint64_t>(static_cast<std::int32_t>(out + 112U)));
        out += 128U;
        copied += 8U;
    }
    for (; copied < flushed; ++copied) {
        const guest::Gpr128 entry = read_qword(memory, copied * 16U + source);
        memory.write_u128_le(out, entry.words);
        cpu.write_gpr(2U, entry);
        out += 16U;
    }
    memory.write_u32_le(plan.draw_list, 0U);
    write_u64(cpu, 10U, flushed);
    write_u64(cpu, 11U, static_cast<std::uint64_t>(static_cast<std::int32_t>(plan.draw_list)));
    write_u64(cpu, 3U, static_cast<std::uint64_t>(static_cast<std::int32_t>(source)));
    write_u64(cpu, 2U, 0U);
    const auto previous_mode = static_cast<std::int8_t>(memory.read_u8(plan.object + 152U));
    write_u64(cpu, 2U, plan.clip_mode & ~static_cast<std::uint64_t>(previous_mode));
    write_u64(cpu, 3U, plan.clip_change_flags);
    memory.write_u8(plan.object + 152U, static_cast<std::uint8_t>(plan.clip_mode));
    if (plan.sends_clip_change) {
        const auto command = clip_change_command(plan.clip_mode);
        const guest::Gpr128 state = read_qword(memory, plan.object + 16U);
        const auto flags_and_arg = (plan.clip_change_flags << 32U) | (cpu.read_gpr_u64(7U) & 0xFFU);
        const guest::Gpr128 change{{static_cast<std::uint32_t>(command), static_cast<std::uint32_t>(command >> 32U),
                                    static_cast<std::uint32_t>(flags_and_arg),
                                    static_cast<std::uint32_t>(flags_and_arg >> 32U)}};
        memory.write_u128_le(out, state.words);
        memory.write_u128_le(out + 16U, change.words);
        cpu.write_gpr(3U, state);
        cpu.write_gpr(6U, change);
        out += 32U;
    }
    guest::Gpr128 tail{};
    if (plan.tail_program != 0U) {
        tail = tail_call_command(plan.tail_program);
    } else {
        tail = read_qword(memory, plan.object + 32U);
    }
    memory.write_u128_le(out, tail.words);
    cpu.write_gpr(2U, tail);
    if (plan.tail_program != 0U) {
        write_u64(cpu, 4U, static_cast<std::uint64_t>(plan.tail_program) << 32U);
    }
    out += 16U;
    write_u64(cpu, 4U, static_cast<std::uint64_t>(static_cast<std::int32_t>(out)));
    write_u64(cpu, 9U, static_cast<std::uint64_t>(static_cast<std::int32_t>(out - 16U)));
    write_u64(cpu, 3U, static_cast<std::uint64_t>(static_cast<std::int32_t>(plan.draw_list)));
    write_u64(cpu, 2U, 1U);
    memory.write_u32_le(plan.draw_list + 4U, out);
    cpu.write_gpr(16U, entry_s0);
    cpu.set_pc(cpu.read_gpr_u32(31U));
    return {plan.retired, plan.retired};
}

} // namespace

bool register_slus_21050_world_object_submit_native_function(NativeFunctionDispatcher& dispatcher,
                                                             const guest::EeMemory& memory) {
    if (!has_retail_body(memory) || native_function_disabled_by_environment("SubmitWorldObject")) {
        return false;
    }
    return dispatcher
        .register_function(kSubmitWorldObjectAddress, "SubmitWorldObject",
                           NativeFunctionLimits{kSubmitRetiredLimit, kSubmitRetiredLimit},
                           NativeFunctionTraits{.scheduler_state_transparent = true},
                           &prepare_submit_world_object, &submit_world_object)
        .registered();
}

} // namespace takedown::runtime::slus_21050
