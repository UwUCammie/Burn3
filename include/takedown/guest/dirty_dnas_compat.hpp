#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstddef>
#include <cstdint>

namespace takedown::guest {

// Burnout 3 ships its obsolete online-authentication client as a relocatable
// ELF inside DATA/INTRO2.M2V.  The retail DirtyElf loader copies the module's
// four-entry export table over these fixed trampolines after relocation.  This
// compatibility boundary deliberately leaves that load/relocation path intact
// and handles only the three authentication calls made by the retail wrapper.
enum class DirtyDnasCompatCall : std::uint8_t {
    NotHandled,
    Start,
    Update,
    Stop,
};

struct DirtyDnasCompatActivity {
    std::uint64_t starts{};
    std::uint64_t updates{};
    std::uint64_t stops{};
};

class DirtyDnasCompatHle {
  public:
    static constexpr GuestAddress kStartTrampoline = 0x0041be28U;
    static constexpr GuestAddress kUpdateTrampoline = 0x0041be30U;
    static constexpr GuestAddress kStopTrampoline = 0x0041be38U;
    static constexpr GuestAddress kSimulateErrorTrampoline = 0x0041be40U;
    static constexpr std::size_t kResultSize = 0x58cU;

    [[nodiscard]] DirtyDnasCompatCall try_handle(R5900State& state, EeMemory& memory);
    [[nodiscard]] bool matches_retail_revision(const EeMemory& memory) const;
    [[nodiscard]] const DirtyDnasCompatActivity& activity() const noexcept;

  private:
    bool started_{};
    DirtyDnasCompatActivity activity_{};
};

} // namespace takedown::guest
