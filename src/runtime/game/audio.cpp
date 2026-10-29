#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include "takedown/engine/audio/voice_pan.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace takedown::runtime {

namespace {

std::uint32_t packed_voice_volumes(const engine::VoiceVolumes volumes) noexcept {
    return static_cast<std::uint32_t>(volumes.right) | (static_cast<std::uint32_t>(volumes.left) << 16U);
}

NativeFunctionExecution update_voice_volumes(NativeFunctionContext& context) {
    guest::EeMemory& memory = context.memory;
    const std::uint32_t voice = memory.read_u32_le(context.cpu.read_gpr_u32(4U) + 32U);
    const float volume = std::bit_cast<float>(memory.read_u32_le(voice + 112U)) *
                         std::bit_cast<float>(memory.read_u32_le(voice + 124U));
    const std::uint32_t target = memory.read_u32_le(voice);
    const std::uint32_t flags = memory.read_u32_le(voice + 24U);
    if ((flags & 24U) != 0U) {
        const engine::VoiceVolumes volumes = engine::scaled_voice_volumes(
            std::bit_cast<float>(memory.read_u32_le(voice + 28U)),
            std::bit_cast<float>(memory.read_u32_le(voice + 32U)), volume);
        memory.write_u32_le(target + 16U, packed_voice_volumes(volumes));
        context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
        return NativeFunctionExecution{.retired_instructions = 30U, .ee_cycles = 30U};
    }
    if ((flags & 32U) != 0U) {
        context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
        return NativeFunctionExecution{.retired_instructions = 12U, .ee_cycles = 12U};
    }
    const auto table = [&memory](const std::uint32_t index) {
        return static_cast<std::int16_t>(memory.read_u16_le((index + 2421056U) * 2U));
    };
    const std::uint32_t mode = memory.read_u32_le(memory.read_u32_le(memory.read_u32_le(voice + 4U) + 32U) + 1636U);
    float pan_scale = 1.0F;
    if (mode != 2U) {
        if (mode == 3U) {
            pan_scale = std::bit_cast<float>(memory.read_u32_le(5113564U));
        }
    } else {
        pan_scale = 0.0F;
    }
    memory.write_u32_le(target + 16U,
                        packed_voice_volumes(engine::panned_voice_volumes(
                            std::bit_cast<float>(memory.read_u32_le(voice + 140U) != 0U
                                                     ? memory.read_u32_le(voice + 128U)
                                                     : memory.read_u32_le(voice + 116U)),
                            pan_scale, volume, table)));
    context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
    return NativeFunctionExecution{.retired_instructions = 95U, .ee_cycles = 95U};
}

constexpr std::array<EngineFunction, 1U> kFunctions{{
    {
        .address = 2821296U,
        .code_bytes = 624U,
        .code_fnv1a64 = 0xf33c37e94c9e15efULL,
        .name = "UpdateVoiceVolumes",
        .cost = 150U,
        .implementation = update_voice_volumes,
        .returns = EngineReturn::Nothing,
    },
}};

} // namespace

std::span<const EngineFunction> audio_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
