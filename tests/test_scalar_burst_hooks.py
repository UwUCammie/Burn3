#!/usr/bin/env python3
"""Source-level consistency check for the boot runner's scalar burst.

src/app/main.cpp runs a very large outer loop body before every interpreted
guest instruction. The scalar burst (TAKEDOWN_SCALAR_BURST=1) skips that body
for stretches of ordinary instructions, which is only exact while it accounts
for everything the body can do at an instruction boundary. This test parses
main.cpp and checks the accounting in three ways.

1. Registration. Every comparison of the guest PC in the loop body, and in
   every lambda the loop body can reach, must be against an address or range
   that the block between SCALAR-BURST-HOOKS-BEGIN/END registers:
     * `pc == X` needs X registered;
     * a range test needs its lower and upper bound in ONE conjunction, and
       the (begin, end, closed/half-open) triple must equal one registered
       range. A one-sided or mixed-range bound is not accepted;
     * `pc != X` acts at every other PC, and a comparison of the executed
       instruction's PC (result.instruction.pc) refers to an instruction the
       hook filter never saw, so both always need an explicit review entry.
   Helpers that receive the PC or the whole CPU state, PC arithmetic, PC
   stores, PC-indexed containers and CPU-state accessors are review tables.

2. Burst-boundary trace. The test walks the loop body the way an ordinary
   iteration executes it at a boundary the burst skips: static eligibility
   holds, the start predicate holds, no horizon deadline is reached, the PC
   is unmarked and the RAM watch is quiet. Under those assumptions every
   condition is evaluated to false / true / unknown from the FACTS table and
   the registered PCs; a false branch is skipped, anything else is entered.
   Every statement the walk can reach must be
     * a local declaration whose initializer is side-effect free (only calls
       named in PURE_METHODS; a constructor only for PLAIN_LOCAL_TYPES), or
     * listed in RUNS with the reason the burst may skip it (replayed per
       burst step, bounded by the horizon, idempotent, host-only), and
       reached exactly as many times as reviewed.
   Conditions are checked in evaluation order, so a call or increment placed
   in front of a PC test is seen, and only state.pc() (or a const copy taken
   before the step) counts as the PC the hook filter tested. Lambdas defined
   before the loop are walked the same way where a reachable statement names
   them; the one host-only lambda the burst deliberately skips is pinned by
   digest. A new per-iteration service, counter, poll or un-gated statement
   therefore fails here whatever object it touches, and so does a PC or
   register test hidden in a lambda.

3. Shape pins. The burst loop, its start conditions, the start predicate,
   the horizon and the RAM watch are compared statement by statement with the
   reviewed text in EXPECTED_SHAPES, and the eligibility predicate must be
   exactly the reviewed conjunction. Removing or weakening a per-step check
   (`if (false || ...)`, a dropped `|| burst_ram_watch.fires()`, an emptied
   horizon term, `(... || true)`, a deleted `return false;`) fails here.

Every table entry names the registered address, eligibility term, predicate
or horizon term, or burst text it relies on, and the test fails when that
justification is no longer present in main.cpp.

Limits. The check is textual. It cannot see inside a method it does not
parse: a reviewed call (kernel dispatchers, the SPU2 pump, a PURE_METHODS
name) is trusted to behave as its table entry says, and callbacks that
kernel or memory code invokes on its own are outside the walk. End-to-end
identity of a run with the burst off and on remains the qualifying test.
"""

import collections
import hashlib
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAIN_CPP = ROOT / "src" / "app" / "main.cpp"
HARNESS = ROOT / "tools" / "run_windows_performance.py"

LOOP_HEAD = "    while (committed_steps < maximum_steps) {"
FUNCTION_HEAD = "int run_boot_entry("
NAMESPACES = ("takedown", "runtime", "guest", "recompiler", "slus_21050")
NATIVE_ONLY = "TAKEDOWN_NATIVE_ONLY_EXECUTABLE"

SWITCH_DEFINITION = (
    'const bool scalar_burst_requested = [] { '
    'const auto* value = std::getenv("TAKEDOWN_SCALAR_BURST"); '
    'return value != nullptr && std::string_view{value} == "1"; }();')

FALSE, TRUE, UNKNOWN = "false", "true", "unknown"


# --------------------------------------------------------------------------
# Review tables. Every entry says how the scalar burst accounts for the use:
#   registered:  normalized expressions that must be marked in the hook block
#   hook_text:   text that must appear in the hook block (registry loops)
#   eligibility: terms that must appear in the static eligibility predicate
#   predicate:   text that must appear in the start predicate
#   horizon:     text that must appear in the horizon computation
#   burst:       text that must appear in the per-step burst code
#   watch:       text that must appear in the burst's guest-RAM watch mirror
#   loop:        text that must appear in the outer loop body
#   assignments: "name=N": the loop body assigns `name` in exactly N places, so
#                a new writer of a variable a fact relies on forces a review
#   why:         the reviewed reason (documentation only)
# --------------------------------------------------------------------------
REQUIREMENT_KINDS = ("registered", "hook_text", "eligibility", "predicate", "horizon", "burst",
                     "watch", "loop", "assignments")


def cover(why, **requirements):
    unknown = set(requirements) - set(REQUIREMENT_KINDS)
    assert not unknown, unknown
    return {"why": why, **{key: tuple(value) for key, value in requirements.items()}}


def counted(count, why, **requirements):
    """A review that covers exactly `count` occurrences; one more needs a new review."""
    return {**cover(why, **requirements), "count": count}


NATIVE_REGISTRY = "native_function_dispatcher.registered_functions()"
COMPILED_REGISTRY = "compiled_block_dispatcher.registered_blocks()"
FRAME_RATE_REGISTRY = "Slus21050VariableFrameRate::hook_addresses()"
PC_OPTIONS_REGISTRY = "Slus21050NativePcOptions::hook_addresses()"
CAMERA_FOV_REGISTRY = "Slus21050CameraFov::hook_addresses()"
CAMERA_FOV_HOOK_LOOP = (
    "for (const auto address : takedown::runtime::Slus21050CameraFov::hook_addresses()) { "
    "hooks.mark_pc(address); }")
MODEL_PARENT_HOOK = "if (native_model_live) hooks.mark_pc(0x0021d2e0U);"
MODEL_FAILURE_PREDICATE = "native_model_live && native_model_live->failed()"
NATIVE_HOOK_LOOP = (
    "for (const auto& function : native_function_dispatcher.registered_functions()) { "
    "if (!hooks.marked(function.address)) { "
    "scalar_burst_native_entries.add(function.address); } "
    "hooks.mark_pc(function.address); }")
NO_COMPILED = "!compiled_coverage_available"
FLUSH_ENTRY = "VifPacketRingFlushClaimTracker::kFlushEntryAddress"
FLUSH_ACTIVE = "vif_packet_ring_flush_claim_tracker.active()"
UI_ENTRY = "SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress"
UI_PER_STEP = "ui_draw_session_feed.requires_pre_instruction("
JOIN_ENTRY = "JoinPumpRegion::kEntryAddress"
JOIN_END = "JoinPumpRegion::kCallbackTailEffectEndAddress"
CALLBACK_RETURN = "EeKernelHle::kGuestCallbackReturnAddress"
RESULT_STOP = "burst_stop = takedown::runtime::ScalarBurstStop::Result;"
NO_HOST_PHASE_PROFILER = "ee_host_phase_profiler == nullptr"
STEP_LEAF = "result=step_r5900(state,memory,instruction_cache);"

# Helpers that receive the guest PC as an argument.
PC_HELPERS = {
    "vif_packet_ring_flush_claim_tracker.requires_pre_instruction": cover(
        "consulted only while a flush is tracked (no burst) or at the flush entry",
        registered=[FLUSH_ENTRY], predicate=[FLUSH_ACTIVE]),
    "vif_packet_ring_flush_claim_tracker.observe_post_native_execution": cover(
        "runs only after a handled native dispatch, i.e. at a registered entry",
        hook_text=[NATIVE_REGISTRY]),
    "vif_packet_ring_flush_claim_tracker.observe_post_instruction": cover(
        "runs only while a flush is tracked, which forbids the bypass step and a burst",
        registered=[FLUSH_ENTRY], predicate=[FLUSH_ACTIVE]),
    "vif_packet_ring_flush_claim_tracker.clip_compiled_budget": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "ui_draw_session_feed.requires_pre_instruction": cover(
        "fixed entry is marked; learned boundaries of an active session are "
        "queried before every burst step with the same context",
        registered=[UI_ENTRY], burst=[UI_PER_STEP, "ui_draw_session_feed.active()"]),
    "ui_draw_session_feed.can_dispatch_atomic_native_function": cover(
        "consulted only for a native candidate, i.e. at a registered entry",
        hook_text=[NATIVE_REGISTRY]),
    "ui_draw_session_feed.clip_compiled_budget": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "ui_draw_packet_session_adapter.expire_unclaimed_plain_text_entry": cover(
        "called only at the plain-text submit entry",
        registered=["kSubmitPlainTextLinesAddress"]),
    "femain_park_pcs->observe": cover(
        "samples every PC", eligibility=["!femain_park_pcs.has_value()"]),
    "femain_alt_phase_pcs->observe": cover(
        "samples every PC; exists only with the park diagnostic",
        eligibility=["!femain_park_pcs.has_value()"]),
    "model_submission_probe.wants_pc": cover(
        "fixed and learned PCs", eligibility=["!model_submission_probe.enabled()"]),
    "instance_batch_capture.wants_pc": cover(
        "fixed and learned lifecycle PCs; active capture observes every dispatch and is "
        "statically ineligible", eligibility=["!instance_batch_capture.enabled()"]),
    "femain_retail_consume_invocation_barrier->observe_dispatch": cover(
        "observes every dispatch while armed",
        eligibility=["femain_retail_consume_invocation_barrier == nullptr"]),
    "observe_profiled_ee_span": cover(
        "per-commit EE PC sample", eligibility=["ee_pc_sampler == nullptr"]),
    "Slus21050CollisionTrace::is_target": cover(
        "focused trace targets", eligibility=["collision_trace == nullptr"]),
    "Slus21050VehicleWorldContactEndpointProbe::is_target": cover(
        "focused probe targets", eligibility=["vehicle_world_contact_probe == nullptr"]),
    "Slus21050SparkTrace::is_target": cover(
        "focused trace targets", eligibility=["spark_trace == nullptr"]),
    "Slus21050FemainFrameClosureDiagnostic::is_scalar_target": cover(
        "diagnostic targets", eligibility=["femain_frame_closure_diagnostic == nullptr"]),
    "is_femain_dynamic_callback_scalar_target": cover(
        "probe targets", eligibility=["!femain_dynamic_callback_probe_enabled"]),
    "ee_dispatch_preparation_cache.prepare": cover(
        "pure lookup; yields a candidate only at a registered native or compiled entry",
        hook_text=[NATIVE_REGISTRY, COMPILED_REGISTRY], eligibility=[NO_COMPILED]),
    "native_function_dispatcher.prepare": cover(
        "called only for a native candidate", hook_text=[NATIVE_REGISTRY]),
    "native_function_dispatcher.lookup": cover(
        "compiled successor scan only", eligibility=[NO_COMPILED]),
    "decode_r5900": cover(
        "rebuilds a compiled block's system-call boundary", eligibility=[NO_COMPILED]),
    "memory.contains": cover(
        "the filter only leaves aligned KUSEG main-RAM PCs unmarked, and those are mapped",
        hook_text=["ScalarBurstHookFilter hooks{memory.size()}"]),
    "memory.read_u32_le": cover("reads the code word at the PC for a trace or a decode"),
    "memory.set_current_ee_pc": cover(
        "provenance for the instruction about to run; repeated before every burst step",
        burst=["memory.set_current_ee_pc(burst_pc)",
               "memory.set_current_guest_step(committed_steps)"]),
    "scalar_burst_hooks.marked": cover("the hook filter itself"),
    "scalar_burst_native_entries.contains": cover(
        "a registered native entry that no other mark claims (the registry loop registers "
        "natives last and records only entries still unmarked): the outer loop does nothing "
        "PC-dependent there but offer it to the native dispatcher, which the burst does itself",
        hook_text=[NATIVE_REGISTRY, "scalar_burst_native_entries.add(function.address)"]),
    "burst_prepare_native": cover(
        "the native stage's candidate lookup, scheduler preflight and preparation, made only "
        "at such an entry; it also reports when the outer iteration would dispatch nothing "
        "(no candidate, a blocked preflight that no exact-cost probe can resolve, or a "
        "resolver that rejects the invocation) and would therefore only step",
        hook_text=[NATIVE_REGISTRY], burst=["scalar_burst_native_entries.contains(burst_pc)"]),
    "ee_aot_note_native_decline": cover(
        "statistics only (TAKEDOWN_EE_AOT_MISSES): counts the native entry the burst stepped "
        "through; no architectural effect",
        burst=["ee_aot_note_native_decline(burst_pc)"]),
    "ee_aot_note_hook_stop": cover(
        "statistics only (TAKEDOWN_EE_AOT_MISSES): counts the PC at which a burst ended for a "
        "hook; no architectural effect",
        burst=["ee_aot_note_hook_stop(burst_pc)"]),
    "hex32": cover("formats the PC for a diagnostic line"),
}

# Helpers that receive the whole CPU state (and can therefore test its PC).
STATE_HELPERS = {
    "takedown::guest::step_r5900": cover("executes the instruction; the burst repeats it"),
    "takedown::guest::run_ee_aot": cover(
        "compiled game code inside the burst: every instruction but the last passes each "
        "per-step burst check (include/takedown/guest/ee_aot.hpp); the last is checked as usual",
        burst=["ee_aot_burst_step(result, burst_budget, burst_declined_entry)"]),
    "kernel.handle_system_call": cover(
        "system-call tail; a burst always ends at a non-Executed result",
        burst=[RESULT_STOP]),
    "kernel.finish_guest_callback": cover(
        "acts only at the callback return sentinel", registered=[CALLBACK_RETURN]),
    "variable_frame_rate->on_dispatch": cover(
        "acts only at the variable frame-rate hook addresses, which the hook block marks",
        hook_text=[FRAME_RATE_REGISTRY]),
    "native_pc_options.on_dispatch": cover(
        "acts only at the PC options hook addresses, which the hook block marks",
        hook_text=[PC_OPTIONS_REGISTRY]),
    "camera_fov.on_dispatch": cover(
        "called only at the FOV-use PC; its complete registry is always marked",
        hook_text=[CAMERA_FOV_HOOK_LOOP]),
    "native_car_select.on_dispatch": cover(
        "called only when the current PC is marked, before offering native dispatch",
        hook_text=["Slus21050CarSelect::hook_addresses()"],
        loop=["native_car_select_active && scalar_burst_hooks.marked(dispatch_pc) && "
              "native_car_select.on_dispatch(state, memory)"]),
    "instance_batch_capture.observe": cover(
        "records every active lifecycle dispatch; capture is statically ineligible",
        eligibility=["!instance_batch_capture.enabled()"]),
    "kernel.finish_guest_thread": cover(
        "acts only at the thread return sentinel, PC 0", registered=["0U"]),
    "kernel.dispatch_pending_alarm_callback": cover(
        "time-driven", horizon=["kernel.next_alarm_deadline()"]),
    "kernel.dispatch_guest_threads": cover(
        "time-driven only through the equal-priority slice",
        horizon=["kernel.next_thread_time_slice_deadline()"]),
    "kernel.dispatch_pending_sif_command_callback": cover(
        "poll-counted delay", predicate=["kernel.sif_command_callback_poll_can_progress()"]),
    "kernel.dispatch_pending_dmac_callback": cover(
        "poll-counted completion delay and COP0 Status gated dispatch",
        predicate=["memory.queued_dmac_completion_count()",
                   "kernel.pending_dmac_callback_can_dispatch(state)"],
        burst=["state.read_cop0(kCop0Status) != burst_cop0_status"]),
    "kernel.dispatch_pending_intc_callback": cover(
        "COP0 Status gated dispatch of pending unmasked causes",
        predicate=["kernel.pending_intc_callback_can_dispatch(state, memory)"],
        burst=["state.read_cop0(kCop0Status) != burst_cop0_status"]),
    "kernel.pending_dmac_callback_can_dispatch": cover(
        "the start predicate's own pure query"),
    "kernel.pending_intc_callback_can_dispatch": cover(
        "the start predicate's own pure query"),
    "dirty_dnas_compat.try_handle": cover(
        "returns NotHandled without side effects away from its three trampolines",
        registered=["DirtyDnasCompatHle::kStartTrampoline",
                    "DirtyDnasCompatHle::kUpdateTrampoline",
                    "DirtyDnasCompatHle::kStopTrampoline"]),
    "vif_packet_ring_flush_claim_tracker.observe_pre_instruction": cover(
        "guarded by requires_pre_instruction", registered=[FLUSH_ENTRY],
        predicate=[FLUSH_ACTIVE]),
    "vif_packet_ring_flush_claim_tracker.observe_post_native_execution": cover(
        "after a handled native dispatch", hook_text=[NATIVE_REGISTRY]),
    "vif_packet_ring_flush_claim_tracker.observe_post_instruction": cover(
        "only while a flush is tracked", registered=[FLUSH_ENTRY], predicate=[FLUSH_ACTIVE]),
    "ui_draw_session_feed.observe_pre_instruction": cover(
        "guarded by requires_pre_instruction", registered=[UI_ENTRY], burst=[UI_PER_STEP]),
    "host_femain_live_transaction.step": cover(
        "inside the VBlank block", horizon=["next_vblank_start"]),
    "object_packet_capture.observe": cover(
        "two fixed PCs", registered=["kObjectPacketBegin", "kObjectPacketEnd"],
        eligibility=["!object_packet_capture.enabled()"]),
    "model_submission_probe.observe": cover(
        "fixed and learned PCs", eligibility=["!model_submission_probe.enabled()"]),
    "takedown::runtime::pending_dmac_callback_is_inert": cover(
        "evaluated only at a join-pump candidate PC", registered=[JOIN_ENTRY, JOIN_END]),
    "join_pump_region->summarize_idle_iterations": cover(
        "join-pump admission", registered=[JOIN_ENTRY, JOIN_END]),
    "join_pump_region->execute_native_wait": cover(
        "join-pump admission", registered=[JOIN_ENTRY, JOIN_END]),
    "join_pump_region->execute": cover(
        "join-pump admission", registered=[JOIN_ENTRY, JOIN_END]),
    "native_function_dispatcher.prepare": cover(
        "native candidate only", hook_text=[NATIVE_REGISTRY]),
    "native_function_dispatcher.dispatch": cover(
        "native candidate only", hook_text=[NATIVE_REGISTRY]),
    "takedown::runtime::can_elide_inert_dmac_poll": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "compiled_candidate->traits.load_fusion_guard": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "compiled_block_dispatcher.dispatch_one": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "compiled_block_dispatcher.dispatch": cover(
        "compiled-block stage only", eligibility=[NO_COMPILED]),
    "collision_trace->begin_sample": cover(
        "observer scalar branch", eligibility=["collision_trace == nullptr"]),
    "collision_trace->finish_sample": cover(
        "observer scalar branch", eligibility=["collision_trace == nullptr"]),
    "vehicle_world_contact_probe->begin_sample": cover(
        "observer scalar branch", eligibility=["vehicle_world_contact_probe == nullptr"]),
    "vehicle_world_contact_probe->finish_sample": cover(
        "observer scalar branch", eligibility=["vehicle_world_contact_probe == nullptr"]),
    "spark_trace->begin_sample": cover(
        "observer scalar branch", eligibility=["spark_trace == nullptr"]),
    "spark_trace->finish_sample": cover(
        "observer scalar branch", eligibility=["spark_trace == nullptr"]),
    "retail_timer_owner_abi_trace.observe_pre_instruction": cover(
        "observer scalar branch", eligibility=["!retail_timer_owner_abi_trace.enabled()"]),
    "retail_timer_owner_abi_trace.observe_post_instruction": cover(
        "observer scalar branch", eligibility=["!retail_timer_owner_abi_trace.enabled()"]),
    "retail_timer_consumer_trace.observe_pre_instruction": cover(
        "observer scalar branch", eligibility=["!retail_timer_consumer_trace.enabled()"]),
    "retail_timer_consumer_trace.observe_post_instruction": cover(
        "observer scalar branch", eligibility=["!retail_timer_consumer_trace.enabled()"]),
    "retail_timer_consumer_trace.observe_hle_boundary": cover(
        "system-call tail of the consumer trace",
        eligibility=["!retail_timer_consumer_trace.enabled()"]),
    "femain_retail_consume_observation_adapter->observe_pre_instruction": cover(
        "observer scalar branch",
        eligibility=["femain_retail_consume_observation_adapter == nullptr"]),
    "femain_retail_consume_observation_adapter->observe_post_instruction": cover(
        "observer scalar branch",
        eligibility=["femain_retail_consume_observation_adapter == nullptr"]),
    "femain_frame_closure_diagnostic->observe": cover(
        "observer scalar branch", eligibility=["femain_frame_closure_diagnostic == nullptr"]),
    "femain_dynamic_callback_probe->observe_pre_instruction": cover(
        "observer scalar branch", eligibility=["!femain_dynamic_callback_probe_enabled"]),
    "femain_dynamic_callback_probe->observe_post_instruction": cover(
        "observer scalar branch", eligibility=["!femain_dynamic_callback_probe_enabled"]),
    "femain_dynamic_callback_probe->observe_hle_dmac_callback_pre_instruction": cover(
        "observer scalar branch", eligibility=["!femain_dynamic_callback_probe_enabled"]),
    "femain_dynamic_callback_probe->observe_hle_dmac_callback_post_instruction": cover(
        "observer scalar branch", eligibility=["!femain_dynamic_callback_probe_enabled"]),
}

# PC comparisons that registration alone does not justify. Keys are
# (operator, operand); a comparison of the executed instruction's PC uses
# ("executed <operator>", operand). `!=` and one-sided bounds always land here.
# Each review covers a counted number of occurrences, so one more use of the
# same comparison somewhere else is not silently accepted.
REVIEWED_COMPARISONS = {
    ("==", "Slus21050CameraFov::kUiSubmissionBegin"): counted(
        1, "member of the always-marked FOV registry; its source is checked below",
        hook_text=[CAMERA_FOV_HOOK_LOOP]),
    ("==", "Slus21050CameraFov::kFovUse"): counted(
        1, "member of the always-marked FOV registry; its source is checked below",
        hook_text=[CAMERA_FOV_HOOK_LOOP]),
    ("==", "kRetailTimerUpdate"): counted(
        1, "constant declared inside the loop; registered by value",
        registered=["0x001d3a60U"], eligibility=["!retail_timer_owner_observer_enabled"]),
    ("==", "kRetailTimerReturn"): counted(
        1, "constant declared inside the loop; registered by value",
        registered=["0x001d3c04U"], eligibility=["!retail_timer_owner_observer_enabled"]),
    ("!=", FLUSH_ENTRY): counted(
        1, "defensive pre-scheduler seam: true at every PC but the flush entry, and gated "
        "by an active flush, which the start predicate refuses",
        registered=[FLUSH_ENTRY], predicate=[FLUSH_ACTIVE],
        loop=["if (vif_packet_ring_flush_claim_tracker.active() && state.pc() !="]),
    ("!=", "milestone.address"): counted(
        1, "skip filter of the retail-trace milestone scan; the scan itself is ineligible",
        eligibility=["!retail_trace"], hook_text=["hooks.mark_pc(milestone.address)"],
        loop=["if (state.pc() != milestone.address) { continue; }"]),
    ("!=", JOIN_ENTRY): counted(
        2, "continue-loop callbacks of the join-pump checkpoint; they run only inside an "
        "admitted region, every PC of which is marked",
        registered=[JOIN_ENTRY, JOIN_END],
        loop=["checkpoint.continue_loop = [](void*, const auto& cpu, const auto& mem) {"]),
    ("<", "0x02000000U"): counted(
        1, "bound of the per-PC hit-watch table, not a hook address",
        eligibility=["ee_pc_hit_watch.empty()"], hook_text=["hooks.mark_pc(watch.address)"],
        loop=["if (!ee_pc_hit_watch.empty() && dispatch_pc < 0x02000000U &&"]),
}

# Lower/upper bound pairs whose bounds are not literally one registered range.
# Keys are (lower operand, upper operand, "closed" | "half_open").
REVIEWED_RANGES = {
    (JOIN_ENTRY, "join_pump_candidate_end", "half_open"): cover(
        "the candidate end is one of three configured ends; the hook block registers the "
        "largest and proves the order with a static_assert",
        registered=[JOIN_ENTRY, JOIN_END],
        hook_text=["static_assert(JoinPumpRegion::kCallbackTailEffectEndAddress >= "
                   "JoinPumpRegion::kExtendedCallerEffectEndAddress && "
                   "JoinPumpRegion::kExtendedCallerEffectEndAddress >= "
                   "JoinPumpRegion::kDirectEffectEndAddress);"],
        loop=["const auto join_pump_candidate_end = join_pump_callback_tail_region_active ? "
              "takedown::recompiler::JoinPumpRegion::kCallbackTailEffectEndAddress : "
              "join_pump_callback_region_requested ? "
              "takedown::recompiler::JoinPumpRegion::kExtendedCallerEffectEndAddress : "
              "takedown::recompiler::JoinPumpRegion::kDirectEffectEndAddress;"]),
}

# Arithmetic on the PC, keyed by operator plus the other operand.
REVIEWED_ARITHMETIC = {
    ">>2U": cover("index into the hit-watch slot table",
                  eligibility=["ee_pc_hit_watch.empty()"]),
    "&0x3U": cover("alignment test guarding the hit-watch table",
                   eligibility=["ee_pc_hit_watch.empty()"]),
    "+4U": cover("sequential-successor arithmetic (delay-slot shape, call target); "
                 "never compared with a hook address"),
    "+static_cast<std::uint32_t>(offset*4U)": cover(
        "successor PC in the compiled bundle scan; the result is tracked as an alias",
        eligibility=[NO_COMPILED]),
    "<<32U": cover("retail-trace call-site key", eligibility=["!retail_trace"]),
}

# Places a PC value is stored (it could be compared later, so each is reviewed).
REVIEWED_STORES = {
    ".handler": cover("retail event trace record", eligibility=["!retail_trace"]),
    ".call_site": cover("retail call-site trace record", eligibility=["!retail_trace"]),
    ".instruction": cover("retail value-transition trace record",
                          eligibility=["!retail_trace"]),
    ".pc": cover("compiled budget profiler sample", eligibility=[NO_COMPILED]),
    ".guest_pc": cover(
        "FEMain provenance record built by snapshot_femain_provenance_boundary, which only "
        "the measurement start calls; a pending start refuses the burst",
        predicate=["guest_work_measurement_start_input_pending"]),
    "native_gap_address": cover("native-only stop report", eligibility=["!native_only"]),
    "burst_pc": cover("the burst's own next-instruction PC"),
    "burst_declined_entry": cover(
        "the burst's own next-instruction PC, kept for a registered native entry whose "
        "replacement the outer iteration would not dispatch (see burst_prepare_native); the "
        "compiled run executes that entry as ordinary guest code",
        hook_text=[NATIVE_REGISTRY, "scalar_burst_native_entries.add(function.address)"],
        burst=["burst_declined_entry = burst_pc"]),
}

# Containers indexed by the PC.
REVIEWED_INDEXES = {
    "native_dma_fallback_sources": cover(
        "counted only for a blocked native candidate", hook_text=[NATIVE_REGISTRY]),
}

# CPU-state accessors the loop body may use. PC-like and pipeline state must be
# listed here with the reason it cannot make an ordinary iteration act.
REVIEWED_STATE_METHODS = {
    "pc": cover("every use is classified individually by this test"),
    "read_gpr_u32": cover("register operand of a PC-gated hook or of a trace; the trace "
                          "below fails any use an ordinary iteration can reach"),
    "read_gpr_u64": cover("register operand of a PC-gated diagnostic"),
    "read_cop0": cover("the burst's own COP0 Status watch"),
    "next_pc": cover("native preflight and compiled budget only",
                     hook_text=[NATIVE_REGISTRY], eligibility=[NO_COMPILED]),
    "is_executing_delay_slot": cover(
        "native preflight and compiled budget only",
        hook_text=[NATIVE_REGISTRY], eligibility=[NO_COMPILED]),
}

# Methods the loop body calls on the objects whose state it polls. A method
# that is not listed here is a new poll or service and must be reviewed.
OBJECT_METHODS = {
    "kernel": cover(
        "dispatchers and finish_* are classified in STATE_HELPERS; the rest are pure queries, "
        "the system-call tail, the clock publish repeated per burst step, the SIF DMA "
        "completion service bounded by the horizon, and RWA shadow calls (ineligible)",
        horizon=["kernel.next_sif_dma_completion_deadline()"],
        burst=["kernel.set_ee_cycle_count(emulated_ee_cycles);"],
        predicate=["kernel.rwa_activity().opcode_counts[0x05U]"],
        eligibility=["!rwa_iop_shadow.has_value()"]),
    "memory": cover(
        "guest memory access (un-gated uses fail the burst-boundary trace), pure device "
        "queries, run-ahead (never due while graphics is idle), the asynchronous graphics "
        "window's fences and VBlank stream marker (an open window reports graphics idle, "
        "and a fence bumps hardware_activity_epoch), the VBlank INTC raise "
        "(horizon) and the burst's own per-step calls",
        predicate=["memory.graphics_dmacs_idle()"], horizon=["next_vblank_start"]),
    "spu2_renderer": cover(
        "the pump is bounded by the next sample cycle; the rest are counters",
        horizon=["spu2_renderer.next_sample_cycle()"]),
    "guest_input_replay": cover(
        "boundary application and step clipping are bounded by the predicate and horizon; "
        "the rest run inside the marked transport-poll hook or are pure queries",
        predicate=["guest_input_replay->instruction_boundary_action_pending()"],
        horizon=["execution_step_limit", "guest_input_replay->next_boundary_cycle_limit()"],
        registered=["kRetailControllerTransportPollReturn"]),
}
OBJECT_METHOD_NAMES = {
    "kernel": {
        "apply_deferred_rwa_shadow_response", "current_thread_id", "dispatch_guest_threads",
        "dispatch_pending_alarm_callback", "dispatch_pending_dmac_callback",
        "dispatch_pending_intc_callback", "dispatch_pending_sif_command_callback",
        "finish_guest_callback", "finish_guest_thread", "gs_crt_state",
        "guest_callback_activation_generation", "guest_callback_active",
        "guest_callback_identity", "guest_callback_kind", "handle_system_call",
        "has_competing_ready_thread", "next_alarm_deadline", "next_sif_dma_completion_deadline",
        "pending_dmac_callback_count", "pending_dmac_callback_mask",
        "pending_sif_command_callback_count", "rwa_activity", "service_sif_dma_completions",
        "set_ee_cycle_count", "set_rwa_execution_observers",
        "sif_command_callback_poll_can_progress", "sif_dma_completion_pending",
        "sif_dma_transfers",
    },
    "memory": {
        "advance_dmac_completion_delays", "advance_idle_gif_dmac", "advance_idle_vif1_dmac", "contains", "gif_dmac_can_progress",
        "gif_dmac_in_flight", "graphics_async_fence", "graphics_idle_diagnostic_state",
        "graphics_service_deadline", "gs_capture", "gs_privileged_display", "hardware_activity_epoch", "intc_status",
        "ipu_activity", "next_internal_graphics_deadline_for_join_pump", "pending_intc_causes",
        "observe_vif1_pure_unpack_boundary",
        "queued_dmac_completion_count", "raise_intc_cause", "read_u16_le", "read_u32_le",
        "read_u8", "run_frame_in_graphics_order", "run_graphics_ahead", "run_in_graphics_order",
        "set_current_ee_pc",
        "set_current_guest_step",
        "vif1_dmac_can_progress", "vif1_dmac_in_flight", "vu1_activity", "vu1_program_cache",
        "write_u32_le", "write_u8",
    },
    "spu2_renderer": {"fnv1a64", "non_silent_frames", "pump_due", "samples_rendered"},
    "guest_input_replay": {
        "apply_instruction_boundary", "clip_step_limit", "completion_boundary", "format",
        "controller_state", "observe_retail_poll", "observed_poll_count", "poll_window",
        "retail_poll_controller_state",
    },
}


# --------------------------------------------------------------------------
# Burst-boundary trace tables
# --------------------------------------------------------------------------
def fact(value, why, **requirements):
    return (value, cover(why, **requirements))


def runs(why, count=1, **requirements):
    return counted(count, why, **requirements)


def host_only(pinned_digest, why, **requirements):
    return {"digest": pinned_digest, "cover": cover(why, **requirements)}


# Conditions with a known value at every boundary a burst skips. Keys are the
# normalized text of a whole condition, one `||` alternative, one `&&`
# conjunct, or the operand of a `!`. A fact marked false or true certifies
# that evaluating the expression there has no side effect. Facts that rest on
# a registered PC describe the instruction about to run, so the trace does not
# use them after the step.
FACTS = {
    # ---- configurations the eligibility predicate excludes ----
    "femain_live_transaction_enabled": fact(
        FALSE, "FEMain live source drain", eligibility=["!femain_live_transaction_enabled"]),
    "subframe_input_bridge.has_value()": fact(
        FALSE, "sub-frame controller injection",
        eligibility=["!subframe_input_bridge.has_value()"]),
    "rwa_iop_shadow.has_value()": fact(
        FALSE, "RWA IOP shadow boot detach", eligibility=["!rwa_iop_shadow.has_value()"]),
    "scripted_input_presses.empty()": fact(
        TRUE, "scripted controller injection", eligibility=["scripted_input_presses.empty()"]),
    "femain_park_pcs.has_value()": fact(
        FALSE, "park-PC histogram", eligibility=["!femain_park_pcs.has_value()"]),
    "retail_trace": fact(FALSE, "retail trace probes and tail", eligibility=["!retail_trace"]),
    "retail_timer_owner_abi_trace.active()": fact(
        FALSE, "set only by the pre-instruction observer of the owner trace, which needs the "
        "trace enabled", eligibility=["!retail_timer_owner_abi_trace.enabled()"]),
    "retail_timer_consumer_trace.collecting()": fact(
        FALSE, "collecting() is enabled && ...",
        eligibility=["!retail_timer_consumer_trace.enabled()"]),
    "object_packet_capture.enabled()": fact(
        FALSE, "object packet capture", eligibility=["!object_packet_capture.enabled()"]),
    "instance_batch_capture.enabled()": fact(
        FALSE, "whole instance lifecycle capture requires every dispatch",
        eligibility=["!instance_batch_capture.enabled()"]),
    "model_submission_probe.enabled()": fact(
        FALSE, "model submission probe", eligibility=["!model_submission_probe.enabled()"]),
    "femain_retail_consume_invocation_barrier!=nullptr": fact(
        FALSE, "FEMain invocation barrier",
        eligibility=["femain_retail_consume_invocation_barrier == nullptr"]),
    "ee_pc_hit_watch.empty()": fact(
        TRUE, "per-PC hit counters", eligibility=["ee_pc_hit_watch.empty()"]),
    "retail_timer_owner_observer_enabled": fact(
        FALSE, "retail timer owner observer",
        eligibility=["!retail_timer_owner_observer_enabled"]),
    "collision_trace!=nullptr": fact(
        FALSE, "focused collision trace", eligibility=["collision_trace == nullptr"]),
    "vehicle_world_contact_probe!=nullptr": fact(
        FALSE, "focused contact probe", eligibility=["vehicle_world_contact_probe == nullptr"]),
    "spark_trace!=nullptr": fact(
        FALSE, "focused spark trace", eligibility=["spark_trace == nullptr"]),
    "femain_dynamic_callback_probe_enabled": fact(
        FALSE, "FEMain dynamic callback probe",
        eligibility=["!femain_dynamic_callback_probe_enabled"]),
    "ee_host_phase_profiler!=nullptr": fact(
        FALSE, "host phase profiler scopes", eligibility=[NO_HOST_PHASE_PROFILER]),
    "ee_pc_sampler==nullptr": fact(
        TRUE, "per-commit EE PC sample", eligibility=["ee_pc_sampler == nullptr"]),
    "native_only": fact(FALSE, "native-only coverage gap", eligibility=["!native_only"]),
    "compiled_candidate!=nullptr": fact(
        FALSE, "no compiled block is registered, so the preparation cache returns none",
        eligibility=[NO_COMPILED], assignments=["compiled_candidate=2"]),
    "compiled_dispatch.covered()": fact(
        FALSE, "a loop local written only by the two dispatch calls of the compiled stage",
        eligibility=[NO_COMPILED], assignments=["compiled_dispatch=2"]),
    "compiled_dispatch.status==CompiledBlockDispatchStatus::CodeMismatch": fact(
        FALSE, "a loop local written only by the two dispatch calls of the compiled stage",
        eligibility=[NO_COMPILED], assignments=["compiled_dispatch=2"]),
    "compiled_execution_boundary.has_value()": fact(
        FALSE, "a loop local written only after a covered compiled dispatch",
        eligibility=[NO_COMPILED], assignments=["compiled_execution_boundary=2"]),
    "scalar_cold_observers_enabled": fact(
        TRUE, "the bypass arm hosts the burst", eligibility=["scalar_cold_observers_enabled"]),
    "scalar_observers_configured": fact(
        FALSE, "the observer arm is ineligible", eligibility=["!scalar_observers_configured"]),

    # ---- latches and devices the start predicate proves idle ----
    "native_simulation_shadow_stop_requested": fact(
        FALSE, "set only inside the retail timer owner observer and the observer scalar "
        "branch, both ineligible; the start predicate tests it as well",
        predicate=["native_simulation_shadow_stop_requested"],
        eligibility=["!retail_timer_owner_observer_enabled", "!scalar_observers_configured"],
        assignments=["native_simulation_shadow_stop_requested=5"]),
    "replay_advance.measurement_start": fact(
        FALSE, "apply_instruction_boundary() reports nothing while no boundary action is "
        "pending and the next recorded step and cycle are not reached",
        predicate=["guest_input_replay->instruction_boundary_action_pending()"],
        horizon=["execution_step_limit", "guest_input_replay->next_boundary_cycle_limit()"]),
    "replay_advance.completion": fact(
        FALSE, "as replay_advance.measurement_start",
        predicate=["guest_input_replay->instruction_boundary_action_pending()"],
        horizon=["execution_step_limit", "guest_input_replay->next_boundary_cycle_limit()"]),
    "guest_work_measurement_start_input_pending": fact(
        FALSE, "measurement start latch",
        predicate=["guest_work_measurement_start_input_pending"]),
    FLUSH_ACTIVE: fact(
        FALSE, "a tracked flush forbids the bypass step and a burst", predicate=[FLUSH_ACTIVE]),
    "host_frame_step_paused": fact(
        FALSE, "host pause latch, set only in the VBlank block or the host pump",
        predicate=["host_frame_step_paused"]),
    "host_window_capture_boundary_paused": fact(
        FALSE, "host pause latch, set only in the VBlank block or the host pump",
        predicate=["host_window_capture_boundary_paused"]),
    "memory.vif1_dmac_can_progress()": fact(
        FALSE, "idle graphics has no VIF1 work", predicate=["memory.graphics_dmacs_idle()"]),
    "memory.gif_dmac_can_progress()": fact(
        FALSE, "idle graphics has no GIF work", predicate=["memory.graphics_dmacs_idle()"]),
    "!rwa_first_op05_step_valid&&kernel.rwa_activity().opcode_counts[0x05U]!=0U": fact(
        FALSE, "first-op05 latch; the opcode count changes only in kernel HLE",
        predicate=["!rwa_first_op05_step_valid && "
                   "kernel.rwa_activity().opcode_counts[0x05U] != 0U"]),
    "kernel.dispatch_pending_sif_command_callback(state)": fact(
        FALSE, "returns before its poll-counted delay and dispatch when nothing can progress",
        predicate=["kernel.sif_command_callback_poll_can_progress()"]),
    "kernel.dispatch_pending_dmac_callback(state,memory)": fact(
        FALSE, "no completion is queued and no latched channel can dispatch",
        predicate=["memory.queued_dmac_completion_count()",
                   "kernel.pending_dmac_callback_can_dispatch(state)",
                   "kernel.guest_callback_active()"],
        burst=["state.read_cop0(kCop0Status) != burst_cop0_status"]),
    "kernel.dispatch_pending_intc_callback(state,memory)": fact(
        FALSE, "no pending unmasked cause can dispatch",
        predicate=["kernel.pending_intc_callback_can_dispatch(state, memory)",
                   "kernel.guest_callback_active()"],
        burst=["state.read_cop0(kCop0Status) != burst_cop0_status"]),

    # ---- deadlines the horizon keeps ahead ----
    "committed_steps>=next_host_window_step": fact(
        FALSE, "host window pump by committed step", horizon=["next_host_window_step"]),
    "host_window_loop_iterations>=kHostWindowLoopIterationInterval*host_window_pump_scale()": fact(
        FALSE, "host window pump by weighted turn count; the burst counts each instruction as one",
        horizon=["host_window_loop_iterations", "kHostWindowLoopIterationInterval"],
        burst=["++host_window_loop_iterations;"]),
    "emulated_ee_cycles>=next_vblank_start": fact(
        FALSE, "the VBlank block", horizon=["next_vblank_start"]),
    "reached_host_frame_step_boundary": fact(
        FALSE, "a loop local set only inside the VBlank block", horizon=["next_vblank_start"],
        loop=["bool reached_host_frame_step_boundary = false;"],
        assignments=["reached_host_frame_step_boundary=2"]),
    "reached_host_capture_boundary": fact(
        FALSE, "a loop local set only inside the VBlank block", horizon=["next_vblank_start"],
        loop=["bool reached_host_capture_boundary = false;"],
        assignments=["reached_host_capture_boundary=2"]),
    "variable_frame_busy_vblank": fact(
        FALSE, "set only inside the VBlank block and cleared by the block that follows it",
        horizon=["next_vblank_start"], assignments=["variable_frame_busy_vblank=2"]),
    "kernel.dispatch_pending_alarm_callback(state)": fact(
        FALSE, "returns before testing time while a callback is active; otherwise the "
        "earliest alarm deadline is not reached",
        horizon=["kernel.next_alarm_deadline()", "kernel.guest_callback_active()"]),
    "thread_dispatch==EeThreadDispatchResult::Switched": fact(
        FALSE, "next_thread_time_slice_deadline() is 0 whenever the dispatcher would switch "
        "regardless of time, and the slice deadline otherwise",
        horizon=["kernel.next_thread_time_slice_deadline()"]),
    "thread_dispatch==EeThreadDispatchResult::NoRunnable": fact(
        FALSE, "next_thread_time_slice_deadline() is 0 when nothing is runnable",
        horizon=["kernel.next_thread_time_slice_deadline()"]),

    # ---- the two guest RAM polls the burst mirrors ----
    "staging_pointer!=rwa_staging_last_pointer": fact(
        FALSE, "staging-global tracker: the same word is compared after every burst step",
        burst=["burst_ram_watch.fires()"],
        watch=["rwa_staging_samples.size() < 8U && memory.contains(0x4e2ce8U, 8U)",
               "ram + 0x004e2ce8U", "pointer != staging_last_pointer",
               "watch.staging_last_pointer = rwa_staging_last_pointer"],
        loop=["if (rwa_staging_samples.size() < 8U && memory.contains(0x4e2ce8U, 8U)) { "
              "const auto staging_pointer = memory.read_u32_le(0x4e2ce8U); "
              "if (staging_pointer != rwa_staging_last_pointer) {"]),
    "virtual_ds2_trace.has_value()&&!virtual_ds2_trace->action_edge_step.has_value()&&"
    "memory.read_u16_le(kRetailControllerAcceptEdge)!=0U": fact(
        FALSE, "accept-edge sampler, rising half: mirrored after every burst step",
        burst=["burst_ram_watch.fires()"],
        watch=["!virtual_ds2_trace->action_edge_step.has_value()", "watch.edge_mode = 1U",
               "ram + kRetailControllerAcceptEdge"]),
    "virtual_ds2_trace.has_value()&&virtual_ds2_trace->action_edge_step.has_value()&&"
    "!virtual_ds2_trace->action_edge_clear_step.has_value()&&"
    "committed_steps>*virtual_ds2_trace->action_edge_step&&"
    "memory.read_u16_le(kRetailControllerAcceptEdge)==0U": fact(
        FALSE, "accept-edge sampler, falling half: mirrored after every burst step",
        burst=["burst_ram_watch.fires()"],
        watch=["!virtual_ds2_trace->action_edge_clear_step.has_value()",
               "first_skipped_step > *virtual_ds2_trace->action_edge_step",
               "watch.edge_mode = 2U"]),

    # ---- hooks that act only at a registered PC ----
    "dispatch_pc==Slus21050CameraFov::kUiSubmissionBegin": fact(
        FALSE, "always-marked camera registry member (verified against its source below)",
        hook_text=[CAMERA_FOV_HOOK_LOOP]),
    "dispatch_pc==Slus21050CameraFov::kFovUse": fact(
        FALSE, "always-marked camera registry member (verified against its source below)",
        hook_text=[CAMERA_FOV_HOOK_LOOP]),
    "kernel.finish_guest_callback(state)": fact(
        FALSE, "acts only at the callback return sentinel", registered=[CALLBACK_RETURN]),
    "kernel.finish_guest_thread(state)": fact(
        FALSE, "acts only at the thread return sentinel, PC 0", registered=["0U"]),
    "memory.contains(state.pc(),sizeof(std::uint32_t))": fact(
        TRUE, "the filter only leaves aligned KUSEG main-RAM PCs unmarked, and those are mapped",
        hook_text=["ScalarBurstHookFilter hooks{memory.size()}"]),
    "ui_draw_session_feed.requires_pre_instruction(ui_draw_execution_context,dispatch_pc)": fact(
        FALSE, "the fixed entry is marked; with a session active the burst asks the same "
        "question, with the same context, before every step",
        registered=[UI_ENTRY],
        burst=["burst_ui_session ? current_ui_execution_context()",
               "ui_draw_session_feed.requires_pre_instruction( burst_ui_context, burst_pc)"]),
    "dirty_dnas_compat.try_handle(state,memory)!=DirtyDnasCompatCall::NotHandled": fact(
        FALSE, "returns NotHandled without side effects away from its three trampolines",
        registered=["DirtyDnasCompatHle::kStartTrampoline",
                    "DirtyDnasCompatHle::kUpdateTrampoline",
                    "DirtyDnasCompatHle::kStopTrampoline"]),
    "scalar_burst_hooks.marked(dispatch_pc)": fact(
        FALSE, "the variable frame-rate hooks are consulted only at a marked PC, and a burst "
        "never executes an instruction there", hook_text=[FRAME_RATE_REGISTRY],
        burst=["scalar_burst_hooks.marked(burst_pc)"]),
    "dispatch_preparation.native_candidate!=nullptr": fact(
        FALSE, "the preparation cache yields a native candidate only at a registered entry",
        hook_text=[NATIVE_REGISTRY]),
    "native_candidate!=nullptr": fact(
        FALSE, "a loop local assigned only from the native candidate of the preparation cache",
        hook_text=[NATIVE_REGISTRY], assignments=["native_candidate=2"],
        loop=["native_candidate = dispatch_preparation.native_candidate;"]),
    "native_dispatch.handled()": fact(
        FALSE, "a loop local written only by the native dispatch, i.e. at a registered entry",
        hook_text=[NATIVE_REGISTRY], assignments=["native_dispatch=1"],
        loop=["native_dispatch = native_function_dispatcher.dispatch("]),

    # ---- the instruction a skipped boundary commits ----
    "result.status==EeStepStatus::Executed": fact(
        TRUE, "a burst step whose result is anything else ends the burst before its tail, "
        "and that tail then runs for real", burst=[RESULT_STOP]),
}

# Statements an ordinary iteration executes at a skipped boundary, with the
# reason the burst is exact without running them as written. A key ending in
# "..." matches by prefix. `count` is how often the trace reaches the text.
RUNS = {
    "native_model_live->throw_if_failed();": runs(
        "the pure start predicate proves the owner's terminal latch empty; this call is "
        "then a no-op, not an assumption that the owner is absent. Parent preparation "
        "stays outer-only; marker cancellation during a store increments the hardware "
        "epoch and ends the burst. An ordinary first store is checked before the new "
        "epoch baseline, leaving its normal commit tail and next outer throw intact",
        predicate=[MODEL_FAILURE_PREDICATE, "memory.graphics_dmacs_idle()"],
        hook_text=[MODEL_PARENT_HOOK, NATIVE_HOOK_LOOP],
        burst=["memory.hardware_activity_epoch() != burst_hardware_activity"],
        loop=["if (native_model_live) native_model_live->throw_if_failed();"]),
    # ---- replayed before every burst step ----
    "service_graphics_dmacs_until(emulated_ee_cycles,vif1_dmac_serviced_ee_cycles,"
    "gif_dmac_serviced_ee_cycles,advance_profiled_vif1_dmac,advance_profiled_gif_dmac);": runs(
        "graphics service to the current cycle; idle graphics makes it a device-clock tick",
        predicate=["memory.graphics_dmacs_idle()"],
        burst=["takedown::runtime::service_graphics_dmacs_until( emulated_ee_cycles, "
               "vif1_dmac_serviced_ee_cycles, gif_dmac_serviced_ee_cycles, advance_idle_vif1, "
               "advance_idle_gif);"]),
    "memory.advance_vif1_dmac(cycles);": runs(
        "body of advance_profiled_vif1_dmac; with idle graphics it equals the idle advance "
        "the burst calls (takedown_scalar_burst_tests compares the two)",
        predicate=["memory.graphics_dmacs_idle()"],
        burst=["memory.advance_idle_vif1_dmac(cycles);"]),
    "memory.advance_gif_dmac(cycles);": runs(
        "body of advance_profiled_gif_dmac; as the VIF1 advance",
        predicate=["memory.graphics_dmacs_idle()"],
        burst=["memory.advance_idle_gif_dmac(cycles);"]),
    "host_window_loop_iterations+=host_window_pump_scale();": runs(
        "host-window turn counter: the pump is host-only work, so the burst counts each skipped "
        "instruction as one unit and an outer turn weighs host_window_pump_scale()",
        burst=["if (host_window != nullptr) { ++host_window_loop_iterations; }"]),
    "kernel.set_ee_cycle_count(emulated_ee_cycles);": runs(
        "kernel clock publish", burst=["kernel.set_ee_cycle_count(emulated_ee_cycles);"]),
    "memory.set_current_ee_pc(dispatch_pc);": runs(
        "memory-side provenance of the instruction about to run",
        burst=["memory.set_current_ee_pc(burst_pc);"]),
    "memory.set_current_guest_step(committed_steps);": runs(
        "memory-side provenance of the instruction about to run",
        burst=["memory.set_current_guest_step(committed_steps);"]),
    "memory.set_current_ee_pc(state.pc());": runs(
        "the bypass arm repeats the provenance publish with the same PC",
        burst=["memory.set_current_ee_pc(burst_pc);"]),
    STEP_LEAF: runs(
        "the step itself",
        burst=["result = takedown::guest::step_r5900(state, memory, instruction_cache);"]),
    "++scalar_observer_bypass_steps;": runs(
        "bypass step counter", burst=["++scalar_observer_bypass_steps;"]),
    "++committed_steps;": runs("commit tail", burst=["++committed_steps;"]),
    "++executed_instructions;": runs("commit tail", burst=["++executed_instructions;"]),
    "++emulated_ee_cycles;": runs("commit tail", burst=["++emulated_ee_cycles;"]),
    "continue;": runs("end of the commit tail: the next boundary begins"),

    # ---- no-ops before their deadline or while idle ----
    "const auto replay_advance=guest_input_replay->apply_instruction_boundary("
    "current_guest_input_timeline_boundary());": runs(
        "does nothing until a pending action, the next recorded step or its cycle is reached",
        predicate=["guest_input_replay->instruction_boundary_action_pending()"],
        horizon=["execution_step_limit", "guest_input_replay->next_boundary_cycle_limit()"]),
    "spu2_renderer.pump_due(emulated_ee_cycles);": runs(
        "a no-op exactly while the cycle is before the start of the next sample",
        horizon=["spu2_renderer.next_sample_cycle()"]),
    "kernel.service_sif_dma_completions(memory);": runs(
        "a no-op until the kernel clock reaches the deadline of the front batch",
        horizon=["kernel.next_sif_dma_completion_deadline()"]),
    "const auto thread_dispatch=kernel.dispatch_guest_threads(state);": runs(
        "continues the running thread until the thread deadline of the horizon; returns at "
        "once while a callback is active",
        horizon=["kernel.next_thread_time_slice_deadline()", "kernel.guest_callback_active()"]),
    "const std::size_t execution_step_limit=guest_input_replay!=nullptr?"
    "guest_input_replay->clip_step_limit(committed_steps,maximum_steps):maximum_steps;": runs(
        "pure query; the burst reuses the value of the ordinary iteration, which cannot "
        "change before the limit itself is reached",
        burst=["scalar_burst_horizon(execution_step_limit)"], horizon=["execution_step_limit"]),

    # ---- idempotent or host-only ----
    "idle_vblank_probe_active=false;": runs(
        "idempotent: the ordinary iteration before the burst already cleared it under the "
        "same kernel state, and only the no-runnable path (horizon) sets it",
        horizon=["kernel.next_thread_time_slice_deadline()"],
        loop=["if (!kernel.guest_callback_active() && kernel.current_thread_id() != 0) { "
              "idle_vblank_probe_active = false; }"],
        assignments=["idle_vblank_probe_active=5"]),
    "scheduler_phase.finish();": runs(
        "empty profiler scope", eligibility=[NO_HOST_PHASE_PROFILER]),
    "native_stage_phase.finish();": runs(
        "empty profiler scope", eligibility=[NO_HOST_PHASE_PROFILER]),
    "compiled_stage_phase.finish();": runs(
        "empty profiler scope", eligibility=[NO_HOST_PHASE_PROFILER]),
    "native_gap_preflight.reset();": runs(
        "idempotent; set only for a native candidate and read only by the native-only stop "
        "report", eligibility=["!native_only"], hook_text=[NATIVE_REGISTRY]),
    "ui_draw_execution_context=current_ui_execution_context();": runs(
        "loop-local context for the UI feed query; the burst builds the same context once, "
        "and it can change only in kernel HLE or the outer loop",
        burst=["burst_ui_session ? current_ui_execution_context()"]),
    "const auto dispatch_preparation=ee_dispatch_preparation_cache.prepare(state.pc());": runs(
        "const lookup memoised in a direct-mapped slot; the result is a pure function of the "
        "frozen registries and names a candidate only at a registered entry",
        hook_text=[NATIVE_REGISTRY, COMPILED_REGISTRY]),
    "compiled_preparation=dispatch_preparation.compiled_preparation;": runs(
        "loop-local copy of the lookup result"),
    "compiled_candidate=femain_retail_consume_acceleration_blocked?nullptr:"
    "compiled_preparation.block();": runs(
        "loop-local; null without registered compiled blocks", eligibility=[NO_COMPILED]),
    "static const bool relaxed_pending_poll=...": runs(
        "function-local static read from the environment once"),
    "static const bool relaxed_delay_slot_dispatch=...": runs(
        "function-local static read from the environment once"),
}

# Lambdas defined before the loop that the trace reaches but does not walk:
# host-only work the burst deliberately skips. The body is pinned by digest
# and may not name guest-visible state.
HOST_ONLY_LAMBDAS = {
    "pace_audio_master": host_only(
        "800c185ff0b7c7cc",
        "audio master-clock throttle: sleeps on the host clock and counts blocks. It reads "
        "the sample counter of the renderer and writes only audio_master_* host statistics. "
        "A burst never renders a sample (horizon), the submit observer of the renderer calls "
        "it at every submission anyway, and the outer call runs again at the boundary after "
        "the burst. Not replayed: the audio-master block count and blocked nanoseconds are "
        "wall-clock statistics",
        horizon=["spu2_renderer.next_sample_cycle()"]),
}
HOST_ONLY_FORBIDDEN = ("state", "memory", "kernel", "emulated_ee_cycles", "committed_steps",
                       "executed_instructions", "next_vblank_start")

# Side-effect free member functions a condition or a declaration initializer
# may call, by name. A bare function call is never accepted on its name.
PURE_METHODS = {
    # std containers and optionals
    "has_value", "empty", "size", "back",
    # session state of the UI feed and the flush tracker (const queries)
    "active",
    # guest RAM reads at fixed main-RAM addresses, and the PC itself
    "contains", "read_u32_le", "pc",
    # const kernel queries
    "guest_callback_active", "guest_callback_identity", "current_thread_id", "rwa_activity",
    # the host-window pump cadence reads whether the memory card is still to be plugged in
    "save_card_present",
}
PURE_CASTS = ("static_cast<", "std::bit_cast<", "reinterpret_cast<", "const_cast<")

# Types an ordinary iteration may default- or brace-construct as a local: plain
# data whose constructor and destructor do nothing observable.
PLAIN_LOCAL_TYPES = {
    "std::int32_t", "std::optional", "SubmitUiDrawBucketsExecutionContext",
    "NativeFunctionDispatchResult", "NativeFunctionPreparation", "CompiledBlockPreparation",
    "CompiledBlockDispatchResult", "EeStepResult",
}

# Reviewed statement-by-statement text of the burst machinery (see check_shapes).
EXPECTED_SHAPES = {
    "burst": r'''
if (scalar_burst_enabled&&result.status==EeStepStatus::Executed)
    auto burst_pc=state.pc();
    if (!scalar_burst_hooks.marked(burst_pc)&&scalar_burst_may_start())
        auto burst_budget=scalar_burst_horizon(execution_step_limit);
        const bool burst_ui_session=ui_draw_session_feed.active();
        const auto burst_ui_context=burst_ui_session?current_ui_execution_context():SubmitUiDrawBucketsExecutionContext{};
        const auto burst_ram_watch=scalar_burst_ram_watch(committed_steps+1U);
        if (burst_budget!=0U&&!burst_ram_watch.fires()&&!(burst_ui_session&&ui_draw_session_feed.requires_pre_instruction(burst_ui_context,burst_pc)))
            constexpr std::size_t kCop0Status=12U;
            const auto burst_hardware_activity=memory.hardware_activity_epoch();
            auto burst_cop0_status=state.read_cop0(kCop0Status);
            const auto advance_idle_vif1=[&memory](const std::uint64_t cycles){memory.advance_idle_vif1_dmac(cycles);};
            const auto advance_idle_gif=[&memory](const std::uint64_t cycles){memory.advance_idle_gif_dmac(cycles);};
            ++scalar_burst.bursts;
            scalar_burst.note_start(burst_budget);
            auto burst_stop=ScalarBurstStop::Horizon;
            const bool burst_ee_aot=ee_aot_enabled&&(ee_aot_in_ui_session||!burst_ui_session)&&!burst_ram_watch.unmirrored;
            if (burst_ee_aot)
                ee_aot_update_session_stops(burst_ui_session,ui_draw_session_feed.learned_callback_target(),ui_draw_session_feed.learned_caller_return());
            const auto burst_prepare_native=[&](const GuestAddress pc,const std::size_t pending,bool&outer_loop_declines)->NativeFunctionPreparation{outer_loop_declines=false;const auto*const candidate=native_function_dispatcher.lookup(pc);if(candidate==nullptr||!candidate->traits.scheduler_state_transparent){return{};}if(interpreter_only||femain_retail_consume_acceleration_blocked||!ui_draw_session_feed.can_dispatch_atomic_native_function(SubmitUiDrawBucketsExecutionContext{},pc)){outer_loop_declines=true;return{};}const NativeFunctionPreflightState burst_preflight_state{.committed_instructions=committed_steps+pending,.instruction_budget=execution_step_limit,.ee_cycles=emulated_ee_cycles+pending,.next_vblank_cycle=next_vblank_start,.next_alarm_cycle=kernel.next_alarm_deadline(),.queued_dmac_completions=relaxed_pending_poll?0U:memory.queued_dmac_completion_count(),.gif_dma_in_flight=!relaxed_pending_poll&&memory.gif_dmac_in_flight(),.vif1_dma_in_flight=!relaxed_pending_poll&&memory.vif1_dmac_in_flight(),.pending_dmac_callbacks=relaxed_pending_poll?0U:kernel.pending_dmac_callback_count(),.pending_sif_callbacks=relaxed_pending_poll?0U:kernel.pending_sif_command_callback_count(),.sif_callback_poll_can_progress=relaxed_pending_poll?false:kernel.sif_command_callback_poll_can_progress(),.sif_dma_completion_pending=kernel.sif_dma_completion_pending(),.next_sif_dma_completion_cycle=kernel.next_sif_dma_completion_deadline(),.competing_ready_thread=kernel.has_competing_ready_thread(),.pending_intc_causes=relaxed_pending_poll?0U:memory.pending_intc_causes(),.executing_delay_slot=state.is_executing_delay_slot(),};const auto preflight=check_native_function_preflight(candidate->limits,candidate->traits,burst_preflight_state);if(preflight!=NativeFunctionPreflightStatus::Ready){outer_loop_declines=!(candidate->has_dynamic_cost_resolver&&native_function_preflight_allows_exact_cost_probe(preflight,candidate->traits,burst_preflight_state));if(outer_loop_declines){++native_function_preflight_counts[static_cast<std::size_t>(preflight)];}return{};}const auto preparation=native_function_dispatcher.prepare(pc,state,memory);outer_loop_declines=!preparation.prepared();return preparation;};
            bool burst_pending=true;
            NativeFunctionPreparation burst_native{};
            auto burst_declined_entry=kEeAotNoDeclinedEntry;
            for (;;)
                if (burst_pending)
                    ++committed_steps;
                    ++executed_instructions;
                    ++emulated_ee_cycles;
                service_graphics_dmacs_until(emulated_ee_cycles,vif1_dmac_serviced_ee_cycles,gif_dmac_serviced_ee_cycles,advance_idle_vif1,advance_idle_gif);
                if (host_window!=nullptr)
                    ++host_window_loop_iterations;
                kernel.set_ee_cycle_count(emulated_ee_cycles);
                memory.set_current_ee_pc(burst_pc);
                memory.set_current_guest_step(committed_steps);
                if (burst_native.prepared())
                    const auto native_call=native_function_dispatcher.dispatch(burst_native,state,memory);
                    burst_native={};
                    burst_pending=false;
                    if (!native_call.handled())
                        if (host_window!=nullptr)
                            --host_window_loop_iterations;
                        burst_stop=ScalarBurstStop::Hook;
                        break;
                    const auto retired=native_call.execution.retired_instructions;
                    const auto cycles=native_call.execution.ee_cycles;
                    observe_profiled_ee_span(committed_steps,burst_pc,retired,EePcSampleKind::NativeFunction);
                    committed_steps+=retired;
                    executed_instructions+=retired;
                    emulated_ee_cycles+=cycles;
                    ++native_function_calls;
                    native_function_retired_instructions+=retired;
                    ++native_function_hits[native_call.function->address];
                    ++scalar_burst.native_calls;
                    if (native_call.function->traits.defers_dmac_completions&&retired>1U)
                        memory.advance_dmac_completion_delays(retired-1U);
                    const std::uint64_t native_cost=std::max(retired,cycles);
                    burst_budget=burst_budget>native_cost?burst_budget-native_cost:0U;
                else
                    if (const auto compiled=burst_ee_aot?ee_aot_burst_step(result,burst_budget,burst_declined_entry):0U;compiled!=0U)
                        burst_pending=true;
                        scalar_observer_bypass_steps+=compiled;
                        scalar_burst.instructions+=compiled;
                        burst_budget-=compiled;
                    else
                        burst_pending=true;
                        result=step_r5900(state,memory,instruction_cache);
                        ++scalar_observer_bypass_steps;
                        ++scalar_burst.instructions;
                        --burst_budget;
                if (burst_pending&&result.status!=EeStepStatus::Executed)
                    burst_stop=ScalarBurstStop::Result;
                    break;
                if (memory.hardware_activity_epoch()!=burst_hardware_activity||(burst_pending&&result.vu0_micro.has_value()))
                    burst_stop=ScalarBurstStop::Hardware;
                    break;
                if (burst_ram_watch.fires())
                    burst_stop=ScalarBurstStop::State;
                    break;
                if (state.read_cop0(kCop0Status)!=burst_cop0_status)
                    if (!scalar_burst_may_start())
                        burst_stop=ScalarBurstStop::State;
                        break;
                    burst_cop0_status=state.read_cop0(kCop0Status);
                if (!burst_pending&&!scalar_burst_may_start())
                    burst_stop=ScalarBurstStop::State;
                    break;
                burst_pc=state.pc();
                burst_declined_entry=kEeAotNoDeclinedEntry;
                if (scalar_burst_hooks.marked(burst_pc)||(burst_ui_session&&ui_draw_session_feed.requires_pre_instruction(burst_ui_context,burst_pc)))
                    bool burst_outer_loop_declines=false;
                    if (burst_native_calls&&burst_budget!=0U&&!burst_ui_session&&scalar_burst_native_entries.contains(burst_pc))
                        burst_native=burst_prepare_native(burst_pc,burst_pending?1U:0U,burst_outer_loop_declines);
                    if (burst_outer_loop_declines&&burst_native_decline_steps)
                        ++scalar_burst.native_declines;
                        ee_aot_note_native_decline(burst_pc);
                        burst_declined_entry=burst_pc;
                    else
                        if (!burst_native.prepared())
                            ee_aot_note_hook_stop(burst_pc);
                            burst_stop=ScalarBurstStop::Hook;
                            break;
                if (scalar_burst_audit&&burst_pending)
                    ++scalar_burst_audit_checks;
                    const bool quiescent=scalar_burst_may_start();
                    const auto horizon=scalar_burst_horizon(execution_step_limit);
                    if (!quiescent||horizon!=burst_budget)
                        std::ostringstream detail;
                        detail<<""<<committed_steps<<""<<hex32(result.instruction.pc)<<""<<(quiescent?"":"")<<""<<horizon<<""<<burst_budget;
                        throw std::logic_error(detail.str());
                if (burst_budget==0U)
                    break;
            scalar_burst.note_stop(burst_stop);
            if (!burst_pending)
                continue;
''',
    "start predicate": r'''
if (native_model_live&&native_model_live->failed())
    return false;
if (!memory.graphics_dmacs_idle())
    return false;
if (kernel.sif_command_callback_poll_can_progress())
    return false;
if (!kernel.guest_callback_active()&&(memory.queued_dmac_completion_count()!=0U||kernel.pending_dmac_callback_can_dispatch(state)||kernel.pending_intc_callback_can_dispatch(state,memory)))
    return false;
if (guest_work_measurement_start_input_pending||host_frame_step_paused||host_window_capture_boundary_paused||native_simulation_shadow_stop_requested)
    return false;
if (guest_input_replay!=nullptr&&guest_input_replay->instruction_boundary_action_pending())
    return false;
if (vif_packet_ring_flush_claim_tracker.active())
    return false;
if (!rwa_first_op05_step_valid&&kernel.rwa_activity().opcode_counts[0x05U]!=0U)
    return false;
return memory.ee_memory_write_observer()==nullptr;
''',
    "horizon": r'''
using scalar_burst_boundaries_before;
const auto first_step=static_cast<std::uint64_t>(committed_steps)+1U;
const auto first_cycle=emulated_ee_cycles+1U;
auto horizon=scalar_burst_boundaries_before(first_step,execution_step_limit);
horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,next_vblank_start));
horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,spu2_renderer.next_sample_cycle()));
if (const auto deadline=kernel.next_sif_dma_completion_deadline();deadline.has_value())
    horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,*deadline));
if (!kernel.guest_callback_active())
    if (const auto deadline=kernel.next_alarm_deadline();deadline.has_value())
        horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,*deadline));
    if (const auto deadline=kernel.next_thread_time_slice_deadline();deadline.has_value())
        horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,*deadline));
if (host_window!=nullptr)
    horizon=std::min(horizon,scalar_burst_boundaries_before(first_step,next_host_window_step));
    horizon=std::min(horizon,scalar_burst_boundaries_before(static_cast<std::uint64_t>(host_window_loop_iterations)+1U,kHostWindowLoopIterationInterval*host_window_pump_scale()));
if (guest_input_replay!=nullptr)
    if (const auto limit=guest_input_replay->next_boundary_cycle_limit();limit.has_value())
        horizon=std::min(horizon,scalar_burst_boundaries_before(first_cycle,*limit));
return horizon;
''',
    "RAM watch fires": r'''
if (unmirrored)
    return true;
if (staging_live)
    const auto*const word=ram+0x004e2ce8U;
    const auto pointer=std::to_integer<std::uint32_t>(word[0])|(std::to_integer<std::uint32_t>(word[1])<<8U)|(std::to_integer<std::uint32_t>(word[2])<<16U)|(std::to_integer<std::uint32_t>(word[3])<<24U);
    if (pointer!=staging_last_pointer)
        return true;
if (edge_mode!=0U)
    const auto*const word=ram+kRetailControllerAcceptEdge;
    const bool edge=(std::to_integer<std::uint32_t>(word[0])|std::to_integer<std::uint32_t>(word[1]))!=0U;
    if (edge==(edge_mode==1U))
        return true;
return false;
''',
    "RAM watch builder": r'''
const auto ram=std::as_const(memory).bytes();
ScalarBurstRamWatch watch{};
watch.ram=ram.data();
constexpr std::size_t kMirroredBytes=0x00500000U;
watch.staging_live=rwa_staging_samples.size()<8U&&memory.contains(0x4e2ce8U,8U);
watch.staging_last_pointer=rwa_staging_last_pointer;
if (virtual_ds2_trace.has_value())
    if (!virtual_ds2_trace->action_edge_step.has_value())
        watch.edge_mode=1U;
    else
        if (!virtual_ds2_trace->action_edge_clear_step.has_value())
            if (first_skipped_step>*virtual_ds2_trace->action_edge_step)
                watch.edge_mode=2U;
            else
                watch.unmirrored=true;
if ((watch.staging_live||watch.edge_mode!=0U)&&ram.size()<kMirroredBytes)
    watch.unmirrored=true;
return watch;
''',
}

# The eligibility predicate must be exactly this conjunction, plus the negation
# of every term scalar_observers_configured ORs together.
REQUIRED_ELIGIBILITY_TERMS = (
    "scalar_cold_observers_enabled", "!scalar_observers_configured",
    "ee_pc_sampler == nullptr", "ee_graphics_service_diagnostics == nullptr",
    "!vu1_profile_directory.has_value()", "compiled_budget_profiler == nullptr",
    "ee_pc_hit_watch.empty()", "!object_packet_capture.enabled()",
    "!instance_batch_capture.enabled()",
    "!model_submission_probe.enabled()", "!retail_timer_owner_observer_enabled",
    "native_simulation_shadow == nullptr",
    "femain_retail_consume_invocation_barrier == nullptr",
    "femain_terminal_diagnostic == nullptr", "!femain_park_pcs.has_value()",
    "gs_semantic_trace_writer == nullptr", "!memory.ram_write_watch_active()",
    "!femain_live_transaction_enabled",
    "!subframe_input_bridge.has_value()", "scripted_input_presses.empty()",
    "!rwa_iop_shadow.has_value()", "!native_only", NO_COMPILED,
    "!vu1_worker_differential", "!vu1_worker_consume",
)


# --------------------------------------------------------------------------
# Lexical helpers
# --------------------------------------------------------------------------
def strip_comments_and_literals(text):
    """Blank out comments, string and character literals, keeping every offset."""
    out = list(text)
    i, n = 0, len(text)

    def blank(start, end):
        for k in range(start, end):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            blank(i, end)
            i = end
        elif c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            blank(i + 1, min(j, n))
            i = j + 1
        elif c == "'":
            # A quote between two alphanumerics is a digit separator (4'915'200U).
            if i > 0 and text[i - 1].isalnum() and nxt.isalnum() and (
                    i + 2 >= n or text[i + 2] != "'"):
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            blank(i + 1, min(j, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def resolve_native_only(code):
    """Blank the native-only arm of every #if on TAKEDOWN_NATIVE_ONLY_EXECUTABLE.

    The burst exists only where the interpreter does, so the trace follows the
    interpreter build. The directive lines of those conditionals are blanked
    as well; any other conditional keeps its `#` and fails the trace if an
    ordinary iteration can reach it. Offsets are preserved.
    """
    out = list(code)
    stack = []  # True: live arm, False: dead arm, None: unrelated conditional
    position = 0
    for line in code.splitlines(keepends=True):
        begin, position = position, position + len(line)
        blank = any(live is False for live in stack)
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|else|endif)\b(.*)", line)
        if directive is not None:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                if kind == "if" and re.fullmatch(r"defined\s*\(\s*%s\s*\)" % NATIVE_ONLY, rest):
                    stack.append(False)
                    blank = True
                elif kind == "if" and re.fullmatch(
                        r"!\s*defined\s*\(\s*%s\s*\)" % NATIVE_ONLY, rest):
                    stack.append(True)
                    blank = True
                else:
                    stack.append(None)
            elif kind == "else":
                if stack and stack[-1] is not None:
                    stack[-1] = not stack[-1]
                    blank = True
            elif stack and stack.pop() is not None:
                blank = True
        if blank:
            for k in range(begin, position):
                if out[k] != "\n":
                    out[k] = " "
    return "".join(out)


def normalize(expression):
    """Canonical text: namespaces dropped, whitespace kept only between words."""
    text = re.sub(r"\s*::\s*", "::", expression)
    text = re.sub(r"\b(?:%s)::" % "|".join(NAMESPACES), "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return re.sub(r"(?<=\W) | (?=\W)", "", text)


def squash(text):
    return re.sub(r"\s+", " ", text)


def find_block(text, begin_marker, end_marker):
    begin, end = text.find(begin_marker), text.find(end_marker)
    if begin < 0 or end < 0 or end < begin:
        return None
    if text.find(begin_marker, begin + 1) >= 0 or text.find(end_marker, end + 1) >= 0:
        return None
    return begin + len(begin_marker), end


OPEN, CLOSE = "([{", ")]}"


def matching_close(code, open_index):
    depth = 0
    for i in range(open_index, len(code)):
        if code[i] in OPEN:
            depth += 1
        elif code[i] in CLOSE:
            depth -= 1
            if depth == 0:
                return i
    return -1


def enclosing_open(code, position, floor):
    """Index of the bracket that encloses `position`, or -1."""
    depth = 0
    for i in range(position - 1, floor - 1, -1):
        if code[i] in CLOSE:
            depth += 1
        elif code[i] in OPEN:
            if depth == 0:
                return i
            depth -= 1
    return -1


def previous_index(code, position):
    i = position - 1
    while i >= 0 and code[i].isspace():
        i -= 1
    return i


def next_index(code, position):
    i = position
    while i < len(code) and code[i].isspace():
        i += 1
    return i


def strip_parentheses(code, begin, end):
    """Trim whitespace and redundant enclosing parentheses from code[begin:end]."""
    while True:
        begin = next_index(code, begin)
        end = previous_index(code, end) + 1
        if begin < end and code[begin] == "(" and matching_close(code, begin) == end - 1:
            begin, end = begin + 1, end - 1
        else:
            return begin, max(begin, end)


def split_top_level(code, begin, end, separator):
    parts, depth, start, i = [], 0, begin, begin
    while i < end:
        c = code[i]
        if c in OPEN:
            depth += 1
        elif c in CLOSE:
            depth -= 1
        elif depth == 0 and code.startswith(separator, i):
            parts.append((start, i))
            start = i + len(separator)
            i += len(separator)
            continue
        i += 1
    parts.append((start, end))
    return parts


def callee_before(code, open_index):
    """The name applied to the bracket at open_index, '' for plain grouping."""
    i = previous_index(code, open_index)
    end = i + 1
    if i >= 0 and code[i] == ">" and code[i - 1] != "-":
        depth = 0
        while i >= 0:
            if code[i] == ">" and code[i - 1] != "-":
                depth += 1
            elif code[i] == "<":
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        i -= 1
    while i >= 0:
        if code[i] == ":" and code[i - 1] != ":" and code[i + 1] != ":":
            break  # a ternary or label colon, not a scope operator
        if code[i].isalnum() or code[i] in "_:." or code[i].isspace():
            if code[i].isspace():
                # Only swallow whitespace inside a qualified/member chain.
                before = previous_index(code, i + 1)
                after = next_index(code, i + 1)
                joins = (before >= 0 and code[before] in ".:>") or (
                    after < len(code) and code[after] in ".:-")
                if not joins:
                    break
            i -= 1
        elif code[i] == ">" and i > 0 and code[i - 1] == "-":
            i -= 2
        else:
            break
    return re.sub(r"\s+", "", code[i + 1:end])


def comparison_after(code, position):
    """Comparison operator starting at the first token at/after position."""
    i = next_index(code, position)
    two, three = code[i:i + 2], code[i:i + 3]
    if two in ("==", "!="):
        return two, i + 2
    if two in ("<=", ">=") and three not in ("<<=", ">>="):
        return two, i + 2
    if code[i:i + 1] in ("<", ">") and two not in ("<<", ">>", "->"):
        return code[i], i + 1
    return None, i


def comparison_before(code, position):
    """Comparison operator ending just before position (skipping spaces)."""
    i = previous_index(code, position)
    if i < 1:
        return None, i
    two = code[i - 1:i + 1]
    if two in ("==", "!="):
        return two, i - 1
    if two in ("<=", ">=") and code[i - 2] not in "<>":
        return two, i - 1
    if code[i] in "<>" and two not in ("<<", ">>", "->") and code[i - 1] != "=":
        # A closing template bracket is followed by '(' and never precedes a PC.
        return code[i], i
    return None, i


def operand_right(code, position, limit):
    depth, i = 0, position
    while i < limit:
        c = code[i]
        if c in OPEN:
            depth += 1
        elif c in CLOSE:
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            if code[i:i + 2] in ("&&", "||") or c in ";,?":
                break
            if c == ":" and code[i:i + 2] != "::" and code[i - 1] != ":":
                break
        i += 1
    return code[position:i]


def operand_left(code, position, floor):
    depth, i = 0, position - 1
    while i >= floor:
        c = code[i]
        if c == "}" and depth == 0:
            break  # end of the previous statement
        if c in CLOSE:
            depth += 1
        elif c in OPEN:
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            if code[i - 1:i + 1] in ("&&", "||") or c in ";,?":
                break
            if c == "=" and code[i - 1] not in "=!<>" and code[i + 1] != "=":
                break
            if c == ":" and code[i - 1] != ":" and code[i + 1] != ":":
                break
        i -= 1
    text = code[i + 1:position]
    return re.sub(r"^\s*return\b", "", text)


ARITHMETIC = ("<<", ">>", "+", "-", "*", "/", "%", "&", "|", "^")


def arithmetic_after(code, position):
    i = next_index(code, position)
    for operator in ARITHMETIC:
        if code.startswith(operator, i):
            if operator in ("&", "|") and code[i:i + 2] in ("&&", "||"):
                return None, i
            if operator == "-" and code[i:i + 2] == "->":
                return None, i
            return operator, i + len(operator)
    return None, i


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def render_shape(shape, depth=0):
    """Indented text of a statement tree, one statement or branch head per line."""
    lines, pad = [], "    " * depth
    for node in shape:
        if isinstance(node, str):
            lines.append(pad + node)
            continue
        kind, head, body = node[0], node[1], node[2]
        lines.append(pad + (kind if head is None else "%s (%s)" % (kind, head)))
        lines.extend(render_shape(body, depth + 1))
        if len(node) > 3:
            lines.append(pad + "else")
            lines.extend(render_shape(node[3], depth + 1))
    return lines


# --------------------------------------------------------------------------
# The analysis
# --------------------------------------------------------------------------
PC_EXPRESSION = re.compile(
    r"\b[A-Za-z_]\w*\s*(?:\.|->)\s*pc\s*\(\s*\)|\bresult\s*\.\s*instruction\s*\.\s*pc\b(?!\s*\()")
CURRENT_PC = "state.pc()"
EXECUTED_PC = "result.instruction.pc"
ALIAS_DECLARATION = re.compile(r"\b(?:const\s+)?auto\s+([A-Za-z_]\w*)\s*=\s*$")
LAMBDA_DEFINITION = re.compile(r"\b(?:const\s+)?auto\s+([A-Za-z_]\w*)\s*=\s*\[(?!\[)")
STATEMENT_KEYWORD = re.compile(r"(if|while|for|do|switch|try)\b")
DECLARATION = re.compile(
    r"^(?P<qualifiers>(?:(?:static|const|constexpr)\s+)*)"
    r"(?P<type>[A-Za-z_][\w:]*(?![\w:])(?:\s*<.*?>)?)"
    r"(?:\s*(?:const\b|[*&]))*\s*"
    r"(?P<name>\b[A-Za-z_]\w*)\s*(?P<init>=(?!=)|\{|;)")
NOT_A_TYPE = ("return", "throw", "delete", "goto", "else", "case", "new", "co_return",
              "typename", "using", "break", "continue")
ASSIGNMENT = re.compile(r"(?<![=!<>])=(?!=)")
JUMPS, FALLS = "jumps", "falls"


class Analysis:
    def __init__(self, source):
        self.source = source
        self.raw_code = strip_comments_and_literals(source)
        # Everything below reads the interpreter build of the file.
        self.code = resolve_native_only(self.raw_code)
        self.violations = []
        self.uses = []  # (kind, key) pairs actually seen, for the stale-entry check

    def fail(self, position, message):
        line = self.code.count("\n", 0, position) + 1
        text = "main.cpp:%d: %s" % (line, message)
        if text not in self.violations:
            self.violations.append(text)

    # ---- structure ----
    def locate(self):
        code = self.code
        heads = [m.start() for m in re.finditer(re.escape(LOOP_HEAD), code)]
        if len(heads) != 1:
            self.violations.append("expected exactly one outer loop head, found %d" % len(heads))
            return False
        self.loop_open = code.index("{", heads[0])
        self.loop_close = matching_close(code, self.loop_open)
        if self.loop_close < 0:
            self.violations.append("outer loop body is not brace balanced")
            return False
        self.function_start = code.rfind(FUNCTION_HEAD, 0, self.loop_open)
        if self.function_start < 0:
            self.violations.append("run_boot_entry not found before the outer loop")
            return False
        blocks = {}
        for name in ("HOOKS", "ELIGIBILITY", "START-PREDICATE"):
            span = find_block(self.source, "// SCALAR-BURST-%s-BEGIN" % name,
                              "// SCALAR-BURST-%s-END" % name)
            if span is None or span[1] > self.loop_open:
                self.violations.append(
                    "missing, duplicated or misplaced SCALAR-BURST-%s block" % name)
                return False
            blocks[name] = span
        self.hook_span = blocks["HOOKS"]
        self.hook_code = code[self.hook_span[0]:self.hook_span[1]]
        self.eligibility_span = blocks["ELIGIBILITY"]
        self.eligibility = squash(code[blocks["ELIGIBILITY"][0]:blocks["ELIGIBILITY"][1]])
        self.predicate_span = blocks["START-PREDICATE"]
        self.predicate = squash(
            code[blocks["START-PREDICATE"][0]:blocks["START-PREDICATE"][1]])
        horizon = re.search(r"const auto scalar_burst_horizon\s*=", code)
        if horizon is None or horizon.start() > self.loop_open:
            self.violations.append("scalar_burst_horizon is not defined before the loop")
            return False
        horizon_open = code.index("{", code.index("]", horizon.end()))
        self.horizon_body = (horizon_open + 1, matching_close(code, horizon_open))
        self.horizon = squash(code[horizon_open:self.horizon_body[1] + 1])
        watch = code.find("struct ScalarBurstRamWatch")
        if watch < 0 or watch > blocks["START-PREDICATE"][0]:
            self.violations.append("ScalarBurstRamWatch is not defined before the start predicate")
            return False
        self.watch_span = (watch, blocks["START-PREDICATE"][0])
        self.watch = squash(code[watch:blocks["START-PREDICATE"][0]])
        body = code[self.loop_open:self.loop_close + 1]
        self.loop_text = squash(body)
        burst = body.find("if (scalar_burst_enabled &&")
        if burst < 0 or body.find("if (scalar_burst_enabled &&", burst + 1) >= 0:
            self.violations.append("expected exactly one burst inside the outer loop body")
            return False
        burst_open = self.loop_open + body.index("{", burst)
        self.burst_span = (self.loop_open + burst, matching_close(code, burst_open) + 1)
        self.burst = squash(code[self.burst_span[0]:self.burst_span[1]])
        return True

    def parse_hooks(self):
        self.singles, self.ranges = set(), set()
        for match in re.finditer(r"\bhooks\s*\.\s*(mark_\w+)\s*\(", self.hook_code):
            open_index = match.end() - 1
            close_index = matching_close(self.hook_code, open_index)
            arguments = self.split_arguments(self.hook_code[open_index + 1:close_index])
            kind = match.group(1)
            if kind == "mark_pc" and len(arguments) == 1:
                self.singles.add(arguments[0])
            elif kind == "mark_closed_range" and len(arguments) == 2:
                self.ranges.add((arguments[0], arguments[1], "closed"))
            elif kind == "mark_half_open_range" and len(arguments) == 2:
                self.ranges.add((arguments[0], arguments[1], "half_open"))
            else:
                self.violations.append("unrecognised hook registration: %s" % match.group(0))
        self.range_begins = {begin for begin, _, _ in self.ranges}
        closed_ends = {end for _, end, kind in self.ranges if kind == "closed"}
        half_open_ends = {end for _, end, kind in self.ranges if kind == "half_open"}
        # Addresses a burst never executes: `pc == X` is false at all of them.
        self.marked = self.singles | self.range_begins | closed_ends
        self.registered = self.marked | half_open_ends
        # One registration point: a const filter, marked nowhere else.
        if not re.search(r"const\s+takedown::runtime::ScalarBurstHookFilter\s+"
                         r"scalar_burst_hooks\s*=", self.hook_code):
            self.violations.append("scalar_burst_hooks must be a const filter built in the "
                                   "SCALAR-BURST-HOOKS block")
        outside = self.code[:self.hook_span[0]] + self.code[self.hook_span[1]:]
        if re.search(r"\.\s*mark_(?:pc|closed_range|half_open_range)\s*\(", outside):
            self.violations.append("hook registration outside the SCALAR-BURST-HOOKS block")
        if len(re.findall(r"\bScalarBurstHookFilter\b", outside)) != 0:
            self.violations.append("a second ScalarBurstHookFilter exists outside the "
                                   "SCALAR-BURST-HOOKS block")

    @staticmethod
    def split_arguments(text):
        arguments, depth, start = [], 0, 0
        for i, c in enumerate(text):
            if c in OPEN:
                depth += 1
            elif c in CLOSE:
                depth -= 1
            elif c == "," and depth == 0:
                arguments.append(normalize(text[start:i]))
                start = i + 1
        arguments.append(normalize(text[start:]))
        return arguments

    # ---- lambdas ----
    def find_lambdas(self):
        """Callable locals of run_boot_entry: name -> [(definition, body open, body close)]."""
        code = self.code
        self.lambdas = {}
        for match in LAMBDA_DEFINITION.finditer(code, self.function_start, self.loop_close):
            capture_close = matching_close(code, match.end() - 1)
            i = next_index(code, capture_close + 1)
            if code[i] == "(":
                i = matching_close(code, i) + 1
            body_open = code.find("{", i)
            if body_open < 0 or ";" in code[i:body_open]:
                continue
            body_close = matching_close(code, body_open)
            if code[next_index(code, body_close + 1)] == "(":
                continue  # immediately invoked: the name is a value, not a callable
            self.lambdas.setdefault(match.group(1), []).append(
                (match.start(), body_open, body_close))
        self.lambda_reference = re.compile(
            r"(?<![\w.>:])(%s)\b" % "|".join(sorted(map(re.escape, self.lambdas))))

    def lambda_definition(self, name, position):
        """The definition of `name` in force at `position` (the nearest earlier one)."""
        best = None
        for definition in self.lambdas.get(name, ()):
            if definition[0] < position and (best is None or definition[0] > best[0]):
                best = definition
        return best

    def lambda_references(self, begin, end):
        """(name, position) of every lambda named in code[begin:end]."""
        found = []
        for match in self.lambda_reference.finditer(self.code, begin, end):
            name, position = match.group(1), match.start()
            if any(definition[0] <= position < definition[1]
                   for definition in self.lambdas[name]):
                continue  # the definition itself
            found.append((name, position))
        return found

    def reachable_lambda_bodies(self):
        """Bodies of the pre-loop lambdas the loop body can name, transitively."""
        pending = [(self.loop_open, self.loop_close)]
        seen, bodies = set(), []
        while pending:
            begin, end = pending.pop()
            for name, position in self.lambda_references(begin, end):
                definition = self.lambda_definition(name, position)
                if definition is None or definition[0] in seen:
                    continue
                seen.add(definition[0])
                if definition[0] < self.loop_open:
                    bodies.append((definition[1], definition[2]))
                pending.append((definition[1], definition[2]))
        # A lambda nested in another reachable lambda is already inside its body.
        return sorted(body for body in bodies if not any(
            outer != body and outer[0] <= body[0] and body[1] <= outer[1] for outer in bodies))

    # ---- requirement checking ----
    def require(self, position, what, coverage):
        for name in coverage.get("registered", ()):
            if normalize(name) not in self.registered:
                self.fail(position, "%s relies on %s being registered in the hook block"
                          % (what, name))
        for text in coverage.get("hook_text", ()):
            if squash(text) not in squash(self.hook_code):
                self.fail(position, "%s relies on the hook block containing '%s'" % (what, text))
        for key, haystack in (("eligibility", self.eligibility), ("predicate", self.predicate),
                              ("horizon", self.horizon), ("burst", self.burst),
                              ("watch", self.watch), ("loop", self.loop_text)):
            for text in coverage.get(key, ()):
                if squash(text) not in haystack:
                    self.fail(position, "%s relies on the %s code containing '%s'"
                              % (what, key, text))
        for entry in coverage.get("assignments", ()):
            name, expected = entry.split("=")
            found = len(re.findall(r"(?<![\w.>])%s\s*=(?!=)" % re.escape(name),
                                   self.code[self.loop_open:self.loop_close]))
            if found != int(expected):
                self.fail(position, "%s relies on the loop body assigning '%s' in exactly %s "
                          "places; it now does so in %d" % (what, name, expected, found))

    def reviewed(self, position, table, kind, key, describe):
        self.uses.append((kind, key))
        coverage = table.get(key)
        if coverage is None:
            self.fail(position, describe)
            return
        self.require(position, "%s '%s'" % (kind, key if isinstance(key, str) else " ".join(key)),
                     coverage)

    # ---- PC expressions ----
    def pc_occurrences(self):
        """(start, end, lo, hi) of every PC expression, aliases included.

        Covers the loop body and the body of every lambda it can reach; lo/hi
        bound the region the occurrence was found in.
        """
        code = self.code
        spans, aliases = [], []
        self.exact_aliases, self.executed_aliases = set(), set()
        for lo, hi in self.pc_regions:
            for match in PC_EXPRESSION.finditer(code, lo, hi):
                start, end = match.start(), match.end()
                spans.append((start, end, lo, hi))
                # `auto NAME = <pc>;` or `auto NAME = static_cast<T>(<pc> + ...);`
                # (Only the text just before the expression can hold the declaration.)
                window = max(lo, start - 400)
                head = code[window:start]
                cast = re.search(r"static_cast\s*<[^<>;]*>\s*\(\s*$", head)
                if cast is not None:
                    head = head[:cast.start()]
                declaration = ALIAS_DECLARATION.search(head)
                if declaration is None:
                    continue
                scope_open = enclosing_open(code, window + declaration.start(), lo)
                scope_close = matching_close(code, scope_open) if scope_open >= 0 else hi
                name = declaration.group(1)
                aliases.append((name, start, scope_close, lo, hi))
                if normalize(code[start:end]) == EXECUTED_PC:
                    self.executed_aliases.add(name)
                elif cast is None and code[next_index(code, end)] == ";" and \
                        declaration.group(0).startswith("const") and \
                        normalize(code[start:end]) == CURRENT_PC:
                    self.exact_aliases.add(name)  # `const auto NAME = state.pc();`
        for name, begin, scope_close, lo, hi in aliases:
            for match in re.finditer(r"(?<![\w.>])%s\b(?!\s*\()" % re.escape(name),
                                     code[begin:scope_close]):
                spans.append((begin + match.start(), begin + match.end(), lo, hi))
        self.aliases = sorted({name for name, _, _, _, _ in aliases})
        # A name that is also declared from PC arithmetic is not the current PC.
        self.exact_aliases -= {name for name in self.aliases
                               if name in self.executed_aliases}
        spans = sorted(set(spans))
        self.pc_by_start = {start: end for start, end, _, _ in spans}
        self.pc_by_end = {end: start for start, end, _, _ in spans}
        return spans

    def classify(self, start, end, lo, hi, pc_name=None):
        code = self.code
        text = normalize(code[start:end])
        pc_name = pc_name or text

        # A cast around the PC is transparent: classify the cast expression.
        before = previous_index(code, start)
        after = next_index(code, end)
        if code[before] == "(" and code[after:after + 1] == ")":
            callee = callee_before(code, before)
            if callee.startswith("static_cast<"):
                return self.classify(code.rfind("static_cast", lo, before), after + 1, lo, hi,
                                     pc_name)

        if code[after:after + 1] == "=" and code[after + 1:after + 2] != "=":
            return None  # an alias being assigned; the assigned value is classified

        operator, operand_start = comparison_after(code, end)
        if operator is not None:
            operand = normalize(operand_right(code, operand_start, hi))
            return self.check_comparison(start, operator, operand, pc_name, lo, hi)

        arithmetic, other_start = arithmetic_after(code, end)
        if arithmetic is not None:
            other = normalize(operand_right(code, other_start, hi))
            other = re.split(r"(==|!=|<=|>=|<(?!<)|>(?!>))", other)[0] if not other.startswith(
                "static_cast<") else other
            key = arithmetic + other
            return self.reviewed(
                start, REVIEWED_ARITHMETIC, "PC arithmetic", key,
                "unreviewed arithmetic on the guest PC: %s %s %s" % (text, arithmetic, other))

        operator, operator_start = comparison_before(code, start)
        if operator is not None:
            operand = normalize(operand_left(code, operator_start, lo))
            mirrored = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(operator, operator)
            return self.check_comparison(start, mirrored, operand, pc_name, lo, hi)

        previous = code[before]
        if previous in "(,":
            open_index = before if previous == "(" else enclosing_open(code, start, lo)
            callee = callee_before(code, open_index) if open_index >= 0 else ""
            if code[open_index] == "(" and callee and callee not in (
                    "if", "while", "for", "switch", "return"):
                return self.reviewed(
                    start, PC_HELPERS, "PC helper", normalize(callee),
                    "the guest PC is passed to unreviewed helper %s(); classify it in "
                    "PC_HELPERS" % callee)
            return self.fail(start, "unrecognised use of the guest PC inside %s%s"
                             % (callee, code[open_index] if open_index >= 0 else "?"))
        if previous == "[":
            container = callee_before(code, before)
            return self.reviewed(
                start, REVIEWED_INDEXES, "PC index", normalize(container),
                "unreviewed container indexed by the guest PC: %s" % container)
        if previous == "=" and code[before - 1] not in "=!<>":
            target = normalize(operand_left(code, before, lo)).split("{")[-1]
            if ALIAS_DECLARATION.search(code[max(lo, before - 400):before + 1]):
                return None  # alias definition; its uses are classified
            return self.reviewed(
                start, REVIEWED_STORES, "PC store", target,
                "the guest PC is stored in unreviewed target '%s'" % target)
        if code[before - 1:before + 1] == "<<":
            return None  # stream output
        return self.fail(start, "unrecognised use of the guest PC near '%s'"
                         % squash(code[max(lo, start - 40):end + 20]))

    def is_executed_pc(self, pc_name):
        return pc_name == EXECUTED_PC or pc_name in self.executed_aliases

    def check_comparison(self, position, operator, operand, pc_name, lo, hi):
        self.uses.append(("PC comparison", (operator, operand)))
        if self.is_executed_pc(pc_name):
            # The burst replaces the tail of the ordinary step that precedes
            # it, whatever PC that instruction had: registration cannot help.
            key = ("executed " + operator, operand)
            coverage = REVIEWED_COMPARISONS.get(key)
            if coverage is None:
                return self.fail(
                    position, "the executed instruction's PC is compared (%s) with '%s'. A "
                    "burst skips the tail of the ordinary step before it at any PC, so "
                    "registering the address does not cover this; review it in "
                    "REVIEWED_COMPARISONS as ('executed %s', '%s')"
                    % (operator, operand, operator, operand))
            return self.require(position, "PC comparison executed %s %s" % (operator, operand),
                                coverage)
        if operator == "==" and operand in self.marked:
            return None
        if operator in ("<", "<=", ">", ">=") and self.matched_range(
                self.conjunction_around(position, lo, hi), position) is not None:
            return None
        coverage = REVIEWED_COMPARISONS.get((operator, operand))
        if coverage is not None:
            self.uses.append(("reviewed comparison", (operator, operand)))
            return self.require(position, "PC comparison %s %s" % (operator, operand), coverage)
        if operator == "==":
            return self.fail(
                position, "guest PC compared (==) with '%s', which the SCALAR-BURST-HOOKS "
                "block does not register" % operand)
        if operator == "!=":
            return self.fail(
                position, "guest PC compared (!=) with '%s': the comparison holds at every "
                "other PC, so registering the address does not cover it; review it in "
                "REVIEWED_COMPARISONS" % operand)
        return self.fail(
            position, "guest PC bound (%s) with '%s' is not part of a lower/upper pair, in "
            "one conjunction, that equals one registered range of the SCALAR-BURST-HOOKS "
            "block" % (operator, operand))

    # ---- range pairs ----
    def pc_comparison(self, begin, end):
        """(pc name, operator, operand) when code[begin:end] is exactly PC <op> X."""
        code = self.code
        begin, end = strip_parentheses(code, begin, end)
        stop = self.pc_by_start.get(begin)
        if stop is not None and stop < end:
            operator, operand_start = comparison_after(code, stop)
            if operator is not None and operand_start < end:
                return (normalize(code[begin:stop]), operator,
                        normalize(code[operand_start:end]))
        start = self.pc_by_end.get(end)
        if start is not None and start > begin:
            operator, operator_start = comparison_before(code, start)
            if operator is not None and operator_start > begin:
                mirrored = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(operator, operator)
                return (normalize(code[start:end]), mirrored,
                        normalize(code[begin:operator_start]))
        return None

    def conjunction_around(self, position, lo, hi):
        """Spans of the `&&` siblings of the expression that contains position."""
        code = self.code
        depth, i = 0, position - 1
        while i >= lo:
            c = code[i]
            if c in CLOSE:
                depth += 1
            elif c in OPEN:
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0:
                if code[i - 1:i + 1] == "||" or c in ";,?":
                    break
                if c == "=" and code[i - 1] not in "=!<>" and code[i + 1] != "=":
                    break
                if c == ":" and code[i - 1] != ":" and code[i + 1] != ":":
                    break
            i -= 1
        left = i + 1
        keyword = re.match(r"\s*return\b", code[left:position])
        if keyword is not None:
            left += keyword.end()
        depth, j = 0, position
        while j < hi:
            c = code[j]
            if c in OPEN:
                depth += 1
            elif c in CLOSE:
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0:
                if code[j:j + 2] == "||" or c in ";,?":
                    break
                if c == ":" and code[j:j + 2] != "::" and code[j - 1] != ":":
                    break
            j += 1
        return split_top_level(code, left, j, "&&")

    def matched_range(self, conjuncts, position=None, current_only=False):
        """Index of the conjunct that completes a registered range test, or None.

        A range test is a lower bound `pc >= B` and an upper bound `pc <= E`
        or `pc < E` on the same PC in one conjunction, whose bounds equal one
        registered (or reviewed) range. With `position`, the pair must include
        the comparison at that position. With `current_only`, the PC must be
        the one a skipped boundary is about to execute.
        """
        atoms = [self.pc_comparison(begin, end) for begin, end in conjuncts]
        for low, lower in enumerate(atoms):
            if lower is None or lower[1] != ">=":
                continue
            for high, upper in enumerate(atoms):
                if upper is None or upper[1] not in ("<=", "<") or upper[0] != lower[0]:
                    continue
                if self.is_executed_pc(lower[0]) or (
                        current_only and not self.is_current_pc(lower[0])):
                    continue
                if position is not None and not any(
                        conjuncts[k][0] <= position < conjuncts[k][1] for k in (low, high)):
                    continue
                triple = (lower[2], upper[2], "closed" if upper[1] == "<=" else "half_open")
                if triple in self.ranges:
                    return max(low, high)
                coverage = REVIEWED_RANGES.get(triple)
                if coverage is not None:
                    self.uses.append(("reviewed range", triple))
                    self.require(conjuncts[low][0], "PC range %s..%s" % triple[:2], coverage)
                    return max(low, high)
        return None

    # ---- whole-state helpers and accessors ----
    def check_state_uses(self):
        code = self.code
        for lo, hi in self.pc_regions:
            for match in re.finditer(r"(?<![\w.>])state\b", code[lo:hi]):
                start = lo + match.start()
                end = lo + match.end()
                after = next_index(code, end)
                if code[after] == ".":
                    method = re.match(r"\.\s*([A-Za-z_]\w*)", code[after:])
                    name = method.group(1)
                    self.reviewed(start, REVIEWED_STATE_METHODS, "CPU state accessor", name,
                                  "unreviewed CPU state accessor state.%s() in the outer loop; "
                                  "classify it in REVIEWED_STATE_METHODS" % name)
                    continue
                before = previous_index(code, start)
                if code[before] in "(," and code[after] in ",)":
                    open_index = before if code[before] == "(" else enclosing_open(
                        code, start, lo)
                    callee = callee_before(code, open_index)
                    if code[open_index] == "(" and callee:
                        self.reviewed(start, STATE_HELPERS, "CPU state helper", callee,
                                      "the CPU state is handed to unreviewed helper %s(); "
                                      "classify it in STATE_HELPERS" % callee)
                        continue
                self.fail(start, "unrecognised use of the CPU state near '%s'"
                          % squash(code[max(lo, start - 40):end + 20]))

    # ---- statements ----
    def parse_statement(self, position, end):
        """The statement starting at the first token at/after position, or None."""
        code = self.code
        i = next_index(code, position)
        if i >= end:
            return None
        if code[i] == "{":
            close = matching_close(code, i)
            return {"kind": "block", "start": i, "end": close + 1, "inner": (i + 1, close)}
        keyword = STATEMENT_KEYWORD.match(code, i)
        if keyword is not None:
            kind = keyword.group(1)
            j = next_index(code, keyword.end())
            if kind in ("if", "while", "for", "switch") and code[j] == "(":
                close = matching_close(code, j)
                k = next_index(code, close + 1)
                while code.startswith("[[", k):
                    k = next_index(code, code.index("]]", k) + 2)
                body = self.parse_statement(k, end)
                statement = {"kind": kind, "start": i, "cond": (j + 1, close), "body": body,
                             "end": body["end"]}
                if kind == "if":
                    k = next_index(code, body["end"])
                    if re.compile(r"else\b").match(code, k):
                        other = self.parse_statement(k + 4, end)
                        statement["else"] = other
                        statement["end"] = other["end"]
                return statement
            if kind == "do":
                body = self.parse_statement(j, end)
                close = matching_close(code, code.index("(", body["end"]))
                return {"kind": "do", "start": i, "end": code.index(";", close) + 1}
            if kind == "try":
                k = self.parse_statement(j, end)["end"]
                while re.compile(r"catch\b").match(code, next_index(code, k)):
                    close = matching_close(code, code.index("(", k))
                    k = self.parse_statement(close + 1, end)["end"]
                return {"kind": "try", "start": i, "end": k}
        depth, k = 0, i
        while k < end:
            c = code[k]
            if c in OPEN:
                depth += 1
            elif c in CLOSE:
                depth -= 1
            elif c == ";" and depth == 0:
                return {"kind": "simple", "start": i, "end": k + 1}
            k += 1
        return {"kind": "simple", "start": i, "end": end}

    def statements_in(self, begin, end):
        statements, position = [], begin
        while True:
            statement = self.parse_statement(position, end)
            if statement is None:
                return statements
            statements.append(statement)
            position = statement["end"]

    def sub_statements(self, statement):
        if statement["kind"] == "block":
            return self.statements_in(*statement["inner"])
        return [statement]

    def top_level_statements(self):
        """Spans of the statements directly inside the loop body."""
        return [(statement["start"], statement["end"])
                for statement in self.statements_in(self.loop_open + 1, self.loop_close)]

    def shape(self, statements):
        code, shape = self.code, []
        for statement in statements:
            kind = statement["kind"]
            if kind == "block":
                shape.append(("block", None, self.shape(self.sub_statements(statement))))
            elif kind in ("if", "while", "for", "switch"):
                node = [kind, normalize(code[statement["cond"][0]:statement["cond"][1]]),
                        self.shape(self.sub_statements(statement["body"]))]
                if "else" in statement:
                    node.append(self.shape(self.sub_statements(statement["else"])))
                shape.append(tuple(node))
            else:
                shape.append(normalize(code[statement["start"]:statement["end"]]))
        return shape

    # ---- burst-boundary trace ----
    def trace(self):
        """Walk the loop body as an ordinary iteration runs it at a skipped boundary."""
        self.run_counts = collections.Counter()
        self.fact_uses, self.host_only_uses, self.pure_uses = set(), set(), set()
        self.local_type_uses = set()
        self.traced_lambdas, self.traced_lambda_names = set(), set()
        self.scopes = [{}]
        self.after_step = False
        self.reached_burst = False
        self.reached_commit = self.trace_statements(
            self.statements_in(self.loop_open + 1, self.loop_close), False) == JUMPS
        for key, coverage in RUNS.items():
            expected = coverage.get("count", 1)
            if self.run_counts[key] not in (0, expected):
                self.violations.append(
                    "an ordinary iteration executes '%s' %d times at a burst boundary; RUNS "
                    "reviewed %d" % (key, self.run_counts[key], expected))
        if not self.reached_burst:
            self.violations.append("the burst-boundary trace does not reach the burst: the "
                                   "bypass step is not on the path an ordinary iteration takes")
        if not self.reached_commit:
            self.violations.append("the burst-boundary trace does not end at the commit tail's "
                                   "`continue;`")

    def trace_statements(self, statements, in_lambda):
        for statement in statements:
            if self.trace_statement(statement, in_lambda) == JUMPS:
                return JUMPS
        return FALLS

    def trace_branch(self, statement, in_lambda):
        if statement is None:
            return FALLS
        self.scopes.append({})
        result = self.trace_statements(self.sub_statements(statement), in_lambda)
        self.scopes.pop()
        return result

    def trace_statement(self, statement, in_lambda):
        kind = statement["kind"]
        if kind == "block":
            return self.trace_branch(statement, in_lambda)
        if kind == "if":
            if (statement["start"], statement["end"]) == self.burst_span:
                self.reached_burst = True  # pinned separately by check_shapes
                return FALLS
            value = self.evaluate_condition(*statement["cond"])
            other = statement.get("else")
            if value == FALSE:
                return self.trace_branch(other, in_lambda)
            if value == TRUE:
                return self.trace_branch(statement["body"], in_lambda)
            taken = self.trace_branch(statement["body"], in_lambda)
            skipped = self.trace_branch(other, in_lambda)
            return JUMPS if taken == JUMPS and skipped == JUMPS else FALLS
        if kind == "while" and self.evaluate_condition(*statement["cond"]) == FALSE:
            return FALLS
        return self.trace_leaf(statement["start"], statement["end"], in_lambda)

    def evaluate_condition(self, begin, end):
        parts = split_top_level(self.code, begin, end, ";")
        for init_begin, init_end in parts[:-1]:  # `if (init; condition)`
            self.check_pure(init_begin, init_end, "the init-statement of a condition",
                            allow_declaration=True)
        return self.evaluate(*parts[-1])

    def match_run(self, text):
        if text in RUNS:
            return text
        for key in RUNS:
            if key.endswith("...") and text.startswith(key[:-3]):
                return key
        return None

    def trace_leaf(self, start, end, in_lambda):
        code = self.code
        raw = squash(code[start:end]).strip()
        text = normalize(code[start:end])
        if "#" in raw:
            self.fail(start, "a preprocessor conditional sits in code an ordinary iteration "
                      "executes at a burst boundary; the trace cannot follow it")
            return FALLS
        key = self.match_run(text)
        if key is not None:
            self.run_counts[key] += 1
            self.require(start, "burst-boundary statement '%s'" % key[:70], RUNS[key])
            for name, position in self.lambda_references(start, end):
                self.trace_lambda(name, position)
            if key == STEP_LEAF:
                # Everything after this point belongs to the instruction just
                # executed; facts about the PC it was fetched from are stale.
                self.after_step = True
                self.scopes = [{} for _ in self.scopes]
            return JUMPS if re.match(r"(?:continue|break|return|throw)\b", raw) else FALLS
        if re.match(r"return\b", raw) and in_lambda:
            self.check_pure(start + len("return"), end - 1,
                            "a lambda result computed at every boundary")
            return JUMPS
        if re.match(r"(?:continue|break|return|throw|goto)\b", raw):
            self.fail(start, "an ordinary iteration reaches this jump at a burst boundary and "
                      "it is not reviewed in RUNS: %s" % text[:120])
            return JUMPS
        call = re.match(r"([A-Za-z_]\w*)\s*\(", code[start:end])
        if call is not None and call.group(1) in self.lambdas:
            open_index = start + call.end() - 1
            close_index = matching_close(code, open_index)
            if normalize(code[close_index + 1:end]) == ";":
                self.check_pure(open_index + 1, close_index,
                                "an argument of %s()" % call.group(1))
                self.trace_lambda(call.group(1), start)
                return FALLS
        lambda_definition = LAMBDA_DEFINITION.match(code, start, end)
        if lambda_definition is not None and code[previous_index(code, end - 1)] == "}":
            return FALLS  # defines a callable; its body is walked where it is called
        declaration = DECLARATION.match(raw)
        if declaration is not None and declaration.group("type") not in NOT_A_TYPE:
            init = declaration.group("init")
            if init != "=":
                kind = normalize(re.sub(r"<.*", "", declaration.group("type")))
                if kind not in PLAIN_LOCAL_TYPES:
                    self.fail(start, "an ordinary iteration constructs a '%s' at every "
                              "instruction boundary; its constructor and destructor are not "
                              "reviewed in PLAIN_LOCAL_TYPES: %s" % (kind, text[:120]))
                else:
                    self.local_type_uses.add(kind)
            if init == "{":
                self.check_pure(code.index("{", start), end - 1,
                                "the initializer of '%s'" % declaration.group("name"))
            elif init == "=":
                assignment = ASSIGNMENT.search(code, start, end)
                value = self.evaluate(assignment.end(), end - 1)
                if "const" in declaration.group("qualifiers") and value != UNKNOWN:
                    self.scopes[-1][declaration.group("name")] = value
            return FALLS
        self.fail(start, "an ordinary iteration executes this statement at every instruction "
                  "boundary, and it is neither a side-effect free declaration nor reviewed in "
                  "RUNS (gate it on a registered PC, or classify it with the eligibility, "
                  "predicate, horizon or burst term that covers it): %s" % text[:160])
        return FALLS

    def trace_lambda(self, name, position):
        definition = self.lambda_definition(name, position)
        if definition is None or (definition[0], self.after_step) in self.traced_lambdas:
            return
        self.traced_lambdas.add((definition[0], self.after_step))
        self.traced_lambda_names.add(name)
        code = self.code
        _, body_open, body_close = definition
        coverage = HOST_ONLY_LAMBDAS.get(name)
        if coverage is not None:
            self.host_only_uses.add(name)
            body = code[body_open:body_close + 1]
            self.require(body_open, "host-only lambda %s" % name, coverage["cover"])
            if digest(normalize(body)) != coverage["digest"]:
                self.fail(body_open, "the body of host-only lambda %s changed; an ordinary "
                          "iteration runs it at every boundary and a burst does not. Review "
                          "it again, then pin digest '%s' in HOST_ONLY_LAMBDAS"
                          % (name, digest(normalize(body))))
            for token in HOST_ONLY_FORBIDDEN:
                if re.search(r"(?<![\w.>])%s\b" % token, body):
                    self.fail(body_open, "host-only lambda %s names guest-visible state '%s'"
                              % (name, token))
            return
        saved_scopes, self.scopes = self.scopes, [{}]
        self.trace_statements(self.statements_in(body_open + 1, body_close), True)
        self.scopes = saved_scopes

    def lookup_fact(self, text, position):
        for scope in reversed(self.scopes):
            if text in scope:
                return scope[text]
        entry = FACTS.get(text)
        if entry is None:
            return None
        value, coverage = entry
        if self.after_step and (coverage.get("registered") or coverage.get("hook_text")):
            return None  # describes the PC the executed instruction was fetched from
        self.fact_uses.add(text)
        self.require(position, "burst-boundary fact '%s'" % text[:70], coverage)
        return value

    def evaluate(self, begin, end):
        """false / true / unknown for code[begin:end] at a skipped boundary.

        Sub-expressions are visited in evaluation order and only as far as
        C++ would evaluate them; everything visited must be side-effect free.
        """
        code = self.code
        begin, end = strip_parentheses(code, begin, end)
        text = normalize(code[begin:end])
        if text in ("false", "true"):
            return text
        known = self.lookup_fact(text, begin)
        if known is not None:
            return known
        if len(split_top_level(code, begin, end, "?")) > 1:
            self.check_pure(begin, end, "a conditional expression")
            return UNKNOWN
        alternatives = split_top_level(code, begin, end, "||")
        if len(alternatives) > 1:
            result = FALSE
            for lo, hi in alternatives:
                value = self.evaluate(lo, hi)
                if value == TRUE:
                    return TRUE
                if value == UNKNOWN:
                    result = UNKNOWN
            return result
        conjuncts = split_top_level(code, begin, end, "&&")
        if len(conjuncts) > 1:
            closing = self.matched_range(conjuncts, current_only=True)
            result = TRUE
            for index, (lo, hi) in enumerate(conjuncts):
                if index == closing:
                    return FALSE  # no unmarked PC lies inside a registered range
                value = self.evaluate(lo, hi)
                if value == FALSE:
                    return FALSE
                if value == UNKNOWN:
                    result = UNKNOWN
            return result
        if code[begin] == "!" and code[begin + 1] != "=" and not self.has_binary_operator(
                begin + 1, end):
            value = self.evaluate(begin + 1, end)
            return {FALSE: TRUE, TRUE: FALSE}.get(value, UNKNOWN)
        comparison = self.pc_comparison(begin, end)
        if comparison is not None and self.is_current_pc(comparison[0]):
            _, operator, operand = comparison
            coverage = REVIEWED_COMPARISONS.get((operator, operand))
            marked = operand in self.marked or (
                operator == "==" and coverage is not None and coverage.get("registered"))
            if marked and operator in ("==", "!="):
                return FALSE if operator == "==" else TRUE
        self.check_pure(begin, end, "a condition")
        return UNKNOWN

    def has_binary_operator(self, begin, end):
        """True when code[begin:end] is more than one unary operand, so a `!` in front of it
        applies to its first operand only."""
        code, depth = self.code, 0
        for i in range(begin, end):
            c = code[i]
            if c in OPEN:
                depth += 1
            elif c in CLOSE:
                depth -= 1
            elif depth == 0 and c in "=<>?+*/%&|^" and code[i - 1:i + 1] != "->":
                return True
        return False

    def is_current_pc(self, pc_name):
        """True when pc_name is the PC of the instruction a skipped boundary is about to run."""
        if pc_name == CURRENT_PC:
            return True
        # Before the step a `const auto NAME = state.pc();` still holds that
        # PC. After it, such a name is the instruction just executed, which
        # the hook filter never tested when an ordinary step precedes a burst.
        return not self.after_step and pc_name in self.exact_aliases

    def check_pure(self, begin, end, what, allow_declaration=False):
        """Fail when code[begin:end], evaluated at every boundary, can have an effect."""
        code = self.code
        text = normalize(code[begin:end])
        plain = re.sub(r"([{,])\.\w+=(?!=)", r"\1", text)  # designated initializers
        if allow_declaration:
            plain = ASSIGNMENT.sub("", plain, count=1)
        if re.search(r"\+\+|--|<<=|>>=", plain) or ASSIGNMENT.search(plain):
            self.fail(begin, "%s modifies state while an ordinary iteration evaluates it at "
                      "every instruction boundary: %s" % (what, text[:120]))
        for match in re.finditer(r"\(", code[begin:end]):
            callee = normalize(callee_before(code, begin + match.start()))
            if not callee or callee in ("sizeof", "alignof", "decltype"):
                continue
            if callee.startswith(PURE_CASTS):
                continue
            callee = re.sub(r"<.*>$", "", callee)
            owner, _, name = re.sub(r"->", ".", callee).rpartition(".")
            if owner and name in PURE_METHODS:  # a member call, never a bare name
                self.pure_uses.add(name)
                continue
            if callee in self.lambdas:
                continue  # walked below
            self.fail(begin, "%s calls %s() at every instruction boundary; it is neither in "
                      "PURE_METHODS nor part of a reviewed FACTS or RUNS entry: %s"
                      % (what, callee, text[:120]))
        for name, position in self.lambda_references(begin, end):
            self.trace_lambda(name, position)

    # ---- object method vocabulary ----
    def check_object_methods(self):
        body = self.code[self.loop_open:self.loop_close]
        self.methods_seen = {}
        for name, known in OBJECT_METHOD_NAMES.items():
            self.require(self.loop_open, "the %s method review" % name, OBJECT_METHODS[name])
            seen = self.methods_seen.setdefault(name, set())
            for match in re.finditer(
                    r"(?<![\w.>])%s\s*(?:\.|->)\s*([A-Za-z_]\w*)\s*\(" % name, body):
                seen.add(match.group(1))
                if match.group(1) not in known:
                    self.fail(self.loop_open + match.start(),
                              "the outer loop calls %s.%s(), which is not in the reviewed "
                              "method list OBJECT_METHOD_NAMES['%s']"
                              % (name, match.group(1), name))

    # ---- shape pins ----
    def shape_sources(self):
        """Statement trees of the burst machinery, by EXPECTED_SHAPES key."""
        code = self.code
        sources = {}
        burst = self.parse_statement(self.burst_span[0], self.burst_span[1])
        sources["burst"] = self.shape([burst])
        predicate = re.search(r"const auto scalar_burst_may_start\s*=", code)
        if predicate is not None and self.predicate_span[0] <= predicate.start() < \
                self.predicate_span[1]:
            body_open = code.index("{", code.index("]", predicate.end()))
            sources["start predicate"] = self.shape(
                self.statements_in(body_open + 1, matching_close(code, body_open)))
        sources["horizon"] = self.shape(self.statements_in(*self.horizon_body))
        fires = re.compile(r"bool fires\(\) const noexcept\s*\{").search(
            code, self.watch_span[0], self.watch_span[1])
        if fires is not None:
            sources["RAM watch fires"] = self.shape(
                self.statements_in(fires.end(), matching_close(code, fires.end() - 1)))
        builder = re.compile(r"const auto scalar_burst_ram_watch\s*=").search(
            code, self.watch_span[0], self.watch_span[1])
        if builder is not None:
            body_open = code.index("{", code.index("]", builder.end()))
            sources["RAM watch builder"] = self.shape(
                self.statements_in(body_open + 1, matching_close(code, body_open)))
        return sources

    def check_shapes(self):
        self.shapes = {name: render_shape(shape) for name, shape in self.shape_sources().items()}
        for name, expected in EXPECTED_SHAPES.items():
            actual = self.shapes.get(name)
            if actual is None:
                self.violations.append("the %s could not be located" % name)
                continue
            wanted = [line for line in expected.strip("\n").split("\n")]
            for index in range(max(len(actual), len(wanted))):
                have = actual[index] if index < len(actual) else "<end>"
                want = wanted[index] if index < len(wanted) else "<end>"
                if have != want:
                    self.violations.append(
                        "the %s no longer matches its reviewed shape at statement %d: "
                        "expected '%s', found '%s'. Every statement and condition of the "
                        "burst machinery is pinned; review the change, then update "
                        "EXPECTED_SHAPES" % (name, index + 1, want.strip(), have.strip()))
                    break

    # ---- required terms ----
    def check_required_terms(self):
        code = self.code
        # Everything scalar_observers_configured ORs together must be restated.
        expected = [normalize(term) for term in REQUIRED_ELIGIBILITY_TERMS]
        configured = re.search(r"const bool scalar_observers_configured\s*=(.*?);", code, re.S)
        if configured is None:
            self.violations.append("scalar_observers_configured not found")
            return
        for term in configured.group(1).split("||"):
            term = squash(term).strip()
            if term.endswith("!= nullptr"):
                negated = term.replace("!= nullptr", "== nullptr")
            else:
                negated = "!" + term
            if negated not in self.eligibility:
                self.violations.append(
                    "eligibility predicate does not restate scalar_observers_configured "
                    "term '%s' (expected '%s')" % (term, negated))
            expected.append(normalize(negated))
        # The predicate must be exactly the reviewed conjunction: a term that
        # is missing, or weakened into `(term || ...)`, is no longer a term.
        declaration = re.compile(
            r"const bool scalar_burst_statically_eligible\s*=").search(
                code, self.eligibility_span[0], self.eligibility_span[1])
        if declaration is None:
            self.violations.append("scalar_burst_statically_eligible is not defined in the "
                                   "SCALAR-BURST-ELIGIBILITY block")
            return
        finish = code.index(";", declaration.end())
        actual = [normalize(code[begin:end])
                  for begin, end in split_top_level(code, declaration.end(), finish, "&&")]
        for term in expected:
            if term not in actual:
                self.violations.append("eligibility predicate lost the term '%s'" % term)
        for term in actual:
            if term not in expected:
                self.violations.append(
                    "eligibility predicate has the unreviewed conjunct '%s'; list it in "
                    "REQUIRED_ELIGIBILITY_TERMS" % term)
        # The burst lives where the interpreter exists, and only there.
        raw = self.raw_code
        guard = raw.rfind("#if", 0, self.burst_span[0])
        closing = raw.rfind("#endif", 0, self.burst_span[0])
        directive = raw[guard:raw.find("\n", guard)]
        between = raw[guard:self.burst_span[0]]
        if NATIVE_ONLY not in directive or "#else" not in between or closing > guard:
            self.violations.append("the burst must sit in the #else arm of "
                                   "#if defined(TAKEDOWN_NATIVE_ONLY_EXECUTABLE)")
        # The eligibility block feeds the one switch the burst tests.
        if not re.search(r"const bool scalar_burst_enabled\s*=\s*scalar_burst_requested\s*&&"
                         r"\s*scalar_burst_statically_eligible\s*;", code):
            self.violations.append("scalar_burst_enabled must be exactly "
                                   "requested && statically eligible")
        if SWITCH_DEFINITION not in squash(self.source):
            self.violations.append("TAKEDOWN_SCALAR_BURST must enable the burst only when "
                                   'exactly "1": expected `%s`' % SWITCH_DEFINITION)

    def check_model_parent_boundary(self):
        """Keep native model preparation out of both native burst escape paths."""
        hooks = normalize(self.hook_code)
        parent, registry = normalize(MODEL_PARENT_HOOK), normalize(NATIVE_HOOK_LOOP)
        if hooks.count(parent) != 1 or hooks.count(registry) != 1 or \
                hooks.find(parent) > hooks.find(registry):
            self.violations.append("native model parent must be an outer-only hook marked "
                                   "before the native registry admits burst-native entries")
        writes = re.findall(r"scalar_burst_native_entries\s*\.\s*(\w+)\s*\(", self.code)
        if writes.count("add") != 1 or any(name not in ("add", "contains") for name in writes):
            self.violations.append("burst-native entries may be populated only by the "
                                   "reviewed native registry loop")
        top = normalize(self.code[self.loop_open + 1:self.loop_close])
        if not top.startswith("if(native_model_live)native_model_live->throw_if_failed();"):
            self.violations.append("native model terminal failure must remain the first "
                                   "outer-loop boundary check")

    def run(self):
        if not self.locate():
            return self
        self.parse_hooks()
        self.find_lambdas()
        self.pc_regions = [(self.loop_open, self.loop_close)] + self.reachable_lambda_bodies()
        for start, end, lo, hi in self.pc_occurrences():
            self.classify(start, end, lo, hi)
        occurrences = collections.Counter(
            key for kind, key in self.uses if kind == "reviewed comparison")
        for key, coverage in REVIEWED_COMPARISONS.items():
            if occurrences[key] not in (0, coverage["count"]):
                self.violations.append(
                    "the guest PC is compared (%s) with '%s' in %d places; REVIEWED_COMPARISONS "
                    "reviewed %d. Review the new use" % (key + (occurrences[key],
                                                                coverage["count"])))
        self.check_state_uses()
        self.trace()
        self.check_object_methods()
        self.check_shapes()
        self.check_required_terms()
        self.check_model_parent_boundary()
        return self


def analyze(source):
    return Analysis(source).run()


def model_failure_contract(source):
    """These two small methods are the semantic premise of skipping the outer poll.

    Pin the actual query and observer, rather than blessing any method named failed.
    The query must neither throw nor mutate, and must cover every terminal source
    the existing outer observer can report. Marker/burst execution is additionally
    exercised by the native live verifier; this is only the source-level premise.
    """
    code = strip_comments_and_literals(source)
    expected = {
        "bool ModelLive::failed() const noexcept":
            "return bool(impl_->failure) || impl_->failure_reason != nullptr;",
        "void ModelLive::throw_if_failed() const":
            "if (impl_->failure) std::rethrow_exception(impl_->failure); "
            "if (impl_->failure_reason) throw std::runtime_error(impl_->failure_reason);",
    }
    errors = []
    for declaration, body in expected.items():
        matches = list(re.finditer(re.escape(declaration) + r"\s*\{", code))
        if len(matches) != 1:
            errors.append("missing or changed terminal observer: " + declaration)
            continue
        begin = matches[0].end()
        if normalize(code[begin:matching_close(code, begin - 1)]) != normalize(body):
            errors.append("terminal observer no longer has the reviewed pure-latch semantics: "
                          + declaration)
    return errors


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
REGISTERED_PC = "kRetailMovieDecoderUpdate"  # any address the hook block marks
UNREVIEWED_WORK = "emulated_ee_cycles += 2U;"  # model-visible and in no review table


class ScalarBurstHookConsistency(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.source = MAIN_CPP.read_text(encoding="utf-8")
        cls.analysis = analyze(cls.source)

    def mutated(self, old, new, count=1):
        self.assertEqual(self.source.count(old), count, "mutation anchor changed: %r" % old)
        return analyze(self.source.replace(old, new)).violations

    def inject_at_loop_top(self, snippet):
        # Keep the terminal observer at the boundary: these mutations test
        # whether the following ordinary work is covered, not failure order.
        anchor = LOOP_HEAD + "\n        if (native_model_live) native_model_live->throw_if_failed();"
        return self.mutated(anchor, anchor + "\n" + snippet)

    def inject_lambda_and_use(self, definition, use):
        """A callable defined just before the loop and a statement at the loop top."""
        return self.mutated(LOOP_HEAD, definition + "\n" + LOOP_HEAD + "\n" + use)

    def assert_flags(self, violations, fragment):
        self.assertTrue(any(fragment in violation for violation in violations),
                        "expected a violation mentioning %r, got %r" % (fragment, violations))

    # ---- the real source ----
    def test_outer_loop_is_fully_accounted_for(self):
        self.assertEqual(self.analysis.violations, [])

    def test_expected_aliases_and_registrations_are_seen(self):
        # Guards against the parser silently finding nothing.
        self.assertTrue({"dispatch_pc", "candidate", "call_pc", "scalar_pc", "burst_pc"}
                        <= set(self.analysis.aliases), self.analysis.aliases)
        self.assertNotIn("candidate", self.analysis.exact_aliases)  # PC arithmetic
        self.assertGreaterEqual(len(self.analysis.singles), 40)
        self.assertEqual(len(self.analysis.ranges), 4)
        comparisons = [key for kind, key in self.analysis.uses if kind == "PC comparison"]
        self.assertGreaterEqual(len(comparisons), 60)
        for operand in ("kRetailControllerTransportPollReturn", "0x00437370U", "0U",
                        "milestone.address", "EeKernelHle::kGuestCallbackReturnAddress",
                        "kRetailVblankPollBegin", "0x0023719cU",
                        "DirtyDnasCompatHle::kStartTrampoline"):
            self.assertTrue(any(operand == key[1] for key in comparisons), operand)
        # Lambdas the loop can reach are classified too, not only the loop body.
        self.assertGreater(len(self.analysis.pc_regions), 20)
        self.assertGreaterEqual(len(self.analysis.top_level_statements()), 150)

    def test_trace_follows_the_ordinary_iteration(self):
        analysis = self.analysis
        self.assertTrue(analysis.reached_burst)
        self.assertTrue(analysis.reached_commit)
        # The walk goes through the bypass step and the commit tail, and into
        # the lambdas those statements call.
        for key in (STEP_LEAF, "++committed_steps;", "continue;",
                    "kernel.set_ee_cycle_count(emulated_ee_cycles);",
                    "memory.advance_vif1_dmac(cycles);"):
            self.assertEqual(analysis.run_counts[key], 1, key)
        self.assertTrue({"current_guest_input_timeline_boundary", "advance_profiled_vif1_dmac",
                         "advance_profiled_gif_dmac", "pace_audio_master",
                         "begin_profiled_host_phase", "current_ui_execution_context",
                         "observe_profiled_ee_span"} <= analysis.traced_lambda_names,
                        analysis.traced_lambda_names)
        # It never enters the system-call tail, the VBlank block or a hook body.
        self.assertNotIn("present_profiled_host_window", analysis.traced_lambda_names)
        self.assertNotIn("timed_graphics_scheduler_work_pending", analysis.traced_lambda_names)

    def test_review_tables_have_no_stale_entries(self):
        seen = {}
        for kind, key in self.analysis.uses:
            seen.setdefault(kind, set()).add(key)
        for kind, table in (("PC helper", PC_HELPERS), ("CPU state helper", STATE_HELPERS),
                            ("PC arithmetic", REVIEWED_ARITHMETIC),
                            ("PC store", REVIEWED_STORES), ("PC index", REVIEWED_INDEXES),
                            ("CPU state accessor", REVIEWED_STATE_METHODS),
                            ("reviewed comparison", REVIEWED_COMPARISONS),
                            ("reviewed range", REVIEWED_RANGES)):
            stale = sorted(set(table) - seen.get(kind, set()))
            self.assertEqual(stale, [], "%s entries no longer used by the loop" % kind)
        self.assertEqual(sorted(set(FACTS) - self.analysis.fact_uses), [],
                         "stale burst-boundary facts")
        for key, coverage in RUNS.items():
            self.assertEqual(self.analysis.run_counts[key], coverage["count"],
                             "RUNS entry reached a different number of times: %s" % key)
        self.assertEqual(sorted(set(HOST_ONLY_LAMBDAS) - self.analysis.host_only_uses), [],
                         "stale host-only lambdas")
        self.assertEqual(sorted(PURE_METHODS - self.analysis.pure_uses), [],
                         "stale pure method names")
        self.assertEqual(sorted(PLAIN_LOCAL_TYPES - self.analysis.local_type_uses), [],
                         "stale plain local types")
        for name, known in OBJECT_METHOD_NAMES.items():
            self.assertEqual(sorted(known - self.analysis.methods_seen[name]), [],
                             "stale %s method entries" % name)
        self.assertEqual(sorted(EXPECTED_SHAPES), sorted(self.analysis.shapes))

    def test_harness_flag_and_statistics_line(self):
        self.assertIn('"TAKEDOWN_SCALAR_BURST": "0"', HARNESS.read_text(encoding="utf-8"))
        self.assertIn('"TAKEDOWN_SCALAR_BURST_AUDIT": "0"', HARNESS.read_text(encoding="utf-8"))
        for piece in ('"  Scalar burst: enabled="', '" bursts="', '" instructions="',
                      '" stops={hook:"', '",horizon:"', '",hardware:"', '",result:"',
                      '",state:"'):
            self.assertIn(piece, self.source)

    def test_model_failure_observer_premise(self):
        source = (ROOT / "src/runtime/slus_21050_model_live.cpp").read_text(encoding="utf-8")
        self.assertEqual(model_failure_contract(source), [])
        old = "return bool(impl_->failure) || impl_->failure_reason!=nullptr;"
        self.assertEqual(source.count(old), 1)
        for changed in ("return false;", "return bool(impl_->failure);",
                        "return impl_->failure_reason!=nullptr;",
                        "throw_if_failed(); return false;"):
            self.assertTrue(model_failure_contract(source.replace(old, changed)), changed)

    def test_camera_registry_contains_both_polled_pcs(self):
        source = (ROOT / "src/runtime/slus_21050_camera_fov.cpp").read_text(encoding="utf-8")
        code = normalize(strip_comments_and_literals(source))
        self.assertIn(normalize(
            "Slus21050CameraFov::hook_addresses() noexcept { "
            "static constexpr std::array<guest::GuestAddress, 2> "
            "kAddresses{kFovUse, kUiSubmissionBegin}; return kAddresses; }"), code)

    # ---- registration: the check must have teeth ----
    def test_detects_unregistered_literal_comparison(self):
        violations = self.inject_at_loop_top(
            "        if (state.pc() == 0x00123456U) { ++committed_steps; }")
        self.assert_flags(violations, "'0x00123456U'")

    def test_detects_unregistered_named_comparison_through_alias(self):
        violations = self.mutated(
            "        memory.set_current_ee_pc(dispatch_pc);\n",
            "        memory.set_current_ee_pc(dispatch_pc);\n"
            "        if (dispatch_pc == kRetailFrontendPhase) { ++committed_steps; }\n")
        self.assert_flags(violations, "(==) with 'kRetailFrontendPhase'")

    def test_detects_new_alias(self):
        violations = self.inject_at_loop_top(
            "        const auto probe_pc = state.pc();\n"
            "        if (probe_pc == 0x00111110U) { ++committed_steps; }")
        self.assert_flags(violations, "'0x00111110U'")

    def test_detects_executed_instruction_pc_comparison(self):
        anchor = ("        stop = result;\n"
                  "        if (result.status != takedown::guest::EeStepStatus::GuestSystemCall) {")
        violations = self.mutated(
            anchor,
            "        if (result.instruction.pc == 0x00222220U) { ++committed_steps; }\n" + anchor)
        self.assert_flags(violations, "'0x00222220U'")
        # Registering the address does not help: the burst skips the tail of
        # the ordinary step before it, and that instruction may sit at a hook.
        tail = ("        if (result.status == takedown::guest::EeStepStatus::Executed) {\n"
                "            observe_profiled_ee_span(")
        violations = self.mutated(
            tail, "        if (result.instruction.pc == %s) { %s }\n%s"
            % (REGISTERED_PC, UNREVIEWED_WORK, tail))
        self.assert_flags(violations, "executed instruction's PC is compared (==)")
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        # The same holds for a PC captured before the step and tested after it.
        violations = self.mutated(
            tail, "        if (dispatch_pc == %s) { %s }\n%s"
            % (REGISTERED_PC, UNREVIEWED_WORK, tail))
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")

    def test_detects_unregistered_range(self):
        violations = self.inject_at_loop_top(
            "        if (state.pc() >= 0x00300000U && state.pc() < 0x00300100U) {}")
        self.assert_flags(violations, "(>=) with '0x00300000U'")
        self.assert_flags(violations, "(<) with '0x00300100U'")

    def test_detects_reversed_comparison(self):
        violations = self.inject_at_loop_top(
            "        if (0x00333330U == state.pc()) { ++committed_steps; }")
        self.assert_flags(violations, "'0x00333330U'")

    def test_detects_unreviewed_pc_helper(self):
        violations = self.inject_at_loop_top("        new_probe.wants(state.pc());")
        self.assert_flags(violations, "new_probe.wants")

    def test_detects_unreviewed_state_helper(self):
        violations = self.inject_at_loop_top("        new_probe.observe(state, memory);")
        self.assert_flags(violations, "new_probe.observe")

    def test_detects_unreviewed_state_accessor(self):
        violations = self.inject_at_loop_top(
            "        if (state.read_fpr_bits(0U) != 0U) { ++committed_steps; }")
        self.assert_flags(violations, "state.read_fpr_bits")

    def test_detects_unreviewed_arithmetic_store_and_index(self):
        self.assert_flags(self.inject_at_loop_top(
            "        if (((state.pc() >> 12U) & 1U) != 0U) { ++committed_steps; }"), ">> 12U")
        self.assert_flags(self.inject_at_loop_top("        last_seen_pc = state.pc();"),
                          "last_seen_pc")
        self.assert_flags(self.inject_at_loop_top("        ++pc_histogram[state.pc()];"),
                          "pc_histogram")

    def test_detects_new_kernel_or_memory_service(self):
        self.assert_flags(self.inject_at_loop_top("        kernel.service_timers(memory);"),
                          "kernel.service_timers()")
        self.assert_flags(self.inject_at_loop_top("        memory.advance_ipu_dmac(1U);"),
                          "memory.advance_ipu_dmac()")
        self.assert_flags(self.inject_at_loop_top("        spu2_renderer.drain_through(0U);"),
                          "spu2_renderer.drain_through()")

    def test_detects_removed_registration(self):
        violations = self.mutated("        hooks.mark_pc(kRetailMovieDecoderUpdate);\n", "")
        self.assert_flags(violations, "'kRetailMovieDecoderUpdate'")
        violations = self.mutated(
            "        hooks.mark_closed_range(0x00237180U, 0x0023719cU);\n", "")
        self.assert_flags(violations, "'0x00237180U'")
        violations = self.mutated("        hooks.mark_pc(0U);\n", "")
        self.assert_flags(violations, "kernel.finish_guest_thread")

    def test_detects_removed_registry_loop(self):
        violations = self.mutated(
            "        for (const auto& function : "
            "native_function_dispatcher.registered_functions()) {\n"
            "            if (!hooks.marked(function.address)) {\n"
            "                scalar_burst_native_entries.add(function.address);\n"
            "            }\n"
            "            hooks.mark_pc(function.address);\n"
            "        }\n", "")
        self.assert_flags(violations, NATIVE_REGISTRY)

    def test_detects_native_model_parent_reentering_burst(self):
        parent = "        " + MODEL_PARENT_HOOK + "\n"
        self.assert_flags(self.mutated(parent, ""), "outer-only hook")
        registry_end = "            hooks.mark_pc(function.address);\n        }\n"
        self.assertEqual(self.source.count(registry_end), 1)
        late = self.source.replace(parent, "").replace(registry_end, registry_end + parent)
        self.assert_flags(analyze(late).violations, "outer-only hook")
        self.assert_flags(self.mutated(parent, parent +
            "        scalar_burst_native_entries.add(0x0021d2e0U);\n"),
            "burst-native entries may be populated only")

    def test_detects_native_model_failure_boundary_changes(self):
        guard = ("        if (native_model_live && native_model_live->failed()) {\n"
                 "            return false;\n        }\n")
        shape = "the start predicate no longer matches its reviewed shape"
        self.assert_flags(self.mutated(guard, ""), shape)
        self.assert_flags(self.mutated(guard, guard.replace("return false;", "return true;")),
                          shape)
        self.assert_flags(self.mutated(guard, guard.replace("failed()", "throw_if_failed()")),
                          shape)
        throw = "        if (native_model_live) native_model_live->throw_if_failed();\n"
        self.assert_flags(self.mutated(throw, ""), "first outer-loop boundary check")

    def test_detects_removed_instance_capture_eligibility(self):
        self.assert_flags(self.mutated("        !instance_batch_capture.enabled() &&\n", ""),
                          "!instance_batch_capture.enabled()")

    def test_detects_removed_camera_registry(self):
        loop = ("        for (const auto address : "
                "takedown::runtime::Slus21050CameraFov::hook_addresses()) {\n"
                "            hooks.mark_pc(address);\n        }\n")
        self.assert_flags(self.mutated(loop, ""), CAMERA_FOV_REGISTRY)

    def test_detects_registration_outside_the_block(self):
        violations = self.inject_at_loop_top("        late_filter.mark_pc(0x00100000U);")
        self.assert_flags(violations, "outside the SCALAR-BURST-HOOKS block")

    # ---- `!=` and the executed PC are not covered by registration ----
    def test_detects_not_equal_against_registered_address(self):
        violations = self.inject_at_loop_top(
            "        if (state.pc() != kRetailVblankPollBranch) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "compared (!=) with 'kRetailVblankPollBranch'")
        # Independently of the review rule, the trace sees the work run.
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        # A `!=` that is already reviewed for one place is not reviewed for another.
        violations = self.inject_at_loop_top(
            "        if (state.pc() != takedown::runtime::slus_21050::"
            "VifPacketRingFlushClaimTracker::kFlushEntryAddress) {}")
        self.assert_flags(violations, "kFlushEntryAddress' in 2 places")
        # Negating an equality is the same thing.
        violations = self.inject_at_loop_top(
            "        if (!(state.pc() == kRetailVblankPollBranch)) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.inject_at_loop_top(
            "        if (state.pc() == kRetailVblankPollBranch) {} else { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.inject_at_loop_top(
            "        const bool at_poll = state.pc() == kRetailVblankPollBranch;\n"
            "        if (!at_poll) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")

    # ---- range bounds are validated as pairs ----
    def test_detects_one_sided_range_bounds(self):
        violations = self.inject_at_loop_top(
            "        if (state.pc() >= kRetailVblankPollBegin && "
            "memory.read_u8(0x00400000U) != 0U) { memory.write_u8(0x00400000U, 0U); }")
        self.assert_flags(violations, "bound (>=) with 'kRetailVblankPollBegin' is not part of")
        self.assert_flags(violations, "memory.write_u8(0x00400000U,0U);")
        violations = self.inject_at_loop_top(
            "        if (state.pc() < takedown::recompiler::JoinPumpRegion::"
            "kCallbackTailEffectEndAddress && state.read_gpr_u32(4U) == 1U) "
            "{ memory.write_u32_le(0x00400000U, 0U); }")
        self.assert_flags(
            violations, "bound (<) with 'JoinPumpRegion::kCallbackTailEffectEndAddress'")
        self.assert_flags(violations, "memory.write_u32_le(0x00400000U,0U);")

    def test_detects_mixed_and_widened_ranges(self):
        # The begin of one registered range with the end of another.
        violations = self.inject_at_loop_top(
            "        if (state.pc() >= kRetailVblankPollBegin && "
            "state.pc() <= kRetailVblankCallbackPollEnd) { ++freeze_spin_entries; }")
        self.assert_flags(violations, "bound (>=) with 'kRetailVblankPollBegin'")
        self.assert_flags(violations, "bound (<=) with 'kRetailVblankCallbackPollEnd'")
        self.assert_flags(violations, "++freeze_spin_entries;")
        # The freeze probe widened to the end of another range.
        violations = self.mutated(
            "        if (state.pc() >= 0x00237180U && state.pc() <= 0x0023719cU) {",
            "        if (state.pc() >= 0x00237180U && state.pc() <= kRetailVblankPollEnd) {")
        self.assert_flags(violations, "bound (>=) with '0x00237180U'")
        self.assert_flags(violations, "++freeze_spin_entries;")
        # Closed where the registration is half-open, and bounds joined by `||`.
        violations = self.mutated(
            "            dispatch_pc < join_pump_candidate_end;",
            "            dispatch_pc <= join_pump_candidate_end;")
        self.assert_flags(violations, "bound (<=) with 'join_pump_candidate_end'")
        violations = self.inject_at_loop_top(
            "        if (state.pc() >= kRetailVblankPollBegin || "
            "state.pc() <= kRetailVblankPollEnd) { ++freeze_spin_entries; }")
        self.assert_flags(violations, "bound (>=) with 'kRetailVblankPollBegin'")
        self.assert_flags(violations, "++freeze_spin_entries;")
        # Bounds on two different PCs do not form a range either.
        violations = self.mutated(
            "        if (state.pc() >= 0x00237180U && state.pc() <= 0x0023719cU) {",
            "        if (state.pc() >= 0x00237180U && result.instruction.pc <= 0x0023719cU) {")
        self.assert_flags(violations, "bound (>=) with '0x00237180U'")

    def test_accepts_work_gated_by_a_registered_pc_or_range(self):
        self.assertEqual(self.inject_at_loop_top(
            "        if (state.pc() == %s && memory.read_u8(0x00400000U) != 0U) "
            "{ %s }" % (REGISTERED_PC, UNREVIEWED_WORK)), [])
        self.assertEqual(self.inject_at_loop_top(
            "        if (state.pc() >= kRetailVblankPollBegin && "
            "state.pc() <= kRetailVblankPollEnd) { %s }" % UNREVIEWED_WORK), [])
        self.assertEqual(self.inject_at_loop_top(
            "        const bool at_new_use = auto_start && state.pc() == %s;\n"
            "        if (at_new_use) { %s }" % (REGISTERED_PC, UNREVIEWED_WORK)), [])
        self.assertEqual(self.inject_at_loop_top(
            "        if (retail_trace) { %s }" % UNREVIEWED_WORK), [])

    # ---- lambdas defined before the loop ----
    def test_detects_pc_hook_in_a_lambda_defined_before_the_loop(self):
        violations = self.inject_lambda_and_use(
            "    const auto at_new_hook = [&] { return state.pc() == 0x00123456U; };",
            "        if (at_new_hook()) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "(==) with '0x00123456U'")
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        # A registered address inside the lambda does not make the caller a hook.
        violations = self.inject_lambda_and_use(
            "    const auto at_known_hook = [&] { return state.pc() == %s; };" % REGISTERED_PC,
            "        if (at_known_hook()) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")

    def test_detects_register_poll_in_a_lambda_defined_before_the_loop(self):
        violations = self.inject_lambda_and_use(
            "    const auto polls_register = [&] { return state.read_gpr_u32(4U) == 0x1234U; };",
            "        if (polls_register()) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.inject_lambda_and_use(
            "    const auto service_new_device = [&] { new_device.tick(emulated_ee_cycles); };",
            "        service_new_device();")
        self.assert_flags(violations, "new_device.tick(emulated_ee_cycles);")

    def test_detects_work_added_to_a_lambda_the_loop_calls(self):
        # begin_profiled_host_phase() runs at every boundary.
        anchor = "        [&](const takedown::runtime::EeHostPhase host_phase) {\n"
        violations = self.mutated(
            anchor, anchor + "            if (state.read_gpr_u32(4U) == 1U) { %s }\n"
            % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.mutated(
            anchor, anchor + "            if (state.pc() == 0x00123456U) { ++committed_steps; }\n")
        self.assert_flags(violations, "(==) with '0x00123456U'")
        # A statement the burst replays once must not run twice per boundary.
        violations = self.mutated(anchor, anchor + "            ++committed_steps;\n")
        self.assert_flags(violations, "executes '++committed_steps;' 2 times")
        # The host-only pacing lambda is pinned.
        anchor = "    const auto pace_audio_master = [&]() {\n"
        violations = self.mutated(
            anchor, anchor + "        if (state.pc() == %s) { return; }\n" % REGISTERED_PC)
        self.assert_flags(violations, "body of host-only lambda pace_audio_master changed")
        self.assert_flags(violations, "names guest-visible state 'state'")
        # A lambda only hook bodies call is not walked, but its PC tests are checked.
        anchor = "    const auto timed_graphics_scheduler_work_pending = [&memory]() {\n"
        violations = self.mutated(
            anchor, anchor + "        if (state.pc() == 0x00123456U) { return true; }\n")
        self.assert_flags(violations, "(==) with '0x00123456U'")

    # ---- per-iteration work that is not a PC test ----
    def test_detects_new_per_iteration_service(self):
        violations = self.inject_at_loop_top(
            "        new_timer_model.service(emulated_ee_cycles);")
        self.assert_flags(violations, "new_timer_model.service(emulated_ee_cycles);")
        violations = self.inject_at_loop_top(
            "        const auto serviced = new_timer_model.service(emulated_ee_cycles);")
        self.assert_flags(violations, "calls new_timer_model.service()")
        violations = self.inject_at_loop_top(
            "        for (auto& device : new_devices) { device.tick(); }")
        self.assert_flags(violations, "for(auto&device:new_devices)")

    def test_detects_new_iteration_counted_gate(self):
        violations = self.inject_at_loop_top(
            "        if (++new_poll_counter >= 100U) "
            "{ new_poll_counter = 0U; emulated_ee_cycles += 1U; }")
        self.assert_flags(violations, "modifies state")
        self.assert_flags(violations, "emulated_ee_cycles+=1U;")
        # Evaluation order matters: the increment runs before the PC test.
        violations = self.inject_at_loop_top(
            "        if (++new_poll_counter >= 100U && state.pc() == %s) {}" % REGISTERED_PC)
        self.assert_flags(violations, "modifies state")
        violations = self.inject_at_loop_top(
            "        if (new_probe.tick() && state.pc() == %s) {}" % REGISTERED_PC)
        self.assert_flags(violations, "calls new_probe.tick()")
        violations = self.inject_at_loop_top(
            "        if (contains(emulated_ee_cycles) && state.pc() == %s) {}" % REGISTERED_PC)
        self.assert_flags(violations, "calls contains()")
        # After the PC test it never runs at a skipped boundary.
        self.assertEqual(self.inject_at_loop_top(
            "        if (state.pc() == %s && new_probe.tick()) {}" % REGISTERED_PC), [])

    def test_detects_new_condition_on_polled_state(self):
        violations = self.inject_at_loop_top(
            "        if (kernel.current_thread_id() == 3) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.inject_at_loop_top(
            "        if (memory.read_u32_le(0x00400000U) != last_polled_word) "
            "{ last_polled_word = 0U; }")
        self.assert_flags(violations, "last_polled_word=0U;")
        violations = self.inject_at_loop_top(
            "        if (state.pc() == %s || memory.read_u8(0x00400000U) != 0U) "
            "{ %s }" % (REGISTERED_PC, UNREVIEWED_WORK))
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        violations = self.inject_at_loop_top(
            "        if (state.read_gpr_u32(4U) == 0x00001234U) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")

    def test_detects_pc_values_that_are_not_the_current_pc(self):
        # Only state.pc(), or a const copy of it taken before the step, is the
        # PC the hook filter tested. Anything else may equal a marked address.
        for declaration in ("auto probe_pc = state.pc();\n        probe_pc += 4U;",
                            "const auto probe_pc = state.pc() + 4U;",
                            "const auto probe_pc = other_cpu.pc();"):
            violations = self.inject_at_loop_top(
                "        %s\n        if (probe_pc == %s) { %s }"
                % (declaration, REGISTERED_PC, UNREVIEWED_WORK))
            self.assert_flags(violations, "emulated_ee_cycles+=2U;")
        # The const copy is the current PC, and the same gate is then accepted.
        self.assertEqual(self.inject_at_loop_top(
            "        const auto probe_pc = state.pc();\n        if (probe_pc == %s) { %s }"
            % (REGISTERED_PC, UNREVIEWED_WORK)), [])
        violations = self.inject_at_loop_top(
            "        if (other_cpu.pc() >= kRetailVblankPollBegin && "
            "other_cpu.pc() <= kRetailVblankPollEnd) { %s }" % UNREVIEWED_WORK)
        self.assert_flags(violations, "emulated_ee_cycles+=2U;")

    def test_detects_side_effecting_local(self):
        violations = self.inject_at_loop_top("        NewTimerTick tick;")
        self.assert_flags(violations, "constructs a 'NewTimerTick'")
        violations = self.inject_at_loop_top("        NewTimerTick tick{emulated_ee_cycles};")
        self.assert_flags(violations, "constructs a 'NewTimerTick'")
        violations = self.inject_at_loop_top("        NewTimerTick tick(emulated_ee_cycles);")
        self.assert_flags(violations, "NewTimerTick tick(emulated_ee_cycles);")
        violations = self.inject_at_loop_top("        const auto ticked = ++new_poll_counter;")
        self.assert_flags(violations, "modifies state")

    def test_detects_new_writer_of_a_variable_a_fact_relies_on(self):
        violations = self.mutated(
            "        native_stage_phase.finish();\n",
            "        native_stage_phase.finish();\n"
            "        compiled_execution_boundary = result;\n")
        self.assert_flags(violations, "assigning 'compiled_execution_boundary' in exactly 2")
        violations = self.mutated(
            "        native_stage_phase.finish();\n",
            "        native_stage_phase.finish();\n"
            "        native_dispatch = native_function_dispatcher.dispatch(\n"
            "            native_preparation, state, memory);\n")
        self.assert_flags(violations, "assigning 'native_dispatch' in exactly 1")

    def test_detects_replayed_statement_run_twice(self):
        violations = self.inject_at_loop_top("        host_window_loop_iterations += host_window_pump_scale();")
        self.assert_flags(violations, "executes 'host_window_loop_iterations+=host_window_pump_scale();' 2 times")
        violations = self.inject_at_loop_top("        continue;")
        self.assert_flags(violations, "does not reach the burst")

    def test_detects_preprocessor_conditional_in_boundary_code(self):
        violations = self.inject_at_loop_top(
            "#if defined(TAKEDOWN_SOME_OPTION)\n        %s\n#endif" % UNREVIEWED_WORK)
        self.assert_flags(violations, "preprocessor conditional")

    def test_detects_changed_mirrored_poll(self):
        violations = self.mutated(
            "        if (rwa_staging_samples.size() < 8U &&\n            memory.contains(0x4e2ce8U, 8U)) {",
            "        if (rwa_staging_samples.size() < 16U &&\n            memory.contains(0x4e2ce8U, 8U)) {")
        self.assert_flags(violations, "staging_pointer!=rwa_staging_last_pointer")
        violations = self.mutated("const auto* const word = ram + 0x004e2ce8U;",
                                  "const auto* const word = ram + 0x004e2cecU;")
        self.assert_flags(violations, "ram + 0x004e2ce8U")
        self.assert_flags(violations, "RAM watch fires no longer matches its reviewed shape")
        violations = self.mutated(
            "            memory.read_u16_le(kRetailControllerAcceptEdge) != 0U) {",
            "            memory.read_u16_le(kRetailControllerAcceptEdge) == 2U) {")
        self.assert_flags(violations, "virtual_ds2_trace->action_edge_step=committed_steps;")

    def test_detects_lost_justification(self):
        # The SPU2 pump is exact only while the horizon bounds it.
        self.assert_flags(self.mutated("spu2_renderer.next_sample_cycle()", "0U"),
                          "burst-boundary statement 'spu2_renderer.pump_due(emulated_ee_cycles);'")
        self.assert_flags(self.mutated("        if (kernel.sif_command_callback_poll_can_progress()) {\n"
                                       "            return false;\n        }\n", ""),
                          "kernel.sif_command_callback_poll_can_progress()")

    # ---- the burst machinery is pinned statement by statement ----
    def test_detects_weakened_per_step_checks(self):
        shape = "the burst no longer matches its reviewed shape"
        self.assert_flags(self.mutated(
            "                                if (scalar_burst_hooks.marked(burst_pc) ||",
            "                                if (false ||"), shape)
        self.assert_flags(self.mutated(
            "                                if (burst_ram_watch.fires()) {\n"
            "                                    // A polled guest RAM word changed.",
            "                                if (false) {\n"
            "                                    // A polled guest RAM word changed."),
            shape)
        self.assert_flags(self.mutated(
            "                                    if (!scalar_burst_may_start()) {\n"
            "                                        burst_stop = takedown::runtime::ScalarBurstStop::State;",
            "                                    if (false) {\n"
            "                                        burst_stop = takedown::runtime::ScalarBurstStop::State;"),
            shape)
        self.assert_flags(self.mutated(
            "                                if (burst_pending &&\n"
            "                                    result.status != takedown::guest::EeStepStatus::Executed) {\n"
            "                                    // System call or fault",
            "                                if (false) {\n"
            "                                    // System call or fault"), shape)
        self.assert_flags(self.mutated(
            "memory.hardware_activity_epoch() != burst_hardware_activity ||", "false ||"), shape)
        self.assert_flags(self.mutated(
            "                                    if (!quiescent || horizon != burst_budget) {",
            "                                    if (!quiescent) {"), shape)

    def test_detects_missing_per_step_work(self):
        violations = self.mutated(
            "                                kernel.set_ee_cycle_count(emulated_ee_cycles);\n", "")
        self.assert_flags(violations, "kernel.set_ee_cycle_count(emulated_ee_cycles);")
        self.assert_flags(violations, "the burst no longer matches its reviewed shape")
        violations = self.mutated(
            "                                        burst_stop = takedown::runtime::ScalarBurstStop::Hook;\n"
            "                                        break;\n"
            "                                    }\n"
            "                                }\n"
            "                                if (scalar_burst_audit",
            "                                        burst_stop = takedown::runtime::ScalarBurstStop::Hook;\n"
            "                                    }\n"
            "                                }\n"
            "                                if (scalar_burst_audit")
        self.assert_flags(violations, "the burst no longer matches its reviewed shape")

    def test_detects_weakened_start_conditions(self):
        shape = "the burst no longer matches its reviewed shape"
        self.assert_flags(self.mutated(
            "if (!scalar_burst_hooks.marked(burst_pc) && scalar_burst_may_start()) {",
            "if (scalar_burst_may_start()) {"), shape)
        self.assert_flags(self.mutated(
            "if (burst_budget != 0U && !burst_ram_watch.fires() &&",
            "if (burst_budget != 0U &&"), shape)

    def test_detects_weakened_start_predicate(self):
        shape = "the start predicate no longer matches its reviewed shape"
        self.assert_flags(self.mutated("        if (!memory.graphics_dmacs_idle()) {\n"
                                       "            return false;\n        }\n",
                                       "        if (!memory.graphics_dmacs_idle()) {\n"
                                       "        }\n"), shape)
        self.assert_flags(self.mutated("        if (!memory.graphics_dmacs_idle()) {\n"
                                       "            return false;\n        }\n", ""), shape)
        self.assert_flags(self.mutated(
            "        return memory.ee_memory_write_observer() == nullptr;",
            "        return true;"), shape)

    def test_detects_weakened_horizon(self):
        shape = "the horizon no longer matches its reviewed shape"
        self.assert_flags(self.mutated(
            "            if (const auto deadline = kernel.next_alarm_deadline(); deadline.has_value()) {\n"
            "                horizon =\n"
            "                    std::min(horizon, scalar_burst_boundaries_before(first_cycle, *deadline));\n"
            "            }\n",
            "            if (const auto deadline = kernel.next_alarm_deadline(); deadline.has_value()) {\n"
            "            }\n"), shape)
        self.assert_flags(self.mutated(
            "scalar_burst_boundaries_before(first_cycle, next_vblank_start)",
            "scalar_burst_boundaries_before(first_step, next_vblank_start)"), shape)

    def test_detects_weakened_eligibility(self):
        violations = self.mutated(
            "ee_pc_hit_watch.empty() && !object_packet_capture.enabled() &&",
            "(ee_pc_hit_watch.empty() || true) && !object_packet_capture.enabled() &&")
        self.assert_flags(violations, "eligibility predicate lost the term 'ee_pc_hit_watch.empty()'")
        self.assert_flags(violations, "unreviewed conjunct '(ee_pc_hit_watch.empty()||true)'")
        self.assert_flags(self.mutated("femain_terminal_diagnostic == nullptr && "
                                       "!femain_park_pcs.has_value() &&",
                                       "femain_terminal_diagnostic == nullptr &&"),
                          "!femain_park_pcs.has_value()")

    def test_detects_new_scalar_observer_not_restated(self):
        violations = self.mutated(
            "        femain_retail_consume_observation_adapter != nullptr;\n"
            "    std::uint64_t scalar_observer_bypass_steps{};",
            "        femain_retail_consume_observation_adapter != nullptr ||\n"
            "        brand_new_tracer != nullptr;\n"
            "    std::uint64_t scalar_observer_bypass_steps{};")
        self.assert_flags(violations, "brand_new_tracer == nullptr")


class LexicalHelpers(unittest.TestCase):
    def test_strip_keeps_offsets_and_digit_separators(self):
        text = 'a = 4\'915\'200U; // state.pc() == 1\nb = "state.pc()"; c = \'x\'; /* d */ e'
        stripped = strip_comments_and_literals(text)
        self.assertEqual(len(stripped), len(text))
        self.assertIn("4'915'200U", stripped)
        self.assertNotIn("state.pc()", stripped)
        self.assertTrue(stripped.rstrip().endswith("e"))

    def test_normalize_drops_namespaces_and_insignificant_spaces(self):
        self.assertEqual(
            normalize("takedown::runtime::slus_21050::\n  VifPacketRingFlushClaimTracker::kFlushEntryAddress"),
            "VifPacketRingFlushClaimTracker::kFlushEntryAddress")
        self.assertEqual(normalize("takedown::guest::EeKernelHle::kGuestCallbackReturnAddress"),
                         "EeKernelHle::kGuestCallbackReturnAddress")
        self.assertEqual(normalize("const   takedown::runtime::Thing*\n  name = a  &&  b ;"),
                         "const Thing*name=a&&b;")

    def test_callee_and_operands(self):
        code = "x = foo.bar(a, state.pc()) && takedown::f<int>(state.pc() + 4U) == q;"
        first = code.index("state.pc()")
        self.assertEqual(callee_before(code, enclosing_open(code, first, 0)), "foo.bar")
        second = code.index("state.pc()", first + 1)
        self.assertEqual(callee_before(code, enclosing_open(code, second, 0)),
                         "takedown::f<int>")
        self.assertEqual(comparison_after(code, code.index(") ==") + 1)[0], "==")
        self.assertEqual(normalize(operand_right(code, code.index("== q") + 2, len(code))), "q")

    def test_native_only_arms_are_resolved(self):
        code = ("a;\n#if defined(TAKEDOWN_NATIVE_ONLY_EXECUTABLE)\nthrow x;\n#else\nb;\n#endif\n"
                "#if defined(OTHER)\nc;\n#endif\nd;\n")
        resolved = resolve_native_only(code)
        self.assertEqual(len(resolved), len(code))
        self.assertNotIn("throw", resolved)
        self.assertNotIn(NATIVE_ONLY, resolved)
        self.assertIn("b;", resolved)
        self.assertIn("#if defined(OTHER)", resolved)  # unrelated conditionals stay visible

    def test_statement_shapes(self):
        analysis = Analysis("void f() { if (a) [[unlikely]] { b(); } else if (c) d(); "
                            "for (;;) { e = [&](int v) { g(v); h(); }; break; } "
                            "while (i) { j(); } k{}; }")
        code = analysis.code
        body = code.index("{")
        shape = analysis.shape(analysis.statements_in(body + 1, matching_close(code, body)))
        self.assertEqual(render_shape(shape), [
            "if (a)", "    b();", "else", "    if (c)", "        d();",
            "for (;;)", "    e=[&](int v){g(v);h();};", "    break;",
            "while (i)", "    j();", "k{};"])


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
