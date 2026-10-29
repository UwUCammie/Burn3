// Runtime support for ahead-of-time translated VU1 launches.
//
// This file is included into the anonymous namespace of
// vu1_micro_interpreter.cpp immediately before the generated module named by
// TAKEDOWN_LOCAL_VU1_AOT_MODULE (tools/vu1_aot_generator.cpp). Generated code
// keeps the VU register file in function-local values, resolves every
// pipeline hazard at generation time, and calls the small kernels below with
// values rather than references so those locals never escape. Each kernel
// uses the same primitive sequence as the interpreter's launcher-default
// configuration (native-float + SIMD upper stages, scalar chop helpers for
// the remaining families); the differential verifier
// (tools/verify_vu1_aot.cpp) holds the two bit-identical.
//
// A translated function leaves through vu1_aot_leave() with a static site
// record describing the pipeline shape at that pair boundary plus a frame of
// the values which travel with that shape. Completion reproduces the E-bit
// exit of commit_pair; a deoptimization rebuilds the live Vu1ExecutionMachine
// so the interpreter loop continues as if it had executed the prefix itself.
//
// STATUS is maintained exactly. Its current nibble derives from the newest
// lazy MAC slot and its FDIV bits from the Q publications. The sticky nibble
// only ever gains bits:
//  - zero and sign are a launch precondition: the dispatcher declines until
//    both are already set, after which no MAC update can change them;
//  - underflow cannot come from a native-float kernel, whose result is never
//    denormal under the launch MXCSR (flush-to-zero with masked underflow,
//    also checked by the dispatcher); the chop kernels judge it explicitly;
//  - overflow is tested on every native-float result (the infinity test the
//    clamp already performs; the kernels put back the infinity which rounding
//    toward zero turns into Fmax) and judged explicitly by the chop kernels.
// Translated code stamps the issue cycle of the first underflow and overflow
// event, which is all vu1_aot_leave() needs to rebuild the four delayed
// STATUS stages exactly.

#if !defined(__SSE2__)
#error "The VU1 AOT tier requires SSE2"
#endif

#if defined(TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY)
// vu1_aot_leave() rebuilds the sparse VI branch history (one entry per VI
// write, read with an issue-cycle adjacency test). The dense A/B baseline
// keeps one entry per issued pair and reads it without that test, so a
// rebuilt sparse ring would be misread after a deoptimization.
#error "The VU1 AOT tier requires the sparse VI branch history: do not combine TAKEDOWN_LOCAL_VU1_AOT_MODULE with TAKEDOWN_VU1_DENSE_VI_BRANCH_HISTORY"
#endif

static_assert(std::endian::native == std::endian::little);

enum class Vu1AotExit : std::uint8_t {
    Declined,    // nothing executed, nothing mutated
    Finished,    // terminal Vu1RunResult produced
    Deoptimized, // machine rebuilt at a pair boundary; interpreter continues
};

struct Vu1AotOutcome {
    Vu1AotExit exit{Vu1AotExit::Declined};
    std::uint8_t reason{};
};

using Vu1AotFunction = Vu1AotOutcome (*)(Vu1ExecutionMachine& machine,
                                         Vu1RunResult& result,
                                         std::uint32_t entry_index);

// Byte range of MicroMem one translation depends on: every pair it translated
// or inspected, including deoptimization sites and dynamic-branch candidates.
struct Vu1AotRange {
    std::uint16_t first_pair{};
    std::uint16_t pair_count{};
    const unsigned char* bytes{};
};

struct Vu1AotDescriptor {
    std::uint16_t entry{};
    std::uint16_t resolved_entry{};
    std::uint32_t entry_index{};
    // The interpreter's lazy tier stages ten upper families through scalar
    // chop arithmetic while its predecoded tier uses the native-float stages.
    // A translation containing one of them matches only the predecoded tier.
    bool requires_upper_stage_tier{};
    // Generated with --flushed-clamps: the kernels lean on MXCSR FTZ|DAZ
    // (with underflow and denormal exceptions masked) for tiny values.
    bool requires_flushed_environment{};
    Vu1AotFunction function{};
    std::span<const Vu1AotRange> ranges{};
};

// Defined by the generated module. Coverage marks are one byte per block
// variant, set when it executes; empty unless generated with --coverage.
[[nodiscard]] std::span<const Vu1AotDescriptor> vu1_aot_descriptors() noexcept;
[[nodiscard]] std::span<const std::uint8_t> vu1_aot_coverage_marks() noexcept;

[[nodiscard]] const Vu1AotDescriptor* resolve_vu1_aot_descriptor(
    const std::span<const std::byte> micro_memory, const std::uint16_t entry,
    const std::uint16_t resolved_entry) noexcept {
    for (const auto& descriptor : vu1_aot_descriptors()) {
        if (descriptor.entry != entry || descriptor.resolved_entry != resolved_entry) {
            continue;
        }
        const bool identical = std::ranges::all_of(
            descriptor.ranges, [&](const Vu1AotRange& range) {
                const auto offset = static_cast<std::size_t>(range.first_pair) * 8U;
                const auto length = static_cast<std::size_t>(range.pair_count) * 8U;
                return offset + length <= micro_memory.size() &&
                       std::memcmp(micro_memory.data() + offset, range.bytes, length) == 0;
            });
        if (identical) {
            return &descriptor;
        }
    }
    return nullptr;
}

// ---- Leaving translated code ---- //

enum class Vu1AotSiteKind : std::uint8_t {
    Completed,
    OperandFault,
    DeoptPairLimit,
    DeoptDynamicTarget,
    DeoptUntranslatedPair,
    DeoptVariantLimit,
};

struct Vu1AotReadyLane {
    std::uint8_t lane{};      // flattened VF lane, register * 4 + lane
    std::uint8_t remaining{}; // cycles until readable, 1..3
};

struct Vu1AotHistoryShape {
    std::uint8_t reg{};
    std::uint8_t age{}; // elapsed cycles since the writing pair issued, 1..4
    bool backup_eligible{};
};

// Static description of one pair boundary. Remaining-cycle fields are relative
// to the elapsed-cycle count of the frame which accompanies the site.
struct Vu1AotSite {
    Vu1AotSiteKind kind{};
    // Completed/OperandFault: the final pair. Deopt: the resume PC (ignored
    // for DeoptDynamicTarget, which resumes at the frame's dynamic target).
    std::uint16_t pc{};
    // Last pair begun before the boundary and its words.
    std::uint16_t last_pc{};
    std::uint32_t last_lower{};
    std::uint32_t last_upper{};
    std::int8_t pending_q_remaining{-1}; // -1: no pending Q
    std::int8_t pending_p_remaining{-1};
    std::uint8_t history_count{};
    std::array<Vu1AotHistoryShape, 4U> history{}; // newest first
    std::array<std::uint8_t, 16U> vi_remaining{};
    std::span<const Vu1AotReadyLane> vf_remaining{};
};

// Marks a lazy MAC slot which still holds a launch-entry value: no MAC update
// had committed by the cycle that slot describes. vu1_aot_mac() ignores it.
inline constexpr std::uint32_t kVu1AotInitialSlot = 0x00010000U;
// "No such cycle" for the first-event stamps of the sticky STATUS bits.
inline constexpr std::size_t kVu1AotNever = ~std::size_t{0U};
// No Q publication has happened in this launch.
inline constexpr std::uint32_t kVu1AotNoFdiv = 0x80000000U;

// Values which travel with a site's shape. MAC slots stay lazy: translated
// code keeps the arithmetic result which produced each flag word and the
// flags are only formed when something observes them.
struct Vu1AotFrame {
    std::size_t committed{};
    std::size_t stall_cycles{};
    std::uint32_t pending_q{};
    std::uint32_t pending_q_status{};
    std::uint32_t pending_p{};
    std::uint32_t dynamic_target{};
    std::array<std::uint32_t, 4U> history_previous{}; // newest first
    std::array<std::uint32_t, 4U> clip{};             // oldest (visible) first
    __m128i mac_source[5U]{};                         // four ring slots, then logical
    std::array<std::uint32_t, 5U> mac_mask{};
    std::array<std::uint32_t, 5U> mac_explicit{};
    // Issue cycle of the first MAC update which raised the sticky underflow
    // and the sticky overflow STATUS bit, or kVu1AotNever.
    std::size_t underflow_cycle{};
    std::size_t overflow_cycle{};
    // STATUS Invalid/Divide bits of the last Q publication (kVu1AotNoFdiv
    // when none happened) and of all of them together.
    std::uint32_t fdiv_last{};
    std::uint32_t fdiv_any{};
    bool xgkick_service_needed{};
};

// MAC word of one lazy slot. `source` is the raw native-float result or the
// stored result of a chop kernel; both yield the interpreter's Z/S bits, and
// only a raw native result can carry value-derived U/O. `explicit_bits` holds
// a launch-history word or the U/O judgments of a chop kernel.
[[gnu::always_inline]] inline std::uint32_t vu1_aot_mac(
    const __m128i source, const std::uint32_t mask,
    const std::uint32_t explicit_bits) noexcept {
    const auto bits = _mm_shuffle_epi32(source, _MM_SHUFFLE(0, 1, 2, 3));
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const auto magnitude_zero = _mm_cmpeq_epi32(
        _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_setzero_si128());
    const auto underflow = _mm_andnot_si128(magnitude_zero, zero);
    const auto overflow = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    const auto lanes = [](const __m128i value) {
        return static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(value)));
    };
    const auto flags = lanes(zero) | (lanes(bits) << 4U) | (lanes(underflow) << 8U) |
                       (lanes(overflow) << 12U);
    return (flags & mask) | explicit_bits;
}

// Lanes (bit 0 = X .. bit 3 = W) whose raw native-float result overflowed:
// exactly the O predicate of vu1_aot_mac(), and the comparison the clamp of
// the same result performs anyway.
[[gnu::always_inline]] inline int vu1_aot_overflow_lanes(const __m128i raw) noexcept {
    const auto exponent_mask = _mm_set1_epi32(0x7f800000);
    return _mm_movemask_ps(_mm_castsi128_ps(
        _mm_cmpeq_epi32(_mm_and_si128(raw, exponent_mask), exponent_mask)));
}

// The issue cycle of the first sticky STATUS event: issue cycles only grow
// within a launch, so an existing stamp wins. Deliberately an out-of-line
// call: it keeps the compiler from folding the rare stamp into branch-free
// code on the path of every MAC update.
[[gnu::noinline, gnu::cold]] std::size_t vu1_aot_first_cycle(
    const std::size_t stamp, const bool raised, const std::size_t cycle) noexcept {
    return raised && stamp == kVu1AotNever ? cycle : stamp;
}

[[gnu::noinline]] Vu1AotOutcome vu1_aot_leave(
    Vu1ExecutionMachine& machine, Vu1RunResult& result, const Vu1AotSite& site,
    const Vu1AotFrame& frame) {
    auto& state = machine.state;
    const auto elapsed = frame.committed + frame.stall_cycles;
    const auto logical_mac = static_cast<std::uint16_t>(vu1_aot_mac(
        frame.mac_source[4U], frame.mac_mask[4U], frame.mac_explicit[4U]));

    // The logical STATUS as the interpreter holds it after the advance with
    // 0-based index `advance` (the pair issued at that elapsed cycle, or a
    // stall cycle), given the lazy MAC slot which that advance pushed.
    // Translated code never writes status_flag_history, so it still holds the
    // launch values. update_fmac() and merge_fdiv() touch disjoint bits, so
    // their order does not matter.
    const auto status_after = [&](const std::size_t advance, const std::size_t slot,
                                  const bool publish_pending) {
        DeferredVu1Status status{state.status_flag_history[3U]};
        if ((frame.mac_mask[slot] & kVu1AotInitialSlot) == 0U) {
            // Sticky zero and sign are already set (launch precondition).
            const auto sticky = static_cast<std::uint16_t>(
                (frame.underflow_cycle <= advance ? 0x0100U : 0U) |
                (frame.overflow_cycle <= advance ? 0x1000U : 0U));
            status.update_fmac(sticky);
            status.update_fmac(static_cast<std::uint16_t>(vu1_aot_mac(
                frame.mac_source[slot], frame.mac_mask[slot], frame.mac_explicit[slot])));
        }
        if (frame.fdiv_last != kVu1AotNoFdiv) {
            // Successive publications replace the current Invalid/Divide pair
            // and accumulate the sticky pair.
            status.merge_fdiv(frame.fdiv_any);
            status.merge_fdiv(frame.fdiv_last);
        }
        if (publish_pending) {
            status.merge_fdiv(frame.pending_q_status);
        }
        return status;
    };

    if (site.kind == Vu1AotSiteKind::Completed) {
        // commit_pair's E-bit completion. At least the two exit pairs ran.
        const auto status =
            status_after(elapsed - 1U, 4U, site.pending_q_remaining >= 0).value();
        state.control[kControlTpc] = static_cast<std::uint16_t>((site.pc + 1U) & kPairMask);
        if (site.pending_q_remaining >= 0) {
            state.control[kControlQ] = frame.pending_q;
        }
        if (site.pending_p_remaining >= 0) {
            state.control[kControlP] = frame.pending_p;
        }
        state.mac_flag_history.fill(logical_mac);
        state.clip_flag_history.fill(frame.clip[3U]);
        state.status_flag_history.fill(status);
        state.control[kControlMac] = logical_mac;
        state.control[kControlClip] = frame.clip[3U];
        state.control[kControlStatus] = status;
        state.flag_history_initialized = true;
        machine.services.synchronize_timed_execution(elapsed);
        static_cast<void>(machine.services.flush_xgkick());
        result = Vu1RunResult{
            .status = Vu1RunStatus::Completed,
            .pc = site.pc,
            .lower_word = site.last_lower,
            .upper_word = site.last_upper,
            .committed_pairs = frame.committed,
            .elapsed_cycles = elapsed,
            .pipeline_stall_cycles = frame.stall_cycles,
        };
        return {.exit = Vu1AotExit::Finished};
    }

    // Rebuild the machine at this boundary. Entries the generator dropped
    // from the shape cannot influence any continuation of the same image.
    machine.pc = static_cast<std::uint16_t>(
        site.kind == Vu1AotSiteKind::DeoptDynamicTarget ? frame.dynamic_target & kPairMask
                                                        : site.pc);
    machine.committed = frame.committed;
    machine.pipeline_stall_cycles = frame.stall_cycles;
    machine.elapsed_cycles = elapsed;
    for (const auto ready : site.vf_remaining) {
        machine.vf_ready_cycles[ready.lane] = elapsed + ready.remaining;
    }
    for (std::size_t reg = 1U; reg < site.vi_remaining.size(); ++reg) {
        if (site.vi_remaining[reg] != 0U) {
            machine.vi_load_ready_cycles[reg] = elapsed + site.vi_remaining[reg];
        }
    }
    for (std::size_t index = 0U; index < site.history_count; ++index) {
        const auto newest_first = site.history_count - 1U - index;
        const auto& shape = site.history[newest_first];
        machine.vi_branch_history[index] = ViBranchHistoryEntry{
            .issue_cycle = elapsed - shape.age,
            .previous_value = static_cast<std::uint16_t>(frame.history_previous[newest_first]),
            .written_reg = shape.reg,
            .backup_eligible = shape.backup_eligible,
        };
    }
    machine.vi_branch_history_size = site.history_count;
    machine.vi_branch_history_next = site.history_count & 3U;
    if (site.pending_q_remaining >= 0) {
        machine.pending_q = PendingQ{
            .bits = frame.pending_q,
            .status_di = frame.pending_q_status,
            .ready_cycle = elapsed + static_cast<std::size_t>(site.pending_q_remaining),
        };
    }
    if (site.pending_p_remaining >= 0) {
        machine.pending_p = PendingP{
            .bits = frame.pending_p,
            .ready_cycle = elapsed + static_cast<std::size_t>(site.pending_p_remaining),
        };
    }
    for (std::size_t stage = 0U; stage < 4U; ++stage) {
        machine.flag_pipelines.mac[stage] = vu1_aot_mac(
            frame.mac_source[stage], frame.mac_mask[stage], frame.mac_explicit[stage]);
        machine.flag_pipelines.clip[stage] = frame.clip[stage];
        // Stage k was pushed by advance elapsed - 4 + k; before that it
        // still holds a launch history entry, which only publications touch.
        if (elapsed + stage >= 4U) {
            machine.flag_pipelines.status[stage] =
                status_after(elapsed + stage - 4U, stage, false);
        } else {
            DeferredVu1Status entry{state.status_flag_history[elapsed + stage]};
            if (frame.fdiv_last != kVu1AotNoFdiv) {
                entry.merge_fdiv(frame.fdiv_any);
                entry.merge_fdiv(frame.fdiv_last);
            }
            machine.flag_pipelines.status[stage] = entry;
        }
    }
    machine.flag_pipelines.oldest = 0U;
    machine.logical_mac = logical_mac;
    machine.logical_clip = frame.clip[3U];
    // The newest stage is the logical value once any cycle has elapsed;
    // before that the machine still holds its launch value.
    if (elapsed != 0U) {
        machine.logical_status = machine.flag_pipelines.status[3U];
    }
    machine.xgkick_service_needed = frame.xgkick_service_needed;
    machine.last_lower = site.last_lower;
    machine.last_upper = site.last_upper;
    state.control[kControlTpc] = site.last_pc;

    if (site.kind == Vu1AotSiteKind::OperandFault) {
        // finish_pair's refused-XGKICK exit: the pair has issued (stall and
        // Q/P publication included) but nothing of it commits.
        machine.persist_pending_state(false);
        result = Vu1RunResult{
            .status = Vu1RunStatus::UnsupportedOperandCase,
            .slot = Vu1Slot::Lower,
            .pc = site.pc,
            .lower_word = site.last_lower,
            .upper_word = site.last_upper,
            .committed_pairs = frame.committed,
            .elapsed_cycles = elapsed,
            .pipeline_stall_cycles = frame.stall_cycles,
        };
        return {.exit = Vu1AotExit::Finished};
    }

    Vu1AotDeoptReason reason{};
    switch (site.kind) {
    case Vu1AotSiteKind::DeoptDynamicTarget: reason = Vu1AotDeoptReason::DynamicTarget; break;
    case Vu1AotSiteKind::DeoptUntranslatedPair: reason = Vu1AotDeoptReason::UntranslatedPair; break;
    case Vu1AotSiteKind::DeoptVariantLimit: reason = Vu1AotDeoptReason::VariantLimit; break;
    default: reason = Vu1AotDeoptReason::PairLimit; break;
    }
    return {.exit = Vu1AotExit::Deoptimized, .reason = static_cast<std::uint8_t>(reason)};
}

// The per-pair XGKICK/GIF service step of commit_pair, entered only while the
// machine's service gate is set. Returns the refreshed gate.
[[gnu::noinline, gnu::cold]] bool vu1_aot_service_pair(
    const Vu1ExecutionServices& services, const std::uint64_t elapsed_cycles) {
    if (services.xgkick_active()) {
        services.synchronize_timed_execution(elapsed_cycles);
    }
    services.advance_xgkick(1U);
    return services.xgkick_nonzero_advance_needed();
}

// The XGKICK lower stage. Returns false when the producer refuses the kick.
[[gnu::noinline]] bool vu1_aot_submit_xgkick(
    const Vu1ExecutionServices& services, const std::uint64_t elapsed_cycles,
    const std::uint16_t vector_address) {
    services.synchronize_timed_execution(elapsed_cycles);
    return services.submit_xgkick(vector_address);
}

// ---- Register and memory access ---- //

[[gnu::always_inline]] inline __m128i vu1_aot_load_vector(const Gpr128& value) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.words.data()));
}

[[gnu::always_inline]] inline void vu1_aot_store_vector(Gpr128& target,
                                                        const __m128i value) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(target.words.data()), value);
}

// VF0 reads as (0, 0, 0, 1.0).
[[gnu::always_inline]] inline __m128i vu1_aot_vf0() noexcept {
    return _mm_set_epi32(0x3f800000, 0, 0, 0);
}

// Lane selector for a destination mask (X=8 .. W=1).
template <unsigned Mask>
[[gnu::always_inline]] inline __m128i vu1_aot_lane_mask() noexcept {
    return _mm_set_epi32((Mask & 1U) != 0U ? -1 : 0, (Mask & 2U) != 0U ? -1 : 0,
                         (Mask & 4U) != 0U ? -1 : 0, (Mask & 8U) != 0U ? -1 : 0);
}

template <unsigned Mask>
[[gnu::always_inline]] inline __m128i vu1_aot_merge(const __m128i previous,
                                                    const __m128i value) noexcept {
    if constexpr (Mask == 0x0fU) {
        return value;
    } else if constexpr (Mask == 0U) {
        return previous;
    } else {
        return vu1_detail::simd_select(vu1_aot_lane_mask<Mask>(), value, previous);
    }
}

template <unsigned Lane>
[[gnu::always_inline]] inline std::uint32_t vu1_aot_lane(const __m128i value) noexcept {
    if constexpr (Lane == 0U) {
        return static_cast<std::uint32_t>(_mm_cvtsi128_si32(value));
    } else {
        return static_cast<std::uint32_t>(_mm_cvtsi128_si32(
            _mm_shuffle_epi32(value, _MM_SHUFFLE(Lane, Lane, Lane, Lane))));
    }
}

template <unsigned Lane>
[[gnu::always_inline]] inline __m128i vu1_aot_broadcast(const __m128i value) noexcept {
    return _mm_shuffle_epi32(value, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
}

[[gnu::always_inline]] inline __m128i vu1_aot_splat(const std::uint32_t value) noexcept {
    return _mm_set1_epi32(static_cast<int>(value));
}

// vu1_data_address() over a local VI word.
[[gnu::always_inline]] inline std::size_t vu1_aot_address(
    const std::uint32_t integer, const std::int32_t immediate) noexcept {
    const auto signed_base =
        static_cast<std::int32_t>(static_cast<std::int16_t>(static_cast<std::uint16_t>(integer)));
    const auto address = (signed_base + immediate) * 16;
    return static_cast<std::size_t>(static_cast<std::uint32_t>(address) & kVu1DataMask);
}

[[gnu::always_inline]] inline __m128i vu1_aot_read_vector(const std::byte* const data,
                                                          const std::size_t offset) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset));
}

[[gnu::always_inline]] inline void vu1_aot_write_vector(std::byte* const data,
                                                        const std::size_t offset,
                                                        const __m128i value) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(data + offset), value);
}

[[gnu::always_inline]] inline std::uint32_t vu1_aot_read_half(const std::byte* const data,
                                                              const std::size_t offset) noexcept {
    std::uint16_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

[[gnu::always_inline]] inline void vu1_aot_write_word(std::byte* const data,
                                                      const std::size_t offset,
                                                      const std::uint32_t value) noexcept {
    std::memcpy(data + offset, &value, sizeof(value));
}

// ---- Upper arithmetic ---- //

// native_float_arithmetic() without its flag masks: the raw binary32 result,
// from which both the stored value (one more clamp) and the MAC word derive.
// The Normalized flags carry that primitive's contract unchanged: each is a
// generation-time proof that every observed lane of the operand already
// passed this same clamp, never a runtime assumption. Unobserved lanes are
// masked out of the stored value and of the MAC word by the caller.
template <vu1_detail::SimdArithmetic Operation, bool LeftNormalized = false,
          bool RightNormalized = false, bool AccumulatorNormalized = false>
[[gnu::always_inline]] inline __m128i vu1_aot_native(
    const __m128i left, const __m128i right,
    const __m128i accumulator = _mm_setzero_si128()) noexcept {
    using vu1_detail::SimdArithmetic;
    const auto load = []<bool Normalized>(const __m128i bits) {
        if constexpr (Normalized) {
            return _mm_castsi128_ps(bits);
        } else {
            return vu1_detail::simd_clamp3(bits);
        }
    };
    const auto a = load.template operator()<LeftNormalized>(left);
    const auto b = load.template operator()<RightNormalized>(right);
    using vu1_detail::SimdNativeFinal;
    using vu1_detail::simd_native_result;
    __m128 result;
    if constexpr (Operation == SimdArithmetic::Add) {
        result = simd_native_result(_mm_add_ps(a, b), a, b, SimdNativeFinal::Add);
    } else if constexpr (Operation == SimdArithmetic::Subtract) {
        result = simd_native_result(_mm_sub_ps(a, b), a, b, SimdNativeFinal::Subtract);
    } else {
        result = _mm_mul_ps(a, b);
        if constexpr (Operation == SimdArithmetic::Multiply) {
            result = simd_native_result(result, a, b, SimdNativeFinal::Multiply);
        } else {
            // Product and sum round separately with a clamp between them.
            const auto product = vu1_detail::simd_clamp3(_mm_castps_si128(result));
            const auto c = load.template operator()<AccumulatorNormalized>(accumulator);
            if constexpr (Operation == SimdArithmetic::MultiplyAdd) {
                result = simd_native_result(_mm_add_ps(c, product), c, product,
                                            SimdNativeFinal::Add);
            } else {
                result = simd_native_result(_mm_sub_ps(c, product), c, product,
                                            SimdNativeFinal::Subtract);
            }
        }
    }
    return _mm_castps_si128(result);
}

[[gnu::always_inline]] inline __m128i vu1_aot_clamp(const __m128i raw) noexcept {
    return _mm_castps_si128(vu1_detail::simd_clamp3(raw));
}

// Opt-in form of the two kernels above for modules generated with
// --flushed-clamps; bit-identical under the launch MXCSR (FTZ|DAZ, masked
// underflow), which the dispatcher verifies before entering such a module.
// DAZ makes the hardware consume a tiny operand as the signed zero the clamp
// would have produced, so operands only need simd_clamp_flushed()'s
// saturation. A sum or product of finite operands is then a normal value, a
// signed zero (FTZ) or an infinity, never a denormal or a NaN, and for those
// clamp3 is a clamp to [-Fmax, Fmax] which keeps the sign of zero.
[[gnu::always_inline]] inline __m128 vu1_aot_saturate(const __m128 raw) noexcept {
    const auto maximum = _mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff));
    const auto minimum = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0xff7fffffU)));
    return _mm_min_ps(_mm_max_ps(raw, minimum), maximum);
}

// simd_native_result() reporting the lanes its fast-path test found at or
// beyond Fmax (movemask order). A zero mask proves the raw result has no
// overflowed lane, so neither the clamp nor the sticky overflow test of
// translated code can change anything and both are skipped.
[[gnu::always_inline]] inline __m128 vu1_aot_native_result(
    const __m128 raw, const __m128 left, const __m128 right,
    const vu1_detail::SimdNativeFinal operation, int* const saturated) noexcept {
    const auto lanes = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(
        _mm_and_si128(_mm_castps_si128(raw), _mm_set1_epi32(0x7fffffff)),
        _mm_set1_epi32(0x7f7ffffe))));
    if (saturated != nullptr) {
        *saturated = lanes;
    }
    if (lanes != 0) [[unlikely]] {
        return vu1_detail::simd_restore_native_overflow(raw, left, right, operation);
    }
    return raw;
}

template <vu1_detail::SimdArithmetic Operation, bool LeftNormalized = false,
          bool RightNormalized = false, bool AccumulatorNormalized = false>
[[gnu::always_inline]] inline __m128i vu1_aot_native_flushed(
    const __m128i left, const __m128i right,
    const __m128i accumulator = _mm_setzero_si128(),
    int* const saturated = nullptr) noexcept {
    using vu1_detail::SimdArithmetic;
    const auto load = []<bool Normalized>(const __m128i bits) {
        if constexpr (Normalized) {
            return _mm_castsi128_ps(bits);
        } else {
            return vu1_detail::simd_clamp_flushed(bits);
        }
    };
    const auto a = load.template operator()<LeftNormalized>(left);
    const auto b = load.template operator()<RightNormalized>(right);
    using vu1_detail::SimdNativeFinal;
    __m128 result;
    if constexpr (Operation == SimdArithmetic::Add) {
        result = vu1_aot_native_result(_mm_add_ps(a, b), a, b, SimdNativeFinal::Add, saturated);
    } else if constexpr (Operation == SimdArithmetic::Subtract) {
        result = vu1_aot_native_result(_mm_sub_ps(a, b), a, b, SimdNativeFinal::Subtract,
                                       saturated);
    } else {
        result = _mm_mul_ps(a, b);
        if constexpr (Operation == SimdArithmetic::Multiply) {
            result = vu1_aot_native_result(result, a, b, SimdNativeFinal::Multiply, saturated);
        } else {
            const auto product = vu1_aot_saturate(result);
            const auto c = load.template operator()<AccumulatorNormalized>(accumulator);
            if constexpr (Operation == SimdArithmetic::MultiplyAdd) {
                result = vu1_aot_native_result(_mm_add_ps(c, product), c, product,
                                               SimdNativeFinal::Add, saturated);
            } else {
                result = vu1_aot_native_result(_mm_sub_ps(c, product), c, product,
                                               SimdNativeFinal::Subtract, saturated);
            }
        }
    }
    return _mm_castps_si128(result);
}

[[gnu::always_inline]] inline __m128i vu1_aot_clamp_flushed(const __m128i raw) noexcept {
    return _mm_castps_si128(vu1_aot_saturate(_mm_castsi128_ps(raw)));
}

// The result of vu1_aot_native_flushed() clamped, given the lane mask it
// reported: with no saturated lane the clamp is the identity.
[[gnu::always_inline]] inline __m128i vu1_aot_clamp_unsaturated(const int saturated,
                                                                const __m128i raw) noexcept {
    if (saturated == 0) [[likely]] {
        return raw;
    }
    return vu1_aot_clamp_flushed(raw);
}

// Outer-product operand order: {fs.y, fs.z, fs.x} * {ft.z, ft.x, ft.y}. Lane
// W is computed on whatever the rotation leaves there and is never observed.
[[gnu::always_inline]] inline __m128i vu1_aot_outer_left(const __m128i fs) noexcept {
    return _mm_shuffle_epi32(fs, _MM_SHUFFLE(3, 0, 2, 1));
}

[[gnu::always_inline]] inline __m128i vu1_aot_outer_right(const __m128i ft) noexcept {
    return _mm_shuffle_epi32(ft, _MM_SHUFFLE(3, 1, 0, 2));
}

// A value together with explicit U/O MAC bits (already in MAC bit positions).
struct Vu1AotFlagged {
    __m128i value;
    std::uint32_t flags;
};

// ---- Chop families ---- //
//
// The interpreter stages these families one lane at a time through
// vu1_add_subtract_bits() and vu1_multiply_bits(). The kernels below are the
// four-lane form of the same arithmetic built from simd_clamp3() and
// simd_chop(): the identical host-double sum, and the exact double product
// whose truncation equals the integer significand multiply. Unselected lanes
// are computed but never observed. vu1_aot_kernel_self_test() holds every
// kernel bit-identical to the scalar helpers, flags included.

struct Vu1AotDoubles {
    __m128d low;
    __m128d high;
};

[[gnu::always_inline]] inline Vu1AotDoubles vu1_aot_widen(const __m128 value) noexcept {
    return {_mm_cvtps_pd(value), _mm_cvtps_pd(_mm_movehl_ps(value, value))};
}

// Lane masks as MAC U/O bits (X is the high bit of each nibble).
[[gnu::always_inline]] inline std::uint32_t vu1_aot_chop_flags(
    const __m128i underflow, const __m128i overflow, const std::uint32_t mask) noexcept {
    const auto lanes = [](const __m128i value) {
        return static_cast<std::uint32_t>(_mm_movemask_ps(
            _mm_castsi128_ps(_mm_shuffle_epi32(value, _MM_SHUFFLE(0, 1, 2, 3)))));
    };
    return ((lanes(underflow) & mask) << 8U) | ((lanes(overflow) & mask) << 12U);
}

// Narrowing of two exact double pairs under the launch MXCSR, which the
// dispatcher requires: rounding toward zero with flush-to-zero. The hardware
// conversion then already is simd_chop()'s value path (truncation, a signed
// zero for a denormal result, signed Fmax for an overflow), without its
// correction passes.
[[gnu::always_inline]] inline __m128i vu1_aot_narrow(const __m128d low,
                                                     const __m128d high) noexcept {
    return _mm_castps_si128(_mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high)));
}

// The MAC U/O judgments of simd_chop() (multiply: of vu1_multiply_bits()) for
// the lanes of one narrowing, exact. Only reached for a result lane which is a
// signed zero (an underflow needs a nonzero exact value) or at least Fmax (an
// overflow needs the exact value beyond it).
template <bool Multiply>
[[gnu::noinline, gnu::cold]] std::uint32_t vu1_aot_narrow_flags_exact(
    const __m128d low, const __m128d high, const __m128i bits, const std::uint32_t mask) noexcept {
    const auto sign = _mm_set1_pd(-0.0);
    const auto zero = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7f800000)),
                                      _mm_setzero_si128());
    const auto nonzero = vu1_detail::simd_pack_double_masks(
        _mm_cmpneq_pd(low, _mm_setzero_pd()), _mm_cmpneq_pd(high, _mm_setzero_pd()));
    const auto limit = _mm_set1_pd(Multiply ? 0x1p128 : 0x1.fffffep127);
    const auto overflow = vu1_detail::simd_pack_double_masks(
        Multiply ? _mm_cmpge_pd(_mm_andnot_pd(sign, low), limit)
                 : _mm_cmpgt_pd(_mm_andnot_pd(sign, low), limit),
        Multiply ? _mm_cmpge_pd(_mm_andnot_pd(sign, high), limit)
                 : _mm_cmpgt_pd(_mm_andnot_pd(sign, high), limit));
    return vu1_aot_chop_flags(_mm_and_si128(zero, nonzero), overflow, mask);
}

template <unsigned Mask, bool Multiply>
[[gnu::always_inline]] inline std::uint32_t vu1_aot_narrow_flags(
    const __m128i bits, const __m128d low, const __m128d high) noexcept {
    if constexpr (Mask == 0U) {
        return 0U;
    } else {
        // Candidate lanes: a zero result or one of at least Fmax. The lane
        // mask is X = bit 3, movemask has X = bit 0.
        constexpr int kLanes = static_cast<int>(((Mask & 8U) >> 3U) | ((Mask & 4U) >> 1U) |
                                                ((Mask & 2U) << 1U) | ((Mask & 1U) << 3U));
        const auto magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
        const auto candidates =
            _mm_or_si128(_mm_cmpeq_epi32(magnitude, _mm_setzero_si128()),
                         _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7ffffe)));
        if ((_mm_movemask_ps(_mm_castsi128_ps(candidates)) & kLanes) == 0) [[likely]] {
            return 0U;
        }
        return vu1_aot_narrow_flags_exact<Multiply>(low, high, bits, Mask);
    }
}

template <unsigned Mask, bool Subtract>
[[gnu::always_inline]] inline Vu1AotFlagged vu1_aot_chop_add(
    const __m128i left, const __m128i right) noexcept {
    const auto a = vu1_aot_widen(vu1_detail::simd_clamp3(left));
    const auto b = vu1_aot_widen(vu1_detail::simd_clamp3(right));
    const auto low = Subtract ? _mm_sub_pd(a.low, b.low) : _mm_add_pd(a.low, b.low);
    const auto high = Subtract ? _mm_sub_pd(a.high, b.high) : _mm_add_pd(a.high, b.high);
    const auto bits = vu1_aot_narrow(low, high);
    return {bits, vu1_aot_narrow_flags<Mask, false>(bits, low, high)};
}

template <unsigned Mask>
[[gnu::always_inline]] inline Vu1AotFlagged vu1_aot_chop_multiply(
    const __m128i left, const __m128i right) noexcept {
    const auto a = vu1_aot_widen(vu1_detail::simd_clamp3(left));
    const auto b = vu1_aot_widen(vu1_detail::simd_clamp3(right));
    const auto low = _mm_mul_pd(a.low, b.low);
    const auto high = _mm_mul_pd(a.high, b.high);
    const auto bits = vu1_aot_narrow(low, high);
    // vu1_multiply_bits() judges overflow on the product's exponent: only a
    // product of at least 2^128 overflows. simd_chop() also flags products
    // between Fmax and 2^128, which merely truncate to Fmax.
    return {bits, vu1_aot_narrow_flags<Mask, true>(bits, low, high)};
}

// accumulator +/- left * right; only the final stage contributes flags.
template <unsigned Mask, bool Subtract>
[[gnu::always_inline]] inline Vu1AotFlagged vu1_aot_chop_multiply_add(
    const __m128i accumulator, const __m128i left, const __m128i right) noexcept {
    const auto a = vu1_aot_widen(vu1_detail::simd_clamp3(left));
    const auto b = vu1_aot_widen(vu1_detail::simd_clamp3(right));
    const auto product = vu1_aot_narrow(_mm_mul_pd(a.low, b.low), _mm_mul_pd(a.high, b.high));
    const auto p = vu1_aot_widen(_mm_castsi128_ps(product));
    const auto c = vu1_aot_widen(vu1_detail::simd_clamp3(accumulator));
    const auto low = Subtract ? _mm_sub_pd(c.low, p.low) : _mm_add_pd(c.low, p.low);
    const auto high = Subtract ? _mm_sub_pd(c.high, p.high) : _mm_add_pd(c.high, p.high);
    const auto bits = vu1_aot_narrow(low, high);
    return {bits, vu1_aot_narrow_flags<Mask, false>(bits, low, high)};
}

// simd_add_signed_zero() under round-toward-zero (an exact zero sum is +0,
// as it is under round-to-nearest), which the dispatcher checks once per
// launch instead of reading MXCSR for every instruction.
[[gnu::always_inline]] inline __m128i vu1_aot_add_signed_zero(
    const __m128i value, __m128i zero_sign, const bool subtract) noexcept {
    const auto sign_mask = _mm_set1_epi32(static_cast<int>(0x80000000U));
    const auto zero_lanes =
        _mm_cmpeq_epi32(_mm_andnot_si128(sign_mask, value), _mm_setzero_si128());
    if (subtract) {
        zero_sign = _mm_xor_si128(zero_sign, sign_mask);
    }
    const auto result_sign = _mm_and_si128(_mm_and_si128(value, sign_mask), zero_sign);
    return vu1_detail::simd_select(zero_lanes, result_sign, value);
}

// simd_arithmetic_vf0_lane(): a broadcast operand taken from VF0. Lane W
// (1.0) is real chop arithmetic; the zero lanes only settle signs.
template <vu1_detail::SimdArithmetic Operation, unsigned Lane, unsigned Mask>
[[gnu::always_inline]] inline Vu1AotFlagged vu1_aot_vf0_lane(
    const __m128i left, const __m128i accumulator) noexcept {
    using vu1_detail::SimdArithmetic;
    if constexpr (Lane == 3U && Operation != SimdArithmetic::Multiply) {
        if constexpr (Operation == SimdArithmetic::MultiplyAdd) {
            return vu1_aot_chop_add<Mask, false>(accumulator, left);
        } else if constexpr (Operation == SimdArithmetic::MultiplySubtract) {
            return vu1_aot_chop_add<Mask, true>(accumulator, left);
        } else {
            return vu1_aot_chop_add<Mask, Operation == SimdArithmetic::Subtract>(
                left, _mm_set1_epi32(0x3f800000));
        }
    } else {
        const auto a = vu1_aot_clamp(left);
        __m128i value;
        if constexpr (Operation == SimdArithmetic::Add) {
            value = vu1_aot_add_signed_zero(a, _mm_setzero_si128(), false);
        } else if constexpr (Operation == SimdArithmetic::Subtract) {
            value = vu1_aot_add_signed_zero(a, _mm_setzero_si128(), true);
        } else {
            value = Lane == 3U
                        ? a
                        : _mm_and_si128(a, _mm_set1_epi32(static_cast<int>(0x80000000U)));
            if constexpr (Operation != SimdArithmetic::Multiply) {
                value = vu1_aot_add_signed_zero(
                    vu1_aot_clamp(accumulator), value,
                    Operation == SimdArithmetic::MultiplySubtract);
            }
        }
        return {value, 0U};
    }
}

// MAX/MINI: signed-integer order on the raw patterns, inverted when both
// operands are negative.
[[gnu::always_inline]] inline __m128i vu1_aot_max(const __m128i left,
                                                  const __m128i right) noexcept {
    const auto greater = _mm_cmpgt_epi32(left, right);
    const auto both_negative =
        _mm_and_si128(_mm_srai_epi32(left, 31), _mm_srai_epi32(right, 31));
    return vu1_detail::simd_select(_mm_xor_si128(greater, both_negative), left, right);
}

[[gnu::always_inline]] inline __m128i vu1_aot_mini(const __m128i left,
                                                   const __m128i right) noexcept {
    const auto greater = _mm_cmpgt_epi32(left, right);
    const auto both_negative =
        _mm_and_si128(_mm_srai_epi32(left, 31), _mm_srai_epi32(right, 31));
    return vu1_detail::simd_select(_mm_xor_si128(greater, both_negative), right, left);
}

// ITOF runs inside NativeFloatArithmeticScope with rounding toward zero.
// For these fractions every nonzero signed int32 result is normal and finite;
// exact power-of-two scaling commutes with chopping its significand to float.
// Convert all four lanes directly, preserving the former double-pair bits.
template <unsigned Fraction>
[[gnu::always_inline]] inline __m128i vu1_aot_itof(const __m128i source) noexcept {
    static_assert(Fraction == 0U || Fraction == 4U || Fraction == 12U || Fraction == 15U);
    const auto converted = _mm_cvtepi32_ps(source);
    if constexpr (Fraction == 0U) {
        return _mm_castps_si128(converted);
    } else {
        return _mm_castps_si128(_mm_mul_ps(
            converted,
            _mm_set1_ps(std::bit_cast<float>(0x3f800000U - (Fraction << 23U)))));
    }
}

// FTOI (vu1_float_to_int_bits): truncate the scaled value; magnitudes of
// 2^31 or more, infinities and NaNs saturate by sign.
template <unsigned Fraction>
[[gnu::always_inline]] inline __m128i vu1_aot_ftoi(const __m128i source) noexcept {
    auto value = _mm_castsi128_ps(source);
    if constexpr (Fraction != 0U) {
        value = _mm_mul_ps(
            value, _mm_set1_ps(std::bit_cast<float>(0x3f800000U + (Fraction << 23U))));
    }
    const auto bits = _mm_castps_si128(value);
    const auto saturates = _mm_cmpgt_epi32(
        _mm_and_si128(bits, _mm_set1_epi32(0x7f800000)), _mm_set1_epi32(0x4effffff));
    const auto saturated =
        _mm_xor_si128(_mm_srai_epi32(bits, 31), _mm_set1_epi32(0x7fffffff));
    return vu1_detail::simd_select(saturates, saturated, _mm_cvttps_epi32(value));
}

[[gnu::always_inline]] inline __m128i vu1_aot_abs(const __m128i source) noexcept {
    return _mm_and_si128(source, _mm_set1_epi32(0x7fffffff));
}

struct Vu1AotLanes {
    alignas(16) std::array<std::uint32_t, 4U> words;

    [[gnu::always_inline]] explicit Vu1AotLanes(const __m128i value) noexcept {
        _mm_store_si128(reinterpret_cast<__m128i*>(words.data()), value);
    }
    [[gnu::always_inline]] [[nodiscard]] __m128i vector() const noexcept {
        return _mm_load_si128(reinterpret_cast<const __m128i*>(words.data()));
    }
};

// stage_vu1_upper_clip_impl().
[[gnu::always_inline]] inline std::uint32_t vu1_aot_clip(
    const std::uint32_t logical_clip, const __m128i source, const __m128i limit) noexcept {
    auto comparison = static_cast<std::int32_t>(vu1_aot_lane<3U>(limit));
    comparison = (comparison & 0x7f800000) != 0 ? comparison & 0x7fffffff : 0x007fffff;
    const auto bound = _mm_set1_epi32(comparison);
    // Per lane: "bits above the bound" (bit 2 * lane) and "sign-flipped bits
    // above the bound" (bit 2 * lane + 1); interleaving the two compare masks
    // yields x+, x-, y+, y-, z+, z-, w+, w- in movemask order.
    const auto above = _mm_cmpgt_epi32(source, bound);
    const auto below = _mm_cmpgt_epi32(
        _mm_xor_si128(source, _mm_set1_epi32(static_cast<int>(0x80000000U))), bound);
    const auto xy = static_cast<std::uint32_t>(
        _mm_movemask_ps(_mm_castsi128_ps(_mm_unpacklo_epi32(above, below))));
    const auto z = static_cast<std::uint32_t>(
        _mm_movemask_ps(_mm_castsi128_ps(_mm_unpackhi_epi32(above, below)))) & 3U;
    return ((logical_clip << 6U) & 0x00ffffffU) | xy | (z << 4U);
}

// ---- FDIV and EFU scalars ---- //

struct Vu1AotQuotient {
    std::uint32_t bits;
    std::uint32_t status_di;
};

[[gnu::always_inline]] inline Vu1AotQuotient vu1_aot_div(
    const std::uint32_t raw_numerator, const std::uint32_t raw_denominator) noexcept {
    const auto numerator_bits = vu1_normalize_fdiv_scalar(raw_numerator);
    const auto denominator_bits = vu1_normalize_fdiv_scalar(raw_denominator);
    if ((denominator_bits & 0x7fffffffU) == 0U) {
        return {((raw_numerator ^ raw_denominator) & 0x80000000U) | 0x7f7fffffU,
                (numerator_bits & 0x7fffffffU) == 0U ? 0x10U : 0x20U};
    }
    const auto quotient = static_cast<double>(to_float(numerator_bits)) /
                          static_cast<double>(to_float(denominator_bits));
    return {vu1_normalize_fdiv_scalar(vu1_chop_scalar_bits(quotient)), 0U};
}

[[gnu::always_inline]] inline Vu1AotQuotient vu1_aot_sqrt(const std::uint32_t raw) noexcept {
    const auto input = static_cast<double>(to_float(vu1_normalize_fdiv_scalar(raw)));
    const auto result_bits = vu1_chop_scalar_bits(std::sqrt(std::fabs(input)));
    return {vu1_normalize_fdiv_scalar(result_bits), (raw & 0x80000000U) != 0U ? 0x10U : 0U};
}

[[gnu::always_inline]] inline Vu1AotQuotient vu1_aot_rsqrt(
    const std::uint32_t raw_numerator, const std::uint32_t raw_radicand) noexcept {
    const auto numerator_bits = vu1_normalize_fdiv_scalar(raw_numerator);
    const auto radicand_bits = vu1_normalize_fdiv_scalar(raw_radicand);
    if ((radicand_bits & 0x7fffffffU) == 0U) {
        return {(raw_numerator & 0x80000000U) | 0x7f7fffffU,
                (numerator_bits & 0x7fffffffU) == 0U ? 0x10U : 0x20U};
    }
    // Two binary32 operations: the root is chopped before the division.
    const auto radicand_value = static_cast<double>(to_float(radicand_bits));
    const auto root_bits = vu1_chop_scalar_bits(std::sqrt(std::fabs(radicand_value)));
    const auto quotient = static_cast<double>(to_float(numerator_bits)) /
                          static_cast<double>(to_float(root_bits));
    return {vu1_normalize_fdiv_scalar(vu1_chop_scalar_bits(quotient)),
            (raw_radicand & 0x80000000U) != 0U ? 0x10U : 0U};
}

[[gnu::always_inline]] inline std::uint32_t vu1_aot_ercpr(const std::uint32_t raw) noexcept {
    const auto input_bits = vu1_flush_denormal(raw);
    const auto input = to_float(input_bits);
    const auto result_bits =
        input == 0.0F ? input_bits : vu1_chop_scalar_bits(1.0 / static_cast<double>(input));
    return vu1_flush_denormal(result_bits);
}

template <bool Reciprocal>
[[gnu::always_inline]] inline std::uint32_t vu1_aot_esqrt(const std::uint32_t raw) noexcept {
    const auto magnitude_bits = vu1_flush_denormal(raw) & 0x7fffffffU;
    auto result_bits =
        vu1_chop_scalar_bits(std::sqrt(static_cast<double>(to_float(magnitude_bits))));
    if constexpr (Reciprocal) {
        if ((result_bits & 0x7fffffffU) != 0U) {
            result_bits =
                vu1_chop_scalar_bits(1.0 / static_cast<double>(to_float(result_bits)));
        }
    }
    return vu1_flush_denormal(result_bits);
}

// ---- Kernel self-test ---- //

// Compares every re-vectorised kernel with the interpreter's own helpers on
// random and edge-pattern lanes under the launch MXCSR (FTZ|DAZ). Returns a
// description of the first difference, or nothing.
[[nodiscard]] std::optional<std::string> vu1_aot_kernel_self_test_impl(
    std::uint64_t seed, const std::size_t vectors) {
    using vu1_detail::SimdArithmetic;
    const vu1_detail::NativeFloatArithmeticScope scope{true};
    static constexpr std::array<std::uint32_t, 24U> kEdges{
        0U,          0x80000000U, 1U,          0x807fffffU, 0x00800000U, 0x80800000U,
        0x3f800000U, 0xbf800000U, 0x3f800001U, 0x3f7fffffU, 0x7f7fffffU, 0xff7fffffU,
        0x7f800000U, 0xff800000U, 0x7fc12345U, 0xff812345U, 0x7f7ffffeU, 0x00ffffffU,
        0x4f000000U, 0xcf000000U, 0x4effffffU, 0x7fffffffU, 0x80000001U, 0x33800000U};
    const auto random = [&seed] {
        seed ^= seed << 13U;
        seed ^= seed >> 7U;
        seed ^= seed << 17U;
        return static_cast<std::uint32_t>(seed >> 16U);
    };
    const auto lane_pattern = [&] {
        const auto value = random();
        switch (value & 7U) {
        case 0U: return kEdges[(value >> 3U) % kEdges.size()];
        case 1U: return (value & 0x807fffffU) | (((value >> 8U) % 24U + 1U) << 23U);    // tiny
        case 2U: return (value & 0x807fffffU) | ((231U + (value >> 8U) % 24U) << 23U);  // huge
        case 3U: return (value & 0x807fffffU) | ((120U + (value >> 8U) % 16U) << 23U);  // near 1
        default: return random();
        }
    };
    const auto hex = [](std::uint64_t value) {
        std::string text;
        do {
            text.insert(text.begin(), "0123456789abcdef"[value & 15U]);
            value >>= 4U;
        } while (value != 0U);
        return text;
    };
    const auto failure = [&hex](const char* kernel, const unsigned lane, const std::uint32_t a,
                                const std::uint32_t b, const std::uint32_t c,
                                const std::uint64_t expected, const std::uint64_t actual) {
        return std::optional<std::string>{
            std::string{kernel} + " lane " + std::to_string(lane) + " a=" + hex(a) + " b=" +
            hex(b) + " c=" + hex(c) + " expected=" + hex(expected) + " actual=" + hex(actual)};
    };
    // STATUS tracking leans on two properties of the kernels. A stored chop
    // result is a signed zero or a normal finite value, so its value-derived
    // MAC bits are only Z and S. A raw native-float result is never denormal
    // under the launch MXCSR, so it never raises U.
    const auto stored_pattern = [](const std::uint32_t bits) {
        const auto exponent = bits & 0x7f800000U;
        return exponent != 0x7f800000U && (exponent != 0U || (bits & 0x007fffffU) == 0U);
    };
    const auto denormal = [](const std::uint32_t bits) {
        return (bits & 0x7f800000U) == 0U && (bits & 0x007fffffU) != 0U;
    };
    const auto mac_bits = [](const unsigned lane, const std::uint32_t stored, const bool underflow,
                             const bool overflow) {
        std::uint32_t flags = 0U;
        if ((stored & 0x7fffffffU) == 0U) flags |= 0x0008U >> lane;
        if ((stored & 0x80000000U) != 0U) flags |= 0x0080U >> lane;
        if (underflow) flags |= 0x0800U >> lane;
        if (overflow) flags |= 0x8000U >> lane;
        return flags;
    };

    for (std::size_t round = 0U; round < vectors; ++round) {
        Gpr128 ga;
        Gpr128 gb;
        Gpr128 gc;
        for (unsigned lane = 0U; lane < 4U; ++lane) {
            ga.words[lane] = lane_pattern();
            gb.words[lane] = lane_pattern();
            gc.words[lane] = lane_pattern();
        }
        if ((round & 7U) == 0U) {
            // Products between Fmax and 2^128: overflow for a double chop,
            // exponent 254 (no overflow) for the significand multiply.
            const auto exponent = 128U + random() % 126U;
            ga.words[0] = (random() & 0x80000000U) | (exponent << 23U) | 0x007ffffeU;
            gb.words[0] = (random() & 0x80000000U) | ((381U - exponent) << 23U) | 0x00000001U;
        }
        const auto a = vu1_aot_load_vector(ga);
        const auto b = vu1_aot_load_vector(gb);
        const auto c = vu1_aot_load_vector(gc);

        // One check per (kernel result, scalar oracle) pair.
        const auto check = [&](const char* kernel, const Vu1AotFlagged& actual,
                               const auto& oracle) -> std::optional<std::string> {
            const Vu1AotLanes value{actual.value};
            const auto lazy = vu1_aot_mac(actual.value, 0xffffU, actual.flags);
            for (unsigned lane = 0U; lane < 4U; ++lane) {
                if (!stored_pattern(value.words[lane])) {
                    return failure("chop kernel stored a denormal or non-finite pattern", lane,
                                   ga.words[lane], gb.words[lane], gc.words[lane], 0U,
                                   value.words[lane]);
                }
                bool underflow = false;
                bool overflow = false;
                const auto expected = oracle(lane, underflow, overflow);
                const auto expected_mac = mac_bits(lane, expected, underflow, overflow);
                const auto lane_mask = 0x8888U >> lane;
                if (value.words[lane] != expected || (lazy & lane_mask) != expected_mac) {
                    return failure(kernel, lane, ga.words[lane], gb.words[lane], gc.words[lane],
                                   (std::uint64_t{expected_mac} << 32U) | expected,
                                   (std::uint64_t{lazy & lane_mask} << 32U) | value.words[lane]);
                }
            }
            return std::nullopt;
        };
        const auto add = [&](const bool subtract) {
            return [&, subtract](const unsigned lane, bool& underflow, bool& overflow) {
                return vu1_add_subtract_bits(ga.words[lane], gb.words[lane], subtract, underflow,
                                             overflow);
            };
        };
        const auto multiply_add = [&](const bool subtract) {
            return [&, subtract](const unsigned lane, bool& underflow, bool& overflow) {
                std::uint32_t product{};
                bool product_underflow = false;
                bool product_overflow = false;
                vu1_multiply_bits(ga.words[lane], gb.words[lane], product, product_underflow,
                                  product_overflow);
                return vu1_add_subtract_bits(gc.words[lane], product, subtract, underflow,
                                             overflow);
            };
        };
        if (auto fault = check("chop add", vu1_aot_chop_add<15U, false>(a, b), add(false))) return fault;
        if (auto fault = check("chop subtract", vu1_aot_chop_add<15U, true>(a, b), add(true))) return fault;
        if (auto fault = check("chop multiply", vu1_aot_chop_multiply<15U>(a, b),
                               [&](const unsigned lane, bool& underflow, bool& overflow) {
                                   std::uint32_t product{};
                                   vu1_multiply_bits(ga.words[lane], gb.words[lane], product,
                                                     underflow, overflow);
                                   return product;
                               })) return fault;
        if (auto fault = check("chop multiply-add",
                               vu1_aot_chop_multiply_add<15U, false>(c, a, b),
                               multiply_add(false))) return fault;
        if (auto fault = check("chop multiply-subtract",
                               vu1_aot_chop_multiply_add<15U, true>(c, a, b),
                               multiply_add(true))) return fault;

        // Native-float kernels against native_float_arithmetic(), including
        // the proof-carrying form on operands which passed the clamp.
        const auto native = [&]<SimdArithmetic Operation>(const char* kernel)
            -> std::optional<std::string> {
            const auto reference = vu1_detail::native_float_arithmetic<Operation>(ga, gb, gc);
            const auto raw = vu1_aot_native<Operation>(a, b, c);
            const auto proven = vu1_aot_native<Operation, true, true, true>(
                vu1_aot_clamp(a), vu1_aot_clamp(b), vu1_aot_clamp(c));
            const auto flushed = vu1_aot_native_flushed<Operation>(a, b, c);
            const auto flushed_proven = vu1_aot_native_flushed<Operation, true, true, true>(
                vu1_aot_clamp(a), vu1_aot_clamp(b), vu1_aot_clamp(c));
            const Vu1AotLanes flushed_value{vu1_aot_clamp_flushed(flushed)};
            const Vu1AotLanes flushed_proven_value{vu1_aot_clamp_flushed(flushed_proven)};
            const Vu1AotLanes value{vu1_aot_clamp(raw)};
            const Vu1AotLanes proven_value{vu1_aot_clamp(proven)};
            const auto expected_mac =
                static_cast<std::uint32_t>(vu1_detail::simd_mac_flags(reference.value, 15U)) |
                (reference.underflow_mask << 8U) | (reference.overflow_mask << 12U);
            const auto lazy = vu1_aot_mac(raw, 0xffffU, 0U);
            const auto proven_lazy = vu1_aot_mac(proven, 0xffffU, 0U);
            const Vu1AotLanes raw_lanes{raw};
            const Vu1AotLanes flushed_raw_lanes{flushed};
            for (unsigned lane = 0U; lane < 4U; ++lane) {
                if (denormal(raw_lanes.words[lane]) || denormal(flushed_raw_lanes.words[lane])) {
                    return failure("native result is denormal under the launch MXCSR", lane,
                                   ga.words[lane], gb.words[lane], gc.words[lane], 0U,
                                   raw_lanes.words[lane]);
                }
                if (value.words[lane] != reference.value.words[lane] ||
                    proven_value.words[lane] != reference.value.words[lane]) {
                    return failure(kernel, lane, ga.words[lane], gb.words[lane], gc.words[lane],
                                   reference.value.words[lane], value.words[lane]);
                }
                if (flushed_value.words[lane] != reference.value.words[lane] ||
                    flushed_proven_value.words[lane] != reference.value.words[lane]) {
                    return failure("flushed kernel", lane, ga.words[lane], gb.words[lane],
                                   gc.words[lane], reference.value.words[lane],
                                   flushed_value.words[lane]);
                }
            }
            if (lazy != expected_mac || proven_lazy != expected_mac) {
                return failure(kernel, 4U, ga.words[0], gb.words[0], gc.words[0], expected_mac,
                               lazy);
            }
            if (vu1_aot_mac(flushed, 0xffffU, 0U) != expected_mac ||
                vu1_aot_mac(flushed_proven, 0xffffU, 0U) != expected_mac) {
                return failure("flushed kernel flags", 4U, ga.words[0], gb.words[0], gc.words[0],
                               expected_mac, vu1_aot_mac(flushed, 0xffffU, 0U));
            }
            return std::nullopt;
        };
        if (auto fault = native.template operator()<SimdArithmetic::Add>("native add")) return fault;
        if (auto fault = native.template operator()<SimdArithmetic::Subtract>("native subtract")) return fault;
        if (auto fault = native.template operator()<SimdArithmetic::Multiply>("native multiply")) return fault;
        if (auto fault = native.template operator()<SimdArithmetic::MultiplyAdd>("native multiply-add")) return fault;
        if (auto fault = native.template operator()<SimdArithmetic::MultiplySubtract>("native multiply-subtract")) return fault;

        // VF0 broadcast operands against simd_arithmetic_vf0_lane().
        const auto vf0 = [&]<SimdArithmetic Operation, unsigned Lane>(const char* kernel)
            -> std::optional<std::string> {
            const auto reference =
                vu1_detail::simd_arithmetic_vf0_lane<Operation>(ga, gc, Lane);
            const auto actual = vu1_aot_vf0_lane<Operation, Lane, 15U>(a, c);
            const Vu1AotLanes value{actual.value};
            const auto expected_mac =
                static_cast<std::uint32_t>(vu1_detail::simd_mac_flags(reference.value, 15U)) |
                (reference.underflow_mask << 8U) | (reference.overflow_mask << 12U);
            for (unsigned lane = 0U; lane < 4U; ++lane) {
                if (value.words[lane] != reference.value.words[lane]) {
                    return failure(kernel, lane, ga.words[lane], 0U, gc.words[lane],
                                   reference.value.words[lane], value.words[lane]);
                }
                if (!stored_pattern(value.words[lane])) {
                    return failure("vf0 kernel stored a denormal or non-finite pattern", lane,
                                   ga.words[lane], 0U, gc.words[lane], 0U, value.words[lane]);
                }
            }
            if (vu1_aot_mac(actual.value, 0xffffU, actual.flags) != expected_mac) {
                return failure(kernel, 4U, ga.words[0], 0U, gc.words[0], expected_mac,
                               vu1_aot_mac(actual.value, 0xffffU, actual.flags));
            }
            return std::nullopt;
        };
#define VU1_AOT_SELF_TEST_VF0(operation)                                                        \
        if (auto fault = vf0.template operator()<SimdArithmetic::operation, 0U>(                \
                "vf0 " #operation " zero lane")) return fault;                                   \
        if (auto fault = vf0.template operator()<SimdArithmetic::operation, 3U>(                \
                "vf0 " #operation " one lane")) return fault
        VU1_AOT_SELF_TEST_VF0(Add);
        VU1_AOT_SELF_TEST_VF0(Subtract);
        VU1_AOT_SELF_TEST_VF0(Multiply);
        VU1_AOT_SELF_TEST_VF0(MultiplyAdd);
        VU1_AOT_SELF_TEST_VF0(MultiplySubtract);
#undef VU1_AOT_SELF_TEST_VF0

        // Conversions and MAX/MINI against the scalar helpers.
        const auto convert = [&]<unsigned Fraction>() -> std::optional<std::string> {
            const Vu1AotLanes floats{vu1_aot_itof<Fraction>(a)};
            const Vu1AotLanes integers{vu1_aot_ftoi<Fraction>(a)};
            const auto scale =
                Fraction == 0U
                    ? 1.0
                    : static_cast<double>(std::bit_cast<float>(0x3f800000U - (Fraction << 23U)));
            for (unsigned lane = 0U; lane < 4U; ++lane) {
                bool underflow = false;
                bool overflow = false;
                const auto expected_float = vu1_chop_exact_bits(
                    static_cast<double>(static_cast<std::int32_t>(ga.words[lane])) * scale,
                    underflow, overflow);
                if (floats.words[lane] != expected_float) {
                    return failure("itof", lane, ga.words[lane], Fraction, 0U, expected_float,
                                   floats.words[lane]);
                }
                const auto expected_integer = vu1_float_to_int_bits(ga.words[lane], Fraction);
                if (integers.words[lane] != expected_integer) {
                    return failure("ftoi", lane, ga.words[lane], Fraction, 0U, expected_integer,
                                   integers.words[lane]);
                }
            }
            return std::nullopt;
        };
        if (auto fault = convert.template operator()<0U>()) return fault;
        if (auto fault = convert.template operator()<4U>()) return fault;
        if (auto fault = convert.template operator()<12U>()) return fault;
        if (auto fault = convert.template operator()<15U>()) return fault;

        // CLIP against the scalar form of stage_vu1_upper_clip_impl().
        {
            auto comparison = static_cast<std::int32_t>(gb.words[3]);
            comparison = (comparison & 0x7f800000) != 0 ? comparison & 0x7fffffff : 0x007fffff;
            const auto history = random();
            auto expected = (history << 6U) & 0x00ffffffU;
            for (std::size_t lane = 0U; lane < 3U; ++lane) {
                const auto bits = ga.words[lane];
                if (std::bit_cast<std::int32_t>(bits) > comparison) {
                    expected |= std::uint32_t{1U} << (lane * 2U);
                }
                if (std::bit_cast<std::int32_t>(bits ^ 0x80000000U) > comparison) {
                    expected |= std::uint32_t{2U} << (lane * 2U);
                }
            }
            const auto actual = vu1_aot_clip(history, a, b);
            if (actual != expected) {
                return failure("clip", 4U, ga.words[0], gb.words[3], history, expected, actual);
            }
        }

        const Vu1AotLanes maximum{vu1_aot_max(a, b)};
        const Vu1AotLanes minimum{vu1_aot_mini(a, b)};
        for (unsigned lane = 0U; lane < 4U; ++lane) {
            const auto left = std::bit_cast<std::int32_t>(ga.words[lane]);
            const auto right = std::bit_cast<std::int32_t>(gb.words[lane]);
            const bool both_negative = left < 0 && right < 0;
            const auto expected_max = static_cast<std::uint32_t>(
                both_negative ? std::min(left, right) : std::max(left, right));
            const auto expected_min = static_cast<std::uint32_t>(
                both_negative ? std::max(left, right) : std::min(left, right));
            if (maximum.words[lane] != expected_max) {
                return failure("max", lane, ga.words[lane], gb.words[lane], 0U, expected_max,
                               maximum.words[lane]);
            }
            if (minimum.words[lane] != expected_min) {
                return failure("mini", lane, ga.words[lane], gb.words[lane], 0U, expected_min,
                               minimum.words[lane]);
            }
        }
    }
    return std::nullopt;
}
