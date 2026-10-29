// Exercise the private native compiler seam against the complete pair machine.
// This TU supplies its VU implementation; the archive supplies other subsystems.
#define TAKEDOWN_VU1_NATIVE_SCHEDULE_TEST
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "../src/guest/vu1_micro_interpreter.cpp"
#include <iostream>

namespace takedown::guest {
namespace {
constexpr CachedVu1Pair fixture(UpperOp upper, LowerOp lower, unsigned index) {
    CachedVu1Pair p{};
    p.upper_word = 0x1000U + index;
    p.lower_word = 0x2000U + index;
    p.upper = DecodedUpper{.op=upper, .lane=static_cast<std::uint8_t>(index % 4),
                          .ft=2, .fs=static_cast<std::uint8_t>(index ? 3 : 1), .fd=3};
    p.lower = DecodedLower{.op=lower, .mask=15, .it=2, .is=1, .ft=3, .fs=3, .immediate=1};
    p.initialized = p.lower_decoded = true;
    p.destination_mask = index == 3 ? 5 : 15;
    if (upper != UpperOp::Nop) {
        p.vf_read_lanes = {static_cast<std::uint8_t>(p.upper->fs*4),
            static_cast<std::uint8_t>(p.upper->fs*4+1),
            static_cast<std::uint8_t>(p.upper->fs*4+2),
            static_cast<std::uint8_t>(p.upper->fs*4+3),
            static_cast<std::uint8_t>(8+index%4)};
        p.vf_read_lane_count = 5;
        if (native_upper_writes_fd(upper)) {
            for (unsigned lane=0; lane<4; ++lane)
                if (p.destination_mask & (8U>>lane))
                    p.upper_vf_write_lanes[p.upper_vf_write_lane_count++] = static_cast<std::uint8_t>(12+lane);
        }
    }
    if (lower == LowerOp::Lq) {
        p.lower_vf_write_lanes = {12,13,14,15}; p.lower_vf_write_lane_count=4;
    }
    p.vi_read_mask = lower == LowerOp::Nop ? 0 : 6;
    return p;
}
constexpr auto p0=fixture(UpperOp::AddLane,LowerOp::Lq,0);
constexpr auto p1=fixture(UpperOp::MulaLane,LowerOp::Iaddiu,1);
constexpr auto p2=fixture(UpperOp::MaddaLane,LowerOp::Ilw,2);
constexpr auto p3=fixture(UpperOp::MaddLane,LowerOp::Isw,3);
constexpr auto p4=fixture(UpperOp::Clip,LowerOp::Fmand,4);
constexpr auto p5=fixture(UpperOp::Nop,LowerOp::Lq,5);
constexpr auto p6=[] { auto p=fixture(UpperOp::AddLane,LowerOp::Nop,6); p.i_bit=true; return p; }();
constexpr auto p7=fixture(UpperOp::Adda,LowerOp::Lq,7);
constexpr auto p8=fixture(UpperOp::Madda,LowerOp::Lq,8);
constexpr auto p9=fixture(UpperOp::MulQ,LowerOp::Nop,9);
static_assert(native_fixed_arithmetic_supported(UpperOp::MulQ));
constexpr auto cross_fixture(UpperOp op, unsigned index) {
    auto pair=fixture(op,LowerOp::Nop,index);
    pair.destination_mask=14;
    pair.vf_read_lanes={12,13,14,8,9,10};
    pair.vf_read_lane_count=6;
    pair.upper_vf_write_lane_count=op==UpperOp::Opmsub ? 3 : 0;
    return pair;
}
constexpr auto p10=cross_fixture(UpperOp::OpMula,10);
constexpr auto p11=cross_fixture(UpperOp::Opmsub,11);
constexpr auto branch_eq=[] {
    auto pair=fixture(UpperOp::Nop,LowerOp::Ibeq,0);
    pair.flow=CachedVu1Flow::ConditionalBranch;
    pair.direct_branch_target=77;
    return pair;
}();
constexpr auto branch_ne=[] { auto pair=branch_eq; pair.lower->op=LowerOp::Ibne; return pair; }();
constexpr auto branch_delay=fixture(UpperOp::Nop,LowerOp::Iaddiu,0);
constexpr auto branch_math_eq=[] {
    auto pair=fixture(UpperOp::AddLane,LowerOp::Ibeq,0);
    pair.flow=CachedVu1Flow::ConditionalBranch;
    pair.direct_branch_target=77;
    return pair;
}();
constexpr auto branch_math_ne=[] { auto pair=branch_math_eq; pair.lower->op=LowerOp::Ibne; return pair; }();
constexpr auto branch_signed(LowerOp op) {
    auto pair=branch_math_eq;
    pair.lower->op=op;
    pair.lower->is=2;
    pair.vi_read_mask=4;
    return pair;
}
constexpr auto branch_lt=branch_signed(LowerOp::Iblt);
constexpr auto branch_gt=branch_signed(LowerOp::Ibgt);
constexpr auto branch_le=branch_signed(LowerOp::Ible);
constexpr auto branch_ge=branch_signed(LowerOp::Ibge);
constexpr auto branch_math_delay=fixture(UpperOp::AddLane,LowerOp::Iaddiu,1);
static_assert(make_native_vu1_schedule(std::array{branch_ge,branch_math_delay}).valid);
static_assert(!make_native_vu1_schedule(std::array{branch_ge,branch_math_delay,branch_delay}).valid);
static_assert(!make_native_vu1_schedule(std::array{branch_ge}).valid);
constexpr auto fused_prefix0=[] {
    auto pair=fixture(UpperOp::Nop,LowerOp::Iaddiu,0);
    pair.unsigned_immediate15=1;
    return pair;
}();
constexpr auto fused_prefix1=[] {
    auto pair=fixture(UpperOp::AddLane,LowerOp::Iaddiu,0);
    pair.unsigned_immediate15=1;
    return pair;
}();
constexpr auto fused_branch_eq=[] {
    auto pair=fixture(UpperOp::AddLane,LowerOp::Ibeq,1);
    pair.flow=CachedVu1Flow::ConditionalBranch;
    pair.direct_branch_target=77;
    return pair;
}();
constexpr auto fused_branch_ne=[] { auto pair=fused_branch_eq; pair.lower->op=LowerOp::Ibne; return pair; }();
constexpr auto loop_prefix0=fixture(UpperOp::Nop,LowerOp::Nop,0);
constexpr auto loop_prefix1=[] { auto pair=fused_prefix1; pair.lower->is=2; return pair; }();
constexpr auto loop_branch=[] { auto pair=fused_branch_ne; pair.direct_branch_target=300; return pair; }();
constexpr auto loop_delay=fixture(UpperOp::Nop,LowerOp::Nop,0);
constexpr auto loop_schedule=make_native_vu1_schedule(std::array{loop_prefix0,loop_prefix1,loop_branch,loop_delay});
static_assert(native_vu1_loop_reentry_safe<loop_schedule>());
constexpr auto loop_vf_hazard=[] {
    auto branch=branch_math_eq;
    branch.upper->fs=3;
    branch.vf_read_lanes={12,13,14,15,8};
    return make_native_vu1_schedule(std::array{branch,loop_delay});
}();
constexpr auto loop_vi_hazard=make_native_vu1_schedule(std::array{
    branch_eq,fixture(UpperOp::Nop,LowerOp::Ilw,0)});
static_assert(loop_vf_hazard.valid && !native_vu1_loop_reentry_safe<loop_vf_hazard>());
static_assert(loop_vi_hazard.valid && !native_vu1_loop_reentry_safe<loop_vi_hazard>());
constexpr auto sequence=std::array{p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11};
constexpr auto schedule=make_native_vu1_schedule(sequence);
static_assert(schedule.valid && schedule.cycles > sequence.size());
static_assert(!schedule.mac_live[0] && schedule.mac_live[3] && schedule.mac_live[11]);
static_assert(!schedule.left_normalized[0] && schedule.left_normalized[1]);
static_assert(schedule.accumulator_normalized[2]);
static_assert(!schedule.left_normalized[6] && schedule.left_normalized[7]); // LQ invalidates; ADD restores.
constexpr auto partial_normalization=[] {
    auto first=fixture(UpperOp::AddLane,LowerOp::Nop,0);
    first.destination_mask=8;
    first.upper_vf_write_lane_count=1;
    auto same=first;
    same.upper->fs=3;
    auto other=same;
    other.destination_mask=4;
    other.upper_vf_write_lanes[0]=13;
    return make_native_vu1_schedule(std::array{first,same,other});
}();
static_assert(partial_normalization.left_normalized[1] && !partial_normalization.left_normalized[2]);
static_assert(schedule.vf_output_ready[12] != 4); // Later lower write survives.
static_assert(!make_native_vu1_schedule(std::array{[] { auto p=p0; p.e_bit=true; return p; }()}).valid);
static_assert(!make_native_vu1_schedule(std::array{[] { auto p=p0; p.flow=CachedVu1Flow::DirectBranch; return p; }()}).valid);
static_assert(!make_native_vu1_schedule(std::array{[] { auto p=p0; p.lower_uses_fdiv_pipe=true; return p; }()}).valid);

void require(bool good, const char* message) {
    if (!good) { std::cerr << message << '\n'; std::exit(1); }
}

template<const CachedVu1Pair& Pair, auto Upper>
void reference_pair(Vu1ExecutionMachine& machine) {
    const auto before=machine.begin_pair(Pair);
    require(!before.terminal,"reference preissue");
    constexpr auto lower=opcode_threaded_lower_plan_for_pair(Pair,{true,true,true,true,true,true});
    Vu1PairEffects effects{};
    if constexpr (Upper != nullptr) Upper(machine.state,before.q_bits,machine.logical_clip,Pair,effects);
    if constexpr (lower.kind == Vu1LowerPlanKind::Fmand)
        (void)stage_opcode_threaded_flag_read(lower,machine.state,machine.flag_pipelines.visible_mac(),machine.flag_pipelines.visible_clip(),effects);
    else stage_opcode_threaded_lower_kind<lower.kind>(lower,machine.state,machine.data_memory,effects);
    require(!machine.finish_pair(Pair,effects,nullptr),"reference finish");
}

template<std::size_t Index,const CachedVu1Pair& Pair,auto Upper>
void native_pair(Vu1ExecutionMachine& machine,std::size_t base,Vu1State* registers) {
    constexpr auto stall=schedule.issue[Index]-(Index ? schedule.issue[Index-1]+1 : 0);
    machine.elapsed_cycles=base+schedule.issue[Index];
    if(native_vu1_flag_liveness_enabled()) {
        if(vu1_detail::native_float_arithmetic_enabled())
            issue_native_scheduled_pair<Pair,Upper,stall,true,NativeVu1Arithmetic::NativeFloatFlushed,schedule.mac_live[Index],false,
                schedule.left_normalized[Index],schedule.right_normalized[Index],schedule.accumulator_normalized[Index]>(machine,registers);
        else
            issue_native_scheduled_pair<Pair,Upper,stall,true,NativeVu1Arithmetic::Sse,schedule.mac_live[Index],false>(machine,registers);
        return;
    }
    if (vu1_detail::native_float_arithmetic_enabled())
        issue_native_scheduled_pair<Pair,Upper,stall,true,NativeVu1Arithmetic::NativeFloatFlushed,true,true,
            schedule.left_normalized[Index],schedule.right_normalized[Index],schedule.accumulator_normalized[Index]>(machine,registers);
    else
        issue_native_scheduled_pair<Pair,Upper,stall,true,NativeVu1Arithmetic::Sse>(machine,registers);
}

constexpr auto div_pair = [] {
    CachedVu1Pair p{};
    p.upper_word = (15U<<21U) | (1U<<11U) | (3U<<6U) | 0x1cU;
    p.lower_word = 0x80020bbcU; // DIV Q,VF1.x,VF2.x
    p.upper = DecodedUpper{.op=UpperOp::MulQ,.fs=1,.fd=3};
    p.lower = DecodedLower{.op=LowerOp::Div,.ft=2,.fs=1};
    p.initialized=p.lower_decoded=p.lower_uses_fdiv_pipe=true;
    p.destination_mask=15;
    p.vf_read_lanes={4,5,6,7,8}; p.vf_read_lane_count=5;
    p.upper_vf_write_lanes={12,13,14,15}; p.upper_vf_write_lane_count=4;
    return p;
}();
constexpr auto div_tail = [] { auto p=div_pair; p.lower_word=0x8000033cU;
    p.lower=DecodedLower{.op=LowerOp::Nop}; p.lower_uses_fdiv_pipe=false;
    p.vf_read_lane_count=4; return p; }();
constexpr auto div_schedule=make_native_vu1_schedule(std::array{div_pair,div_pair,div_tail});
static_assert(div_schedule.valid && div_schedule.issue[1]==7 && div_schedule.cycles==9);
constexpr auto signed_div_branch=[] {
    auto p=div_tail;
    p.lower_word=(0x2fU<<25U)|(2U<<11U)|3U;
    p.lower=DecodedLower{.op=LowerOp::Ibge,.is=2,.immediate=3};
    p.flow=CachedVu1Flow::ConditionalBranch;
    p.direct_branch_target=4;
    p.vi_read_mask=4;
    return p;
}();
constexpr auto signed_div_schedule=make_native_vu1_schedule(std::array{signed_div_branch,div_pair});
static_assert(signed_div_schedule.valid && signed_div_schedule.conditional_exit);
static_assert(!native_vu1_loop_reentry_safe<signed_div_schedule>());

void run_native_div_tests() {
#if defined(__SSE2__)
    const vu1_detail::NativeFloatArithmeticScope float_scope;
#endif
    EeMemory memory(256);
    for(unsigned pc=0;pc<3;++pc) {
        memory.write_u32_le(0x11008000U+pc*8U,pc==2 ? div_tail.lower_word : div_pair.lower_word);
        memory.write_u32_le(0x11008004U+pc*8U,div_pair.upper_word);
    }
    std::array<std::byte,kVu1MemorySize> data{};
    const Vu1ExecutionServiceCallbacks callbacks{
        .xgkick_nonzero_advance_needed=+[](const void*) noexcept{return false;},
        .advance_xgkick=+[](void*,std::uint32_t){},
        .xgkick_active=+[](const void*) noexcept{return false;},
        .flush_xgkick=+[](void*){return true;},
        .xgkick_mode=+[](const void*) noexcept{return Vu1XgkickMode::DelayedSnapshot;},
        .submit_xgkick=+[](void*,std::uint16_t){return true;},
        .read_vif1_top=+[](const void*){return std::uint32_t{};},
        .read_vif1_itop=+[](const void*){return std::uint32_t{};},
        .synchronize_timed_execution=+[](void*,std::uint64_t) noexcept{}};
    Vu1ExecutionServices services{data,data.data(),callbacks};
    constexpr std::array edge{0U,0x80000000U,1U,0x80000001U,0x3f800000U,0x40400000U,
        0x7f7fffffU,0x7f800000U,0xff800000U,0x7fc00000U};
    std::uint32_t rng=0xb736112a;
    auto next=[&]{rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
    for(unsigned trial=0;trial<2048;++trial) {
        Vu1State reference;
        for(auto& vec:reference.vectors) for(auto& word:vec.words) word=next();
        reference.vectors[1].words[0]=trial<100 ? edge[trial/10] : next();
        reference.vectors[2].words[0]=trial<100 ? edge[trial%10] : next();
        reference.control[kControlQ]=0x3f800000U;
        reference.control[kControlStatus]=next();
        reference.initialize_flag_history_from_controls();
        auto native_state=reference;
        auto fallback_state=reference;
        auto result=run_vu1_micro(reference,memory,0,{.pair_limit=3});
        Vu1ExecutionMachine fallback{fallback_state,services,0,{.pair_limit=3}};
        require(!issue_local_inline_pair<div_pair,stage_vu1_upper_mul_q_impl>(fallback,nullptr),"DIV fallback first pair");
        require(!issue_local_inline_pair<div_pair,stage_vu1_upper_mul_q_impl>(fallback,nullptr),"DIV fallback occupied pipe");
        require(!issue_local_inline_pair<div_tail,stage_vu1_upper_mul_q_impl>(fallback,nullptr),"DIV fallback Q consumer");
        fallback.persist_pending_state(false);
        require(fallback_state.vectors==reference.vectors && fallback_state.control==reference.control &&
            fallback_state.status_flag_history==reference.status_flag_history &&
            fallback.elapsed_cycles==result.elapsed_cycles && fallback.pipeline_stall_cycles==result.pipeline_stall_cycles,
            "DIV dynamic fallback differs from full interpreter");
        Vu1ExecutionMachine native{native_state,services,0,{.pair_limit=3}};
        native.pending_q=PendingQ{.bits=1,.status_di=0,.ready_cycle=1};
        require(!admit_native_vu1_schedule<div_schedule>(native,nullptr),"busy external Q admitted");
        native.pending_q.reset();
        require(admit_native_vu1_schedule<div_schedule>(native,nullptr),"free Q declined");
        native.flag_pipelines.oldest=trial%4;
        auto registers=native_vu1_register_frame<div_schedule>(native.state);
        Vu1ExecutionMachine frame{registers,native.services,0,native.options,Vu1ExecutionMachine::NativeFrameTag{}};
        const bool graph_frame=(trial/4)%2!=0;
        std::size_t flag_rotation=0, vi_rotation=0;
        if(graph_frame) {
            registers=native.state;
            copy_native_vu1_graph_machine(frame,native);
            normalize_native_vu1_loop_frame<false>(frame,flag_rotation,vi_rotation);
        } else initialize_native_vu1_machine_frame(frame,native);
        auto& execution=(graph_frame || trial%2) ? frame : native;
        issue_native_scheduled_pair<div_pair,stage_vu1_upper_mul_q_impl,0>(execution);
        require(execution.pending_q && execution.pending_q->ready_cycle==7,"DIV delayed writeback");
        execution.elapsed_cycles=7;
        issue_native_scheduled_pair<div_pair,stage_vu1_upper_mul_q_impl,6>(execution);
        require(execution.pending_q && execution.pending_q->ready_cycle==14,"second DIV replaces matured Q");
        execution.elapsed_cycles=8;
        issue_native_scheduled_pair<div_tail,stage_vu1_upper_mul_q_impl,0>(execution);
        if(graph_frame) {
            finish_native_vu1_schedule_inline<div_schedule,div_tail,0,true>(execution,0);
            static_assert(div_schedule.resets_flag_origin);
            flag_rotation=0;
            normalize_native_vu1_loop_frame<false>(execution,flag_rotation,vi_rotation);
            native.state=registers;
            publish_native_vu1_graph_machine(native,execution,flag_rotation,vi_rotation);
        } else if(trial%2) {
            publish_native_vu1_machine_frame<div_schedule.resets_flag_origin>(native,execution);
            publish_native_vu1_scalar_frame<div_schedule>(native,execution);
            publish_native_vu1_register_frame<div_schedule>(native.state,registers);
        }
        if(!graph_frame) finish_native_vu1_schedule<div_schedule,div_tail,0>(native,0);
        require(result.elapsed_cycles==native.elapsed_cycles && result.pipeline_stall_cycles==native.pipeline_stall_cycles,
            "DIV native pipe timing differs from interpreter");
        native.persist_pending_state(false);
        require(reference.vectors==native_state.vectors,"DIV upper uses pre-pair Q");
        require(reference.control[kControlQ]==native_state.control[kControlQ] &&
            reference.control[kControlStatus]==native_state.control[kControlStatus] &&
            reference.status_flag_history==native_state.status_flag_history &&
            reference.mac_flag_history==native_state.mac_flag_history,"DIV flags/Q differ from full interpreter");
    }
    // Full interpreter oracle for the hot signed-branch/DIV-delay shape.
    // Run far enough beyond either branch target to observe Q publication.
    for(unsigned pc=0;pc<12;++pc) {
        memory.write_u32_le(0x11008000U+pc*8U,
            pc==0 ? signed_div_branch.lower_word : pc==1 ? div_pair.lower_word : div_tail.lower_word);
        memory.write_u32_le(0x11008004U+pc*8U,div_pair.upper_word);
    }
    for(unsigned trial=0;trial<256;++trial) {
        Vu1State reference;
        for(auto& vector:reference.vectors) for(auto& word:vector.words) word=next();
        reference.write_integer(2,static_cast<std::uint16_t>(trial%3==0 ? 0 : trial%3==1 ? 0xffff : 0x7fff));
        reference.control[kControlQ]=0x3f800000U;
        reference.initialize_flag_history_from_controls();
        auto native_state=reference;
        const auto result=run_vu1_micro(reference,memory,0,{.pair_limit=10});
        Vu1ExecutionMachine native{native_state,services,0,{.pair_limit=10}};
        require(admit_native_vu1_schedule<signed_div_schedule>(native,nullptr),"signed DIV delay admission");
        issue_native_scheduled_pair<signed_div_branch,stage_vu1_upper_mul_q_impl,0>(native);
        native.elapsed_cycles=signed_div_schedule.issue[1];
        issue_native_scheduled_pair<div_pair,stage_vu1_upper_mul_q_impl,signed_div_schedule.issue[1]-1>(native);
        finish_native_vu1_schedule<signed_div_schedule,div_pair,0>(native,0);
        for(unsigned i=2;i<10;++i)
            require(!issue_local_inline_pair<div_tail,stage_vu1_upper_mul_q_impl>(native,nullptr),"signed DIV tail");
        native.persist_pending_state(false);
        require(native.pc==result.pc && native.elapsed_cycles==result.elapsed_cycles &&
            native.pipeline_stall_cycles==result.pipeline_stall_cycles &&
            native_state.vectors==reference.vectors && native_state.control==reference.control &&
            native_state.mac_flag_history==reference.mac_flag_history &&
            native_state.status_flag_history==reference.status_flag_history,
            "signed branch DIV delay differs from full interpreter");
    }
    // A division-pipe stall advances only TimedLive transfers. Keep the
    // synchronization sequence at the exact issued-pair boundaries.
    struct Trace { bool timed{}; std::vector<std::array<std::uint64_t,2>> events; };
    const Vu1ExecutionServiceCallbacks traced_callbacks{
        .xgkick_nonzero_advance_needed=+[](const void*) noexcept{return true;},
        .advance_xgkick=+[](void* p,std::uint32_t n){static_cast<Trace*>(p)->events.push_back({0,n});},
        .xgkick_active=+[](const void*) noexcept{return true;},
        .flush_xgkick=+[](void*){return true;},
        .xgkick_mode=+[](const void* p) noexcept{return static_cast<const Trace*>(p)->timed ? Vu1XgkickMode::TimedLive : Vu1XgkickMode::DelayedSnapshot;},
        .submit_xgkick=+[](void*,std::uint16_t){return true;},
        .read_vif1_top=+[](const void*){return std::uint32_t{};},
        .read_vif1_itop=+[](const void*){return std::uint32_t{};},
        .synchronize_timed_execution=+[](void* p,std::uint64_t n) noexcept{static_cast<Trace*>(p)->events.push_back({1,n});}};
    for(bool timed : {false,true}) {
        Trace trace{.timed=timed,.events={}};
        Vu1ExecutionServices traced_services{data,&trace,traced_callbacks};
        Vu1State state;
        Vu1ExecutionMachine execution{state,traced_services,0,{.pair_limit=3}};
        execution.xgkick_service_needed=true;
        issue_native_scheduled_pair<div_pair,stage_vu1_upper_mul_q_impl,0,true>(execution);
        execution.elapsed_cycles=7;
        issue_native_scheduled_pair<div_pair,stage_vu1_upper_mul_q_impl,6,true>(execution);
        execution.elapsed_cycles=8;
        issue_native_scheduled_pair<div_tail,stage_vu1_upper_mul_q_impl,0,true>(execution);
        std::vector<std::array<std::uint64_t,2>> expected{{1,1},{0,1}};
        if(timed) expected.push_back({0,6});
        expected.insert(expected.end(),{{1,8},{0,1},{1,9},{0,1}});
        require(trace.events==expected,"DIV stall changed timed transfer ordering");
    }
    std::cout<<"2048 native DIV cases matched full interpreter, including long stalls and special values\n";
}

void run_native_mulq_tests() {
#if defined(__SSE2__)
    const vu1_detail::NativeFloatArithmeticScope float_scope;
    constexpr std::array edge{0U,0x80000000U,1U,0x007fffffU,0x00800000U,
        0x3f800000U,0xbf800000U,0x7f7fffffU,0xff7fffffU,0x7f800000U,0x7fc12345U};
    std::uint32_t random=0xa27c35f1U;
    const auto next=[&] { random=random*1664525U+1013904223U; return random; };
    const auto check=[&]<unsigned Mask>() {
        static constexpr auto pair=[] { auto p=p9; p.destination_mask=Mask; return p; }();
        for(unsigned trial=0;trial<2048;++trial) {
            Vu1State state;
            auto& fs=state.vectors[pair.upper->fs];
            for(unsigned lane=0;lane<4;++lane)
                fs.words[lane]=trial<edge.size() ? edge[(trial+lane)%edge.size()] : next();
            const auto q=trial<edge.size()*edge.size() ? edge[(trial/edge.size())%edge.size()] : next();
            // Deliberately different: scheduled Q must come from the argument,
            // not a stale architectural control register or decoded VF0.
            state.control[kControlQ]=~q;
            Vu1UpperStageEffects expected{}, sse{}, native_float{};
            stage_vu1_upper_mul_q_impl(state,q,0,pair,expected);
            require(stage_native_vu1_arithmetic<pair,NativeVu1Arithmetic::Sse>(state,q,sse),"MULq SIMD stage declined");
            require(stage_native_vu1_arithmetic<pair,NativeVu1Arithmetic::NativeFloat>(state,q,native_float),"MULq native stage declined");
            for(const auto* actual : {&sse,&native_float})
                require(actual->upper_fd==expected.upper_fd && actual->upper_mask==expected.upper_mask &&
                    actual->upper_underflow_mask==expected.upper_underflow_mask &&
                    actual->upper_overflow_mask==expected.upper_overflow_mask &&
                    actual->upper_writes_fd && !actual->upper_writes_acc && actual->upper_updates_mac,
                    "MULq differs from scalar values/masks/exception flags");
        }
    };
    check.template operator()<0>();
    check.template operator()<5>();
    check.template operator()<10>();
    check.template operator()<15>();
    std::cout<<"8192 MULq cases matched scalar arithmetic in both native modes\n";
#endif
}

void run_native_schedule_tests() {
#if defined(__SSE2__)
    const vu1_detail::NativeFloatArithmeticScope float_scope;
#endif
    struct TransferTrace {
        std::span<const std::byte> memory;
        std::uint32_t remaining{};
        Vu1XgkickMode mode{Vu1XgkickMode::TimedLive};
        std::vector<std::array<std::uint64_t,3>> events;
    };
    const Vu1ExecutionServiceCallbacks callbacks{
        .xgkick_nonzero_advance_needed=+[](const void* context) noexcept{
            return static_cast<const TransferTrace*>(context)->remaining != 0;},
        .advance_xgkick=+[](void* context,std::uint32_t cycles){
            auto& trace=*static_cast<TransferTrace*>(context);
            std::uint64_t hash=14695981039346656037ULL;
            for(auto byte:trace.memory) hash=(hash^std::to_integer<unsigned>(byte))*1099511628211ULL;
            trace.events.push_back({0,cycles,hash});
            trace.remaining=cycles>=trace.remaining || trace.mode==Vu1XgkickMode::DelayedSnapshot ? 0 : trace.remaining-cycles;
        },
        .xgkick_active=+[](const void* context) noexcept{
            return static_cast<const TransferTrace*>(context)->remaining != 0;},
        .flush_xgkick=+[](void*){return true;},
        .xgkick_mode=+[](const void* context) noexcept{return static_cast<const TransferTrace*>(context)->mode;},
        .submit_xgkick=+[](void*,std::uint16_t){return true;},
        .read_vif1_top=+[](const void*){return std::uint32_t{0};},
        .read_vif1_itop=+[](const void*){return std::uint32_t{0};},
        .synchronize_timed_execution=+[](void* context,std::uint64_t cycles) noexcept{
            static_cast<TransferTrace*>(context)->events.push_back({1,cycles,0});}};
    std::uint32_t random=0x724137ab;
    auto next=[&] { random^=random<<13; random^=random>>17; random^=random<<5; return random; };
    for (unsigned trial=0;trial<2048;++trial) {
        std::array<std::byte,kVu1MemorySize> memory{};
        for(auto& byte:memory) byte=static_cast<std::byte>(next());
        auto native_memory=memory;
        Vu1State state;
        for(auto& vector:state.vectors) for(auto& word:vector.words) word=next();
        for(auto& word:state.accumulator.words) word=next();
        for(auto& word:state.control) word=next();
        state.initialize_flag_history_from_controls();
        for(std::size_t slot=0;slot<4;++slot) {
            state.mac_flag_history[slot]=next()&0xffffU;
            state.clip_flag_history[slot]=next()&0xffffffU;
            state.status_flag_history[slot]=next();
        }
        auto native_state=state;
        TransferTrace trace{.memory=memory,.events={}}, native_trace{.memory=native_memory,.events={}};
        Vu1ExecutionServices services{memory,&trace,callbacks}, native_services{native_memory,&native_trace,callbacks};
        Vu1ExecutionMachine reference{state,services,0,{}}, native{native_state,native_services,0,{}};
        // Ring origins must survive the private frame's normalized layout.
        reference.flag_pipelines.oldest=native.flag_pipelines.oldest=trial%4;
        reference.logical_mac=native.logical_mac=static_cast<std::uint16_t>(reference.flag_pipelines.newest_mac());
        reference.logical_clip=native.logical_clip=reference.flag_pipelines.newest_clip();
        reference.logical_status=native.logical_status=reference.flag_pipelines.newest_status();
        reference.vi_branch_history_next=native.vi_branch_history_next=trial%4;
        reference.vi_branch_history_size=native.vi_branch_history_size=trial%5;
        for(std::size_t slot=0;slot<4;++slot)
            reference.vi_branch_history[slot]=native.vi_branch_history[slot]=ViBranchHistoryEntry{
                .issue_cycle=slot,.previous_value=static_cast<std::uint16_t>(next()),
                .written_reg=static_cast<std::uint8_t>(1+next()%15),.backup_eligible=(next()&1)!=0};
        reference.elapsed_cycles=native.elapsed_cycles=trial%11;
        const auto base=native.elapsed_cycles;
        require(admit_native_vu1_schedule<schedule>(native,nullptr),"clean block declined");
        native.vf_ready_cycles[4]=base+1;
        require(!admit_native_vu1_schedule<schedule>(native,nullptr),"late external VF admitted");
        native.vf_ready_cycles[4]=0;
        native.vi_load_ready_cycles[1]=base+1;
        require(!admit_native_vu1_schedule<schedule>(native,nullptr),"late external VI admitted");
        native.vi_load_ready_cycles[1]=0;
        native.options.pair_limit=sequence.size()-1;
        require(!admit_native_vu1_schedule<schedule>(native,nullptr),"short budget admitted");
        native.options.pair_limit=sequence.size();
        native.xgkick_service_needed=true;
        require(!admit_native_vu1_schedule<schedule>(native,nullptr),"active transfer admitted");
        native.xgkick_service_needed=false;
        reference.pending_q=native.pending_q=PendingQ{.bits=next(),.status_di=next()&0x30U,
            .ready_cycle=base+trial%(schedule.cycles+3)};
        reference.pending_p=native.pending_p=PendingP{.bits=next(),
            .ready_cycle=base+trial%(schedule.cycles+4)};
        require(admit_native_vu1_schedule<schedule>(native,nullptr),"pending scalar result unnecessarily declined");
        trace.remaining=native_trace.remaining=trial%static_cast<unsigned>(schedule.cycles+4);
        trace.mode=native_trace.mode=trial%3 ? Vu1XgkickMode::TimedLive : Vu1XgkickMode::DelayedSnapshot;
        reference.xgkick_service_needed=native.xgkick_service_needed=trace.remaining!=0;
        require(admit_native_vu1_schedule<schedule,true>(native,nullptr),"transfer-aware native path declined");
        const bool graph_frame=(trial/4)%2!=0;
        const bool normalized_graph=graph_frame && (trial/8)%2!=0;
        std::size_t graph_flag_rotation=0, graph_vi_rotation=0;
        auto registers=native_vu1_register_frame<schedule>(native.state);
        if(graph_frame) registers=native.state;
        Vu1ExecutionMachine private_execution{registers,native.services,native.pc,native.options,
                                              Vu1ExecutionMachine::NativeFrameTag{}};
        if(graph_frame) copy_native_vu1_graph_machine(private_execution,native);
        else initialize_native_vu1_machine_frame(private_execution,native);
        if(normalized_graph) {
            private_execution.pending_branch_target=77;
            normalize_native_vu1_loop_frame<false>(private_execution,graph_flag_rotation,graph_vi_rotation);
            require(private_execution.pending_branch_target==77 &&
                !admit_native_vu1_schedule<schedule,true>(private_execution,nullptr),
                "graph normalization discarded entry branch hazard");
            private_execution.pending_branch_target.reset();
        }
        const bool private_frame=graph_frame || trial%2!=0;
        auto& execution=private_frame ? private_execution : native;
        const bool shared_graph=!private_frame && (trial/8)%2!=0;
#define CHECK_PAIR(Index,Pair,Upper) reference_pair<Pair,Upper>(reference); native_pair<Index,Pair,Upper>(execution,base,private_frame ? &registers : shared_graph ? &execution.state : nullptr)
        CHECK_PAIR(0,p0,stage_vu1_upper_add_lane_impl);
        CHECK_PAIR(1,p1,stage_vu1_upper_mula_lane_impl);
        CHECK_PAIR(2,p2,stage_vu1_upper_madda_lane_impl);
        CHECK_PAIR(3,p3,stage_vu1_upper_madd_lane_impl);
        CHECK_PAIR(4,p4,stage_vu1_upper_clip_impl);
        CHECK_PAIR(5,p5,nullptr);
        CHECK_PAIR(6,p6,stage_vu1_upper_add_lane_impl);
        CHECK_PAIR(7,p7,stage_vu1_upper_adda_impl);
        CHECK_PAIR(8,p8,stage_vu1_upper_madda_impl);
        CHECK_PAIR(9,p9,stage_vu1_upper_mul_q_impl);
        CHECK_PAIR(10,p10,stage_vu1_upper_opmula_impl);
        CHECK_PAIR(11,p11,stage_vu1_upper_opmsub_impl);
#undef CHECK_PAIR
        if(graph_frame) {
            finish_native_vu1_schedule_inline<schedule,p11,0,true>(execution,base);
            native.state=registers;
            if(normalized_graph) {
                if constexpr (schedule.resets_flag_origin) graph_flag_rotation=0;
                normalize_native_vu1_loop_frame<false>(execution,graph_flag_rotation,graph_vi_rotation);
                publish_native_vu1_graph_machine(native,execution,graph_flag_rotation,graph_vi_rotation);
            } else copy_native_vu1_graph_machine(native,execution);
        } else if(private_frame) {
            publish_native_vu1_machine_frame(native,execution);
            publish_native_vu1_register_frame<schedule>(native.state,registers);
        }
        if(shared_graph) finish_native_vu1_schedule_inline<schedule,p11,0,true>(native,base);
        else if(!graph_frame) finish_native_vu1_schedule<schedule,p11,0>(native,base);
        require(trace.events==native_trace.events && trace.remaining==native_trace.remaining &&
                reference.xgkick_service_needed==native.xgkick_service_needed,"transfer ordering or observed packet memory differs");
        require(reference.elapsed_cycles==native.elapsed_cycles && reference.committed==native.committed &&
            reference.pipeline_stall_cycles==native.pipeline_stall_cycles && reference.pc==native.pc &&
            reference.vf_ready_cycles==native.vf_ready_cycles && reference.vi_load_ready_cycles==native.vi_load_ready_cycles,
            "native schedule differs from dynamic hazards/retirement");
        require(reference.vi_branch_history_next==native.vi_branch_history_next &&
                reference.vi_branch_history_size==native.vi_branch_history_size,"VI history cursor");
        for(std::size_t i=0;i<reference.vi_branch_history.size();++i) {
            const auto& a=reference.vi_branch_history[i]; const auto& b=native.vi_branch_history[i];
            require(a.issue_cycle==b.issue_cycle && a.previous_value==b.previous_value && a.written_reg==b.written_reg &&
                    a.backup_eligible==b.backup_eligible,"VI branch backup");
        }
        require(state.control==native_state.control &&
                reference.pending_q.has_value()==native.pending_q.has_value() &&
                reference.pending_p.has_value()==native.pending_p.has_value(),"scalar publication boundary");
        require(reference.flag_pipelines.oldest==native.flag_pipelines.oldest &&
            reference.flag_pipelines.mac==native.flag_pipelines.mac &&
            reference.flag_pipelines.clip==native.flag_pipelines.clip,"flag ring publication");
        reference.persist_pending_state(false); native.persist_pending_state(false);
        for(std::size_t i=0;i<32;++i) require(state.vectors[i].words==native_state.vectors[i].words,"VF output");
        for(std::size_t index=0;index<state.control.size();++index)
            if(index!=kControlStatus || !native_vu1_flag_liveness_enabled())
                require(state.control[index]==native_state.control[index],"control output");
        require(state.accumulator.words==native_state.accumulator.words &&
            state.mac_flag_history==native_state.mac_flag_history && state.clip_flag_history==native_state.clip_flag_history &&
            (native_vu1_flag_liveness_enabled() || state.status_flag_history==native_state.status_flag_history) &&
            memory==native_memory,"memory/flags/control output");

        // Exercise the compiled branch + delay pair with an independent
        // predicate oracle: adjacency backup, flag-write barriers, stalls,
        // both outcomes, history wrap, and a delay slot at PC zero.
        const auto branch_pc=static_cast<std::uint16_t>(trial%2 ? 2047 : 100);
        reference.pc=native.pc=branch_pc;
        reference.vf_ready_cycles.fill(0); native.vf_ready_cycles.fill(0);
        state.write_integer(1,11); native_state.write_integer(1,11);
        constexpr std::array<std::uint16_t,6> signed_edges{0,1,0x7fff,0x8000,0xffff,0xfffe};
        const auto current=static_cast<std::uint16_t>((trial&16U) ? signed_edges[(trial/32)%signed_edges.size()] : trial%2 ? 11 : 12);
        const auto previous=static_cast<std::uint16_t>((trial&16U) ? signed_edges[(trial/64+3)%signed_edges.size()] : 11);
        state.write_integer(2,current); native_state.write_integer(2,current);
        const bool backup=trial%3!=0;
        const auto newest=trial%4;
        reference.vi_branch_history_size=native.vi_branch_history_size=1;
        reference.vi_branch_history_next=native.vi_branch_history_next=(newest+1)&3U;
        reference.vi_branch_history[newest]=native.vi_branch_history[newest]=ViBranchHistoryEntry{
            .issue_cycle=reference.elapsed_cycles-1,.previous_value=previous,.written_reg=2,.backup_eligible=backup};
        const bool stall=trial%4==0;
        reference.vi_load_ready_cycles[1]=native.vi_load_ready_cycles[1]=reference.elapsed_cycles+(stall ? 2 : 0);
        reference.vi_load_ready_cycles[2]=native.vi_load_ready_cycles[2]=reference.elapsed_cycles+(stall ? 2 : 0);
        trace.remaining=native_trace.remaining=trial%3;
        reference.xgkick_service_needed=native.xgkick_service_needed=trace.remaining!=0;
        const auto check_branch=[&]<const CachedVu1Pair& Pair,std::uint16_t Start,
                                   const CachedVu1Pair& Delay=branch_delay,auto Upper=nullptr,auto DelayUpper=nullptr>() {
            static constexpr auto control_schedule=make_native_vu1_schedule(std::array{Pair,Delay});
            static_assert(control_schedule.valid && control_schedule.conditional_exit);
            reference.options.pair_limit=native.options.pair_limit=native.committed+2;
            const auto control_base=native.elapsed_cycles;
            const bool fast=admit_native_vu1_schedule<control_schedule,true>(native,nullptr);
            require(fast!=stall,"compiled branch hazard admission");
            const auto read_value=backup && !stall ? previous : current;
            const auto signed_value=std::bit_cast<std::int16_t>(read_value);
            const bool taken=Pair.lower->op==LowerOp::Ibeq ? read_value==11 :
                Pair.lower->op==LowerOp::Ibne ? read_value!=11 :
                Pair.lower->op==LowerOp::Iblt ? signed_value<0 :
                Pair.lower->op==LowerOp::Ibgt ? signed_value>0 :
                Pair.lower->op==LowerOp::Ible ? signed_value<=0 : signed_value>=0;
            const auto before=reference.begin_pair(Pair);
            require(!before.terminal,"branch reference preissue");
            Vu1PairEffects effects{};
            if constexpr(Upper!=nullptr) Upper(reference.state,before.q_bits,reference.logical_clip,Pair,effects);
            effects.branch_taken=taken;
            effects.branch_target=77;
            require(!reference.finish_pair(Pair,effects,nullptr),"branch reference finish");
            if(fast) issue_native_scheduled_pair<Pair,Upper,0,true>(native);
            else require(!issue_local_inline_pair<Pair,Upper>(native,nullptr),"compiled branch finish");
            require(native.pending_branch_target.has_value()==taken &&
                    (fast || native.pc==((branch_pc+1)&2047)),"branch predicate or delay PC");
            reference_pair<Delay,DelayUpper>(reference);
            if(fast) {
                native.elapsed_cycles=control_base+control_schedule.issue[1];
                issue_native_scheduled_pair<Delay,DelayUpper,control_schedule.issue[1]-1,true>(native);
                finish_native_vu1_schedule<control_schedule,Delay,Start>(native,control_base);
            } else require(!issue_local_inline_pair<Delay,DelayUpper>(native,nullptr),"compiled delay finish");
            require(native.pc==(taken ? 77 : ((branch_pc+2)&2047)) && !native.pending_branch_target,
                    "compiled delay did not preserve latched branch target");
        };
        if(trial&16U) {
            const auto signed_case=[&]<const CachedVu1Pair& Pair>() {
                if(trial%2) check_branch.template operator()<Pair,2047,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
                else check_branch.template operator()<Pair,100,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
            };
            switch((trial/4)%4) {
            case 0: signed_case.template operator()<branch_lt>(); break;
            case 1: signed_case.template operator()<branch_gt>(); break;
            case 2: signed_case.template operator()<branch_le>(); break;
            default: signed_case.template operator()<branch_ge>(); break;
            }
        } else if(trial&8U) {
            if(trial&4U) {
                if(trial%2) check_branch.template operator()<branch_math_eq,2047,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
                else check_branch.template operator()<branch_math_eq,100,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
            } else {
                if(trial%2) check_branch.template operator()<branch_math_ne,2047,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
                else check_branch.template operator()<branch_math_ne,100,branch_math_delay,stage_vu1_upper_add_lane_impl,stage_vu1_upper_add_lane_impl>();
            }
        } else if(trial&4U) {
            if(trial%2) check_branch.template operator()<branch_eq,2047>();
            else check_branch.template operator()<branch_eq,100>();
        } else {
            if(trial%2) check_branch.template operator()<branch_ne,2047>();
            else check_branch.template operator()<branch_ne,100>();
        }
        require(reference.pc==native.pc && reference.committed==native.committed &&
                reference.elapsed_cycles==native.elapsed_cycles &&
                reference.pipeline_stall_cycles==native.pipeline_stall_cycles &&
                trace.events==native_trace.events && state.read_integer(2)==native_state.read_integer(2),
                "compiled branch retirement, VI write or transfer trace differs");
        require(state.vectors==native_state.vectors && reference.logical_mac==native.logical_mac &&
                reference.flag_pipelines.mac==native.flag_pipelines.mac &&
                reference.vf_ready_cycles==native.vf_ready_cycles,
                "branch/delay arithmetic, delayed flags or readiness differs");
        if(trial<64) {
            // A fused private frame writes VI2 before its branch. A VF stall
            // at that branch disables backup, so it must read the private VI
            // value (12), not the still-unpublished architectural value (11).
            reference.pc=native.pc=200;
            state.write_integer(1,11); native_state.write_integer(1,11);
            state.write_integer(2,11); native_state.write_integer(2,11);
            reference.vf_ready_cycles.fill(0); native.vf_ready_cycles.fill(0);
            reference.vi_load_ready_cycles.fill(0); native.vi_load_ready_cycles.fill(0);
            reference.vi_branch_history_size=native.vi_branch_history_size=0;
            reference.options.pair_limit=native.options.pair_limit=native.committed+4;
            trace.remaining=native_trace.remaining=4;
            reference.xgkick_service_needed=native.xgkick_service_needed=true;
            const auto fused=[&]<const CachedVu1Pair& Branch>() {
                static constexpr auto plan=make_native_vu1_schedule(std::array{
                    fused_prefix0,fused_prefix1,Branch,branch_delay});
                static_assert(plan.valid && plan.conditional_exit && plan.issue[2]-plan.issue[1]-1==3);
                require(admit_native_vu1_schedule<plan,true>(native,nullptr),"fused schedule entry declined");
                auto local_registers=native_vu1_register_frame<plan>(native.state);
                Vu1ExecutionMachine local{local_registers,native.services,native.pc,native.options,
                                          Vu1ExecutionMachine::NativeFrameTag{}};
                initialize_native_vu1_machine_frame(local,native);
                const auto start=native.elapsed_cycles;
                reference_pair<fused_prefix0,nullptr>(reference);
                reference_pair<fused_prefix1,stage_vu1_upper_add_lane_impl>(reference);
                require(reference.state.read_integer(2)==12,"fused prefix did not change branch operand");
                const auto before=reference.begin_pair(Branch);
                require(!before.terminal && before.stall_cycles==3,"fused branch stall oracle");
                Vu1PairEffects effects{};
                stage_vu1_upper_add_lane_impl(reference.state,before.q_bits,reference.logical_clip,Branch,effects);
                effects.branch_taken=Branch.lower->op==LowerOp::Ibne;
                effects.branch_target=77;
                require(!reference.finish_pair(Branch,effects,nullptr),"fused reference branch");
                reference_pair<branch_delay,nullptr>(reference);
                local.elapsed_cycles=start;
                issue_native_scheduled_pair<fused_prefix0,nullptr,0,true>(local,&local_registers);
                local.elapsed_cycles=start+plan.issue[1];
                issue_native_scheduled_pair<fused_prefix1,stage_vu1_upper_add_lane_impl,0,true>(local,&local_registers);
                local.elapsed_cycles=start+plan.issue[2];
                issue_native_scheduled_pair<Branch,stage_vu1_upper_add_lane_impl,3,true>(local,&local_registers);
                local.elapsed_cycles=start+plan.issue[3];
                issue_native_scheduled_pair<branch_delay,nullptr,0,true>(local,&local_registers);
                publish_native_vu1_machine_frame(native,local);
                publish_native_vu1_register_frame<plan>(native.state,local_registers);
                finish_native_vu1_schedule<plan,branch_delay,200>(native,start);
                require(native.pc==(Branch.lower->op==LowerOp::Ibne ? 77 : 204) &&
                        native.pc==reference.pc && !native.pending_branch_target &&
                        state.vectors==native_state.vectors && state.read_integer(2)==native_state.read_integer(2) &&
                        native.elapsed_cycles==reference.elapsed_cycles &&
                        native.committed==reference.committed &&
                        native.flag_pipelines.mac==reference.flag_pipelines.mac &&
                        native.vf_ready_cycles==reference.vf_ready_cycles && trace.events==native_trace.events,
                        "fused private-frame branch changed output, timing, flags or transfer ordering");
            };
            if(trial%2) fused.template operator()<fused_branch_eq>();
            else fused.template operator()<fused_branch_ne>();

            // Repeated native iterations retain one register/machine frame.
            // Exercise a taken back edge, natural exit and budget exit with
            // a partial next-iteration budget, across all original ring origins.
            reference.pc=native.pc=300;
            state.write_integer(1,static_cast<std::uint16_t>(2+trial%4));
            native_state.write_integer(1,state.read_integer(1));
            state.write_integer(2,0); native_state.write_integer(2,0);
            reference.vf_ready_cycles.fill(0); native.vf_ready_cycles.fill(0);
            reference.vi_load_ready_cycles.fill(0); native.vi_load_ready_cycles.fill(0);
            reference.vi_branch_history_size=native.vi_branch_history_size=0;
            const auto iteration_limit=1+trial%6;
            reference.options.pair_limit=native.options.pair_limit=native.committed+4*iteration_limit+trial%4;
            trace.remaining=native_trace.remaining=static_cast<std::uint32_t>(trial%24);
            reference.xgkick_service_needed=native.xgkick_service_needed=trace.remaining!=0;
            require(admit_native_vu1_schedule<loop_schedule,true>(native,nullptr),"native loop entry declined");
            auto loop_registers=native_vu1_register_frame<loop_schedule>(native.state);
            Vu1ExecutionMachine loop_frame{loop_registers,native.services,native.pc,native.options,
                                          Vu1ExecutionMachine::NativeFrameTag{}};
            initialize_native_vu1_machine_frame(loop_frame,native);
            auto loop_base=native.elapsed_cycles;
            std::size_t iterations=0, flag_rotation=0, vi_rotation=0;
            for (;;) {
                reference_pair<loop_prefix0,nullptr>(reference);
                reference_pair<loop_prefix1,stage_vu1_upper_add_lane_impl>(reference);
                const auto before=reference.begin_pair(loop_branch);
                require(!before.terminal && before.stall_cycles==3,"loop branch dependency oracle");
                Vu1PairEffects effects{};
                stage_vu1_upper_add_lane_impl(reference.state,before.q_bits,reference.logical_clip,loop_branch,effects);
                effects.branch_taken=reference.state.read_integer(2)!=reference.state.read_integer(1);
                effects.branch_target=300;
                require(!reference.finish_pair(loop_branch,effects,nullptr),"loop reference branch");
                reference_pair<loop_delay,nullptr>(reference);
                loop_frame.elapsed_cycles=loop_base;
                issue_native_scheduled_pair<loop_prefix0,nullptr,0,true>(loop_frame,&loop_registers);
                loop_frame.elapsed_cycles=loop_base+loop_schedule.issue[1];
                issue_native_scheduled_pair<loop_prefix1,stage_vu1_upper_add_lane_impl,0,true>(loop_frame,&loop_registers);
                loop_frame.elapsed_cycles=loop_base+loop_schedule.issue[2];
                issue_native_scheduled_pair<loop_branch,stage_vu1_upper_add_lane_impl,3,true>(loop_frame,&loop_registers);
                loop_frame.elapsed_cycles=loop_base+loop_schedule.issue[3];
                issue_native_scheduled_pair<loop_delay,nullptr,0,true>(loop_frame,&loop_registers);
                ++iterations;
                if(iterations>=iteration_limit || loop_frame.pending_branch_target!=300U) break;
                normalize_native_vu1_loop_frame(loop_frame,flag_rotation,vi_rotation);
                loop_base+=loop_schedule.cycles;
            }
            require(iterations==std::min<std::size_t>(iteration_limit,2+trial%4),"loop exit iteration differs");
            native.flag_pipelines.oldest=(native.flag_pipelines.oldest+flag_rotation)&3U;
            native.vi_branch_history_next=(native.vi_branch_history_next+vi_rotation)&3U;
            publish_native_vu1_machine_frame(native,loop_frame);
            publish_native_vu1_register_frame<loop_schedule>(native.state,loop_registers);
            native.committed+=(iterations-1)*4;
            native.pipeline_stall_cycles+=(iterations-1)*(loop_schedule.cycles-4);
            finish_native_vu1_schedule<loop_schedule,loop_delay,300>(native,loop_base);
            require(reference.pc==native.pc && reference.committed==native.committed &&
                    reference.elapsed_cycles==native.elapsed_cycles &&
                    reference.pipeline_stall_cycles==native.pipeline_stall_cycles &&
                    state.vectors==native_state.vectors && state.read_integer(2)==native_state.read_integer(2) &&
                    reference.vf_ready_cycles==native.vf_ready_cycles &&
                    reference.flag_pipelines.mac==native.flag_pipelines.mac &&
                    reference.flag_pipelines.oldest==native.flag_pipelines.oldest &&
                    reference.vi_branch_history_next==native.vi_branch_history_next &&
                    reference.vi_branch_history_size==native.vi_branch_history_size &&
                    trace.events==native_trace.events,
                    "native loop changed output, retirement, history origins or transfer order");
            for(std::size_t slot=0;slot<4;++slot) {
                const auto& expected=reference.vi_branch_history[slot];
                const auto& actual=native.vi_branch_history[slot];
                require(expected.issue_cycle==actual.issue_cycle && expected.previous_value==actual.previous_value &&
                        expected.written_reg==actual.written_reg && expected.backup_eligible==actual.backup_eligible,
                        "native loop changed delayed VI history");
            }
        }
    }
    std::cout<<"2048 native schedule differential cases passed\n";
}

void run_outer_product_stage_tests() {
    // Compare the new vectorized stages to the original scalar arithmetic,
    // including exceptional encodings, fixed XYZ masks and preserved W.
    if (vu1_detail::native_float_arithmetic_enabled()) return;
    std::uint32_t random=0x94634752U;
    auto next=[&] { random^=random<<13;random^=random>>17;random^=random<<5;return random; };
    constexpr std::array edge={0U,0x80000000U,1U,0x007fffffU,0x00800000U,
        0x7f7fffffU,0xff7fffffU,0x7f800000U,0xff800000U,0x7fc00000U,0x3f800001U};
    for(unsigned trial=0;trial<4096;++trial) {
        Vu1State state;
        for(auto& vector:state.vectors)
            for(auto& word:vector.words) word=trial<edge.size() ? edge[(trial+next())%edge.size()] : next();
        for(auto& word:state.accumulator.words) word=next();
        for(bool subtract:{false,true}) {
            auto pair=subtract ? p11 : p10;
            pair.destination_mask=static_cast<std::uint8_t>(trial%16); // Ignored by these opcodes.
            pair.upper->fs=static_cast<std::uint8_t>(trial%4);
            pair.upper->ft=static_cast<std::uint8_t>((trial/4)%4);
            const auto fs=state.read_vector(pair.upper->fs);
            const auto ft=state.read_vector(pair.upper->ft);
            const auto acc=state.read_accumulator_ref();
            Vu1UpperStageEffects actual{},expected{};
            expected.upper_fd=subtract ? fs : acc;
            for(unsigned lane=0;lane<3;++lane)
                expected.upper_fd.words[lane]=subtract ?
                    stage_upper_msub(acc.words[lane],fs.words[(lane+1)%3],ft.words[(lane+2)%3],expected,8U>>lane) :
                    stage_upper_mul(fs.words[(lane+1)%3],ft.words[(lane+2)%3],expected,8U>>lane);
            if(subtract) stage_vu1_upper_opmsub_impl(state,0,0,pair,actual);
            else stage_vu1_upper_opmula_impl(state,0,0,pair,actual);
            require(actual.upper_fd==expected.upper_fd && actual.upper_mask==14 &&
                actual.upper_underflow_mask==expected.upper_underflow_mask &&
                actual.upper_overflow_mask==expected.upper_overflow_mask &&
                actual.upper_writes_fd==subtract && actual.upper_writes_acc!=subtract &&
                actual.upper_updates_mac,"cross-product stage differs from scalar oracle");
        }
    }
    std::cout<<"8192 cross-product stage differential cases passed\n";
}
} // namespace
} // namespace takedown::guest

int main() {
    takedown::guest::run_native_mulq_tests();
    takedown::guest::run_outer_product_stage_tests();
    takedown::guest::run_native_schedule_tests();
    takedown::guest::run_native_div_tests();
}
