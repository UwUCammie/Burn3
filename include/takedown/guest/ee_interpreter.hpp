#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::guest {

// Host-only decode cache for the scalar reference interpreter. Every lookup is
// validated against both the live guest PC and the instruction word just read
// from EE memory, so relocated or self-modifying code observes exactly the same
// decode as the uncached path. Owning the cache at the runner level keeps the
// standalone step_r5900 API deterministic and allocation-free for tests.
class R5900InstructionCache {
  public:
    static constexpr std::size_t kEntryCount = 4096U;

    [[nodiscard]] const DecodedR5900Instruction&
    decode(GuestAddress pc, std::uint32_t word) noexcept;
    void clear() noexcept;

  private:
    struct Entry {
        GuestAddress pc{};
        std::uint32_t word{};
        DecodedR5900Instruction instruction{};
        bool valid{};
    };

    static_assert((kEntryCount & (kEntryCount - 1U)) == 0U);
    std::array<Entry, kEntryCount> entries_{};
};

[[nodiscard]] EeStepResult step_r5900(R5900State& state, EeMemory& memory);
[[nodiscard]] EeStepResult step_r5900(R5900State& state, EeMemory& memory,
                                      R5900InstructionCache& cache);
// Executes one already-fetched instruction at state.pc(), exactly as
// step_r5900 does after its fetch. Compiled guest code (src/guest/ee_aot.cpp)
// uses it for the instructions it does not translate.
[[nodiscard]] EeStepResult execute_r5900_decoded(R5900State& state, EeMemory& memory,
                                                 const DecodedR5900Instruction& instruction);

} // namespace takedown::guest
