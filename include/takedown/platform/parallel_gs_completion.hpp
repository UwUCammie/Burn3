#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown {
namespace platform {

// Fixed-width, little-endian records exchanged by the optional
// paraLLEl-GS completion FIFO. The renderer owns the producer side and the
// recomp owns the consumer side. The renderer opens this FIFO for writing, so
// the consumer must create/open its read end before launching the renderer.
// Keeping the wire shape here avoids making either side infer completion from
// FIFO close timing or host frame pacing.
static constexpr std::uint32_t kParallelGsCompletionMagic = 0x33434750U; // "PGC3"
static constexpr std::uint16_t kParallelGsCompletionVersion = 1U;
static constexpr std::size_t kParallelGsCompletionRecordBytes = 64U;

enum class ParallelGsCompletionRecordType : std::uint16_t {
    Hello = 1U,
    // The renderer's ordered GPU submission fence has signalled. This proves
    // submitted GS/display commands through the associated raw VBlank have
    // completed on the GPU; it does not prove host presentation or scanout.
    Complete = 2U,
    Error = 3U,
    Eos = 4U,
};

enum class ParallelGsCompletionError : std::uint64_t {
    StreamTruncated = 1U,
    FenceUnavailable = 2U,
    OutputWrite = 3U,
    PendingOverflow = 4U,
};

static constexpr std::uint32_t kParallelGsCompletionEosClean = 1U << 0U;

// Values in this logical record are host-endian.  Use encode/decode below for
// the fixed little-endian wire representation.  The two reserved words keep
// the record 64 bytes so future readers can reject unknown versions without
// guessing field boundaries.
struct ParallelGsCompletionRecord {
    std::uint32_t magic = kParallelGsCompletionMagic;
    std::uint16_t version = kParallelGsCompletionVersion;
    ParallelGsCompletionRecordType type = ParallelGsCompletionRecordType::Hello;
    std::uint32_t size = static_cast<std::uint32_t>(kParallelGsCompletionRecordBytes);
    std::uint32_t flags = 0U;
    std::uint64_t session_epoch = 0U;
    std::uint64_t sequence = 0U;
    std::uint64_t raw_vsync_index = 0U;
    std::uint64_t value = 0U;
    std::uint64_t reserved0 = 0U;
    std::uint64_t reserved1 = 0U;
};

static_assert(sizeof(ParallelGsCompletionRecord) == 64U,
              "completion record fields must remain fixed-width");

using ParallelGsCompletionBytes =
    std::array<std::uint8_t, kParallelGsCompletionRecordBytes>;

inline void parallel_gs_completion_store_u16(std::uint8_t* const destination,
                                              const std::uint16_t value) noexcept {
    destination[0U] = static_cast<std::uint8_t>(value);
    destination[1U] = static_cast<std::uint8_t>(value >> 8U);
}

inline void parallel_gs_completion_store_u32(std::uint8_t* const destination,
                                              const std::uint32_t value) noexcept {
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        destination[byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

inline void parallel_gs_completion_store_u64(std::uint8_t* const destination,
                                              const std::uint64_t value) noexcept {
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        destination[byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

inline std::uint16_t parallel_gs_completion_load_u16(
    const std::uint8_t* const source) noexcept {
    return static_cast<std::uint16_t>(source[0U]) |
           (static_cast<std::uint16_t>(source[1U]) << 8U);
}

inline std::uint32_t parallel_gs_completion_load_u32(
    const std::uint8_t* const source) noexcept {
    std::uint32_t value = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(source[byte]) << (byte * 8U);
    }
    return value;
}

inline std::uint64_t parallel_gs_completion_load_u64(
    const std::uint8_t* const source) noexcept {
    std::uint64_t value = 0U;
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(source[byte]) << (byte * 8U);
    }
    return value;
}

inline ParallelGsCompletionBytes encode_parallel_gs_completion(
    const ParallelGsCompletionRecord& record) noexcept {
    ParallelGsCompletionBytes bytes{};
    parallel_gs_completion_store_u32(bytes.data() + 0U, record.magic);
    parallel_gs_completion_store_u16(bytes.data() + 4U, record.version);
    parallel_gs_completion_store_u16(bytes.data() + 6U,
                                     static_cast<std::uint16_t>(record.type));
    parallel_gs_completion_store_u32(bytes.data() + 8U, record.size);
    parallel_gs_completion_store_u32(bytes.data() + 12U, record.flags);
    parallel_gs_completion_store_u64(bytes.data() + 16U, record.session_epoch);
    parallel_gs_completion_store_u64(bytes.data() + 24U, record.sequence);
    parallel_gs_completion_store_u64(bytes.data() + 32U, record.raw_vsync_index);
    parallel_gs_completion_store_u64(bytes.data() + 40U, record.value);
    parallel_gs_completion_store_u64(bytes.data() + 48U, record.reserved0);
    parallel_gs_completion_store_u64(bytes.data() + 56U, record.reserved1);
    return bytes;
}

inline bool decode_parallel_gs_completion(
    const std::uint8_t* const bytes, const std::size_t size,
    ParallelGsCompletionRecord& record) noexcept {
    if (bytes == nullptr || size != kParallelGsCompletionRecordBytes) {
        return false;
    }

    record.magic = parallel_gs_completion_load_u32(bytes + 0U);
    record.version = parallel_gs_completion_load_u16(bytes + 4U);
    record.type = static_cast<ParallelGsCompletionRecordType>(
        parallel_gs_completion_load_u16(bytes + 6U));
    record.size = parallel_gs_completion_load_u32(bytes + 8U);
    record.flags = parallel_gs_completion_load_u32(bytes + 12U);
    record.session_epoch = parallel_gs_completion_load_u64(bytes + 16U);
    record.sequence = parallel_gs_completion_load_u64(bytes + 24U);
    record.raw_vsync_index = parallel_gs_completion_load_u64(bytes + 32U);
    record.value = parallel_gs_completion_load_u64(bytes + 40U);
    record.reserved0 = parallel_gs_completion_load_u64(bytes + 48U);
    record.reserved1 = parallel_gs_completion_load_u64(bytes + 56U);
    return record.magic == kParallelGsCompletionMagic &&
           record.version == kParallelGsCompletionVersion &&
           record.size == kParallelGsCompletionRecordBytes;
}

} // namespace platform
} // namespace takedown
