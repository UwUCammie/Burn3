#include "takedown/recompiler/r5900_leaf_module_builder.hpp"

#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/recompiler/r5900_cpp_lifter.hpp"
#include "takedown/recompiler/r5900_function_cfg.hpp"
#include "takedown/recompiler/r5900_word_extractor.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace takedown::recompiler {

namespace {

struct Candidate {
    guest::GuestAddress address{};
    std::string suggested_name;
};

struct AcceptedFunction {
    guest::GuestAddress address{};
    std::string generated_name;
    std::vector<std::uint32_t> words;
    std::string implementation_source;
};

struct ProvenExtent {
    std::size_t instruction_count{};
};

[[nodiscard]] std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

[[nodiscard]] std::string bare_hex32(const std::uint32_t value) {
    return hex32(value).substr(2);
}

// Every direct JAL target that is also a function candidate, keyed and sorted
// by guest address. When several candidates share an address the
// lexicographically smallest non-empty name wins.
[[nodiscard]] std::vector<Candidate> collect_direct_call_candidates(const content::ElfStaticAnalysis& analysis) {
    std::set<std::uint32_t> direct_targets;
    for (const auto& call : analysis.calls) {
        direct_targets.insert(call.target);
    }
    std::map<std::uint32_t, std::string> candidates;
    for (const auto& function : analysis.functions) {
        if (!function.is_direct_call_target || !direct_targets.contains(function.address)) {
            continue;
        }
        auto [it, inserted] = candidates.try_emplace(function.address, function.suggested_name);
        if (!inserted && !function.suggested_name.empty() &&
            (it->second.empty() || function.suggested_name < it->second)) {
            it->second = function.suggested_name;
        }
    }
    std::vector<Candidate> result;
    result.reserve(candidates.size());
    for (auto& [address, name] : candidates) {
        result.push_back(Candidate{address, std::move(name)});
    }
    return result;
}

std::string cfg_blocker_text(const R5900CfgBlockerKind kind) {
    switch (kind) {
    case R5900CfgBlockerKind::InvalidExecutableSpan:
        return "invalid executable span";
    case R5900CfgBlockerKind::MisalignedEntry:
        return "misaligned entry";
    case R5900CfgBlockerKind::EntryOutsideExecutable:
        return "entry outside executable span";
    case R5900CfgBlockerKind::InstructionLimitReached:
        return "instruction limit reached";
    case R5900CfgBlockerKind::UnreadableInstruction:
        return "unreadable instruction";
    case R5900CfgBlockerKind::UnknownInstruction:
        return "unknown instruction";
    case R5900CfgBlockerKind::MissingDelaySlot:
        return "missing delay slot";
    case R5900CfgBlockerKind::ControlFlowInDelaySlot:
        return "control flow in delay slot";
    case R5900CfgBlockerKind::TargetOutsideExecutable:
        return "target outside bounded executable span";
    case R5900CfgBlockerKind::AddressOverflow:
        return "guest address overflow";
    case R5900CfgBlockerKind::IndirectJumpTarget:
        return "indirect jump target";
    case R5900CfgBlockerKind::IndirectCallTarget:
        return "indirect call target";
    case R5900CfgBlockerKind::ExceptionControlFlow:
        return "exception control flow";
    }
    return "unknown CFG blocker";
}

// Proves that the CFG is exactly one complete block ending in JR $ra with an
// always-executed delay slot, and that its instructions form one contiguous,
// address-ordered extent starting at the entry.
[[nodiscard]] std::optional<ProvenExtent> prove_single_return_leaf(const R5900FunctionCfg& cfg, std::string& detail,
                                                                   R5900LeafCandidateReason& reason) {
    if (!cfg.blockers.empty()) {
        const auto& blocker = cfg.blockers.front();
        reason = R5900LeafCandidateReason::CfgBlocked;
        detail = cfg_blocker_text(blocker.kind) + " at " + hex32(blocker.address);
        return std::nullopt;
    }
    if (cfg.blocks.size() != 1U) {
        reason = R5900LeafCandidateReason::NotSingleCompleteReturnLeaf;
        detail = "the reachable CFG does not contain exactly one basic block";
        return std::nullopt;
    }
    const auto& block = cfg.blocks.front();
    if (block.entry_address != cfg.entry_address || block.instruction_addresses.empty() ||
        !block.delay_slot_address || block.delay_slot_policy != R5900BlockDelaySlotPolicy::Always ||
        block.outgoing_edges.size() != 1U || block.outgoing_edges.front().kind != R5900CfgEdgeKind::Return ||
        block.outgoing_edges.front().target_address ||
        block.outgoing_edges.front().delay_slot != R5900DelaySlotDisposition::Execute) {
        reason = R5900LeafCandidateReason::NotSingleCompleteReturnLeaf;
        detail = "the only block is not a complete JR-return plus always-executed delay slot";
        return std::nullopt;
    }
    const auto return_address = block.instruction_addresses.back();
    if (return_address > 0xfffffffbU || *block.delay_slot_address != return_address + 4U ||
        block.outgoing_edges.front().source_address != return_address ||
        cfg.instructions.size() != block.instruction_addresses.size() + 1U) {
        reason = R5900LeafCandidateReason::NonContiguousLeafExtent;
        detail = "the CFG return and delay-slot addresses do not form one contiguous extent";
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < cfg.instructions.size(); ++index) {
        const auto expected64 = static_cast<std::uint64_t>(cfg.entry_address) + index * 4U;
        if (expected64 > 0xffffffffU ||
            cfg.instructions[index].decoded.pc != static_cast<guest::GuestAddress>(expected64)) {
            reason = R5900LeafCandidateReason::NonContiguousLeafExtent;
            detail = "the reachable instructions are not a contiguous address-ordered leaf";
            return std::nullopt;
        }
        const auto expected = static_cast<guest::GuestAddress>(expected64);
        if (index + 1U < cfg.instructions.size()) {
            if (block.instruction_addresses[index] != expected) {
                reason = R5900LeafCandidateReason::NonContiguousLeafExtent;
                detail = "a non-delay instruction is not owned by the single leaf block";
                return std::nullopt;
            }
        } else if (!cfg.instructions[index].referenced_as_delay_slot || *block.delay_slot_address != expected) {
            reason = R5900LeafCandidateReason::NonContiguousLeafExtent;
            detail = "the final contiguous word is not the proven return delay slot";
            return std::nullopt;
        }
    }
    return ProvenExtent{.instruction_count = cfg.instructions.size()};
}

// The largest instruction count a candidate can have inside file-backed
// executable PT_LOAD data, capped by the caller's per-function limit.
std::optional<std::size_t> file_backed_instruction_capacity(const content::Elf32LoadPlan& load_plan,
                                                            const guest::GuestAddress address,
                                                            const std::size_t maximum_instruction_count) {
    std::uint64_t greatest_remaining_bytes = 0U;
    for (const auto& segment : load_plan.segments) {
        if (!segment.executable() || segment.file_size == 0U) {
            continue;
        }
        const auto begin = static_cast<std::uint64_t>(segment.virtual_address);
        const auto end = begin + segment.file_size;
        const auto candidate = static_cast<std::uint64_t>(address);
        if (begin > candidate || end <= candidate) {
            continue;
        }
        greatest_remaining_bytes = std::max(greatest_remaining_bytes, end - candidate);
    }
    const auto available_words = greatest_remaining_bytes / 4U;
    if (available_words == 0U) {
        return std::nullopt;
    }
    return std::min<std::size_t>(available_words, maximum_instruction_count);
}

bool is_pure_scalar_kind(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Nop:
    case guest::R5900InstructionKind::Sll:
    case guest::R5900InstructionKind::Srl:
    case guest::R5900InstructionKind::Movn:
    case guest::R5900InstructionKind::Addu:
    case guest::R5900InstructionKind::Daddu:
    case guest::R5900InstructionKind::Or:
    case guest::R5900InstructionKind::Slt:
    case guest::R5900InstructionKind::Addiu:
    case guest::R5900InstructionKind::Andi:
    case guest::R5900InstructionKind::Lui:
        return true;
    default:
        return false;
    }
}

// A scheduler-transparent pure leaf touches only scalar GPRs: every word is
// from the pure grammar except one JR $ra immediately before the final word.
bool is_scheduler_transparent_pure_leaf(const guest::GuestAddress address, const std::span<const std::uint32_t> words) {
    if (words.size() < 2U || words.size() > 16U) {
        return false;
    }
    std::vector<guest::DecodedR5900Instruction> decoded;
    decoded.reserve(words.size());
    for (std::size_t index = 0U; index < words.size(); ++index) {
        const auto pc = address + static_cast<guest::GuestAddress>(index) * 4U;
        decoded.push_back(guest::decode_r5900(pc, words[index]));
    }
    const auto return_index = words.size() - 2U;
    if (decoded[return_index].kind != guest::R5900InstructionKind::Jr || decoded[return_index].rs != 31U ||
        decoded[return_index].control_flow != guest::R5900ControlFlow::Return ||
        !decoded[return_index].has_delay_slot) {
        return false;
    }
    for (std::size_t index = 0U; index < decoded.size(); ++index) {
        if (index == return_index) {
            continue;
        }
        if (decoded[index].control_flow != guest::R5900ControlFlow::None || !is_pure_scalar_kind(decoded[index].kind)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string compose_module_source(const std::vector<AcceptedFunction>& functions) {
    std::ostringstream output;
    output << "// Generated by build_r5900_leaf_module; do not edit by hand.\n"
           << "#include \"takedown/guest/ee_memory.hpp\"\n"
           << "#include \"takedown/runtime/native_function_dispatcher.hpp\"\n\n"
           << "#include <array>\n"
           << "#include <cstddef>\n"
           << "#include <cstdint>\n"
           << "#include <stdexcept>\n"
           << "#include <string>\n\n";
    for (const auto& function : functions) {
        output << function.implementation_source << '\n';
    }
    output << "namespace takedown::recompiler::generated {\n\n";
    if (!functions.empty()) {
        output << "namespace {\n\n"
               << "template <std::size_t Size>\n"
               << "bool has_exact_signature(\n"
               << "    const takedown::guest::EeMemory& memory,\n"
               << "    const takedown::guest::GuestAddress address,\n"
               << "    const std::array<std::uint32_t, Size>& words) {\n"
               << "    if (!memory.contains(address, Size * sizeof(std::uint32_t))) {\n"
               << "        return false;\n"
               << "    }\n"
               << "    for (std::size_t index = 0U; index < Size; ++index) {\n"
               << "        const auto pc = address +\n"
               << "            static_cast<takedown::guest::GuestAddress>(index * 4U);\n"
               << "        if (memory.read_u32_le(pc) != words[index]) {\n"
               << "            return false;\n"
               << "        }\n"
               << "    }\n"
               << "    return true;\n"
               << "}\n\n";
        for (const auto& function : functions) {
            output << "constexpr std::array<std::uint32_t, " << std::dec << function.words.size() << "U> kSignature_"
                   << bare_hex32(function.address) << "{\n";
            for (const auto word : function.words) {
                output << "    " << hex32(word) << "U,\n";
            }
            output << "};\n\n";
        }
        output << "void require_registration_or_promoted_override(\n"
               << "    const takedown::runtime::NativeFunctionRegistrationResult& result,\n"
               << "    const char* name) {\n"
               << "    if (result.registered() ||\n"
               << "        result.status == takedown::runtime::\n"
               << "            NativeFunctionRegistrationStatus::DuplicateAddress) {\n"
               << "        return;\n"
               << "    }\n"
               << "    throw std::logic_error(\n"
               << "        std::string{\"could not register generated R5900 leaf: \"} + name);\n"
               << "}\n\n"
               << "} // namespace\n\n";
    }
    output << "bool register_generated_r5900_leaf_module(\n"
           << "    takedown::runtime::NativeFunctionDispatcher& dispatcher,\n"
           << "    const takedown::guest::EeMemory& memory) {\n";
    if (!functions.empty()) {
        output << "    if (";
        for (std::size_t index = 0U; index < functions.size(); ++index) {
            const auto& function = functions[index];
            if (index != 0U) {
                output << "\n        ";
            }
            output << "!has_exact_signature(memory, " << hex32(function.address) << "U, kSignature_" << bare_hex32(function.address) << ")";
            if (index + 1U != functions.size()) {
                output << " ||";
            }
        }
        output << ") {\n"
               << "        return false;\n"
               << "    }\n\n";
        for (const auto& function : functions) {
            const auto instruction_count = function.words.size();
            output << "    require_registration_or_promoted_override(\n"
                   << "        dispatcher.register_function(\n"
                   << "            " << hex32(function.address) << "U, \"" << function.generated_name << "\", {" << std::dec << instruction_count
                   << "U, " << instruction_count << "U},\n"
                   << "            {.scheduler_state_transparent = true}, " << function.generated_name << "),\n"
                   << "        \"" << function.generated_name << "\");\n";
        }
    }
    output << "    return true;\n"
           << "}\n\n"
           << "} // namespace takedown::recompiler::generated\n";
    return output.str();
}

} // namespace

std::size_t R5900LeafModuleBuildResult::accepted_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        inventory.begin(), inventory.end(), [](const R5900LeafCandidateInventoryEntry& entry) {
            return entry.disposition == R5900LeafCandidateDisposition::Accepted;
        }));
}

std::size_t R5900LeafModuleBuildResult::refused_count() const noexcept {
    return inventory.size() - accepted_count();
}

R5900LeafModuleBuildResult build_r5900_leaf_module(const guest::EeMemory& memory,
                                                   const content::Elf32LoadPlan& load_plan,
                                                   const content::ElfStaticAnalysis& static_analysis,
                                                   const R5900LeafModuleBuildLimits& limits) {
    R5900LeafModuleBuildResult result;
    std::vector<AcceptedFunction> accepted;
    const auto candidates = collect_direct_call_candidates(static_analysis);
    result.inventory.reserve(candidates.size());
    for (std::size_t candidate_index = 0U; candidate_index < candidates.size(); ++candidate_index) {
        const auto& candidate = candidates[candidate_index];
        R5900LeafCandidateInventoryEntry inventory{
            .address = candidate.address,
            .suggested_name = candidate.suggested_name,
        };
        const auto refuse = [&inventory](const R5900LeafCandidateReason reason, std::string detail) {
            inventory.disposition = R5900LeafCandidateDisposition::Refused;
            inventory.reason = reason;
            inventory.detail = std::move(detail);
        };

        if (candidate_index >= limits.maximum_direct_call_candidates) {
            refuse(R5900LeafCandidateReason::CandidateLimitExceeded,
                   "the candidate lies beyond the caller's direct-call candidate limit");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }
        if (limits.maximum_instructions_per_function == 0U) {
            refuse(R5900LeafCandidateReason::PerFunctionInstructionLimitIsZero,
                   "the caller supplied a zero per-function instruction limit");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }
        if (accepted.size() >= limits.maximum_generated_functions) {
            refuse(R5900LeafCandidateReason::GeneratedFunctionLimitExceeded,
                   "the generated-function limit was reached before this candidate");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }
        const auto capacity =
            file_backed_instruction_capacity(load_plan, candidate.address, limits.maximum_instructions_per_function);
        if (!capacity) {
            refuse(R5900LeafCandidateReason::NotInFileBackedExecutableData,
                   "the candidate entry is not the start of a complete word in file-backed executable PT_LOAD data");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }

        const auto extracted = extract_elf_r5900_words(memory, load_plan,
                                                       R5900WordExtractionRequest{
                                                           .start_address = candidate.address,
                                                           .instruction_count = *capacity,
                                                           .maximum_instruction_count =
                                                               limits.maximum_instructions_per_function,
                                                       });
        if (!extracted.succeeded()) {
            const std::string detail = extracted.diagnostics.empty()
                                           ? std::string{"bounded ELF word extraction failed"}
                                           : extracted.diagnostics.front().message;
            refuse(R5900LeafCandidateReason::ExtractionFailed, detail);
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }

        const auto cfg = analyze_r5900_function_cfg(candidate.address, extracted.as_executable_word_span(),
                                                    limits.maximum_instructions_per_function);
        auto proof_reason = R5900LeafCandidateReason::NotSingleCompleteReturnLeaf;
        std::string proof_detail;
        const auto extent = prove_single_return_leaf(cfg, proof_detail, proof_reason);
        if (!extent) {
            refuse(proof_reason, std::move(proof_detail));
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }
        if (extent->instruction_count > extracted.words.size()) {
            refuse(R5900LeafCandidateReason::NonContiguousLeafExtent,
                   "the CFG extent exceeds the explicitly extracted word range");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }
        const auto leaf_words = std::span<const std::uint32_t>{extracted.words}.first(extent->instruction_count);
        if (!is_scheduler_transparent_pure_leaf(candidate.address, leaf_words)) {
            refuse(R5900LeafCandidateReason::NotSchedulerTransparentPureLeaf,
                   "the proven leaf is outside the bounded pure scalar grammar: LUI, ADDIU, NOP, DADDU, SLL, SRL, ANDI, "
                   "OR, ADDU, SLT, and MOVN around one final JR $ra");
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }

        auto lifted = lift_r5900_leaf_function_to_cpp(candidate.address, leaf_words, candidate.suggested_name);
        if (!lifted.succeeded()) {
            const std::string detail = lifted.diagnostics.empty()
                                           ? std::string{"the deterministic leaf lifter refused the extent"}
                                           : lifted.diagnostics.front().message;
            refuse(R5900LeafCandidateReason::LifterRejected, detail);
            result.inventory.emplace_back(std::move(inventory));
            continue;
        }

        inventory.disposition = R5900LeafCandidateDisposition::Accepted;
        inventory.reason = R5900LeafCandidateReason::Accepted;
        inventory.instruction_count = extent->instruction_count;
        inventory.generated_name = lifted.generated_name;
        inventory.detail = "single complete pure return leaf proven by bounded CFG";
        accepted.push_back(AcceptedFunction{
            .address = candidate.address,
            .generated_name = lifted.generated_name,
            .words = {leaf_words.begin(), leaf_words.end()},
            .implementation_source = std::move(lifted.cpp_source),
        });
        result.inventory.emplace_back(std::move(inventory));
    }
    result.cpp_source = compose_module_source(accepted);
    return result;
}

} // namespace takedown::recompiler
