#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::runtime {

namespace {

struct UnpackedName {
    std::array<char, 13> text{};
    std::uint64_t overflow{};
};

NativeFunctionExecution unpack_name(NativeFunctionContext& context) {
    const guest::Gpr128 value_word = context.cpu.read_gpr(0U);
    std::uint64_t value = value_word.words[0] | (static_cast<std::uint64_t>(value_word.words[1]) << 32U);
    UnpackedName name{};
    for (std::size_t index = 12U; index != 0U; --index) {
        const std::size_t position = index - 1U;
        const std::uint64_t digit = value % 40U;
        if (digit == 39U) {
            name.text[position] = '_';
        } else if (digit > 12U) {
            name.text[position] = static_cast<char>(static_cast<std::uint8_t>(digit) + 52U);
        } else if (digit > 2U) {
            name.text[position] = static_cast<char>(static_cast<std::uint8_t>(digit) + 45U);
        } else if (digit == 2U) {
            name.text[position] = '/';
        } else if (digit == 1U) {
            name.text[position] = '-';
        } else {
            name.text[position] = ' ';
        }
        value /= 40U;
    }
    name.text[12] = 0;
    std::size_t length = 12U;
    while (length != 0U && name.text[length - 1U] == ' ') {
        name.text[length - 1U] = 0;
        --length;
    }
    name.overflow = value;
    const std::uint32_t destination = context.cpu.read_gpr_u32(5U);
    for (std::size_t index = 0U; index != 13U; ++index) {
        context.memory.write_u8(destination + static_cast<std::uint32_t>(index),
                                static_cast<std::uint8_t>(name.text[index]));
    }
    context.cpu.write_gpr_u64(2U, name.overflow);
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return NativeFunctionExecution{.retired_instructions = 2500U, .ee_cycles = 2500U};
}

constexpr std::array<EngineFunction, 1U> kFunctions{{
    {
        .address = 2206528U,
        .code_bytes = 292U,
        .code_fnv1a64 = 15340758557658773117ULL,
        .name = "UnpackName",
        .cost = 3000U,
        .implementation = unpack_name,
    },
}};

} // namespace

std::span<const EngineFunction> content_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
