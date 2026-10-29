#include "takedown/recompiler/r5900_function_cfg.hpp"

#include "takedown/guest/r5900_instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace takedown::recompiler {

namespace {

// Stable order for edges: kind, source, then the optional target, delay disposition and executable flag.
bool edge_less(const R5900CfgEdge& left, const R5900CfgEdge& right) {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    if (left.source_address != right.source_address) {
        return left.source_address < right.source_address;
    }
    if (left.target_address.has_value() != right.target_address.has_value()) {
        return !left.target_address.has_value();
    }
    if (left.target_address.has_value() && *left.target_address != *right.target_address) {
        return *left.target_address < *right.target_address;
    }
    if (left.delay_slot != right.delay_slot) {
        return left.delay_slot < right.delay_slot;
    }
    return left.target_is_in_executable < right.target_is_in_executable;
}

void sort_unique_edges(std::vector<R5900CfgEdge>& edges) {
    std::sort(edges.begin(), edges.end(), edge_less);
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}


// Per-address facts gathered while walking reachable code.
struct InstructionRecord {
    guest::DecodedR5900Instruction decoded{};
    bool referenced_as_delay_slot{};
};

struct FlowRecord {
    std::vector<R5900CfgEdge> edges{};
    bool terminator{};
};

bool blocker_less(const R5900CfgBlocker& left, const R5900CfgBlocker& right) {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    if (left.address != right.address) {
        return left.address < right.address;
    }
    if (left.related_address.has_value() != right.related_address.has_value()) {
        return !left.related_address.has_value();
    }
    return left.related_address.has_value() && *left.related_address < *right.related_address;
}

void sort_unique_blockers(std::vector<R5900CfgBlocker>& blockers) {
    std::sort(blockers.begin(), blockers.end(), blocker_less);
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
}

class FunctionCfgBuilder {
  public:
    FunctionCfgBuilder(const guest::GuestAddress entry, const R5900ExecutableWordSpan executable,
                       const std::size_t max_instruction_count)
        : entry_(entry), executable_(executable), max_instruction_count_(max_instruction_count) {
        result_.entry_address = entry;
    }

    void discover_from(guest::GuestAddress seed) {
        std::vector<guest::GuestAddress> pending{seed};
        while (!pending.empty()) {
            guest::GuestAddress address = pending.back();
            pending.pop_back();
            guest::GuestAddress previous = address;
            while (true) {
                if (instructions_.count(address) != 0U) {
                    break;
                }
                if (!executable_.contains(address)) {
                    result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::TargetOutsideExecutable, address,
                                                               previous});
                    break;
                }
                const std::optional<std::uint32_t> word = executable_.read_word(address);
                if (!word.has_value()) {
                    result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::UnreadableInstruction, address});
                    break;
                }
                const guest::DecodedR5900Instruction decoded = guest::decode_r5900(address, *word);
                if (decoded.kind == guest::R5900InstructionKind::Unknown) {
                    result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::UnknownInstruction, address});
                    break;
                }
                if (instructions_.size() >= max_instruction_count_) {
                    result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::InstructionLimitReached, address});
                    result_.truncated = true;
                    break;
                }
                instructions_[address] = InstructionRecord{decoded, false};
                if (decoded.control_flow == guest::R5900ControlFlow::None) {
                    previous = address;
                    address += 4U;
                    continue;
                }
                add_transfer(decoded, pending);
                break;
            }
        }
    }

    R5900FunctionCfg finish() {
        std::set<guest::GuestAddress> leaders{entry_};
        for (const auto& [address, record] : instructions_) {
            if (record.decoded.control_flow != guest::R5900ControlFlow::None) {
                leaders.insert(address + 8U);
            }
        }
        for (const auto& [address, flow] : flows_) {
            for (const R5900CfgEdge& edge : flow.edges) {
                if (edge.target_address.has_value() && instructions_.count(*edge.target_address) != 0U) {
                    leaders.insert(*edge.target_address);
                }
            }
        }
        for (const auto& [address, record] : instructions_) {
            result_.instructions.push_back(R5900CfgInstruction{record.decoded, record.referenced_as_delay_slot});
        }
        for (const guest::GuestAddress leader : leaders) {
            if (instructions_.count(leader) == 0U) {
                continue;
            }
            R5900BasicBlock block;
            block.entry_address = leader;
            for (guest::GuestAddress address = leader; instructions_.count(address) != 0U; address += 4U) {
                block.instruction_addresses.push_back(address);
                const auto flow = flows_.find(address);
                if (flow != flows_.end() && flow->second.terminator) {
                    for (const R5900CfgEdge& edge : flow->second.edges) {
                        block.outgoing_edges.push_back(edge);
                    }
                    break;
                }
                if (leaders.count(address + 4U) != 0U) {
                    break;
                }
            }
            result_.blocks.push_back(std::move(block));
        }
        sort_unique_blockers(result_.blockers);
        return std::move(result_);
    }

  private:
    void add_transfer(const guest::DecodedR5900Instruction& decoded, std::vector<guest::GuestAddress>& pending) {
        const guest::GuestAddress delay_address = decoded.pc + 4U;
        const guest::GuestAddress after = decoded.pc + 8U;
        FlowRecord flow;
        flow.terminator = true;
        if (decoded.has_delay_slot) {
            const std::optional<std::uint32_t> slot_word = executable_.read_word(delay_address);
            if (!slot_word.has_value()) {
                result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::MissingDelaySlot, decoded.pc});
                return;
            }
            const guest::DecodedR5900Instruction slot = guest::decode_r5900(delay_address, *slot_word);
            if (slot.control_flow != guest::R5900ControlFlow::None) {
                result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::ControlFlowInDelaySlot, delay_address});
                return;
            }
            instructions_[delay_address] = InstructionRecord{slot, true};
        }
        const bool likely = is_r5900_likely_branch(decoded.kind);
        switch (decoded.control_flow) {
        case guest::R5900ControlFlow::Branch: {
            const guest::GuestAddress target = decoded.target;
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::BranchTaken, decoded.pc, target,
                                              R5900DelaySlotDisposition::Execute, executable_.contains(target)});
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::BranchNotTaken, decoded.pc, after,
                                              likely ? R5900DelaySlotDisposition::Annul
                                                     : R5900DelaySlotDisposition::Execute,
                                              executable_.contains(after)});
            pending.push_back(target);
            pending.push_back(after);
            break;
        }
        case guest::R5900ControlFlow::Jump:
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::Jump, decoded.pc, decoded.target,
                                              R5900DelaySlotDisposition::Execute, executable_.contains(decoded.target)});
            pending.push_back(decoded.target);
            break;
        case guest::R5900ControlFlow::Call:
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::Call, decoded.pc, decoded.target,
                                              R5900DelaySlotDisposition::Execute, executable_.contains(decoded.target)});
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::CallReturn, decoded.pc, after,
                                              R5900DelaySlotDisposition::None, executable_.contains(after)});
            pending.push_back(after);
            break;
        case guest::R5900ControlFlow::Return:
            flow.edges.push_back(R5900CfgEdge{R5900CfgEdgeKind::Return, decoded.pc, std::nullopt,
                                              R5900DelaySlotDisposition::Execute, false});
            break;
        case guest::R5900ControlFlow::Exception:
            result_.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::ExceptionControlFlow, decoded.pc});
            break;
        case guest::R5900ControlFlow::None:
            break;
        }
        flows_[decoded.pc] = std::move(flow);
    }

    guest::GuestAddress entry_;
    R5900ExecutableWordSpan executable_;
    std::size_t max_instruction_count_;
    R5900FunctionCfg result_;
    std::map<guest::GuestAddress, InstructionRecord> instructions_;
    std::map<guest::GuestAddress, FlowRecord> flows_;
};


} // namespace

R5900FunctionCfg analyze_r5900_function_cfg(const guest::GuestAddress entry_address,
                                            const R5900ExecutableWordSpan executable,
                                            const std::size_t max_instruction_count) {
    if (!executable.is_well_formed()) {
        R5900FunctionCfg cfg;
        cfg.entry_address = entry_address;
        cfg.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::InvalidExecutableSpan, entry_address});
        return cfg;
    }
    if ((entry_address & 3U) != 0U) {
        R5900FunctionCfg cfg;
        cfg.entry_address = entry_address;
        cfg.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::MisalignedEntry, entry_address});
        return cfg;
    }
    if (!executable.contains(entry_address)) {
        R5900FunctionCfg cfg;
        cfg.entry_address = entry_address;
        cfg.blockers.push_back(R5900CfgBlocker{R5900CfgBlockerKind::EntryOutsideExecutable, entry_address});
        return cfg;
    }
    FunctionCfgBuilder builder(entry_address, executable, max_instruction_count);
    builder.discover_from(entry_address);
    return builder.finish();
}


bool R5900ExecutableWordSpan::is_well_formed() const noexcept {
    if ((base_address & 3U) != 0U) {
        return false;
    }
    const unsigned long long available_bytes = 4294967296ULL - static_cast<unsigned long long>(base_address);
    return (available_bytes >> 2U) >= words.size();
}

bool R5900ExecutableWordSpan::contains(const guest::GuestAddress address) const noexcept {
    if ((base_address & 3U) == 0U) {
        const unsigned long long available_bytes = 4294967296ULL - static_cast<unsigned long long>(base_address);
        if (words.size() <= (available_bytes >> 2U)) {
            if (address >= base_address) {
                if (((address - base_address) & 3U) == 0U) {
                    const unsigned long long byte_offset = static_cast<unsigned long long>(address - base_address);
                    return (byte_offset >> 2U) < words.size();
                }
            }
        }
    }
    return false;
}

std::optional<std::uint32_t> R5900ExecutableWordSpan::read_word(const guest::GuestAddress address) const noexcept {
    if ((base_address & 3U) != 0U) {
        return std::nullopt;
    }
    const unsigned long long available_bytes = 4294967296ULL - static_cast<unsigned long long>(base_address);
    if (words.size() > (available_bytes >> 2U)) {
        return std::nullopt;
    }
    if (address < base_address) {
        return std::nullopt;
    }
    if (((address - base_address) & 3U) != 0U) {
        return std::nullopt;
    }
    const unsigned long long byte_offset = static_cast<unsigned long long>(address - base_address);
    if (words.size() <= (byte_offset >> 2U)) {
        return std::nullopt;
    }
    const unsigned long long index = static_cast<unsigned long long>(address - base_address) >> 2U;
    return words[index];
}

bool is_r5900_likely_branch(const guest::R5900InstructionKind kind) noexcept {
    switch (kind) {
    case guest::R5900InstructionKind::Bltzl:
    case guest::R5900InstructionKind::Bgezl:
    case guest::R5900InstructionKind::Beql:
    case guest::R5900InstructionKind::Bnel:
    case guest::R5900InstructionKind::Blezl:
    case guest::R5900InstructionKind::Bgtzl:
    case guest::R5900InstructionKind::Bc0fl:
    case guest::R5900InstructionKind::Bc0tl:
    case guest::R5900InstructionKind::Bc2fl:
    case guest::R5900InstructionKind::Bc2tl:
    case guest::R5900InstructionKind::Bc1fl:
    case guest::R5900InstructionKind::Bc1tl:
        return true;
    default:
        return false;
    }
}


} // namespace takedown::recompiler
