#include "takedown/runtime/slus_21050_graphics_options.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace takedown::runtime {

const std::array<Slus21050GraphicsOptions::Override, 5U> Slus21050GraphicsOptions::kOverrides{{
    {6708941U, 1, 0U, 1U, Group::Widescreen},
    {5114424U, 4, 1067030938U, 1068149419U, Group::Widescreen},
    {5115004U, 4, 1070554153U, 1071877689U, Group::Widescreen},
    {5114940U, 4, 1068149419U, 1071877689U, Group::Widescreen},
    {5114516U, 4, 1066052267U, 1069081487U, Group::Widescreen},
}};

std::optional<std::string>
Slus21050GraphicsOptions::signature_mismatch(const guest::EeMemory& memory) {
    for (const Override& entry : kOverrides) {
        if (!memory.contains(entry.address, entry.bytes)) {
            std::ostringstream message;
            message << "graphics option address 0x" << std::hex << std::setw(8) << std::setfill('0')
                    << entry.address << " is outside guest memory";
            return message.str();
        }
        if (entry.bytes != 4) {
            continue;
        }
        const std::uint32_t value = memory.read_u32_le(entry.address);
        if (entry.retail != value && entry.replacement != value) {
            std::ostringstream message;
            message << "graphics option value at 0x" << std::hex << std::setw(8) << std::setfill('0')
                    << entry.address << " is 0x" << std::setw(8) << value << ", expected 0x"
                    << std::setw(8) << entry.retail;
            return message.str();
        }
    }
    return std::nullopt;
}

bool Slus21050GraphicsOptions::group_active(const Group group) const noexcept {
    switch (group) {
    case Group::Widescreen:
        return settings_.widescreen;
    }
    return false;
}

bool Slus21050GraphicsOptions::register_native_effects(NativeFunctionDispatcher& dispatcher,
                                                       const guest::EeMemory& memory) {
    struct Effect {
        guest::GuestAddress begin;
        guest::GuestAddress end;
        std::uint64_t hash;
        bool blur;
        const char* name;
    };
    const std::array<Effect, 4U> effects{{
        {3123616U, 3124624U, 479794196314929007ULL, true, "NativeRadialBlurPolicy"},
        {4353040U, 4354160U, 2459291729203850742ULL, true, "NativeCameraRadialBlurPolicy"},
        {2365888U, 2366400U, 11133449368403337782ULL, true, "NativeScreenFilterPolicy"},
        {2513344U, 2514048U, 18370794986758281360ULL, false, "NativeBloomPolicy"},
    }};
    for (const Effect& effect : effects) {
        if (!memory.contains(effect.begin, effect.end - effect.begin)) {
            return false;
        }
        if (dispatcher.lookup(effect.begin) != nullptr) {
            return false;
        }
        std::uint64_t hash = 14695981039346656037ULL;
        for (guest::GuestAddress at = effect.begin; at < effect.end; ++at) {
            hash = (hash ^ memory.read_u8(at)) * 1099511628211ULL;
        }
        if (effect.hash != hash) {
            return false;
        }
    }
    for (const Effect& effect : effects) {
        const bool blur = effect.blur;
        if (dispatcher
                .register_function(
                    effect.begin, std::string(effect.name),
                    NativeFunctionLimits{.maximum_retired_instructions = 1U, .maximum_ee_cycles = 1U},
                    NativeFunctionTraits{.scheduler_state_transparent = true,
                                         .single_step_host_call = true,
                                         .defers_dmac_completions = false},
                    [this, blur](const NativeFunctionPrepareContext&)
                        -> std::optional<NativeFunctionLimits> {
                        if (blur ? settings_.motion_blur : settings_.bloom) {
                            return {};
                        }
                        return NativeFunctionLimits{.maximum_retired_instructions = 1U,
                                                    .maximum_ee_cycles = 1U};
                    },
                    [this](NativeFunctionContext& context) -> NativeFunctionExecution {
                        ++statistics_.effects_skipped;
                        context.cpu.set_pc(context.cpu.read_gpr_u32(31U));
                        return NativeFunctionExecution{.retired_instructions = 1U, .ee_cycles = 1U};
                    })
                .status != NativeFunctionRegistrationStatus::Registered) {
            return false;
        }
    }
    return true;
}

void Slus21050GraphicsOptions::on_vblank(guest::EeMemory& memory) {
    ++statistics_.vblanks;
    if (!any_applied_ && settings_ == Settings{}) {
        return;
    }
    bool any_applied = false;
    for (std::size_t index = 0; index != 5U; ++index) {
        const Override& entry = kOverrides[index];
        switch (entry.group) {
        case Group::Widescreen:
            if (settings_.widescreen) {
                const std::uint8_t bytes = entry.bytes;
                const guest::GuestAddress address = entry.address;
                const std::uint32_t current = bytes == 1 ? static_cast<std::uint32_t>(memory.read_u8(address))
                                                         : memory.read_u32_le(address);
                const std::uint32_t replacement = entry.replacement;
                if (current != replacement) {
                    if (bytes == 1) {
                        memory.write_u8(address, static_cast<std::uint8_t>(replacement));
                    } else {
                        memory.write_u32_le(address, replacement);
                    }
                    ++statistics_.writes;
                }
                applied_[index] = true;
                any_applied = true;
                continue;
            }
            break;
        }
        if (applied_[index]) {
            const std::uint32_t retail = entry.retail;
            const std::uint8_t bytes = entry.bytes;
            const guest::GuestAddress address = entry.address;
            if (bytes == 1) {
                memory.write_u8(address, static_cast<std::uint8_t>(retail));
            } else {
                memory.write_u32_le(address, retail);
            }
            ++statistics_.restores;
            applied_[index] = false;
        }
    }
    any_applied_ = any_applied;
}

} // namespace takedown::runtime
