#pragma once

#include <array>
#include <cstdint>

namespace takedown::guest {

using GuestAddress = std::uint32_t;

struct Gpr128 {
    std::array<std::uint32_t, 4> words{};

    bool operator==(const Gpr128&) const = default;
};

} // namespace takedown::guest
