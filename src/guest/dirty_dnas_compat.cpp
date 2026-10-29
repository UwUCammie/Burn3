#include "takedown/guest/dirty_dnas_compat.hpp"

#include <algorithm>
#include <array>

namespace takedown::guest {
namespace {

struct WordSignature {
    GuestAddress address{};
    std::uint32_t word{};
};

// Retail instruction words that identify the DirtyElf wrapper revision.
constexpr std::array<WordSignature, 12> kRetailSignatures{{
    {0x0041be58U, 0x27bdffc0U}, {0x0041be5cU, 0x3c0201ecU}, {0x0026c15cU, 0x0c106f96U},
    {0x0026c160U, 0x8c6429d4U}, {0x0026c168U, 0x0c106f8aU}, {0x0026c16cU, 0x24050001U},
    {0x0026bef0U, 0x0c106f8cU}, {0x0026bef4U, 0x0000202dU}, {0x0026bef8U, 0x0002102bU},
    {0x0026bf0cU, 0x0c106f8eU}, {0x0026bf10U, 0x27a40020U}, {0x0026bf14U, 0x0c106fe4U},
}};

constexpr std::array<GuestAddress, 4> kTrampolines{0x0041be28U, 0x0041be30U, 0x0041be38U,
                                                   0x0041be40U};
constexpr std::array<std::uint32_t, 4> kExportOffsets{1584U, 1936U, 2232U, 2368U};
constexpr std::array<WordSignature, 20> kModuleSignatures{{
    {1584U, 0x27bdffa0U}, {1592U, 0x7fb10040U}, {1596U, 0x7fb20030U}, {1600U, 0x00a0882dU},
    {1604U, 0x7fb30020U}, {1612U, 0x7fb00050U}, {1616U, 0xffbf0010U}, {1936U, 0x27bdffd0U},
    {1944U, 0x7fb00020U}, {1948U, 0x7fb10010U}, {1956U, 0xffbf0000U}, {2232U, 0x27bdffd0U},
    {2240U, 0x7fb00020U}, {2244U, 0x0000282dU}, {2248U, 0x7fb10010U}, {2252U, 0x0080802dU},
    {2260U, 0xffbf0000U}, {2368U, 0x27bdfe90U}, {2372U, 0x7fb10150U}, {2376U, 0x7fb00160U},
}};
constexpr std::array<GuestAddress, 3> kRetailUpdateReturns{0x0026bea0U, 0x0026bef8U, 0x0041bf60U};
constexpr std::array<GuestAddress, 3> kRetailStopReturns{0x0026bebcU, 0x0026bf14U, 0x0041bf70U};

[[nodiscard]] bool word_matches(const EeMemory& memory, const WordSignature& signature) {
    return memory.contains(signature.address, 4U) &&
           memory.read_u32_le(signature.address) == signature.word;
}

[[nodiscard]] bool is_known_return(const std::array<GuestAddress, 3>& returns, const GuestAddress address) {
    return std::ranges::find(returns, address) != returns.end();
}

} // namespace

bool DirtyDnasCompatHle::matches_retail_revision(const EeMemory& memory) const {
    if (!std::ranges::all_of(kRetailSignatures,
                     [&memory](const WordSignature& signature) {
                         return word_matches(memory, signature);
                     })) {
        return false;
    }

    std::array<GuestAddress, 4> targets{};
    for (std::size_t index = 0; index < kTrampolines.size(); ++index) {
        const auto trampoline = kTrampolines[index];
        if (!memory.contains(trampoline, 4U)) {
            return false;
        }
        const auto instruction = memory.read_u32_le(trampoline);
        if ((instruction >> 26U) != 2U) {
            return false;
        }
        targets[index] = ((trampoline + 4U) & 0xf0000000U) | ((instruction << 2U) & 0x0ffffffcU);
    }
    if (targets[0] < kExportOffsets[0]) {
        return false;
    }
    const auto text_base = targets[0] - kExportOffsets[0];
    for (std::size_t index = 0; index < targets.size(); ++index) {
        if (targets[index] != text_base + kExportOffsets[index]) {
            return false;
        }
    }
    for (const auto& signature : kModuleSignatures) {
        const WordSignature relocated{text_base + signature.address, signature.word};
        if (!word_matches(memory, relocated)) {
            return false;
        }
    }
    return true;
}

DirtyDnasCompatCall DirtyDnasCompatHle::try_handle(R5900State& state, EeMemory& memory) {
    const auto pc = state.pc();
    if (pc != kStartTrampoline && pc != kUpdateTrampoline && pc != kStopTrampoline) {
        return DirtyDnasCompatCall::NotHandled;
    }
    if (!matches_retail_revision(memory)) {
        return DirtyDnasCompatCall::NotHandled;
    }

    const auto return_address = state.read_gpr_u32(31U);

    if (pc == kStartTrampoline) {
        const auto result_address = state.read_gpr_u32(4U);
        const auto mode = state.read_gpr_u32(5U);
        if (return_address != 0x0026c170U && return_address != 0x0041bf4cU) {
            return DirtyDnasCompatCall::NotHandled;
        }
        if (mode != 1U && mode != 0x01050201U) {
            return DirtyDnasCompatCall::NotHandled;
        }
        if (!memory.contains(result_address, 160U)) {
            return DirtyDnasCompatCall::NotHandled;
        }
        started_ = true;
        ++activity_.starts;
        const auto resume = state.read_gpr_u32(31U);
        state.write_gpr_sign_extended_32(2U, 0U);
        state.set_pc(resume);
        return DirtyDnasCompatCall::Start;
    }

    if (!started_) {
        return DirtyDnasCompatCall::NotHandled;
    }

    if (pc == kUpdateTrampoline) {
        if (!is_known_return(kRetailUpdateReturns, return_address)) {
            return DirtyDnasCompatCall::NotHandled;
        }
        if (state.read_gpr_u32(4U) > 1U) {
            return DirtyDnasCompatCall::NotHandled;
        }
        ++activity_.updates;
        const auto resume = state.read_gpr_u32(31U);
        state.write_gpr_sign_extended_32(2U, 0U);
        state.set_pc(resume);
        return DirtyDnasCompatCall::Update;
    }

    const auto result_address = state.read_gpr_u32(4U);
    if (!is_known_return(kRetailStopReturns, return_address)) {
        return DirtyDnasCompatCall::NotHandled;
    }
    if (!memory.contains(result_address, kResultSize)) {
        return DirtyDnasCompatCall::NotHandled;
    }
    std::ranges::fill(memory.view(result_address, kResultSize), std::byte{});
    started_ = false;
    ++activity_.stops;
    const auto resume = state.read_gpr_u32(31U);
    state.write_gpr_sign_extended_32(2U, 1U);
    state.set_pc(resume);
    return DirtyDnasCompatCall::Stop;
}

const DirtyDnasCompatActivity& DirtyDnasCompatHle::activity() const noexcept {
    return activity_;
}

} // namespace takedown::guest
