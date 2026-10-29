// Private implementation included after the fixed arithmetic stages. Generated
// code supplies constant operands; no opcode or hazard dispatch occurs in an
// admitted body. The interpreter remains the fallback and differential oracle.
[[maybe_unused]] bool native_vu1_schedule_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_NATIVE_SCHEDULE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

[[maybe_unused]] bool native_vu1_transfer_schedule_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_NATIVE_TRANSFER_SCHEDULE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

template<std::size_t N>
struct NativeVu1Schedule {
    static constexpr std::size_t unused = std::numeric_limits<std::size_t>::max();
    std::array<std::size_t, N> issue{};
    std::array<std::size_t, 128> vf_input_deadline{};
    std::array<std::size_t, 16> vi_input_deadline{};
    std::array<std::size_t, 128> vf_output_ready{};
    std::array<std::size_t, 16> vi_output_ready{};
    std::array<bool, N> mac_live{};
    std::array<bool, N> left_normalized{}, right_normalized{}, accumulator_normalized{};
    std::size_t cycles{};
    std::uint32_t vector_registers{};
    std::size_t q_input_deadline{unused};
    std::size_t q_output_ready{};
    bool resets_flag_origin{};
    bool conditional_exit{};
    bool valid{true};
};

[[maybe_unused]] bool native_vu1_flag_liveness_enabled() {
    static const bool enabled = [] {
        const auto* value=std::getenv("TAKEDOWN_VU1_NATIVE_FLAG_LIVENESS");
        return value && std::string_view{value}=="1";
    }();
    return enabled;
}

constexpr bool native_upper_updates_mac(UpperOp op) {
    switch(op) {
    case UpperOp::Nop: case UpperOp::Clip: case UpperOp::Abs:
    case UpperOp::Vitof: case UpperOp::Vftoi:
    case UpperOp::Max: case UpperOp::MaxLane: case UpperOp::MaxI:
    case UpperOp::Mini: case UpperOp::MiniLane: case UpperOp::MiniI:
        return false;
    default: return true;
    }
}

constexpr bool native_upper_writes_fd(UpperOp op) {
    switch (op) {
    case UpperOp::Nop: case UpperOp::Clip:
    case UpperOp::MaddaLane: case UpperOp::MulaLane: case UpperOp::AddaLane:
    case UpperOp::SubaLane: case UpperOp::MsubaLane:
    case UpperOp::Mula: case UpperOp::OpMula: case UpperOp::Adda:
    case UpperOp::Madda: case UpperOp::Suba: case UpperOp::Msuba:
    case UpperOp::MulaQ: case UpperOp::MulaI: case UpperOp::AddaQ: case UpperOp::AddaI:
    case UpperOp::MaddaQ: case UpperOp::MaddaI: case UpperOp::SubaQ: case UpperOp::SubaI:
    case UpperOp::MsubaQ: case UpperOp::MsubaI:
        return false;
    default: return true;
    }
}

constexpr bool native_fixed_arithmetic_supported(UpperOp op) {
    return op==UpperOp::AddLane || op==UpperOp::Add || op==UpperOp::Adda || op==UpperOp::AddaLane ||
        op==UpperOp::SubLane || op==UpperOp::Sub || op==UpperOp::Mul || op==UpperOp::MulaLane ||
        op==UpperOp::MaddLane || op==UpperOp::Madd || op==UpperOp::MaddaLane || op==UpperOp::Madda ||
        op==UpperOp::MsubLane || op==UpperOp::Msub || op==UpperOp::OpMula || op==UpperOp::Opmsub ||
        op==UpperOp::MulQ;
}

constexpr bool native_upper_writes_acc(UpperOp op) {
    switch (op) {
    case UpperOp::MaddaLane: case UpperOp::MulaLane: case UpperOp::AddaLane:
    case UpperOp::SubaLane: case UpperOp::MsubaLane:
    case UpperOp::Mula: case UpperOp::OpMula: case UpperOp::Adda:
    case UpperOp::Madda: case UpperOp::Suba: case UpperOp::Msuba:
    case UpperOp::MulaQ: case UpperOp::MulaI: case UpperOp::AddaQ: case UpperOp::AddaI:
    case UpperOp::MaddaQ: case UpperOp::MaddaI: case UpperOp::SubaQ: case UpperOp::SubaI:
    case UpperOp::MsubaQ: case UpperOp::MsubaI: return true;
    default: return false;
    }
}

constexpr bool native_signed_branch(const CachedVu1Pair& pair) {
    if (pair.i_bit || pair.flow != CachedVu1Flow::ConditionalBranch || !pair.lower) return false;
    const auto op = pair.lower->op;
    return op == LowerOp::Iblt || op == LowerOp::Ibgt || op == LowerOp::Ible || op == LowerOp::Ibge;
}

template<const CachedVu1Pair& Pair>
[[gnu::always_inline]] inline void stage_native_signed_branch(
    Vu1ExecutionMachine& machine, const Vu1State& registers, const std::size_t stall,
    Vu1PairEffects& effects) {
    static_assert(native_signed_branch(Pair));
    // Use the same delayed VI view as the interpreter. The delay-slot write
    // cannot change this latched predicate, and a stall cancels adjacency backup.
    const auto value = std::bit_cast<std::int16_t>(
        machine.conditional_branch_vi_source(Pair.lower->is, stall, &registers));
    if constexpr (Pair.lower->op == LowerOp::Iblt) effects.branch_taken = value < 0;
    else if constexpr (Pair.lower->op == LowerOp::Ibgt) effects.branch_taken = value > 0;
    else if constexpr (Pair.lower->op == LowerOp::Ible) effects.branch_taken = value <= 0;
    else effects.branch_taken = value >= 0;
    if (effects.branch_taken) effects.branch_target = Pair.direct_branch_target;
}

template<std::size_t N>
constexpr NativeVu1Schedule<N> make_native_vu1_schedule(
    const std::array<CachedVu1Pair, N>& pairs) {
    NativeVu1Schedule<N> result;
    result.vf_input_deadline.fill(result.unused);
    result.vi_input_deadline.fill(result.unused);
    // A clamped arithmetic result can feed later native arithmetic directly.
    // Raw loads and other unproved writers invalidate the affected lanes.
    std::array<unsigned,32> normalized{};
    normalized[0]=15U; // Architectural VF0 is always (0,0,0,1).
    unsigned normalized_acc{};
    for (std::size_t index = 0; index < N; ++index) {
        const auto& pair = pairs[index];
        const auto lower = opcode_threaded_lower_plan_for_pair(
            pair, {true, true, true, true, true, true});
        const bool branch = pair.flow == CachedVu1Flow::ConditionalBranch && index+2U==N &&
            (lower.kind==Vu1LowerPlanKind::Ibeq || lower.kind==Vu1LowerPlanKind::Ibne || native_signed_branch(pair));
        const bool div = !pair.i_bit && pair.lower && pair.lower->op == LowerOp::Div;
        if (!pair.initialized || !pair.upper || !pair.lower_decoded ||
            (pair.flow != CachedVu1Flow::Fallthrough && !branch) || pair.e_bit ||
            pair.exits_after_delay || (pair.lower_uses_fdiv_pipe && !div) || pair.lower_uses_efu_pipe ||
            (lower.kind == Vu1LowerPlanKind::Reference && !div && !branch) ||
            (!branch && (lower.kind == Vu1LowerPlanKind::Ibeq || lower.kind == Vu1LowerPlanKind::Ibne))) {
            result.valid = false;
            return result;
        }
        result.conditional_exit = result.conditional_exit || branch;
        result.vector_registers |= (1U << pair.upper->fs) | (1U << pair.upper->ft) | (1U << pair.upper->fd);
        if (pair.lower) result.vector_registers |= (1U << pair.lower->fs) | (1U << pair.lower->ft);
        auto issue = result.cycles;
        for (std::size_t lane = 0; lane < pair.vf_read_lane_count; ++lane)
            issue = std::max(issue, result.vf_output_ready[pair.vf_read_lanes[lane]]);
        for (std::size_t reg = 1; reg < 16; ++reg)
            if (pair.vi_read_mask & (1U << reg))
                issue = std::max(issue, result.vi_output_ready[reg]);
        if (div) {
            issue = std::max(issue, result.q_output_ready);
            if (!result.q_output_ready) result.q_input_deadline = issue;
            result.q_output_ready = issue + kDivSqrtLatency;
        }
        result.resets_flag_origin |= issue - result.cycles >= Vu1FlagPipelines::kStageCount;
        result.issue[index] = issue;
        for (std::size_t lane = 0; lane < pair.vf_read_lane_count; ++lane) {
            const auto id = pair.vf_read_lanes[lane];
            if (result.vf_output_ready[id] == 0)
                result.vf_input_deadline[id] = std::min(result.vf_input_deadline[id], issue);
        }
        for (std::size_t reg = 1; reg < 16; ++reg)
            if ((pair.vi_read_mask & (1U << reg)) && !result.vi_output_ready[reg])
                result.vi_input_deadline[reg] = std::min(result.vi_input_deadline[reg], issue);
        for (std::size_t lane = 0; lane < pair.upper_vf_write_lane_count; ++lane)
            result.vf_output_ready[pair.upper_vf_write_lanes[lane]] = issue + 4;
        const bool suppressed = pair.lower && native_upper_writes_fd(pair.upper->op) &&
                                pair.upper->fd == pair.lower->ft;
        const auto op=pair.upper->op;
        const bool cross=op==UpperOp::OpMula || op==UpperOp::Opmsub;
        const unsigned mask=cross ? 14U : pair.destination_mask;
        const bool broadcast=op==UpperOp::AddLane || op==UpperOp::AddaLane || op==UpperOp::SubLane ||
            op==UpperOp::MulaLane || op==UpperOp::MaddLane || op==UpperOp::MaddaLane || op==UpperOp::MsubLane;
        const unsigned right_mask=broadcast ? (8U>>pair.upper->lane) : mask;
        result.left_normalized[index]=(normalized[pair.upper->fs]&mask)==mask;
        result.right_normalized[index]=(normalized[pair.upper->ft]&right_mask)==right_mask;
        result.accumulator_normalized[index]=(normalized_acc&mask)==mask;
        const bool clamped=native_fixed_arithmetic_supported(op);
        if(native_upper_writes_acc(op))
            normalized_acc=(normalized_acc&~mask) | (clamped ? mask : 0U);
        if(native_upper_writes_fd(op) && pair.upper->fd)
            normalized[pair.upper->fd]=(normalized[pair.upper->fd]&~mask) | (clamped ? mask : 0U);
        if(!suppressed)
            for(std::size_t lane=0;lane<pair.lower_vf_write_lane_count;++lane) {
                const auto id=pair.lower_vf_write_lanes[lane];
                if(id/4U) normalized[id/4U]&=~(8U>>(id%4U));
            }
        if (!suppressed)
            for (std::size_t lane = 0; lane < pair.lower_vf_write_lane_count; ++lane)
                result.vf_output_ready[pair.lower_vf_write_lanes[lane]] = issue + 4;
        if (pair.lower && pair.lower->op == LowerOp::Ilw && pair.lower->it)
            result.vi_output_ready[pair.lower->it] = issue + 4;
        result.cycles = issue + 1;
    }
    // Track the producing instruction of each delayed MAC slot. Retain every
    // value read inside the block and every value visible across its exit.
    // Dropping the other writes is valid only when FMAC STATUS accumulation
    // is separately omitted; exact mode continues to compute every MAC.
    std::array<std::size_t,4> history{};
    history.fill(result.unused);
    std::size_t logical=result.unused, oldest=0, previous=0;
    const auto advance=[&] { history[oldest]=logical; oldest=(oldest+1U)&3U; };
    const auto retain=[&](std::size_t source) {
        if(source!=result.unused) result.mac_live[source]=true;
    };
    for(std::size_t index=0;index<N;++index) {
        for(auto cycle=previous;cycle<result.issue[index];++cycle) advance();
        const auto& pair=pairs[index];
        if(!pair.i_bit && pair.lower && pair.lower->op==LowerOp::Fmand)
            retain(history[oldest]);
        if(native_upper_updates_mac(pair.upper->op)) logical=index;
        advance();
        previous=result.issue[index]+1U;
    }
    retain(logical);
    for(auto source:history) retain(source);
    return result;
}

template<const auto& Schedule, std::size_t Lane>
[[gnu::always_inline]] inline bool native_vf_ready(const Vu1ExecutionMachine& machine) {
    if constexpr (Schedule.vf_input_deadline[Lane] == Schedule.unused) return true;
    else return machine.vf_ready_cycles[Lane] <= machine.elapsed_cycles + Schedule.vf_input_deadline[Lane];
}
template<const auto& Schedule, std::size_t Reg>
[[gnu::always_inline]] inline bool native_vi_ready(const Vu1ExecutionMachine& machine) {
    if constexpr (Schedule.vi_input_deadline[Reg] == Schedule.unused) return true;
    else return machine.vi_load_ready_cycles[Reg] <= machine.elapsed_cycles + Schedule.vi_input_deadline[Reg];
}
template<const auto& Schedule, bool AllowTransfers = false>
[[gnu::always_inline]] inline bool admit_native_vu1_schedule_inline(const Vu1ExecutionMachine& machine,
                              const std::array<std::uint64_t, kVu1PairCount>* profile) {
    static_assert(Schedule.valid);
    if (profile || machine.pending_branch_target ||
        machine.e_delay_pending || (!AllowTransfers && machine.xgkick_service_needed) ||
        machine.committed > machine.options.pair_limit ||
        machine.options.pair_limit - machine.committed < Schedule.issue.size()) return false;
    const auto vf_ready = [&]<std::size_t... Lanes>(std::index_sequence<Lanes...>) {
        return (native_vf_ready<Schedule, Lanes>(machine) && ...);
    }(std::make_index_sequence<128>{});
    const auto vi_ready = [&]<std::size_t... Registers>(std::index_sequence<Registers...>) {
        return (native_vi_ready<Schedule, Registers>(machine) && ...);
    }(std::make_index_sequence<16>{});
    const bool q_ready = Schedule.q_input_deadline == Schedule.unused || !machine.pending_q ||
        machine.pending_q->ready_cycle <= machine.elapsed_cycles + Schedule.q_input_deadline;
    return vf_ready && vi_ready && q_ready;
}

template<const auto& Schedule, bool AllowTransfers = false>
bool admit_native_vu1_schedule(const Vu1ExecutionMachine& machine,
                              const std::array<std::uint64_t, kVu1PairCount>* profile) {
    return admit_native_vu1_schedule_inline<Schedule, AllowTransfers>(machine, profile);
}

enum class NativeVu1Arithmetic { Shared, Sse, NativeFloat, NativeFloatFlushed };

// Diagnostic control for same-executable A/B measurements. This is consulted
// only by explicitly generated NativeFloatFlushed modules; launcher modules
// retain their existing arithmetic and incur no new check.
[[maybe_unused]] bool native_vu1_flushed_arithmetic_enabled() {
    static const bool enabled = [] {
        const auto* value=std::getenv("TAKEDOWN_VU1_FLUSHED_ARITHMETIC");
        return value == nullptr || std::string_view{value} == "1";
    }();
    return enabled;
}

// An admitted iteration can immediately repeat only when its own last writes
// meet every next-iteration input deadline. Entry admission separately proves
// readiness inherited from instructions outside the loop.
template<const auto& Schedule>
constexpr bool native_vu1_loop_reentry_safe() {
    if (!Schedule.valid || !Schedule.conditional_exit) return false;
    if (Schedule.q_output_ready && Schedule.q_output_ready > Schedule.cycles + Schedule.q_input_deadline)
        return false;
    for (std::size_t lane=0;lane<128;++lane)
        if (Schedule.vf_input_deadline[lane]!=Schedule.unused &&
            Schedule.vf_output_ready[lane]>Schedule.cycles+Schedule.vf_input_deadline[lane])
            return false;
    for (std::size_t reg=1;reg<16;++reg)
        if (Schedule.vi_input_deadline[reg]!=Schedule.unused &&
            Schedule.vi_output_ready[reg]>Schedule.cycles+Schedule.vi_input_deadline[reg])
            return false;
    return true;
}

template<bool ClearPendingBranch = true>
[[gnu::always_inline]] inline void normalize_native_vu1_loop_frame(
    Vu1ExecutionMachine& frame, std::size_t& flag_rotation, std::size_t& vi_rotation) {
    const auto flags=frame.flag_pipelines;
    const auto history=frame.vi_branch_history;
    flag_rotation=(flag_rotation+flags.oldest)&3U;
    vi_rotation=(vi_rotation+frame.vi_branch_history_next)&3U;
    for (std::size_t slot=0;slot<4;++slot) {
        const auto index=(flags.oldest+slot)&3U;
        frame.flag_pipelines.mac[slot]=flags.mac[index];
        frame.flag_pipelines.clip[slot]=flags.clip[index];
        frame.flag_pipelines.status[slot]=flags.status[index];
        frame.vi_branch_history[slot]=history[(frame.vi_branch_history_next+slot)&3U];
    }
    frame.flag_pipelines.oldest=0;
    frame.vi_branch_history_next=0;
    // The delay slot has retired. A branch in the next iteration must not
    // inherit the previous iteration's pending-branch suppression rule.
    if constexpr (ClearPendingBranch) frame.pending_branch_target.reset();
}

[[gnu::always_inline]] inline void initialize_native_vu1_machine_frame(
    Vu1ExecutionMachine& frame, const Vu1ExecutionMachine& source) {
    frame.logical_mac = source.logical_mac;
    frame.logical_clip = source.logical_clip;
    frame.logical_status = source.logical_status;
    // Normalize ring origins once. Fixed instruction schedules can then expose
    // each history slot as a compile-time index throughout the native body.
    for (std::size_t slot=0;slot<4;++slot) {
        const auto flag_slot=(source.flag_pipelines.oldest+slot)&3U;
        frame.flag_pipelines.mac[slot]=source.flag_pipelines.mac[flag_slot];
        frame.flag_pipelines.clip[slot]=source.flag_pipelines.clip[flag_slot];
        frame.flag_pipelines.status[slot]=source.flag_pipelines.status[flag_slot];
        frame.vi_branch_history[slot]=source.vi_branch_history[(source.vi_branch_history_next+slot)&3U];
    }
    frame.flag_pipelines.oldest=0;
    frame.vi_branch_history_next=0;
    frame.vi_branch_history_size=source.vi_branch_history_size;
    frame.pending_q=source.pending_q;
    frame.pending_p=source.pending_p;
    frame.xgkick_service_needed=source.xgkick_service_needed;
}

template<bool AbsoluteFlagOrigin = false>
[[gnu::always_inline]] inline void publish_native_vu1_machine_frame(
    Vu1ExecutionMachine& target, const Vu1ExecutionMachine& frame) {
    target.logical_mac=frame.logical_mac;
    target.logical_clip=frame.logical_clip;
    target.logical_status=frame.logical_status;
    const auto flag_origin=AbsoluteFlagOrigin ? 0U : target.flag_pipelines.oldest;
    const auto vi_origin=target.vi_branch_history_next;
    for (std::size_t slot=0;slot<4;++slot) {
        const auto flag_slot=(flag_origin+slot)&3U;
        target.flag_pipelines.mac[flag_slot]=frame.flag_pipelines.mac[slot];
        target.flag_pipelines.clip[flag_slot]=frame.flag_pipelines.clip[slot];
        target.flag_pipelines.status[flag_slot]=frame.flag_pipelines.status[slot];
        target.vi_branch_history[(vi_origin+slot)&3U]=frame.vi_branch_history[slot];
    }
    target.flag_pipelines.oldest=(flag_origin+frame.flag_pipelines.oldest)&3U;
    target.vi_branch_history_next=(vi_origin+frame.vi_branch_history_next)&3U;
    target.vi_branch_history_size=frame.vi_branch_history_size;
    target.xgkick_service_needed=frame.xgkick_service_needed;
    target.pending_branch_target=frame.pending_branch_target;
}

// A generated graph owns one register/machine frame across several admitted
// blocks. Unlike a normalized single-block frame, this preserves absolute
// pipeline origins and scoreboards so every subsequent admission sees the
// same machine as a dispatcher return would. References stay bound to the
// destination's private registers and the same external transfer service.
[[gnu::always_inline]] inline void copy_native_vu1_graph_machine(
    Vu1ExecutionMachine& target, const Vu1ExecutionMachine& source) {
#define COPY_GRAPH_FIELD(field) target.field = source.field
    COPY_GRAPH_FIELD(options);
    COPY_GRAPH_FIELD(pc);
    COPY_GRAPH_FIELD(committed);
    COPY_GRAPH_FIELD(elapsed_cycles);
    COPY_GRAPH_FIELD(pipeline_stall_cycles);
    COPY_GRAPH_FIELD(vf_ready_cycles);
    COPY_GRAPH_FIELD(vi_load_ready_cycles);
    COPY_GRAPH_FIELD(vi_branch_history);
    COPY_GRAPH_FIELD(vi_branch_history_size);
    COPY_GRAPH_FIELD(vi_branch_history_next);
    COPY_GRAPH_FIELD(pending_q);
    COPY_GRAPH_FIELD(pending_p);
    COPY_GRAPH_FIELD(flag_pipelines);
    COPY_GRAPH_FIELD(logical_mac);
    COPY_GRAPH_FIELD(logical_clip);
    COPY_GRAPH_FIELD(logical_status);
    COPY_GRAPH_FIELD(pending_branch_target);
    COPY_GRAPH_FIELD(e_delay_pending);
    COPY_GRAPH_FIELD(last_lower);
    COPY_GRAPH_FIELD(last_upper);
    COPY_GRAPH_FIELD(data_memory);
    COPY_GRAPH_FIELD(xgkick_service_needed);
    COPY_GRAPH_FIELD(native_scheduled_blocks);
    COPY_GRAPH_FIELD(native_scheduled_pairs);
    COPY_GRAPH_FIELD(native_transfer_blocks);
    COPY_GRAPH_FIELD(native_flag_liveness_blocks);
    COPY_GRAPH_FIELD(native_control_pairs);
    COPY_GRAPH_FIELD(native_control_fast_pairs);
#undef COPY_GRAPH_FIELD
}

// Restore physical history slots after graph edges normalized their origins.
// The rotations accumulate across blocks, except a long flag stall resets
// the flag origin absolutely (the caller clears flag_rotation in that case).
[[gnu::always_inline]] inline void publish_native_vu1_graph_machine(
    Vu1ExecutionMachine& target, const Vu1ExecutionMachine& frame,
    std::size_t flag_rotation, std::size_t vi_rotation) {
    copy_native_vu1_graph_machine(target, frame);
    for (std::size_t slot=0;slot<4;++slot) {
        const auto flag_slot=(flag_rotation+slot)&3U;
        target.flag_pipelines.mac[flag_slot]=frame.flag_pipelines.mac[slot];
        target.flag_pipelines.clip[flag_slot]=frame.flag_pipelines.clip[slot];
        target.flag_pipelines.status[flag_slot]=frame.flag_pipelines.status[slot];
        target.vi_branch_history[(vi_rotation+slot)&3U]=frame.vi_branch_history[slot];
    }
    target.flag_pipelines.oldest=(flag_rotation+frame.flag_pipelines.oldest)&3U;
    target.vi_branch_history_next=(vi_rotation+frame.vi_branch_history_next)&3U;
}

// The transfer service sees packet memory, not this private register frame.
// Fixed register indices let the compiler eliminate unused slots and keep
// intermediate vectors live across service calls. Publish at the block exit.
template<const auto& Schedule>
[[gnu::always_inline]] inline Vu1State native_vu1_register_frame(const Vu1State& source) {
    Vu1State frame{};
    [&]<std::size_t... Registers>(std::index_sequence<Registers...>) {
        ([&] { if constexpr (Schedule.vector_registers & (1U << Registers))
            frame.vectors[Registers] = source.vectors[Registers]; }(), ...);
    }(std::make_index_sequence<32>{});
    frame.control = source.control;
    frame.accumulator = source.accumulator;
    return frame;
}

template<const auto& Schedule>
[[gnu::always_inline]] inline void publish_native_vu1_register_frame(Vu1State& target, const Vu1State& frame) {
    [&]<std::size_t... Registers>(std::index_sequence<Registers...>) {
        ([&] { if constexpr (Registers && (Schedule.vector_registers & (1U << Registers)))
            target.vectors[Registers] = frame.vectors[Registers]; }(), ...);
    }(std::make_index_sequence<32>{});
    for (std::size_t reg=1;reg<16;++reg) target.control[reg] = frame.control[reg];
    target.control[kControlI] = frame.control[kControlI];
    target.accumulator = frame.accumulator;
}

template<NativeVu1Arithmetic Mode>
bool admit_native_vu1_arithmetic() {
    if constexpr (Mode == NativeVu1Arithmetic::Shared) return true;
    if (!vu1_detail::simd_arithmetic_enabled()) return false;
    if constexpr (Mode == NativeVu1Arithmetic::NativeFloat || Mode == NativeVu1Arithmetic::NativeFloatFlushed)
        return vu1_detail::native_float_arithmetic_enabled();
    return !vu1_detail::native_float_arithmetic_enabled() && !vu1_detail::avx512_arithmetic_enabled();
}

// Choose an arithmetic implementation once for the whole generated block.
// Fixed operands and inlined vector operations let the compiler keep values
// across guest operations instead of repeatedly dispatching arithmetic modes.
template<const CachedVu1Pair& Pair, NativeVu1Arithmetic Mode,
         bool LeftNormalized = false, bool RightNormalized = false, bool AccumulatorNormalized = false>
[[gnu::always_inline]] inline bool stage_native_vu1_arithmetic(
    const Vu1State& state, std::uint32_t q_bits, Vu1UpperStageEffects& effects) {
#if defined(__SSE2__)
    constexpr auto op = Pair.upper->op;
    constexpr auto operation = [] {
        using A = vu1_detail::SimdArithmetic;
        switch (op) {
        case UpperOp::AddLane: case UpperOp::Add: case UpperOp::Adda: case UpperOp::AddaLane: return A::Add;
        case UpperOp::SubLane: case UpperOp::Sub: return A::Subtract;
        case UpperOp::Mul: case UpperOp::MulaLane: case UpperOp::OpMula: case UpperOp::MulQ: return A::Multiply;
        case UpperOp::MaddLane: case UpperOp::Madd: case UpperOp::MaddaLane: case UpperOp::Madda: return A::MultiplyAdd;
        default: return A::MultiplySubtract;
        }
    }();
    constexpr bool supported = native_fixed_arithmetic_supported(op);
    if constexpr (Mode==NativeVu1Arithmetic::Shared || !supported) return false;
    else {
        constexpr bool broadcast = op==UpperOp::AddLane || op==UpperOp::AddaLane || op==UpperOp::SubLane ||
            op==UpperOp::MulaLane || op==UpperOp::MaddLane || op==UpperOp::MaddaLane || op==UpperOp::MsubLane;
        constexpr bool outer_product = op==UpperOp::OpMula || op==UpperOp::Opmsub;
        constexpr auto mask = outer_product ? 14U : Pair.destination_mask;
        constexpr bool accumulator = op==UpperOp::Adda || op==UpperOp::AddaLane || op==UpperOp::MulaLane ||
            op==UpperOp::Madda || op==UpperOp::MaddaLane || op==UpperOp::OpMula;
        const auto& original_fs = state.read_vector_ref(Pair.upper->fs);
        auto fs = original_fs;
        auto ft = state.read_vector_ref(Pair.upper->ft);
        const auto& acc = state.read_accumulator_ref();
        // Use the pre-pair Q selected by the scheduler, including a pending
        // result which becomes readable before architectural publication.
        if constexpr (op==UpperOp::MulQ) ft.words.fill(q_bits);
        if constexpr (broadcast) ft.words.fill(ft.words[Pair.upper->lane]);
        if constexpr (outer_product) {
            fs.words = {fs.words[1], fs.words[2], fs.words[0], 0U};
            ft.words = {ft.words[2], ft.words[0], ft.words[1], 0U};
        }
        const auto result = [&] {
            // MULq previously used four scalar reference multiplications.
            // Vectorize their exact double/chop semantics even in native-float
            // mode; Q is not covered by the vector normalization proof.
            if constexpr (op==UpperOp::MulQ)
                return vu1_detail::simd_arithmetic<operation>(fs,ft,acc);
            else if constexpr (broadcast && Pair.upper->ft==0)
                return vu1_detail::simd_arithmetic_vf0_lane<operation>(fs,acc,Pair.upper->lane);
            else if constexpr (Mode==NativeVu1Arithmetic::NativeFloatFlushed) {
                if (native_vu1_flushed_arithmetic_enabled())
                    return vu1_detail::native_float_flushed_arithmetic<operation,LeftNormalized,RightNormalized,AccumulatorNormalized>(fs,ft,acc);
                return vu1_detail::native_float_arithmetic<operation,LeftNormalized,RightNormalized,AccumulatorNormalized>(fs,ft,acc);
            } else if constexpr (Mode==NativeVu1Arithmetic::NativeFloat)
                return vu1_detail::native_float_arithmetic<operation,LeftNormalized,RightNormalized,AccumulatorNormalized>(fs,ft,acc);
            else return vu1_detail::simd_arithmetic<operation>(fs,ft,acc);
        }();
        effects.upper_fd = accumulator ? acc : original_fs;
        effects.upper_mask = mask;
        for (std::size_t lane=0;lane<4;++lane)
            if (mask & (8U>>lane)) effects.upper_fd.words[lane]=result.value.words[lane];
        effects.upper_underflow_mask = result.underflow_mask & mask;
        effects.upper_overflow_mask = result.overflow_mask & mask;
        effects.upper_writes_fd = !accumulator;
        effects.upper_writes_acc = accumulator;
        effects.upper_updates_mac = true;
        return true;
    }
#else
    (void)state;
    (void)q_bits;
    (void)effects;
    return false;
#endif
}

// Shared by a fixed native schedule and its dynamically checked fallback.
template<const CachedVu1Pair& Pair>
[[gnu::always_inline]] inline void stage_native_vu1_div(
    const Vu1State& registers, std::size_t elapsed_cycles, Vu1PairEffects& effects) {
        const auto raw_numerator = registers.read_vector_ref(Pair.lower->fs).words[Pair.lower->is];
        const auto raw_denominator = registers.read_vector_ref(Pair.lower->ft).words[Pair.lower->it];
        const auto numerator = vu1_normalize_fdiv_scalar(raw_numerator);
        const auto denominator = vu1_normalize_fdiv_scalar(raw_denominator);
        std::uint32_t bits{}, status{};
        if ((denominator & 0x7fffffffU) == 0) {
            bits = ((raw_numerator ^ raw_denominator) & 0x80000000U) | 0x7f7fffffU;
            status = (numerator & 0x7fffffffU) == 0 ? 0x10U : 0x20U;
        } else {
            bits = vu1_normalize_fdiv_scalar(vu1_chop_scalar_bits(
                static_cast<double>(to_float(numerator)) / static_cast<double>(to_float(denominator))));
        }
        effects.new_pending_q = PendingQ{.bits=bits,.status_di=status,
            .ready_cycle=elapsed_cycles+kDivSqrtLatency};
}

template<const auto& Schedule>
[[gnu::always_inline]] inline void publish_native_vu1_scalar_frame(
    Vu1ExecutionMachine& target, const Vu1ExecutionMachine& frame) {
    if constexpr (Schedule.q_output_ready != 0) {
        target.pending_q = frame.pending_q;
        target.state.write_control(kControlQ, frame.state.read_control(kControlQ));
    }
}

template<const CachedVu1Pair& Pair, auto UpperStage, std::size_t Stall, bool ServiceTransfers = false,
         NativeVu1Arithmetic Arithmetic = NativeVu1Arithmetic::Shared,
         bool TrackMac = true, bool TrackStatus = true,
         bool LeftNormalized = false, bool RightNormalized = false, bool AccumulatorNormalized = false>
[[gnu::always_inline]] inline void issue_native_scheduled_pair(
    Vu1ExecutionMachine& machine, Vu1State* register_frame = nullptr) {
    auto& registers = register_frame ? *register_frame : machine.state;
    constexpr auto lower = opcode_threaded_lower_plan_for_pair(
        Pair, {true, true, true, true, true, true});
    constexpr bool div = !Pair.i_bit && Pair.lower && Pair.lower->op == LowerOp::Div;
    static_assert(lower.kind != Vu1LowerPlanKind::Reference || div || native_signed_branch(Pair));
    if constexpr (Stall != 0) {
        if constexpr (ServiceTransfers)
            if (machine.services.xgkick_mode() == Vu1XgkickMode::TimedLive)
                machine.advance_xgkick_if_needed(static_cast<std::uint32_t>(Stall));
        machine.advance_flag_pipelines_during_stall(Stall);
    }
    // Admission/scheduling proves an older
    // result is ready before a new division can overwrite the single Q pipe.
    if constexpr (div) {
        if (machine.pending_q && machine.pending_q->ready_cycle <= machine.elapsed_cycles)
            machine.publish_pending_q_inline();
    }
    Vu1PairEffects effects{};
    if constexpr (UpperStage != nullptr) {
        auto q = registers.read_control(kControlQ);
        if constexpr (Pair.upper->op == UpperOp::MulQ || Pair.upper->op == UpperOp::MaddQ)
            if (machine.pending_q && machine.pending_q->ready_cycle <= machine.elapsed_cycles)
                q = machine.pending_q->bits;
        if (!stage_native_vu1_arithmetic<Pair, Arithmetic,LeftNormalized,RightNormalized,AccumulatorNormalized>(registers, q, effects))
            UpperStage(registers, q, machine.logical_clip, Pair, effects);
    }
    if constexpr (div) {
        stage_native_vu1_div<Pair>(registers, machine.elapsed_cycles, effects);
    } else if constexpr (native_signed_branch(Pair)) {
        stage_native_signed_branch<Pair>(machine, registers, Stall, effects);
    } else if constexpr (lower.kind == Vu1LowerPlanKind::Ibeq || lower.kind == Vu1LowerPlanKind::Ibne)
        (void)stage_opcode_threaded_branch(lower, Pair,
            machine.conditional_branch_vi_source(lower.target, Stall, &registers),
            machine.conditional_branch_vi_source(lower.source, Stall, &registers), effects);
    else if constexpr (lower.kind == Vu1LowerPlanKind::Fmand || lower.kind == Vu1LowerPlanKind::Fcget)
        (void)stage_opcode_threaded_flag_read(lower, registers,
            machine.flag_pipelines.visible_mac(), machine.flag_pipelines.visible_clip(), effects);
    else if constexpr (lower.kind == Vu1LowerPlanKind::Fcand || lower.kind == Vu1LowerPlanKind::Fcor)
        (void)stage_opcode_threaded_clip_predicate(lower,
            machine.flag_pipelines.visible_clip(), Pair.lower->immediate24, effects);
    else
        stage_opcode_threaded_lower_kind<lower.kind>(lower, registers, machine.data_memory, effects);
    if constexpr (!TrackMac) effects.upper_updates_mac=false;
    if (effects.upper_updates_mac)
        effects.upper_flags = static_cast<std::uint16_t>(
            mac_flags_for_vector(effects.upper_fd, effects.upper_mask) |
            mac_underflow_flags(effects.upper_underflow_mask) |
            mac_overflow_flags(effects.upper_overflow_mask));
    // I literals are observed after the upper stage, just as in the reference.
    if constexpr (Pair.i_bit) machine.last_lower = Pair.lower_word;
    (void)machine.commit_pair<true,TrackStatus>(Pair, effects, nullptr, register_frame);
    if constexpr (Pair.flow == CachedVu1Flow::ConditionalBranch)
        if (effects.branch_taken) machine.pending_branch_target=effects.branch_target;
    if constexpr (ServiceTransfers) {
        if (machine.xgkick_service_needed) {
            if (machine.services.xgkick_active())
                machine.services.synchronize_timed_execution(machine.elapsed_cycles + 1U);
            machine.advance_xgkick_if_needed(1U);
        }
    }
}

template<const auto& Schedule, const CachedVu1Pair& Last, std::uint16_t Start, bool PrivateFrame = false>
[[gnu::always_inline]] inline void finish_native_vu1_schedule_inline(Vu1ExecutionMachine& machine, const std::size_t base) {
    // These blocks cannot read STATUS or P, or issue a new EFU result.
    // A DIV publishes its predecessor before replacing the pending Q result.
    // FDIV's I/D merge commutes with FMAC's Z/S/U/O updates and broadcasts to
    // all history slots. Resolve Q uses at their scheduled issue time above,
    // then publish scalar values and I/D once at the last observed issue.
    const auto last_issue = base + Schedule.issue.back();
    if (machine.pending_q && machine.pending_q->ready_cycle <= last_issue) {
        if constexpr (PrivateFrame) machine.publish_pending_q_inline();
        else machine.publish_pending_q();
    }
    if (machine.pending_p && machine.pending_p->ready_cycle <= last_issue) {
        machine.state.write_control(kControlP, machine.pending_p->bits);
        machine.pending_p.reset();
    }
    [&]<std::size_t... Lanes>(std::index_sequence<Lanes...>) {
        ([&] { if constexpr (Schedule.vf_output_ready[Lanes])
            machine.vf_ready_cycles[Lanes] = base + Schedule.vf_output_ready[Lanes]; }(), ...);
    }(std::make_index_sequence<128>{});
    [&]<std::size_t... Registers>(std::index_sequence<Registers...>) {
        ([&] { if constexpr (Schedule.vi_output_ready[Registers])
            machine.vi_load_ready_cycles[Registers] = base + Schedule.vi_output_ready[Registers]; }(), ...);
    }(std::make_index_sequence<16>{});
    machine.elapsed_cycles = base + Schedule.cycles;
    machine.pipeline_stall_cycles += Schedule.cycles - Schedule.issue.size();
    machine.committed += Schedule.issue.size();
    machine.last_lower = Last.lower_word;
    machine.last_upper = Last.upper_word;
    machine.state.write_control(kControlTpc, (Start + Schedule.issue.size() - 1U) & kPairMask);
    machine.pc = static_cast<std::uint16_t>((Start + Schedule.issue.size()) & kPairMask);
    if constexpr (Schedule.conditional_exit) {
        if(machine.pending_branch_target) machine.pc=*machine.pending_branch_target;
        machine.pending_branch_target.reset();
    }
}

template<const auto& Schedule, const CachedVu1Pair& Last, std::uint16_t Start>
void finish_native_vu1_schedule(Vu1ExecutionMachine& machine, const std::size_t base) {
    finish_native_vu1_schedule_inline<Schedule, Last, Start>(machine, base);
}
