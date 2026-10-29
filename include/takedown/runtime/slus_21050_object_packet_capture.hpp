#pragma once

#include "takedown/guest/r5900_state.hpp"
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <vector>

namespace takedown::guest { class EeMemory; }
namespace takedown::runtime::slus_21050 {
inline constexpr std::uint32_t kObjectPacketBegin = 0x0026af50U;
inline constexpr std::uint32_t kObjectPacketEnd = 0x0026b1a8U;
struct ObjectPacketRegion {
    std::uint32_t address{};
    std::vector<std::byte> before, after;
};
struct ObjectPacketCase {
    std::uint64_t sequence{}, retired{};
    guest::R5900State before, after;
    std::vector<ObjectPacketRegion> regions;
};
void write_object_packet_case(std::ostream&, const ObjectPacketCase&);
[[nodiscard]] ObjectPacketCase read_object_packet_case(std::istream&);
[[nodiscard]] bool same_object_packet_cpu(const guest::R5900State&, const guest::R5900State&);

// Read-only bounded recovery probe. This is never a native execution path.
class ObjectPacketCapture {
public:
    ObjectPacketCapture();
    [[nodiscard]] bool enabled() const noexcept { return !directory_.empty(); }
    void observe(const guest::R5900State&, const guest::EeMemory&, std::uint64_t step,
                 bool measured_window);
    [[nodiscard]] unsigned captured() const noexcept { return captured_; }
private:
    std::filesystem::path directory_;
    std::optional<ObjectPacketCase> pending_;
    std::uint64_t sequence_{}, start_step_{};
    unsigned captured_{};
};
}
