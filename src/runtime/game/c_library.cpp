#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include "takedown/engine/util/bsd_qsort.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace takedown::runtime {

namespace {

using guest::GuestAddress;

// strcmp: the difference of the first differing bytes. The original walks the
// strings a byte at a time, so the cost grows with the length compared.
NativeFunctionExecution string_compare(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto& memory = call.memory();
    GuestAddress left = call.arg(0U);
    GuestAddress right = call.arg(1U);
    std::uint32_t length = 0U;
    while (memory.read_u8(left) != 0U && memory.read_u8(left) == memory.read_u8(right)) {
        ++left;
        ++right;
        ++length;
    }
    call.return_value(static_cast<std::uint32_t>(static_cast<int>(memory.read_u8(left)) - static_cast<int>(memory.read_u8(right))));
    return call.finish(length + 12U);
}

// The game's character class table (bit 0 set for upper case letters).
int to_lower(const guest::EeMemory& memory, const int character) {
    return (memory.read_u8(4928601U + static_cast<std::uint32_t>(character)) & 1U) != 0U ? character + 32 : character;
}

// strcasecmp: the bytes are folded to lower case before they are compared.
NativeFunctionExecution string_compare_ignoring_case(NativeFunctionContext& context) {
    GuestCall call{context};
    const auto& memory = call.memory();
    GuestAddress left = call.arg(0U);
    GuestAddress right = call.arg(1U);
    std::uint32_t length = 0U;
    while (memory.read_u8(left) != 0U && to_lower(memory, static_cast<std::int8_t>(memory.read_u8(left))) ==
               to_lower(memory, static_cast<std::int8_t>(memory.read_u8(right)))) {
        ++left;
        ++right;
        ++length;
    }
    call.return_value(static_cast<std::uint32_t>(to_lower(memory, memory.read_u8(left)) - to_lower(memory, memory.read_u8(right))));
    return call.finish(length * 15U + 16U);
}

// A zero divisor is left to the original code.
std::uint32_t unsigned_division_cost(const NativeFunctionPrepareContext& context) {
    return context.cpu.read_gpr_u64(5U) == 0U ? 0U : 90U;
}

NativeFunctionExecution unsigned_divide64(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_value64(call.arg64(0U) / call.arg64(1U));
    return call.finish(1U);
}

NativeFunctionExecution unsigned_remainder64(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_value64(call.arg64(0U) % call.arg64(1U));
    return call.finish(1U);
}

// The comparison a qsort caller hands over, by the address of the original's
// comparison function. Callers with any other comparison keep the original.
enum class SortKey {
    Unsupported,
    FloatAt0,
    FloatAt4,
    FloatAt4Descending,
    WordAt12,
    ByteAt9,
    FlaggedThenWords,
};

SortKey sort_key(const std::uint32_t comparison) {
    switch (comparison) {
    case 1808416U:
        return SortKey::FloatAt0;
    case 3182656U:
    case 3182704U:
        return SortKey::FloatAt4;
    case 1678736U:
        return SortKey::FloatAt4Descending;
    case 3182560U:
        return SortKey::WordAt12;
    case 3182608U:
        return SortKey::ByteAt9;
    case 1330720U:
        return SortKey::FlaggedThenWords;
    default:
        return SortKey::Unsupported;
    }
}

// The elements as they are in guest memory, or in a private copy of them
// (`base` is the guest address the copy starts at).
struct GuestElements {
    const guest::EeMemory& memory;
};

struct CopiedElements {
    const std::vector<std::uint8_t>& bytes;
    std::uint32_t base;
};

std::uint8_t read_byte(const GuestElements& elements, const GuestAddress address) {
    return elements.memory.read_u8(address);
}

std::uint8_t read_byte(const CopiedElements& elements, const GuestAddress address) {
    return elements.bytes[address - elements.base];
}

std::uint32_t read_word(const GuestElements& elements, const GuestAddress address) {
    return elements.memory.read_u32_le(address);
}

std::uint32_t read_word(const CopiedElements& elements, const GuestAddress address) {
    std::uint32_t value = 0U;
    for (std::uint32_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(elements.bytes[address - elements.base + byte]) << (byte * 8U);
    }
    return value;
}

float read_float(const GuestElements& elements, const GuestAddress address) {
    return std::bit_cast<float>(read_word(elements, address));
}

float read_float(const CopiedElements& elements, const GuestAddress address) {
    return std::bit_cast<float>(read_word(elements, address));
}

// The EE's float: denormals read as zero and infinities as the largest value.
float finite_float(const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000U) == 0U) {
        return std::bit_cast<float>(bits & 0x80000000U);
    }
    if ((bits & 0x7f800000U) == 0x7f800000U) {
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f7fffffU);
    }
    return value;
}

// -1 when `right` is the larger of the two, otherwise 1 (equal keys included).
int float_order(const float left, const float right) {
    const float lower = finite_float(left);
    const float upper = finite_float(right);
    return upper > lower ? -1 : 1;
}

// Records that compare equal are told apart as "after", as the original does.
int ordering(const bool before) {
    return before ? -1 : 1;
}

// RECOVERY: WordAt12 compare operand order and Copied loop block numbering differ from the original.
template <class Elements>
std::int32_t key_compare(const Elements& memory, const SortKey key, const GuestAddress a, const GuestAddress b) {
    switch (key) {
    case SortKey::FloatAt0: {
        const float right = read_float(memory, b);
        const float left = read_float(memory, a);
        return float_order(left, right);
    }
    case SortKey::FloatAt4:
        return float_order(read_float(memory, a + 4U), read_float(memory, b + 4U));
    case SortKey::FloatAt4Descending:
        return float_order(read_float(memory, b + 4U), read_float(memory, a + 4U));
    case SortKey::WordAt12:
    {
        const std::uint32_t left = read_word(memory, a + 12U);
        const std::uint32_t right = read_word(memory, b + 12U);
        return ordering(left < right);
    }
    case SortKey::ByteAt9:
        return ordering(static_cast<std::int8_t>(read_byte(memory, a + 9U)) < static_cast<std::int8_t>(read_byte(memory, b + 9U)));
    case SortKey::FlaggedThenWords: {
        const std::uint8_t left_flag = read_byte(memory, a + 8U);
        const std::uint8_t right_flag = read_byte(memory, b + 8U);
        if (left_flag != right_flag) {
            return left_flag == 0U ? 1 : -1;
        }
        const std::uint32_t left_high = read_word(memory, a + 4U);
        const std::uint32_t right_high = read_word(memory, b + 4U);
        if (left_high != right_high) {
            return static_cast<std::int32_t>(left_high - right_high);
        }
        const std::uint32_t left_low = read_word(memory, a);
        const std::uint32_t right_low = read_word(memory, b);
        return static_cast<std::int32_t>(left_low - right_low);
    }
    }
    return 0;
}

// Sorts `count` records of `size` bytes at `base` the way the C library does
// and reports how many comparisons and exchanges it took; `swap` exchanges
// `bytes` bytes at two addresses.
template <class Elements, class Swap>
engine::QsortCounts run_qsort(const Elements& elements, const SortKey key, const std::uint32_t base,
                              const std::uint32_t count, const std::uint32_t size, Swap&& swap) {
    engine::QsortCounts counts;
    auto compare = [&](const std::uint32_t a, const std::uint32_t b) { return key_compare(elements, key, a, b); };
    engine::bsd_qsort(base, count, size, compare, swap, counts);
    return counts;
}

void swap_copied_bytes(std::vector<std::uint8_t>& bytes, const GuestAddress base, const std::uint32_t a,
                       const std::uint32_t b, const std::uint32_t length) {
    for (std::uint32_t byte = 0U; byte < length; ++byte) {
        std::swap(bytes[a - base + byte], bytes[b - base + byte]);
    }
}

// The sort runs on a copy of the records, and the cost follows what the C
// library did: its comparisons, exchanges and the words moved.
// RECOVERY: the original inlines the first level of bsd_qsort here; this build keeps the call (not identical).
std::uint32_t qsort_cost(const NativeFunctionPrepareContext& context) {
    const guest::R5900State& cpu = context.cpu;
    const SortKey key = sort_key(cpu.read_gpr_u32(7U));
    const GuestAddress base = cpu.read_gpr_u32(4U);
    const std::uint32_t count = cpu.read_gpr_u32(5U);
    const std::uint32_t size = cpu.read_gpr_u32(6U);
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * size;
    if (key == SortKey::Unsupported || bytes > 1048576U) {
        return 0U;
    }
    std::vector<std::uint8_t> copy(bytes);
    for (std::uint32_t byte = 0U; byte < bytes; ++byte) {
        copy[byte] = context.memory.read_u8(base + byte);
    }
    const engine::QsortCounts counts = run_qsort(
        CopiedElements{copy, base}, key, base, count, size,
        [&copy, &base](const std::uint32_t a, const std::uint32_t b, const std::uint32_t length) {
            swap_copied_bytes(copy, base, a, b, length);
        });
    const std::uint32_t units = (base & 7U) == 0U && (size & 7U) == 0U ? counts.swapped_bytes / 8U : counts.swapped_bytes;
    return 40U + counts.comparisons * 20U + counts.swaps * 8U + units * 6U;
}

// RECOVERY: the original inlines the first level of bsd_qsort here; this build keeps the call (not identical).
NativeFunctionExecution sort(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    run_qsort(GuestElements{memory}, sort_key(call.arg(3U)), call.arg(0U), call.arg(1U), call.arg(2U),
              [&memory](const std::uint32_t a, const std::uint32_t b, const std::uint32_t bytes) {
                  for (std::uint32_t byte = 0U; byte < bytes; ++byte) {
                      const std::uint8_t left = memory.read_u8(a + byte);
                      memory.write_u8(a + byte, memory.read_u8(b + byte));
                      memory.write_u8(b + byte, left);
                  }
              });
    return call.finish(1U);
}

// Comparison functions the game hands to its sort, each on a pair of records.
template <SortKey Key>
NativeFunctionExecution compare_key(NativeFunctionContext& context) {
    GuestCall call{context};
    call.return_value(static_cast<std::uint32_t>(key_compare(GuestElements{call.memory()}, Key, call.arg(0U), call.arg(1U))));
    return call.finish(9U);
}

constexpr std::array<EngineFunction, 11U> kFunctions{{
    {
        .address = 1232192U,
        .code_bytes = 1404U,
        .code_fnv1a64 = 0xde570502461017d5ULL,
        .name = "__udivdi3",
        .cost = 200U,
        .implementation = unsigned_divide64,
        .call_cost = unsigned_division_cost,
    },
    {
        .address = 1233600U,
        .code_bytes = 1384U,
        .code_fnv1a64 = 0xb88a872891c28470ULL,
        .name = "__umoddi3",
        .cost = 200U,
        .implementation = unsigned_remainder64,
        .call_cost = unsigned_division_cost,
    },
    {
        .address = 1200952U,
        .code_bytes = 2540U,
        .code_fnv1a64 = 0x0f9e51565807c31aULL,
        .name = "qsort",
        .cost = 2000000U,
        .implementation = sort,
        .returns = EngineReturn::Nothing,
        .call_cost = qsort_cost,
    },
    {
        .address = 3182560U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0x2c6d21d48ab85536ULL,
        .name = "CompareKeyWord12",
        .cost = 20U,
        .implementation = compare_key<SortKey::WordAt12>,
    },
    {
        .address = 3182608U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0x64515202a3d555cfULL,
        .name = "CompareKeyByte9",
        .cost = 20U,
        .implementation = compare_key<SortKey::ByteAt9>,
    },
    {
        .address = 3182656U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0x3f5e2545f3e6aecaULL,
        .name = "CompareKeyFloat4",
        .cost = 20U,
        .implementation = compare_key<SortKey::FloatAt4>,
    },
    {
        .address = 1678736U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0xe8ba830c4bb030b9ULL,
        .name = "CompareKeyFloat4Descending",
        .cost = 20U,
        .implementation = compare_key<SortKey::FloatAt4Descending>,
    },
    {
        .address = 1330720U,
        .code_bytes = 80U,
        .code_fnv1a64 = 0x92ff625ca48cb721ULL,
        .name = "CompareFlaggedThenWords",
        .cost = 30U,
        .implementation = compare_key<SortKey::FlaggedThenWords>,
    },
    {
        .address = 3182704U,
        .code_bytes = 40U,
        .code_fnv1a64 = 0x3f5e2545f3e6aecaULL,
        .name = "CompareKeyFloat4Twin",
        .cost = 20U,
        .implementation = compare_key<SortKey::FloatAt4>,
    },
    {
        .address = 1208712U,
        .code_bytes = 324U,
        .code_fnv1a64 = 0x0b74b46f99ca716aULL,
        .name = "strcmp",
        .cost = 100000U,
        .implementation = string_compare,
    },
    {
        .address = 1207792U,
        .code_bytes = 216U,
        .code_fnv1a64 = 0x913ea6f9ed1a98c6ULL,
        .name = "strcasecmp",
        .cost = 100000U,
        .implementation = string_compare_ignoring_case,
    },
}};

} // namespace

std::span<const EngineFunction> c_library_engine_functions() {
    return kFunctions;
}

} // namespace takedown::runtime
