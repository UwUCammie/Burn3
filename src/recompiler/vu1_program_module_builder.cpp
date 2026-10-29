#include "takedown/recompiler/vu1_program_module_builder.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace takedown::recompiler {

namespace {

using guest::kVu1MicroProgramPairCount;
using guest::Vu1ProfiledProgramSnapshot;
using guest::vu1_detail::CachedVu1Pair;

struct ObservedBasicBlock {
    std::uint16_t start_pc;
    std::vector<CachedVu1Pair> pairs;
    std::uint64_t committed_pairs;
};

struct GeneratedPairRecipe {
    std::string_view upper_stage{};
    std::string_view lower_kind{};
};

struct GeneratedSpan {
    std::uint16_t start_pc{};
    std::vector<GeneratedPairRecipe> recipes{};
};

struct RankedSnapshot {
    const Vu1ProfiledProgramSnapshot* snapshot{};
};

// Profile weights saturate instead of wrapping so a hot block never sorts below a cold one.
[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t lhs,
                                           const std::uint64_t rhs) noexcept {
    return rhs > ~std::uint64_t{0U} - lhs ? ~std::uint64_t{0U} : lhs + rhs;
}

// Exact committed-PC histogram decides which natural blocks are worth emitting:
// every executed run of pairs inside a static leader boundary becomes a block.
//
// RECOVERY: GIMPLE matches the original except for SSA version numbering (one extra
// SSA name shifts later `name_N` suffixes); no source-level cause was found.
[[nodiscard]] std::vector<ObservedBasicBlock>
find_observed_basic_blocks(const Vu1ProfiledProgramSnapshot& snapshot) {
    std::array<CachedVu1Pair, kVu1MicroProgramPairCount> decoded{};
    std::bitset<kVu1MicroProgramPairCount> boundaries;
    boundaries.set(0U);
    boundaries.set(snapshot.profile.entry);
    boundaries.set(snapshot.profile.resolved_entry);
    for (std::size_t pc = 0U; pc != kVu1MicroProgramPairCount; ++pc) {
        CachedVu1Pair& pair = decoded[pc];
        pair = guest::vu1_detail::decode_vu1_pair_metadata(
            std::span<const std::byte>{snapshot.micro_memory}, static_cast<std::uint16_t>(pc));
        if (pair.flow == guest::vu1_detail::CachedVu1Flow::Fallthrough &&
            !pair.exits_after_delay) {
            continue;
        }
        boundaries.set((pc + 1U) & (kVu1MicroProgramPairCount - 1U));
        boundaries.set((pc + 2U) & (kVu1MicroProgramPairCount - 1U));
        if (pair.flow == guest::vu1_detail::CachedVu1Flow::DirectBranch ||
            pair.flow == guest::vu1_detail::CachedVu1Flow::ConditionalBranch) {
            boundaries.set(pair.direct_branch_target);
        }
    }

    std::vector<std::size_t> leaders;
    leaders.reserve(boundaries.count());
    for (std::size_t pc = 0U; pc <= kVu1MicroProgramPairCount - 1U; ++pc) {
        if (boundaries.test(pc)) {
            leaders.push_back(pc);
        }
    }

    std::vector<ObservedBasicBlock> blocks;
    blocks.reserve(leaders.size());
    for (std::size_t leader_index = 0U; leader_index < leaders.size(); ++leader_index) {
        const auto start = leaders[leader_index];
        const auto end = leader_index + 1U < leaders.size() ? leaders[leader_index + 1U]
                                                              : kVu1MicroProgramPairCount;
        auto pc = start;
        while (pc < end) {
            while (pc < end && snapshot.pair_counts[pc] == 0U) {
                ++pc;
            }
            if (pc == end) {
                break;
            }
            const auto first = pc;
            auto committed_pairs = std::uint64_t{0U};
            while (pc < end && snapshot.pair_counts[pc] != 0U) {
                committed_pairs = saturating_add(committed_pairs, snapshot.pair_counts[pc]);
                ++pc;
            }
            ObservedBasicBlock block;
            block.start_pc = static_cast<std::uint16_t>(first);
            block.committed_pairs = committed_pairs;
            block.pairs.reserve(pc - first);
            block.pairs.insert(block.pairs.end(), decoded.begin() + first, decoded.begin() + pc);
            blocks.push_back(std::move(block));
        }
    }

    std::sort(blocks.begin(), blocks.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.committed_pairs != rhs.committed_pairs) {
            return lhs.committed_pairs > rhs.committed_pairs;
        }
        if (lhs.pairs.size() != rhs.pairs.size()) {
            return lhs.pairs.size() > rhs.pairs.size();
        }
        return lhs.start_pc < rhs.start_pc;
    });
    return blocks;
}

[[nodiscard]] std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] std::string hex16(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(4)
           << static_cast<unsigned int>(value);
    return output.str();
}

[[nodiscard]] std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

[[nodiscard]] std::string static_block_symbol(const std::size_t program_index,
                                              const std::uint16_t start_pc) {
    return "kStaticBlock_" + std::to_string(program_index) + "_" + std::to_string(start_pc);
}

[[nodiscard]] std::string static_upper_stages_symbol(const std::size_t program_index,
                                                     const std::uint16_t start_pc) {
    return "kStaticUpperStages_" + std::to_string(program_index) + "_" +
           std::to_string(start_pc);
}

// Fully qualified runtime thunk for an upper-slot opcode, or "nullptr" when the opcode
// has no per-pair stage and the generated descriptor must fall back to lazy decode.
[[nodiscard]] std::string_view upper_stage_symbol(const CachedVu1Pair& pair) {
    if (!pair.upper.has_value()) {
        return "nullptr";
    }
    switch (pair.upper->op) {
    case guest::vu1_detail::UpperOp::MaddLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_madd_lane";
    case guest::vu1_detail::UpperOp::MaddaLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_madda_lane";
    case guest::vu1_detail::UpperOp::AddLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_add_lane";
    case guest::vu1_detail::UpperOp::SubLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_sub_lane";
    case guest::vu1_detail::UpperOp::MsubLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_msub_lane";
    case guest::vu1_detail::UpperOp::MulaLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_mula_lane";
    case guest::vu1_detail::UpperOp::Mul:
        return "takedown::guest::vu1_detail::stage_vu1_upper_mul";
    case guest::vu1_detail::UpperOp::Add:
        return "takedown::guest::vu1_detail::stage_vu1_upper_add";
    case guest::vu1_detail::UpperOp::Sub:
        return "takedown::guest::vu1_detail::stage_vu1_upper_sub";
    case guest::vu1_detail::UpperOp::Madd:
        return "takedown::guest::vu1_detail::stage_vu1_upper_madd";
    case guest::vu1_detail::UpperOp::Msub:
        return "takedown::guest::vu1_detail::stage_vu1_upper_msub";
    case guest::vu1_detail::UpperOp::OpMula:
        return "takedown::guest::vu1_detail::stage_vu1_upper_opmula";
    case guest::vu1_detail::UpperOp::Opmsub:
        return "takedown::guest::vu1_detail::stage_vu1_upper_opmsub";
    case guest::vu1_detail::UpperOp::AddaLane:
        return "takedown::guest::vu1_detail::stage_vu1_upper_adda_lane";
    case guest::vu1_detail::UpperOp::Adda:
        return "takedown::guest::vu1_detail::stage_vu1_upper_adda";
    case guest::vu1_detail::UpperOp::Madda:
        return "takedown::guest::vu1_detail::stage_vu1_upper_madda";
    case guest::vu1_detail::UpperOp::MulQ:
        return "takedown::guest::vu1_detail::stage_vu1_upper_mul_q";
    case guest::vu1_detail::UpperOp::MaddQ:
        return "takedown::guest::vu1_detail::stage_vu1_upper_madd_q";
    case guest::vu1_detail::UpperOp::Abs:
        return "takedown::guest::vu1_detail::stage_vu1_upper_abs";
    case guest::vu1_detail::UpperOp::Vitof:
        return "takedown::guest::vu1_detail::stage_vu1_upper_vitof";
    case guest::vu1_detail::UpperOp::Vftoi:
        return "takedown::guest::vu1_detail::stage_vu1_upper_vftoi";
    case guest::vu1_detail::UpperOp::Clip:
        return "takedown::guest::vu1_detail::stage_vu1_upper_clip";
    default:
        return "nullptr";
    }
}

[[nodiscard]] std::optional<std::string_view>
generated_upper_stage_symbol(const CachedVu1Pair& pair) {
    if (!pair.upper.has_value()) {
        return std::nullopt;
    }
    if (pair.upper->op == guest::vu1_detail::UpperOp::Nop) {
        return std::string_view{"nullptr"};
    }
    switch (pair.upper->op) {
    case guest::vu1_detail::UpperOp::AddLane:
    case guest::vu1_detail::UpperOp::SubLane:
    case guest::vu1_detail::UpperOp::MaddLane:
    case guest::vu1_detail::UpperOp::MsubLane:
    case guest::vu1_detail::UpperOp::MulQ:
    case guest::vu1_detail::UpperOp::Mul:
    case guest::vu1_detail::UpperOp::AddaLane:
    case guest::vu1_detail::UpperOp::MaddaLane:
    case guest::vu1_detail::UpperOp::MulaLane:
    case guest::vu1_detail::UpperOp::Vftoi:
    case guest::vu1_detail::UpperOp::Clip:
        break;
    default:
        return std::nullopt;
    }
    const auto symbol = upper_stage_symbol(pair);
    if (symbol == "nullptr") {
        return std::nullopt;
    }
    return symbol;
}

[[nodiscard]] std::optional<std::string_view>
generated_lower_kind_symbol(const CachedVu1Pair& pair) {
    if (!pair.lower_decoded) {
        return std::nullopt;
    }
    if (pair.i_bit) {
        return "Vu1PreparedLowerKind::Noop";
    }
    if (!pair.lower.has_value()) {
        return std::nullopt;
    }
    switch (pair.lower->op) {
    case guest::vu1_detail::LowerOp::Nop:
        return "Vu1PreparedLowerKind::Noop";
    case guest::vu1_detail::LowerOp::Iaddiu:
        return "Vu1PreparedLowerKind::Iaddiu";
    case guest::vu1_detail::LowerOp::Lq:
        return pair.lower->mask == 15U ? "Vu1PreparedLowerKind::LqFull"
                                       : "Vu1PreparedLowerKind::LqMasked";
    case guest::vu1_detail::LowerOp::Sq:
        return pair.lower->mask == 15U ? "Vu1PreparedLowerKind::SqFull"
                                       : "Vu1PreparedLowerKind::SqMasked";
    case guest::vu1_detail::LowerOp::Mr32:
        return pair.lower->mask == 15U ? "Vu1PreparedLowerKind::Mr32Full"
                                       : "Vu1PreparedLowerKind::Mr32Masked";
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<GeneratedPairRecipe> generated_pair_recipe(const CachedVu1Pair& pair) {
    if (pair.flow != guest::vu1_detail::CachedVu1Flow::Fallthrough || pair.e_bit ||
        pair.exits_after_delay) {
        return std::nullopt;
    }
    const auto upper = generated_upper_stage_symbol(pair);
    const auto lower = generated_lower_kind_symbol(pair);
    if (!upper.has_value() || !lower.has_value()) {
        return std::nullopt;
    }
    return GeneratedPairRecipe{*upper, *lower};
}

[[nodiscard]] std::vector<GeneratedSpan>
find_generated_spans(const std::span<const ObservedBasicBlock> blocks) {
    std::vector<GeneratedSpan> spans;
    for (const auto& block : blocks) {
        auto offset = std::size_t{0U};
        while (offset < block.pairs.size()) {
            while (offset < block.pairs.size() && !generated_pair_recipe(block.pairs[offset])) {
                ++offset;
            }
            const auto start = offset;
            std::vector<GeneratedPairRecipe> recipes;
            while (offset < block.pairs.size()) {
                const auto recipe = generated_pair_recipe(block.pairs[offset]);
                if (!recipe.has_value()) {
                    break;
                }
                recipes.push_back(*recipe);
                ++offset;
            }
            if (recipes.size() > 3U) {
                spans.push_back(GeneratedSpan{static_cast<std::uint16_t>(block.start_pc + start),
                                              std::move(recipes)});
            }
        }
    }
    return spans;
}

[[nodiscard]] std::string generated_span_recipes_symbol(const std::size_t program_index,
                                                        const std::uint16_t start_pc) {
    return "kGeneratedSpanRecipes_" + std::to_string(program_index) + "_" +
           std::to_string(start_pc);
}

[[nodiscard]] std::string generated_span_executor_symbol(const std::size_t program_index,
                                                         const std::uint16_t start_pc) {
    return "execute_generated_span_" + std::to_string(program_index) + "_" +
           std::to_string(start_pc);
}

template <std::size_t Count>
void emit_u8_array(std::ostringstream& output, const std::array<std::uint8_t, Count>& values) {
    output << '{';
    for (std::size_t index = 0U; index != values.size(); ++index) {
        if (index != 0U) {
            output << ", ";
        }
        output << static_cast<unsigned int>(values[index]) << 'U';
    }
    output << '}';
}

void emit_generated_spans(std::ostringstream& output, const std::span<const GeneratedSpan> spans,
                          const std::size_t program_index) {
    for (const auto& span : spans) {
        output << "constexpr std::array<Vu1PreparedPairRecipe, " << std::dec
               << span.recipes.size() << "U> "
               << generated_span_recipes_symbol(program_index, span.start_pc) << "{\n";
        for (const auto& recipe : span.recipes) {
            output << "    Vu1PreparedPairRecipe{" << recipe.upper_stage << ", "
                   << recipe.lower_kind << "},\n";
        }
        output << "};\n\n"
               << "void " << generated_span_executor_symbol(program_index, span.start_pc)
               << "(takedown::guest::Vu1GeneratedSpanContext& context) noexcept {\n";
        for (const auto& recipe : span.recipes) {
            output << "    if (!issue_vu1_prepared_pair<" << recipe.upper_stage << ", "
                   << recipe.lower_kind << ">(context)) return;\n";
        }
        output << "}\n\n";
    }
    if (spans.empty()) {
        return;
    }

    std::vector<const GeneratedSpan*> by_start;
    by_start.reserve(spans.size());
    for (const auto& span : spans) {
        by_start.push_back(&span);
    }
    std::sort(by_start.begin(), by_start.end(),
              [](const auto* lhs, const auto* rhs) { return lhs->start_pc < rhs->start_pc; });

    output << "Vu1GeneratedSpan resolve_generated_span_" << std::dec << program_index
           << "(const std::uint16_t pc) noexcept {\n"
           << "    switch (pc) {\n";
    for (const auto* span : by_start) {
        for (std::size_t offset = 0U; offset < span->recipes.size(); ++offset) {
            output << "    case " << hex16(static_cast<std::uint16_t>(span->start_pc + offset))
                   << "U:\n";
        }
        output << "        return Vu1GeneratedSpan{\n"
               << "            .start_pc = " << hex16(span->start_pc) << "U,\n"
               << "            .recipes = std::span<const Vu1PreparedPairRecipe>{"
               << generated_span_recipes_symbol(program_index, span->start_pc) << "},\n"
               << "            .executor = "
               << generated_span_executor_symbol(program_index, span->start_pc) << ",\n"
               << "        };\n";
    }
    output << "    default:\n"
           << "        return {};\n"
           << "    }\n"
           << "}\n\n";
}

// One CachedVu1Pair aggregate initialiser; every field is spelled out so the generated
// table needs no runtime decode.
void emit_cached_pair(std::ostringstream& output, const CachedVu1Pair& pair) {
    output << "    CachedVu1Pair{\n"
           << "        .lower_word = " << hex32(pair.lower_word) << "U,\n"
           << "        .upper_word = " << hex32(pair.upper_word) << "U,\n";
    if (pair.upper.has_value()) {
        const auto& upper = *pair.upper;
        output << "        .upper = std::optional<DecodedUpper>{DecodedUpper{\n"
               << "            .op = static_cast<UpperOp>("
               << static_cast<unsigned int>(upper.op) << "U),\n"
               << "            .lane = " << static_cast<unsigned int>(upper.lane)
               << "U,\n"
               << "            .ft = " << static_cast<unsigned int>(upper.ft)
               << "U,\n"
               << "            .fs = " << static_cast<unsigned int>(upper.fs)
               << "U,\n"
               << "            .fd = " << static_cast<unsigned int>(upper.fd)
               << "U,\n"
               << "            .fraction = "
               << static_cast<unsigned int>(upper.fraction) << "U,\n"
               << "        }},\n";
    } else {
        output << "        .upper = std::nullopt,\n";
    }
    if (pair.lower.has_value()) {
        const auto& lower = *pair.lower;
        output << "        .lower = std::optional<DecodedLower>{DecodedLower{\n"
               << "            .op = static_cast<LowerOp>("
               << static_cast<unsigned int>(lower.op) << "U),\n"
               << "            .mask = " << static_cast<unsigned int>(lower.mask)
               << "U,\n"
               << "            .it = " << static_cast<unsigned int>(lower.it)
               << "U,\n"
               << "            .is = " << static_cast<unsigned int>(lower.is)
               << "U,\n"
               << "            .ft = " << static_cast<unsigned int>(lower.ft)
               << "U,\n"
               << "            .fs = " << static_cast<unsigned int>(lower.fs)
               << "U,\n"
               << "            .fd = " << static_cast<unsigned int>(lower.fd)
               << "U,\n"
               << "            .immediate = " << static_cast<int>(lower.immediate)
               << ",\n"
               << "            .immediate24 = " << hex32(lower.immediate24)
               << "U,\n"
               << "        }},\n";
    } else {
        output << "        .lower = std::nullopt,\n";
    }
    output << "        .vf_read_lanes = ";
    emit_u8_array(output, pair.vf_read_lanes);
    output << ",\n"
           << "        .vf_read_lane_count = "
           << static_cast<unsigned int>(pair.vf_read_lane_count) << "U,\n"
           << "        .upper_vf_write_lanes = ";
    emit_u8_array(output, pair.upper_vf_write_lanes);
    output << ",\n"
           << "        .lower_vf_write_lanes = ";
    emit_u8_array(output, pair.lower_vf_write_lanes);
    output << ",\n"
           << "        .upper_vf_write_lane_count = "
           << static_cast<unsigned int>(pair.upper_vf_write_lane_count) << "U,\n"
           << "        .lower_vf_write_lane_count = "
           << static_cast<unsigned int>(pair.lower_vf_write_lane_count) << "U,\n"
           << "        .vi_read_mask = " << hex16(pair.vi_read_mask) << "U,\n"
           << "        .direct_branch_target = " << hex16(pair.direct_branch_target)
           << "U,\n"
           << "        .unsigned_immediate15 = " << hex16(pair.unsigned_immediate15)
           << "U,\n"
           << "        .destination_mask = "
           << static_cast<unsigned int>(pair.destination_mask) << "U,\n"
           << "        .flow = static_cast<CachedVu1Flow>("
           << static_cast<unsigned int>(pair.flow) << "U),\n"
           << "        .i_bit = " << (pair.i_bit ? "true" : "false") << ",\n"
           << "        .e_bit = " << (pair.e_bit ? "true" : "false") << ",\n"
           << "        .lower_decoded = " << (pair.lower_decoded ? "true" : "false")
           << ",\n"
           << "        .lower_uses_fdiv_pipe = "
           << (pair.lower_uses_fdiv_pipe ? "true" : "false") << ",\n"
           << "        .lower_uses_efu_pipe = "
           << (pair.lower_uses_efu_pipe ? "true" : "false") << ",\n"
           << "        .exits_after_delay = "
           << (pair.exits_after_delay ? "true" : "false") << ",\n"
           << "        .initialized = " << (pair.initialized ? "true" : "false") << ",\n"
           << "    },\n";
}

void emit_static_blocks(std::ostringstream& output, const std::span<const ObservedBasicBlock> blocks,
                        const std::size_t program_index) {
    for (const auto& block : blocks) {
        output << "// Natural block " << hex16(block.start_pc) << ", " << std::dec
               << block.pairs.size() << " pair(s), profile weight " << block.committed_pairs
               << ".\n"
               << "constexpr std::array<CachedVu1Pair, " << block.pairs.size() << "U> "
               << static_block_symbol(program_index, block.start_pc) << "{\n";
        for (const auto& pair : block.pairs) {
            emit_cached_pair(output, pair);
        }
        output << "};\n"
               << "constexpr std::array<Vu1UpperStageThunk, " << block.pairs.size() << "U> "
               << static_upper_stages_symbol(program_index, block.start_pc) << "{\n";
        for (const auto& pair : block.pairs) {
            output << "    " << upper_stage_symbol(pair) << ",\n";
        }
        output << "};\n\n";
    }
    if (blocks.empty()) {
        return;
    }

    std::vector<const ObservedBasicBlock*> by_start;
    by_start.reserve(blocks.size());
    for (const auto& block : blocks) {
        by_start.push_back(&block);
    }
    std::sort(by_start.begin(), by_start.end(),
              [](const auto* lhs, const auto* rhs) { return lhs->start_pc < rhs->start_pc; });

    output << "Vu1StaticBasicBlock resolve_static_block_" << std::dec << program_index
           << "(const std::uint16_t pc) noexcept {\n"
           << "    switch (pc) {\n";
    for (const auto* block : by_start) {
        for (std::size_t offset = 0U; offset < block->pairs.size(); ++offset) {
            output << "    case " << hex16(static_cast<std::uint16_t>(block->start_pc + offset))
                   << "U:\n";
        }
        output << "        return Vu1StaticBasicBlock{\n"
               << "            .start_pc = " << hex16(block->start_pc) << "U,\n"
               << "            .pairs = std::span<const CachedVu1Pair>{"
               << static_block_symbol(program_index, block->start_pc) << "},\n"
               << "            .upper_stages = std::span<const Vu1UpperStageThunk>{"
               << static_upper_stages_symbol(program_index, block->start_pc) << "},\n"
               << "        };\n";
    }
    output << "    default:\n"
           << "        return {};\n"
           << "    }\n"
           << "}\n\n";
}

[[nodiscard]] bool profile_precedes(const RankedSnapshot& left,
                                    const RankedSnapshot& right) noexcept {
    const auto& lhs = left.snapshot->profile;
    const auto& rhs = right.snapshot->profile;
    if (lhs.host_nanoseconds != rhs.host_nanoseconds) {
        return lhs.host_nanoseconds > rhs.host_nanoseconds;
    }
    if (lhs.committed_pairs != rhs.committed_pairs) {
        return lhs.committed_pairs > rhs.committed_pairs;
    }
    if (lhs.runs != rhs.runs) {
        return lhs.runs > rhs.runs;
    }
    return std::tie(lhs.hash.high, lhs.hash.low, lhs.entry, lhs.resolved_entry) <
           std::tie(rhs.hash.high, rhs.hash.low, rhs.entry, rhs.resolved_entry);
}

[[nodiscard]] std::vector<RankedSnapshot>
validate_and_rank(const std::span<const Vu1ProfiledProgramSnapshot> snapshots) {
    std::vector<RankedSnapshot> ranked;
    ranked.reserve(snapshots.size());
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint16_t, std::uint16_t>,
             const Vu1ProfiledProgramSnapshot*>
        identities;
    for (const auto& snapshot : snapshots) {
        if (snapshot.micro_memory.size() != 16384U) {
            throw std::invalid_argument(
                "VU1 program module snapshot must contain exactly 16 KiB of MicroMem");
        }
        if (snapshot.profile.entry >= kVu1MicroProgramPairCount ||
            snapshot.profile.resolved_entry >= kVu1MicroProgramPairCount) {
            throw std::invalid_argument(
                "VU1 program module snapshot entry is outside the 11-bit MicroMem range");
        }
        const auto identity =
            std::make_tuple(snapshot.profile.hash.high, snapshot.profile.hash.low,
                            snapshot.profile.entry, snapshot.profile.resolved_entry);
        if (const auto existing = identities.find(identity); existing != identities.end()) {
            if (existing->second->micro_memory != snapshot.micro_memory) {
                throw std::invalid_argument(
                    "VU1 program module contains an ambiguous hash/entry/resolved-entry identity");
            }
        } else {
            identities.emplace(identity, &snapshot);
        }
        ranked.push_back(RankedSnapshot{&snapshot});
    }
    std::sort(ranked.begin(), ranked.end(), profile_precedes);
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint16_t, std::uint16_t>>
        selected_identities;
    std::erase_if(ranked, [&selected_identities](const RankedSnapshot& candidate) {
        const auto& profile = candidate.snapshot->profile;
        return !selected_identities
                    .insert({profile.hash.high, profile.hash.low, profile.entry,
                             profile.resolved_entry})
                    .second;
    });
    return ranked;
}

// One 16 KiB MicroMem image as a constexpr std::byte table, sixteen bytes per line.
void emit_micro_memory(std::ostringstream& output, const std::size_t image_index,
                       const Vu1ProfiledProgramSnapshot& image) {
    output << "constexpr std::array<std::byte, " << std::dec << 16384U << "U> kMicroMemory_"
           << image_index << "{\n";
    for (std::size_t byte = 0U; byte < image.micro_memory.size(); ++byte) {
        if (byte % 16U == 0U) {
            output << "    ";
        }
        output << "std::byte{" << std::hex << std::nouppercase << std::setfill('0') << "0x"
               << std::setw(2) << std::to_integer<unsigned int>(image.micro_memory[byte]) << "}";
        if (byte + 1U != image.micro_memory.size()) {
            output << ", ";
        }
        if (byte % 16U == 15U || byte + 1U == image.micro_memory.size()) {
            output << '\n';
        }
    }
    output << "};\n\n";
}

// Conservative first portable executor: only straight-line pairs with an upper NOP and
// a lower NOP or integer literal arithmetic, up to the first E-bit delay slot.
[[nodiscard]] bool
supports_linear_integer_executor(const Vu1ProfiledProgramSnapshot& snapshot) noexcept {
    const auto is_literal_lower = [](const CachedVu1Pair& literal_pair) {
        switch (literal_pair.lower->op) {
        case guest::vu1_detail::LowerOp::Nop:
        case guest::vu1_detail::LowerOp::Iaddiu:
        case guest::vu1_detail::LowerOp::Isubiu:
        case guest::vu1_detail::LowerOp::Iadd:
        case guest::vu1_detail::LowerOp::Isub:
        case guest::vu1_detail::LowerOp::Iaddi:
        case guest::vu1_detail::LowerOp::Iand:
        case guest::vu1_detail::LowerOp::Ior:
            return true;
        default:
            return false;
        }
    };
    auto pair = static_cast<std::uint64_t>(snapshot.profile.entry);
    bool e_delay_pending = false;
    for (std::size_t visited = 0U; visited != kVu1MicroProgramPairCount; ++visited) {
        const auto decoded = guest::vu1_detail::decode_vu1_pair_metadata(
            std::span<const std::byte>{snapshot.micro_memory}, static_cast<std::uint16_t>(pair));
        if (!decoded.upper.has_value() || decoded.upper->op != guest::vu1_detail::UpperOp::Nop ||
            !decoded.lower_decoded) {
            return false;
        }
        if (!decoded.i_bit && !is_literal_lower(decoded)) {
            return false;
        }
        if (e_delay_pending) {
            return true;
        }
        e_delay_pending = decoded.e_bit;
        pair = (pair + 1U) & (kVu1MicroProgramPairCount - 1U);
    }
    return true;
}

// Emits the complete local-only module: MicroMem images, static block and generated span
// tables, the optional linear-integer executors and register_generated_vu1_program_module().
//
// RECOVERY: GIMPLE matches the original except for the `SR.N` scalar-replacement counter in
// four field loads, which sits 12 higher here because of unrelated earlier functions.
[[nodiscard]] std::string compose_source(const std::span<const RankedSnapshot> selected) {
    std::vector<const Vu1ProfiledProgramSnapshot*> images;
    std::vector<std::size_t> selected_image_indices;
    std::vector<std::vector<ObservedBasicBlock>> selected_blocks;
    std::vector<std::vector<GeneratedSpan>> selected_generated_spans;
    std::vector<bool> selected_linear_integer_executors;
    images.reserve(selected.size());
    selected_image_indices.reserve(selected.size());
    selected_blocks.reserve(selected.size());
    selected_generated_spans.reserve(selected.size());
    selected_linear_integer_executors.reserve(selected.size());
    // The static-block budget is shared by every selected program.
    auto remaining_static_pairs = std::uint64_t{8192U};
    for (const auto& candidate : selected) {
        const auto existing =
            std::find_if(images.begin(), images.end(), [&candidate](const auto* image) {
                return image->profile.hash == candidate.snapshot->profile.hash &&
                       image->micro_memory == candidate.snapshot->micro_memory;
            });
        if (existing == images.end()) {
            selected_image_indices.push_back(images.size());
            images.push_back(candidate.snapshot);
        } else {
            selected_image_indices.push_back(static_cast<std::size_t>(existing - images.begin()));
        }

        const auto supports_linear_integer = supports_linear_integer_executor(*candidate.snapshot);
        selected_linear_integer_executors.push_back(supports_linear_integer);

        auto blocks = supports_linear_integer ? std::vector<ObservedBasicBlock>{}
                                              : find_observed_basic_blocks(*candidate.snapshot);
        std::vector<ObservedBasicBlock> budgeted_blocks;
        budgeted_blocks.reserve(blocks.size());
        for (auto& block : blocks) {
            if (remaining_static_pairs < block.pairs.size()) {
                continue;
            }
            remaining_static_pairs -= block.pairs.size();
            budgeted_blocks.push_back(std::move(block));
        }
        selected_generated_spans.push_back(find_generated_spans(budgeted_blocks));
        selected_blocks.push_back(std::move(budgeted_blocks));
    }

    std::ostringstream output;
    output << "// Local-only generated VU1 program descriptors; do not distribute.\n"
           << "// Generated by build_vu1_program_module; do not edit by hand.\n"
           << "#include \"takedown/guest/vu1_micro_interpreter.hpp\"\n\n"
           << "#include <array>\n"
           << "#include <cstddef>\n"
           << "#include <cstdint>\n"
           << "#include <optional>\n"
           << "#include <span>\n\n"
           << "namespace takedown::recompiler::generated {\n\n"
           << "static_assert(takedown::guest::kVu1CompiledProgramAbiVersion == 2U,\n"
           << "              \"regenerate this local VU1 module for the current compiled-program ABI\");\n"
           << "static_assert(takedown::guest::vu1_detail::kVu1StaticPairMetadataVersion == 3U,\n"
           << "              \"regenerate this local VU1 module for the current static-pair metadata schema\");\n"
           << "static_assert(takedown::guest::vu1_detail::kVu1GeneratedSpanAbiVersion == 1U,\n"
           << "              \"regenerate this local VU1 module for the current generated-span ABI\");\n\n"
           << "namespace {\n\n"
           << "using CachedVu1Flow = takedown::guest::vu1_detail::CachedVu1Flow;\n"
           << "using CachedVu1Pair = takedown::guest::vu1_detail::CachedVu1Pair;\n"
           << "using DecodedLower = takedown::guest::vu1_detail::DecodedLower;\n"
           << "using DecodedUpper = takedown::guest::vu1_detail::DecodedUpper;\n"
           << "using LowerOp = takedown::guest::vu1_detail::LowerOp;\n"
           << "using UpperOp = takedown::guest::vu1_detail::UpperOp;\n"
           << "using Vu1PreparedLowerKind = takedown::guest::vu1_detail::Vu1PreparedLowerKind;\n"
           << "using Vu1PreparedPairRecipe = takedown::guest::vu1_detail::Vu1PreparedPairRecipe;\n"
           << "using Vu1GeneratedSpan = takedown::guest::vu1_detail::Vu1GeneratedSpan;\n"
           << "using Vu1UpperStageThunk = takedown::guest::vu1_detail::Vu1UpperStageThunk;\n"
           << "using Vu1StaticBasicBlock = takedown::guest::vu1_detail::Vu1StaticBasicBlock;\n\n";
    output << "using takedown::guest::vu1_detail::issue_vu1_prepared_pair;\n\n";

    for (std::size_t index = 0U; index < images.size(); ++index) {
        emit_micro_memory(output, index, *images[index]);
    }

    for (std::size_t index = 0U; index < selected_blocks.size(); ++index) {
        emit_static_blocks(output, selected_blocks[index], index);
        emit_generated_spans(output, selected_generated_spans[index], index);
    }

    for (std::size_t index = 0U; index < selected.size(); ++index) {
        if (!selected_linear_integer_executors[index]) {
            continue;
        }
        output << "takedown::guest::Vu1RunResult execute_program_" << std::dec << index
               << "(takedown::guest::Vu1CompiledProgramContext& context) {\n"
               << "    return takedown::guest::execute_vu1_compiled_linear_integer_program(\n"
               << "        context, std::span<const std::byte>{kMicroMemory_"
               << selected_image_indices[index] << "});\n"
               << "}\n\n";
    }

    output << "} // namespace\n\n"
           << "bool register_generated_vu1_program_module(\n"
           << "    takedown::guest::Vu1ProgramCache& cache) {\n"
           << "    static_cast<void>(cache);\n";
    for (std::size_t index = 0U; index < selected.size(); ++index) {
        const auto& profile = selected[index].snapshot->profile;
        output << "    // Profile-resolved first block: " << hex16(profile.resolved_entry) << "\n"
               << "    if (!cache.register_compiled_program(\n"
               << "            takedown::guest::Vu1CompiledProgramDescriptor{\n"
               << "                .hash = takedown::guest::Vu1ProgramHash{\n"
               << "                    .low = " << hex64(profile.hash.low) << "ULL,\n"
               << "                    .high = " << hex64(profile.hash.high) << "ULL,\n"
               << "                },\n"
               << "                .entry = " << hex16(profile.entry) << "U,\n"
               << "                .resolved_entry = " << hex16(profile.resolved_entry) << "U,\n"
               << "                .micro_memory = std::span<const std::byte>{"
               << "kMicroMemory_" << std::dec << selected_image_indices[index] << "},\n"
               << (!selected_blocks[index].empty()
                       ? "                .static_block_resolver = resolve_static_block_" +
                             std::to_string(index) + ",\n"
                       : std::string{})
               << (!selected_blocks[index].empty()
                       ? std::string{"                .static_metadata_version = 3U,\n"}
                       : std::string{})
               << (!selected_generated_spans[index].empty()
                       ? "                .generated_span_resolver = resolve_generated_span_" +
                             std::to_string(index) + ",\n"
                       : std::string{})
               << (!selected_generated_spans[index].empty()
                       ? std::string{"                .generated_span_abi_version = 1U,\n"}
                       : std::string{})
               << (selected_linear_integer_executors[index]
                       ? "                .executor = execute_program_" + std::to_string(index) +
                             ",\n"
                       : std::string{})
               << "                .abi_version = 2U,\n"
               << "            })) {\n"
               << "        return false;\n"
               << "    }\n";
    }
    output << "    return true;\n"
           << "}\n\n"
           << "} // namespace takedown::recompiler::generated\n";
    return output.str();
}

} // namespace

std::string build_vu1_program_module(const std::span<const Vu1ProfiledProgramSnapshot> snapshots,
                                     const std::size_t maximum_programs) {
    auto ranked = validate_and_rank(snapshots);
    if (ranked.size() > maximum_programs) {
        ranked.resize(maximum_programs);
    }
    return compose_source(ranked);
}

} // namespace takedown::recompiler
