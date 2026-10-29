#include "takedown/engine/math/trigonometry.hpp"

#include "takedown/engine/math/chopped.hpp"

#include <cstdint>

namespace takedown::engine {

float sine_kernel(const float x, const float tail, const bool has_tail) noexcept {
    const std::uint32_t magnitude = std::bit_cast<std::uint32_t>(x) & 0x7fffffffU;
    // Below 2^-27 the sine is x itself.
    if (magnitude <= 0x31ffffffU && static_cast<int>(x) == 0) {
        return x;
    }
    const float z = x * x;
    const float v = z * x;
    const float r = ((((z * 1.58969088e-10F - 2.50507597e-8F) * z + 2.75573120e-6F) * z - 1.98412701e-4F) * z) + 8.33333377e-3F;
    if (!has_tail) {
        return (z * r - 1.66666657e-1F) * v + x;
    }
    return x - (((tail * 0.5F - v * r) * z - tail) + v * 1.66666657e-1F);
}

float wave_cosine(const float angle) noexcept {
    const float turns = std::fabs(angle) * 0.159154943F;
    const float x = std::fabs(turns - std::trunc(turns) - 0.5F) - 0.25F;
    const float x2 = x * x;
    const float x4 = x2 * x2;
    const float x8 = x4 * x4;
    float sum = x * -41.3416748F * x2;
    sum += x * -76.5749588F * x2 * x4;
    sum += x * 81.6022263F * x4;
    sum += x * 6.28318501F;
    return x * 39.7106590F * x8 + sum;
}

float wave_fold_chopped(const float angle) noexcept {
    constexpr float kRound = 12582912.0F; // 1.5 * 2^23
    const float whole = chop_add(chop_sub(chop_mul(std::fabs(angle), -0.159154943F), kRound), kRound);
    const float fraction = chop_sub(whole, chop_mul(std::fabs(angle), -0.159154943F));
    return chop_sub(std::fabs(chop_sub(fraction, 0.5F)), 0.25F);
}

// RECOVERY: the original keeps the 2*pi coefficient of the linear term as a
// separate constant operand in its GIMPLE; no spelling found that reproduces it.
float wave_cosine_chopped(const float angle) noexcept {
    const float x = wave_fold_chopped(angle);
    const float x2 = chop_mul(x, x);
    const float x4 = chop_mul(x2, x2);
    const float x8 = chop_mul(x4, x4);
    float sum = chop_mul(chop_mul(x, -41.3416748F), x2);
    sum = chop_madd(sum, chop_mul(chop_mul(x, -76.5749588F), x2), x4);
    sum = chop_madd(sum, chop_mul(x, 81.6022263F), x4);
    sum = chop_add(sum, chop_mul(x, 6.28318501F));
    sum = chop_madd(sum, chop_mul(x, 39.7106590F), x8);
    return sum;
}

} // namespace takedown::engine
