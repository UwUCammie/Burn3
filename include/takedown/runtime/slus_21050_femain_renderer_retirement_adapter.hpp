#pragma once

#include "takedown/presentation/gs_renderer_worker.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"
#include "takedown/runtime/slus_21050_femain_renderer_retirement_witness.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <string_view>
#include <thread>

namespace takedown::runtime {

enum class Slus21050FemainRendererRetirementAdapterPhase : std::uint8_t {
    Empty,
    Bound,
    Submitted,
    CompletionReady,
    ReceiptPending,
    Retired,
    Failed,
};

enum class Slus21050FemainRendererRetirementAdapterStatus : std::uint8_t {
    Ok,
    InvalidState,
    InvalidBinding,
    ContextMismatch,
    ThreadMismatch,
    Pending,
    WorkerFailure,
    RetirementRejected,
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainRendererRetirementAdapterStatus status) noexcept;

// All pointers are borrowed live participants, including the exact EeMemory
// object whose post-publication mutation generations were captured. They are
// supplied on every operation so a destroyed participant can be represented by
// nullptr without the adapter ever dereferencing a stale pointer.  worker_epoch and
// runtime_epoch are caller-owned, explicit nonzero generations.  The runtime
// epoch is independent of the FEMain player/ledger session generation.
struct Slus21050FemainRendererRetirementAdapterContext {
    Slus21050FemainMovieTransactionOwner* owner{};
    Slus21050FemainFrameTransactionLedger* ledger{};
    guest::EeMemory* memory{};
    presentation::GsRendererWorker* worker{};
    // This scalar identity is supplied by the serialized emulation caller and
    // is correlated with the source/terminal witness.  It is separate from
    // std::thread::id so a reused host thread cannot inherit an old proof.
    std::uint64_t emulation_thread_identity{};
    std::uint64_t worker_epoch{};
    std::uint64_t runtime_epoch{};
};

using Slus21050FemainRendererFrameCompletion = presentation::GsRendererWorkerFrameCompletionResult;

struct Slus21050FemainRendererRetirementAdapterResult {
    Slus21050FemainRendererRetirementAdapterStatus status{
        Slus21050FemainRendererRetirementAdapterStatus::InvalidState};
    std::optional<Slus21050FemainRendererFrameCompletion> completion{};
    std::optional<Slus21050FemainMovieTransactionResult> retirement{};

    [[nodiscard]] bool ok() const noexcept {
        return status == Slus21050FemainRendererRetirementAdapterStatus::Ok;
    }
    [[nodiscard]] bool pending() const noexcept {
        return status == Slus21050FemainRendererRetirementAdapterStatus::Pending;
    }
};

// One fixed-capacity, passive bridge from a published owner reservation to an
// ordered GS worker completion.  It stores only value identities and the one
// worker future; it stores no borrowed participant pointers.  Consequently,
// destroying the worker/owner/ledger after a failed or abandoned operation
// does not make adapter destruction dereference invalid memory.  The caller
// must keep the live owner, ledger, and worker objects available while making
// a successful submit/poll/retire call.
class Slus21050FemainRendererRetirementAdapter final {
  public:
    using Phase = Slus21050FemainRendererRetirementAdapterPhase;
    using Status = Slus21050FemainRendererRetirementAdapterStatus;
    using Context = Slus21050FemainRendererRetirementAdapterContext;
    using Completion = Slus21050FemainRendererFrameCompletion;
    using Witness = Slus21050FemainRendererTerminalPrimaryDeliveryWitness;
    using Result = Slus21050FemainRendererRetirementAdapterResult;

    Slus21050FemainRendererRetirementAdapter() noexcept = default;
    ~Slus21050FemainRendererRetirementAdapter() = default;

    Slus21050FemainRendererRetirementAdapter(const Slus21050FemainRendererRetirementAdapter&) =
        delete;
    Slus21050FemainRendererRetirementAdapter&
    operator=(const Slus21050FemainRendererRetirementAdapter&) = delete;
    Slus21050FemainRendererRetirementAdapter(Slus21050FemainRendererRetirementAdapter&&) = delete;
    Slus21050FemainRendererRetirementAdapter&
    operator=(Slus21050FemainRendererRetirementAdapter&&) = delete;

    // Captures one exact GuestPublished owner reservation and one completed,
    // typed terminal primary-delivery witness. Binding verifies the nested
    // source evidence and correlates its player/lease/plan/ledger identities
    // before recording the current worker/runtime generations. Binding does
    // not submit work or mutate owner, player, or ledger.
    [[nodiscard]] Result
    bind(const Context& context,
         const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept;

    // The sole production request seam.  The caller invokes this only at the
    // future complete-frame boundary; no request is made during bind or poll.
    [[nodiscard]] Result submit(const Context& context) noexcept;

    // Serialized emulation-thread nonblocking poll.  A worker callback only
    // fulfills the future; this method alone consumes it and may advance the
    // adapter phase.
    [[nodiscard]] Result poll(const Context& context) noexcept;

    // Serialized emulation-thread retirement.  This reaches the owner only
    // after a valid echoed completion, and passes through the caller's exact
    // guest boundaries without inferring or fabricating them.
    [[nodiscard]] Result
    retire(const Context& context,
           const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept;

    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] std::optional<Completion> completion() const noexcept { return completion_; }

  private:
    [[nodiscard]] Result make_result(Status status) const noexcept;
    [[nodiscard]] Result fail(Status status) noexcept;
    [[nodiscard]] bool thread_matches() const noexcept;
    [[nodiscard]] bool context_identities_match(const Context& context) const noexcept;
    [[nodiscard]] bool owner_and_ledger_match(const Context& context,
                                              bool published,
                                              bool validate_source_evidence) const noexcept;
    [[nodiscard]] static std::uintptr_t object_identity(const void* object) noexcept;

    // Immutable identity copies captured at bind.  No participant pointer is
    // retained, so every later operation must provide and revalidate a live
    // context before dereferencing any participant.
    std::uintptr_t owner_identity_{};
    std::uintptr_t ledger_identity_address_{};
    std::uintptr_t memory_identity_{};
    std::uintptr_t worker_identity_{};
    std::uint64_t emulation_thread_identity_{};
    std::uint64_t owner_player_identity_{};
    std::uint64_t owner_player_session_generation_{};
    Slus21050MovieFrameLease owner_frame_lease_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t ledger_session_generation_{};
    std::uint64_t worker_epoch_{};
    std::uint64_t runtime_epoch_{};
    std::uint64_t frame_id_{};
    std::uint64_t terminal_primary_event_ordinal_{};
    std::uint64_t primary_stream_digest_{};
    std::optional<Slus21050FemainFrameGraphicsSourceEvidence> source_evidence_{};
    std::optional<Completion> completion_{};
    std::optional<Slus21050FemainMovieTransactionResult> retirement_{};
    std::future<Completion> completion_future_{};
    std::thread::id owning_thread_{};
    Phase phase_{Phase::Empty};
};

} // namespace takedown::runtime
