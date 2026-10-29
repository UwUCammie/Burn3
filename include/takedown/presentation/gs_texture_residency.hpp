#pragma once

#include "takedown/guest/gs_texture_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>

namespace takedown::presentation {

// Validates a complete canonical image and returns the retained allocation
// capacity of its texel/index and effective-palette payload. This is the common
// shape and accounting contract used by both the residency registry and queued
// renderer commands which retain an image independently of that registry.
[[nodiscard]] std::optional<std::size_t>
validated_gs_texture_image_payload_bytes(const guest::GsTextureImageSnapshot& snapshot) noexcept;

// Opaque process-local identity for one texture-residency registry. Callers
// can construct only the invalid zero identity; valid registry identities are
// issued monotonically and are never reused, including after destruction.
class GsTextureResidencyRegistryIdentity {
  public:
    constexpr GsTextureResidencyRegistryIdentity() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0U; }

    bool operator==(const GsTextureResidencyRegistryIdentity&) const = default;

  private:
    friend class GsTextureResidencyRegistry;

    explicit constexpr GsTextureResidencyRegistryIdentity(const std::uint64_t value) noexcept
        : value_(value) {}

    std::uint64_t value_{};
};

// Process-local capability for one registry-owned immutable texture image. Its
// diagnostic value remains monotonic within the issuing registry, while the
// issuer identity and constructor stay private. A caller can retain or return
// an issued token but cannot retarget it to another entry in the same registry.
class GsTextureResidencyToken final {
  public:
    constexpr GsTextureResidencyToken() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value_ != 0U && registry_.valid();
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    bool operator==(const GsTextureResidencyToken&) const = default;

  private:
    friend class GsTextureResidencyRegistry;

    constexpr GsTextureResidencyToken(const std::uint64_t value,
                                      const GsTextureResidencyRegistryIdentity registry) noexcept
        : value_(value), registry_(registry) {}

    std::uint64_t value_{};
    GsTextureResidencyRegistryIdentity registry_{};
};

struct GsTextureResidencyRegistryConfig {
    // Counts retained vector allocation capacity: direct texels, or indexed
    // texels plus their effective palette. Accounting capacity rather than
    // logical length prevents a small image with oversized reserved storage
    // from bypassing the bound. The registry accepts an image which lands
    // exactly on this limit.
    std::size_t maximum_resident_payload_bytes{64U * 1024U * 1024U};

    // Testable exhaustion boundary. Zero disables all acquisitions; otherwise
    // tokens are issued monotonically from one through this value.
    std::uint64_t maximum_token_value{std::numeric_limits<std::uint64_t>::max()};
};

// Single-owner registry for immutable renderer-neutral texture snapshots.
// The intended owner is the ordered GS worker thread; this class performs no
// internal locking. Every successful acquisition receives a fresh token even
// when its content equals an existing image. Content deduplication is a later
// optimization and is deliberately absent from this correctness checkpoint.
class GsTextureResidencyRegistry {
  public:
    // Construction reserves one process-wide nonzero issuer identity. If all
    // uint64_t identities have ever been issued, construction throws
    // std::overflow_error and the exhausted identity space remains exhausted.
    explicit GsTextureResidencyRegistry(GsTextureResidencyRegistryConfig config = {});

    GsTextureResidencyRegistry(const GsTextureResidencyRegistry&) = delete;
    GsTextureResidencyRegistry& operator=(const GsTextureResidencyRegistry&) = delete;
    GsTextureResidencyRegistry(GsTextureResidencyRegistry&&) = delete;
    GsTextureResidencyRegistry& operator=(GsTextureResidencyRegistry&&) = delete;

    // Validates the complete snapshot before allocating or consuming a token.
    // Malformed shapes, payload-cap overflow, and token exhaustion reject
    // atomically. The registry strongly owns an accepted snapshot until its
    // token is explicitly released.
    [[nodiscard]] std::optional<GsTextureResidencyToken>
    acquire(guest::GsTextureImageSnapshot snapshot);
    [[nodiscard]] std::optional<GsTextureResidencyToken>
    acquire(guest::GsTextureImageResidencyCapture capture);

    [[nodiscard]] std::shared_ptr<const guest::GsTextureImageSnapshot>
    find(GsTextureResidencyToken token) const noexcept;

    // Validates and refreshes the sealed source proof in place. Missing or
    // foreign capabilities return no result; a present false result means the
    // current semantic image differs from the immutable resident generation.
    [[nodiscard]] std::optional<guest::GsTextureImageValidationResult>
    validate_source(GsTextureResidencyToken token, std::uint64_t tex0,
                    const guest::GsLiveRenderer& renderer) noexcept;

    // Drops the registry's strong ownership and payload accounting for an
    // existing token. External shared pointers returned by find() remain valid.
    [[nodiscard]] bool release(GsTextureResidencyToken token) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t resident_payload_bytes() const noexcept;

  private:
    struct Entry {
        std::shared_ptr<guest::GsTextureImageResidencyCapture> capture;
        std::size_t payload_bytes{};
    };

    [[nodiscard]] static GsTextureResidencyRegistryIdentity mint_process_identity();

    GsTextureResidencyRegistryIdentity identity_{};
    GsTextureResidencyRegistryConfig config_{};
    std::map<std::uint64_t, Entry> entries_{};
    std::size_t resident_payload_bytes_{};
    std::uint64_t next_token_value_{1U};
    bool token_space_exhausted_{};
};

} // namespace takedown::presentation
