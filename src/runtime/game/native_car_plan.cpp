// P1 WP3: plan_draw_car for the live guest memory. The selection logic is the template in native_car_plan.inc (also
// compiled by tests/test_native_car_plan.cpp against oracle values); this file instantiates it for EeMemory.

#include "native_car_plan.inc"

#include "takedown/guest/ee_memory.hpp"

namespace takedown::runtime::native_car {

namespace {

// Read-only accessor with the names the plan templates use (u8/s8/u16/u32/f32 of a guest address).
class EeMemoryReader {
public:
    explicit EeMemoryReader(const guest::EeMemory& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint8_t u8(const std::uint32_t a) const { return memory_.read_u8(a); }
    [[nodiscard]] std::int8_t s8(const std::uint32_t a) const { return static_cast<std::int8_t>(memory_.read_u8(a)); }
    [[nodiscard]] std::uint16_t u16(const std::uint32_t a) const { return memory_.read_u16_le(a); }
    [[nodiscard]] std::uint32_t u32(const std::uint32_t a) const { return memory_.read_u32_le(a); }
    [[nodiscard]] float f32(const std::uint32_t a) const { return std::bit_cast<float>(memory_.read_u32_le(a)); }

private:
    const guest::EeMemory& memory_;
};

} // namespace

bool plan_draw_car_ee(const guest::EeMemory& memory, const std::uint32_t s, const std::uint32_t f, const std::uint32_t b,
                      const std::int32_t lod, CarDrawPlan& plan) {
    return plan_draw_car(EeMemoryReader{memory}, s, f, b, lod, plan);
}

} // namespace takedown::runtime::native_car
