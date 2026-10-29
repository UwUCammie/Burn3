#include "takedown/runtime/slus_21050_ui_draw_session_observer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace takedown::runtime::slus_21050 {
namespace ui_draw_guest_detail {
namespace {

// Reviewed constant tables recovered from the object's read-only data.
// Reviewed constant tables recovered from the object's read-only data.
constexpr std::array<std::uint32_t, 68> kStaticBoundaries{{
    1918848U,
    1930688U,
    1930720U,
    1930724U,
    1930728U,
    1930732U,
    1930736U,
    1930740U,
    1930744U,
    1930748U,
    1930792U,
    1930804U,
    1930864U,
    1930868U,
    1930872U,
    1930880U,
    1930884U,
    1930888U,
    1930896U,
    1930900U,
    1930904U,
    1930912U,
    1930916U,
    1930920U,
    1930948U,
    1930952U,
    1930956U,
    1930964U,
    1930968U,
    1930972U,
    1931000U,
    1931004U,
    1931008U,
    1931016U,
    1931020U,
    1931024U,
    1931028U,
    1931156U,
    1931160U,
    1931164U,
    1931172U,
    1931176U,
    1931188U,
    1931192U,
    1931196U,
    1931204U,
    1931208U,
    1931212U,
    1931216U,
    1931248U,
    1931432U,
    1931436U,
    1931440U,
    1931444U,
    1931484U,
    1931488U,
    2193632U,
    2193952U,
    2194496U,
    2194496U,
    2194752U,
    2194752U,
    2195056U,
    2195056U,
    2195056U,
    2195056U,
    2203424U,
    2203888U}};
constexpr std::array<std::uint64_t, 128> kStaticBoundaryPages{{
    0ULL,
    0ULL,
    0ULL,
    0ULL,
    0ULL,
    0ULL,
    0ULL,
    9437184ULL,
    109051904ULL}};

constexpr std::array<std::uint32_t, 4> kUiDrawSessionOneVector{{1065353216U, 1065353216U, 1065353216U, 1065353216U}};
// Setter call sites: {call pc, delay-slot pc, setter target, return address, setter kind, argument count, flag}.
constexpr std::array<SetterCallSite, 10> kSetterCallSites{{
    {0x1d7670U, 0x1d7674U, 0x217e70U, 0x1d7678U, UiDrawSessionSetterKind::Kind0, 1U},
    {0x1d7680U, 0x1d7684U, 0x217e70U, 0x1d7688U, UiDrawSessionSetterKind::Kind0, 2U},
    {0x1d7690U, 0x1d7694U, 0x217e70U, 0x1d7698U, UiDrawSessionSetterKind::Kind0, 3U},
    {0x1d76a0U, 0x1d76a4U, 0x217e70U, 0x1d76a8U, UiDrawSessionSetterKind::Kind0, 0U},
    {0x1d76c4U, 0x1d76c8U, 0x217d40U, 0x1d76ccU, UiDrawSessionSetterKind::Kind1, 1U},
    {0x1d76d4U, 0x1d76d8U, 0x217d40U, 0x1d76dcU, UiDrawSessionSetterKind::Kind1, 0U},
    {0x1d76f8U, 0x1d76fcU, 0x217c40U, 0x1d7700U, UiDrawSessionSetterKind::Kind2, 1U},
    {0x1d7708U, 0x1d770cU, 0x217c40U, 0x1d7710U, UiDrawSessionSetterKind::Kind2, 0U},
    {0x1d7794U, 0x1d7798U, 0x217a20U, 0x1d779cU, UiDrawSessionSetterKind::Kind4, std::nullopt},
    {0x1d77c4U, 0x1d77c8U, 0x2178e0U, 0x1d77ccU, UiDrawSessionSetterKind::Kind3, std::nullopt},
}};

// Reads one 32-bit little-endian word of guest memory.
__attribute__((always_inline)) inline std::uint32_t read_u32(const guest::EeMemory& memory,
                                                             const guest::GuestAddress address) noexcept {
    return memory.read_u32_le(address);
}

// Base plus byte offset, or nothing when the 32-bit sum overflows.
__attribute__((always_inline)) inline std::optional<guest::GuestAddress> offset_address(
    const guest::GuestAddress base, const std::uint32_t offset) noexcept {
    const std::uint64_t sum = static_cast<std::uint64_t>(base) + offset;
    if (sum > 4294967295ULL) { return std::nullopt; }
    return static_cast<guest::GuestAddress>(sum);
}


// Reads one 32-byte UI text render record from guest memory (A0 record layout).
TextDrawRenderRecordBits read_render_record(const guest::EeMemory& memory, const guest::GuestAddress address) {
    TextDrawRenderRecordBits record;
    record.layout_box.base_x_bits = memory.read_u32_le(address);
    record.layout_box.base_y_bits = memory.read_u32_le(address + 4U);
    record.layout_box.horizontal_extent_bits = memory.read_u32_le(address + 8U);
    record.vertical_extent_bits = memory.read_u32_le(address + 12U);
    record.color.lanes[0] = memory.read_u32_le(address + 16U);
    record.color.lanes[1] = memory.read_u32_le(address + 20U);
    record.color.lanes[2] = memory.read_u32_le(address + 24U);
    record.color.lanes[3] = memory.read_u32_le(address + 28U);
    return record;
}

// FNV-1a over a domain string, the function's entry address and size, and its bytes.
__attribute__((always_inline)) inline std::optional<std::uint64_t> function_fingerprint(const guest::EeMemory& memory,
                                                  const guest::GuestAddress address,
                                                  const std::size_t body_bytes) noexcept {
    constexpr char kDomain[24] = "takedown-ee-function-v1";
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char value : kDomain) {
        hash = (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(value))) * 1099511628211ULL;
    }
    for (unsigned shift = 0; shift != 32U; shift += 8U) {
        hash = (hash ^ ((address >> shift) & 255U)) * 1099511628211ULL;
    }
    const auto body = static_cast<std::uint32_t>(body_bytes);
    for (unsigned shift = 0; shift != 32U; shift += 8U) {
        hash = (hash ^ ((body >> shift) & 255U)) * 1099511628211ULL;
    }
    for (std::uint32_t offset = 0; offset < body; ++offset) {
        const std::uint8_t value = memory.read_u8(address + offset);
        hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ULL;
    }
    return hash;
}

bool is_reviewed_code_page_range_valid(const guest::EeMemory& memory,
                                       const std::array<guest::EeMainRamCodePageToken, 5U>& pages) noexcept {
    return std::all_of(pages.begin(), pages.end(), [&](const guest::EeMainRamCodePageToken& page) {
        return memory.validate_main_ram_code_page(page);
    });
}

constexpr std::array<std::uint32_t, 5> kSessionReviewedCodePages{{1916928U, 1929216U, 2191360U, 2199552U, 2203648U}};
constexpr std::array<UiTextCallbackPayloadReviewedFunction, 9> kSessionReviewedFunctions{{
    {1930688U, 804ULL, 16042290782071849798ULL},
    {1918848U, 1752ULL, 12724362078075871034ULL},
    {2193632U, 308ULL, 15977696261475555356ULL},
    {2193952U, 536ULL, 530366181308675863ULL},
    {2194496U, 252ULL, 17180085750144147719ULL},
    {2194752U, 300ULL, 4715185619524913338ULL},
    {2195056U, 328ULL, 4733354230414767449ULL},
    {2203424U, 464ULL, 16782614340437661199ULL},
    {2203888U, 408ULL, 3381953517370809804ULL}}};

} // namespace

const std::array<std::uint32_t, 5> kUiTextCallbackPayloadReviewedCodePages{{1929216U, 1933312U, 2199552U, 3112960U, 3117056U}};
const std::array<UiTextCallbackPayloadReviewedFunction, 8> kUiTextCallbackPayloadReviewedFunctions{{
    {3116112U, 632ULL, 4806424622881744615ULL},
    {3116752U, 612ULL, 10484272105924700748ULL},
    {3117376U, 452ULL, 5790617513735962148ULL},
    {3117840U, 888ULL, 9045103797425168583ULL},
    {1932816U, 648ULL, 15273014190714661832ULL},
    {2201648U, 496ULL, 18091304630199889834ULL},
    {2202144U, 616ULL, 8414566995517809548ULL},
    {2202768U, 8ULL, 15324386673544022450ULL}}};

// Static boundary table: the page bitmap filters first, then the sorted table is searched.
bool is_submit_ui_draw_buckets_session_static_boundary(const guest::GuestAddress pc) noexcept {
    const std::uint32_t page = pc >> 12U;
    if (page > 8191U) { return false; }
    if (((kStaticBoundaryPages[page >> 6U] >> (page & 63U)) & 1U) == 0U) { return false; }
    return std::binary_search(kStaticBoundaries.begin(), kStaticBoundaries.end(), pc);
}

std::span<const guest::GuestAddress> submit_ui_draw_buckets_session_static_boundaries() noexcept {
    return {kStaticBoundaries.data(), kStaticBoundaries.size()};
}

bool UiDrawSessionCodeProfileToken::valid_for(const guest::EeMemory& memory) const noexcept {
    return profile_revision_ == 1U && is_reviewed_code_page_range_valid(memory, pages_);
}

bool UiTextCallbackPayloadCodeProfileToken::valid_for(const guest::EeMemory& memory) const noexcept {
    return profile_revision_ == 1U && is_reviewed_code_page_range_valid(memory, pages_);
}

namespace {
void saturating_increment(std::uint64_t& counter) noexcept {
    if (counter != 18446744073709551615ULL) { ++counter; }
}
} // namespace

UiTextCallbackPayloadCodeProfileAdmission UiTextCallbackPayloadCodeProfileCache::admit_or_reuse(
    const guest::EeMemory& memory) noexcept {
    using Availability = UiTextCallbackPayloadCodeProfileAvailability;
    saturating_increment(diagnostics_.admission_attempts);
    if (token_.valid_for(memory)) {
        saturating_increment(diagnostics_.cache_reuses);
        return {Availability::ReusedFromCache, token_};
    }
    UiTextCallbackPayloadCodeProfileToken candidate{};
    candidate.profile_revision_ = 1U;
    for (std::size_t index = 0; index < kUiTextCallbackPayloadReviewedCodePages.size(); ++index) {
        const auto capture = memory.capture_main_ram_code_page(kUiTextCallbackPayloadReviewedCodePages[index]);
        if (capture.status != guest::EeMainRamCodePageCaptureStatus::Captured) {
            saturating_increment(diagnostics_.code_page_capture_rejections);
            return {Availability::CodePageCaptureRejected, std::nullopt};
        }
        candidate.pages_[index] = capture.token;
    }
    saturating_increment(diagnostics_.fingerprint_scans);
    bool fingerprints_match = true;
    for (const UiTextCallbackPayloadReviewedFunction& reviewed : kUiTextCallbackPayloadReviewedFunctions) {
        const std::uint64_t begin = reviewed.address;
        const std::uint64_t end = reviewed.body_bytes + begin;
        std::optional<std::uint64_t> fingerprint;
        if (begin <= end && end <= memory.size()) {
            fingerprint = function_fingerprint(memory, reviewed.address,
                                               static_cast<std::size_t>(reviewed.body_bytes));
        }
        if (!fingerprint.has_value() || *fingerprint != reviewed.fingerprint) {
            fingerprints_match = false;
            break;
        }
    }
    if (!fingerprints_match) {
        saturating_increment(diagnostics_.fingerprint_rejections);
        return {Availability::FunctionFingerprintRejected, std::nullopt};
    }
    if (candidate.profile_revision_ != 1U || !is_reviewed_code_page_range_valid(memory, candidate.pages_)) {
        saturating_increment(diagnostics_.changed_during_admission);
        return {Availability::ChangedDuringAdmission, std::nullopt};
    }
    token_ = candidate;
    saturating_increment(diagnostics_.successful_admissions);
    return {Availability::FreshlyAdmitted, candidate};
}

UiTextCallbackPayloadCodeProfileCacheDiagnostics UiTextCallbackPayloadCodeProfileCache::diagnostics() const noexcept {
    return diagnostics_;
}

void UiTextCallbackPayloadCodeProfileCache::record_active_invocation_invalidation() noexcept {
    saturating_increment(diagnostics_.active_invocation_invalidations);
}

UiTextCallbackPayloadCodeProfileAvailability UiTextCallbackPayloadCodeProfileInvocation::observe_known_callback(
    const guest::EeMemory& memory, UiTextCallbackPayloadCodeProfileCache& cache) noexcept {
    using Availability = UiTextCallbackPayloadCodeProfileAvailability;
    if (state_ == 2U) { return sticky_availability_; }
    if (state_ == 1U) {
        if (token_.has_value() && token_->valid_for(memory)) { return Availability::AvailableForInvocation; }
        token_.reset();
        state_ = 2U;
        sticky_availability_ = Availability::InvalidatedDuringInvocation;
        cache.record_active_invocation_invalidation();
        return Availability::InvalidatedDuringInvocation;
    }
    const UiTextCallbackPayloadCodeProfileAdmission admission = cache.admit_or_reuse(memory);
    sticky_availability_ = admission.availability;
    if (admission.token.has_value() && static_cast<std::uint8_t>(admission.availability) - 1U <= 2U) {
        token_ = admission.token;
        state_ = 1U;
    } else {
        state_ = 2U;
    }
    return admission.availability;
}

bool UiTextCallbackPayloadCodeProfileInvocation::requested() const noexcept { return state_ != 0U; }

bool UiTextCallbackPayloadCodeProfileInvocation::available() const noexcept {
    return state_ == 1U && token_.has_value();
}

bool UiDrawSessionNodeVisitBitmap::insert(const guest::GuestAddress node) noexcept {
    if ((node & 15U) != 8U || node > 33554360U) { return false; }
    const std::uint64_t slot = static_cast<std::uint64_t>(node) >> 4U;
    const std::size_t word_index = static_cast<std::size_t>(slot >> 6U);
    const std::uint64_t mask = 1ULL << (slot & 63U);
    const std::uint64_t word = bitmap_[word_index];
    if ((mask & word) != 0U) { return false; }
    if (word == 0U) {
        if (touched_word_count_ == 32768U) { return false; }
        touched_word_indices_[touched_word_count_] = static_cast<std::uint16_t>(word_index);
        ++touched_word_count_;
    }
    bitmap_[word_index] = mask | word;
    return true;
}

void UiDrawSessionNodeVisitBitmap::clear() noexcept {
    for (std::size_t index = 0; index < touched_word_count_; ++index) {
        bitmap_[touched_word_indices_[index]] = 0U;
    }
    touched_word_count_ = 0U;
}

bool UiDrawSessionNodeVisitBitmap::empty() const noexcept { return touched_word_count_ == 0U; }

std::size_t UiDrawSessionNodeVisitBitmap::touched_word_count() const noexcept { return touched_word_count_; }

bool SubmitUiDrawBucketsSessionWorkspace::claim() noexcept {
    if (claimed_) { return false; }
    visit_.clear();
    claimed_ = true;
    return true;
}

void SubmitUiDrawBucketsSessionWorkspace::release() noexcept {
    visit_.clear();
    claimed_ = false;
}

bool SubmitUiDrawBucketsSessionWorkspace::claimed() const noexcept { return claimed_; }

std::size_t SubmitUiDrawBucketsSessionWorkspace::code_profile_refresh_count() const noexcept {
    return code_profile_refresh_count_;
}

UiTextCallbackPayloadCodeProfileCacheDiagnostics
SubmitUiDrawBucketsSessionWorkspace::payload_code_profile_diagnostics() const noexcept {
    return payload_code_profile_cache_.diagnostics();
}


namespace {
// CSWTCH.1005: entry boundary by pending role.
__attribute__((always_inline)) inline UiDrawSessionBoundaryKind entry_boundary_for_role(const std::uint8_t role) noexcept {
    switch (role) {
    case 0U: return UiDrawSessionBoundaryKind::Boundary5;
    case 1U: return UiDrawSessionBoundaryKind::Boundary9;
    case 2U: return UiDrawSessionBoundaryKind::Boundary18;
    case 3U: return UiDrawSessionBoundaryKind::Boundary30;
    default: return UiDrawSessionBoundaryKind::Boundary9;
    }
}
// CSWTCH.517: direct-call boundary by pending role.
__attribute__((always_inline)) inline UiDrawSessionBoundaryKind direct_boundary_for_role(const std::uint8_t role) noexcept {
    switch (role) {
    case 0U: return UiDrawSessionBoundaryKind::Boundary4;
    case 1U: return UiDrawSessionBoundaryKind::Boundary8;
    case 2U: return UiDrawSessionBoundaryKind::Boundary17;
    case 3U: return UiDrawSessionBoundaryKind::Boundary29;
    default: return UiDrawSessionBoundaryKind::Boundary8;
    }
}
// CSWTCH.429: flag argument by lowest-bit index.
__attribute__((always_inline)) inline std::uint32_t flag_argument_for_index(const int index) noexcept {
    switch (index) {
    case 0: return 1U;
    case 1: return 2U;
    case 2: return 0U;
    case 3: return 0U;
    case 4: return 3U;
    default: return 0U;
    }
}
} // namespace


namespace {

constexpr guest::GuestAddress kSubmitEntryAddress = 0x001d75c0U;
constexpr std::uint32_t kGlobalPointerValue = 5146224U;

} // namespace

// Sealing and emission helpers. The sealed state is sticky: only the first seal records its reason.
SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::emit(
    const UiDrawSessionBoundaryKind boundary,
    std::optional<UiDrawSessionObservationEvent> event) noexcept {
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = boundary;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    result.event = std::move(event);
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::complete(
    std::optional<UiDrawSessionObservationEvent> event) noexcept {
    ++event_ordinal_;
    phase_ = 31U;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Completed;
    result.boundary = UiDrawSessionBoundaryKind::Boundary34;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    result.event = std::move(event);
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::seal(
    const UiDrawSessionSealReason reason, const UiDrawSessionBoundaryKind boundary) noexcept {
    if (phase_ != 32U) {
        ++event_ordinal_;
        phase_ = 32U;
        seal_reason_ = reason;
        sealed_boundary_ = boundary;
    }
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Sealed;
    result.boundary = sealed_boundary_;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = seal_reason_;
    return result;
}

// Shared sealing used by every mismatch path: the first seal wins.
SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_submit_entry(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    const bool entry_matches = !cpu.is_executing_delay_slot() && cpu.pc() == kSubmitEntryAddress &&
                               cpu.next_pc() == kSubmitEntryAddress + 4U &&
                               cpu.read_gpr_u32(28U) == kGlobalPointerValue;
    const std::uint32_t stack_pointer = cpu.read_gpr_u32(29U);
    const std::uint32_t return_address = cpu.read_gpr_u32(31U);
    if (!entry_matches || (stack_pointer & 15U) != 0U || stack_pointer <= 303U || return_address == 0U ||
        (return_address & 3U) != 0U) {
        return seal(UiDrawSessionSealReason::EntryMismatch, UiDrawSessionBoundaryKind::Boundary1);
    }
    if (!admit_or_reuse_code_profile(memory)) {
        return seal(UiDrawSessionSealReason::CodeProfileRejected,
                                   UiDrawSessionBoundaryKind::Boundary1);
    }
    owner_ = cpu.read_gpr_u32(4U);
    caller_stack_pointer_ = stack_pointer;
    caller_return_address_ = return_address;
    frame_stack_pointer_ = stack_pointer - 304U;
    const std::uint64_t begin = frame_stack_pointer_;
    if (begin + 304U > memory.size()) {
        if (phase_ != 32U) {
            ++event_ordinal_;
            phase_ = 32U;
            seal_reason_ = UiDrawSessionSealReason::FrameOutOfRange;
            sealed_boundary_ = UiDrawSessionBoundaryKind::Boundary1;
        }
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Sealed;
        result.boundary = sealed_boundary_;
        result.event_ordinal = event_ordinal_;
        result.seal_reason = seal_reason_;
        return result;
    }
    phase_ = 1U;
    UiDrawSessionSubmitEntryObservation entry;
    entry.owner = owner_;
    entry.caller_stack_pointer = caller_stack_pointer_;
    entry.caller_return_address = caller_return_address_;
    return emit(UiDrawSessionBoundaryKind::Boundary1, UiDrawSessionObservationEvent{entry});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_direct_entry(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const std::uint8_t role = pending_direct_.has_value() ? pending_direct_->role : std::uint8_t{};
    const bool entered = pending_direct_.has_value() && !cpu.is_executing_delay_slot() &&
                         pending_direct_->target == cpu.pc() && pending_direct_->target + 4U == cpu.next_pc() &&
                         pending_direct_->return_address == cpu.read_gpr_u32(31U) &&
                         cpu.read_gpr_u32(28U) == 5146224U && frame_stack_pointer_ == cpu.read_gpr_u32(29U);
    if (!entered) {
        Boundary boundary = Boundary::Boundary9;
        if (pending_direct_.has_value() && role == 2U) {
            boundary = Boundary::Boundary18;
        } else if (pending_direct_.has_value() && role == 3U) {
            boundary = Boundary::Boundary30;
        } else if (pending_direct_.has_value() && role == 0U) {
            boundary = Boundary::Boundary5;
        }
        return seal(Reason::EntryMismatch, boundary);
    }
    const Boundary entry_boundary = role <= 3U ? entry_boundary_for_role(role) : Boundary::Boundary9;
    if (!code_profile_.valid_for(memory)) { return seal(Reason::CodeProfileRejected, entry_boundary); }

    if (role == 0U) {
        if (owner_ != cpu.read_gpr_u32(4U) || owner_ != cpu.read_gpr_u32(16U) ||
            retained_bucket_count_ != cpu.read_gpr_u32(17U)) {
            return seal(Reason::EntryMismatch, Boundary::Boundary5);
        }
        phase_ = 6U;
        ++event_ordinal_;
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Observed;
        result.boundary = Boundary::Boundary5;
        result.event_ordinal = event_ordinal_;
        result.seal_reason = UiDrawSessionSealReason::None;
        return result;
    }
    if (role == 1U) {
        if (owner_ != cpu.read_gpr_u32(16U) || retained_bucket_count_ != cpu.read_gpr_u32(17U) ||
            cpu.read_gpr_u32(20U) != 0U || cpu.read_gpr_u32(21U) != 64U) {
            return seal(Reason::EntryMismatch, Boundary::Boundary9);
        }
        active_target_ = capture_active_ui_render_target_from_guest(memory);
        if (!active_target_.has_value()) { return seal(Reason::TargetUnavailable, Boundary::Boundary9); }
        phase_ = 7U;
        UiDrawSessionBeginObservation begin;
        begin.owner = owner_;
        begin.frame_stack_pointer = frame_stack_pointer_;
        begin.retained_bucket_count = retained_bucket_count_;
        begin.target = *active_target_;
        return emit(Boundary::Boundary9, UiDrawSessionObservationEvent{begin});
    }
    if (role == 2U) {
        if (!pending_direct_.has_value() || !pending_direct_->setter_kind.has_value()) { return seal(Reason::SetterRejected, Boundary::Boundary18); }
        const UiDrawSessionSetterKind kind = *pending_direct_->setter_kind;
        if (!matches_setter_state(cpu, kind) || observed_setter_count_ >= expected_setter_count_ ||
            expected_setters_[observed_setter_count_].kind != kind) {
            return seal(Reason::SetterRejected, Boundary::Boundary18);
        }
        const ExpectedSetter& expected = expected_setters_[observed_setter_count_];
        if (pending_direct_->setter_argument.has_value() && cpu.read_gpr_u32(4U) != *pending_direct_->setter_argument) {
            return seal(Reason::SetterRejected, Boundary::Boundary18);
        }
        if (expected.checks_argument &&
            (!pending_direct_->setter_argument.has_value() || *pending_direct_->setter_argument != expected.argument)) {
            return seal(Reason::SetterRejected, Boundary::Boundary18);
        }
        if (kind == UiDrawSessionSetterKind::Kind0 || kind == UiDrawSessionSetterKind::Kind1 ||
            kind == UiDrawSessionSetterKind::Kind2) {
            if (previous_flags_ != cpu.read_gpr_u32(21U) || current_flag_delta_ != cpu.read_gpr_u32(18U)) {
                return seal(Reason::EntryMismatch, Boundary::Boundary18);
            }
        }
        if (kind == UiDrawSessionSetterKind::Kind3 || kind == UiDrawSessionSetterKind::Kind4) {
            if (current_flags_ != cpu.read_gpr_u32(21U) || current_flag_delta_ != cpu.read_gpr_u32(18U)) {
                return seal(Reason::EntryMismatch, Boundary::Boundary18);
            }
            UiDrawSessionSealReason target_reason = UiDrawSessionSealReason::None;
            if (!target_matches(memory, target_reason)) {
                return seal(target_reason, Boundary::Boundary18);
            }
        }
        if (kind == UiDrawSessionSetterKind::Kind4) {
            const std::uint64_t frame = frame_stack_pointer_;
            const std::uint64_t minimum = frame + 280U;
            const std::uint64_t maximum = frame + 288U;
            if (minimum > 4294967295ULL || maximum > 4294967295ULL ||
                cpu.read_gpr_u32(4U) != static_cast<std::uint32_t>(minimum) ||
                cpu.read_gpr_u32(5U) != static_cast<std::uint32_t>(maximum) || minimum + 8U > memory.size() ||
                maximum + 8U > memory.size()) {
                return seal(Reason::EntryMismatch, Boundary::Boundary18);
            }
        }
        ++observed_setter_count_;
        ++setter_count_;
        phase_ = 16U;
        UiDrawSessionSetterObservation setter;
        setter.kind = kind;
        setter.bucket_index = current_bucket_index_;
        setter.bucket = current_bucket_;
        setter.call_site = pending_direct_->call_site;
        setter.target = pending_direct_->target;
        setter.return_address = pending_direct_->return_address;
        setter.argument0 = cpu.read_gpr_u32(4U);
        setter.argument1 = cpu.read_gpr_u32(5U);
        return emit(Boundary::Boundary18, UiDrawSessionObservationEvent{setter});
    }
    // Roles 3 and above: the bucket-end observation after the callback set-up.
    if (!cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
        cpu.read_gpr_u32(17U) == 4294967295U) {
        UiDrawSessionSealReason target_reason = UiDrawSessionSealReason::None;
        if (!target_matches(memory, target_reason)) {
            return seal(target_reason, Boundary::Boundary30);
        }
        active_target_ = capture_active_ui_render_target_from_guest(memory);
        if (!active_target_.has_value()) { return seal(Reason::TargetUnavailable, Boundary::Boundary30); }
        phase_ = 27U;
        UiDrawSessionEndObservation end;
        end.bucket = current_bucket_;
        end.counter = cpu.read_gpr_u32(17U);
        end.target = *active_target_;
        return emit(Boundary::Boundary30, UiDrawSessionObservationEvent{end});
    }
    return seal(Reason::EntryMismatch, Boundary::Boundary30);
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_loop_control(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool at_loop_control = !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U &&
                                 cpu.read_gpr_u32(28U) == 5146224U &&
                                 cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                                 cpu.pc() == 1931176U;
    if (!at_loop_control) { return seal(Reason::EntryMismatch, Boundary::Boundary12); }
    if (phase_ == 9U) {
        processed_bucket_count_ = 0U;
        if (bucket_array_ != cpu.read_gpr_u32(16U) || retained_bucket_count_ != cpu.read_gpr_u32(17U)) {
            return seal(Reason::LoopProgressMismatch, Boundary::Boundary12);
        }
        if (retained_bucket_count_ != 0U) {
            const std::uint64_t bytes = static_cast<std::uint64_t>(retained_bucket_count_) * 16U;
            if ((bucket_array_ & 3U) != 0U || static_cast<std::uint64_t>(bucket_array_) + bytes > memory.size()) {
                return seal(Reason::LoopProgressMismatch, Boundary::Boundary12);
            }
        }
    } else {
        const std::uint64_t expected_bucket = static_cast<std::uint64_t>(bucket_array_) +
                                              static_cast<std::uint64_t>(processed_bucket_count_) * 16U;
        const std::uint32_t expected_remaining = retained_bucket_count_ - static_cast<std::uint32_t>(processed_bucket_count_);
        if (expected_bucket > 4294967295ULL || static_cast<std::uint32_t>(expected_bucket) != cpu.read_gpr_u32(16U) ||
            expected_remaining != cpu.read_gpr_u32(17U)) {
            return seal(Reason::LoopProgressMismatch, Boundary::Boundary12);
        }
    }
    const bool terminal = processed_bucket_count_ == static_cast<std::size_t>(retained_bucket_count_);
    if ((cpu.read_gpr_u32(17U) == 0U) != terminal) {
        return seal(Reason::LoopProgressMismatch, Boundary::Boundary12);
    }
    current_bucket_ = cpu.read_gpr_u32(16U);
    phase_ = terminal ? 26U : 10U;
    UiDrawSessionLoopControlObservation loop;
    loop.bucket_array = bucket_array_;
    loop.processed_bucket_count = processed_bucket_count_;
    loop.remaining_bucket_count = cpu.read_gpr_u32(17U);
    loop.terminal = terminal;
    return emit(Boundary::Boundary12, UiDrawSessionObservationEvent{loop});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_bucket_decision(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool at_decision = !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U &&
                             cpu.read_gpr_u32(28U) == 5146224U &&
                             cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                             cpu.read_gpr_u32(16U) == current_bucket_ && cpu.pc() == 1930792U;
    if (!at_decision || processed_bucket_count_ >= retained_bucket_count_ ||
        cpu.read_gpr_u32(17U) != static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) ||
        static_cast<std::uint64_t>(current_bucket_) + 16U > memory.size()) {
        return seal(Reason::LoopProgressMismatch, Boundary::Boundary13);
    }
    current_bucket_index_ = static_cast<std::uint32_t>(processed_bucket_count_);
    current_flags_ = cpu.read_gpr_u32(3U);
    const std::uint32_t clip = read_u32(memory, current_bucket_ + 8U);
    current_scissor_ = read_u32(memory, current_bucket_ + 12U);
    const bool active = cpu.read_gpr_u32(2U) != 0U;
    const std::uint32_t flags = read_u32(memory, current_bucket_);
    if (current_flags_ != flags) { return seal(Reason::FrameOutOfRange, Boundary::Boundary13); }
    const bool expected_active = (current_flags_ & 1U) == 0U && clip != 0U;
    if (expected_active != active) { return seal(Reason::FrameOutOfRange, Boundary::Boundary13); }
    expected_setter_count_ = 0U;
    observed_setter_count_ = 0U;
    if (active) {
        ++active_bucket_count_;
        phase_ = 12U;
    } else {
        phase_ = 11U;
    }
    UiDrawSessionBucketDecisionObservation decision;
    decision.bucket_index = static_cast<std::size_t>(current_bucket_index_);
    decision.bucket = current_bucket_;
    decision.flags = current_flags_;
    decision.clip = clip;
    decision.scissor = current_scissor_;
    decision.active = active;
    return emit(Boundary::Boundary13, UiDrawSessionObservationEvent{decision});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_flag_transition(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    current_flag_delta_ = current_flags_ ^ previous_flags_;
    const std::uint32_t flags = current_flags_;
    const std::uint32_t previous = previous_flags_;
    const std::uint32_t delta = current_flag_delta_;
    const bool matches = !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U &&
                         cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                         cpu.read_gpr_u32(16U) == current_bucket_ && cpu.pc() == 1930804U &&
                         processed_bucket_count_ < retained_bucket_count_ &&
                         cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
                         cpu.read_gpr_u32(3U) == flags && cpu.read_gpr_u32(21U) == previous &&
                         cpu.read_gpr_u32(18U) == delta && previous_scissor_ == cpu.read_gpr_u32(20U);
    if (!matches) { return seal(Reason::EntryMismatch, Boundary::Boundary15); }
    if ((delta & 102U) != 0U) {
        const std::uint32_t selector = delta & 38U;
        const std::uint32_t below = selector - 1U;
        std::uint32_t argument = 0U;
        if (((selector ^ below) > below) != false) {
            const int bit = __builtin_ffs(static_cast<int>(selector));
            const int index = bit - 2;
            if (index <= 4 && index >= 0) { argument = flag_argument_for_index(index); }
        }
        expected_setters_[expected_setter_count_] = ExpectedSetter{UiDrawSessionSetterKind::Kind0, argument, true};
        ++expected_setter_count_;
    }
    if ((delta & 72U) != 0U) {
        expected_setters_[expected_setter_count_] =
            ExpectedSetter{UiDrawSessionSetterKind::Kind1, (flags >> 3U) & 1U, true};
        ++expected_setter_count_;
    }
    if ((delta & 80U) != 0U) {
        expected_setters_[expected_setter_count_] =
            ExpectedSetter{UiDrawSessionSetterKind::Kind2, (flags >> 4U) & 1U, true};
        ++expected_setter_count_;
    }
    phase_ = 13U;
    expected_convergence_ = (expected_setter_count_ != 0U && expected_setters_[0].kind == UiDrawSessionSetterKind::Kind0)
                                ? 0U
                                : 1930920U;
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = Boundary::Boundary15;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::arm_direct_call(
    const PendingDirectCall& call, const UiDrawSessionBoundaryKind boundary) noexcept {
    pending_direct_ = call;
    phase_ = 4U;
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = boundary;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_setter_call(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const SetterCallSite* site = nullptr;
    for (const SetterCallSite& candidate : kSetterCallSites) {
        if (candidate.call_site == cpu.pc()) {
            site = &candidate;
            break;
        }
    }
    if (site == nullptr || cpu.is_executing_delay_slot()) { return seal(Reason::SetterRejected, Boundary::Boundary16); }
    const UiDrawSessionSetterKind kind = site->kind;
    const bool flags_are_current = kind == UiDrawSessionSetterKind::Kind3 || kind == UiDrawSessionSetterKind::Kind4;
    const bool bucket_state_matches =
        cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
        processed_bucket_count_ < retained_bucket_count_ &&
        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
        cpu.read_gpr_u32(18U) == current_flag_delta_;
    const bool state_matches = bucket_state_matches && previous_scissor_ == cpu.read_gpr_u32(20U) &&
                               cpu.read_gpr_u32(21U) == (flags_are_current ? current_flags_ : previous_flags_);
    if (!state_matches || expected_convergence_ != 0U || observed_setter_count_ >= expected_setter_count_ ||
        expected_setters_[observed_setter_count_].kind != kind) {
        return seal(Reason::SetterRejected, Boundary::Boundary16);
    }
    const ExpectedSetter& expected = expected_setters_[observed_setter_count_];
    if (expected.checks_argument &&
        (!site->argument.has_value() || *site->argument != expected.argument)) {
        return seal(Reason::SetterRejected, Boundary::Boundary16);
    }
    PendingDirectCall pending;
    pending.role = 2U;
    pending.call_site = site->call_site;
    pending.delay_slot = site->delay_slot;
    pending.target = site->target;
    pending.return_address = site->return_address;
    pending.setter_kind = kind;
    pending.setter_argument = site->argument;
    pending_direct_ = pending;
    phase_ = 4U;
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = Boundary::Boundary16;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_scissor_decision(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool bucket_state_matches =
        !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
        processed_bucket_count_ < retained_bucket_count_ &&
        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
        cpu.read_gpr_u32(18U) == current_flag_delta_;
    if (!bucket_state_matches || cpu.pc() != 1931028U || expected_convergence_ != 0U ||
        observed_setter_count_ != expected_setter_count_ || previous_flags_ != cpu.read_gpr_u32(21U) ||
        previous_scissor_ != cpu.read_gpr_u32(20U)) {
        return seal(Reason::SetterRejected, Boundary::Boundary20);
    }
    const std::uint32_t scissor = cpu.read_gpr_u32(2U);
    if (read_u32(memory, current_bucket_ + 12U) != scissor) {
        return seal(Reason::FrameOutOfRange, Boundary::Boundary20);
    }
    current_scissor_ = scissor;
    if (previous_scissor_ != scissor) {
        if (scissor != 0U) {
            const std::uint64_t begin = scissor;
            const bool misaligned = (scissor & 3U) != 0U;
            if (misaligned || begin + 32U > 4294967295ULL || begin + 32U + 16U > memory.size()) {
                return seal(Reason::FrameOutOfRange, Boundary::Boundary20);
            }
        }
        const UiDrawSessionSetterKind kind = current_scissor_ == 0U ? UiDrawSessionSetterKind::Kind3
                                                                    : UiDrawSessionSetterKind::Kind4;
        expected_setters_[expected_setter_count_] = ExpectedSetter{kind, 0U, false};
        ++expected_setter_count_;
        phase_ = 15U;
        expected_convergence_ = 0U;
    } else {
        phase_ = 17U;
        expected_convergence_ = 1931212U;
    }
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = Boundary::Boundary20;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_post_setter_head(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool bucket_state_matches =
        !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
        processed_bucket_count_ < retained_bucket_count_ &&
        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
        cpu.read_gpr_u32(18U) == current_flag_delta_;
    if (!bucket_state_matches || cpu.pc() != 1931216U || expected_convergence_ != 0U ||
        observed_setter_count_ != expected_setter_count_ || current_flags_ != cpu.read_gpr_u32(21U) ||
        previous_scissor_ != cpu.read_gpr_u32(20U) || cpu.read_gpr_u32(2U) != 1065353216U) {
        return seal(Reason::SetterRejected, Boundary::Boundary21);
    }
    post_setter_head_ = read_u32(memory, current_bucket_ + 8U);
    previous_flags_ = current_flags_;
    phase_ = 18U;
    UiDrawSessionBucketHeadObservation head;
    head.bucket_index = static_cast<std::size_t>(current_bucket_index_);
    head.bucket = current_bucket_;
    head.flags = current_flags_;
    head.scissor = current_scissor_;
    head.head = post_setter_head_;
    return emit(Boundary::Boundary21, UiDrawSessionObservationEvent{head});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_post_setter_head_commit(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const std::uint32_t expected_pc = post_setter_head_ == 0U ? 1931172U : 1931248U;
    const guest::Gpr128 unit = cpu.read_gpr(18U);
    const bool bucket_state_matches =
        !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
        processed_bucket_count_ < retained_bucket_count_ &&
        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
        unit.words == kUiDrawSessionOneVector && current_scissor_ == cpu.read_gpr_u32(20U) &&
        current_flags_ == cpu.read_gpr_u32(21U);
    if (!bucket_state_matches || cpu.pc() != expected_pc || post_setter_head_ != cpu.read_gpr_u32(19U)) {
        return seal(Reason::EntryMismatch, Boundary::Boundary22);
    }
    previous_scissor_ = current_scissor_;
    if (post_setter_head_ == 0U) {
        ++processed_bucket_count_;
        phase_ = 25U;
    } else {
        expected_node_ = post_setter_head_;
        current_node_index_ = 0U;
        if (workspace_ == nullptr) { return seal(Reason::WorkspaceUnavailable, Boundary::Boundary22); }
        workspace_->visit_.clear();
        phase_ = 19U;
    }
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = Boundary::Boundary22;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}


SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_callback_call(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const guest::Gpr128 unit = cpu.read_gpr(18U);
    const bool state = !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U &&
                       cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                       cpu.read_gpr_u32(16U) == current_bucket_ && processed_bucket_count_ < retained_bucket_count_ &&
                       cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
                       unit.words == kUiDrawSessionOneVector && current_scissor_ == cpu.read_gpr_u32(20U) &&
                       current_flags_ == cpu.read_gpr_u32(21U);
    if (!state || cpu.pc() != 1931432U || expected_node_ != cpu.read_gpr_u32(19U)) {
        return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary23);
    }
    if (callback_count_ == 65536U) { return seal(Reason::CallbackLimitExceeded, Boundary::Boundary23); }
    const guest::GuestAddress node = expected_node_;
    if ((node & 15U) != 8U || static_cast<std::uint64_t>(node) + 72U > memory.size() || workspace_ == nullptr) {
        return seal(Reason::FrameOutOfRange, Boundary::Boundary23);
    }
    if (node > 33554360U || !workspace_->visit_.insert(node)) {
        return seal(Reason::FrameOutOfRange, Boundary::Boundary23);
    }
    const std::uint32_t callback_address = read_u32(memory, node + 68U);
    const std::uint32_t descriptor = node + 40U;
    const std::uint32_t parent = read_u32(memory, node + 64U);
    const std::uint64_t parent_end = static_cast<std::uint64_t>(parent) + 32U;
    const std::uint64_t render_begin = static_cast<std::uint64_t>(frame_stack_pointer_) + 176U;
    const bool parent_engaged = parent_end <= 4294967295ULL;
    const bool render_engaged = render_begin <= 4294967295ULL;
    const std::uint32_t parent_address = parent_engaged ? static_cast<std::uint32_t>(parent_end) : 0U;
    const std::uint32_t render_address = render_engaged ? static_cast<std::uint32_t>(render_begin) : 0U;
    const bool ok = parent_engaged && render_engaged && callback_address != 0U && (callback_address & 3U) == 0U &&
                    (parent & 15U) == 0U && static_cast<std::uint64_t>(parent_address) + 32U <= memory.size() &&
                    static_cast<std::uint64_t>(render_address) + 32U <= memory.size() &&
                    parent == cpu.read_gpr_u32(3U) && callback_address == cpu.read_gpr_u32(2U) &&
                    descriptor == cpu.read_gpr_u32(5U) && current_scissor_ == cpu.read_gpr_u32(20U) &&
                    current_flags_ == cpu.read_gpr_u32(21U);
    if (!ok) { return seal(Reason::FrameOutOfRange, Boundary::Boundary23); }
    const TextDrawRenderRecordBits local = read_render_record(memory, node + 8U);
    TextDrawRenderRecordBits parent_bits_source;
    (void)parent_bits_source;
    UiTextParentTransformBits parent_bits;
    parent_bits.translation_x_bits = read_u32(memory, parent_address);
    parent_bits.translation_y_bits = read_u32(memory, parent_address + 4U);
    parent_bits.scale_x_bits = read_u32(memory, parent_address + 8U);
    parent_bits.scale_y_bits = read_u32(memory, parent_address + 12U);
    parent_bits.color.lanes[0] = read_u32(memory, parent_address + 16U);
    parent_bits.color.lanes[1] = read_u32(memory, parent_address + 20U);
    parent_bits.color.lanes[2] = read_u32(memory, parent_address + 24U);
    parent_bits.color.lanes[3] = read_u32(memory, parent_address + 28U);
    const TextDrawRenderRecordBits transformed = transform_ui_text_render_record(local, parent_bits);
    const TextDrawRenderRecordBits recorded = read_render_record(memory, render_address);
    if (transformed != recorded) { return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary23); }
    UiDrawSessionSealReason target_reason = UiDrawSessionSealReason::None;
    if (!target_matches(memory, target_reason)) {
        return seal(target_reason, Boundary::Boundary23);
    }

    UiDrawPostHierarchyCallback visit;
    visit.key.value = descriptor;
    visit.bucket_index = current_bucket_index_;
    visit.node_index = current_node_index_;
    visit.callback_address = callback_address;
    visit.descriptor = descriptor;
    visit.local_render = local;
    visit.parent = parent_bits;
    visit.text_effect = decode_ui_text_effect_callback(callback_address);
    const text_draw_guest_detail::UiTextCallbackBoundaryArmResult armed = callback_boundary_.arm_walker_visit(visit);
    if (armed.status != 0U || !armed.occurrence.has_value()) {
        return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary23);
    }
    std::optional<UiTextCallbackPayloadCodeProfileAvailability> payload;
    if (visit.text_effect.has_value() && workspace_ != nullptr) {
        payload = payload_code_profile_.observe_known_callback(memory, workspace_->payload_code_profile_cache_);
    }
    const UiTextCallbackPayloadCodeProfileAvailability payload_value =
        payload.value_or(UiTextCallbackPayloadCodeProfileAvailability::NotApplicable);
    PendingCallback pending;
    pending.node = node;
    pending.render_record = render_address;
    pending.occurrence = *armed.occurrence;
    pending.payload_profile = payload_value;
    pending_callback_ = pending;
    ++callback_count_;
    phase_ = 21U;
    UiDrawSessionCallbackCallObservation call;
    call.occurrence = *armed.occurrence;
    call.node = node;
    call.render_record = render_address;
    call.payload_profile = payload_value;
    return emit(Boundary::Boundary23, UiDrawSessionObservationEvent{call});
}



// Matches the outer frame: no delay slot, next PC is the next instruction, GP and SP match.
bool SubmitUiDrawBucketsSessionObserver::matches_outer_frame(const guest::R5900State& cpu) const noexcept {
    return !cpu.is_executing_delay_slot() && cpu.next_pc() == cpu.pc() + 4U &&
           cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_;
}

bool SubmitUiDrawBucketsSessionObserver::matches_current_bucket(const guest::R5900State& cpu) const noexcept {
    return matches_outer_frame(cpu) && cpu.read_gpr_u32(16U) == current_bucket_;
}

bool SubmitUiDrawBucketsSessionObserver::matches_bucket_counter(const guest::R5900State& cpu) const noexcept {
    if (processed_bucket_count_ >= retained_bucket_count_) { return false; }
    return cpu.read_gpr_u32(17U) ==
           static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U);
}

bool SubmitUiDrawBucketsSessionObserver::matches_active_bucket_state(const guest::R5900State& cpu) const noexcept {
    return matches_current_bucket(cpu) && matches_bucket_counter(cpu) && cpu.read_gpr_u32(18U) == current_flag_delta_;
}

bool SubmitUiDrawBucketsSessionObserver::matches_setter_state(const guest::R5900State& cpu,
                                                              const UiDrawSessionSetterKind kind) const noexcept {
    if (!matches_active_bucket_state(cpu)) { return false; }
    const bool flags_are_current = kind == UiDrawSessionSetterKind::Kind3 || kind == UiDrawSessionSetterKind::Kind4;
    return previous_scissor_ == cpu.read_gpr_u32(20U) &&
           cpu.read_gpr_u32(21U) == (flags_are_current ? current_flags_ : previous_flags_);
}

bool SubmitUiDrawBucketsSessionObserver::matches_callback_bucket_state(const guest::R5900State& cpu) const noexcept {
    return matches_current_bucket(cpu) && matches_bucket_counter(cpu) &&
           cpu.read_gpr(18U).words == kUiDrawSessionOneVector && current_scissor_ == cpu.read_gpr_u32(20U) &&
           current_flags_ == cpu.read_gpr_u32(21U);
}

bool SubmitUiDrawBucketsSessionObserver::target_matches(const guest::EeMemory& memory,
                                                        UiDrawSessionSealReason& reason) const noexcept {
    using Reason = UiDrawSessionSealReason;
    const std::optional<ActiveUiRenderTargetGuestBinding> current =
        capture_active_ui_render_target_from_guest(memory);
    if (!current.has_value()) {
        reason = Reason::TargetUnavailable;
        return false;
    }
    if (!active_target_.has_value()) {
        reason = Reason::TargetUnavailable;
        return false;
    }
    if (active_target_->target != current->target) {
        reason = Reason::Reason11;
        return false;
    }
    const UiRenderTargetView& expected = active_target_->view;
    const UiRenderTargetView& actual = current->view;
    const bool same_view = actual.identity.value == expected.identity.value && actual.width == expected.width &&
                           actual.height == expected.height && actual.origin_x == expected.origin_x &&
                           actual.origin_y == expected.origin_y;
    if (!same_view) {
        reason = Reason::Reason11;
        return false;
    }
    reason = Reason::None;
    return true;
}

std::optional<UiDrawSessionBoundaryKind> SubmitUiDrawBucketsSessionObserver::fixed_boundary_kind(
    const guest::GuestAddress pc) const noexcept {
    switch (pc) {
    case 1930688U: return UiDrawSessionBoundaryKind::Boundary1;
    case 1930720U: return UiDrawSessionBoundaryKind::Boundary2;
    case 1930724U: return UiDrawSessionBoundaryKind::Boundary3;
    case 1930728U: return UiDrawSessionBoundaryKind::Boundary4;
    case 1918848U: return UiDrawSessionBoundaryKind::Boundary5;
    case 1930732U: return UiDrawSessionBoundaryKind::Boundary6;
    case 1930736U: return UiDrawSessionBoundaryKind::Boundary7;
    case 1930740U: return UiDrawSessionBoundaryKind::Boundary8;
    case 1930744U: return UiDrawSessionBoundaryKind::Boundary10;
    case 1930748U: return UiDrawSessionBoundaryKind::Boundary11;
    case 1930792U: return UiDrawSessionBoundaryKind::Boundary13;
    case 1930804U: return UiDrawSessionBoundaryKind::Boundary15;
    case 1931028U: return UiDrawSessionBoundaryKind::Boundary20;
    case 1931172U: return UiDrawSessionBoundaryKind::Boundary14;
    case 1931176U: return UiDrawSessionBoundaryKind::Boundary12;
    case 1931188U: return UiDrawSessionBoundaryKind::Boundary28;
    case 1931192U: return UiDrawSessionBoundaryKind::Boundary29;
    case 1931196U: return UiDrawSessionBoundaryKind::Boundary31;
    case 1931216U: return UiDrawSessionBoundaryKind::Boundary21;
    case 1931248U: return UiDrawSessionBoundaryKind::Boundary22;
    case 1931432U: return UiDrawSessionBoundaryKind::Boundary23;
    case 1931436U: return UiDrawSessionBoundaryKind::Boundary24;
    case 1931440U: return UiDrawSessionBoundaryKind::Boundary26;
    case 1931444U: return UiDrawSessionBoundaryKind::Boundary27;
    case 1931484U: return UiDrawSessionBoundaryKind::Boundary32;
    case 1931488U: return UiDrawSessionBoundaryKind::Boundary33;
    case 2203424U: return UiDrawSessionBoundaryKind::Boundary30;
    case 2203888U: return UiDrawSessionBoundaryKind::Boundary9;
    default: return std::nullopt;
    }
}

bool SubmitUiDrawBucketsSessionObserver::admit_or_reuse_code_profile(const guest::EeMemory& memory) noexcept {
    if (workspace_ == nullptr) { return false; }
    if (workspace_->code_profile_.valid_for(memory)) {
        code_profile_ = workspace_->code_profile_;
        return true;
    }
    UiDrawSessionCodeProfileToken candidate{};
    candidate.profile_revision_ = 1U;
    for (std::size_t index = 0; index < kSessionReviewedCodePages.size(); ++index) {
        const auto capture = memory.capture_main_ram_code_page(kSessionReviewedCodePages[index]);
        if (capture.status != guest::EeMainRamCodePageCaptureStatus::Captured) { return false; }
        candidate.pages_[index] = capture.token;
    }
    for (const UiTextCallbackPayloadReviewedFunction& reviewed : kSessionReviewedFunctions) {
        const std::uint64_t begin = reviewed.address;
        const std::uint64_t end = reviewed.body_bytes + begin;
        std::optional<std::uint64_t> fingerprint;
        if (begin <= end && end <= memory.size()) {
            fingerprint = function_fingerprint(memory, reviewed.address, static_cast<std::size_t>(reviewed.body_bytes));
        }
        if (!fingerprint.has_value() || *fingerprint != reviewed.fingerprint) { return false; }
    }
    if (!candidate.valid_for(memory)) { return false; }
    workspace_->code_profile_ = candidate;
    if (workspace_->code_profile_refresh_count_ != 18446744073709551615ULL) { ++workspace_->code_profile_refresh_count_; }
    code_profile_ = candidate;
    return true;
}

void SubmitUiDrawBucketsSessionObserver::release_workspace() noexcept {
    if (workspace_ == nullptr) { return; }
    workspace_->visit_.clear();
    workspace_->claimed_ = false;
    workspace_ = nullptr;
}

SubmitUiDrawBucketsSessionObserver::SubmitUiDrawBucketsSessionObserver(
    SubmitUiDrawBucketsSessionWorkspace& workspace) noexcept {
    phase_ = 0U;
    seal_reason_ = UiDrawSessionSealReason::None;
    sealed_boundary_ = UiDrawSessionBoundaryKind::Boundary0;
    event_ordinal_ = 0U;
    owner_ = 0U;
    caller_stack_pointer_ = 0U;
    caller_return_address_ = 0U;
    frame_stack_pointer_ = 0U;
    bucket_array_ = 0U;
    retained_bucket_count_ = 0U;
    processed_bucket_count_ = 0U;
    active_bucket_count_ = 0U;
    setter_count_ = 0U;
    callback_count_ = 0U;
    current_bucket_ = 0U;
    current_bucket_index_ = 0U;
    current_flags_ = 0U;
    current_flag_delta_ = 0U;
    current_scissor_ = 0U;
    previous_flags_ = 64U;
    previous_scissor_ = 0U;
    post_setter_head_ = 0U;
    expected_node_ = 0U;
    current_node_index_ = 0U;
    expected_setters_ = {};
    expected_setter_count_ = 0U;
    observed_setter_count_ = 0U;
    expected_convergence_ = 0U;
    pending_direct_.reset();
    pending_callback_.reset();
    active_target_.reset();
    workspace_ = nullptr;
    code_profile_ = {};
    payload_code_profile_ = {};
    callback_boundary_ = {};
    if (workspace.claimed()) {
        phase_ = 32U;
        seal_reason_ = UiDrawSessionSealReason::WorkspaceUnavailable;
        sealed_boundary_ = UiDrawSessionBoundaryKind::Boundary0;
        return;
    }
    workspace.claim();
    workspace_ = &workspace;
}

SubmitUiDrawBucketsSessionObserver::~SubmitUiDrawBucketsSessionObserver() { release_workspace(); }

bool SubmitUiDrawBucketsSessionObserver::completed() const noexcept { return phase_ == 31U; }

bool SubmitUiDrawBucketsSessionObserver::requires_instruction_boundary(const guest::GuestAddress candidate_pc) const noexcept {
    switch (phase_) {
    case 31U:
    case 32U:
        return false;
    case 4U:
    case 5U:
    case 21U:
    case 22U:
    case 29U:
    case 30U:
        return true;
    default:
        return is_submit_ui_draw_buckets_session_static_boundary(candidate_pc);
    }
}

bool SubmitUiDrawBucketsSessionObserver::allows_atomic_native_dispatch() const noexcept {
    switch (phase_) {
    case 6U:
    case 7U:
    case 16U:
    case 23U:
    case 27U:
        return true;
    default:
        return false;
    }
}

bool SubmitUiDrawBucketsSessionObserver::sealed() const noexcept { return phase_ == 32U; }

UiDrawSessionSealReason SubmitUiDrawBucketsSessionObserver::seal_reason() const noexcept { return seal_reason_; }

std::uint64_t SubmitUiDrawBucketsSessionObserver::event_count() const noexcept { return event_ordinal_; }


SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_callback_delay_slot(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    if (pending_callback_.has_value() && cpu.is_executing_delay_slot() && cpu.pc() == 1931436U &&
        cpu.next_pc() == pending_callback_->occurrence.visit.callback_address &&
        frame_stack_pointer_ == cpu.read_gpr_u32(29U) && cpu.read_gpr_u32(28U) == 5146224U &&
        cpu.read_gpr_u32(16U) == current_bucket_ && processed_bucket_count_ < retained_bucket_count_ &&
        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
        cpu.read_gpr(18U).words == kUiDrawSessionOneVector &&
        pending_callback_->node == cpu.read_gpr_u32(19U) && current_scissor_ == cpu.read_gpr_u32(20U) &&
        current_flags_ == cpu.read_gpr_u32(21U) &&
        pending_callback_->occurrence.visit.callback_address == cpu.read_gpr_u32(2U) &&
        pending_callback_->occurrence.visit.descriptor == cpu.read_gpr_u32(5U) &&
        cpu.read_gpr_u32(31U) == 1931440U) {
        phase_ = 22U;
        ++event_ordinal_;
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Observed;
        result.boundary = Boundary::Boundary24;
        result.event_ordinal = event_ordinal_;
        result.seal_reason = UiDrawSessionSealReason::None;
        return result;
    }
    return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary24);
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_callback_entry(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    if (!pending_callback_.has_value() || cpu.is_executing_delay_slot() || !matches_callback_bucket_state(cpu) ||
        pending_callback_->node != cpu.read_gpr_u32(19U) || current_scissor_ != cpu.read_gpr_u32(20U) ||
        current_flags_ != cpu.read_gpr_u32(21U)) {
        return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary25);
    }
    UiDrawSessionSealReason target_reason = UiDrawSessionSealReason::None;
    if (!target_matches(memory, target_reason)) {
        return seal(target_reason, Boundary::Boundary25);
    }
    const UiDrawCallbackEntryAbiResult abi =
        observe_submit_ui_draw_callback_entry_abi_with_code_profile(cpu, memory, code_profile_);
    if (abi.status == UiDrawCallbackEntryAbiStatus::FunctionProfileRejected) {
        return seal(Reason::CodeProfileRejected, Boundary::Boundary25);
    }
    const text_draw_guest_detail::UiTextCallbackBoundaryObservationResult matched = callback_boundary_.consume_callback_entry_observation(abi, memory);
    const text_draw_guest_detail::UiTextCallbackBoundaryEntryResult* entry = matched.entry.has_value() ? &*matched.entry : nullptr;
    if (matched.status != 0U || entry == nullptr || !entry->occurrence.has_value() || !entry->entry.has_value() ||
        !entry->abi.has_value() || entry->entry->status > 1U ||
        entry->occurrence->source_ordinal != pending_callback_->occurrence.source_ordinal) {
        return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary25);
    }
    const UiDrawPostHierarchyCallback& expected = pending_callback_->occurrence.visit;
    const bool same_post = entry->expected_post.key.value == expected.key.value &&
                           entry->expected_post.bucket_index == expected.bucket_index &&
                           entry->expected_post.node_index == expected.node_index &&
                           entry->expected_post.callback_address == expected.callback_address &&
                           entry->expected_post.descriptor == expected.descriptor &&
                           entry->expected_post.local_render.layout_box == expected.local_render.layout_box &&
                           entry->expected_post.local_render.vertical_extent_bits == expected.local_render.vertical_extent_bits &&
                           entry->expected_post.local_render.color == expected.local_render.color &&
                           entry->expected_post.parent == expected.parent &&
                           entry->expected_effect == expected.text_effect;
    if (!same_post) { return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary25); }
    callback_entry_abi_ = entry->abi;
    pending_callback_->entry_status = entry->entry->status;
    phase_ = 23U;
    UiDrawSessionCallbackEntryObservation observation;
    observation.occurrence = pending_callback_->occurrence;
    observation.entry = *entry->abi;
    observation.status = entry->entry->status;
    return emit(Boundary::Boundary25, UiDrawSessionObservationEvent{observation});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_callback_return(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool matches = pending_callback_.has_value() && callback_entry_abi_.has_value() &&
                         pending_callback_->entry_status.has_value() && !cpu.is_executing_delay_slot() &&
                         cpu.pc() == 1931440U && matches_callback_bucket_state(cpu) &&
                         cpu.next_pc() == 1931444U && pending_callback_->node == cpu.read_gpr_u32(19U) &&
                         current_scissor_ == cpu.read_gpr_u32(20U) && current_flags_ == cpu.read_gpr_u32(21U) &&
                         cpu.read_gpr_u32(31U) == 1931440U;
    if (!matches) { return seal(Reason::CallbackBoundaryRejected, Boundary::Boundary26); }
    if (code_profile_.profile_revision_ != 1U || !code_profile_.valid_for(memory)) {
        return seal(Reason::CodeProfileRejected, Boundary::Boundary26);
    }
    UiDrawSessionSealReason target_reason = UiDrawSessionSealReason::None;
    if (!target_matches(memory, target_reason)) {
        return seal(target_reason, Boundary::Boundary26);
    }
    const guest::GuestAddress next_node = read_u32(memory, pending_callback_->node);
    expected_node_ = next_node;
    phase_ = 24U;
    UiDrawSessionCallbackReturnObservation observation;
    observation.occurrence = pending_callback_->occurrence;
    observation.entry = *callback_entry_abi_;
    observation.status = *pending_callback_->entry_status;
    observation.next_node = next_node;
    return emit(Boundary::Boundary26, UiDrawSessionObservationEvent{observation});
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_callback_next_commit(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool state = pending_callback_.has_value() && !cpu.is_executing_delay_slot() &&
                       cpu.next_pc() == cpu.pc() + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
                       cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(16U) == current_bucket_ &&
                       processed_bucket_count_ < retained_bucket_count_ &&
                       cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
                       cpu.read_gpr(18U).words == kUiDrawSessionOneVector && current_scissor_ == cpu.read_gpr_u32(20U) &&
                       current_flags_ == cpu.read_gpr_u32(21U);
    if (!state || cpu.pc() != 1931444U || expected_node_ != cpu.read_gpr_u32(19U)) {
        return seal(Reason::Reason9, Boundary::Boundary27);
    }
    pending_callback_.reset();
    if (expected_node_ == 0U) {
        ++processed_bucket_count_;
        phase_ = 25U;
    } else {
        ++current_node_index_;
        phase_ = 20U;
    }
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = Boundary::Boundary27;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}

SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_submit_return(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const bool at_return = !cpu.is_executing_delay_slot() && cpu.pc() == caller_return_address_ &&
                           cpu.next_pc() == caller_return_address_ + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
                           cpu.read_gpr_u32(29U) == caller_stack_pointer_ && cpu.read_gpr_u32(31U) == caller_return_address_;
    if (!at_return) { return seal(Reason::EntryMismatch, Boundary::Boundary34); }
    if (!code_profile_.valid_for(memory)) { return seal(Reason::CodeProfileRejected, Boundary::Boundary34); }
    SubmitUiDrawBucketsSessionSummary summary;
    summary.owner = owner_;
    summary.bucket_array = bucket_array_;
    summary.caller_stack_pointer = caller_stack_pointer_;
    summary.caller_return_address = caller_return_address_;
    summary.retained_bucket_count = retained_bucket_count_;
    summary.processed_bucket_count = processed_bucket_count_;
    summary.active_bucket_count = active_bucket_count_;
    summary.setter_count = setter_count_;
    summary.callback_count = callback_count_;
    summary.target = active_target_.value_or(ActiveUiRenderTargetGuestBinding{});
    return complete(UiDrawSessionObservationEvent{summary});
}


SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_direct_delay_slot(
    const guest::R5900State& cpu) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const std::uint8_t role = pending_direct_.has_value() ? pending_direct_->role : std::uint8_t{};
    bool setter_state = true;
    if (role == 2U) {
        setter_state = false;
        if (pending_direct_.has_value() && pending_direct_->setter_kind.has_value()) {
            const UiDrawSessionSetterKind kind = *pending_direct_->setter_kind;
            const bool flags_are_current = kind == UiDrawSessionSetterKind::Kind3 || kind == UiDrawSessionSetterKind::Kind4;
            setter_state = current_bucket_ == cpu.read_gpr_u32(16U) && processed_bucket_count_ < retained_bucket_count_ &&
                           cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
                           cpu.read_gpr_u32(18U) == current_flag_delta_ && previous_scissor_ == cpu.read_gpr_u32(20U) &&
                           cpu.read_gpr_u32(21U) == (flags_are_current ? current_flags_ : previous_flags_);
        }
    }
    const bool matches = pending_direct_.has_value() && !cpu.is_executing_delay_slot() &&
                         pending_direct_->delay_slot == cpu.pc() && pending_direct_->target == cpu.next_pc() &&
                         pending_direct_->return_address == cpu.read_gpr_u32(31U) &&
                         cpu.read_gpr_u32(28U) == 5146224U && frame_stack_pointer_ == cpu.read_gpr_u32(29U) &&
                         (role != 0U || (owner_ == cpu.read_gpr_u32(4U) && retained_bucket_count_ == cpu.read_gpr_u32(17U))) &&
                         setter_state;
    if (!matches) {
        Boundary boundary = Boundary::Boundary8;
        if (pending_direct_.has_value() && role == 2U) {
            boundary = Boundary::Boundary17;
        } else if (pending_direct_.has_value() && role == 3U) {
            boundary = Boundary::Boundary29;
        } else if (pending_direct_.has_value() && role == 0U) {
            boundary = Boundary::Boundary4;
        }
        return seal(Reason::EntryMismatch, boundary);
    }
    const Boundary boundary = role <= 3U ? direct_boundary_for_role(role) : Boundary::Boundary8;
    phase_ = 5U;
    ++event_ordinal_;
    SubmitUiDrawBucketsSessionObservation result;
    result.status = UiDrawSessionObservationStatus::Observed;
    result.boundary = boundary;
    result.event_ordinal = event_ordinal_;
    result.seal_reason = UiDrawSessionSealReason::None;
    return result;
}


SubmitUiDrawBucketsSessionObservation SubmitUiDrawBucketsSessionObserver::observe_instruction_boundary(
    const guest::R5900State& cpu, const guest::EeMemory& memory) noexcept {
    using Boundary = UiDrawSessionBoundaryKind;
    using Reason = UiDrawSessionSealReason;
    const guest::GuestAddress pc = cpu.pc();
    switch (phase_) {
    case 4U:
        return observe_direct_delay_slot(cpu);
    case 5U:
        return observe_direct_entry(cpu, memory);
    case 21U:
        return observe_callback_delay_slot(cpu);
    case 22U:
        return observe_callback_entry(cpu, memory);
    case 29U: {
        if (cpu.is_executing_delay_slot() && pc == 1931488U && cpu.next_pc() == caller_return_address_ &&
            cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
            cpu.read_gpr_u32(31U) == caller_return_address_) {
            phase_ = 30U;
            return observed(Boundary::Boundary33);
        }
        return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary33);
    }
    case 30U:
        return observe_submit_return(cpu, memory);
    case 31U: {
        SubmitUiDrawBucketsSessionObservation result;
        result.status = UiDrawSessionObservationStatus::Completed;
        result.boundary = Boundary::Boundary34;
        result.event_ordinal = event_ordinal_;
        return result;
    }
    case 32U:
        return sealed_observation();
    default:
        break;
    }

    // Phases 6, 7, 16, 23 and 27 are handled by their own GIMPLE branch.
    switch (phase_) {
    case 6U: {
        const guest::GuestAddress ra = pending_direct_.value_or(PendingDirectCall{}).return_address;
        if (ra == pc && !cpu.is_executing_delay_slot()) {
            if (!(cpu.next_pc() == pc + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
                  cpu.read_gpr_u32(29U) == frame_stack_pointer_ && owner_ == cpu.read_gpr_u32(16U) &&
                  retained_bucket_count_ == cpu.read_gpr_u32(17U) && ra == cpu.read_gpr_u32(31U))) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary6);
            }
            if (code_profile_.profile_revision_ != 1U || !code_profile_.valid_for(memory)) {
                return seal(Reason::CodeProfileRejected, Boundary::Boundary6);
            }
            pending_direct_.reset();
            phase_ = 3U;
            return observed(Boundary::Boundary6);
        }
        return static_boundary_observation(pc);
    }
    case 7U: {
        const guest::GuestAddress ra = pending_direct_.value_or(PendingDirectCall{}).return_address;
        if (ra == pc && !cpu.is_executing_delay_slot()) {
            if (!(cpu.next_pc() == pc + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
                  cpu.read_gpr_u32(29U) == frame_stack_pointer_ && owner_ == cpu.read_gpr_u32(16U) &&
                  retained_bucket_count_ == cpu.read_gpr_u32(17U) && cpu.read_gpr_u32(20U) == 0U &&
                  cpu.read_gpr_u32(21U) == 64U && cpu.next_pc() == ra + 4U && ra == cpu.read_gpr_u32(31U))) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary10);
            }
            if (code_profile_.profile_revision_ != 1U || !code_profile_.valid_for(memory)) {
                return seal(Reason::CodeProfileRejected, Boundary::Boundary10);
            }
            UiDrawSessionSealReason reason = UiDrawSessionSealReason::None;
            if (!target_matches(memory, reason)) { return seal(reason, Boundary::Boundary10); }
            pending_direct_.reset();
            phase_ = 8U;
            return observed(Boundary::Boundary10);
        }
        return static_boundary_observation(pc);
    }
    case 16U: {
        const PendingDirectCall pending = pending_direct_.value_or(PendingDirectCall{});
        if (pending.return_address == pc && !cpu.is_executing_delay_slot()) {
            if (!pending.setter_kind.has_value()) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary19);
            }
            if (!matches_setter_state(cpu, *pending.setter_kind)) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary19);
            }
            if (cpu.next_pc() != pending.return_address + 4U || cpu.read_gpr_u32(31U) != pending.return_address) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary19);
            }
            if (code_profile_.profile_revision_ != 1U || !code_profile_.valid_for(memory)) {
                return seal(Reason::CodeProfileRejected, Boundary::Boundary19);
            }
            const UiDrawSessionSetterKind kind = *pending.setter_kind;
            const bool core_known = expected_setter_count_ != 0U && observed_setter_count_ < expected_setter_count_;
            const UiDrawSessionSetterKind core = core_known ? expected_setters_[observed_setter_count_].kind
                                                            : UiDrawSessionSetterKind::Kind0;
            pending_direct_.reset();
            expected_convergence_ = 0U;
            if (kind == UiDrawSessionSetterKind::Kind0) {
                if (pending.return_address == 1930888U || pending.return_address == 1930904U) {
                    expected_convergence_ = 1930920U;
                } else if (!core_known || core != UiDrawSessionSetterKind::Kind1) {
                    expected_convergence_ = 1930972U;
                }
            } else if (kind == UiDrawSessionSetterKind::Kind1) {
                if (!core_known || core != UiDrawSessionSetterKind::Kind2) { expected_convergence_ = 1931024U; }
            }
            phase_ = (kind == UiDrawSessionSetterKind::Kind3 || kind == UiDrawSessionSetterKind::Kind4) ? 17U : 13U;
            return observed(Boundary::Boundary19);
        }
        return static_boundary_observation(pc);
    }
    case 23U: {
        if (pc == 1931440U) { return observe_callback_return(cpu, memory); }
        bool interesting = false;
        switch (pc) {
        case 1930688U:
        case 1930720U:
        case 1930724U:
        case 1930728U:
        case 1930732U:
        case 1930736U:
        case 1930740U:
        case 1930744U:
            interesting = true;
            break;
        default:
            for (const SetterCallSite& site : kSetterCallSites) {
                if (pc == site.call_site || pc == site.delay_slot || pc == site.return_address) {
                    interesting = true;
                    break;
                }
            }
            break;
        }
        if (!interesting) { return SubmitUiDrawBucketsSessionObservation{}; }
        return seal_unconditionally(Reason::Reason9, fixed_boundary_kind(pc).value_or(Boundary::Boundary0));
    }
    case 27U: {
        const guest::GuestAddress ra = pending_direct_.value_or(PendingDirectCall{}).return_address;
        if (ra == pc && !cpu.is_executing_delay_slot()) {
            if (!(cpu.next_pc() == pc + 4U && cpu.read_gpr_u32(28U) == 5146224U &&
                  cpu.read_gpr_u32(29U) == frame_stack_pointer_ && cpu.read_gpr_u32(17U) == 4294967295U &&
                  cpu.next_pc() == ra + 4U && ra == cpu.read_gpr_u32(31U))) {
                return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary31);
            }
            if (code_profile_.profile_revision_ != 1U || !code_profile_.valid_for(memory)) {
                return seal(Reason::CodeProfileRejected, Boundary::Boundary31);
            }
            UiDrawSessionSealReason reason = UiDrawSessionSealReason::None;
            if (!target_matches(memory, reason)) { return seal(reason, Boundary::Boundary31); }
            pending_direct_.reset();
            phase_ = 28U;
            return observed(Boundary::Boundary31);
        }
        return static_boundary_observation(pc);
    }
    default:
        break;
    }

    // Default chain (GIMPLE L14): pc-specific handlers, each gated on the phase.
    if (pc == 1930688U) {
        if (phase_ != 0U) { return seal_unconditionally(Reason::Reason5, Boundary::Boundary1); }
        return observe_submit_entry(cpu, memory);
    }
    if (pc == 1930720U && phase_ == 1U) {
        const std::optional<guest::GuestAddress> count_address = offset_address(owner_, 48U);
        const bool ok = !cpu.is_executing_delay_slot() && cpu.next_pc() == 1930724U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        cpu.read_gpr_u32(4U) == owner_ && count_address.has_value() &&
                        (*count_address & 3U) == 0U &&
                        static_cast<std::uint64_t>(*count_address) + 4U <= memory.size();
        if (!ok) { return seal(Reason::FrameOutOfRange, Boundary::Boundary2); }
        const std::uint32_t retained = read_u32(memory, *count_address);
        retained_bucket_count_ = retained;
        if (retained > 4096U) { return seal(Reason::CallbackLimitExceeded, Boundary::Boundary2); }
        phase_ = 2U;
        UiDrawSessionRetainedCountObservation count;
        count.owner = owner_;
        count.retained_count_address = *count_address;
        count.retained_bucket_count = retained;
        return emit(Boundary::Boundary2, UiDrawSessionObservationEvent{count});
    }
    if (pc == 1930724U && phase_ == 2U) {
        const bool ok = !cpu.is_executing_delay_slot() && cpu.next_pc() == 1930728U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        owner_ == cpu.read_gpr_u32(4U) && retained_bucket_count_ == cpu.read_gpr_u32(17U);
        if (!ok) { return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary3); }
        PendingDirectCall pending;
        pending.role = 0U;
        pending.call_site = 1930724U;
        pending.delay_slot = 1930728U;
        pending.target = 1918848U;
        pending.return_address = 1930732U;
        pending_direct_ = pending;
        phase_ = 4U;
        return observed(Boundary::Boundary3);
    }
    if (pc == 1930748U && phase_ == 8U) {
        const std::optional<guest::GuestAddress> bucket_address = offset_address(owner_, 44U);
        const bool ok = cpu.is_executing_delay_slot() && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.next_pc() == 1931176U &&
                        cpu.read_gpr_u32(16U) == owner_ && cpu.read_gpr_u32(17U) == retained_bucket_count_ &&
                        bucket_address.has_value() && (*bucket_address & 3U) == 0U &&
                        static_cast<std::uint64_t>(*bucket_address) + 4U <= memory.size();
        if (!ok) { return seal(Reason::FrameOutOfRange, Boundary::Boundary11); }
        bucket_array_ = read_u32(memory, *bucket_address);
        phase_ = 9U;
        return observed(Boundary::Boundary11);
    }
    if (pc == 1930736U) {
        if (phase_ != 3U) { return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary7); }
        const bool ok = !cpu.is_executing_delay_slot() && cpu.next_pc() == 1930740U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        owner_ == cpu.read_gpr_u32(16U) && cpu.read_gpr_u32(21U) == 64U &&
                        retained_bucket_count_ == cpu.read_gpr_u32(17U);
        if (!ok) { return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary7); }
        PendingDirectCall pending;
        pending.role = 1U;
        pending.call_site = 1930736U;
        pending.delay_slot = 1930740U;
        pending.target = 2203888U;
        pending.return_address = 1930744U;
        pending_direct_ = pending;
        phase_ = 4U;
        return observed(Boundary::Boundary7);
    }
    if (pc == 1931176U && (phase_ == 9U || phase_ == 25U)) { return observe_loop_control(cpu, memory); }
    if (pc == 1930792U && phase_ == 10U) { return observe_bucket_decision(cpu, memory); }
    if (pc == 1931172U && phase_ == 11U) {
        const bool ok = !cpu.is_executing_delay_slot() && cpu.next_pc() == 1931176U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        cpu.read_gpr_u32(16U) == current_bucket_ &&
                        processed_bucket_count_ < retained_bucket_count_ &&
                        cpu.read_gpr_u32(17U) == static_cast<std::uint32_t>(retained_bucket_count_ - processed_bucket_count_ - 1U) &&
                        cpu.read_gpr_u32(21U) == previous_flags_ && cpu.read_gpr_u32(20U) == previous_scissor_;
        if (!ok) { return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary14); }
        ++processed_bucket_count_;
        phase_ = 25U;
        return observed(Boundary::Boundary14);
    }
    if (pc == 1930804U && phase_ == 12U) { return observe_flag_transition(cpu); }
    {
        bool setter_site = false;
        for (const SetterCallSite& site : kSetterCallSites) {
            if (pc == site.call_site) {
                setter_site = true;
                break;
            }
        }
        if (setter_site && (phase_ == 13U || phase_ == 15U)) { return observe_setter_call(cpu); }
    }
    if (pc == 1931028U && phase_ == 13U) { return observe_scissor_decision(cpu, memory); }
    if (pc == 1931216U && (phase_ == 15U || phase_ == 17U)) { return observe_post_setter_head(cpu, memory); }
    if ((pc == 1931172U || pc == 1931248U) && phase_ == 18U) { return observe_post_setter_head_commit(cpu); }
    if (pc == 1931432U && phase_ == 20U) { return observe_callback_call(cpu, memory); }
    if (pc == 1931444U) {
        if (phase_ == 19U) {
            if (matches_callback_bucket_state(cpu) && expected_node_ == cpu.read_gpr_u32(19U)) {
                phase_ = 20U;
                return observed(Boundary::Boundary27);
            }
            return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary27);
        }
        if (phase_ == 24U) { return observe_callback_next_commit(cpu); }
    }
    if (pc == 1931188U && phase_ == 26U) {
        const std::uint64_t final_bucket = static_cast<std::uint64_t>(bucket_array_) +
                                           static_cast<std::uint64_t>(retained_bucket_count_) * 16U;
        const bool ok = cpu.is_executing_delay_slot() && cpu.next_pc() == 1931192U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        final_bucket <= 4294967295ULL &&
                        static_cast<std::uint32_t>(final_bucket) == cpu.read_gpr_u32(16U) &&
                        cpu.read_gpr_u32(17U) == 4294967295U;
        if (!ok) { return seal_unconditionally(Reason::LoopProgressMismatch, Boundary::Boundary28); }
        current_bucket_ = static_cast<std::uint32_t>(final_bucket);
        PendingDirectCall pending;
        pending.role = 3U;
        pending.call_site = 1931188U;
        pending.delay_slot = 1931192U;
        pending.target = 2203424U;
        pending.return_address = 1931196U;
        pending_direct_ = pending;
        phase_ = 4U;
        return observed(Boundary::Boundary28);
    }
    if (pc == 1931484U && phase_ == 28U) {
        const bool ok = !cpu.is_executing_delay_slot() && cpu.next_pc() == 1931488U &&
                        cpu.read_gpr_u32(28U) == 5146224U && cpu.read_gpr_u32(29U) == frame_stack_pointer_ &&
                        caller_return_address_ == cpu.read_gpr_u32(31U);
        if (!ok) { return seal_unconditionally(Reason::EntryMismatch, Boundary::Boundary32); }
        phase_ = 29U;
        return observed(Boundary::Boundary32);
    }

    // Unexpected boundary: a fixed boundary outside the phase's own transitions.
    const std::optional<Boundary> unexpected = fixed_boundary_kind(pc);
    if (!unexpected.has_value()) { return SubmitUiDrawBucketsSessionObservation{}; }
    const bool group13 = phase_ == 13U && expected_convergence_ == pc && matches_active_bucket_state(cpu) &&
                         previous_scissor_ == cpu.read_gpr_u32(20U) && previous_flags_ == cpu.read_gpr_u32(21U) &&
                         (pc == 1930920U || pc == 1930972U || pc == 1931024U);
    const bool phase17 = phase_ == 17U && expected_convergence_ == pc && matches_active_bucket_state(cpu) &&
                         previous_scissor_ == cpu.read_gpr_u32(20U) && current_flags_ == cpu.read_gpr_u32(21U) &&
                         pc == 1931212U;
    if (!group13 && !phase17) { return seal_unconditionally(Reason::Reason5, *unexpected); }
    expected_convergence_ = 0U;
    const bool core_known = expected_setter_count_ != 0U && observed_setter_count_ < expected_setter_count_;
    const UiDrawSessionSetterKind core = core_known ? expected_setters_[observed_setter_count_].kind
                                                    : UiDrawSessionSetterKind::Kind0;
    if (pc == 1930920U) {
        if (!(core_known && core == UiDrawSessionSetterKind::Kind1)) { expected_convergence_ = 1930972U; }
    } else if (pc == 1930972U) {
        if (!(core_known && core == UiDrawSessionSetterKind::Kind2)) { expected_convergence_ = 1931024U; }
    }
    return SubmitUiDrawBucketsSessionObservation{};
}

} // namespace takedown::runtime::slus_21050::ui_draw_guest_detail

} // namespace takedown::runtime::slus_21050
