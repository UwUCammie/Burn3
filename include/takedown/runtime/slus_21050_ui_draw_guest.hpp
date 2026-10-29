#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace takedown::guest {

class EeMemory;
class R5900State;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

inline constexpr std::size_t kSubmitUiDrawBucketsBodyBytes = 804U;
inline constexpr std::uint64_t kSubmitUiDrawBucketsFingerprint = 0xdea1aa7878ea8346ULL;
inline constexpr guest::GuestAddress kUpdateUiHierarchyTransformsAddress = 0x001d4780U;
inline constexpr std::size_t kUpdateUiHierarchyTransformsBodyBytes = 1752U;
inline constexpr std::uint64_t kUpdateUiHierarchyTransformsFingerprint = 0xb09603e4b929df3aULL;
inline constexpr guest::GuestAddress kSubmitUiDrawCallbackReturnAddress = 0x001d78b0U;

// The adapter is entered after UpdateUiHierarchyTransforms has populated the
// retained node/parent graph. Live context-1 state comes from the ordered GS
// mirror because the game-side cache does not contain equivalent TEST, ZBUF,
// XYOFFSET, or necessarily current FRAME values.
struct SubmitUiDrawBucketsGuestInput {
    guest::GuestAddress owner{};
    // SubmitUiDrawBuckets loads owner +0x30 before calling the hierarchy
    // transform. A post-hierarchy adapter must consume that retained value,
    // even if the owner field has since changed.
    std::uint32_t retained_bucket_count{};
    UiDrawContext1Bits live;

    bool operator==(const SubmitUiDrawBucketsGuestInput&) const = default;
};

// Exact inputs already observed at their retail times: the caller reads the
// retained count before UpdateUiHierarchyTransforms, then loads bucket_array
// from owner +0x2c at 0x001d75fc, the delay slot of the post-Begin branch at
// 0x001d75f8. A post-hierarchy observer must receive these values rather than
// reread owner fields later.
struct SubmitUiDrawBucketsPostHierarchyInput {
    guest::GuestAddress bucket_array{};
    std::uint32_t retained_bucket_count{};

    bool operator==(const SubmitUiDrawBucketsPostHierarchyInput&) const = default;
};

// One callback source snapshot before the walker enters its generic JALR. This
// source-private observer retains the local and parent operands used to derive
// the transient A0 render record; it does not expose or claim to reproduce
// that stack record itself. descriptor is node +0x28 and key retains that same
// game-level identity without exposing the node pointer. Unknown callback
// addresses are preserved because the retail JALR is generic; text_effect is
// populated only for the three reviewed text targets.
struct UiDrawPostHierarchyCallback {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    guest::GuestAddress callback_address{};
    guest::GuestAddress descriptor{};
    TextDrawRenderRecordBits local_render;
    UiTextParentTransformBits parent;
    std::optional<UiTextEffectKind> text_effect;

    bool operator==(const UiDrawPostHierarchyCallback&) const = default;
};

class UiDrawPostHierarchyCallbackObserver {
  public:
    virtual ~UiDrawPostHierarchyCallbackObserver() = default;

    // Returning false stops after this observed callback. The observer may
    // mutate the same guest memory; later reads intentionally see that state.
    [[nodiscard]] virtual bool observe(const UiDrawPostHierarchyCallback& callback) = 0;
};

enum class UiDrawPostHierarchyCallbackTraversalStatus : std::uint8_t {
    Completed,
    InputRejected,
    FunctionProfileRejected,
    GraphRejected,
    ObserverStopped,
};

struct UiDrawPostHierarchyCallbackTraversalResult {
    UiDrawPostHierarchyCallbackTraversalStatus status{
        UiDrawPostHierarchyCallbackTraversalStatus::InputRejected};
    std::size_t observed_callback_count{};

    [[nodiscard]] bool completed() const noexcept {
        return status == UiDrawPostHierarchyCallbackTraversalStatus::Completed;
    }
};

// ABI captured at a real generic callback entry reached from
// SubmitUiDrawBuckets. render_record is the actual A0 stack address, unlike
// UiDrawPostHierarchyCallback's copied source operands. descriptor is raw A1
// and callback_address is the target reached through the generic JALR; neither
// is restricted to a known text callback.
struct UiDrawCallbackEntryAbi {
    guest::GuestAddress callback_address{};
    guest::GuestAddress render_record{};
    guest::GuestAddress descriptor{};

    bool operator==(const UiDrawCallbackEntryAbi&) const = default;
};

enum class UiDrawCallbackEntryAbiStatus : std::uint8_t {
    Observed,
    FunctionProfileRejected,
    CpuStateRejected,
    RenderRecordRejected,
};

struct UiDrawCallbackEntryAbiResult {
    UiDrawCallbackEntryAbiStatus status{UiDrawCallbackEntryAbiStatus::FunctionProfileRejected};
    std::optional<UiDrawCallbackEntryAbi> entry;

    [[nodiscard]] bool observed() const noexcept {
        return status == UiDrawCallbackEntryAbiStatus::Observed && entry.has_value();
    }
};

struct UiDrawGuestAdRowBits {
    std::uint64_t value{};
    std::uint64_t gs_register{};

    bool operator==(const UiDrawGuestAdRowBits&) const = default;
};

// The reducer's semantic presets correspond to these reachable initialized
// retail rows. This checkpoint requires their exact values: accepting an
// arbitrary row while retaining named equation/wrap enums would mislabel it.
struct UiDrawGuestStateTableSnapshot {
    std::array<UiDrawGuestAdRowBits, 4U> alpha_rows;
    std::array<UiDrawGuestAdRowBits, 2U> clamp_rows;

    bool operator==(const UiDrawGuestStateTableSnapshot&) const = default;
};

// Current game-side binding reached through the retail active-target owner
// cell. target is the render-target object used as the guest-side
// UiRenderTargetIdentity key; target_owner is retained as source evidence.
// Two owners which resolve to the same target and view are therefore
// semantically equivalent at this boundary. This does not resolve a host GPU
// resource or handle.
struct ActiveUiRenderTargetGuestBinding {
    guest::GuestAddress target_owner{};
    guest::GuestAddress target{};
    UiRenderTargetView view;

    bool operator==(const ActiveUiRenderTargetGuestBinding&) const = default;
};

// Exact bucket header values plus only the pointer targets reached by retail.
// reached_nodes is empty for disabled and empty buckets. A clip rectangle is
// retained only when an active bucket changes to that nonzero pointer.
struct UiDrawBucketGuestSourceSnapshot {
    guest::GuestAddress address{};
    std::uint32_t flags{};
    guest::GuestAddress node_head{};
    guest::GuestAddress scissor{};
    std::optional<UiClipRectangleBits> reached_scissor_rectangle;
    std::vector<guest::GuestAddress> reached_nodes;

    bool operator==(const UiDrawBucketGuestSourceSnapshot&) const = default;
};

// Immutable values read while converting the retained guest graph to the two
// typed, guest-independent inputs. The flattened text_nodes in the enclosing
// plan are in the same bucket/node order as reached_nodes here.
struct SubmitUiDrawBucketsGuestSourceSnapshot {
    guest::GuestAddress bucket_array{};
    std::uint32_t retained_bucket_count{};
    UiDrawCachedInputsBits cached;
    guest::GuestAddress target_owner{};
    guest::GuestAddress target{};
    UiRenderTargetView target_view;
    UiDrawGuestStateTableSnapshot state_tables;
    std::vector<UiDrawBucketGuestSourceSnapshot> buckets;

    bool operator==(const SubmitUiDrawBucketsGuestSourceSnapshot&) const = default;
};

struct UiDrawGuestFunctionFingerprint {
    guest::GuestAddress address{};
    std::size_t body_bytes{};
    std::uint64_t value{};

    bool operator==(const UiDrawGuestFunctionFingerprint&) const = default;
};

// Atomic, owned result of guest adaptation. state is produced through
// UiDrawStateInput and text_nodes carries the corresponding callback records;
// no spans or guest-memory references escape the planning call.
struct SubmitUiDrawBucketsGuestPlan {
    SubmitUiDrawBucketsGuestInput input;
    SubmitUiDrawBucketsGuestSourceSnapshot sources;
    UiDrawStatePlan state;
    std::vector<UiTextNodeSubmissionBits> text_nodes;
    std::vector<UiDrawGuestFunctionFingerprint> reviewed_functions;

    bool operator==(const SubmitUiDrawBucketsGuestPlan&) const = default;
};

// Reads the current active-target owner chain and owns the exact target view
// consumed by the UI state reducer. Pointer alignment and every reached range
// are validated before access. Target extent semantics are deliberately left
// to the reducer, or to an exact comparison with an already-canonical view at
// a later callback boundary.
[[nodiscard]] std::optional<ActiveUiRenderTargetGuestBinding>
capture_active_ui_render_target_from_guest(const guest::EeMemory& memory);

// Decodes the reviewed post-hierarchy owner/bucket/node graph, cached setter
// inputs, and active target. Unsupported callbacks, malformed/cyclic reached
// lists, unsafe direct-main-RAM ranges, or a reducer-invalid state reject the
// complete operation without publishing a partial plan.
[[nodiscard]] std::optional<SubmitUiDrawBucketsGuestPlan>
plan_submit_ui_draw_buckets_from_guest(const guest::EeMemory& memory,
                                       const SubmitUiDrawBucketsGuestInput& input);

// Source-private post-hierarchy callback-order observer for the future dynamic
// runner. It validates the narrow walker/hierarchy profile, reads each bucket
// and node immediately before observation, and rereads current node +0x00 only
// after the observer returns, matching the retail load at 0x001d78b0. Its
// second bucket-head load is structurally separate, but occurs immediately
// because this helper deliberately does not execute the intervening setters or
// scissor logic; it is not an exact post-setter sample. It does not execute
// Begin/End, setters, the guest callback, or the transient A0 stack write; it
// creates no UiTextPacketSession or ticket ledger and makes no live renderer,
// ABI, state-order, or lifetime claim. Graph rejection can occur after earlier
// observations, so this is intentionally not an all-or-none plan.
[[nodiscard]] UiDrawPostHierarchyCallbackTraversalResult
observe_submit_ui_draw_bucket_callbacks_from_guest(
    guest::EeMemory& memory, const SubmitUiDrawBucketsPostHierarchyInput& input,
    UiDrawPostHierarchyCallbackObserver& observer);

// Reads the actual callback-entry ABI after the generic JALR has completed its
// delay slot. It gates the caller body, requires RA == 0x001d78b0 and the
// retail A0 == SP + 0xb0 relation, and validates the 32-byte A0 range without
// copying it. It preserves an unknown JALR target and raw A1. It does not prove
// the preceding setter/Begin timing, execute a callback, construct a session
// or ledger, or make a dispatcher/renderer activation claim.
[[nodiscard]] UiDrawCallbackEntryAbiResult observe_submit_ui_draw_callback_entry_abi(
    const guest::R5900State& cpu, const guest::EeMemory& memory);

namespace ui_draw_guest_detail {  // RECOVERY: reconstructed namespace opener inside slus_21050

// RECOVERY: reconstructed declaration
// Lost from the header; layout inferred from the GIMPLE of valid_for and the
// observer that fills it: five 24-byte main-RAM code-page tokens followed by a
// revision word. valid_for is defined in the session observer translation unit.
struct UiDrawSessionCodeProfileToken {
    std::array<guest::EeMainRamCodePageToken, 5U> pages_{};
    std::uint32_t profile_revision_{};

    [[nodiscard]] bool valid_for(const guest::EeMemory& memory) const noexcept;
};

// RECOVERY: reconstructed declaration
[[nodiscard]] UiDrawCallbackEntryAbiResult
observe_submit_ui_draw_callback_entry_abi_with_code_profile(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const UiDrawSessionCodeProfileToken& code_profile);

} // namespace ui_draw_guest_detail

} // namespace takedown::runtime::slus_21050
