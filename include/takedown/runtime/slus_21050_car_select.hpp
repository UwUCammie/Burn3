#pragma once
#include "takedown/platform/parallel_gs_host_ui.hpp"
#include <memory>
#include <span>
#include <cstdint>
namespace takedown::content { class ModManager; }
namespace takedown::guest { class EeMemory; class R5900State; }
namespace takedown::runtime {
class NativeFunctionDispatcher;
class Slus21050AddedCars;
// Native catalogue controller behind the original frontend presenter.
// Original screen construction, artwork, transforms and animation remain intact.
class Slus21050CarSelect {
public:
    explicit Slus21050CarSelect(Slus21050AddedCars& cars);
    bool register_functions(NativeFunctionDispatcher&, const guest::EeMemory&);
    static std::span<const std::uint32_t> hook_addresses();
    bool on_dispatch(guest::R5900State&, guest::EeMemory&);
    void refresh(const guest::EeMemory&, const content::ModManager&, bool content_changed = false);
    // The skin button (R1) on the car list: the selected car's next skin
    // (docs/MODDING.md, "Car skins"). False when the list is not idle or the
    // car has no skins.
    bool next_skin(guest::EeMemory&);
    [[nodiscard]] platform::ParallelGsHostUi host_ui(const guest::EeMemory&) const;
    struct State;
private:
    std::shared_ptr<State> state_;
};
}
