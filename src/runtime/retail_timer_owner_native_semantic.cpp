#include "takedown/runtime/retail_timer_owner_native_semantic.hpp"

#include "takedown/runtime/retail_timer_owner_abi_trace.hpp"
#include "takedown/guest/ee_cop1_arithmetic.hpp"

#include <cstdint>
#include <string_view>

namespace takedown::runtime {

std::string_view to_string(const RetailTimerOwnerNativeSemanticFailure failure) noexcept {
    switch (failure) {
    case RetailTimerOwnerNativeSemanticFailure::None:
        return "none";
    case RetailTimerOwnerNativeSemanticFailure::Disabled:
        return "disabled";
    case RetailTimerOwnerNativeSemanticFailure::WrongEntryAddress:
        return "wrong_entry_address";
    case RetailTimerOwnerNativeSemanticFailure::EntryStateMismatch:
        return "entry_state_mismatch";
    case RetailTimerOwnerNativeSemanticFailure::CodeRangeInvalid:
        return "code_range_invalid";
    case RetailTimerOwnerNativeSemanticFailure::InstructionWordMismatch:
        return "instruction_word_mismatch";
    case RetailTimerOwnerNativeSemanticFailure::DomainFingerprintMismatch:
        return "domain_fingerprint_mismatch";
    case RetailTimerOwnerNativeSemanticFailure::RawFingerprintMismatch:
        return "raw_fingerprint_mismatch";
    case RetailTimerOwnerNativeSemanticFailure::MemoryRangeInvalid:
        return "memory_range_invalid";
    case RetailTimerOwnerNativeSemanticFailure::PathPredicateMismatch:
        return "path_predicate_mismatch";
    case RetailTimerOwnerNativeSemanticFailure::PreflightRejected:
        return "preflight_rejected";
    }
    return "unknown";
}

RetailTimerOwnerNativeSemanticAdmission RetailTimerOwnerNativeSemantic::admit(
    const NativeFunctionPrepareContext& context,
    const NativeFunctionPreflightState& preflight) const noexcept {
    if (!enabled_) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::Disabled};
    }
    if (context.entry_address != 0x001d3a60U) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::WrongEntryAddress};
    }
    const auto& cpu = context.cpu;
    if (cpu.pc() != 0x001d3a60U || cpu.next_pc() != 0x001d3a64U || cpu.is_executing_delay_slot()) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::EntryStateMismatch};
    }
    if (!context.memory.is_main_ram_range(0x001d3a60U, 428U)) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::CodeRangeInvalid};
    }
    if (retail_timer_owner_live_function_fingerprint(context.memory) !=
        5002102151770485814ULL) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::DomainFingerprintMismatch};
    }

    // FNV-1a over the 428 code bytes; zero when the code is not in main RAM.
    std::uint64_t hash = 0U;
    if (context.memory.is_main_ram_range(0x001d3a60U, 428U)) {
        hash = 14695981039346656037ULL;
        for (std::uint32_t index = 0U; index != 428U; ++index) {
            const std::uint8_t byte = context.memory.read_u8(0x001d3a60U + index);
            hash = (hash ^ byte) * 1099511628211ULL;
        }
    }
    if (hash != 496009247747727067ULL) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::RawFingerprintMismatch};
    }

    for (const auto& path_word : kRetailTimerOwnerNativeSemanticPath) {
        if (context.memory.read_u32_le(path_word.pc) != path_word.word) {
            return RetailTimerOwnerNativeSemanticAdmission{
                .failure = RetailTimerOwnerNativeSemanticFailure::InstructionWordMismatch};
        }
    }

    const std::uint32_t gp = cpu.read_gpr(28).words[0];
    const std::uint32_t a0 = cpu.read_gpr(4).words[0];
    const std::uint32_t tick = gp - 24372U;
    const bool ok = (a0 & 3U) == 0U && (tick & 3U) == 0U && (static_cast<std::uint64_t>(a0) + 64U) <= 33554432ULL &&
                    context.memory.is_main_ram_range(a0, 64U) &&
                    (static_cast<std::uint64_t>(tick) + 8U) <= 33554432ULL &&
                    context.memory.is_main_ram_range(tick, 8U) &&
                    !context.memory.main_ram_ranges_overlap(a0, 64U, tick, 8U) &&
                    !context.memory.main_ram_ranges_overlap(a0, 64U, 0x001d3a60U, 428U);
    if (!ok) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::MemoryRangeInvalid};
    }

    const std::uint32_t state_word = context.memory.read_u32_le(a0);
    const std::uint32_t tick_value = context.memory.read_u32_le(tick);
    const std::uint32_t word36 = context.memory.read_u32_le(a0 + 36U);
    const std::uint32_t word24 = context.memory.read_u32_le(a0 + 24U);
    const std::uint32_t word20 = context.memory.read_u32_le(a0 + 20U);
    const std::uint32_t word4 = context.memory.read_u32_le(a0 + 4U);
    const std::uint32_t delta = tick_value - word20 - word4;
    if (state_word == tick_value || word36 != word24 ||
        static_cast<std::int32_t>(delta) <= 0 || context.memory.read_u8(a0 + 40U) == 0U ||
        word36 != 1U) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::PathPredicateMismatch};
    }

    const NativeFunctionTraits traits{.scheduler_state_transparent = false,
                                      .single_step_host_call = false,
                                      .defers_dmac_completions = false};
    if (check_native_function_preflight(kRetailTimerOwnerNativeSemanticCost, traits, preflight) !=
        NativeFunctionPreflightStatus::Ready) {
        return RetailTimerOwnerNativeSemanticAdmission{
            .failure = RetailTimerOwnerNativeSemanticFailure::PreflightRejected};
    }
    return RetailTimerOwnerNativeSemanticAdmission{
        .failure = RetailTimerOwnerNativeSemanticFailure::None,
        .execution = {43U, 43U},
        .return_pc = cpu.read_gpr(31).words[0],
    };
}

RetailTimerOwnerNativeSemanticResult RetailTimerOwnerNativeSemantic::apply(
    NativeFunctionContext& context,
    const NativeFunctionPreflightState& preflight) const noexcept {
    const auto admission = admit(
        NativeFunctionPrepareContext{context.cpu, context.memory, context.entry_address}, preflight);
    if (admission.failure != RetailTimerOwnerNativeSemanticFailure::None) {
        return RetailTimerOwnerNativeSemanticResult{.admission = admission, .applied = false};
    }

    const auto& cpu = context.cpu;
    const std::uint32_t a0 = cpu.read_gpr(4).words[0];
    const std::uint32_t gp = cpu.read_gpr(28).words[0];
    if (gp <= 24371U) {
        return RetailTimerOwnerNativeSemanticResult{
            .admission = RetailTimerOwnerNativeSemanticAdmission{
                .failure = RetailTimerOwnerNativeSemanticFailure::MemoryRangeInvalid},
            .applied = false};
    }
    const std::uint32_t tick_address = gp - 24372U;
    const std::uint32_t tick = context.memory.read_u32_le(tick_address);
    const std::uint32_t w4 = context.memory.read_u32_le(a0 + 4U);
    const std::uint32_t w12 = context.memory.read_u32_le(a0 + 12U);
    const std::uint32_t w16 = context.memory.read_u32_le(a0 + 16U);
    const std::uint32_t w20 = context.memory.read_u32_le(a0 + 20U);
    const std::uint32_t w24 = context.memory.read_u32_le(a0 + 24U);
    const std::uint32_t tick_scale = context.memory.read_u32_le(tick_address + 4U);
    const std::uint32_t w36 = context.memory.read_u32_le(a0 + 36U);
    const std::uint8_t byte40 = context.memory.read_u8(a0 + 40U);

    const std::uint32_t delta = (tick - w20) - w4;
    const std::uint32_t next12 = w12 + delta;

    // The residue is built on a copy of the live state and committed at the end.
    guest::R5900State output = cpu;
    // Integer path, in instruction order (sign-extended 32-bit results).
    output.write_gpr_sign_extended_32(5, w36);        // lw a1,0x24(a0)
    output.write_gpr_sign_extended_32(6, w24);        // lw a2,0x18(a0)
    output.write_gpr_sign_extended_32(3, tick);       // lw v1,-0x5f34(gp)
    output.write_gpr_sign_extended_32(6, tick);       // lw a2,0(a0) after sw v1,0(a0)
    output.write_gpr_sign_extended_32(5, w4);         // lw a1,4(a0)
    output.write_gpr_sign_extended_32(3, tick - w20); // subu v1,a2,v1
    output.write_gpr_sign_extended_32(7, delta);      // subu a3,v1,a1
    output.write_gpr_u64(5, byte40);                  // lbu a1,0x28(a0)
    output.write_gpr_sign_extended_32(6, w24);        // lw a2,0x18(a0)
    output.write_gpr_u64(5, 1U);                      // addiu a1,zero,1
    output.write_gpr_sign_extended_32(5, next12);     // addu a1,a1,a3 (after lw a1,0xc(a0))

    // Single-precision path through the reviewed COP1 helpers.
    output.write_fpr_bits(1, w24);       // lwc1 f1,0x18(a0)
    output.write_fpr_bits(0, w16);       // lwc1 f0,0x10(a0)
    output.write_fpr_bits(2, next12);    // lwc1 f2,0x0c(a0) (after sw a1,0xc(a0))
    output.write_fpr_bits(3, tick_scale); // lwc1 f3,-0x5f30(gp)
    guest::ee_cop1::cvt_s_w(output, 1U, 1U);
    guest::ee_cop1::div_s(output, 1U, 3U, 1U);
    guest::ee_cop1::cvt_s_w(output, 0U, 0U);
    guest::ee_cop1::cvt_s_w(output, 0U, 2U);
    guest::ee_cop1::mula_s(output, 0U, 1U);
    guest::ee_cop1::madd_s(output, 0U, 2U, 3U);

    // The four stores of the reviewed path.
    context.memory.write_u32_le(a0, tick);
    context.memory.write_u32_le(a0 + 12U, next12);
    context.memory.write_u32_le(a0 + 4U, tick - w20);
    context.memory.write_u32_le(a0 + 32U, output.read_fpr_bits(0));

    // Return residue: jr ra with its delay-slot nop.
    output.set_pc(output.read_gpr(31).words[0]);
    context.cpu = output;
    return RetailTimerOwnerNativeSemanticResult{
        .admission = admission,
        .applied = true,
    };
}

} // namespace takedown::runtime
