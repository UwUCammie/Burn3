#include "takedown/runtime/game/n3.hpp"

#include <vector>

#ifndef N3_LIST
#define N3_LIST "n3_list.inc"
#endif

namespace takedown::runtime {

#define N3(address) extern const EngineFunction n3_##address;
#include N3_LIST
#undef N3

std::span<const EngineFunction> n3_engine_functions() {
    static const std::vector<EngineFunction> functions{
#define N3(address) n3_##address,
#include N3_LIST
#undef N3
    };
    return functions;
}

} // namespace takedown::runtime
