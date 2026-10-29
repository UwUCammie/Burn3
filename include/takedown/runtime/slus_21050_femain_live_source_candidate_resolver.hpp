#pragma once

#include "takedown/guest/dmac_source_chain.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace takedown::runtime {

// This resolver is deliberately value-only.  It derives the one candidate
// source word which a live FEMain publication can offer to EeMemory's
// existing timed pre-snapshot resolver; it does not register an observer,
// call the source producer, mutate guest memory, or create authority.
enum class Slus21050FemainLiveSourceCandidateResolverFailure : std::uint8_t {
    None,
    Disabled,
    PublicationIdentityInvalid,
    PublicationPlanInvalid,
    PublicationQueueInvalid,
    PublicationQueueChanged,
    QueueAddressOverflow,
    QueueRootInvalid,
    QueueRootOutOfRange,
    QueueRootAliased,
    GuestMutationUnavailable,
    GuestMemoryChanged,
    ChainWalkRejected,
    ChainWalkAmbiguous,
    FirstRefMissing,
    CandidateAmbiguous,
    CandidateUnaligned,
    CandidateOutOfRange,
    CandidateWrongBuffer,
    CandidateAliased,
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainLiveSourceCandidateResolverFailure failure) noexcept;

// The candidate is a source-chain identity, rather than an authority token.
// It remains eligible only after the existing EeMemory pre-snapshot resolver
// returns a matching Vif1DmacPreSnapshotResolvedMarker.  The accepted queue
// shape is exactly one REF at chain_root followed by one zero-QWC END at the
// published queue end.  The source and terminal ordinals/locations are kept
// in the value so a later handoff cannot discard that topology proof.
struct Slus21050FemainLiveSourceCandidate {
    guest::GuestAddress source{};
    std::uint32_t chain_root{};
    std::uint32_t source_tag_location{};
    std::uint32_t source_tag_ordinal{};
    std::uint32_t terminal_tag_location{};
    std::uint32_t terminal_tag_ordinal{};
    guest::GsQuadword source_tag_words{};
    guest::GsQuadword terminal_tag_words{};
    std::uint32_t source_payload_word_ordinal{};
    std::uint32_t source_qwc{};
    guest::GsQuadword expected_quadword{};
    std::uint64_t publication_serial{};
    std::uint64_t player_session_generation{};
    std::uint64_t frame_generation{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainLiveSourceCandidate&) const = default;
    [[nodiscard]] bool valid() const noexcept;
};

// Resolver-backed source completion proof.  The candidate is retained in its
// complete authenticated form; the queue snapshot binds the proof to the
// exact publication which supplied the chain, while the span fields prove
// that the producer compared every REF payload quadword before completion.
struct Slus21050FemainLiveResolverSourceProof {
    Slus21050FemainLiveSourceCandidate candidate{};
    Slus21050FemainMoviePublicationQueueSnapshot publication_queue{};
    std::uint32_t matched_source_qwc{};
    std::uint64_t matched_span_digest{};

    bool operator==(const Slus21050FemainLiveResolverSourceProof&) const = default;
};

struct Slus21050FemainLiveSourceCandidateResolverResult {
    Slus21050FemainLiveSourceCandidateResolverFailure failure{
        Slus21050FemainLiveSourceCandidateResolverFailure::Disabled};
    std::optional<Slus21050FemainLiveSourceCandidate> candidate{};

    [[nodiscard]] bool resolved() const noexcept {
        return failure == Slus21050FemainLiveSourceCandidateResolverFailure::None &&
               candidate.has_value() && candidate->valid();
    }
};

class Slus21050FemainLiveSourceCandidateResolver final {
  public:
    explicit Slus21050FemainLiveSourceCandidateResolver(bool enabled = false) noexcept
        : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // The packet must already have crossed the owner/ledger publication seam.
    // The resolver rechecks the immutable queue snapshot against the live
    // queue fields, walks queue_read + 0x80 through the shared bounded DMAC
    // walker, and accepts exactly one REF payload fully contained by one
    // published FEMain buffer.  Any stale, malformed, aliased, cyclic, or
    // ambiguous state fails closed.
    [[nodiscard]] Slus21050FemainLiveSourceCandidateResolverResult
    resolve(const guest::EeMemory& memory,
            const Slus21050FemainMoviePublicationPacket& publication) const noexcept;

  private:
    bool enabled_{};
};

// EeMemory's pre-snapshot resolver is the authoritative confirmation step.
// This pure gate is useful to callers which keep the derived candidate in a
// value-only handoff: it accepts only the exact marker identity emitted by
// the shared walk and rejects stale, aliased, or cross-chain markers.
[[nodiscard]] bool confirm_slus_21050_femain_live_source_candidate(
    const Slus21050FemainLiveSourceCandidate& candidate,
    const guest::Vif1DmacPreSnapshotResolvedMarker& marker) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_live_source_candidate(
    const Slus21050FemainLiveSourceCandidate& candidate) noexcept;

[[nodiscard]] std::uint64_t digest_slus_21050_femain_live_source_span(
    std::span<const guest::GsQuadword> span) noexcept;

[[nodiscard]] bool validate_slus_21050_femain_live_resolver_source_proof(
    const Slus21050FemainLiveResolverSourceProof& proof) noexcept;

} // namespace takedown::runtime
