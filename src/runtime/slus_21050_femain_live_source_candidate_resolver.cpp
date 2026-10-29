#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace takedown::runtime {
namespace {

using Candidate = Slus21050FemainLiveSourceCandidate;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::string_view kCandidateDomain = "slus-21050-femain-live-source-candidate-v1";
constexpr std::string_view kSpanDomain = "slus-21050-femain-live-source-span-v1";

void mix_word(std::uint64_t& hash, const std::uint64_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void mix_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::size_t shift = 0U; shift != 64U; shift += 8U) {
        mix_word(hash, (value >> shift) & 255U);
    }
}

void mix_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    for (std::size_t shift = 0U; shift != 32U; shift += 8U) {
        mix_word(hash, (value >> shift) & 255U);
    }
}

void mix_string(std::uint64_t& hash, const std::string_view string) noexcept {
    mix_u64(hash, string.size());
    for (const char character : string) {
        mix_word(hash, static_cast<std::uint8_t>(character));
    }
}

[[nodiscard]] bool contains_span(const guest::EeMemory& memory, const guest::GuestAddress range_begin,
                                 const std::size_t range_bytes, const guest::GuestAddress address,
                                 const std::size_t address_bytes) noexcept {
    if (range_bytes != 0U && memory.is_main_ram_range(range_begin, range_bytes) && address_bytes != 0U &&
        memory.is_main_ram_range(address, address_bytes)) {
        const auto last = static_cast<std::uint64_t>(address) + address_bytes - 1U;
        if (last > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        return memory.main_ram_ranges_overlap(range_begin, range_bytes, address, 1U) &&
               memory.main_ram_ranges_overlap(range_begin, range_bytes, static_cast<guest::GuestAddress>(last),
                                              1U);
    }
    return false;
}

[[nodiscard]] std::uint64_t authenticate_candidate(const Slus21050FemainLiveSourceCandidate& candidate) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    mix_string(hash, kCandidateDomain);
    mix_u32(hash, candidate.source);
    mix_u32(hash, candidate.chain_root);
    mix_u32(hash, candidate.source_tag_location);
    mix_u32(hash, candidate.source_tag_ordinal);
    mix_u32(hash, candidate.terminal_tag_location);
    mix_u32(hash, candidate.terminal_tag_ordinal);
    for (const auto word : candidate.source_tag_words) {
        mix_u32(hash, word);
    }
    for (const auto word : candidate.terminal_tag_words) {
        mix_u32(hash, word);
    }
    mix_u32(hash, candidate.source_payload_word_ordinal);
    mix_u32(hash, candidate.source_qwc);
    for (const auto word : candidate.expected_quadword) {
        mix_u32(hash, word);
    }
    mix_u64(hash, candidate.publication_serial);
    mix_u64(hash, candidate.player_session_generation);
    mix_u64(hash, candidate.frame_generation);
    return hash != 0U ? hash : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] bool queue_ends_at_root(const std::uint32_t queue_end,
                                      const std::uint32_t chain_root) noexcept {
    const auto root_end = static_cast<std::uint64_t>(chain_root) + 16U;
    return static_cast<std::uint64_t>(queue_end) == root_end;
}

constexpr guest::GuestAddress kControlBase = 0x004e27f8U;
constexpr std::size_t kControlBytes = 72U;
constexpr guest::GuestAddress kControlSlots = 0x004e27f8U;
constexpr std::size_t kControlSlotBytes = 8U;
constexpr guest::GuestAddress kQueueBaseAddress = 0x004e2800U;
constexpr guest::GuestAddress kBufferIndexAddress = 0x004e2804U;
constexpr guest::GuestAddress kTransfersActiveAddress = 0x004e2830U;
constexpr guest::GuestAddress kQueueReadAddress = 0x004e2838U;
constexpr guest::GuestAddress kQueueEndAddress = 0x004e283cU;

using Failure = Slus21050FemainLiveSourceCandidateResolverFailure;
using Result = Slus21050FemainLiveSourceCandidateResolverResult;

[[nodiscard]] Result reject(const Failure failure) noexcept {
    return Result{failure, std::nullopt};
}

[[nodiscard]] bool publication_identity_valid(const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    const auto& reservation = packet.published_reservation;
    const auto& plan = packet.plan;
    if (packet.owner_identity == 0U || packet.publication_serial == 0U ||
        packet.player_identity == 0U || packet.player_session_generation == 0U ||
        packet.ledger_identity == 0U ||
        packet.ledger_session_generation != packet.player_session_generation ||
        reservation.ledger_identity != packet.ledger_identity ||
        reservation.session_generation != packet.ledger_session_generation ||
        reservation.frame_generation == 0U || reservation.slot >= kSlus21050FemainFrameTransactionSlotCount ||
        plan.layout_generation == 0U || plan.first_buffer == 0U || plan.second_buffer == 0U ||
        plan.buffer_bytes == 0U || plan.frame_generation == 0U || plan.payload_digest == 0U ||
        reservation.frame_generation != plan.frame_generation ||
        plan.media_ordinal != reservation.frame.media_ordinal ||
        plan.source_sequence != reservation.frame.source_sequence ||
        plan.source_checksum != reservation.frame.source_checksum || !plan.player_lease.has_value() ||
        plan.player_lease->session_generation == 0U || plan.player_lease->lease_generation == 0U ||
        packet.player_session_generation != plan.player_lease->session_generation) {
        return false;
    }
    return plan.timeline_ordinal.has_value();
}

[[nodiscard]] bool plan_layout_valid(const guest::EeMemory& memory,
                                     const Slus21050FemainMoviePublicationPlanIdentity& plan) noexcept {
    if (static_cast<std::uint64_t>(plan.second_buffer) < plan.buffer_bytes ||
        static_cast<std::uint64_t>(plan.first_buffer) !=
            static_cast<std::uint64_t>(plan.second_buffer) - plan.buffer_bytes ||
        (plan.first_buffer & 15U) != 0U || (plan.second_buffer & 15U) != 0U ||
        !contains_span(memory, plan.first_buffer, plan.buffer_bytes, plan.first_buffer, plan.buffer_bytes) ||
        !contains_span(memory, plan.second_buffer, plan.buffer_bytes, plan.second_buffer,
                       plan.buffer_bytes) ||
        memory.main_ram_ranges_overlap(plan.first_buffer, plan.buffer_bytes, plan.second_buffer,
                                       plan.buffer_bytes)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool queue_fields_present(const Slus21050FemainMoviePublicationQueueSnapshot& queue) noexcept {
    if (queue.queue_read == 0U || queue.queue_end == 0U || queue.queue_base == 0U ||
        queue.buffer_index > 1U || queue.transfers_active > 1U) {
        return false;
    }
    return true;
}

[[nodiscard]] bool queue_in_main_ram(const guest::EeMemory& memory,
                                     const Slus21050FemainMoviePublicationQueueSnapshot& queue) noexcept {
    if (queue.queue_read == 0U || queue.queue_end == 0U || queue.queue_base == 0U ||
        queue.buffer_index > 1U || queue.transfers_active > 1U || (queue.queue_read & 15U) != 0U ||
        (queue.queue_end & 15U) != 0U || queue.queue_read >= queue.queue_end) {
        return false;
    }
    const unsigned long long queue_bytes = static_cast<unsigned long long>(queue.queue_end) - queue.queue_read;
    const unsigned long long authenticated_queue_bytes = queue_bytes + 16U;
    return queue_bytes >= 16U && queue_bytes <= authenticated_queue_bytes &&
           memory.is_main_ram_range(queue.queue_read, authenticated_queue_bytes);
}

[[nodiscard]] guest::GuestAddress
active_queue_slot(const std::uint8_t buffer_index) noexcept {
    const auto active_queue_index = static_cast<std::uint8_t>(buffer_index ^ 1U);
    return kControlSlots + static_cast<guest::GuestAddress>(active_queue_index) * 4U;
}

[[nodiscard]] bool live_queue_matches(const guest::EeMemory& memory,
                                      const Slus21050FemainMoviePublicationPacket& packet,
                                      const std::uint32_t active_queue_read,
                                      const std::uint32_t live_queue_base,
                                      const std::uint8_t live_buffer_index,
                                      const std::uint8_t live_transfers_active,
                                      const std::uint32_t live_queue_end,
                                      const std::uint32_t live_queue_read) noexcept {
    const auto& queue = packet.queue;
    if (queue.queue_read == 0U || queue.queue_end == 0U || queue.queue_base == 0U ||
        queue.buffer_index > 1U || queue.transfers_active > 1U || (queue.queue_read & 15U) != 0U ||
        (queue.queue_end & 15U) != 0U || queue.queue_read >= queue.queue_end ||
        live_queue_read != queue.queue_read || live_queue_end != queue.queue_end ||
        live_transfers_active != queue.transfers_active || live_buffer_index != queue.buffer_index ||
        live_queue_base != queue.queue_base || active_queue_read != queue.queue_read) {
        return false;
    }
    return memory.is_main_ram_range(queue.queue_read, 16U) &&
           memory.is_main_ram_range(queue.queue_end - 16U, 16U) &&
           memory.is_main_ram_range(queue.queue_base, 4U) &&
           memory.is_main_ram_range(kControlSlots, kControlSlotBytes);
}

// One mutation token per guest range whose contents the proof depends on:
// the control block, the queued chain, and both movie buffers.
struct NarrowMutationProof {
    std::array<guest::EeMainRamMutationToken, 3> tokens{};

    [[nodiscard]] bool capture(const guest::EeMemory& memory,
                               const Slus21050FemainMoviePublicationPlanIdentity& plan,
                               const std::uint32_t queue_read, const std::size_t queue_bytes) noexcept {
        if (!memory.is_main_ram_range(kControlBase, kControlBytes) ||
            !memory.is_main_ram_range(queue_read, queue_bytes)) {
            return false;
        }
        const auto control_capture = memory.capture_main_ram_mutation(kControlBase, kControlBytes);
        const auto queue_capture = memory.capture_main_ram_mutation(queue_read, queue_bytes);
        const auto buffers_capture =
            memory.capture_main_ram_mutation(plan.first_buffer, plan.buffer_bytes * 2U);
        if (control_capture.status != guest::EeMainRamMutationCaptureStatus::Captured ||
            queue_capture.status != guest::EeMainRamMutationCaptureStatus::Captured ||
            buffers_capture.status != guest::EeMainRamMutationCaptureStatus::Captured) {
            return false;
        }
        tokens = {control_capture.token, queue_capture.token, buffers_capture.token};
        return true;
    }

    [[nodiscard]] bool validate(const guest::EeMemory& memory,
                                const Slus21050FemainMoviePublicationPlanIdentity& plan,
                                const std::uint32_t queue_read, const std::size_t queue_bytes) const noexcept {
        return memory.validate_main_ram_mutation(tokens[0], kControlBase, kControlBytes) &&
               memory.validate_main_ram_mutation(tokens[1], queue_read, queue_bytes) &&
               memory.validate_main_ram_mutation(tokens[2], plan.first_buffer, plan.buffer_bytes * 2U);
    }
};

// Keeps the walker's reads and every tag payload inside the published queue
// or one of the two movie buffers.
struct SourceRangeGuard {
    const guest::EeMemory& memory;
    const Slus21050FemainMoviePublicationPlanIdentity& plan;
    std::uint32_t queue_read{};
    std::size_t queue_bytes{};
    guest::DmacSourceTag current_tag{};
    bool reading_payload{};
    bool range_rejected{};

    [[nodiscard]] bool tag_payload_allowed(const guest::DmacSourceTag& tag) const noexcept {
        if (tag.qwc == 0U) {
            return true;
        }
        const auto bytes = static_cast<std::uint64_t>(tag.qwc) * 16U;
        const guest::GuestAddress address = tag.payload.address();
        if (tag.id == 1U || tag.id == 2U || tag.id == 5U || tag.id == 6U || tag.id == 7U) {
            return contains_span(memory, queue_read, queue_bytes, address, bytes);
        }
        return contains_span(memory, queue_read, queue_bytes, address, bytes) ||
               contains_span(memory, plan.first_buffer, plan.buffer_bytes, address, bytes) ||
               contains_span(memory, plan.second_buffer, plan.buffer_bytes, address, bytes);
    }

    void begin_tag(const guest::DmacSourceTag& tag) noexcept {
        current_tag = tag;
        reading_payload = true;
        if (!tag_payload_allowed(tag)) {
            range_rejected = true;
        }
    }

    [[nodiscard]] bool quadword_allowed(const guest::DmacSourceAddress address) const noexcept {
        if (range_rejected || address.scratchpad()) {
            return false;
        }
        if (reading_payload) {
            return contains_span(memory, current_tag.payload.address(),
                                 static_cast<std::size_t>(current_tag.qwc) * 16U, address.address(), 16U);
        }
        return contains_span(memory, queue_read, queue_bytes, address.address(), 16U);
    }
};

[[nodiscard]] inline guest::GsQuadword read_quadword(const guest::EeMemory& memory,
                                              const guest::GuestAddress address) {
    return guest::GsQuadword{memory.read_u32_le(address), memory.read_u32_le(address + 4U),
                             memory.read_u32_le(address + 8U), memory.read_u32_le(address + 12U)};
}

[[nodiscard]] bool ref_payload_in_one_buffer(const guest::EeMemory& memory,
                                             const Slus21050FemainMoviePublicationPlanIdentity& plan,
                                             const guest::GuestAddress address,
                                             const std::uint32_t qwc) noexcept {
    if (address == 0U || (address & 15U) != 0U || qwc == 0U) {
        return false;
    }
    const auto bytes = static_cast<std::uint64_t>(qwc) * 16U;
    const bool in_first = contains_span(memory, plan.first_buffer, plan.buffer_bytes, address, bytes);
    const bool in_second = contains_span(memory, plan.second_buffer, plan.buffer_bytes, address, bytes);
    return in_first != in_second;
}

} // namespace

std::string_view to_string(const Slus21050FemainLiveSourceCandidateResolverFailure failure) noexcept {
    using Failure = Slus21050FemainLiveSourceCandidateResolverFailure;
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::Disabled:
        return "disabled";
    case Failure::PublicationIdentityInvalid:
        return "publication-identity-invalid";
    case Failure::PublicationPlanInvalid:
        return "publication-plan-invalid";
    case Failure::PublicationQueueInvalid:
        return "publication-queue-invalid";
    case Failure::PublicationQueueChanged:
        return "publication-queue-changed";
    case Failure::QueueAddressOverflow:
        return "queue-address-overflow";
    case Failure::QueueRootInvalid:
        return "queue-root-invalid";
    case Failure::QueueRootOutOfRange:
        return "queue-root-out-of-range";
    case Failure::QueueRootAliased:
        return "queue-root-aliased";
    case Failure::GuestMutationUnavailable:
        return "guest-mutation-unavailable";
    case Failure::GuestMemoryChanged:
        return "guest-memory-changed";
    case Failure::ChainWalkRejected:
        return "chain-walk-rejected";
    case Failure::ChainWalkAmbiguous:
        return "chain-walk-ambiguous";
    case Failure::FirstRefMissing:
        return "first-ref-missing";
    case Failure::CandidateAmbiguous:
        return "candidate-ambiguous";
    case Failure::CandidateUnaligned:
        return "candidate-unaligned";
    case Failure::CandidateOutOfRange:
        return "candidate-out-of-range";
    case Failure::CandidateWrongBuffer:
        return "candidate-wrong-buffer";
    case Failure::CandidateAliased:
        return "candidate-aliased";
    default:
        return "unknown";
    }
}

bool Slus21050FemainLiveSourceCandidate::valid() const noexcept {
    return source != 0U && (source & 15U) == 0U && chain_root != 0U && source_tag_location != 0U &&
           chain_root == source_tag_location && source_tag_ordinal == 0U &&
           chain_root <= 0xffffffefU && terminal_tag_location != 0U &&
           terminal_tag_location == chain_root + 16U && terminal_tag_ordinal == 1U &&
           (source_tag_words[0] & 0x70000000U) == 0x30000000U &&
           (source_tag_words[0] & 0xffffU) == source_qwc && source_tag_words[1] == source &&
           (terminal_tag_words[0] & 0x70000000U) == 0x70000000U &&
           (terminal_tag_words[0] & 0xffffU) == 0U && source_qwc != 0U && publication_serial != 0U &&
           player_session_generation != 0U && frame_generation != 0U && authentication_tag != 0U;
}

std::uint64_t authenticate_slus_21050_femain_live_source_candidate(
    const Slus21050FemainLiveSourceCandidate& candidate) noexcept {
    return authenticate_candidate(candidate);
}

std::uint64_t digest_slus_21050_femain_live_source_span(
    const std::span<const guest::GsQuadword> span) noexcept {
    if (span.empty()) {
        return 0U;
    }
    std::uint64_t hash = kFnvOffsetBasis;
    mix_string(hash, kSpanDomain);
    mix_u64(hash, span.size());
    for (const guest::GsQuadword& quadword : span) {
        for (const auto word : quadword) {
            mix_u32(hash, word);
        }
    }
    return hash == 0U ? 1U : hash;
}

bool validate_slus_21050_femain_live_resolver_source_proof(
    const Slus21050FemainLiveResolverSourceProof& proof) noexcept {
    const auto& candidate = proof.candidate;
    const auto& queue = proof.publication_queue;
    if (!candidate.valid() || candidate.authentication_tag != authenticate_candidate(candidate) ||
        queue.queue_read == 0U || queue.queue_end == 0U || queue.queue_base == 0U ||
        queue.queue_read >= queue.queue_end || (queue.queue_read & 15U) != 0U ||
        (queue.queue_end & 15U) != 0U || queue.buffer_index > 1U || queue.transfers_active > 1U ||
        queue.queue_read > 0xffffff7fU || candidate.chain_root != queue.queue_read + 0x80U ||
        candidate.terminal_tag_location != queue.queue_end || candidate.terminal_tag_ordinal != 1U ||
        proof.matched_source_qwc == 0U || proof.matched_source_qwc != candidate.source_qwc ||
        proof.matched_span_digest == 0U) {
        return false;
    }
    return queue_ends_at_root(queue.queue_end, candidate.chain_root);
}


// RECOVERY: GIMPLE of the .part.0 clone differs from the original in two places only:
// root_end is folded to queue_read + 144 instead of chain_root + 16, and the four
// trailing is_main_ram_range branch probabilities of the live-queue check are 33/67
// instead of 50/50.  The control flow, calls and failure codes are identical.
Slus21050FemainLiveSourceCandidateResolverResult Slus21050FemainLiveSourceCandidateResolver::resolve(
    const guest::EeMemory& memory, const Slus21050FemainMoviePublicationPacket& publication) const noexcept {
    if (!enabled_) {
        return reject(Failure::Disabled);
    }
    if (!publication_identity_valid(publication)) {
        return reject(Failure::PublicationIdentityInvalid);
    }
    if (!plan_layout_valid(memory, publication.plan)) {
        return reject(Failure::PublicationPlanInvalid);
    }
    if (!memory.is_main_ram_range(kControlBase, kControlBytes) ||
        !queue_in_main_ram(memory, publication.queue)) {
        return reject(Failure::PublicationQueueInvalid);
    }
    if (publication.queue.queue_read > 0xffffff7fU) {
        return reject(Failure::QueueAddressOverflow);
    }
    const unsigned int chain_root = publication.queue.queue_read + 0x80U;
    if ((publication.queue.queue_read & 15U) != 0U || (chain_root & 0x80000000U) != 0U) {
        return reject(Failure::QueueRootAliased);
    }
    if (!memory.is_main_ram_range(chain_root, 16U)) {
        return reject(Failure::QueueRootOutOfRange);
    }
    if (memory.main_ram_ranges_overlap(chain_root, 16U, publication.plan.first_buffer, publication.plan.buffer_bytes) ||
        memory.main_ram_ranges_overlap(chain_root, 16U, publication.plan.second_buffer, publication.plan.buffer_bytes)) {
        return reject(Failure::QueueRootInvalid);
    }
    const unsigned long long queue_end = publication.queue.queue_end;
    const unsigned long long authenticated_queue_end = queue_end + 16U;
    const unsigned long long queue_bytes = authenticated_queue_end - publication.queue.queue_read;
    const unsigned long long root_end = chain_root + 16U;
    if (authenticated_queue_end < queue_end || authenticated_queue_end < root_end) {
        return reject(Failure::QueueRootInvalid);
    }
    NarrowMutationProof mutation{};
    if (!mutation.capture(memory, publication.plan, publication.queue.queue_read, queue_bytes)) {
        return reject(Failure::GuestMutationUnavailable);
    }
    SourceRangeGuard source_ranges{memory, publication.plan, publication.queue.queue_read, queue_bytes};
    if (!live_queue_matches(
            memory, publication,
            memory.read_u32_le(active_queue_slot(publication.queue.buffer_index)),
            memory.read_u32_le(kQueueBaseAddress), memory.read_u8(kBufferIndexAddress),
            memory.read_u8(kTransfersActiveAddress), memory.read_u32_le(kQueueEndAddress),
            memory.read_u32_le(kQueueReadAddress))) {
        const auto& queue = publication.queue;
        if (queue.queue_read == 0U || queue.queue_end == 0U || queue.queue_base == 0U ||
            queue.buffer_index > 1U || queue.transfers_active > 1U) {
            return reject(Failure::PublicationQueueInvalid);
        }
        return reject(Failure::PublicationQueueChanged);
    }

    std::optional<guest::DmacSourceTag> candidate_tag{};
    std::optional<guest::DmacSourceTag> terminal_tag{};
    bool source_shape_valid = true;
    bool terminal_shape_valid = false;
    std::uint32_t candidate_tag_ordinal = 0U;
    std::uint32_t terminal_tag_ordinal = 0U;
    std::uint32_t tag_ordinal = 0U;
    std::size_t candidate_count = 0U;

    const guest::DmacSourceResult result = guest::walk_dmac_source_chain(
        guest::DmacSourceCursor{.tadr = {chain_root}}, guest::DmacSourceOptions{},
        guest::DmacSourceCallbacks{
            .read_quadword =
                [&memory, &source_ranges](const guest::DmacSourceAddress address)
                    -> std::optional<guest::DmacQuadword> {
                if (!source_ranges.quadword_allowed(address) ||
                    !memory.is_main_ram_range(address.address(), 16U)) {
                    return std::nullopt;
                }
                return read_quadword(memory, address.address());
            },
            .on_tag =
                [&memory, &publication, &source_ranges, &candidate_tag, &terminal_tag, &source_shape_valid,
                 &terminal_shape_valid, &candidate_count, &candidate_tag_ordinal, &terminal_tag_ordinal,
                 &tag_ordinal, chain_root](const guest::DmacSourceTag& tag) {
                source_ranges.begin_tag(tag);
                if (tag_ordinal == 0U) {
                    source_shape_valid = tag.location.address() == chain_root && tag.id == 3U;
                } else if (tag_ordinal == 1U) {
                    terminal_shape_valid = tag.location.address() == publication.queue.queue_end &&
                                           tag.id == 7U && tag.qwc == 0U;
                    if (terminal_shape_valid) {
                        terminal_tag = tag;
                        terminal_tag_ordinal = tag_ordinal;
                    }
                } else {
                    terminal_shape_valid = false;
                }
                if (tag.id != 3U || tag.payload.scratchpad()) {
                    ++tag_ordinal;
                    return;
                }
                if (!ref_payload_in_one_buffer(memory, publication.plan, tag.payload.address(), tag.qwc)) {
                    ++tag_ordinal;
                    return;
                }
                if (++candidate_count == 1U) {
                    candidate_tag = tag;
                    candidate_tag_ordinal = tag_ordinal;
                }
                ++tag_ordinal;
            },
            .on_tag_complete =
                [&source_ranges](const guest::DmacSourceTag&, const guest::DmacSourceCursor&) {
                    source_ranges.reading_payload = false;
                },
        });

    if (!result.completed()) {
        if (source_ranges.range_rejected && candidate_count == 0U) {
            return reject(Failure::FirstRefMissing);
        }
        return {.failure = result.stop == guest::DmacSourceStop::CursorLoop ? Failure::ChainWalkAmbiguous
                                                                            : Failure::ChainWalkRejected};
    }
    if (result.tags_processed != 2U || result.stop != guest::DmacSourceStop::End ||
        result.stop_tag.scratchpad() || result.stop_tag.address() != publication.queue.queue_end ||
        result.stop_tag_id != 7U || queue_end != root_end || !source_shape_valid ||
        !terminal_shape_valid || !terminal_tag.has_value() || terminal_tag->location.scratchpad()) {
        return reject(Failure::ChainWalkRejected);
    }
    if (candidate_count == 0U || !candidate_tag.has_value()) {
        return reject(Failure::FirstRefMissing);
    }
    if (candidate_count != 1U) {
        return reject(Failure::CandidateAmbiguous);
    }
    const guest::GuestAddress source = candidate_tag->payload.address();
    if (!memory.is_main_ram_range(source, 16U)) {
        return reject(Failure::CandidateOutOfRange);
    }
    const bool in_first = contains_span(memory, publication.plan.first_buffer, publication.plan.buffer_bytes, source, 16U);
    const bool in_second = contains_span(memory, publication.plan.second_buffer, publication.plan.buffer_bytes, source, 16U);
    if (!in_first && !in_second) {
        return reject(Failure::CandidateWrongBuffer);
    }
    if (in_first && in_second) {
        return reject(Failure::CandidateAliased);
    }
    const bool in_active_buffer = publication.queue.buffer_index == 1U ? in_first : in_second;
    if (!in_active_buffer) {
        return reject(Failure::CandidateWrongBuffer);
    }

    Slus21050FemainLiveSourceCandidate candidate{
        .source = source,
        .chain_root = chain_root,
        .source_tag_location = candidate_tag->location.address(),
        .source_tag_ordinal = candidate_tag_ordinal,
        .terminal_tag_location = terminal_tag->location.address(),
        .terminal_tag_ordinal = terminal_tag_ordinal,
        .source_tag_words = candidate_tag->words,
        .terminal_tag_words = terminal_tag->words,
        .source_qwc = candidate_tag->qwc,
        .expected_quadword = read_quadword(memory, source),
        .publication_serial = publication.publication_serial,
        .player_session_generation = publication.player_session_generation,
        .frame_generation = publication.plan.frame_generation,
    };
    if (!mutation.validate(memory, publication.plan, publication.queue.queue_read, queue_bytes)) {
        return reject(Failure::GuestMemoryChanged);
    }
    candidate.authentication_tag = authenticate_candidate(candidate);
    if (!candidate.valid()) {
        return reject(Failure::ChainWalkRejected);
    }
    return {Failure::None, candidate};
}

bool confirm_slus_21050_femain_live_source_candidate(
    const Slus21050FemainLiveSourceCandidate& candidate, const guest::Vif1DmacPreSnapshotResolvedMarker& marker) noexcept {
    return candidate.valid() && candidate.authentication_tag == authenticate_candidate(candidate) &&
           marker.token != 0U && marker.generation != 0U &&
           marker.candidate_source == candidate.source && candidate.source == marker.request.source &&
           marker.request.chain_root == candidate.chain_root &&
           marker.request.source_tag_location == candidate.source_tag_location &&
           marker.request.source_tag_ordinal == candidate.source_tag_ordinal &&
           marker.request.source_payload_word_ordinal == candidate.source_payload_word_ordinal &&
           marker.request.expected_quadword == candidate.expected_quadword;
}


} // namespace takedown::runtime
