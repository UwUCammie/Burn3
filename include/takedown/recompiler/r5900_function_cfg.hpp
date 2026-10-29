#pragma once

#include "takedown/guest/r5900_instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::recompiler {

// A non-owning, word-aligned executable range.  The caller deliberately chooses
// the range supplied to one analysis; the CFG builder never scans unrelated ELF
// contents for plausible-looking instructions or function starts.
struct R5900ExecutableWordSpan {
    guest::GuestAddress base_address{};
    std::span<const std::uint32_t> words{};

    [[nodiscard]] bool is_well_formed() const noexcept;
    [[nodiscard]] bool contains(guest::GuestAddress address) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    read_word(guest::GuestAddress address) const noexcept;
};

enum class R5900CfgEdgeKind : std::uint8_t {
    Fallthrough,
    BranchTaken,
    BranchNotTaken,
    Jump,
    Call,
    CallReturn,
    Return,
    IndirectJump,
    IndirectCall,
    Exception,
};

// Delay-slot execution belongs to an outgoing transfer, rather than to the
// lexical instruction list.  In particular, a likely branch annuls its slot on
// the not-taken edge.
enum class R5900DelaySlotDisposition : std::uint8_t {
    None,
    Execute,
    Annul,
};

enum class R5900BlockDelaySlotPolicy : std::uint8_t {
    None,
    Always,
    TakenOnly,
};

struct R5900CfgEdge {
    R5900CfgEdgeKind kind{R5900CfgEdgeKind::Fallthrough};
    guest::GuestAddress source_address{};
    std::optional<guest::GuestAddress> target_address{};
    R5900DelaySlotDisposition delay_slot{R5900DelaySlotDisposition::None};
    bool target_is_in_executable{};

    bool operator==(const R5900CfgEdge&) const = default;
};

struct R5900CfgInstruction {
    guest::DecodedR5900Instruction decoded{};
    // True when at least one reachable transfer refers to this word as its
    // architectural delay slot.  The same address can also be a block leader if
    // guest code explicitly branches to it.
    bool referenced_as_delay_slot{};
};

struct R5900BasicBlock {
    guest::GuestAddress entry_address{};
    // Addresses of ordinary instructions owned by this block.  A terminating
    // control instruction is included; its slot is kept separately because a
    // likely branch does not execute that word on every outgoing edge.
    std::vector<guest::GuestAddress> instruction_addresses{};
    std::optional<guest::GuestAddress> delay_slot_address{};
    R5900BlockDelaySlotPolicy delay_slot_policy{R5900BlockDelaySlotPolicy::None};
    std::vector<R5900CfgEdge> outgoing_edges{};
};

enum class R5900CfgBlockerKind : std::uint8_t {
    InvalidExecutableSpan,
    MisalignedEntry,
    EntryOutsideExecutable,
    InstructionLimitReached,
    UnreadableInstruction,
    UnknownInstruction,
    MissingDelaySlot,
    ControlFlowInDelaySlot,
    TargetOutsideExecutable,
    AddressOverflow,
    IndirectJumpTarget,
    IndirectCallTarget,
    ExceptionControlFlow,
};

struct R5900CfgBlocker {
    R5900CfgBlockerKind kind{R5900CfgBlockerKind::UnknownInstruction};
    guest::GuestAddress address{};
    std::optional<guest::GuestAddress> related_address{};

    bool operator==(const R5900CfgBlocker&) const = default;
};

struct R5900FunctionCfg {
    guest::GuestAddress entry_address{};
    // Both collections are sorted by guest address.  Edges and blockers also
    // have stable enum/address ordering, making output suitable for generated
    // source and reproducible manifests.
    std::vector<R5900CfgInstruction> instructions{};
    std::vector<R5900BasicBlock> blocks{};
    std::vector<R5900CfgBlocker> blockers{};
    bool truncated{};

    [[nodiscard]] bool is_liftable() const noexcept { return blockers.empty(); }
};

[[nodiscard]] R5900FunctionCfg analyze_r5900_function_cfg(guest::GuestAddress entry_address,
                                                          R5900ExecutableWordSpan executable,
                                                          std::size_t max_instruction_count);

[[nodiscard]] bool is_r5900_likely_branch(guest::R5900InstructionKind kind) noexcept;

} // namespace takedown::recompiler
