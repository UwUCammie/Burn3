#include "takedown/presentation/gs_texture_residency.hpp"

#include "takedown/guest/gs_live_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace takedown::presentation {

namespace {

// Process-wide issuer for registry identities. Zero is reserved as invalid.
std::atomic<std::uint64_t> next_registry_identity{1U};

// Base-level GS dimensions are powers of two no larger than 2048 texels.
[[nodiscard]] constexpr bool is_supported_texture_dimension(const std::uint32_t value) noexcept {
    return value - 1U < 2048U && (value & (value - 1U)) == 0U;
}

} // namespace

// RECOVERY: the original keeps the texel_count and palette_bytes optionals in memory
// (their engaged flag and value are read back through the object); the rebuilt
// scalarizes them, so GIMPLE differs while the semantics are identical.
std::optional<std::size_t>
validated_gs_texture_image_payload_bytes(const guest::GsTextureImageSnapshot& snapshot) noexcept {
    if (!is_supported_texture_dimension(snapshot.width) ||
        !is_supported_texture_dimension(snapshot.height)) {
        return std::nullopt;
    }

    const std::optional<std::size_t> texel_count =
        static_cast<std::size_t>(snapshot.height) * static_cast<std::size_t>(snapshot.width);
    if (!texel_count) {
        return std::nullopt;
    }

    if (std::holds_alternative<DirectTexturePlane>(snapshot.plane)) {
        const auto& direct = std::get<DirectTexturePlane>(snapshot.plane);
        switch (snapshot.source_pixel_storage_mode) {
        case 0U:
        case 1U:
        case 50U:
        case 58U:
            if (direct.texels.size() != *texel_count) {
                return std::nullopt;
            }
            return direct.texels.capacity() * sizeof(Rgba8);
        default:
            return std::nullopt;
        }
    }

    if (std::holds_alternative<IndexedTexturePlane>(snapshot.plane)) {
        const auto& indexed = std::get<IndexedTexturePlane>(snapshot.plane);
        if (snapshot.source_pixel_storage_mode != 19U && snapshot.source_pixel_storage_mode != 20U &&
            snapshot.source_pixel_storage_mode != 27U) {
            return std::nullopt;
        }
        if (indexed.indices.size() != *texel_count) {
            return std::nullopt;
        }

        const auto expected_palette_size =
            snapshot.source_pixel_storage_mode == 20U ? 16ULL : 256ULL;
        if (indexed.effective_palette.size() != expected_palette_size) {
            return std::nullopt;
        }
        if (snapshot.source_pixel_storage_mode == 20U &&
            std::find_if(indexed.indices.begin(), indexed.indices.end(),
                         [](const std::uint8_t index) { return index > 15U; }) !=
                indexed.indices.end()) {
            return std::nullopt;
        }

        const std::optional<std::size_t> palette_bytes =
            indexed.effective_palette.capacity() * sizeof(Rgba8);
        if (*palette_bytes > std::numeric_limits<std::size_t>::max() - indexed.indices.capacity()) {
            return std::nullopt;
        }
        return *palette_bytes + indexed.indices.capacity();
    }

    return std::nullopt;
}

GsTextureResidencyRegistryIdentity GsTextureResidencyRegistry::mint_process_identity() {
    auto current = next_registry_identity.load(std::memory_order_relaxed);
    while (current != 0U) {
        const auto next = current != std::numeric_limits<std::uint64_t>::max() ? current + 1U : 0U;
        if (next_registry_identity.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
            return GsTextureResidencyRegistryIdentity{current};
        }
    }
    throw std::overflow_error("GS texture residency registry identity space exhausted");
}

GsTextureResidencyRegistry::GsTextureResidencyRegistry(const GsTextureResidencyRegistryConfig config)
    : identity_(mint_process_identity()), config_(config) {
    token_space_exhausted_ = config.maximum_token_value == 0U;
}

std::optional<GsTextureResidencyToken>
GsTextureResidencyRegistry::acquire(guest::GsTextureImageSnapshot snapshot) {
    return acquire(guest::GsTextureImageResidencyCapture(
        std::move(snapshot), std::nullopt, guest::GsLocalMemoryMutationCheckpoint{}, std::nullopt));
}

// RECOVERY: differs from the original in two GIMPLE details only: the payload optionals
// (see validated_gs_texture_image_payload_bytes) are scalarized here, and the original
// merges every early exit into one shared return/cleanup block, which is not reproduced.
std::optional<GsTextureResidencyToken>
GsTextureResidencyRegistry::acquire(guest::GsTextureImageResidencyCapture capture) {
    const auto payload_bytes = validated_gs_texture_image_payload_bytes(capture.image_);
    if (!payload_bytes) {
        return std::nullopt;
    }
    if (token_space_exhausted_) {
        return std::nullopt;
    }
    if (*payload_bytes > config_.maximum_resident_payload_bytes) {
        return std::nullopt;
    }
    if (resident_payload_bytes_ > config_.maximum_resident_payload_bytes - *payload_bytes) {
        return std::nullopt;
    }

    auto owned = std::make_shared<guest::GsTextureImageResidencyCapture>(std::move(capture));
    const auto token_value = next_token_value_;
    if (!entries_.try_emplace(token_value, Entry{std::move(owned), *payload_bytes}).second) {
        return std::nullopt;
    }

    resident_payload_bytes_ += *payload_bytes;
    if (token_value == config_.maximum_token_value) {
        token_space_exhausted_ = true;
    } else {
        ++next_token_value_;
    }
    return GsTextureResidencyToken(token_value, identity_);
}

// RECOVERY: the original reads the token's value field through memory (its address is
// taken by the validity check); the rebuilt reads it as a scalar. Semantics are identical.
std::shared_ptr<const guest::GsTextureImageSnapshot>
GsTextureResidencyRegistry::find(const GsTextureResidencyToken token) const noexcept {
    if (!token.valid() || token.registry_ != identity_) {
        return {};
    }
    const auto it = entries_.find(token.value());
    if (it == entries_.end()) {
        return {};
    }
    return std::shared_ptr<const guest::GsTextureImageSnapshot>(it->second.capture,
                                                               &it->second.capture->image_);
}

// RECOVERY: same token-field access difference as find(); semantics are identical.
std::optional<guest::GsTextureImageValidationResult>
GsTextureResidencyRegistry::validate_source(const GsTextureResidencyToken token, const std::uint64_t tex0,
                                            const guest::GsLiveRenderer& renderer) noexcept {
    if (!token.valid() || token.registry_ != identity_) {
        return std::nullopt;
    }
    const auto it = entries_.find(token.value());
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return renderer.validate_texture_image_for_residency(tex0, *it->second.capture);
}

// RECOVERY: same token-field access difference as find(); semantics are identical.
bool GsTextureResidencyRegistry::release(const GsTextureResidencyToken token) noexcept {
    if (!token.valid() || token.registry_ != identity_) {
        return false;
    }
    const auto it = entries_.find(token.value());
    if (it == entries_.end()) {
        return false;
    }
    resident_payload_bytes_ -= it->second.payload_bytes;
    entries_.erase(it);
    return true;
}

std::size_t GsTextureResidencyRegistry::size() const noexcept {
    return entries_.size();
}

std::size_t GsTextureResidencyRegistry::resident_payload_bytes() const noexcept {
    return resident_payload_bytes_;
}

} // namespace takedown::presentation
