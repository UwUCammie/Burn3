#pragma once

#include "takedown/guest/retail_ds2.hpp"
#include "takedown/guest/types.hpp"

namespace takedown::guest { class EeMemory; }
namespace takedown::runtime {
// Revision adapter for the native host's second physical controller. The game
// continues to own logical-player assignment, join prompts and action edges.
class Slus21050Controllers {
public:
    static constexpr guest::GuestAddress decode_boundary = 0x00214aacU;
    [[nodiscard]] bool publish_second(guest::EeMemory&, bool connected,
                                      const guest::RetailDs2DecodedSample&);
private:
    bool owned_{};
};
}
