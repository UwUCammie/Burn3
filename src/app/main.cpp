// RECOVERY: reconstructed from the GIMPLE of the release object (runtime/setup/objects/main.cpp.obj, built
// from 0.0.24 sources with TAKEDOWN_DISTRIBUTION_BUILD=1) with tools/gimple.py on the recovery drive.
// Most functions compare identical; run_boot_entry, main, a few lambdas and some std:: instantiations
// still differ in inlining and lambda numbering only (see RECOVERY_MANIFEST.tsv). Behaviour was checked
// against the archived object by running boot-entry with --parallel-gs-stream for 1e9 guest steps:
// the GS stream and the statistics are identical. Version text comes from the generated version.hpp.
#include "takedown/version.hpp"
#include "takedown/audio/host_audio_sink.hpp"
#include "takedown/audio/iop_r3000_executor.hpp"
#include "takedown/audio/rwa_iop_shadow_runtime.hpp"
#include "takedown/audio/rwa_spu2_boot.hpp"
#include "takedown/audio/rwa_spu2_voice.hpp"
#include "takedown/audio/spu2_core.hpp"
#include "takedown/audio/spu2_renderer.hpp"
#include "takedown/config/app_settings.hpp"
#include "takedown/config/settings_store.hpp"
#include "takedown/content/disc_path_resolver.hpp"
#include "takedown/content/elf_loader.hpp"
#include "takedown/content/elf_static_analysis.hpp"
#include "takedown/content/importer.hpp"
#include "takedown/content/mod_manager.hpp"
#include "takedown/content/mod_overlay.hpp"
#include "takedown/guest/dirty_dnas_compat.hpp"
#include "takedown/guest/ee_aot.hpp"
#include "takedown/guest/ee_interpreter.hpp"
#include "takedown/guest/ee_kernel.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_local_memory.hpp"
#include "takedown/guest/gs_privileged_display.hpp"
#include "takedown/guest/ipu_mpeg2_decoder.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/retail_ds2.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"
#include "takedown/guest/vu1_avx512_arithmetic.hpp"
#include "takedown/guest/vu1_micro_interpreter.hpp"
#include "takedown/guest/vu1_simd_arithmetic.hpp"
#include "takedown/guest/vu1_static_program.hpp"
#include "takedown/net/matchmaking.hpp"
#include "takedown/platform/gs_semantic_compare.hpp"
#include "takedown/platform/gs_semantic_trace.hpp"
#include "takedown/platform/host_wait_stats.hpp"
#include "takedown/platform/input_bindings.hpp"
#include "takedown/platform/parallel_gs_completion_reader.hpp"
#include "takedown/platform/parallel_gs_input.hpp"
#include "takedown/platform/parallel_gs_stream.hpp"
#include "takedown/platform/native_cmd_emitter.hpp" // NS-3
#include "takedown/platform/pcsx2_gs_dump.hpp"
#include "takedown/platform/pcsx2_gs_savestate.hpp"
#include "takedown/platform/pcsx2_pine.hpp"
#include "takedown/platform/precise_sleep.hpp"
#include "takedown/platform/process.hpp"
#include "takedown/platform/release_diagnostics.hpp"
#include "takedown/platform/window.hpp"
#include "takedown/presentation/graphics_options.hpp"
#include "takedown/presentation/gs_capture_coordinator.hpp"
#include "takedown/presentation/gs_display_policy.hpp"
#include "takedown/presentation/gs_renderer_worker.hpp"
#include "takedown/presentation/gs_scanout.hpp"
#include "takedown/presentation/mods_page_ui.hpp"
#include "takedown/presentation/presentation_pacer.hpp"
#include "takedown/presentation/rolling_fps.hpp"
#include "takedown/presentation/stats_overlay.hpp"
#include "takedown/recompiler/join_pump_region.hpp"
#include "takedown/recompiler/r5900_compiled_block_lifter.hpp"
#include "takedown/recompiler/r5900_compiled_block_module_builder.hpp"
#include "takedown/recompiler/r5900_cpp_lifter.hpp"
#include "takedown/recompiler/r5900_function_cfg.hpp"
#include "takedown/recompiler/r5900_leaf_module_builder.hpp"
#include "takedown/recompiler/r5900_word_extractor.hpp"
#include "takedown/recompiler/vu1_program_module_builder.hpp"
#include "takedown/runtime/compiled_block_dispatcher.hpp"
#include "takedown/runtime/compiled_budget_profiler.hpp"
#include "takedown/runtime/direct_instruction_progress.hpp"
#include "takedown/runtime/ee_dispatch_preparation_cache.hpp"
#include "takedown/runtime/ee_graphics_service_diagnostics.hpp"
#include "takedown/runtime/ee_host_phase_profiler.hpp"
#include "takedown/runtime/ee_pc_sampler.hpp"
#include "takedown/runtime/frontend_input_replay.hpp"
#include "takedown/runtime/graphics_async.hpp"
#include "takedown/runtime/guest_input_timeline.hpp"
#include "takedown/runtime/guest_work_measurement_window.hpp"
#include "takedown/runtime/inert_dmac_poll.hpp"
#include "takedown/runtime/join_pump_graphics_deadline.hpp"
#include "takedown/runtime/media_parity_audit.hpp"
#include "takedown/runtime/native_call_capture.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/native_function_preflight.hpp"
#include "takedown/runtime/native_simulation_shadow.hpp"
#include "takedown/runtime/retail_reference_timer.hpp"
#include "takedown/runtime/retail_timer_consumer_trace.hpp"
#include "takedown/runtime/retail_timer_owner_abi_trace.hpp"
#include "takedown/runtime/scalar_burst.hpp"
#include "takedown/runtime/slus_21050_added_cars.hpp"
#include "takedown/runtime/slus_21050_boot_volume.hpp"
#include "takedown/runtime/slus_21050_camera_fov.hpp"
#include "takedown/runtime/slus_21050_car_select.hpp"
#include "takedown/runtime/slus_21050_collision_trace.hpp"
#include "takedown/runtime/slus_21050_controllers.hpp"
#include "takedown/runtime/slus_21050_developer_unlock.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"
#include "takedown/runtime/slus_21050_femain_bridge.hpp"
#include "takedown/runtime/slus_21050_femain_dynamic_callback_probe.hpp"
#include "takedown/runtime/slus_21050_femain_frame_closure_diagnostic.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_producer.hpp"
#include "takedown/runtime/slus_21050_femain_live_terminal_authority.hpp"
#include "takedown/runtime/slus_21050_femain_live_terminal_coordinator.hpp"
#include "takedown/runtime/slus_21050_femain_live_transaction.hpp"
#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"
#include "takedown/runtime/slus_21050_femain_parallel_gs_completion_ledger.hpp"
#include "takedown/runtime/slus_21050_femain_park_diagnostic.hpp"
#include "takedown/runtime/slus_21050_femain_provenance_window.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_invocation_barrier.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_observation_adapter.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_probe.hpp"
#include "takedown/runtime/slus_21050_femain_terminal_producer_diagnostic.hpp"
#include "takedown/runtime/slus_21050_frame_rate.hpp"
#include "takedown/runtime/slus_21050_graphics_options.hpp"
#include "takedown/runtime/slus_21050_instance_batch_capture.hpp"
#include "takedown/runtime/slus_21050_menu_mouse.hpp"
#include "takedown/runtime/slus_21050_model_live.hpp"
#include "takedown/runtime/slus_21050_model_submission_probe.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"
#include "takedown/runtime/slus_21050_native_cars.hpp"
#include "takedown/runtime/slus_21050_native_functions.hpp"
#include "takedown/runtime/slus_21050_native_pc_options.hpp"
#include "takedown/runtime/slus_21050_online_count.hpp"
#include "takedown/runtime/slus_21050_settings_page.hpp"
#include "takedown/runtime/slus_21050_object_packet_capture.hpp"
#include "takedown/runtime/slus_21050_object_submission_adapter.hpp"
#include "takedown/runtime/slus_21050_online.hpp"
#include "takedown/runtime/slus_21050_render_state_packet.hpp"
#include "takedown/runtime/slus_21050_scenery_detail.hpp"
#include "takedown/runtime/slus_21050_spark_trace.hpp"
#include "takedown/runtime/slus_21050_strip_submission.hpp"
#include "takedown/runtime/slus_21050_ui_draw_packet_session.hpp"
#include "takedown/runtime/slus_21050_ui_draw_session_feed.hpp"
#include "takedown/runtime/slus_21050_ui_text_vif_transport.hpp"
#include "takedown/runtime/slus_21050_vif_packet_ring_flush_claim_tracker.hpp"
#include "takedown/runtime/subframe_input.hpp"
#include "takedown/runtime/variable_frame_clock.hpp"
#include "takedown/runtime/vif1_replay.hpp"
#include "takedown/runtime/vu1_worker.hpp"
#include "takedown/save/card_import.hpp"
#include "takedown/save/profile_slots.hpp"
#include "takedown/save/save_card_device.hpp"
#include "takedown/save/save_data.hpp"
#include "takedown/ui/control_style.hpp"
#include "takedown/ui/controls_editor.hpp"
#include "takedown/ui/developer_unlock.hpp"
#include "takedown/ui/host_prompt.hpp"
#include "takedown/ui/mod_sync_session.hpp"
#include "takedown/ui/pc_options_session.hpp"
#include "takedown/ui/save_slot_menu.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <io.h>

namespace takedown::app {

// Host-side counters sampled at the guest-work measurement boundaries; the
// printer lives in run_diagnostics.cpp.
struct GuestWorkHostSnapshot {
    platform::HostCpuSnapshot cpu{};
    std::uint64_t audio_wait_nanoseconds{};
    std::uint64_t gs_write_nanoseconds{};
    std::uint64_t summarized_instructions{};
    std::array<std::uint64_t, 5> native_prepared_families{};
    std::uint64_t native_submission_jobs{};
    std::uint64_t native_execution_jobs{};
    std::uint64_t native_submission_packets{};
    std::uint64_t native_submission_quadwords{};
    runtime::slus_21050::ModelLiveStatistics model{};
    runtime::slus_21050::ModelProducerStatistics model_producer{};
    runtime::slus_21050::ModelLiveGeometryReuseStatistics model_reuse{};
    runtime::slus_21050::ModelGeometryExecutorStatistics model_workers{};
    std::uint64_t vu1_aot_pairs{};
    std::uint64_t vu1_aot_launches{};
    std::uint64_t vu1_aot_completed{};
};

void print_guest_work_host_statistics(const GuestWorkHostSnapshot& first,
                                      const GuestWorkHostSnapshot& last,
                                      bool model_enabled);
void print_native_model_statistics(const runtime::slus_21050::ModelLive& native_model_live);

} // namespace takedown::app

namespace {

using takedown::guest::GuestAddress;

// The FEMain source producer owns the primary GS command stream itself; the
// renderer-facing observer slot only needs a sink that accepts every command.
class FEmainSourcePrimaryCommandSink final : public takedown::guest::GsCommandObserver {
  public:
    void on_gs_register_write(const takedown::guest::GsRegisterWrite&) override {}
    void on_gs_image_transfer(const takedown::guest::GsImageTransfer&) override {}
    void on_gs_image_transfer(takedown::guest::GsImageTransfer&&) override {}
};

// Routes guest RAM writes made inside one consumer scope to the retail timer
// consumer trace, restoring the previous observer when the scope closes.
class ScopedRetailTimerConsumerMemoryWriteCapture {
  public:
    enum class Kind : std::uint8_t {
        Scalar,
        Hle,
        SchedulerSifDma,
    };

    ScopedRetailTimerConsumerMemoryWriteCapture(
        takedown::runtime::RetailTimerConsumerTrace& trace,
        takedown::guest::EeMemory& memory,
        const Kind kind,
        const takedown::runtime::RetailTimerConsumerTraceContext& context)
        : trace_(trace), memory_(memory), kind_(kind) {
        switch (kind_) {
        case Kind::Scalar:
            active_ = trace_.begin_scalar_memory_write_capture(context);
            break;
        case Kind::Hle:
            active_ = trace_.begin_hle_memory_write_capture(context);
            break;
        case Kind::SchedulerSifDma:
            active_ = trace_.begin_scheduler_sif_dma_memory_write_capture();
            break;
        }
        if (active_) {
            previous_observer_ = memory_.ee_memory_write_observer();
            memory_.set_ee_memory_write_observer(&trace_);
        }
    }

    ScopedRetailTimerConsumerMemoryWriteCapture(const ScopedRetailTimerConsumerMemoryWriteCapture&) = delete;
    ScopedRetailTimerConsumerMemoryWriteCapture& operator=(
        const ScopedRetailTimerConsumerMemoryWriteCapture&) = delete;

    ~ScopedRetailTimerConsumerMemoryWriteCapture() {
        if (!active_) {
            return;
        }
        memory_.set_ee_memory_write_observer(previous_observer_);
        switch (kind_) {
        case Kind::Scalar:
            trace_.end_scalar_memory_write_capture();
            break;
        case Kind::Hle:
            trace_.end_hle_memory_write_capture();
            break;
        case Kind::SchedulerSifDma:
            trace_.end_scheduler_sif_dma_memory_write_capture();
            break;
        }
    }

  private:
    takedown::runtime::RetailTimerConsumerTrace& trace_;
    takedown::guest::EeMemory& memory_;
    Kind kind_;
    takedown::guest::EeMemoryWriteObserver* previous_observer_{};
    bool active_{};
};

[[nodiscard]] bool focused_trace_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050CollisionTrace::is_target(pc) ||
           takedown::runtime::Slus21050SparkTrace::is_target(pc);
}

[[nodiscard]] bool vehicle_world_contact_probe_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050VehicleWorldContactEndpointProbe::is_target(pc);
}

// Every PC the FEMain dynamic-callback probe and frame-closure diagnostic
// observe must stay on the scalar path while they are enabled.
[[nodiscard]] bool is_femain_dynamic_callback_scalar_target(const GuestAddress pc) noexcept {
    using namespace takedown::runtime;
    return Slus21050FemainFrameClosureDiagnostic::is_scalar_target(pc) ||
           pc == kSlus21050FemainDmacDispatcherCaller ||
           pc == kSlus21050FemainDmacDispatcherDelayPc ||
           pc == kSlus21050FemainDmacDispatcherEntry ||
           pc == kSlus21050FemainDmacInitialRecordLoadPc ||
           pc == kSlus21050FemainDmacLoopRecordLoadPc ||
           pc == kSlus21050FemainDmacRecordDispatchBoundaryPc ||
           pc == kSlus21050FemainDynamicCallbackLoadPc ||
           pc == kSlus21050FemainDynamicCallbackJalrPc ||
           pc == kSlus21050FemainDynamicCallbackDelayPc ||
           pc == kSlus21050FemainDynamicCallbackReturnPc ||
           pc == kSlus21050FemainDynamicCallbackRestorePc ||
           pc == kSlus21050FemainDynamicCallbackActiveStorePc ||
           pc == kSlus21050FemainDmacDispatcherReturnPc ||
           pc == kSlus21050FemainDmacDispatcherReturnDelayPc ||
           pc == kSlus21050FemainDmacDispatcherCallerResumePc;
}

[[nodiscard]] bool focused_or_femain_dynamic_callback_scalar_target(const GuestAddress pc) noexcept {
    return focused_trace_target(pc) || is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool focused_or_vehicle_world_contact_or_femain_scalar_target(const GuestAddress pc) noexcept {
    return focused_trace_target(pc) || vehicle_world_contact_probe_scalar_target(pc) ||
           is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool collision_or_vehicle_world_contact_or_femain_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050CollisionTrace::is_target(pc) ||
           vehicle_world_contact_probe_scalar_target(pc) || is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool spark_or_vehicle_world_contact_or_femain_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050SparkTrace::is_target(pc) ||
           vehicle_world_contact_probe_scalar_target(pc) || is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool vehicle_world_contact_or_femain_scalar_target(const GuestAddress pc) noexcept {
    return vehicle_world_contact_probe_scalar_target(pc) || is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool collision_or_femain_dynamic_callback_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050CollisionTrace::is_target(pc) ||
           is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool spark_or_femain_dynamic_callback_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050SparkTrace::is_target(pc) ||
           is_femain_dynamic_callback_scalar_target(pc);
}

[[nodiscard]] bool focused_or_vehicle_world_contact_scalar_target(const GuestAddress pc) noexcept {
    return focused_trace_target(pc) || vehicle_world_contact_probe_scalar_target(pc);
}

[[nodiscard]] bool collision_or_vehicle_world_contact_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050CollisionTrace::is_target(pc) ||
           vehicle_world_contact_probe_scalar_target(pc);
}

[[nodiscard]] bool spark_or_vehicle_world_contact_scalar_target(const GuestAddress pc) noexcept {
    return takedown::runtime::Slus21050SparkTrace::is_target(pc) ||
           vehicle_world_contact_probe_scalar_target(pc);
}

void print_usage(std::ostream& output) {
    output << takedown::kGameTitle << "\n\n"
           << "Usage:\n"
           << "  takedown-recomp doctor\n"
           << "  takedown-recomp inspect-elf <boot-elf>\n"
           << "  takedown-recomp analyze-elf <boot-elf>\n"
           << "  takedown-recomp analyze-function <boot-elf> <address> [maximum-decoded-instructions]\n"
           << "  takedown-recomp lift-function-cpp <boot-elf> <address> <instruction-count> [name]\n"
           << "  takedown-recomp lift-instruction-block-cpp <boot-elf> <address> [name]\n"
           << "  takedown-recomp generate-block-module-cpp <boot-elf> <start-address> <instruction-count> [maximum-generated-blocks]\n"
           << "  takedown-recomp generate-sparse-block-module-cpp <boot-elf> <start-address>:<instruction-count> [...]\n"
           << "  takedown-recomp generate-leaf-module-cpp <boot-elf> [maximum-generated-functions]\n"
           << "  takedown-recomp decode-entry <boot-elf> [instruction-count]\n"
           << "  takedown-recomp probe-entry <boot-elf> [maximum-steps]\n"
           << "  takedown-recomp boot-entry <boot-elf> [maximum-steps] [--capture-gs <directory>] [--scanout-ppm <file>] [--window] [--host-controller] [--auto-start] [--subframe-input] [--parallel-gs-stream <file>] [--parallel-gs-live <fifo>] [--parallel-gs-completion <fifo>] [--parallel-gs-input <fifo>] [--guest-work-measurement-steps <count>] [--profile-guest-work-window] [--guest-input-record <file>] [--guest-input-replay <file>] [--parallel-gs-live-capture <file>] [--parallel-gs-only] [--gs-semantic-trace <file>] [--gs-semantic-events <count>] [--gs-semantic-vblank <index|last>] [--femain-terminal-diagnostic <file>] [--femain-terminal-diagnostic-guest-work-only] [--host-movie-playback] [--headless-movie-eos] [--headless-frontend-replay] [--run-until-close] [--run-until-stop] [--retail-trace] [--interpreter-only] [--native-only] [--disable-vu1-upper-stages] [--disable-vu1-natural-blocks] [--disable-vu1-lower-plans] [--vu1-simple-alu-lower-plans] [--vu1-ilw-lower-plans] [--vu1-isw-lower-plans] [--vu1-clip-predicate-lower-plans] [--vu1-worker-differential] [--vu1-worker-async-stage2] [--vu1-worker-consume [--vu1-worker-consume-audit-period <N>]] [--collision-trace <file>] [--spark-trace <file>] [--ipu-frame-dump <file>] [--ee-host-phase-profile <empty-directory>] [--ee-graphics-service-profile <empty-directory>] [--ee-pc-profile <empty-directory>] [--vu1-profile <empty-directory>] [--require-media-parity] [--stats-overlay]\n"
           << "    F8 latches the current GS interval at its next modeled VBlank and writes a numbered .gssem without stopping the window when --gs-semantic-vblank last is active. A local renderer also adds matching clean-frame .ppm and raw .gsvram files; --parallel-gs-only deliberately keeps the semantic-only fast path.\n"
           << "    F6 toggles VBlank-accurate frame-step mode; F7 advances one guest frame.\n"
           << "    --guest-work-measurement-steps <N> waits for explicit B3IN bit 20, then\nstarts at next existing EE instruction boundary and stops after exactly N\ncommitted guest steps; no guest memory is written. Without a positional\nstep limit it waits for that input edge, never a wall-clock timeout.\n"
           << "    --profile-guest-work-window starts requested EE-PC, EE-host-phase, and\nVU1 profilers at the B3IR A boundary and stops them at exact C. It requires\n--guest-work-measurement-steps and at least one profile output option.\n"
           << "    --guest-input-record writes changed full controller snapshots at exact\nretail-poll step/cycle boundaries through one completed measurement.\n"
           << "    --guest-input-replay applies that finite timeline, validates every retail\npoll and boundary, and uses its A marker as the sole measurement arm.\n"
           << "    --guest-input-poll-diagnostic <new-file> preserves replay controller states\nand poll ordinals while recording actual timings. A/C steps stay fixed.\nRequires --guest-input-replay; diagnostic only, not strict replay parity.\n"
           << "    --frame-rate retail|variable overrides the [graphics] frame_rate setting.\nvariable runs one simulation tick per displayed frame, lasting as long as the\nprevious frame really took. --frame-time-script <spec> replaces the host clock\nfor exact repeats (live, retail, milliseconds, a comma list, or\nlcg:<seed>:<min>:<max>);\n--frame-time-script-lead <ms> fixes every frame before a replay\'s A marker;\n--guest-input-time-keyed replays a timeline by guest time (needs\n--guest-input-poll-diagnostic).\n"
           << "    --subframe-input timestamps host controller edges on arrival and applies\n"
           << "    each at the first guest boundary at or after its arrival cycle; the\n"
           << "    default remains hardware-parity sampling at the retail pad poll.\n"
           << "    --femain-terminal-diagnostic writes bounded producer evidence (or use\n"
           << "    TAKEDOWN_FEMAIN_TERMINAL_DIAGNOSTIC); it never authorizes a terminal frame.\n"
           << "    --femain-terminal-diagnostic-guest-work-only clears startup evidence at the\n"
           << "    exact guest-work A boundary and collects through C; it requires both\n"
           << "    --femain-terminal-diagnostic <file> and --guest-work-measurement-steps <N>.\n"
           << "    --ee-pc-profile also writes approximate 4096-instruction boundary-window host rankings; it does not time every EE instruction.\n"
           << "    --ee-host-phase-profile samples durable runner phases independently of EE PC profiling; VIF1 includes nested VU1 work and GIF includes synchronous GS stream observers.\n"
           << "    --ee-graphics-service-profile records opt-in integer route/event counters inside the exact B3IR A-to-C graphics callback; it requires the host-phase profile and emits diagnostics only (performance_claim=false).\n"
           << "    --disable-vu1-upper-stages disables only automatic runtime-hot VU1 upper-stage dispatch for diagnostic A/B runs; exact generated/static descriptors remain enabled.\n"
           << "    --disable-vu1-natural-blocks disables only automatic runtime-hot VU1 natural-block batching for diagnostic A/B runs; ordinary predecode and exact generated/static descriptors remain enabled.\n"
           << "    --disable-vu1-lower-plans disables only cache-owned lower-slot plans inside admitted VU1 natural/static ranges for diagnostic A/B runs.\n"
           << "    --vu1-simple-alu-lower-plans opts into the interpreter-only IAND/IOR lower-slot plans for diagnostic A/B runs; it is also enabled by TAKEDOWN_VU1_SIMPLE_ALU_LOWER_PLANS when that variable is present and not exactly \"0\".\n"
           << "    --vu1-ilw-lower-plans opts into the interpreter-only ILW lower-slot plan for diagnostic A/B runs; it is also enabled by TAKEDOWN_VU1_ILW_LOWER_PLANS when that variable is present and not exactly \"0\".\n"
           << "    --vu1-isw-lower-plans opts into the interpreter-only ISW lower-slot plan for diagnostic A/B runs; it is also enabled by TAKEDOWN_VU1_ISW_LOWER_PLANS when that variable is present and not exactly \"0\".\n"
           << "    --vu1-flag-read-lower-plans opts into the interpreter-only FMAND/FCGET lower-slot plans for diagnostic A/B runs; it is also enabled by TAKEDOWN_VU1_FLAG_READ_LOWER_PLANS when that variable is present and not exactly \"0\".\n"
           << "    --vu1-clip-predicate-lower-plans opts into the interpreter-only FCAND/FCOR lower-slot plans for diagnostic A/B runs; it is also enabled by TAKEDOWN_VU1_CLIP_PREDICATE_LOWER_PLANS when that variable is present and not exactly \"0\".\n"
           << "    --vu1-worker-differential runs every eligible VU1 launch through the detached Stage 1 worker service and compares state, VUMem, and timing with the synchronous reference; it never overlaps asynchronously.\n"
           << "    --vu1-worker-async-stage2 opts into VU1 worker Stage 2; milestone 1 only installs the deferrable-launch classification (default denial-conservative) and changes no behavior yet.\n"
           << "    --vu1-worker-consume opts into VU1 worker Stage 2 milestone 3: admitted launches execute only on the worker and the emulation thread consumes the journaled result (VUMem deltas, byte-exact PATH1 packets, pending-producer restore) at the documented fences; --vu1-worker-consume-audit-period <N> additionally runs every Nth admitted launch synchronously and hard-errors on any journal mismatch.\n"
           << "  takedown-recomp pcsx2-probe [--socket <path>]\n"
           << "  takedown-recomp pcsx2-snapshot --output <directory> --range <address>:<bytes> [--socket <path>]\n"
           << "  takedown-recomp pcsx2-inspect-gs <dump.gs|dump.gs.zst>\n"
           << "  takedown-recomp pcsx2-compare-gs <live.gssem> <dump.gs|dump.gs.zst>\n"
           << "  takedown-recomp pcsx2-render-savestate-scanout <GS.bin> <PCSX2-Internal-Structures.dat> <output.ppm>\n"
           << "  takedown-recomp replay-vif1 <raw-ee-ram.bin> <tadr> [--tte] [--gs-signature] [--raw-gs <output.raw>] [--pcsx2-vu-state <extracted-directory>] [--vu1-worker-differential] [--vu1-worker-async-stage2] [--vu1-worker-consume [--vu1-worker-consume-audit-period <N>]]\\n"
           << "  takedown-recomp replay-gif <raw-ee-ram.bin> <tadr>\n"
           << "  takedown-recomp import <game.iso|game.chd> --output <directory> [--allow-unknown]\\n"
           << "  takedown-recomp save-data <root> list\\n"
           << "  takedown-recomp save-data <root> read <save> [--print-bytes]\\n"
           << "  takedown-recomp save-data <root> write <save> <file> <host-file>\\n"
           << "  takedown-recomp save-data <root> import-memcard <memcard.ps2>\\n"
           << "  takedown-recomp save-data <saves-root> slots\\n"
           << "  takedown-recomp save-data <saves-root> import <source> <slot> [--replace]\\n"
           << "    The save-data subcommands manage host-side game save files (PCSX2-compatible\\n"
           << "    folder-form layout; user-facing name is \"save data\" / \"profiles\" / \"storage\").\\n";
}


void write_rgba_ppm(const std::filesystem::path& path,
                    const std::uint32_t width,
                    const std::uint32_t height,
                    const std::span<const unsigned char> rgba) {
    if (path.empty()) {
        throw std::invalid_argument("--scanout-ppm requires a non-empty file path");
    }
    const auto pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U || rgba.size() / 4U < pixel_count || rgba.size() != pixel_count * 4U) {
        throw std::invalid_argument("cannot export an incomplete RGBA scanout frame");
    }
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite scanout PPM: " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create scanout PPM: " + path.string());
    }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t offset = 0; offset < rgba.size(); offset += 4U) {
        output.put(static_cast<char>(rgba[offset]));
        output.put(static_cast<char>(rgba[offset + 1U]));
        output.put(static_cast<char>(rgba[offset + 2U]));
    }
    if (!output) {
        throw std::runtime_error("unable to write complete scanout PPM: " + path.string());
    }
}

void write_raw_gs_vram(const std::filesystem::path& path, const std::span<const unsigned char> vram) {
    if (path.empty() || vram.size() != takedown::guest::kGsLocalMemoryBytes) {
        throw std::invalid_argument("cannot export an incomplete GS VRAM image");
    }
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite GS VRAM image: " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create GS VRAM image: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(vram.data()),
                 static_cast<std::streamsize>(vram.size()));
    if (!output) {
        throw std::runtime_error("unable to write complete GS VRAM image: " + path.string());
    }
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void write_luma_ppm(const std::filesystem::path& path,
                    const std::uint32_t width,
                    const std::uint32_t height,
                    const std::span<const unsigned char> luma) {
    if (path.empty()) {
        throw std::invalid_argument("--ipu-frame-dump requires a non-empty file path");
    }
    const auto pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U || pixel_count != luma.size()) {
        throw std::invalid_argument("cannot export an incomplete IPU luma frame");
    }
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite IPU frame PPM: " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create IPU frame PPM: " + path.string());
    }
    output << "P5\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(luma.data()), static_cast<std::streamsize>(luma.size()));
    if (!output) {
        throw std::runtime_error("unable to write complete IPU frame PPM: " + path.string());
    }
}

void prepare_empty_capture_directory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument("--capture-gs requires a non-empty directory path");
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        throw std::runtime_error("unable to inspect GS capture directory: " + error.message());
    }
    if (exists) {
        if (!std::filesystem::is_directory(directory, error) || error) {
            throw std::invalid_argument("GS capture path exists and is not a directory: " + directory.string());
        }
        const bool empty = std::filesystem::is_empty(directory, error);
        if (error) {
            throw std::runtime_error("unable to inspect GS capture directory: " + error.message());
        }
        if (!empty) {
            throw std::invalid_argument("GS capture directory must be empty: " + directory.string());
        }
        return;
    }
    if (!std::filesystem::create_directories(directory, error) || error) {
        throw std::runtime_error("unable to create GS capture directory: " + error.message());
    }
}

void write_vu1_profile_artifacts(const std::filesystem::path& directory,
                                 const takedown::guest::Vu1ProgramCache& cache) {
    std::vector<takedown::guest::Vu1ProgramProfile> profiles{cache.profiles().begin(), cache.profiles().end()};
    std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
        if (left.host_nanoseconds != right.host_nanoseconds) {
            return left.host_nanoseconds > right.host_nanoseconds;
        }
        if (left.committed_pairs != right.committed_pairs) {
            return left.committed_pairs > right.committed_pairs;
        }
        if (left.runs != right.runs) {
            return left.runs > right.runs;
        }
        if (left.hash.high != right.hash.high) {
            return left.hash.high < right.hash.high;
        }
        if (left.hash.low != right.hash.low) {
            return left.hash.low < right.hash.low;
        }
        return std::tie(left.entry, left.resolved_entry) < std::tie(right.entry, right.resolved_entry);
    });

    const std::filesystem::path table_path = directory / "profile.tsv";
    std::ofstream table(table_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!table) {
        throw std::runtime_error("unable to create VU1 profile table: " + table_path.string());
    }
    table << "hash_high\thash_low\tentry\tresolved_entry\truns\tcompleted_runs\tcommitted_pairs\t"
             "elapsed_cycles\tpipeline_stall_cycles\thost_nanoseconds\tcompiled_runs\t"
             "descriptor_dispatches\tdirect_predecoded_dispatches\tstatic_block_dispatches\t"
             "native_executor_dispatches\n";
    for (const auto& profile : profiles) {
        table << hex64(profile.hash.high) << '\t' << hex64(profile.hash.low) << '\t'
              << hex32(profile.entry) << '\t' << hex32(profile.resolved_entry) << '\t' << profile.runs
              << '\t' << profile.completed_runs << '\t' << profile.committed_pairs << '\t'
              << profile.elapsed_cycles << '\t' << profile.pipeline_stall_cycles << '\t'
              << profile.host_nanoseconds << '\t' << profile.compiled_runs << '\t' << profile.compiled_runs
              << '\t' << profile.direct_predecoded_runs << '\t' << profile.static_block_runs << '\t'
              << profile.native_executor_runs << '\n';
    }
    if (!table) {
        throw std::runtime_error("unable to write complete VU1 profile table: " + table_path.string());
    }

    const std::vector<takedown::guest::Vu1ProfiledProgramSnapshot> snapshots = cache.profiled_program_snapshots();
    const std::filesystem::path pairs_path = directory / "pairs.tsv";
    std::ofstream pairs(pairs_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!pairs) {
        throw std::runtime_error("unable to create VU1 pair profile table: " + pairs_path.string());
    }
    pairs << "hash_high\thash_low\tentry\tresolved_entry\tpc\tcommitted_pairs\tlower_word\tupper_word\t"
             "upper_op\tlower_op\tflow\n";
    for (const auto& snapshot : snapshots) {
        for (std::size_t pc = 0; pc < takedown::guest::kVu1MicroProgramPairCount; ++pc) {
            const auto count = snapshot.pair_counts[pc];
            if (count == 0U) {
                continue;
            }
            const takedown::guest::vu1_detail::CachedVu1Pair decoded =
                takedown::guest::vu1_detail::decode_vu1_pair_metadata(snapshot.micro_memory,
                                                                      static_cast<std::uint16_t>(pc));
            pairs << hex64(snapshot.profile.hash.high) << '\t' << hex64(snapshot.profile.hash.low) << '\t'
                  << hex32(snapshot.profile.entry) << '\t' << hex32(snapshot.profile.resolved_entry) << '\t'
                  << hex32(static_cast<std::uint32_t>(pc)) << '\t' << count << '\t'
                  << hex32(decoded.lower_word) << '\t' << hex32(decoded.upper_word) << '\t'
                  << (decoded.upper ? static_cast<int>(decoded.upper->op) : -1) << '\t'
                  << (decoded.lower ? static_cast<int>(decoded.lower->op) : -1) << '\t'
                  << static_cast<int>(decoded.flow) << '\n';
        }
    }
    if (!pairs) {
        throw std::runtime_error("unable to write complete VU1 pair profile table: " + pairs_path.string());
    }

    const auto generated_source = takedown::recompiler::build_vu1_program_module(snapshots, 32U);
    const std::filesystem::path source_path = directory / "hot-vu1-programs.cpp";
    std::ofstream source(source_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!source) {
        throw std::runtime_error("unable to create generated VU1 module: " + source_path.string());
    }
    source.write(generated_source.data(), static_cast<std::streamsize>(generated_source.size()));
    if (!source) {
        throw std::runtime_error("unable to write complete generated VU1 module: " + source_path.string());
    }
}

[[nodiscard]] std::uint64_t
classified_fusion_dispatches(const takedown::runtime::CompiledBlockFusionStatistics& stats) {
    return stats.outer_not_fusible_stops + stats.entered_delay_slot_stops +
               stats.first_result_not_safepoint_or_one_to_one_stops + stats.progress_checkpoint_stops +
               stats.zero_capacity_budget_stops + stats.one_instruction_budget_stops +
               stats.fused_budget_stops + stats.scalar_barrier_stops +
               stats.outer_live_word_mismatch_stops + stats.missing_successor_stops +
               stats.nonfusible_successor_stops + stats.successor_live_word_mismatch_stops +
               stats.post_instruction_delay_slot_stops + stats.trait_invariant_violations;
}

void write_compiled_block_fusion_profile(const std::filesystem::path& directory,
                                         const takedown::runtime::CompiledBlockFusionStatistics& statistics) {
    const std::filesystem::path path = directory / "compiled-fusion.tsv";
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite EE fusion profile artifact: " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create EE fusion profile artifact: " + path.string());
    }
    output << "limiter\tcount\n";
    output << "classified_dispatches\t"
           << classified_fusion_dispatches(statistics)
           << '\n'
           << "outer_not_fusible\t" << statistics.outer_not_fusible_stops << '\n'
           << "entered_delay_slot\t" << statistics.entered_delay_slot_stops << '\n'
           << "first_result_not_safepoint_or_one_to_one\t"
           << statistics.first_result_not_safepoint_or_one_to_one_stops << '\n'
           << "progress_checkpoint\t" << statistics.progress_checkpoint_stops << '\n'
           << "zero_capacity_budget\t" << statistics.zero_capacity_budget_stops << '\n'
           << "one_instruction_budget\t" << statistics.one_instruction_budget_stops << '\n'
           << "fused_budget\t" << statistics.fused_budget_stops << '\n'
           << "scalar_barrier\t" << statistics.scalar_barrier_stops << '\n'
           << "outer_live_word_mismatch\t" << statistics.outer_live_word_mismatch_stops << '\n'
           << "missing_successor\t" << statistics.missing_successor_stops << '\n'
           << "nonfusible_successor\t" << statistics.nonfusible_successor_stops << '\n'
           << "successor_live_word_mismatch\t" << statistics.successor_live_word_mismatch_stops << '\n'
           << "post_instruction_delay_slot\t" << statistics.post_instruction_delay_slot_stops << '\n'
           << "trait_invariant_violation\t" << statistics.trait_invariant_violations << '\n'
           << "fused_dispatches\t" << statistics.fused_dispatches << '\n'
           << "fused_successor_instructions\t" << statistics.fused_successor_instructions << '\n';
    if (!output) {
        throw std::runtime_error("unable to write complete EE fusion profile artifact: " + path.string());
    }
}

std::filesystem::path gs_semantic_checkpoint_filename(const std::filesystem::path& primary_path,
                                                    const std::size_t index) {
    const std::filesystem::path extension = primary_path.extension();
    const std::filesystem::path stem = extension.empty() ? primary_path.filename() : primary_path.stem();
    std::ostringstream filename;
    filename << stem.string() << "-capture-" << std::setfill('0') << std::setw(4) << index;
    if (extension.empty()) {
        filename << ".gssem";
    } else {
        filename << extension.string();
    }
    return primary_path.parent_path() / filename.str();
}

void print_bounded_ascii_runs(const takedown::guest::EeMemory& memory,
                              const takedown::guest::GuestAddress address,
                              const std::uint32_t size) {
    std::size_t emitted = 0U;
    std::uint32_t cursor = 0U;
    while (cursor < size && emitted != 4U) {
        while (cursor < size) {
            const std::uint8_t value = memory.read_u8(address + cursor);
            if (value >= 0x20U && value <= 0x7EU) {
                break;
            }
            ++cursor;
        }
        const std::uint32_t start = cursor;
        while (cursor < size) {
            const std::uint8_t value = memory.read_u8(address + cursor);
            if (value < 0x20U || value > 0x7EU) {
                break;
            }
            ++cursor;
        }
        if (cursor - start < 4U) {
            if (cursor < size) {
                ++cursor;
            }
            continue;
        }
        std::string run;
        run.reserve(std::min<std::size_t>(cursor - start, 96U));
        for (std::size_t index = 0; index < std::min<std::size_t>(cursor - start, 96U); ++index) {
            run.push_back(
                static_cast<char>(memory.read_u8(address + start + static_cast<std::uint32_t>(index))));
        }
        std::cout << "    ASCII +" << hex32(start) << ": " << run;
        if (cursor - start > 96U) {
            std::cout << "...";
        }
        std::cout << '\n';
        ++emitted;
    }
}

std::size_t parse_bounded_count(const std::string_view text,
                                const std::string_view description,
                                const std::size_t maximum) {
    std::size_t value = 0U;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0U || value > maximum) {
        throw std::invalid_argument(std::string(description) + " must be an integer from 1 to " +
                                    std::to_string(maximum));
    }
    return value;
}

std::uint32_t parse_u32(const std::string_view text, const std::string_view description) {
    std::string_view digits = text;
    int base = 10;
    if (digits.size() > 2U && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2U);
    }
    if (digits.empty()) {
        throw std::invalid_argument(std::string(description) + " must be a 32-bit integer");
    }
    std::uint64_t value = 0U;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(description) + " must be a 32-bit integer");
    }
    return static_cast<std::uint32_t>(value);
}

// A guest EE range read through PCSX2 PINE: `byte_count` bytes starting at `address`.
struct Pcsx2CaptureRange {
    std::uint32_t address{};
    std::uint32_t byte_count{};
};

// Reads one PINE batch, re-checking that PCSX2 is still paused first.
std::vector<std::uint32_t> read_pcsx2_words(takedown::platform::Pcsx2PineClient& client,
                                            const std::span<const std::uint32_t> addresses) {
    auto permit = client.verify_paused();
    if (!permit) {
        throw std::runtime_error("PCSX2 is no longer paused; refusing to read guest memory");
    }
    return client.read32_batch(std::move(*permit), addresses);
}

std::vector<unsigned char> read_pcsx2_capture_range(takedown::platform::Pcsx2PineClient& client,
                                                    const Pcsx2CaptureRange range) {
    std::vector<unsigned char> bytes;
    bytes.reserve(range.byte_count);
    std::array<std::uint32_t, takedown::platform::kPcsx2PineMaximumRead32Batch> addresses{};
    for (std::uint32_t offset = 0U; offset < range.byte_count;) {
        const std::size_t count = std::min<std::size_t>((range.byte_count - offset) / sizeof(std::uint32_t),
                                                         takedown::platform::kPcsx2PineMaximumRead32Batch);
        for (std::size_t index = 0; index < count; ++index) {
            addresses[index] = range.address + offset + static_cast<std::uint32_t>(index) * 4U;
        }
        const std::vector<std::uint32_t> words =
            read_pcsx2_words(client, std::span<const std::uint32_t>(addresses.data(), count));
        for (const auto word : words) {
            for (unsigned byte = 0U; byte != 4U; ++byte) {
                bytes.push_back(static_cast<unsigned char>(word >> (byte * 8U)));
            }
        }
        offset += static_cast<std::uint32_t>(count) * 4U;
    }
    return bytes;
}

// RECOVERY: orig has basic_ofstream::open(wchar_t const*) [clone .part.0] out of line; inlined in partial build.

[[nodiscard]] bool write_femain_dynamic_callback_report(
    const std::filesystem::path& path,
    const takedown::runtime::Slus21050FemainDynamicCallbackProbe& probe) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    const auto candidates = probe.candidates();
    const auto counters = probe.counters();
    output << "schema=1\n"
           << "enabled=" << (probe.enabled() ? 1 : 0) << "\n"
           << "candidate_evidence_only=1\n"
           << "terminal_authority=none\n"
           << "terminal_witness_emitted=0\n"
           << "failure=" << takedown::runtime::to_string(probe.failure()) << "\n"
           << "failure.hle-dmac=" << takedown::runtime::to_string(probe.hle_dmac_failure()) << "\n"
           << "candidate_count=" << candidates.size() << "\n"
           << "candidate_capacity="
           << takedown::runtime::Slus21050FemainDynamicCallbackProbe::kMaximumCandidates << "\n"
           << "counter.authenticated-invocations-armed=" << counters.authenticated_invocations_armed
           << "\n"
           << "counter.invocations-completed-resumed=" << counters.invocations_completed_resumed
           << "\n"
           << "counter.callback-seam-loads-while-armed=" << counters.callback_seam_loads_while_armed
           << "\n"
           << "counter.complete-callback-observations-published="
           << counters.complete_callback_observations_published << "\n"
           << "counter.unarmed-seam-loads-ignored=" << counters.unarmed_seam_loads_ignored << "\n"
           << "counter.hle-dmac-callbacks-armed=" << counters.hle_dmac_callbacks_armed << "\n"
           << "counter.hle-dmac-callbacks-completed=" << counters.hle_dmac_callbacks_completed << "\n"
           << "dispatch.committed-total=" << probe.dispatch_histogram().dispatches << "\n"
           << "dispatch.hle-dmac.committed-total=" << probe.hle_dmac_dispatch_histogram().dispatches
           << "\n"
           << "serial_snapshot.valid=0\n"
           << "serial_snapshot.primary=unknown\n"
           << "serial_snapshot.worker=unknown\n"
           << "seam.dispatcher-caller-word="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherCallerWord) << "\n"
           << "seam.dispatcher-delay-pc="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherDelayPc) << "\n"
           << "seam.dispatcher-delay-word="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherDelayWord) << "\n"
           << "seam.dispatcher-entry-word="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherEntryWord) << "\n"
           << "seam.dispatcher-return-pc="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherReturnPc) << "\n"
           << "seam.dispatcher-return-delay-pc="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherReturnDelayPc) << "\n"
           << "seam.dispatcher-caller-resume-pc="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherCallerResumePc) << "\n"
           << "seam.dispatcher-entry=" << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherEntry)
           << "\n"
           << "seam.dispatcher-caller="
           << hex32(takedown::runtime::kSlus21050FemainDmacDispatcherCaller) << "\n"
           << "seam.initial-command-load="
           << hex32(takedown::runtime::kSlus21050FemainDmacInitialRecordLoadPc) << "\n"
           << "seam.loop-command-load="
           << hex32(takedown::runtime::kSlus21050FemainDmacLoopRecordLoadPc) << "\n"
           << "seam.command-dispatch="
           << hex32(takedown::runtime::kSlus21050FemainDmacRecordDispatchBoundaryPc) << "\n"
           << "seam.load-pc=" << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackLoadPc) << "\n"
           << "seam.jalr-pc=" << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackJalrPc) << "\n"
           << "seam.delay-pc=" << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackDelayPc)
           << "\n"
           << "seam.return-pc=" << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackReturnPc)
           << "\n"
           << "seam.restore-pc=" << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackRestorePc)
           << "\n"
           << "seam.active-store-pc="
           << hex32(takedown::runtime::kSlus21050FemainDynamicCallbackActiveStorePc) << "\n";
    for (std::size_t type = 0U; type < probe.dispatch_histogram().command_type_counts.size(); ++type) {
        const auto count = probe.dispatch_histogram().command_type_counts[type];
        if (count != 0U) {
            output << "dispatch.type=" << type << " count=" << count << "\n";
        }
    }
    for (std::size_t type = 0U; type < probe.hle_dmac_dispatch_histogram().command_type_counts.size();
         ++type) {
        const auto count = probe.hle_dmac_dispatch_histogram().command_type_counts[type];
        if (count != 0U) {
            output << "dispatch.hle-dmac.type=" << type << " count=" << count << "\n";
        }
    }
    for (const auto& candidate : candidates) {
        output << "candidate ordinal=" << candidate.ordinal
               << " observations=" << candidate.observation_count
               << " first-guest-step=" << candidate.first_guest_step
               << " last-guest-step=" << candidate.last_guest_step
               << " guest-step=" << candidate.guest_step << " guest-pc=" << hex32(candidate.guest_pc)
               << " memory-pc=" << hex32(candidate.memory_pc)
               << " queue-command=" << hex32(candidate.queue_command)
               << " queue-command-type=" << static_cast<unsigned>(candidate.queue_command_type)
               << " queue-read=" << hex32(candidate.queue_read)
               << " queue-end=" << hex32(candidate.queue_end)
               << " callback-target=" << hex32(candidate.callback_target)
               << " callback-target-first-word=" << hex32(candidate.callback_target_first_word)
               << " callback-return-address=" << hex32(candidate.callback_return_address)
               << " callback-return=" << hex32(candidate.callback_return)
               << " transfers-active-at-load=" << static_cast<unsigned>(candidate.transfers_active_at_load)
               << " transfers-active-pre=" << static_cast<unsigned>(candidate.transfers_active_pre)
               << " transfers-active-post=" << static_cast<unsigned>(candidate.transfers_active_post)
               << " callback-return-zero-count=" << candidate.callback_return_zero_count
               << " callback-return-nonzero-count=" << candidate.callback_return_nonzero_count
               << " active-pre-zero-count=" << candidate.transfers_active_pre_zero_count
               << " active-pre-nonzero-count=" << candidate.transfers_active_pre_nonzero_count
               << " active-post-zero-count=" << candidate.transfers_active_post_zero_count
               << " active-post-nonzero-count=" << candidate.transfers_active_post_nonzero_count
               << " channel-known=" << (candidate.channel_known ? 1 : 0)
               << " channel=" << candidate.channel
               << " dispatcher-argument=" << hex32(candidate.dispatcher_argument)
               << " interrupted-pc-known=" << (candidate.interrupted_pc_known ? 1 : 0)
               << " interrupted-pc=" << hex32(candidate.interrupted_pc)
               << " callback-identity=" << hex64(candidate.callback_identity)
               << " callback-identity-local=" << (candidate.callback_identity_is_local_invocation ? 1 : 0)
               << " callback-identity-first=" << hex64(candidate.callback_identity_first)
               << " callback-identity-last=" << hex64(candidate.callback_identity_last)
               << " invocation-guest-step=" << candidate.invocation_guest_step
               << " invocation-caller-pc=" << hex32(candidate.invocation_caller_pc)
               << " invocation-delay-pc=" << hex32(candidate.invocation_delay_pc)
               << " invocation-entry-pc=" << hex32(candidate.invocation_entry_pc)
               << " invocation-caller-word=" << hex32(candidate.invocation_caller_instruction_word)
               << " invocation-delay-word=" << hex32(candidate.invocation_delay_instruction_word)
               << " invocation-entry-word=" << hex32(candidate.invocation_entry_instruction_word)
               << " serial-valid=" << (candidate.serial_snapshot.valid ? 1 : 0)
               << " serial-count=" << candidate.serial_snapshot_count
               << " primary-serial-first=" << candidate.primary_serial_first
               << " primary-serial-last=" << candidate.primary_serial_last
               << " worker-serial-first=" << candidate.worker_serial_first
               << " worker-serial-last=" << candidate.worker_serial_last
               << " load-word=" << hex32(candidate.load_instruction_word)
               << " jalr-word=" << hex32(candidate.jalr_instruction_word)
               << " delay-word=" << hex32(candidate.delay_instruction_word)
               << " return-word=" << hex32(candidate.callback_return_instruction_word)
               << " restore-word=" << hex32(candidate.restore_instruction_word)
               << " active-store-word=" << hex32(candidate.active_store_instruction_word) << '\n';
    }
    return output.good();
}

[[nodiscard]] bool write_femain_frame_closure_report(
    const std::filesystem::path& path,
    const takedown::runtime::Slus21050FemainFrameClosureDiagnostic& diagnostic) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << "schema=1\n"
           << "enabled=" << (diagnostic.enabled() ? 1 : 0) << "\n"
           << "candidate_evidence_only=1\n"
           << "terminal_authority=none\n"
           << "terminal_witness_emitted=0\n"
           << "phase=" << takedown::runtime::to_string(diagnostic.phase()) << "\n"
           << "failure=" << takedown::runtime::to_string(diagnostic.failure()) << "\n"
           << "candidate_count=" << diagnostic.candidate_count() << "\n"
           << "candidate_capacity="
           << takedown::runtime::Slus21050FemainFrameClosureDiagnostic::kMaximumCandidates << "\n";
    const auto write_snapshot = [&output](const std::uint64_t ordinal, const std::string_view boundary,
                                          const takedown::runtime::Slus21050FemainFrameClosureSnapshot&
                                              snapshot) {
        output << "boundary ordinal=" << ordinal << " kind=" << boundary
               << " guest-step=" << snapshot.guest_step << " guest-cycle=" << snapshot.guest_cycle
               << " guest-vblank=" << snapshot.guest_vblank << " audio-sample=" << snapshot.audio_sample
               << " thread-id=" << snapshot.execution.thread_id
               << " callback-active=" << (snapshot.execution.callback_active ? 1 : 0)
               << " callback-identity=" << snapshot.execution.callback_identity
               << " callback-activation-generation=" << snapshot.execution.callback_activation_generation
               << " observation-epoch=" << snapshot.observation_epoch
               << " timeline-count=" << snapshot.timeline_event_count
               << " raw-count=" << snapshot.raw_event_count << " eop-count=" << snapshot.eop_event_count
               << " primary-count=" << snapshot.primary_command_count
               << " worker-count=" << snapshot.worker_dispatch_count
               << " queue-read=" << hex32(snapshot.queue_read) << " queue-end=" << hex32(snapshot.queue_end)
               << " transfers-active=" << static_cast<unsigned>(snapshot.transfers_active)
               << " s0=" << hex64(snapshot.s0) << " s1=" << hex64(snapshot.s1) << " s2=" << hex64(snapshot.s2)
               << " publication-bound=" << (snapshot.publication.present ? 1 : 0)
               << " owner=" << snapshot.publication.owner_identity
               << " publication-serial=" << snapshot.publication.publication_serial
               << " player=" << snapshot.publication.player_identity
               << " player-session=" << snapshot.publication.player_session_generation
               << " ledger=" << snapshot.publication.ledger_identity
               << " ledger-session=" << snapshot.publication.ledger_session_generation
               << " frame-generation=" << snapshot.publication.frame_generation
               << " publication-queue-read=" << hex32(snapshot.publication.queue_read)
               << " publication-queue-end=" << hex32(snapshot.publication.queue_end)
               << " publication-queue-base=" << hex32(snapshot.publication.queue_base)
               << " publication-buffer-index=" << static_cast<unsigned>(snapshot.publication.buffer_index)
               << '\n';
    };
    for (const auto& candidate : diagnostic.candidates()) {
        output << "candidate ordinal=" << candidate.ordinal
               << " sequence-index=" << candidate.sequence_index
               << " first-call=" << hex32(candidate.sequence.first_call)
               << " second-call=" << hex32(candidate.sequence.second_call)
               << " final-pc=" << hex32(candidate.sequence.final_pc) << '\n';
        write_snapshot(candidate.ordinal, "before", candidate.before);
        write_snapshot(candidate.ordinal, "between", candidate.between);
        write_snapshot(candidate.ordinal, "after", candidate.after);
    }
    return output.good();
}

// RECOVERY: profile counts differ from orig (entry 1073741822, merge bb 0.66E, else bb 0.44E); context-dependent, no source variant matched.
[[nodiscard]] bool write_femain_retail_consume_report(
    const std::filesystem::path& path,
    const takedown::runtime::Slus21050FemainRetailConsumeInvocationBarrier& barrier,
    const takedown::runtime::Slus21050FemainRetailConsumeObservationAdapter& adapter,
    const std::uint64_t record_ordinal) {
    const auto mode = std::ios::binary | (record_ordinal == 1U ? std::ios::trunc : std::ios::app);
    std::ofstream output(path, mode);
    if (!output) {
        return false;
    }
    const auto& probe = adapter.probe();
    const auto result = probe.result();
    const takedown::runtime::Slus21050FemainRetailConsumeObservationIdentity& identity = barrier.identity();
    const takedown::runtime::Slus21050FemainRetailConsumeExecutionContext& execution =
        barrier.execution_context();
    output << "record=" << record_ordinal << "\n"
           << "schema=1\n"
           << "candidate_evidence_only=1\n"
           << "terminal_authority=none\n"
           << "terminal_witness_emitted=0\n"
           << "barrier.phase=" << takedown::runtime::to_string(barrier.phase()) << "\n"
           << "barrier.failure=" << takedown::runtime::to_string(barrier.failure()) << "\n"
           << "barrier.acceleration-blocked=" << (barrier.blocks_accelerated_dispatch() ? 1 : 0) << "\n"
           << "publication.serial=" << barrier.publication_serial() << "\n"
           << "identity.runtime-epoch=" << identity.runtime_epoch << "\n"
           << "identity.observation-epoch=" << identity.observation_epoch << "\n"
           << "identity.invocation-serial=" << identity.invocation_serial << "\n"
           << "identity.guest-execution-context=" << identity.guest_execution_context_identity
           << "\n"
           << "execution.thread-id=" << execution.thread_id << "\n"
           << "execution.callback-active=" << (execution.callback_active ? 1 : 0) << "\n"
           << "execution.callback-identity=" << execution.callback_identity << "\n"
           << "adapter.attachment=" << takedown::runtime::to_string(adapter.attachment()) << "\n"
           << "adapter.failure=" << takedown::runtime::to_string(adapter.failure()) << "\n"
           << "adapter.pre-forwarded=" << adapter.pre_instructions_forwarded() << "\n"
           << "adapter.post-forwarded=" << adapter.post_instructions_forwarded() << "\n"
           << "adapter.scalar-writes-forwarded=" << adapter.scalar_writes_forwarded() << "\n"
           << "probe.phase=" << static_cast<unsigned>(probe.phase()) << "\n"
           << "probe.failure=" << takedown::runtime::to_string(probe.failure()) << "\n"
           << "probe.bound=" << (probe.bound() ? 1 : 0) << "\n"
           << "probe.queue-read-write-count=" << probe.queue_read_write_count() << "\n"
           << "probe.queue-read-rolling-digest="
           << hex64(probe.queue_read_aggregate().rolling_digest) << "\n"
           << "probe.complete=" << (result.complete() ? 1 : 0) << "\n"
           << "evidence.present=" << (result.evidence.has_value() ? 1 : 0) << "\n";
    if (result.evidence.has_value()) {
        const auto& evidence = *result.evidence;
        output << "evidence.publication-serial=" << evidence.binding.publication_serial << "\n"
               << "evidence.call-guest-step=" << evidence.call.guest_step << "\n"
               << "evidence.call-vblank=" << evidence.call.boundary.guest_vblank << "\n"
               << "evidence.call-audio-sample=" << evidence.call.boundary.audio_sample << "\n"
               << "evidence.terminal-guest-step=" << evidence.terminal.guest_step << "\n"
               << "evidence.terminal-vblank=" << evidence.terminal.boundary.guest_vblank << "\n"
               << "evidence.terminal-audio-sample=" << evidence.terminal.boundary.audio_sample << "\n"
               << "evidence.return-guest-step=" << evidence.return_path.caller_resume_guest_step << "\n"
               << "evidence.queue-read-count=" << evidence.queue_read_aggregate.count << "\n"
               << "evidence.queue-read-digest=" << hex64(evidence.queue_read_aggregate.rolling_digest) << "\n"
               << "evidence.successful-return=" << (evidence.successful_return ? 1 : 0) << "\n";
    }
    return output.good();
}

// RECOVERY: matches modulo context; orig GIMPLE shows .constprop.0 clone and out-of-line path::_S_convert<char>.
bool exists_on_path(std::string_view executable) {
    const char* raw_path = std::getenv("PATH");
    if (raw_path == nullptr) {
        return false;
    }
    const std::string path_value(raw_path);
    std::size_t begin = 0U;
    while (begin <= path_value.size()) {
        const auto end = path_value.find(':', begin);
        const auto length = end == std::string::npos ? path_value.size() - begin : end - begin;
        const std::filesystem::path directory =
            length == 0U ? std::filesystem::path(".") : std::filesystem::path(path_value.substr(begin, length));
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / std::filesystem::path(executable), error)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return false;
}

Pcsx2CaptureRange parse_pcsx2_capture_range(const std::string_view text) {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U == text.size() ||
        text.find(':', separator + 1U) != std::string_view::npos) {
        throw std::invalid_argument("--range must use <address>:<bytes>, for example 0x00100000:0x00400000");
    }
    const std::uint32_t address = parse_u32(text.substr(0U, separator), "PCSX2 snapshot address");
    const std::uint32_t byte_count = parse_u32(text.substr(separator + 1U), "PCSX2 snapshot byte count");
    const auto end = static_cast<unsigned long long>(address) + byte_count;
    if (byte_count == 0U || (address & 3U) != 0U || (byte_count & 3U) != 0U || address < 0x00100000U ||
        end > 0x02000000ULL) {
        throw std::invalid_argument(
            "--range must be 32-bit aligned and stay inside user EE RAM [0x00100000, 0x02000000)");
    }
    return Pcsx2CaptureRange{address, byte_count};
}

// RECOVERY: matches modulo wchar memcpy vs __builtin_memcpy naming (global include-order artifact); orig is .constprop.0.
std::filesystem::path parse_pcsx2_socket_option(
    const int argc,
    char** argv,
    const int first_option,
    std::optional<std::filesystem::path>& output_directory,
    std::optional<Pcsx2CaptureRange>& range) {
    std::filesystem::path socket_path = takedown::platform::Pcsx2PineClient::default_socket_path();
    bool socket_supplied = false;
    for (int index = first_option; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--socket") {
            if (socket_supplied || index + 1 >= argc) {
                throw std::invalid_argument("--socket must appear once and be followed by a path");
            }
            socket_path = std::filesystem::path(argv[++index]);
            socket_supplied = true;
            continue;
        } else if (argument == "--output") {
            if (output_directory.has_value() || argc <= index + 1) {
                throw std::invalid_argument("--output must appear once and be followed by a directory");
            }
            output_directory = std::filesystem::path(argv[++index]);
            continue;
        } else if (argument == "--range") {
            if (range.has_value() || argc <= index + 1) {
                throw std::invalid_argument("--range must appear once and be followed by a range");
            }
            range = parse_pcsx2_capture_range(argv[++index]);
            continue;
        } else {
            throw std::invalid_argument("unknown PCSX2 PINE option: " + std::string(argument));
        }
    }
    return socket_path;
}

[[nodiscard]] std::string_view pcsx2_status_name(const takedown::platform::Pcsx2PineStatus status) {
    switch (status) {
    case takedown::platform::Pcsx2PineStatus::Running:
        return "running";
    case takedown::platform::Pcsx2PineStatus::Paused:
        return "paused";
    case takedown::platform::Pcsx2PineStatus::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

// RECOVERY: matches modulo context (operator<<.constprop call, inlined optional<path> dtor).
int run_pcsx2_probe(const int argc, char** argv) {
    std::optional<std::filesystem::path> unexpected_output;
    std::optional<Pcsx2CaptureRange> unexpected_range;
    const std::filesystem::path socket_path =
        parse_pcsx2_socket_option(argc, argv, 2, unexpected_output, unexpected_range);
    if (unexpected_output.has_value() || unexpected_range.has_value()) {
        throw std::invalid_argument("pcsx2-probe accepts only --socket <path>");
    }
    auto client = takedown::platform::Pcsx2PineClient::connect(socket_path);
    const auto version = client.version();
    const auto status = client.status();
    std::cout << "PCSX2 PINE read-only probe\n"
              << "  socket:  " << socket_path.string() << '\n'
              << "  version: " << version << '\n'
              << "  status:  " << pcsx2_status_name(status) << '\n';
    if (status == takedown::platform::Pcsx2PineStatus::Shutdown) {
        std::cout << "  note:    no active PS2 virtual machine\n";
        return 2;
    }
    std::cout << "  title:   " << client.title() << '\n' << "  serial:  " << client.serial() << '\n';
    if (status != takedown::platform::Pcsx2PineStatus::Paused) {
        std::cout << "  note:    memory capture is disabled until PCSX2 is paused\n";
        return 0;
    }
    std::cout << "  EE RAM:   bounded Read32 capture available\n"
              << "  GS CRTC: unavailable through PINE ram-only reads; use a GS trace or debugger-specific capture\n";
    return 0;
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
    std::error_code error;
    const std::filesystem::path canonical_candidate = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonical_parent = std::filesystem::weakly_canonical(parent, error);
    if (error) {
        return false;
    }
    auto candidate_part = canonical_candidate.begin();
    for (const auto& parent_part : canonical_parent) {
        if (candidate_part == canonical_candidate.end() || candidate_part->compare(parent_part) != 0) {
            return false;
        }
        ++candidate_part;
    }
    return true;
}

void prepare_pcsx2_snapshot_directory(const std::filesystem::path& directory) {
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        throw std::runtime_error("unable to inspect PCSX2 snapshot directory: " + error.message());
    }
    if (exists) {
        if (!std::filesystem::is_directory(directory, error) || error) {
            throw std::invalid_argument("PCSX2 snapshot output exists and is not a directory: " +
                                        directory.string());
        }
        const bool empty = std::filesystem::is_empty(directory, error);
        if (error) {
            throw std::runtime_error("unable to inspect PCSX2 snapshot directory: " + error.message());
        }
        if (!empty) {
            throw std::invalid_argument("PCSX2 snapshot output directory must be empty: " + directory.string());
        }
        return;
    }
    if (!std::filesystem::create_directories(directory, error) || error) {
        throw std::runtime_error("unable to create PCSX2 snapshot directory: " + error.message());
    }
}

// RECOVERY: structure/strings/order match; helper inlining (prepare_pcsx2_snapshot_directory, path_is_within, parse_pcsx2_capture_range) and out-of-line string helpers cannot be reproduced in the isolated build.
int run_pcsx2_snapshot(const int argc, char** argv) {
    std::optional<std::filesystem::path> output_directory;
    std::optional<Pcsx2CaptureRange> range;
    const std::filesystem::path socket_path =
        parse_pcsx2_socket_option(argc, argv, 2, output_directory, range);
    if (output_directory.has_value() && range.has_value()) {
        if (output_directory->empty()) {
            throw std::invalid_argument("PCSX2 snapshot output directory must not be empty");
        }
        if (path_is_within(*output_directory, std::filesystem::current_path())) {
            throw std::invalid_argument("PCSX2 snapshots may contain user-supplied game code; choose an output "
                                        "directory outside the source tree");
        }
        prepare_pcsx2_snapshot_directory(*output_directory);

        auto client = takedown::platform::Pcsx2PineClient::connect(socket_path);
        const auto version = client.version();
        if (client.status() != takedown::platform::Pcsx2PineStatus::Paused) {
            throw std::runtime_error("PCSX2 must be paused before a local memory snapshot is read");
        }
        const auto title = client.title();
        const auto serial = client.serial();
        const auto bytes = read_pcsx2_capture_range(client, *range);
        const auto range_end = range->address + range->byte_count;
        const auto raw_filename = "ee-ram-" + hex32(range->address) + "-" + hex32(range_end) + ".bin";
        const std::filesystem::path raw_path = *output_directory / raw_filename;
        std::ostringstream metadata;
        metadata << "PCSX2 PINE local read-only snapshot\n"
                 << "version=" << version << '\n'
                 << "title=" << title << '\n'
                 << "serial=" << serial << '\n'
                 << "socket=" << socket_path.string() << '\n'
                 << "range_start=" << hex32(range->address) << '\n'
                 << "range_end=" << hex32(range_end) << '\n'
                 << "byte_count=" << range->byte_count << '\n'
                 << "raw_file=" << raw_filename << '\n'
                 << "gs_crtc=unavailable through PINE ram-only reads\n"
                 << "warning=Contains user-supplied game memory. Keep local and untracked.\n";
        const auto write_raw = [](const std::filesystem::path& file, const std::span<const std::uint8_t>& contents) {
            std::ofstream output(file, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error("unable to create PCSX2 snapshot file: " + file.string());
            }
            output.write(reinterpret_cast<const char*>(contents.data()),
                         static_cast<std::streamsize>(contents.size()));
            if (!output.good()) {
                throw std::runtime_error("unable to write PCSX2 snapshot file: " + file.string());
            }
        };
        const auto write_text = [](const std::filesystem::path& file, const std::string_view& contents) {
            std::ofstream output(file, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error("unable to create PCSX2 snapshot metadata: " + file.string());
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output.good()) {
                throw std::runtime_error("unable to write PCSX2 snapshot metadata: " + file.string());
            }
        };
        write_raw(raw_path, bytes);
        write_text(*output_directory / "snapshot.txt", metadata.str());
        std::cout << "PCSX2 PINE local snapshot complete\n"
                  << "  serial: " << serial << '\n'
                  << "  range:  " << hex32(range->address) << '-' << hex32(range_end) << " (" << range->byte_count
                  << " bytes)\n"
                  << "  output: " << output_directory->string() << '\n'
                  << "  note:   snapshot contents were not printed and remain local/untracked\n";
        return 0;
    }
    throw std::invalid_argument("pcsx2-snapshot requires --output <directory> and --range <address>:<bytes>");
}

// Stop-reason names for the VIF1 source-chain walker, indexed by DmacSourceStop.
constexpr const char* kVif1ChainStopNames[] = {
    "refe",        "end",       "ret-empty", "tie-irq",   "tag-oob",   "payload-oob",
    "addr-ovf",    "tag-limit", "qwc-limit", "call-ovf",  "loop",
};

// RECOVERY: statement-for-statement match; only whole-TU inlining decisions differ in isolation (path(char*) constructor,
// make_unique<ParallelGsRawStreamWriter>, basic_string::_M_create inlined or not).
int run_replay_vif1(const int argc, char** argv) {
    if (argc < 4) {
        throw std::invalid_argument(
            "replay-vif1 requires a raw 32 MiB EE-RAM image, TADR, and optional --tte and --gs-signature, "
            "--raw-gs <output.raw>, --pcsx2-vu-state <extracted-directory>, and --vu1-worker-differential");
    }
    bool tag_transfer_enable = false;
    bool fingerprint_gs_commands = false;
    bool vu1_worker_differential = false;
    bool vu1_worker_async_stage2 = false;
    bool vu1_worker_consume = false;
    std::uint32_t vu1_worker_consume_audit_period = 0;
    std::optional<std::filesystem::path> raw_gs_output;
    std::optional<std::filesystem::path> pcsx2_vu_state_directory;
    for (int index = 4; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--tte" && !tag_transfer_enable) {
            tag_transfer_enable = true;
        } else if (option == "--gs-signature" && !fingerprint_gs_commands) {
            fingerprint_gs_commands = true;
        } else if (option == "--vu1-worker-differential" && !vu1_worker_differential) {
            vu1_worker_differential = true;
        } else if (option == "--vu1-worker-async-stage2" && !vu1_worker_async_stage2) {
            vu1_worker_async_stage2 = true;
        } else if (option == "--vu1-worker-consume" && !vu1_worker_consume) {
            vu1_worker_consume = true;
        } else if (option == "--vu1-worker-consume-audit-period" && vu1_worker_consume_audit_period == 0 &&
                   index + 1 < argc) {
            vu1_worker_consume_audit_period =
                parse_u32(argv[++index], "--vu1-worker-consume-audit-period <N>");
        } else if (option == "--pcsx2-vu-state" && !pcsx2_vu_state_directory && index + 1 < argc) {
            pcsx2_vu_state_directory = std::filesystem::path(argv[++index]);
        } else if (option == "--raw-gs" && !raw_gs_output && index + 1 < argc) {
            raw_gs_output = std::filesystem::path(argv[++index]);
        } else {
            throw std::invalid_argument(
                "replay-vif1 options are --tte, --gs-signature, --raw-gs <output.raw>, "
                "--pcsx2-vu-state <extracted-directory>, --vu1-worker-differential, "
                "--vu1-worker-async-stage2, --vu1-worker-consume, and "
                "--vu1-worker-consume-audit-period <N>, each at most once");
        }
    }
    if (vu1_worker_async_stage2 && vu1_worker_consume) {
        throw std::invalid_argument(
            "--vu1-worker-consume and --vu1-worker-async-stage2 are mutually exclusive (consume replaces "
            "the deferred parity pipeline)");
    }
    takedown::runtime::set_vu1_worker_async_stage2_enabled(vu1_worker_async_stage2);
    takedown::runtime::set_vu1_worker_consume_enabled(vu1_worker_consume);
    const std::uint32_t tadr = parse_u32(argv[3], "VIF1 TADR");
    takedown::runtime::Vu1WorkerDifferentialController vu1_worker_differential_controller;
    if (vu1_worker_differential || vu1_worker_consume) {
        vu1_worker_differential_controller.set_enabled(true);
        if (vu1_worker_async_stage2) {
            vu1_worker_differential_controller.set_deferred(true);
        }
        if (vu1_worker_consume) {
            vu1_worker_differential_controller.set_consume(true);
            vu1_worker_differential_controller.set_consume_audit_period(vu1_worker_consume_audit_period);
        }
        takedown::runtime::set_vu1_worker_differential_controller(&vu1_worker_differential_controller);
    }
    std::unique_ptr<takedown::platform::ParallelGsRawStreamWriter> raw_gs_writer;
    if (raw_gs_output) {
        raw_gs_writer = std::make_unique<takedown::platform::ParallelGsRawStreamWriter>(*raw_gs_output);
    }
    const takedown::runtime::Vif1ReplayResult replay = takedown::runtime::replay_vif1_from_raw_ee_ram(
        std::filesystem::path(argv[2]), tadr,
        takedown::runtime::Vif1ReplayOptions{tag_transfer_enable, fingerprint_gs_commands, raw_gs_writer.get(),
                                             pcsx2_vu_state_directory});
    if (raw_gs_writer != nullptr) {
        raw_gs_writer->close();
    }
    if (vu1_worker_differential || vu1_worker_consume) {
        vu1_worker_differential_controller.join_deferred_worker();
        takedown::runtime::set_vu1_worker_differential_controller(nullptr);
        vu1_worker_differential_controller.report(std::cout);
    }

    const takedown::guest::EeVu1Activity& activity = replay.activity;
    std::cout << "VIF1 raw EE-RAM replay\n"
              << "  start:              tadr=" << hex32(tadr) << " tte=" << (tag_transfer_enable ? "on" : "off")
              << '\n';
    if (replay.pcsx2_vu_seed.loaded) {
        const takedown::runtime::Vif1ReplayResult::Pcsx2VuSeed& seed = replay.pcsx2_vu_seed;
        std::cout << "  PCSX2 VU seed:      loaded VU1 data/micro + " << seed.register_seed_words
                  << " VIF1 register words"
                  << " cycle=" << hex32(seed.cycle) << " mode=" << hex32(seed.mode) << " mask=" << hex32(seed.mask)
                  << " itops=" << hex32(seed.itops) << " base=" << hex32(seed.base)
                  << " offset=" << hex32(seed.offset) << '\n';
    } else {
        std::cout << "  PCSX2 VU seed:      clean device state\n";
    }
    std::cout << "  chain:              starts=" << activity.vif1_chcr_start_writes
              << " ok=" << activity.vif1_chain_completions << " faults=" << activity.vif1_chain_faults
              << " tags=" << activity.vif1_chain_tags_processed << " words=" << activity.vif1_chain_words_submitted
              << '\n'
              << "  tag histogram:      ";
    bool printed_tag = false;
    for (std::size_t id = 0; id != activity.vif1_chain_tag_id_counts.size(); ++id) {
        if (activity.vif1_chain_tag_id_counts[id] == 0) {
            continue;
        }
        std::cout << (printed_tag ? " " : "") << "id" << id << '=' << activity.vif1_chain_tag_id_counts[id];
        printed_tag = true;
    }
    std::cout << (printed_tag ? "\n" : "none\n") << "  VIF stream:         mpg=" << activity.vif1_mpg_uploads
              << " micro-words=" << activity.vif1_micro_words_written << " unpack=" << activity.vif1_unpack_commands
              << '/' << activity.vif1_unpack_vectors_written << " vumem-words=" << activity.vif1_vumem_words_written
              << " unsupported=" << activity.vif1_unsupported_commands;
    if (activity.vif1_unsupported_commands != 0) {
        std::cout << " first=" << hex32(activity.vif1_first_unsupported_word);
    }
    std::cout << '\n'
              << "  VU micro:           runs=" << activity.micro_runs_started << " ok=" << activity.micro_runs_completed
              << " faults=" << activity.micro_run_faults << " pairs=" << activity.micro_pairs_committed
              << " cycles=" << activity.micro_cycles_elapsed << " stalls=" << activity.micro_pipeline_stall_cycles;
    if (activity.saw_mscal) {
        std::cout << " first-mscal=" << hex32(activity.first_mscal_entry);
    }
    std::cout << '\n'
              << "  XGKICK/PATH1:       attempts=" << activity.xgkick_packets
              << " completed=" << activity.xgkick_packets_completed << " faults=" << activity.xgkick_faults
              << " qwords=" << activity.xgkick_quadwords << " oversized=" << activity.xgkick_oversized_tags
              << " oversized-at-submit=" << activity.xgkick_oversized_tags_present_at_submit
              << " start-tag-changed=" << activity.xgkick_start_tags_changed_before_transfer;
    if (activity.xgkick_packets != 0) {
        std::cout << " first-vector=" << hex32(activity.xgkick_first_vector);
    }
    std::cout << '\n' << "  first chain fault:  ";
    if (activity.vif1_chain_faults == 0) {
        std::cout << "none\n";
    } else {
        std::cout << "reason=" << activity.vif1_first_chain_fault_reason
                  << " tadr=" << hex32(activity.vif1_first_chain_fault_tadr) << '\n';
    }
    std::cout << "  first VU fault:     ";
    if (activity.micro_run_faults == 0) {
        std::cout << "none\n";
    } else {
        std::cout << "entry=" << hex32(activity.first_fault_entry) << " pc=" << hex32(activity.first_fault_pc)
                  << " slot=" << takedown::guest::to_string(activity.first_fault_slot)
                  << " status=" << takedown::guest::to_string(activity.first_fault_status)
                  << " lower=" << hex32(activity.first_fault_lower_word)
                  << " upper=" << hex32(activity.first_fault_upper_word) << '\n';
    }
    std::cout << "  last VU fault:      ";
    if (activity.micro_run_faults == 0) {
        std::cout << "none\n";
    } else {
        std::cout << "pc=" << hex32(activity.last_fault_pc) << " lower=" << hex32(activity.last_fault_lower_word)
                  << " upper=" << hex32(activity.last_fault_upper_word) << '\n';
    }
    std::cout << "  VU fault signatures:";
    for (std::size_t index = 0; index < activity.fault_signature_count; ++index) {
        const takedown::guest::EeVu1Activity::FaultSignature& signature = activity.fault_signatures[index];
        std::cout << "\n    [#" << index << "] count=" << signature.count << " entry=" << hex32(signature.entry)
                  << " pc=" << hex32(signature.pc) << " slot=" << takedown::guest::to_string(signature.slot)
                  << " status=" << takedown::guest::to_string(signature.status)
                  << " lower=" << hex32(signature.lower_word) << " upper=" << hex32(signature.upper_word);
    }
    if (activity.fault_signatures_dropped != 0) {
        std::cout << "\n    dropped=" << activity.fault_signatures_dropped;
    }
    std::cout << '\n';
    std::cout << "  final DMAC:         chcr=" << hex32(replay.final_chcr) << " madr=" << hex32(replay.final_madr)
              << " qwc=" << hex32(replay.final_qwc) << " tadr=" << hex32(replay.final_tadr)
              << " asr0=" << hex32(replay.final_asr0) << " asr1=" << hex32(replay.final_asr1)
              << " events=" << replay.completion_events << '/' << replay.queued_completion_events << '\n';
    if (replay.gs_fingerprint.enabled) {
        const takedown::runtime::Vif1ReplayGsFingerprint& fingerprint = replay.gs_fingerprint;
        std::cout << "  GS decoded stream:  fnv1a64=" << hex64(fingerprint.fnv1a64)
                  << " tags=" << fingerprint.gif_tag_begins << " writes=" << fingerprint.register_writes
                  << " images=" << fingerprint.image_transfers << " image-bytes=" << fingerprint.image_payload_bytes
                  << " diagnostics=" << fingerprint.diagnostics
                  << " bounded-writes-omitted=" << fingerprint.capture_activity.omitted_register_writes << '\n'
                  << "  GS signature scope: replay-emitted decoded GIF commands only; raw EE RAM does not "
                     "contain initial GS VRAM or privileged display state, so this is not a scanout\n";
    }
    return activity.vif1_chain_completions != 0 && activity.vif1_chain_faults == 0 ? 0 : 3;
}


// RECOVERY: matches except that the print_circuit lambda is outlined in the release and inlined when built in isolation (inline-unit budget).
void print_gs_dump_display(const takedown::platform::Pcsx2GsDumpCrtcRegisters& registers) {
    using takedown::guest::GsDispfbFields;
    using takedown::guest::GsDisplayFields;

    const takedown::guest::GsPmodeFields pmode = takedown::guest::decode_gs_pmode(registers.pmode);
    const takedown::guest::GsSmode2Fields smode2 = takedown::guest::decode_gs_smode2(registers.smode2);
    const GsDispfbFields dispfb1 = takedown::guest::decode_gs_dispfb(registers.dispfb1);
    const GsDisplayFields display1 = takedown::guest::decode_gs_display(registers.display1);
    const GsDispfbFields dispfb2 = takedown::guest::decode_gs_dispfb(registers.dispfb2);
    const GsDisplayFields display2 = takedown::guest::decode_gs_display(registers.display2);
    const takedown::guest::GsBgcolorFields bgcolor = takedown::guest::decode_gs_bgcolor(registers.bgcolor);
    std::cout << "  final GS CRTC metadata\n"
              << "    PMODE: EN1=" << std::string(pmode.circuit1_enabled ? "on" : "off")
              << " EN2=" << std::string(pmode.circuit2_enabled ? "on" : "off")
              << " CRTMD=" << static_cast<unsigned>(pmode.crt_mode)
              << " MMOD=" << std::string(pmode.mmod ? "on" : "off")
              << " AMOD=" << std::string(pmode.amod ? "on" : "off")
              << " SLBG=" << std::string(pmode.slbg ? "on" : "off")
              << " ALP=" << static_cast<unsigned>(pmode.alpha) << '\n'
              << "    SMODE2: INT=" << std::string(smode2.interlaced ? "on" : "off")
              << " FFMD=" << std::string(smode2.field_frame_mode ? "on" : "off")
              << " DPMS=" << static_cast<unsigned>(smode2.display_power_management) << '\n';
    const auto print_circuit = [](const std::uint32_t index, const GsDispfbFields& dispfb,
                                  const GsDisplayFields& display) {
        std::cout << "    circuit " << index << ": FBP=" << dispfb.framebuffer_base_pointer
                  << " FBW=" << static_cast<unsigned>(dispfb.framebuffer_width)
                  << " PSM=" << static_cast<unsigned>(dispfb.pixel_storage_mode)
                  << " DBX=" << dispfb.display_x << " DBY=" << dispfb.display_y
                  << " | DX=" << display.display_x << " DY=" << display.display_y
                  << " MAGH=" << static_cast<unsigned>(display.horizontal_magnification)
                  << " MAGV=" << static_cast<unsigned>(display.vertical_magnification)
                  << " DW=" << display.display_width_raw << " DH=" << display.display_height_raw
                  << '\n';
    };
    print_circuit(1U, dispfb1, display1);
    print_circuit(2U, dispfb2, display2);
    std::cout << "    BGCOLOR: R=" << static_cast<unsigned>(bgcolor.red)
              << " G=" << static_cast<unsigned>(bgcolor.green)
              << " B=" << static_cast<unsigned>(bgcolor.blue) << '\n';
}

// RECOVERY: matches except operator<<(char)/operator<<(const char*) inlining, which depends on the whole translation unit.
void print_gs_dump_gif_statistics(const takedown::platform::Pcsx2GsDumpGifStatistics& gif) {
    struct NamedRegister {
        std::uint8_t address;
        std::string_view name;
    };
    const std::array<std::string_view, 8> primitive_names{
        "point", "line", "line-strip", "triangle", "triangle-strip", "triangle-fan", "sprite", "reserved",
    };
    const std::array<NamedRegister, 33> named_registers{{
        {0x00U, "PRIM"},
        {0x01U, "RGBAQ"},
        {0x02U, "ST"},
        {0x03U, "UV"},
        {0x04U, "XYZF2"},
        {0x05U, "XYZ2"},
        {0x06U, "TEX0_1"},
        {0x07U, "TEX0_2"},
        {0x08U, "CLAMP_1"},
        {0x09U, "CLAMP_2"},
        {0x0cU, "XYZF3"},
        {0x0dU, "XYZ3"},
        {0x14U, "TEX1_1"},
        {0x15U, "TEX1_2"},
        {0x18U, "XYOFFSET_1"},
        {0x19U, "XYOFFSET_2"},
        {0x1aU, "PRMODECONT"},
        {0x1bU, "PRMODE"},
        {0x3bU, "TEXA"},
        {0x40U, "SCISSOR_1"},
        {0x41U, "SCISSOR_2"},
        {0x42U, "ALPHA_1"},
        {0x43U, "ALPHA_2"},
        {0x47U, "TEST_1"},
        {0x48U, "TEST_2"},
        {0x4cU, "FRAME_1"},
        {0x4dU, "FRAME_2"},
        {0x4eU, "ZBUF_1"},
        {0x4fU, "ZBUF_2"},
        {0x50U, "BITBLTBUF"},
        {0x51U, "TRXPOS"},
        {0x52U, "TRXREG"},
        {0x53U, "TRXDIR"},
    }};

    std::cout << "  GIF transfer slots: path1-old=" << gif.transfer_packets_by_path[0] << '/'
              << gif.transfer_bytes_by_path[0] << "B path2=" << gif.transfer_packets_by_path[1] << '/'
              << gif.transfer_bytes_by_path[1] << "B path3=" << gif.transfer_packets_by_path[2] << '/'
              << gif.transfer_bytes_by_path[2] << "B path1-new=" << gif.transfer_packets_by_path[3] << '/'
              << gif.transfer_bytes_by_path[3] << "B\n";
    if (!gif.statistics_available) {
        std::cout << "  GIF tag analysis: unavailable for this serialized GS-state version\n";
        return;
    }
    std::cout << "  GIF tags: PACKED=" << gif.gif_tags_by_format[0] << " REGLIST=" << gif.gif_tags_by_format[1]
              << " IMAGE=" << gif.gif_tags_by_format[2] << " IMAGE2=" << gif.gif_tags_by_format[3]
              << " image-qw=" << gif.image_payload_quadwords << '\n'
              << "  GS draw kicks: " << gif.draw_kicks;
    if (gif.draw_kicks_without_primitive != 0) {
        std::cout << " without-PRIM=" << gif.draw_kicks_without_primitive;
    }
    std::cout << '\n';
    for (std::size_t primitive = 0; primitive < primitive_names.size(); ++primitive) {
        if (gif.primitive_state_writes[primitive] == 0 && gif.draw_kicks_by_primitive[primitive] == 0) {
            continue;
        }
        std::cout << "    " << primitive_names[primitive] << ": PRIM-writes=" << gif.primitive_state_writes[primitive]
                  << " draw-kicks=" << gif.draw_kicks_by_primitive[primitive] << '\n';
    }

    std::uint64_t total_register_writes = 0;
    for (const auto count : gif.register_writes_by_address) {
        total_register_writes += count;
    }
    std::cout << "  selected A+D GS register writes (total " << total_register_writes << "):\n";
    std::uint64_t named_register_writes = 0;
    for (const NamedRegister& named : named_registers) {
        const auto count = gif.register_writes_by_address[named.address];
        named_register_writes += count;
        if (count != 0) {
            std::cout << "    " << named.name << '=' << count << '\n';
        }
    }
    if (total_register_writes > named_register_writes) {
        std::cout << "    other=" << total_register_writes - named_register_writes << '\n';
    }
    std::uint64_t direct_descriptor_writes = 0;
    for (const auto count : gif.direct_register_descriptor_counts) {
        direct_descriptor_writes += count;
    }
    std::cout << "  direct GIF register descriptors: " << direct_descriptor_writes << '\n';
    if (gif.zero_loop_tags != 0 || gif.adc_suppressed_kicks != 0 || gif.nonkicking_xyz_writes != 0 ||
        gif.path0_incomplete_resets != 0 || gif.unsupported_transfer_path_packets != 0 || gif.incomplete_paths != 0) {
        std::cout << "  GIF analysis boundaries: unsupported-path-packets=" << gif.unsupported_transfer_path_packets
                  << " incomplete-paths=" << gif.incomplete_paths
                  << " path1-old-resets=" << gif.path0_incomplete_resets
                  << " zero-loop-tags=" << gif.zero_loop_tags << " ADC-suppressed=" << gif.adc_suppressed_kicks
                  << " XYZ3/non-kicks=" << gif.nonkicking_xyz_writes << '\n';
    }
}

// RECOVERY: matches except std inlining decisions (string_view::compare, basic_string::_M_create, _Save_errno, ostream
// operator<< for char/const char*) that depend on the whole translation unit.
int run_save_data(int argc, char** argv) {
    if (argc < 4) {
        throw std::invalid_argument(
            "save-data requires a root directory and a subcommand (list | read | write | import-memcard | slots | "
            "import)");
    }
    const std::filesystem::path root = argv[2];
    const std::string_view subcommand = argv[3];
    std::error_code error;

    if (subcommand == "slots") {
        const std::size_t active_slot = takedown::save::read_active_profile_slot(root);
        for (const auto& slot : takedown::save::list_profile_slots(root)) {
            std::cout << (slot.slot == active_slot ? "* " : "  ") << takedown::save::describe_profile_slot(slot)
                      << '\n';
        }
        const std::vector<takedown::save::ImportSource> sources = takedown::save::detect_import_sources(root, error);
        for (const takedown::save::ImportSource& source : sources) {
            std::error_code summary_error;
            const std::optional<takedown::save::Pcsx2CardSummary> summary =
                takedown::save::summarize_import_source(source, std::filesystem::path{}, summary_error);
            std::cout << "  import source: " << source.path.string();
            if (summary && summary->profile_name) {
                std::cout << " (profile " << *summary->profile_name << ')';
            }
            std::cout << '\n';
        }
        return 0;
    }

    if (subcommand == "import") {
        if (argc < 6) {
            throw std::invalid_argument("save-data import requires <source> <slot>");
        }
        const std::optional<takedown::save::ImportSource> source = takedown::save::classify_import_path(argv[4]);
        if (!source) {
            throw std::invalid_argument("import source is not a .ps2 file, a folder card, or a save folder");
        }
        const std::size_t slot = std::stoul(argv[5]);
        const bool replace = argc > 6 && std::string_view(argv[6]) == "--replace";
        std::filesystem::path backup;
        if (!takedown::save::import_into_profile_slot(
                root, *source, slot,
                replace ? takedown::save::SlotOverwrite::backup_and_replace : takedown::save::SlotOverwrite::refuse,
                error, &backup)) {
            throw std::runtime_error("import failed: " + error.message() +
                                     (replace ? "" : " (use --replace to back up and replace)"));
        }
        std::cout << takedown::save::describe_profile_slot(takedown::save::summarize_profile_slot(root, slot))
                  << '\n';
        if (!backup.empty()) {
            std::cout << "previous save data kept in " << backup.string() << '\n';
        }
        return 0;
    }

    const takedown::save::SaveDataStore store(root, error);
    if (error) {
        throw std::runtime_error("cannot open save-data root: " + error.message());
    }

    if (subcommand == "list") {
        const std::vector<std::string> saves = store.list_saves(error);
        if (error) {
            throw std::runtime_error("listing save data failed: " + error.message());
        }
        std::cout << "save data under " << root.string() << ":\n";
        for (const std::string& save : saves) {
            std::cout << "  " << save << '\n';
        }
        return 0;
    }

    if (subcommand == "read") {
        if (argc == 4) {
            throw std::invalid_argument("save-data read requires <save>");
        }
        const std::string_view save_name = argv[4];
        const bool print_bytes = argc > 5 && std::string_view(argv[5]) == "--print-bytes";
        const std::optional<std::vector<takedown::save::SaveFile>> save = store.read_save(save_name, error);
        if (error) {
            throw std::runtime_error("reading save data failed: " + error.message());
        }
        if (!save) {
            throw std::runtime_error("save data not found: " + std::string(save_name));
        }
        std::cout << "save " << save_name << ":\n";
        for (const takedown::save::SaveFile& file : *save) {
            std::cout << "  " << file.name << " (" << file.content.size() << " bytes)\n";
            if (print_bytes) {
                std::cout.write(reinterpret_cast<const char*>(file.content.data()),
                                static_cast<std::streamsize>(file.content.size()));
                std::cout << '\n';
            }
        }
        return 0;
    }

    if (subcommand == "write") {
        if (argc < 7) {
            throw std::invalid_argument("save-data write requires <save> <file> <host-file>");
        }
        const std::string_view save_name = argv[4];
        const std::string_view file_name = argv[5];
        const std::filesystem::path host_file = argv[6];
        if (!store.has_save(save_name) && !store.create_save(save_name, error)) {
            throw std::runtime_error("creating save data failed: " + error.message());
        }
        std::ifstream input(host_file, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open host file: " + host_file.string());
        }
        std::vector<std::byte> content;
        std::array<std::byte, 4096> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_count = input.gcount();
            if (read_count > 0) {
                content.insert(content.end(), buffer.cbegin(), buffer.cbegin() + read_count);
            }
        }
        if (!store.write_save_file(save_name, file_name, content, error)) {
            throw std::runtime_error("writing save data failed: " + error.message());
        }
        std::cout << "wrote " << content.size() << " bytes to save " << save_name << " file " << file_name << '\n';
        return 0;
    }

    if (subcommand == "import-memcard") {
        if (argc == 4) {
            throw std::invalid_argument("save-data import-memcard requires <memcard.ps2>");
        }
        const std::filesystem::path image_path = argv[4];
        std::ifstream input(image_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open save-card image: " + image_path.string());
        }
        std::vector<std::byte> image_bytes;
        std::array<std::byte, 4096> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_count = input.gcount();
            if (read_count > 0) {
                image_bytes.insert(image_bytes.end(), buffer.cbegin(), buffer.cbegin() + read_count);
            }
        }
        const std::optional<std::vector<takedown::save::CardSaveDirectory>> directories =
            takedown::save::read_import_source(
                takedown::save::ImportSource{takedown::save::ImportSourceKind::card_image, image_path}, error);
        if (!directories) {
            throw std::runtime_error("reading save-card image failed: " + error.message());
        }
        for (const takedown::save::CardSaveDirectory& directory : *directories) {
            if (!store.has_save(directory.name) && !store.create_save(directory.name, error)) {
                throw std::runtime_error("creating save data failed: " + error.message());
            }
            for (const takedown::save::CardFile& file : directory.files) {
                if (!store.write_save_file(directory.name, file.name, file.content, error)) {
                    throw std::runtime_error("writing save data failed: " + error.message());
                }
            }
            std::cout << "imported save " << directory.name << " (" << directory.files.size() << " files)\n";
        }
        return 0;
    }

    throw std::invalid_argument("unknown save-data subcommand: " + std::string(subcommand));
}

int run_import(int argc, char** argv) {
    if (argc < 5) {
        throw std::invalid_argument("import requires an image and --output <directory>");
    }
    const std::filesystem::path source_image = argv[2];
    std::filesystem::path output_directory;
    takedown::content::ImportOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--allow-unknown") {
            options.allow_unknown = true;
            continue;
        } else if (argument == "--output") {
            if (index + 1 >= argc) {
                throw std::invalid_argument("--output requires a directory");
            }
            output_directory = std::filesystem::path(argv[++index]);
            continue;
        } else {
            throw std::invalid_argument("unknown import option: " + std::string(argument));
        }
    }
    if (output_directory.empty()) {
        throw std::invalid_argument("import requires --output <directory>");
    }

    std::cout << "Importing user-supplied game content...\n";
    const takedown::content::ImportedGame game =
        takedown::content::import_game_content(source_image, output_directory, options);
    std::cout << "Import complete\n"
              << "  disc ID:       " << game.disc_id << '\n'
              << "  source format: " << takedown::content::to_string(game.source_format) << '\n'
              << "  boot ELF:      " << game.boot_elf.string() << '\n'
              << "  entry point:   " << std::showbase << std::hex << game.executable.entry_point << std::dec << '\n'
              << "  content root:  " << game.root.string() << '\n'
              << "  manifest:      " << game.manifest.string() << '\n';
    return 0;
}


[[nodiscard]] std::string elf_segment_permissions(const takedown::content::Elf32LoadSegment& segment) {
    std::string permissions = "---";
    if (segment.readable()) {
        permissions[0] = 'r';
    }
    if (segment.writable()) {
        permissions[1] = 'w';
    }
    if (segment.executable()) {
        permissions[2] = 'x';
    }
    return permissions;
}

int run_inspect_elf(int argc, char** argv) {
    if (argc != 3) {
        throw std::invalid_argument("inspect-elf requires one boot ELF path");
    }

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);

    std::cout << "PS2 ELF load plan\n"
              << "  entry point:    " << std::showbase << std::hex << plan.executable.entry_point << '\n'
              << "  required RAM:   " << std::dec << plan.required_memory_size() << " bytes\n"
              << "  load segments:  " << plan.segments.size() << '\n';
    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        const auto& segment = plan.segments[index];
        std::cout << "  [" << std::dec << index << "] " << elf_segment_permissions(segment)
                  << " vaddr=" << std::showbase << std::hex << segment.virtual_address
                  << " file=" << std::dec << segment.file_size << " memory=" << segment.memory_size
                  << " offset=" << std::showbase << std::hex << segment.file_offset << '\n';
    }
    std::cout << std::dec;
    return 0;
}

takedown::recompiler::R5900WordExtractionResult
extract_elf_words_or_throw(const takedown::guest::EeMemory& memory,
                           const takedown::content::Elf32LoadPlan& plan,
                           const takedown::recompiler::R5900WordExtractionRequest& request) {
    auto extraction = takedown::recompiler::extract_elf_r5900_words(memory, plan, request);
    if (!extraction.succeeded()) {
        const auto message = extraction.diagnostics.empty() ? std::string("unknown extraction failure")
                                                            : extraction.diagnostics.front().message;
        throw std::invalid_argument("cannot extract bounded retail code at " +
                                    hex32(request.start_address) + ": " + message);
    }
    return extraction;
}

[[nodiscard]] std::string_view cfg_edge_kind_name(const takedown::recompiler::R5900CfgEdgeKind kind) {
    using takedown::recompiler::R5900CfgEdgeKind;
    switch (kind) {
    case R5900CfgEdgeKind::Fallthrough:
        return "fallthrough";
    case R5900CfgEdgeKind::BranchTaken:
        return "branch-taken";
    case R5900CfgEdgeKind::BranchNotTaken:
        return "branch-not-taken";
    case R5900CfgEdgeKind::Jump:
        return "jump";
    case R5900CfgEdgeKind::Call:
        return "call";
    case R5900CfgEdgeKind::CallReturn:
        return "call-return";
    case R5900CfgEdgeKind::Return:
        return "return";
    case R5900CfgEdgeKind::IndirectJump:
        return "indirect-jump";
    case R5900CfgEdgeKind::IndirectCall:
        return "indirect-call";
    case R5900CfgEdgeKind::Exception:
        return "exception";
    }
    return "unknown";
}

[[nodiscard]] std::string_view
delay_slot_disposition_name(const takedown::recompiler::R5900DelaySlotDisposition disposition) {
    using takedown::recompiler::R5900DelaySlotDisposition;
    switch (disposition) {
    case R5900DelaySlotDisposition::None:
        return "none";
    case R5900DelaySlotDisposition::Execute:
        return "execute";
    case R5900DelaySlotDisposition::Annul:
        return "annul";
    }
    return "unknown";
}

[[nodiscard]] std::string_view
block_delay_slot_policy_name(const takedown::recompiler::R5900BlockDelaySlotPolicy policy) {
    using takedown::recompiler::R5900BlockDelaySlotPolicy;
    switch (policy) {
    case R5900BlockDelaySlotPolicy::None:
        return "none";
    case R5900BlockDelaySlotPolicy::Always:
        return "always";
    case R5900BlockDelaySlotPolicy::TakenOnly:
        return "taken-only";
    }
    return "unknown";
}

[[nodiscard]] std::string_view cfg_blocker_kind_name(const takedown::recompiler::R5900CfgBlockerKind kind) {
    using takedown::recompiler::R5900CfgBlockerKind;
    switch (kind) {
    case R5900CfgBlockerKind::InvalidExecutableSpan:
        return "invalid-executable-span";
    case R5900CfgBlockerKind::MisalignedEntry:
        return "misaligned-entry";
    case R5900CfgBlockerKind::EntryOutsideExecutable:
        return "entry-outside-executable";
    case R5900CfgBlockerKind::InstructionLimitReached:
        return "instruction-limit";
    case R5900CfgBlockerKind::UnreadableInstruction:
        return "unreadable-instruction";
    case R5900CfgBlockerKind::UnknownInstruction:
        return "unknown-instruction";
    case R5900CfgBlockerKind::MissingDelaySlot:
        return "missing-delay-slot";
    case R5900CfgBlockerKind::ControlFlowInDelaySlot:
        return "control-flow-in-delay-slot";
    case R5900CfgBlockerKind::TargetOutsideExecutable:
        return "target-outside-executable";
    case R5900CfgBlockerKind::AddressOverflow:
        return "address-overflow";
    case R5900CfgBlockerKind::IndirectJumpTarget:
        return "indirect-jump-target";
    case R5900CfgBlockerKind::IndirectCallTarget:
        return "indirect-call-target";
    case R5900CfgBlockerKind::ExceptionControlFlow:
        return "exception-control-flow";
    }
    return "unknown";
}

int run_analyze_function(const int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        throw std::invalid_argument(
            "analyze-function requires an ELF path, entry address, and optional decode limit");
    }

    const GuestAddress entry_address = parse_u32(argv[3], "function entry address");
    const auto instruction_limit =
        argc == 5 ? parse_bounded_count(argv[4], "function decode limit", 65536U) : std::size_t{4096U};

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    const auto segment =
        std::find_if(plan.segments.begin(), plan.segments.end(), [entry_address](const auto& candidate) {
            const auto begin = static_cast<std::uint64_t>(candidate.virtual_address);
            const auto end = begin + candidate.file_size;
            return candidate.executable() && entry_address >= begin && entry_address < end;
        });
    if (segment == plan.segments.end()) {
        throw std::out_of_range("function entry is outside file-backed executable ELF data: " +
                                hex32(entry_address));
    }
    if ((segment->virtual_address & 3U) != 0U || segment->file_size < 4U) {
        throw std::invalid_argument("the containing executable ELF segment is not a word-aligned R5900 range");
    }

    const auto segment_word_count = segment->file_size / sizeof(std::uint32_t);
    const auto extraction = extract_elf_words_or_throw(
        memory, plan,
        {
            .start_address = segment->virtual_address,
            .instruction_count = segment_word_count,
            .maximum_instruction_count = memory.size() / 4U,
        });
    const auto cfg = takedown::recompiler::analyze_r5900_function_cfg(
        entry_address, extraction.as_executable_word_span(), instruction_limit);

    std::cout << "R5900 function CFG\n"
              << "  entry point:    " << hex32(entry_address) << '\n'
              << "  decoded words:  " << cfg.instructions.size() << '\n'
              << "  basic blocks:   " << cfg.blocks.size() << '\n'
              << "  blockers:       " << cfg.blockers.size() << '\n'
              << "  truncated:      " << (cfg.truncated ? "yes" : "no") << '\n'
              << "  CFG complete:   " << (cfg.is_liftable() ? "yes" : "no") << '\n';
    std::cout << "  instructions:\n";
    for (const auto& instruction : cfg.instructions) {
        std::cout << "    " << hex32(instruction.decoded.pc) << "  " << hex32(instruction.decoded.word) << "  "
                  << takedown::guest::format_r5900_instruction(instruction.decoded);
        if (instruction.referenced_as_delay_slot) {
            std::cout << "  [delay slot]";
        }
        std::cout << '\n';
    }
    std::cout << "  blocks:\n";
    for (const auto& block : cfg.blocks) {
        std::cout << "    " << hex32(block.entry_address) << " words=";
        for (std::size_t index = 0; index < block.instruction_addresses.size(); ++index) {
            std::cout << (index == 0 ? "" : ",") << hex32(block.instruction_addresses[index]);
        }
        std::cout << " slot-policy=" << block_delay_slot_policy_name(block.delay_slot_policy);
        if (block.delay_slot_address) {
            std::cout << " slot=" << hex32(*block.delay_slot_address);
        }
        std::cout << '\n';
        for (const auto& edge : block.outgoing_edges) {
            std::cout << "      " << cfg_edge_kind_name(edge.kind) << " -> ";
            if (edge.target_address) {
                std::cout << hex32(*edge.target_address)
                          << (edge.target_is_in_executable ? " [executable]" : " [external]");
            } else {
                std::cout << "dynamic";
            }
            std::cout << " slot=" << delay_slot_disposition_name(edge.delay_slot) << '\n';
        }
    }
    if (!cfg.blockers.empty()) {
        std::cout << "  blockers:\n";
        for (const auto& blocker : cfg.blockers) {
            std::cout << "    " << cfg_blocker_kind_name(blocker.kind) << " at " << hex32(blocker.address);
            if (blocker.related_address) {
                std::cout << " related=" << hex32(*blocker.related_address);
            }
            std::cout << '\n';
        }
    }
    return cfg.is_liftable() ? 0 : 3;
}

int run_lift_function_cpp(const int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        throw std::invalid_argument(
            "lift-function-cpp requires an ELF path, address, instruction count, and optional name");
    }

    const GuestAddress entry_address = parse_u32(argv[3], "function entry address");
    const std::size_t instruction_count = parse_bounded_count(argv[4], "function instruction count", 4096U);
    const std::string_view suggested_name = argc == 6 ? std::string_view(argv[5]) : std::string_view{};

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    const auto extraction = extract_elf_words_or_throw(
        memory, plan,
        {
            .start_address = entry_address,
            .instruction_count = instruction_count,
            .maximum_instruction_count = 4096U,
        });
    const auto lifted = takedown::recompiler::lift_r5900_leaf_function_to_cpp(
        entry_address, extraction.words, suggested_name);
    if (!lifted.succeeded()) {
        std::cerr << "C++ lift refused " << hex32(entry_address) << ":\n";
        for (const auto& diagnostic : lifted.diagnostics) {
            std::cerr << "  " << hex32(diagnostic.pc) << ' ' << hex32(diagnostic.word) << ": "
                      << diagnostic.message << '\n';
        }
        return 3;
    }
    std::cout << lifted.cpp_source;
    return 0;
}

int run_lift_instruction_block_cpp(const int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        throw std::invalid_argument(
            "lift-instruction-block-cpp requires an ELF path, instruction address, and optional name");
    }

    const GuestAddress address = parse_u32(argv[3], "instruction address");
    const std::string_view suggested_name = argc == 5 ? std::string_view(argv[4]) : std::string_view{};

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    const auto extraction = extract_elf_words_or_throw(
        memory, plan,
        {
            .start_address = address,
            .instruction_count = 1U,
            .maximum_instruction_count = 1U,
        });
    const auto lifted = takedown::recompiler::lift_r5900_instruction_to_compiled_block_cpp(
        address, extraction.words.front(), suggested_name);
    if (!lifted.succeeded()) {
        const auto& diagnostic = *lifted.diagnostic;
        std::cerr << "Compiled-block lift refused " << hex32(address) << ":\n"
                  << "  " << hex32(diagnostic.instruction.pc) << ' ' << hex32(diagnostic.instruction.word)
                  << ": " << diagnostic.message << '\n';
        return 3;
    }
    std::cout << lifted.cpp_source;
    return 0;
}

int run_generate_block_module_cpp(const int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        throw std::invalid_argument(
            "generate-block-module-cpp requires an ELF path, start address, instruction count, and optional "
            "generated-block limit");
    }

    const GuestAddress start_address = parse_u32(argv[3], "compiled-block module start address");
    const std::size_t instruction_count =
        parse_bounded_count(argv[4], "compiled-block module instruction count", 65536U);
    const auto generated_block_limit =
        argc == 6 ? parse_bounded_count(argv[5], "generated block limit", 65536U) : instruction_count;

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    const auto extraction = extract_elf_words_or_throw(
        memory, plan,
        {
            .start_address = start_address,
            .instruction_count = instruction_count,
            .maximum_instruction_count = 65536U,
        });
    const auto module =
        takedown::recompiler::build_r5900_compiled_block_module(extraction, generated_block_limit);
    std::cerr << "Generated local resumable R5900 block module: accepted=" << module.accepted_count()
              << " refused=" << module.lifter_rejected_count()
              << " over-limit=" << module.generation_limit_exceeded_count()
              << " supplied=" << module.inventory.size() << '\n'
              << "Keep this game-derived source only under the ignored local-analysis/ tree.\n";
    std::cout << module.cpp_source;
    return 0;
}

int run_generate_sparse_block_module_cpp(const int argc, char** argv) {
    if (argc <= 3) {
        throw std::invalid_argument("generate-sparse-block-module-cpp requires an ELF path and at least one "
                                    "<start-address>:<instruction-count> range");
    }

    constexpr std::size_t kMaximumRanges = 256U;
    constexpr std::size_t kMaximumSuppliedWords = 262144U;
    const auto range_count = static_cast<std::size_t>(argc - 3);
    if (range_count > kMaximumRanges) {
        throw std::invalid_argument("sparse block module accepts at most " + std::to_string(kMaximumRanges) +
                                    " ranges");
    }

    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    std::vector<takedown::recompiler::R5900WordExtractionResult> extractions;
    extractions.reserve(range_count);
    std::size_t total_words = 0U;
    for (int argument_index = 3; argument_index < argc; ++argument_index) {
        const std::string_view range = argv[argument_index];
        const auto separator = range.find(':');
        if (separator == std::string_view::npos || separator == 0U || separator + 1U == range.size() ||
            range.find(':', separator + 1U) != std::string_view::npos) {
            throw std::invalid_argument("sparse block range must use <start-address>:<instruction-count>");
        }
        const GuestAddress start_address = parse_u32(range.substr(0U, separator), "sparse block range start address");
        const std::size_t instruction_count = parse_bounded_count(
            range.substr(separator + 1U), "sparse block range instruction count", 65536U);
        if (instruction_count > kMaximumSuppliedWords - total_words) {
            throw std::invalid_argument("sparse block module exceeds the " +
                                        std::to_string(kMaximumSuppliedWords) + " supplied-word limit");
        }
        total_words += instruction_count;
        extractions.push_back(extract_elf_words_or_throw(memory, plan,
                                                         {
                                                             .start_address = start_address,
                                                             .instruction_count = instruction_count,
                                                             .maximum_instruction_count = 65536U,
                                                         }));
    }

    const auto module = takedown::recompiler::build_r5900_compiled_block_module(extractions, total_words);
    std::cerr << "Generated local sparse R5900 block module: ranges=" << extractions.size()
              << " accepted=" << module.accepted_count() << " refused=" << module.lifter_rejected_count()
              << " over-limit=" << module.generation_limit_exceeded_count()
              << " supplied=" << module.inventory.size() << '\n'
              << "Keep this game-derived source only under the ignored local-analysis/ tree.\n";
    std::cout << module.cpp_source;
    return 0;
}

int run_generate_leaf_module_cpp(const int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        throw std::invalid_argument("generate-leaf-module-cpp requires an ELF path and optional "
                                    "generated-function limit");
    }

    const auto generated_function_limit =
        argc == 4 ? parse_bounded_count(argv[3], "generated function limit", 8192U) : 4096U;
    const std::filesystem::path elf_path(argv[2]);
    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(elf_path, memory);
    const auto analysis = takedown::content::analyze_ps2_elf_static(elf_path);
    const auto module = takedown::recompiler::build_r5900_leaf_module(
        memory, plan, analysis,
        {
            .maximum_direct_call_candidates = 20000U,
            .maximum_instructions_per_function = 256U,
            .maximum_generated_functions = generated_function_limit,
        });
    std::cerr << "Generated local R5900 leaf module: accepted=" << module.accepted_count()
              << " refused=" << module.refused_count() << " candidates=" << module.inventory.size() << '\n'
              << "Keep this game-derived source only under the ignored local-analysis/ tree.\n";
    std::cout << module.cpp_source;
    return 0;
}

int run_decode_entry(const int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        throw std::invalid_argument("decode-entry requires a boot ELF path and optional instruction count");
    }

    const auto instruction_count = argc == 4 ? parse_bounded_count(argv[3], "instruction count", 4096U) : 128U;
    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);

    std::cout << "R5900 decode at ELF entry\n"
              << "  entry point:   " << hex32(plan.executable.entry_point) << '\n'
              << "  instructions: " << instruction_count << '\n';
    auto pc = plan.executable.entry_point;
    for (std::size_t index = 0; index < instruction_count; ++index, pc += 4U) {
        if ((pc & 3U) != 0U || !plan.contains_executable_file_range(pc, 4U)) {
            throw std::out_of_range("decode reached outside file-backed executable data at " + hex32(pc));
        }
        const std::uint32_t word = memory.read_u32_le(pc);
        const auto instruction = takedown::guest::decode_r5900(pc, word);
        std::cout << "  " << hex32(pc) << "  " << hex32(word) << "  "
                  << takedown::guest::format_r5900_instruction(instruction) << '\n';
    }
    return 0;
}

int run_probe_entry(const int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        throw std::invalid_argument("probe-entry requires a boot ELF path and optional step limit");
    }

    const auto maximum_steps =
        argc == 4 ? parse_bounded_count(argv[3], "step limit", 50000000U) : 20000000U;
    takedown::guest::EeMemory memory(takedown::guest::kEeMainRamSize);
    memory.configure_ram_write_watch();
    const auto plan = takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    takedown::guest::R5900State state(plan.executable.entry_point);

    takedown::guest::EeStepResult stop{};
    stop.status = takedown::guest::EeStepStatus::FetchFault;
    std::size_t executed = 0U;
    bool outside_executable_data = false;
    for (; executed < maximum_steps; ++executed) {
        if (!plan.contains_executable_file_range(state.pc(), 4U)) {
            outside_executable_data = true;
            break;
        }
        const auto result = takedown::guest::step_r5900(state, memory);
        if (result.status != takedown::guest::EeStepStatus::Executed) {
            stop = result;
            break;
        }
    }

    std::cout << "R5900 reference-execution probe\n"
              << "  entry point: " << hex32(plan.executable.entry_point) << '\n'
              << "  executed:    " << executed << " instruction(s)\n";
    if (executed == maximum_steps) {
        std::cout << "  stop reason: configured step limit\n"
                  << "  next PC:     " << hex32(state.pc()) << '\n';
        return 0;
    }
    if (outside_executable_data) {
        std::cout << "  stop reason: outside file-backed executable data\n"
                  << "  stop PC:     " << hex32(state.pc()) << '\n';
        return 0;
    }

    using takedown::guest::EeStepStatus;
    std::cout << "  stop reason: " << takedown::guest::to_string(stop.status) << '\n'
              << "  stop PC:     " << hex32(stop.instruction.pc) << '\n';
    if (stop.status == EeStepStatus::UnsupportedInstruction ||
        stop.status == EeStepStatus::UnsupportedOperandCase ||
        stop.status == EeStepStatus::ArithmeticOverflow || stop.status == EeStepStatus::GuestSystemCall ||
        stop.status == EeStepStatus::MemoryAccessFault || stop.status == EeStepStatus::Vu0MicroExecutionFault) {
        std::cout << "  raw word:    " << hex32(stop.instruction.word) << '\n'
                  << "  decoded as:  " << takedown::guest::format_r5900_instruction(stop.instruction) << '\n';
    }
    if (stop.vu0_micro) {
        const auto& micro = *stop.vu0_micro;
        std::cout << "  VU0 micro status: " << takedown::guest::to_string(micro.status) << '\n'
                  << "  VU0 micro pair:   " << hex32(micro.pc) << '\n'
                  << "  VU0 micro slot:   " << takedown::guest::to_string(micro.slot) << '\n'
                  << "  VU0 lower word:   " << hex32(micro.lower_word) << '\n'
                  << "  VU0 upper word:   " << hex32(micro.upper_word) << '\n'
                  << "  VU0 pairs run:    " << micro.committed_pairs << '\n';
    }
    if (stop.status == EeStepStatus::GuestSystemCall) {
        const std::uint32_t syscall = state.read_gpr_u32(3U);
        std::cout << "  syscall:     " << syscall;
        if (syscall == 60U) {
            std::cout << " (SetupThread)";
        } else if (syscall == 61U) {
            std::cout << " (SetupHeap)";
        } else if (syscall == 35U) {
            std::cout << " (ExitThread)";
        }
        std::cout << '\n'
                  << "  a0:          " << hex32(state.read_gpr_u32(4U)) << '\n'
                  << "  a1:          " << hex32(state.read_gpr_u32(5U)) << '\n'
                  << "  a2:          " << hex32(state.read_gpr_u32(6U)) << '\n'
                  << "  a3:          " << hex32(state.read_gpr_u32(7U)) << '\n'
                  << "  t0:          " << hex32(state.read_gpr_u32(8U)) << '\n';
        if (syscall == 60U) {
            std::cout << "  milestone:   CRT register and BSS initialization completed\n";
        }
    }
    return 0;
}

class ScopedRetailTimerConsumerObserverSession {
  public:
    ScopedRetailTimerConsumerObserverSession(takedown::runtime::RetailTimerConsumerTrace& trace,
                                             takedown::guest::EeMemory& memory)
        : trace_(&trace), memory_(&memory) {}
    [[nodiscard]] bool begin() {
        if (active_) {
            return true;
        }
        if (!trace_->begin_temporal_memory_write_coverage()) {
            return false;
        }
        previous_observer_ = memory_->ee_memory_write_observer();
        memory_->set_ee_memory_write_observer(trace_);
        active_ = true;
        return true;
    }
    void reset() {
        if (!active_) {
            return;
        }
        memory_->set_ee_memory_write_observer(previous_observer_);
        trace_->end_temporal_memory_write_coverage();
        active_ = false;
    }
  private:
    takedown::runtime::RetailTimerConsumerTrace* trace_;
    takedown::guest::EeMemory* memory_;
    takedown::guest::EeMemoryWriteObserver* previous_observer_{};
    bool active_{};
};



// Inlined helper of the menu mouse code: the frontend title screen is phase 5 with either of two updaters live.
[[nodiscard]] bool is_menu_title_screen(const takedown::guest::EeMemory& memory, const std::uint32_t phase,
                                        const std::uint32_t updater, const std::uint32_t flow) {
    return phase == 5U && ((flow == 32124240U && updater == 32079388U) ||
                           (flow == 32124288U && updater == 32081016U && memory.read_u32_le(updater) == 5111936U));
}

// Letter used by the replay report for a scheduled frontend input.
[[nodiscard]] char frontend_replay_input_symbol(const takedown::runtime::FrontendReplayInput input) {
    switch (input) {
    case takedown::runtime::FrontendReplayInput::Neutral:
        return 'N';
    case takedown::runtime::FrontendReplayInput::DpadDown:
        return 'D';
    case takedown::runtime::FrontendReplayInput::DpadUp:
        return 'U';
    case takedown::runtime::FrontendReplayInput::Cross:
        return 'X';
    }
    return '?';
}

// RECOVERY: file-local helper (inlined in the original); the filename stream ends before the caller's existence check.
std::filesystem::path gs_upload_capture_path(const std::filesystem::path& directory,
                                             const takedown::guest::GsImageTransfer& image, std::size_t index) {
    std::ostringstream filename;
    filename << "gs-upload-" << std::setfill('0') << std::setw(4) << index << "-path"
             << (image.path == takedown::guest::GifPath::Path1 ? 1 : image.path == takedown::guest::GifPath::Path2 ? 2 : 3)
             << '-' << image.width << 'x' << image.height << "-psm" << std::hex << std::setw(2)
             << image.destination_psm << ".ppm";
    return directory / filename.str();
}




using namespace takedown;
using namespace takedown::runtime;
using namespace takedown::runtime::slus_21050;
using namespace takedown::platform;
using takedown::recompiler::JoinPumpRegion;
using takedown::guest::EeStepResult;
using takedown::guest::EeStepStatus;
namespace fs = std::filesystem;

int run_boot_entry(int argc, char** argv) {
    // ---- HostWindowPendingSnapshot (from P5/prologue.cpp)
    struct HostWindowPendingSnapshot {
        std::future<takedown::presentation::GsRendererWorkerSnapshot> future;
        std::uint64_t vblank;
    };
    // ---- DmacCallbackTrace (from Q8/prologue.cpp)
    struct DmacCallbackTrace {
        std::uint32_t channel;
        std::uint32_t read_before;
        std::uint32_t end_before;
        std::uint8_t active_before;
        std::uint64_t dispatch_step;
        std::uint64_t return_step;
        std::uint32_t read_after;
        std::uint32_t end_after;
        std::uint8_t active_after;
    };
    // ---- RpcBindTrace (from L5/prologue.cpp)
    struct RpcBindTrace {
        std::uint64_t step;
        std::uint32_t source;
        std::uint32_t client;
        std::uint32_t server_id;
    };
    static_assert(sizeof(RpcBindTrace) == 24U);
    // ---- RwaDmaSubmissionTrace (from L5/prologue.cpp)
    struct RwaDmaSubmissionTrace {
        std::uint64_t step;
        std::int32_t id;
        std::uint32_t source;
        std::array<std::uint32_t, 12> words;
        std::optional<std::array<std::uint32_t, 28>> extra_words;
        std::vector<takedown::guest::EeSifDmaTransferState> descriptors;
    };
    static_assert(sizeof(RwaDmaSubmissionTrace) == 208U);
    // ---- EATraxChildQueryTrace (from L3/prologue.cpp)
    struct EATraxChildQueryTrace {
        std::uint64_t step;
        std::uint32_t node;
        std::uint32_t child;
        std::uint32_t interface;
        std::uint32_t result;
    };
    // ---- RetailEventTrace (from L3/prologue.cpp)
    struct RetailEventTrace {
        std::uint64_t step;
        std::uint32_t handler;
        std::uint32_t object;
        std::uint32_t event;
        std::uint32_t argument2;
        std::uint32_t argument3;
        std::uint32_t return_address;
    };
    // ---- PendingFemainParallelGsCorrelation (from L3/prologue.cpp)
    struct PendingFemainParallelGsCorrelation {
        takedown::runtime::Slus21050FemainMoviePublicationPacket publication;
        takedown::runtime::Slus21050FemainLiveSourceCandidate source;
    };
    struct FEmainLiveSourceDrainContext {
        std::unique_ptr<takedown::runtime::Slus21050FemainLiveSourceProducer>& source;
        std::unique_ptr<takedown::runtime::Slus21050FemainLiveTerminalCoordinator>& terminal;
        takedown::guest::GsCapture& capture;
        bool& terminal_attached;
        bool& failure_pending;
        takedown::runtime::Slus21050FemainParallelGsCompletionLedger* completion_ledger;
        std::optional<PendingFemainParallelGsCorrelation>& pending_correlation;
        std::size_t& raised_vblank_starts;
        std::uint64_t& next_vblank_start;
    };
    // ---- RetailPhaseTransition (from L5/my_decls.cpp)
    struct RetailPhaseTransition {
        std::uint64_t step{};
        std::uint32_t instruction{};
        std::uint32_t from{};
        std::uint32_t to{};
    };
    // ---- RetailValueTransition (from L5/my_decls.cpp)
    struct RetailValueTransition {
        std::uint64_t step{};
        std::uint32_t instruction{};
        std::uint32_t from{};
        std::uint32_t to{};
    };
    // ---- RetailCallSiteTrace (from L5/prologue.cpp)
    struct RetailCallSiteTrace {
        takedown::guest::GuestAddress call_site{};
        takedown::guest::GuestAddress target{};
        std::uint64_t hits{};
        std::size_t first_step{};
        std::size_t last_step{};
    };
    // ---- RetailPcMilestone (from L3/prologue.cpp)
    struct RetailPcMilestone {
        takedown::guest::GuestAddress address{};
        std::string_view label{};
        std::uint64_t first_step{};
        std::uint64_t hits{};
        std::uint64_t last_step{};
    };
    // ---- HeadlessMovieEosTrace (from Q3/prologue.cpp)
    struct HeadlessMovieEosTrace {
        std::uint64_t start_step;
        bool looping_background;
        bool start_skip_exempt;
        bool host_playback;
        bool host_playback_opened;
        std::optional<std::uint64_t> accepted_step;
        std::optional<std::uint64_t> start_skip_requested_step;
        std::optional<std::uint64_t> start_skip_input_poll;
        std::optional<std::uint64_t> start_skip_audio_frames;
        std::optional<std::uint64_t> start_skip_audio_non_silent_frames;
        std::optional<std::uint64_t> start_skip_audio_fnv1a64;
        std::optional<std::uint64_t> start_skip_key_ons;
        std::optional<std::uint64_t> start_skip_active_voices;
        std::optional<std::uint64_t> paused_step;
        std::optional<std::uint64_t> abort_poll_step;
        std::optional<std::uint64_t> abort_requested_step;
        std::optional<std::uint64_t> stop_completed_step;
        std::optional<std::uint64_t> destroyed_step;
        std::uint64_t command_baseline;
        std::uint64_t fdec_baseline;
        std::uint64_t to_ipu_baseline;
        std::uint64_t accepted_fdec_count;
        std::uint64_t accepted_to_ipu_count;
        std::uint64_t pause_assertions;
        std::uint32_t abort_decoder;
        std::uint32_t abort_s0;
        std::uint32_t abort_movie_state;
        std::uint32_t abort_work_state;
        std::uint32_t abort_stream;
        std::array<std::uint8_t, 3> abort_flags;
        std::uint64_t abort_fdec_count;
        std::uint64_t abort_to_ipu_count;
        std::string filename;
    };
    static_assert(sizeof(HeadlessMovieEosTrace) == 344U);
    // ---- VirtualDs2Trace (from Q3/prologue.cpp)
    struct VirtualDs2Trace {
        struct EdgePoll {
            std::uint64_t step{};
            std::uint32_t selector{};
            std::uint16_t accept_edge{};
            std::uint32_t result{0xFFFFFFFFU};
            std::array<std::uint32_t, 2> action_blocks{};
        };
        struct FrontendEvent {
            std::uint64_t step{};
            std::uint32_t event{};
            std::uint32_t argument3{};
        };
        std::uint64_t update_count{};
        std::uint64_t reserved_{};
        std::optional<std::uint64_t> registered_step;
        std::optional<std::uint64_t> armed_step;
        std::optional<std::uint64_t> pulse_step;
        std::uint64_t pulse_attempt_count{};
        std::optional<std::uint64_t> decoded_step;
        std::optional<std::uint64_t> action_edge_step;
        std::optional<std::uint64_t> action_edge_clear_step;
        std::optional<std::uint64_t> gate_accepted_step;
        std::optional<std::uint64_t> title_candidate_step;
        std::optional<std::uint64_t> title_armed_step;
        std::optional<std::uint64_t> title_pulse_step;
        std::optional<std::uint64_t> title_decoded_step;
        std::optional<std::uint64_t> title_edge_step;
        std::optional<std::uint64_t> title_poll_step;
        std::optional<std::uint64_t> title_result_step;
        std::optional<std::uint64_t> title_accepted_step;
        std::optional<std::uint64_t> title_queued_step;
        std::optional<std::uint64_t> title_event_step;
        std::optional<std::uint64_t> title_next_flow_step;
        std::array<std::uint32_t, 4> profile_words{};
        std::uint32_t decoded_gate_bits{};
        std::uint32_t title_decoded_action_bits{};
        std::uint32_t title_action_source{};
        std::uint8_t title_action_source_active{};
        std::uint8_t title_action_source_mode{};
        std::uint8_t title_action_previous{};
        std::uint8_t title_action_current{};
        std::uint32_t title_poll_result{};
        std::uint32_t reserved2_{};
        std::array<EdgePoll, 16> edge_polls{};
        std::uint64_t edge_poll_count{};
        std::array<FrontendEvent, 16> frontend_events{};
        std::uint64_t frontend_event_count{};
        std::uint64_t first_step{};
        std::uint32_t title_action_channel{0xFFFFFFFFU};
    };
    // ---- HeadlessFrontendReplayTrace (from Q3/prologue.cpp)
    struct HeadlessFrontendReplayTrace {
        struct ManagerUpdate {
            std::uint64_t step{};
            std::uint64_t replay_update{};
            takedown::runtime::FrontendReplayInput input{};
            std::array<std::uint8_t, 4> decoded{};
            std::uint32_t action_device{};
            std::uint32_t fallback_device{};
            std::array<std::uint8_t, 2> device_flags{};
            std::array<std::uint8_t, 2> fallback_device_flags{};
            std::int32_t confirm_logical_action{-1};
            std::array<std::int32_t, 2> confirm_mapped_channels{-1, -1};
            std::int32_t confirm_fallback_channel{-1};
            std::uint32_t confirm_direct_value{};
            std::array<std::uint8_t, 2> confirm_direct_edge_state{};
            std::uint32_t confirm_fallback_value{};
            std::array<std::uint8_t, 2> confirm_fallback_edge_state{};
            std::array<std::uint16_t, 15> actions{};
            std::uint16_t accept_edge{};
            std::uint32_t flow{};
            std::uint32_t updater{};
            std::uint32_t updater_state{};
            std::array<std::uint8_t, 3> updater_flags{};
            std::array<std::uint32_t, 6> menu_fields{};
        };
        struct MenuPoll {
            std::uint64_t step{};
            std::uint64_t replay_update{};
            takedown::runtime::FrontendReplayInput input{};
            std::uint32_t result{};
            std::array<std::uint32_t, 6> fields{};
        };
        struct EdgePoll {
            std::uint64_t step{};
            std::uint64_t replay_update{};
            takedown::runtime::FrontendReplayInput input{};
            std::uint32_t selector{};
            std::uint16_t accept_edge{};
            std::uint32_t result{0xFFFFFFFFU};
            std::array<std::uint32_t, 2> action_blocks{};
        };
        std::uint64_t start_step{};
        std::uint64_t start_update_count{};
        std::optional<std::uint64_t> first_down_step;
        std::optional<std::uint64_t> first_up_step;
        std::optional<std::uint64_t> first_cross_step;
        std::optional<std::uint64_t> last_cross_step;
        std::uint64_t cross_pulse_count{};
        std::array<ManagerUpdate, 128> manager_updates{};
        std::uint64_t manager_update_count{};
        std::array<MenuPoll, 192> menu_polls{};
        std::uint64_t menu_poll_count{};
        std::array<EdgePoll, 192> edge_polls{};
        std::uint64_t edge_poll_count{};
    };
    // ---- GraphicsAsyncTeardownGuard (from P2/prologue.cpp)
    struct GraphicsAsyncTeardownGuard {
        takedown::guest::EeMemory* memory;
        ~GraphicsAsyncTeardownGuard() {
            memory->graphics_async_fence(takedown::runtime::GraphicsAsyncFence::Teardown);
        }
    };

    if (argc <= 2) {
        throw std::invalid_argument("boot-entry requires a boot ELF path and optional runner settings");
    }

    std::size_t maximum_steps = 20'000'000U;
    bool step_limit_supplied = false;
    bool host_controller_bridge = false;
    bool auto_start = false;
    bool subframe_input = false;
    bool host_movie_playback = false;
    bool headless_movie_eos = false;
    bool headless_frontend_replay = false;
    bool window_requested = false;
    bool run_until_close = false;
    bool run_until_stop = false;
    bool retail_trace = false;
    bool interpreter_only = false;
    bool native_only = false;
    bool disable_vu1_upper_stages = false;
    bool disable_vu1_natural_blocks = false;
    bool disable_vu1_lower_plans = false;
    bool vu1_simple_alu_lower_plans =
        std::getenv("TAKEDOWN_VU1_SIMPLE_ALU_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_SIMPLE_ALU_LOWER_PLANS");
    bool vu1_simple_alu_lower_plans_cli = false;
    bool vu1_ilw_lower_plans =
        std::getenv("TAKEDOWN_VU1_ILW_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_ILW_LOWER_PLANS");
    bool vu1_ilw_lower_plans_cli = false;
    bool vu1_isw_lower_plans =
        std::getenv("TAKEDOWN_VU1_ISW_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_ISW_LOWER_PLANS");
    bool vu1_isw_lower_plans_cli = false;
    bool vu1_flag_read_lower_plans =
        std::getenv("TAKEDOWN_VU1_FLAG_READ_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_FLAG_READ_LOWER_PLANS");
    bool vu1_flag_read_lower_plans_cli = false;
    bool vu1_clip_predicate_lower_plans =
        std::getenv("TAKEDOWN_VU1_CLIP_PREDICATE_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_CLIP_PREDICATE_LOWER_PLANS");
    bool vu1_clip_predicate_lower_plans_cli = false;
    const bool vu1_branch_lower_plans =
        std::getenv("TAKEDOWN_VU1_BRANCH_LOWER_PLANS") != nullptr &&
        takedown::platform::host_default_on_flag_enabled("TAKEDOWN_VU1_BRANCH_LOWER_PLANS");
    bool vu1_worker_differential = false;
    bool vu1_worker_async_stage2 = false;
    bool vu1_worker_consume = false;
    std::uint32_t vu1_worker_consume_audit_period = 0U;
    bool stats_overlay_startup = false;
    bool require_media_parity = false;
    std::optional<std::filesystem::path> save_data_root_override;
    std::optional<std::filesystem::path> mods_directory_override;
    bool mods_disabled_option = false;
    std::size_t gs_semantic_events = 100'000U;
    bool gs_semantic_events_supplied = false;
    bool gs_semantic_vblank_supplied = false;
    std::optional<std::filesystem::path> gs_capture_directory;
    std::optional<std::filesystem::path> parallel_gs_stream_path;
    std::optional<std::filesystem::path> parallel_gs_live_fifo_path;
    std::optional<std::filesystem::path> parallel_gs_completion_fifo_path;
    std::optional<std::filesystem::path> parallel_gs_input_fifo_path;
    std::optional<std::size_t> guest_work_measurement_step_budget;
    bool profile_guest_work_window = false;
    std::optional<std::filesystem::path> guest_input_record_path;
    std::optional<std::filesystem::path> guest_input_replay_path;
    std::optional<std::filesystem::path> guest_input_poll_diagnostic_path;
    std::optional<std::uint64_t> guest_input_poll_window_start;
    bool guest_input_time_keyed = false;
    std::optional<takedown::runtime::FrameTimeScript> frame_time_script;
    std::optional<takedown::runtime::FrameTimeScript> frame_time_script_lead;
    std::optional<takedown::config::FrameRateMode> frame_rate_option;
    std::optional<std::filesystem::path> parallel_gs_live_capture_path;
    std::optional<std::filesystem::path> parallel_gs_digest_path;
    bool parallel_gs_only = false;
    std::optional<std::filesystem::path> gs_semantic_trace_path;
    std::optional<std::uint64_t> gs_semantic_vblank;
    bool gs_semantic_last_nonempty_completed_vblank = false;
    std::optional<std::filesystem::path> femain_terminal_diagnostic_path;
    bool femain_terminal_diagnostic_cli_supplied = false;
    bool femain_terminal_diagnostic_guest_work_only = false;
    std::optional<std::filesystem::path> scanout_ppm_path;
    std::optional<std::filesystem::path> ipu_frame_dump_path;
    std::optional<std::filesystem::path> collision_trace_path;
    std::optional<std::filesystem::path> spark_trace_path;
    std::optional<std::filesystem::path> ee_host_phase_profile_directory;
    std::optional<std::filesystem::path> ee_graphics_service_profile_directory;
    std::optional<std::filesystem::path> ee_pc_profile_directory;
    std::optional<std::filesystem::path> vu1_profile_directory;

    for (int index = 3; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--capture-gs") {
            if (gs_capture_directory.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--capture-gs must appear once and be followed by a directory");
            }
            gs_capture_directory = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--parallel-gs-stream") {
            if (parallel_gs_stream_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-stream must appear once and be followed by a file path");
            }
            parallel_gs_stream_path = std::filesystem::path(argv[++index]);
            if (parallel_gs_stream_path->empty()) {
                throw std::invalid_argument("--parallel-gs-stream requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--parallel-gs-live") {
            if (parallel_gs_live_fifo_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-live must appear once and be followed by a FIFO path");
            }
            parallel_gs_live_fifo_path = std::filesystem::path(argv[++index]);
            if (parallel_gs_live_fifo_path->empty()) {
                throw std::invalid_argument("--parallel-gs-live requires a non-empty FIFO path");
            }
            continue;
        }
        if (argument == "--parallel-gs-input") {
            if (parallel_gs_input_fifo_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-input must appear once and be followed by a FIFO path");
            }
            parallel_gs_input_fifo_path = std::filesystem::path(argv[++index]);
            if (parallel_gs_input_fifo_path->empty()) {
                throw std::invalid_argument("--parallel-gs-input requires a non-empty FIFO path");
            }
            continue;
        }
        if (argument == "--parallel-gs-completion") {
            if (parallel_gs_completion_fifo_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-completion must appear once and be followed by a FIFO path");
            }
            parallel_gs_completion_fifo_path = std::filesystem::path(argv[++index]);
            if (parallel_gs_completion_fifo_path->empty()) {
                throw std::invalid_argument("--parallel-gs-completion requires a non-empty FIFO path");
            }
            continue;
        }
        if (argument == "--guest-work-measurement-steps") {
            if (guest_work_measurement_step_budget.has_value() || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--guest-work-measurement-steps must appear once and be followed by a count");
            }
            guest_work_measurement_step_budget = parse_bounded_count(
                argv[++index], "guest-work measurement step budget", 50000000000ULL);
            continue;
        }
        if (argument == "--profile-guest-work-window") {
            if (profile_guest_work_window) {
                throw std::invalid_argument("--profile-guest-work-window must appear at most once");
            }
            profile_guest_work_window = true;
            continue;
        }
        if (argument == "--guest-input-record") {
            if (guest_input_record_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--guest-input-record must appear once and be followed by a file path");
            }
            guest_input_record_path = std::filesystem::path(argv[++index]);
            if (guest_input_record_path->empty()) {
                throw std::invalid_argument("--guest-input-record requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--guest-input-replay") {
            if (guest_input_replay_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--guest-input-replay must appear once and be followed by a file path");
            }
            guest_input_replay_path = std::filesystem::path(argv[++index]);
            if (guest_input_replay_path->empty()) {
                throw std::invalid_argument("--guest-input-replay requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--guest-input-poll-diagnostic") {
            if (guest_input_poll_diagnostic_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--guest-input-poll-diagnostic requires one new output file path");
            }
            guest_input_poll_diagnostic_path = std::filesystem::path(argv[++index]);
            if (guest_input_poll_diagnostic_path->empty()) {
                throw std::invalid_argument("--guest-input-poll-diagnostic requires a non-empty path");
            }
            continue;
        }
        if (argument == "--guest-input-time-keyed") {
            guest_input_time_keyed = true;
            continue;
        }
        if (argument == "--frame-rate") {
            if (frame_rate_option.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--frame-rate must appear once: retail or variable");
            }
            const std::string_view mode = argv[++index];
            if (mode == "retail") {
                frame_rate_option = takedown::config::FrameRateMode::Retail;
            } else if (mode == "variable") {
                frame_rate_option = takedown::config::FrameRateMode::Variable;
            } else {
                throw std::invalid_argument("--frame-rate accepts retail or variable");
            }
            continue;
        }
        if (argument == "--frame-time-script" || argument == "--frame-time-script-lead") {
            std::optional<takedown::runtime::FrameTimeScript>& script =
                argument == "--frame-time-script" ? frame_time_script : frame_time_script_lead;
            if (script.has_value() || index + 1 >= argc) {
                throw std::invalid_argument(std::string(argument) +
                                            " must appear once and be followed by a script");
            }
            script = takedown::runtime::FrameTimeScript::parse(argv[++index]);
            if (!script.has_value() ||
                (argument == "--frame-time-script-lead" && script->is_live())) {
                throw std::invalid_argument(
                    std::string(argument) +
                    " needs retail, milliseconds, a comma list or lcg:<seed>:<min>:<max> "
                    "(--frame-time-script also accepts live)");
            }
            continue;
        }
        if (argument == "--guest-input-poll-window-start") {
            if (guest_input_poll_window_start.has_value() || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--guest-input-poll-window-start must appear once and be followed by a poll ordinal");
            }
            guest_input_poll_window_start = parse_bounded_count(
                argv[++index], "--guest-input-poll-window-start", ~0ULL);
            continue;
        }
        if (argument == "--parallel-gs-digest") {
            if (parallel_gs_digest_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-digest must appear once and be followed by a new file path");
            }
            parallel_gs_digest_path = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--parallel-gs-live-capture") {
            if (parallel_gs_live_capture_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--parallel-gs-live-capture must appear once and be followed by a file path");
            }
            parallel_gs_live_capture_path = std::filesystem::path(argv[++index]);
            if (parallel_gs_live_capture_path->empty()) {
                throw std::invalid_argument("--parallel-gs-live-capture requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--parallel-gs-only") {
            if (parallel_gs_only) {
                throw std::invalid_argument("--parallel-gs-only must appear at most once");
            }
            parallel_gs_only = true;
            continue;
        }
        if (argument == "--gs-semantic-trace") {
            if (gs_semantic_trace_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--gs-semantic-trace must appear once and be followed by a file path");
            }
            gs_semantic_trace_path = std::filesystem::path(argv[++index]);
            if (gs_semantic_trace_path->empty()) {
                throw std::invalid_argument("--gs-semantic-trace requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--femain-terminal-diagnostic") {
            if (femain_terminal_diagnostic_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--femain-terminal-diagnostic must appear once and be followed by a file path");
            }
            femain_terminal_diagnostic_path = std::filesystem::path(argv[++index]);
            if (femain_terminal_diagnostic_path->empty()) {
                throw std::invalid_argument("--femain-terminal-diagnostic requires a non-empty file path");
            }
            femain_terminal_diagnostic_cli_supplied = true;
            continue;
        }
        if (argument == "--femain-terminal-diagnostic-guest-work-only") {
            if (femain_terminal_diagnostic_guest_work_only) {
                throw std::invalid_argument("--femain-terminal-diagnostic-guest-work-only must appear at most once");
            }
            femain_terminal_diagnostic_guest_work_only = true;
            continue;
        }
        if (argument == "--gs-semantic-events") {
            if (gs_semantic_events_supplied || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--gs-semantic-events must appear once and be followed by a count");
            }
            gs_semantic_events = parse_bounded_count(
                argv[++index], "GS semantic trace event limit", 5000000ULL);
            gs_semantic_events_supplied = true;
            continue;
        }
        if (argument == "--gs-semantic-vblank") {
            if (gs_semantic_vblank_supplied || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--gs-semantic-vblank must appear once and be followed by an index");
            }
            const std::string_view selection = argv[++index];
            if (selection == "last") {
                gs_semantic_last_nonempty_completed_vblank = true;
            } else {
                gs_semantic_vblank = parse_u32(selection, "GS semantic trace VBlank index");
            }
            gs_semantic_vblank_supplied = true;
            continue;
        }
        if (argument == "--scanout-ppm") {
            if (scanout_ppm_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--scanout-ppm must appear once and be followed by a file path");
            }
            scanout_ppm_path = std::filesystem::path(argv[++index]);
            if (scanout_ppm_path->empty()) {
                throw std::invalid_argument("--scanout-ppm requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--ipu-frame-dump") {
            if (ipu_frame_dump_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--ipu-frame-dump must appear once and be followed by a file path");
            }
            ipu_frame_dump_path = std::filesystem::path(argv[++index]);
            if (ipu_frame_dump_path->empty()) {
                throw std::invalid_argument("--ipu-frame-dump requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--collision-trace") {
            if (collision_trace_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--collision-trace must appear once and be followed by a file path");
            }
            collision_trace_path = std::filesystem::path(argv[++index]);
            if (collision_trace_path->empty()) {
                throw std::invalid_argument("--collision-trace requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--spark-trace") {
            if (spark_trace_path.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--spark-trace must appear once and be followed by a file path");
            }
            spark_trace_path = std::filesystem::path(argv[++index]);
            if (spark_trace_path->empty()) {
                throw std::invalid_argument("--spark-trace requires a non-empty file path");
            }
            continue;
        }
        if (argument == "--vu1-profile") {
            if (vu1_profile_directory.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--vu1-profile must appear once and be followed by an empty directory");
            }
            vu1_profile_directory = std::filesystem::path(argv[++index]);
            if (vu1_profile_directory->empty()) {
                throw std::invalid_argument("--vu1-profile requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--ee-pc-profile") {
            if (ee_pc_profile_directory.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--ee-pc-profile must appear once and be followed by an empty directory");
            }
            ee_pc_profile_directory = std::filesystem::path(argv[++index]);
            if (ee_pc_profile_directory->empty()) {
                throw std::invalid_argument("--ee-pc-profile requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--ee-host-phase-profile") {
            if (ee_host_phase_profile_directory.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--ee-host-phase-profile must appear once and be followed by an empty directory");
            }
            ee_host_phase_profile_directory = std::filesystem::path(argv[++index]);
            if (ee_host_phase_profile_directory->empty()) {
                throw std::invalid_argument("--ee-host-phase-profile requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--ee-graphics-service-profile") {
            if (ee_graphics_service_profile_directory.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--ee-graphics-service-profile must appear once and be followed by an empty directory");
            }
            ee_graphics_service_profile_directory = std::filesystem::path(argv[++index]);
            if (ee_graphics_service_profile_directory->empty()) {
                throw std::invalid_argument("--ee-graphics-service-profile requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--auto-start") {
            if (auto_start) {
                throw std::invalid_argument("--auto-start may appear only once");
            }
            auto_start = true;
            host_controller_bridge = true;
            continue;
        }
        if (argument == "--host-controller") {
            host_controller_bridge = true;
            continue;
        }
        if (argument == "--subframe-input") {
            if (subframe_input) {
                throw std::invalid_argument("--subframe-input may appear only once");
            }
            subframe_input = true;
            continue;
        }
        if (argument == "--headless-movie-eos") {
            if (headless_movie_eos) {
                throw std::invalid_argument("--headless-movie-eos may appear only once");
            }
            headless_movie_eos = true;
            continue;
        }
        if (argument == "--host-movie-playback") {
            if (host_movie_playback) {
                throw std::invalid_argument("--host-movie-playback may appear only once");
            }
            host_movie_playback = true;
            continue;
        }
        if (argument == "--headless-frontend-replay") {
            if (headless_frontend_replay) {
                throw std::invalid_argument("--headless-frontend-replay may appear only once");
            }
            headless_frontend_replay = true;
            continue;
        }
        if (argument == "--window") {
            if (window_requested) {
                throw std::invalid_argument("--window may appear only once");
            }
            window_requested = true;
            continue;
        }
        if (argument == "--stats-overlay") {
            if (stats_overlay_startup) {
                throw std::invalid_argument("--stats-overlay may appear only once");
            }
            stats_overlay_startup = true;
            continue;
        }
        if (argument == "--mods-dir") {
            if (mods_directory_override.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--mods-dir must appear once and be followed by a directory");
            }
            mods_directory_override = std::filesystem::path(argv[++index]);
            if (mods_directory_override->empty()) {
                throw std::invalid_argument("--mods-dir requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--no-mods") {
            mods_disabled_option = true;
            continue;
        }
        if (argument == "--save-data-root") {
            if (save_data_root_override.has_value() || index + 1 >= argc) {
                throw std::invalid_argument("--save-data-root must appear once and be followed by a directory");
            }
            save_data_root_override = std::filesystem::path(argv[++index]);
            if (save_data_root_override->empty()) {
                throw std::invalid_argument("--save-data-root requires a non-empty directory path");
            }
            continue;
        }
        if (argument == "--require-media-parity") {
            if (require_media_parity) {
                throw std::invalid_argument("--require-media-parity may appear only once");
            }
            require_media_parity = true;
            continue;
        }
        if (argument == "--run-until-close") {
            if (run_until_close) {
                throw std::invalid_argument("--run-until-close may appear only once");
            }
            run_until_close = true;
            continue;
        }
        if (argument == "--run-until-stop") {
            if (run_until_stop) {
                throw std::invalid_argument("--run-until-stop may appear only once");
            }
            run_until_stop = true;
            continue;
        }
        if (argument == "--retail-trace") {
            if (retail_trace) {
                throw std::invalid_argument("--retail-trace may appear only once");
            }
            retail_trace = true;
            continue;
        }
        if (argument == "--interpreter-only") {
            if (interpreter_only) {
                throw std::invalid_argument("--interpreter-only may appear only once");
            }
            interpreter_only = true;
            continue;
        }
        if (argument == "--disable-vu1-upper-stages") {
            if (disable_vu1_upper_stages) {
                throw std::invalid_argument("--disable-vu1-upper-stages may appear only once");
            }
            disable_vu1_upper_stages = true;
            continue;
        }
        if (argument == "--disable-vu1-natural-blocks") {
            if (disable_vu1_natural_blocks) {
                throw std::invalid_argument("--disable-vu1-natural-blocks may appear only once");
            }
            disable_vu1_natural_blocks = true;
            continue;
        }
        if (argument == "--disable-vu1-lower-plans") {
            if (disable_vu1_lower_plans) {
                throw std::invalid_argument("--disable-vu1-lower-plans may appear only once");
            }
            disable_vu1_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-simple-alu-lower-plans") {
            if (vu1_simple_alu_lower_plans_cli) {
                throw std::invalid_argument("--vu1-simple-alu-lower-plans may appear only once");
            }
            vu1_simple_alu_lower_plans_cli = true;
            vu1_simple_alu_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-ilw-lower-plans") {
            if (vu1_ilw_lower_plans_cli) {
                throw std::invalid_argument("--vu1-ilw-lower-plans may appear only once");
            }
            vu1_ilw_lower_plans_cli = true;
            vu1_ilw_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-isw-lower-plans") {
            if (vu1_isw_lower_plans_cli) {
                throw std::invalid_argument("--vu1-isw-lower-plans may appear only once");
            }
            vu1_isw_lower_plans_cli = true;
            vu1_isw_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-flag-read-lower-plans") {
            if (vu1_flag_read_lower_plans_cli) {
                throw std::invalid_argument("--vu1-flag-read-lower-plans may appear only once");
            }
            vu1_flag_read_lower_plans_cli = true;
            vu1_flag_read_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-clip-predicate-lower-plans") {
            if (vu1_clip_predicate_lower_plans_cli) {
                throw std::invalid_argument("--vu1-clip-predicate-lower-plans may appear only once");
            }
            vu1_clip_predicate_lower_plans_cli = true;
            vu1_clip_predicate_lower_plans = true;
            continue;
        }
        if (argument == "--vu1-worker-differential") {
            if (vu1_worker_differential) {
                throw std::invalid_argument("--vu1-worker-differential may appear only once");
            }
            vu1_worker_differential = true;
            continue;
        }
        if (argument == "--vu1-worker-async-stage2") {
            if (vu1_worker_async_stage2) {
                throw std::invalid_argument("--vu1-worker-async-stage2 may appear only once");
            }
            vu1_worker_async_stage2 = true;
            continue;
        }
        if (argument == "--vu1-worker-consume") {
            if (vu1_worker_consume) {
                throw std::invalid_argument("--vu1-worker-consume may appear only once");
            }
            vu1_worker_consume = true;
            continue;
        }
        if (argument == "--vu1-worker-consume-audit-period") {
            if (index + 1 >= argc) {
                throw std::invalid_argument("--vu1-worker-consume-audit-period requires <N>");
            }
            vu1_worker_consume_audit_period =
                parse_u32(argv[++index], "--vu1-worker-consume-audit-period <N>");
            continue;
        }
        if (argument == "--native-only") {
            if (native_only) {
                throw std::invalid_argument("--native-only may appear only once");
            }
            native_only = true;
            continue;
        }
        if (argument.starts_with("--")) {
            throw std::invalid_argument("unknown boot-entry option: " + std::string(argument));
        } else {
            if (step_limit_supplied) {
                throw std::invalid_argument("boot-entry accepts only one step limit");
            }
            maximum_steps = parse_bounded_count(argument, "step limit", 50000000000ULL);
            step_limit_supplied = true;
        }
    }

    if (run_until_close && !window_requested) {
        throw std::invalid_argument("--run-until-close requires --window");
    }
    if (host_movie_playback && headless_movie_eos) {
        throw std::invalid_argument("--host-movie-playback cannot be combined with --headless-movie-eos");
    }
    if (guest_input_record_path.has_value() && guest_input_replay_path.has_value()) {
        throw std::invalid_argument("--guest-input-record and --guest-input-replay are mutually exclusive");
    }
    if (guest_input_poll_diagnostic_path.has_value() && !guest_input_replay_path.has_value()) {
        throw std::invalid_argument("--guest-input-poll-diagnostic requires --guest-input-replay");
    }
    if (guest_input_poll_window_start.has_value() && !guest_input_poll_diagnostic_path.has_value()) {
        throw std::invalid_argument(
            "--guest-input-poll-window-start requires --guest-input-poll-diagnostic");
    }
    if (guest_input_time_keyed &&
        (!guest_input_poll_diagnostic_path.has_value() || guest_input_poll_window_start.has_value())) {
        throw std::invalid_argument(
            "--guest-input-time-keyed requires --guest-input-poll-diagnostic and excludes "
            "--guest-input-poll-window-start");
    }
    const bool guest_input_timeline_enabled =
        guest_input_record_path.has_value() || guest_input_replay_path.has_value();
    if (guest_input_timeline_enabled && !host_controller_bridge) {
        throw std::invalid_argument(
            "guest input record/replay requires --host-controller or --auto-start for the controller bridge");
    }
    if (guest_input_timeline_enabled && !guest_work_measurement_step_budget.has_value()) {
        throw std::invalid_argument(
            "guest input record/replay requires --guest-work-measurement-steps <count>");
    }
    if (guest_input_record_path.has_value() && !parallel_gs_input_fifo_path.has_value()) {
        throw std::invalid_argument(
            "--guest-input-record requires --parallel-gs-input <fifo> for live input");
    }
    if (guest_input_replay_path.has_value() && parallel_gs_input_fifo_path.has_value()) {
        throw std::invalid_argument(
            "--guest-input-replay cannot be combined with live --parallel-gs-input");
    }
    if (guest_input_timeline_enabled && (subframe_input || headless_frontend_replay)) {
        throw std::invalid_argument(
            "guest input record/replay cannot be combined with --subframe-input or "
            "--headless-frontend-replay");
    }
    if (guest_input_timeline_enabled && std::getenv("TAKEDOWN_INPUT_SCRIPT") != nullptr) {
        throw std::invalid_argument(
            "guest input record/replay cannot be combined with TAKEDOWN_INPUT_SCRIPT");
    }
    if ((gs_semantic_events_supplied || gs_semantic_vblank_supplied) &&
        !gs_semantic_trace_path.has_value()) {
        throw std::invalid_argument(
            "--gs-semantic-events and --gs-semantic-vblank require --gs-semantic-trace");
    }
    if (!femain_terminal_diagnostic_path.has_value()) {
        const char* const environment = std::getenv("TAKEDOWN_FEMAIN_TERMINAL_DIAGNOSTIC");
        if (environment != nullptr) {
            if (environment[0] == '\0') {
                throw std::invalid_argument(
                    "TAKEDOWN_FEMAIN_TERMINAL_DIAGNOSTIC requires a non-empty output path");
            }
            femain_terminal_diagnostic_path = std::filesystem::path(environment);
        }
    }
    if (femain_terminal_diagnostic_guest_work_only) {
        if (!femain_terminal_diagnostic_cli_supplied || !femain_terminal_diagnostic_path.has_value()) {
            throw std::invalid_argument(
                "--femain-terminal-diagnostic-guest-work-only requires --femain-terminal-diagnostic <file>");
        }
        if (!guest_work_measurement_step_budget.has_value()) {
            throw std::invalid_argument(
                "--femain-terminal-diagnostic-guest-work-only requires --guest-work-measurement-steps "
                "<count>");
        }
    }
    if (native_only && interpreter_only) {
        throw std::invalid_argument("--native-only cannot be combined with --interpreter-only");
    }
    if (collision_trace_path.has_value() && native_only) {
        throw std::invalid_argument(
            "--collision-trace cannot be combined with --native-only because its observed "
            "instructions require the scalar EE fallback");
    }
    if (spark_trace_path.has_value() && native_only) {
        throw std::invalid_argument(
            "--spark-trace cannot be combined with --native-only because its observed instructions "
            "require the scalar EE fallback");
    }
    if (parallel_gs_stream_path.has_value() && parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-stream and --parallel-gs-live are mutually exclusive");
    }
    if (parallel_gs_digest_path.has_value() && !parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-digest requires --parallel-gs-live <fifo>");
    }
    if (parallel_gs_live_capture_path.has_value() && !parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-live-capture requires --parallel-gs-live <fifo>");
    }
    if (parallel_gs_completion_fifo_path.has_value() && !parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-completion requires --parallel-gs-live <fifo>");
    }
    if (parallel_gs_input_fifo_path.has_value() && !parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-input requires --parallel-gs-live <fifo>");
    }
    if (parallel_gs_input_fifo_path.has_value() && !window_requested) {
        throw std::invalid_argument(
            "--parallel-gs-input requires --window for host hotkeys and close handling");
    }
    if (parallel_gs_input_fifo_path.has_value() && !host_controller_bridge) {
        throw std::invalid_argument(
            "--parallel-gs-input requires --host-controller or --auto-start for the controller bridge");
    }
    if (guest_work_measurement_step_budget.has_value() && !parallel_gs_input_fifo_path.has_value() &&
        !guest_input_replay_path.has_value()) {
        throw std::invalid_argument(
            "--guest-work-measurement-steps requires --parallel-gs-input <fifo> for B3IN bit 20 or "
            "--guest-input-replay <file> for a recorded A boundary");
    }
    if (guest_work_measurement_step_budget.has_value() && (run_until_close || run_until_stop)) {
        throw std::invalid_argument(
            "--guest-work-measurement-steps cannot be combined with --run-until-close or "
            "--run-until-stop; B3IN bit 20 is its only arm boundary");
    }
    if (guest_work_measurement_step_budget.has_value() && !step_limit_supplied) {
        maximum_steps = std::numeric_limits<std::size_t>::max();
    }
    if (profile_guest_work_window && !guest_work_measurement_step_budget.has_value()) {
        throw std::invalid_argument(
            "--profile-guest-work-window requires --guest-work-measurement-steps <count>");
    }
    if (ee_graphics_service_profile_directory.has_value() && !profile_guest_work_window) {
        throw std::invalid_argument(
            "--ee-graphics-service-profile requires --profile-guest-work-window");
    }
    if (ee_graphics_service_profile_directory.has_value() && !ee_host_phase_profile_directory.has_value()) {
        throw std::invalid_argument(
            "--ee-graphics-service-profile requires --ee-host-phase-profile");
    }
    if (profile_guest_work_window && !ee_pc_profile_directory.has_value() &&
        !ee_host_phase_profile_directory.has_value() && !vu1_profile_directory.has_value() &&
        !ee_graphics_service_profile_directory.has_value()) {
        throw std::invalid_argument(
            "--profile-guest-work-window requires --ee-pc-profile, --ee-host-phase-profile, or "
            "--vu1-profile");
    }
    if (parallel_gs_only && !parallel_gs_live_fifo_path.has_value()) {
        throw std::invalid_argument("--parallel-gs-only requires --parallel-gs-live <fifo>");
    }
    if (parallel_gs_only && !window_requested) {
        throw std::invalid_argument(
            "--parallel-gs-only requires --window for guest keyboard/controller input");
    }
    if (parallel_gs_only && scanout_ppm_path.has_value()) {
        throw std::invalid_argument(
            "--parallel-gs-only cannot produce --scanout-ppm from the disabled software GS");
    }
    if (step_limit_supplied && run_until_close) {
        throw std::invalid_argument("--run-until-close cannot be combined with a maximum step count");
    }
    if (run_until_close) {
        maximum_steps = std::numeric_limits<std::size_t>::max();
    }
    if (window_requested && run_until_stop) {
        throw std::invalid_argument("--run-until-stop is headless and cannot be combined with --window");
    }
    if (step_limit_supplied && run_until_stop) {
        throw std::invalid_argument("--run-until-stop cannot be combined with a maximum step count");
    }
    if (run_until_close && run_until_stop) {
        throw std::invalid_argument("--run-until-stop cannot be combined with --run-until-close");
    }
    if (run_until_stop) {
        maximum_steps = std::numeric_limits<std::size_t>::max();
    }
    if (headless_frontend_replay && !auto_start) {
        throw std::invalid_argument("--headless-frontend-replay requires --auto-start");
    }
    if (subframe_input && !host_controller_bridge) {
        throw std::invalid_argument("--subframe-input requires --host-controller or --auto-start");
    }

    std::unique_ptr<takedown::runtime::GuestInputTimelineRecorder> guest_input_recorder;
    std::unique_ptr<takedown::runtime::GuestInputTimelineReplay> guest_input_replay;
    if (guest_input_record_path.has_value()) {
        guest_input_recorder = std::make_unique<takedown::runtime::GuestInputTimelineRecorder>(
            *guest_input_record_path, takedown::runtime::GuestInputTimelineFormat::B3IR2);
        std::cout << "Guest input timeline: recording B3IR 2 to \"" << guest_input_record_path->string()
                  << "\"; changed states are sampled only at retail DS2 polls\n"
                  << std::flush;
    } else if (guest_input_replay_path.has_value()) {
        guest_input_replay = std::make_unique<takedown::runtime::GuestInputTimelineReplay>(
            *guest_input_replay_path, guest_input_poll_diagnostic_path, guest_input_poll_window_start,
            guest_input_time_keyed);
        std::cout << "Guest input timeline: replaying B3IR "
                  << static_cast<std::uint32_t>(guest_input_replay->format()) << " from \""
                  << guest_input_replay_path->string()
                  << "\"; live guest controller input and B3IN arming are disabled\n"
                  << std::flush;
    }
    if (guest_input_poll_diagnostic_path.has_value()) {
        std::cout << "Guest input poll diagnostic: original controller states and poll ordinals; actual "
                     "timings recorded to \""
                  << guest_input_poll_diagnostic_path->string()
                  << "\"; not strict replay or identical-output/performance qualification\n"
                  << std::flush;
    }
    if (guest_input_time_keyed) {
        std::cout << "Guest input timeline: time-keyed; states and the A/C markers apply at their "
                     "recorded guest cycles, whatever the tick rate\n"
                  << std::flush;
    }
    if (guest_input_poll_window_start.has_value()) {
        std::cout << "Guest input poll window: measurement spans retail polls "
                  << *guest_input_poll_window_start
                  << " to the recorded final poll; step and cycle counts are reported, not required\n"
                  << std::flush;
    }
    if (guest_input_timeline_enabled && host_movie_playback) {
        std::cout << "Guest input timeline: host movie frames and natural EOS use the guest SPU2 sample "
                     "clock; no wall clock selects frontend movie progress\n"
                  << std::flush;
    }

    if (gs_capture_directory.has_value()) {
        prepare_empty_capture_directory(*gs_capture_directory);
    }
    if (vu1_profile_directory.has_value()) {
        if (vu1_profile_directory->empty()) {
            throw std::invalid_argument("--vu1-profile requires a non-empty directory path");
        }
        std::error_code error;
        if (std::filesystem::exists(*vu1_profile_directory, error)) {
            if (error || !std::filesystem::is_directory(*vu1_profile_directory, error)) {
                throw std::invalid_argument(
                    "VU1 profile path exists and is not an inspectable directory: " +
                    vu1_profile_directory->string());
            }
            if (!std::filesystem::is_empty(*vu1_profile_directory, error) || error) {
                throw std::invalid_argument("VU1 profile directory must be empty: " +
                                            vu1_profile_directory->string());
            }
        } else if (!std::filesystem::create_directories(*vu1_profile_directory, error) || error) {
            throw std::runtime_error("unable to create VU1 profile directory: " + error.message());
        }
    }

    std::unique_ptr<takedown::runtime::EeHostPhaseProfiler> ee_host_phase_profiler;
    if (ee_host_phase_profile_directory.has_value()) {
        takedown::runtime::prepare_empty_ee_host_phase_profile_directory(*ee_host_phase_profile_directory);
        ee_host_phase_profiler = std::make_unique<takedown::runtime::EeHostPhaseProfiler>();
    }
    std::unique_ptr<takedown::runtime::EeGraphicsServiceDiagnostics> ee_graphics_service_diagnostics;
    if (ee_graphics_service_profile_directory.has_value()) {
        takedown::runtime::prepare_empty_ee_graphics_service_profile_directory(
            *ee_graphics_service_profile_directory);
        ee_graphics_service_diagnostics =
            std::make_unique<takedown::runtime::EeGraphicsServiceDiagnostics>();
    }
    std::unique_ptr<takedown::runtime::EePcSampler> ee_pc_sampler;
    if (ee_pc_profile_directory.has_value()) {
        takedown::runtime::prepare_empty_ee_pc_profile_directory(*ee_pc_profile_directory);
        ee_pc_sampler = std::make_unique<takedown::runtime::EePcSampler>();
    }
    if (scanout_ppm_path.has_value()) {
        std::error_code error;
        if (std::filesystem::exists(*scanout_ppm_path, error) || error) {
            throw std::invalid_argument("scanout PPM already exists or cannot be inspected: " +
                                        scanout_ppm_path->string());
        }
    }
    std::unique_ptr<takedown::runtime::Slus21050CollisionTrace> collision_trace;
    if (collision_trace_path.has_value()) {
        collision_trace =
            std::make_unique<takedown::runtime::Slus21050CollisionTrace>(*collision_trace_path);
    }
    takedown::runtime::Slus21050SparkTrace* spark_trace = nullptr;
    if (spark_trace_path.has_value()) {
        spark_trace =
            std::make_unique<takedown::runtime::Slus21050SparkTrace>(*spark_trace_path).release();
    }
    takedown::runtime::Slus21050VehicleWorldContactEndpointProbe* vehicle_world_contact_probe = nullptr;
    if (const char* const raw = std::getenv("TAKEDOWN_VEHICLE_WORLD_CONTACT_PROBE");
    raw != nullptr && std::strcmp(raw, "1") == 0) {
        vehicle_world_contact_probe = new takedown::runtime::Slus21050VehicleWorldContactEndpointProbe();
    }
    if (vehicle_world_contact_probe != nullptr && native_only) {
        throw std::invalid_argument(
            "TAKEDOWN_VEHICLE_WORLD_CONTACT_PROBE requires the scalar EE fallback");
    }
    takedown::guest::EeMemory memory;
    memory.configure_ram_write_watch();

    struct EePcHitWatchEntry {
        std::uint32_t address;
        std::uint64_t total;
        std::uint64_t guest_work_window;
    };
    std::vector<EePcHitWatchEntry> ee_pc_hit_watch;
    std::vector<std::uint8_t> ee_pc_hit_watch_slots;
    if (const char* const raw = std::getenv("TAKEDOWN_EE_PC_HIT_WATCH"); raw != nullptr && *raw != '\0') {
        const std::string_view list = raw;
        std::size_t begin = 0U;
        while (begin < list.size()) {
            const std::size_t comma = list.find(',', begin);
            const std::size_t end = comma == std::string_view::npos ? list.size() : comma;
            const std::uint32_t address =
                parse_u32(list.substr(begin, end - begin), "TAKEDOWN_EE_PC_HIT_WATCH address");
            if ((address & 3U) != 0U || address >= takedown::guest::kEeMainRamSize) {
                throw std::invalid_argument(
                    "TAKEDOWN_EE_PC_HIT_WATCH addresses must be aligned user EE RAM PCs");
            }
            if (ee_pc_hit_watch.size() >= 255U) {
                throw std::invalid_argument("TAKEDOWN_EE_PC_HIT_WATCH accepts at most 255 addresses");
            }
            ee_pc_hit_watch.push_back(EePcHitWatchEntry{address, 0U, 0U});
            if (comma == std::string_view::npos) {
                break;
            }
            begin = comma + 1U;
        }
        std::sort(ee_pc_hit_watch.begin(), ee_pc_hit_watch.end(),
                  [](const auto& left, const auto& right) { return left.address < right.address; });
        if (std::adjacent_find(ee_pc_hit_watch.begin(), ee_pc_hit_watch.end(),
                               [](const auto& left, const auto& right) {
            return left.address == right.address;
        }) != ee_pc_hit_watch.end()) {
            throw std::invalid_argument("TAKEDOWN_EE_PC_HIT_WATCH addresses must be unique");
        }
        ee_pc_hit_watch_slots.assign(takedown::guest::kEeMainRamSize / 4U, std::uint8_t{255U});
        for (std::size_t index = 0; index < ee_pc_hit_watch.size(); ++index) {
            ee_pc_hit_watch_slots[ee_pc_hit_watch[index].address >> 2U] = static_cast<std::uint8_t>(index);
        }
        std::cout << "EE PC hit watch: exact-addresses=" << ee_pc_hit_watch.size()
                  << "; use --interpreter-only for instruction-exact occupancy\n"
                  << std::flush;
    }

    const bool retail_timer_differential_enabled =
        takedown::platform::host_experiment_flag_enabled("TAKEDOWN_RETAIL_TIMER_DIFFERENTIAL");
    if (retail_timer_differential_enabled && !interpreter_only) {
        throw std::invalid_argument("TAKEDOWN_RETAIL_TIMER_DIFFERENTIAL=1 requires --interpreter-only");
    }
    const bool native_simulation_shadow_enabled =
        takedown::platform::host_experiment_flag_enabled("TAKEDOWN_NATIVE_SIMULATION_SHADOW");
    if (native_simulation_shadow_enabled && !interpreter_only) {
        throw std::invalid_argument("TAKEDOWN_NATIVE_SIMULATION_SHADOW=1 requires --interpreter-only");
    }
    if (native_simulation_shadow_enabled && !guest_work_measurement_step_budget.has_value()) {
        throw std::invalid_argument(
            "TAKEDOWN_NATIVE_SIMULATION_SHADOW=1 requires --guest-work-measurement-steps <count>");
    }
    std::uint32_t native_simulation_shadow_hz = 60U;
    if (native_simulation_shadow_enabled) {
        if (const char* const value = std::getenv("TAKEDOWN_NATIVE_SIMULATION_HZ"); value != nullptr) {
            const std::size_t hz =
                parse_bounded_count(value, "TAKEDOWN_NATIVE_SIMULATION_HZ", 480U);
            if (hz < 60U) {
                throw std::invalid_argument(
                    "TAKEDOWN_NATIVE_SIMULATION_HZ must be an integer from 60 to 480");
            }
            native_simulation_shadow_hz = static_cast<std::uint32_t>(hz);
        }
    }
    const char* const retail_timer_consumer_trace_environment =
        std::getenv("TAKEDOWN_RETAIL_TIMER_CONSUMER_TRACE");
    if (retail_timer_consumer_trace_environment != nullptr && !interpreter_only) {
        throw std::invalid_argument("TAKEDOWN_RETAIL_TIMER_CONSUMER_TRACE requires --interpreter-only");
    }
    if (retail_timer_consumer_trace_environment != nullptr &&
        retail_timer_consumer_trace_environment[0] == '\0') {
        throw std::invalid_argument(
            "TAKEDOWN_RETAIL_TIMER_CONSUMER_TRACE requires a non-empty output path");
    }
    const std::optional<std::filesystem::path> retail_timer_consumer_trace_path =
        retail_timer_consumer_trace_environment != nullptr
            ? std::optional<std::filesystem::path>(retail_timer_consumer_trace_environment)
            : std::nullopt;
    const char* const retail_timer_owner_abi_trace_environment =
        std::getenv("TAKEDOWN_RETAIL_TIMER_OWNER_ABI_TRACE");
    if (retail_timer_owner_abi_trace_environment != nullptr && !interpreter_only) {
        throw std::invalid_argument("TAKEDOWN_RETAIL_TIMER_OWNER_ABI_TRACE requires --interpreter-only");
    }
    if (retail_timer_owner_abi_trace_environment != nullptr &&
        retail_timer_owner_abi_trace_environment[0] == '\0') {
        throw std::invalid_argument(
            "TAKEDOWN_RETAIL_TIMER_OWNER_ABI_TRACE requires a non-empty output path");
    }
    if (retail_timer_owner_abi_trace_environment != nullptr &&
        !guest_work_measurement_step_budget.has_value()) {
        throw std::invalid_argument(
            "TAKEDOWN_RETAIL_TIMER_OWNER_ABI_TRACE requires --guest-work-measurement-steps <count>");
    }
    const std::optional<std::filesystem::path> retail_timer_owner_abi_trace_path =
        retail_timer_owner_abi_trace_environment != nullptr
            ? std::optional<std::filesystem::path>(retail_timer_owner_abi_trace_environment)
            : std::nullopt;
    struct RetailTimerProjection {
        takedown::guest::GuestAddress object;
        takedown::runtime::RetailReferenceTimerState expected;
        bool guest_work_window;
    };
    std::optional<RetailTimerProjection> retail_timer_projection;
    std::optional<std::uint32_t> native_simulation_shadow_object;
    std::unique_ptr<takedown::runtime::NativeSimulationShadow> native_simulation_shadow;
    if (native_simulation_shadow_enabled) {
        takedown::runtime::NativeSimulationDriverConfig shadow_config;
        shadow_config.clock.simulation_hz_numerator = native_simulation_shadow_hz;
        shadow_config.clock.max_catch_up_steps =
            std::min<std::uint32_t>(8U, (native_simulation_shadow_hz + 29U) / 30U);
        native_simulation_shadow = std::make_unique<takedown::runtime::NativeSimulationShadow>(
            true, shadow_config);
    }
    bool native_simulation_shadow_stop_requested = false;
    const bool retail_timer_owner_observer_enabled =
        retail_timer_differential_enabled || native_simulation_shadow_enabled;
    std::uint64_t retail_timer_differential_calls = 0U;
    std::uint64_t retail_timer_differential_matches = 0U;
    std::uint64_t retail_timer_differential_mismatches = 0U;
    std::uint64_t retail_timer_differential_window_calls = 0U;
    std::uint64_t retail_timer_differential_window_matches = 0U;
    std::uint64_t retail_timer_differential_window_mismatches = 0U;
    std::optional<std::string> retail_timer_first_mismatch;
    bool retail_timer_differential_passed = false;
    const auto read_retail_timer_state = [&memory](const GuestAddress object) {
        if (!memory.contains(object, 41U)) {
            throw std::runtime_error("retail timer differential object is outside guest memory");
        }
        const auto read_word = [&](const std::uint32_t offset) {
            return memory.read_u32_le(object + offset);
        };
        takedown::runtime::RetailReferenceTimerState state;
        state.observed_tick = read_word(0U);
        state.consumed_tick_offset = static_cast<std::int32_t>(read_word(4U));
        state.paused_ticks = static_cast<std::int32_t>(read_word(8U));
        state.whole_ticks = static_cast<std::int32_t>(read_word(12U));
        state.subdivision_remainder = static_cast<std::int32_t>(read_word(16U));
        state.baseline_tick = memory.read_u32_le(object + 20U);
        state.subdivisions = static_cast<std::int32_t>(read_word(24U));
        state.step_seconds = std::bit_cast<float>(read_word(28U));
        state.elapsed_seconds = std::bit_cast<float>(read_word(32U));
        state.pending_subdivisions = static_cast<std::int32_t>(read_word(36U));
        state.accumulating = memory.read_u8(object + 40U) != 0U;
        return state;
    };

    const bool fast_gif_status_read = host_default_on_flag_enabled("TAKEDOWN_FAST_GIF_STATUS_READ");
    memory.set_fast_gif_status_read_enabled(fast_gif_status_read);
    const bool gif_blocked_arbiter_cache =
        host_default_on_flag_enabled("TAKEDOWN_GIF_BLOCKED_ARBITER_CACHE");
    memory.set_gif_blocked_arbiter_cache_enabled(gif_blocked_arbiter_cache);
    const bool gif_path3_batch = host_default_on_flag_enabled("TAKEDOWN_GIF_PATH3_BATCH");
    memory.set_gif_path3_batch_enabled(gif_path3_batch);
    const bool gif_idle_pump_gate = host_default_on_flag_enabled("TAKEDOWN_GIF_IDLE_PUMP_GATE");
    memory.set_gif_idle_pump_gate_enabled(gif_idle_pump_gate);
    memory.vu1_program_cache().set_profiling_enabled(vu1_profile_directory.has_value() &&
                                                     !profile_guest_work_window);
    memory.vu1_program_cache().set_runtime_hot_upper_stages_enabled(!disable_vu1_upper_stages);
    memory.vu1_program_cache().set_runtime_hot_natural_blocks_enabled(!disable_vu1_natural_blocks);
    const char* linear_scoreboard_flag = std::getenv("TAKEDOWN_VU1_LINEAR_SCOREBOARD");
    const char* minimal_counters_flag = std::getenv("TAKEDOWN_VU1_MINIMAL_COUNTERS");
    // RECOVERY: block order of this && / != "1" expression differs from the original (fail block should precede the equal block).
    const bool minimal_counters =
        !(minimal_counters_flag != nullptr && std::string_view(minimal_counters_flag) != "1");
    memory.vu1_program_cache().set_execution_statistics_enabled(!minimal_counters);
    memory.vu1_program_cache().set_linear_scoreboard_enabled(
        linear_scoreboard_flag != nullptr && std::string_view(linear_scoreboard_flag) == "1");
    memory.vu1_program_cache().set_opcode_threaded_lower_stages_enabled(!disable_vu1_lower_plans);
    memory.vu1_program_cache().set_simple_alu_lower_plans_enabled(vu1_simple_alu_lower_plans);
    memory.vu1_program_cache().set_ilw_lower_plans_enabled(vu1_ilw_lower_plans);
    memory.vu1_program_cache().set_isw_lower_plans_enabled(vu1_isw_lower_plans);
    memory.vu1_program_cache().set_flag_read_lower_plans_enabled(vu1_flag_read_lower_plans);
    memory.vu1_program_cache().set_clip_predicate_lower_plans_enabled(vu1_clip_predicate_lower_plans);
    memory.vu1_program_cache().set_branch_lower_plans_enabled(vu1_branch_lower_plans);

    static takedown::runtime::Vu1WorkerDifferentialController vu1_worker_differential_controller;
    if (vu1_worker_async_stage2 && vu1_worker_consume) {
        throw std::invalid_argument(
            "--vu1-worker-consume and --vu1-worker-async-stage2 are mutually exclusive (consume "
            "replaces the deferred parity pipeline)");
    }
    if (vu1_worker_consume && vu1_profile_directory.has_value()) {
        throw std::invalid_argument(
            "--vu1-worker-consume cannot run with --vu1-profile: admitted launches execute on the "
            "worker, so the live profile columns undercount and are not dispatch-health evidence");
    }
    if (vu1_worker_differential || vu1_worker_consume) {
        vu1_worker_differential_controller.set_enabled(true);
        takedown::runtime::set_vu1_worker_differential_controller(&vu1_worker_differential_controller);
    }
    takedown::runtime::set_vu1_worker_async_stage2_enabled(vu1_worker_async_stage2);
    if (vu1_worker_differential) {
        vu1_worker_differential_controller.set_deferred(vu1_worker_async_stage2);
    }
    takedown::runtime::set_vu1_worker_consume_enabled(vu1_worker_consume);
    if (vu1_worker_consume) {
        vu1_worker_differential_controller.set_consume(true);
        vu1_worker_differential_controller.set_consume_audit_period(vu1_worker_consume_audit_period);
    }
    memory.set_vu1_observed_window_recording(vu1_worker_async_stage2 || vu1_worker_consume);
    if (spark_trace != nullptr) {
        memory.set_vu1_xgkick_observer(spark_trace);
    }
    const char* untimed = std::getenv("TAKEDOWN_GRAPHICS_UNTIMED");
    const bool graphics_timed = !(untimed != nullptr && *untimed == '1');
    memory.set_gif_dmac_timing_enabled(graphics_timed);
    memory.set_vif1_dmac_timing_enabled(graphics_timed);
    const bool render_gs = (window_requested || scanout_ppm_path.has_value()) && !parallel_gs_only;
    const bool skip_compat_present = host_compat_present_suppressed(parallel_gs_only);
    const bool host_window_timing_enabled =
        host_experiment_flag_enabled("TAKEDOWN_HOST_WINDOW_TIMING");
    const char* v = std::getenv("TAKEDOWN_FEMAIN_LIVE_TRANSACTION");
    const bool femain_live_transaction_enabled = v != nullptr && *v == '1';
    memory.set_gs_capture_enabled(
        gs_capture_directory.has_value() || parallel_gs_stream_path.has_value() ||
        parallel_gs_live_fifo_path.has_value() || gs_semantic_trace_path.has_value() ||
        femain_terminal_diagnostic_path.has_value() || render_gs || femain_live_transaction_enabled);

    std::unique_ptr<takedown::platform::ParallelGsCompletionReader> parallel_gs_completion_reader;
    if (parallel_gs_completion_fifo_path.has_value()) {
        parallel_gs_completion_reader =
            std::make_unique<takedown::platform::ParallelGsCompletionReader>(
                *parallel_gs_completion_fifo_path);
        std::cout << "Receiving optional paraLLEl-GS completion records from FIFO \""
                  << parallel_gs_completion_fifo_path->string()
                  << "\" (GPU submission completion only; presentation remains unproven)\n"
                  << std::flush;
    }
    std::unique_ptr<takedown::runtime::Slus21050FemainParallelGsCompletionLedger>
        femain_parallel_gs_completion_ledger;
    if (femain_live_transaction_enabled && parallel_gs_completion_reader != nullptr) {
        femain_parallel_gs_completion_ledger =
            std::make_unique<takedown::runtime::Slus21050FemainParallelGsCompletionLedger>(true);
        std::cout << "Correlating authenticated FEMain source EOP with optional paraLLEl-GS GPU "
                     "completion records (diagnostic only; no terminal or presentation authority)\n"
                  << std::flush;
    }

    std::unique_ptr<takedown::platform::ParallelGsRawStreamWriter> parallel_gs_stream_writer;
    if (parallel_gs_stream_path.has_value()) {
        parallel_gs_stream_writer =
            std::make_unique<takedown::platform::ParallelGsRawStreamWriter>(*parallel_gs_stream_path);
        memory.gs_capture().set_raw_gif_observer(parallel_gs_stream_writer.get());
    } else if (parallel_gs_live_fifo_path.has_value()) {
        std::cout << "Connecting guest-lossless live GS stream to FIFO \""
                  << parallel_gs_live_fifo_path->string()
                  << "\"; start parallel-gs-stream with that FIFO before this command\n"
                  << std::flush;
        parallel_gs_stream_writer = std::make_unique<takedown::platform::ParallelGsRawStreamWriter>(
            *parallel_gs_live_fifo_path, takedown::platform::ParallelGsStreamOpenMode::ExistingFifo,
            parallel_gs_live_capture_path);
        if (parallel_gs_digest_path.has_value()) {
            parallel_gs_stream_writer->record_digest(*parallel_gs_digest_path);
        }
        memory.gs_capture().set_raw_gif_observer(parallel_gs_stream_writer.get());
    }
    GraphicsAsyncTeardownGuard graphics_async_teardown_guard{&memory};
    std::unique_ptr<takedown::platform::ParallelGsInputReader> parallel_gs_input_reader;
    if (parallel_gs_input_fifo_path.has_value()) {
        parallel_gs_input_reader = std::make_unique<takedown::platform::ParallelGsInputReader>(
            *parallel_gs_input_fifo_path);
        std::cout << "Receiving focused paraLLEl-GS keyboard input from FIFO \""
                  << parallel_gs_input_fifo_path->string() << "\"\n"
                  << std::flush;
    }
    std::optional<takedown::runtime::GuestWorkMeasurementWindow> guest_work_measurement_window;
    if (guest_work_measurement_step_budget.has_value()) {
        guest_work_measurement_window.emplace(*guest_work_measurement_step_budget);
        std::cout << "Guest-work measurement: awaiting B3IN bit 20; requested-steps="
                  << *guest_work_measurement_step_budget
                  << " scope=EE instruction-boundary fixed guest work; stop uses existing "
                     "step-budget clipping\n"
                  << std::flush;
    }
    struct Vu1LowerPlanWindowTelemetry {
        std::array<std::uint64_t, 5> native_start{};
        std::array<std::uint64_t, 5> native_end{};
        bool candidate_enabled{};
        bool ilw_candidate_enabled{};
        bool isw_candidate_enabled{};
        bool branch_candidate_enabled{};
        bool flag_read_candidate_enabled{};
        bool clip_predicate_candidate_enabled{};
        bool start_captured{};
        bool end_captured{};
        bool complete{};
        bool branch_complete{};
        bool flag_read_complete{};
        bool fmand_complete{};
        bool fcget_complete{};
        bool clip_predicate_complete{};
        bool fcand_complete{};
        bool fcor_complete{};
        std::uint64_t start_hits{};
        std::uint64_t start_fallbacks{};
        std::uint64_t end_hits{};
        std::uint64_t end_fallbacks{};
        std::uint64_t delta_hits{};
        std::uint64_t delta_fallbacks{};
        std::uint64_t delta_total{};
        std::uint64_t branch_start_hits{};
        std::uint64_t branch_start_fallbacks{};
        std::uint64_t branch_end_hits{};
        std::uint64_t branch_end_fallbacks{};
        std::uint64_t branch_delta_hits{};
        std::uint64_t branch_delta_fallbacks{};
        std::uint64_t branch_delta_total{};
        std::uint64_t flag_read_start_hits{};
        std::uint64_t flag_read_start_fallbacks{};
        std::uint64_t flag_read_end_hits{};
        std::uint64_t flag_read_end_fallbacks{};
        std::uint64_t flag_read_delta_hits{};
        std::uint64_t flag_read_delta_fallbacks{};
        std::uint64_t flag_read_delta_total{};
        std::uint64_t fmand_start_hits{};
        std::uint64_t fmand_start_fallbacks{};
        std::uint64_t fmand_end_hits{};
        std::uint64_t fmand_end_fallbacks{};
        std::uint64_t fmand_delta_hits{};
        std::uint64_t fmand_delta_fallbacks{};
        std::uint64_t fmand_delta_total{};
        std::uint64_t fcget_start_hits{};
        std::uint64_t fcget_start_fallbacks{};
        std::uint64_t fcget_end_hits{};
        std::uint64_t fcget_end_fallbacks{};
        std::uint64_t fcget_delta_hits{};
        std::uint64_t fcget_delta_fallbacks{};
        std::uint64_t fcget_delta_total{};
        std::uint64_t clip_predicate_start_hits{};
        std::uint64_t clip_predicate_start_fallbacks{};
        std::uint64_t clip_predicate_end_hits{};
        std::uint64_t clip_predicate_end_fallbacks{};
        std::uint64_t clip_predicate_delta_hits{};
        std::uint64_t clip_predicate_delta_fallbacks{};
        std::uint64_t clip_predicate_delta_total{};
        std::uint64_t fcand_start_hits{};
        std::uint64_t fcand_start_fallbacks{};
        std::uint64_t fcand_end_hits{};
        std::uint64_t fcand_end_fallbacks{};
        std::uint64_t fcand_delta_hits{};
        std::uint64_t fcand_delta_fallbacks{};
        std::uint64_t fcand_delta_total{};
        std::uint64_t fcor_start_hits{};
        std::uint64_t fcor_start_fallbacks{};
        std::uint64_t fcor_end_hits{};
        std::uint64_t fcor_end_fallbacks{};
        std::uint64_t fcor_delta_hits{};
        std::uint64_t fcor_delta_fallbacks{};
        std::uint64_t fcor_delta_total{};
    };
    std::optional<Vu1LowerPlanWindowTelemetry> vu1_lower_plan_window_telemetry;
    if (guest_work_measurement_window.has_value()) {
        vu1_lower_plan_window_telemetry.emplace();
        vu1_lower_plan_window_telemetry->candidate_enabled = vu1_simple_alu_lower_plans;
        vu1_lower_plan_window_telemetry->ilw_candidate_enabled = vu1_ilw_lower_plans;
        vu1_lower_plan_window_telemetry->isw_candidate_enabled = vu1_isw_lower_plans;
        vu1_lower_plan_window_telemetry->branch_candidate_enabled =
            !disable_vu1_lower_plans && vu1_branch_lower_plans;
        vu1_lower_plan_window_telemetry->flag_read_candidate_enabled =
            !disable_vu1_lower_plans && vu1_flag_read_lower_plans;
        vu1_lower_plan_window_telemetry->clip_predicate_candidate_enabled =
            !disable_vu1_lower_plans && vu1_clip_predicate_lower_plans;
    }
    if (profile_guest_work_window) {
        std::cout << "Profile scope: awaiting B3IR A; requested profilers will cover only the exact "
                     "A-to-C guest-work window\n"
                  << std::flush;
    }
    std::unique_ptr<takedown::platform::GsSemanticTraceWriter> gs_semantic_trace_writer;
    if (gs_semantic_trace_path.has_value()) {
        gs_semantic_trace_writer = std::make_unique<takedown::platform::GsSemanticTraceWriter>(
            *gs_semantic_trace_path,
            takedown::platform::GsSemanticTraceConfig{gs_semantic_events, gs_semantic_vblank,
                                                      gs_semantic_last_nonempty_completed_vblank});
        memory.gs_capture().set_diagnostic_raw_gif_observer(gs_semantic_trace_writer.get());
        memory.gs_capture().set_diagnostic_command_observer(gs_semantic_trace_writer.get());
        memory.set_vif1_dmac_chain_observer(gs_semantic_trace_writer.get());
        memory.set_vu0_micro_call_observer(gs_semantic_trace_writer.get());
        if (spark_trace == nullptr) {
            memory.set_vu1_xgkick_observer(gs_semantic_trace_writer.get());
        }
    }
    if (parallel_gs_stream_writer != nullptr && !gs_capture_directory.has_value() &&
        !gs_semantic_trace_path.has_value() && !femain_terminal_diagnostic_path.has_value() &&
        !render_gs && !femain_live_transaction_enabled) {
        memory.gs_capture().set_raw_passthrough_only(true);
    }

    const takedown::config::LoadedSettings loaded_settings =
        takedown::config::load_settings_file(takedown::config::default_settings_path());
    for (const auto& diagnostic : loaded_settings.diagnostics) {
        std::cerr << "Settings: [" << diagnostic.key << "] " << diagnostic.message << '\n';
    }
    // RECOVERY: original inlines path copy/move-assign and ModManager member moves here; this spelling calls them (std inlining differs in the harness, source shape believed equal).
    takedown::content::ModOverlay mod_overlay;
    takedown::content::ModManager mods_manager;
    {
        const auto mods_directory = takedown::content::select_mods_directory(
            std::filesystem::path(argv[2]).parent_path(),
            mods_directory_override.has_value() ? mods_directory_override
                                                : loaded_settings.settings.mods_directory,
            loaded_settings.settings.mods_enabled && !mods_disabled_option);
        if (mods_directory.has_value()) {
            mods_manager = takedown::content::ModManager(
                *mods_directory, std::filesystem::path(argv[2]).parent_path());
            mods_manager.rescan();
            mod_overlay = mods_manager.build_overlay();
            if (!mods_manager.mods().empty()) {
                std::cout << mods_manager.describe();
            }
            if (!mod_overlay.empty() || !mod_overlay.warnings().empty()) {
                std::cout << mod_overlay.describe(200U);
            }
        }
    }
    takedown::runtime::Slus21050FemainBridge headless_femain_bridge;
    std::optional<std::filesystem::path> headless_femain_movie_path;
    std::error_code headless_femain_path_error;
    if (headless_movie_eos) {
        headless_femain_movie_path = takedown::content::resolve_game_path(
            std::filesystem::path(argv[2]).parent_path(), &mod_overlay, "tracks\\_FEMain.m2v",
            headless_femain_path_error);
        if (headless_femain_movie_path.has_value()) {
            headless_femain_bridge = takedown::runtime::Slus21050FemainBridge::decode_first_real_frame(
                *headless_femain_movie_path);
        }
    }
    takedown::runtime::Slus21050MoviePlayer host_movie_player;
    std::optional<std::filesystem::path> host_movie_path;
    std::error_code host_movie_path_error;
    std::optional<std::uint64_t> host_movie_clock_start_nanoseconds;
    std::optional<std::uint64_t> host_movie_clock_anchor_samples;
    std::optional<std::uint64_t> host_movie_clock_pause_start_nanoseconds;
    const char* femain_background_run_flag = std::getenv("TAKEDOWN_FEMAIN_BACKGROUND_RUN");
    const bool femain_background_run =
        femain_background_run_flag != nullptr && *femain_background_run_flag == '3';
    std::unique_ptr<takedown::runtime::Slus21050FemainLiveTerminalAuthority>
        host_femain_live_terminal_authority;
    if (femain_live_transaction_enabled) {
        host_femain_live_terminal_authority =
            std::make_unique<takedown::runtime::Slus21050FemainLiveTerminalAuthority>(true);
    }
    takedown::runtime::Slus21050FemainLiveTransaction host_femain_live_transaction(
        host_movie_player, host_femain_live_terminal_authority.get(), femain_live_transaction_enabled);
    std::uint64_t host_movie_clock_paused_nanoseconds = 0U;
    std::uint64_t host_movie_sessions = 0U;
    std::uint64_t host_movie_frames_staged = 0U;
    std::uint64_t host_movie_buffers_staged = 0U;
    std::uint64_t host_movie_bytes_staged = 0U;
    std::uint64_t host_movie_loop_restarts = 0U;
    const std::uint64_t host_femain_emulation_thread_identity =
        femain_live_transaction_enabled
            ? std::max<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()), 1U)
            : 0U;
    memory.set_ipu_mpeg2_decoder(takedown::guest::create_libavcodec_mpeg2_decoder());
    memory.gs_capture().set_image_history_enabled(gs_capture_directory.has_value());
    const takedown::content::Elf32LoadPlan plan =
        takedown::content::load_ps2_elf(std::filesystem::path(argv[2]), memory);
    takedown::guest::R5900State state(plan.executable.entry_point);
    takedown::runtime::RetailTimerConsumerTrace retail_timer_consumer_trace(
        retail_timer_consumer_trace_path.value_or(std::filesystem::path{}),
        takedown::runtime::RetailTimerConsumerTrace::kMaximumNestedJalrTargets,
        guest_work_measurement_window.has_value()
            ? takedown::runtime::RetailTimerConsumerTraceScope::GuestWorkWindow
            : takedown::runtime::RetailTimerConsumerTraceScope::WholeRun,
        takedown::runtime::RetailTimerConsumerTrace::kMaximumMemoryWriteEvents);
    takedown::runtime::RetailTimerOwnerAbiTrace retail_timer_owner_abi_trace(
        retail_timer_owner_abi_trace_path.value_or(std::filesystem::path{}));
    ScopedRetailTimerConsumerObserverSession retail_timer_consumer_observer_session(
        retail_timer_consumer_trace, memory);
    if (retail_timer_consumer_trace.collecting() && !retail_timer_consumer_observer_session.begin()) {
        throw std::logic_error("retail timer consumer temporal write coverage could not open");
    }
    takedown::guest::R5900InstructionCache instruction_cache;
    takedown::runtime::slus_21050::UiTextVifTransportClaimRegistry ui_text_vif_transport;
    ui_text_vif_transport.attach(memory);
    std::unique_ptr<takedown::runtime::slus_21050::ModelLive> native_model_live;
    takedown::guest::Vif1Path2SourceMarkerObserver* base_marker_observer = &ui_text_vif_transport;
    const char* flag = std::getenv("TAKEDOWN_NATIVE_MODEL_PARENT");
    if (flag != nullptr && std::string_view(flag) == "1") {
        if (parallel_gs_stream_writer == nullptr || interpreter_only || gs_capture_directory.has_value()) {
            throw std::runtime_error("native model parent requires the raw paraLLEl-GS renderer");
        }
        native_model_live = std::make_unique<takedown::runtime::slus_21050::ModelLive>(
            ui_text_vif_transport,
            [&parallel_gs_stream_writer](const TakedownNative::ModelPublication& model) {
            parallel_gs_stream_writer->write_native_model(model);
            return true;
        });
        base_marker_observer = native_model_live.get();
        memory.set_vif1_path2_source_marker_observer(native_model_live.get());
    }
    std::unique_ptr<takedown::runtime::Slus21050FemainTerminalProducerDiagnostic>
        femain_terminal_diagnostic;
    std::unique_ptr<takedown::runtime::Slus21050FemainRetailConsumeObservationAdapter>
        femain_retail_consume_observation_adapter;
    std::unique_ptr<takedown::runtime::Slus21050FemainRetailConsumeInvocationBarrier>
        femain_retail_consume_invocation_barrier;
    std::unique_ptr<takedown::runtime::Slus21050FemainProvenanceWindow> femain_provenance_window;
    std::optional<std::filesystem::path> femain_dynamic_callback_report_path;
    std::optional<std::filesystem::path> femain_frame_closure_report_path;
    std::optional<std::filesystem::path> femain_retail_consume_report_path;
    std::uint64_t femain_retail_consume_report_records = 0U;
    takedown::runtime::Slus21050FemainDynamicCallbackProbe* femain_dynamic_callback_probe = nullptr;
    takedown::runtime::Slus21050FemainFrameClosureDiagnostic* femain_frame_closure_diagnostic = nullptr;
    if (femain_terminal_diagnostic_path.has_value()) {
        femain_retail_consume_report_path = std::filesystem::path(
            femain_terminal_diagnostic_path->string() + ".retail-consume");
        femain_terminal_diagnostic =
            std::make_unique<takedown::runtime::Slus21050FemainTerminalProducerDiagnostic>(true, render_gs);
        if (femain_terminal_diagnostic_guest_work_only) {
            femain_terminal_diagnostic->request_guest_work_scope();
            femain_provenance_window =
                std::make_unique<takedown::runtime::Slus21050FemainProvenanceWindow>(true);
            femain_dynamic_callback_probe =
                new takedown::runtime::Slus21050FemainDynamicCallbackProbe(true);
            femain_frame_closure_diagnostic =
                new takedown::runtime::Slus21050FemainFrameClosureDiagnostic(true);
            femain_dynamic_callback_report_path = std::filesystem::path(
                femain_terminal_diagnostic_path->string() + ".dynamic-callback");
            femain_frame_closure_report_path = std::filesystem::path(
                femain_terminal_diagnostic_path->string() + ".frame-closure");
        }
        femain_terminal_diagnostic->set_raw_forward_observer(gs_semantic_trace_writer.get());
        femain_terminal_diagnostic->set_primary_forward_observer(gs_semantic_trace_writer.get());
        femain_terminal_diagnostic->set_chain_forward_observer(gs_semantic_trace_writer.get());
        femain_terminal_diagnostic->set_marker_forward_observer(base_marker_observer);
        memory.gs_capture().set_diagnostic_raw_gif_observer(femain_terminal_diagnostic.get());
        memory.set_vif1_dmac_chain_observer(femain_terminal_diagnostic.get());
        memory.set_vif1_path2_source_marker_observer(femain_terminal_diagnostic.get());
        static_cast<void>(femain_terminal_diagnostic->attach_scalar_write_observer(memory));
        femain_retail_consume_observation_adapter.reset(
            new takedown::runtime::Slus21050FemainRetailConsumeObservationAdapter(true));
        femain_retail_consume_invocation_barrier.reset(
            new takedown::runtime::Slus21050FemainRetailConsumeInvocationBarrier(true));
        femain_terminal_diagnostic->set_retail_consume_scalar_write_observer(
            femain_retail_consume_observation_adapter.get());
        femain_retail_consume_observation_adapter->note_live_binding_missing();
    }
    std::unique_ptr<Slus21050FemainLiveSourceProducer> host_femain_live_source_producer{};
    std::unique_ptr<Slus21050FemainLiveTerminalCoordinator> host_femain_live_terminal_coordinator{};
    bool host_femain_live_terminal_coordinator_attached = false;
    bool host_femain_live_source_activation_pending = false;
    bool host_femain_live_source_failure_pending = false;
    std::optional<PendingFemainParallelGsCorrelation> pending_femain_parallel_gs_correlation{};

    // RECOVERY: three lambda()#N closures defined before this point were fully inlined in the
    // original; these empty lambdas keep the numbering of the later ones aligned.
    [[maybe_unused]] const auto recovery_lambda_slot_1 = []() {};
    [[maybe_unused]] const auto recovery_lambda_slot_2 = []() {};
    [[maybe_unused]] const auto recovery_lambda_slot_3 = []() {};
    const auto femain_live_marker_forward_observer = [&]() -> guest::Vif1Path2SourceMarkerObserver* {
        if (femain_terminal_diagnostic != nullptr) {
            return femain_terminal_diagnostic.get();
        }
        return base_marker_observer;
    };

    const auto attach_femain_live_source_bridge = [&]() {
        if (femain_live_transaction_enabled && host_femain_live_source_producer == nullptr &&
            host_femain_live_terminal_coordinator == nullptr) {
            if (!memory.vif1_path2_source_markers_quiescent() ||
                !memory.gs_capture().gif_observation_quiescent()) {
                return false;
            }
            auto source_producer = std::make_unique<Slus21050FemainLiveSourceProducer>(true);
            auto terminal_coordinator =
                std::make_unique<Slus21050FemainLiveTerminalCoordinator>(true);
            source_producer->set_marker_forward_observer(femain_live_marker_forward_observer());
            memory.set_vif1_path2_source_marker_observer(source_producer.get());
            host_femain_live_source_producer = std::move(source_producer);
            host_femain_live_terminal_coordinator = std::move(terminal_coordinator);
            return true;
        }
        return host_femain_live_source_producer != nullptr &&
               host_femain_live_terminal_coordinator != nullptr;
    };

    const auto detach_femain_live_source_bridge = [&]() {
        host_femain_live_source_activation_pending = false;
        host_femain_live_source_failure_pending = false;
        pending_femain_parallel_gs_correlation.reset();
        if (femain_parallel_gs_completion_ledger != nullptr) {
            femain_parallel_gs_completion_ledger->cancel_pending();
        }
        if (host_femain_live_source_producer == nullptr) {
            host_femain_live_terminal_coordinator_attached = false;
            memory.gs_capture().set_terminal_raw_gif_observer(nullptr);
            memory.gs_capture().set_terminal_command_observer(nullptr);
            if (host_femain_live_terminal_coordinator != nullptr) {
                host_femain_live_terminal_coordinator->set_raw_forward_observer(nullptr);
                host_femain_live_terminal_coordinator.reset();
            }
            return;
        }
        if (host_femain_live_terminal_coordinator != nullptr &&
            host_femain_live_terminal_coordinator_attached) {
            host_femain_live_terminal_coordinator->detach(memory.gs_capture(),
                                                          *host_femain_live_source_producer);
        }
        host_femain_live_terminal_coordinator_attached = false;
        memory.gs_capture().set_terminal_raw_gif_observer(nullptr);
        memory.gs_capture().set_terminal_command_observer(nullptr);
        if (host_femain_live_terminal_coordinator != nullptr) {
            host_femain_live_terminal_coordinator->set_raw_forward_observer(nullptr);
            host_femain_live_terminal_coordinator.reset();
        }
        if (memory.detach_vif1_path2_source_marker_observer(
                host_femain_live_source_producer.get())) {
            memory.set_vif1_path2_source_marker_observer(femain_live_marker_forward_observer());
        }
        host_femain_live_source_producer->set_marker_forward_observer(nullptr);
        host_femain_live_source_producer->set_raw_forward_observer(nullptr);
        host_femain_live_source_producer.reset();
    };

    const auto activate_femain_live_source_bridge = [&]() {
        if (!host_femain_live_source_activation_pending) {
            return host_femain_live_source_producer != nullptr;
        }
        if (!attach_femain_live_source_bridge()) {
            return false;
        }
        const auto started = host_femain_live_transaction.begin_session();
        if (started.status != Slus21050FemainLiveTransactionStatus::SessionStarted) {
            detach_femain_live_source_bridge();
            return false;
        }
        host_femain_live_source_activation_pending = false;
        return true;
    };

    const auto persist_femain_retail_consume_observation = [&]() {
        if (!femain_retail_consume_report_path.has_value() ||
            femain_retail_consume_invocation_barrier == nullptr ||
            femain_retail_consume_observation_adapter == nullptr) {
            return;
        }
        if (femain_retail_consume_report_records == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("FEMain retail consume candidate report record overflow");
        }
        const auto next_record = femain_retail_consume_report_records + 1U;
        if (!write_femain_retail_consume_report(*femain_retail_consume_report_path,
                                                *femain_retail_consume_invocation_barrier,
                                                *femain_retail_consume_observation_adapter,
                                                next_record)) {
            throw std::runtime_error(
                "FEMain retail consume candidate report could not write its report");
        }
        femain_retail_consume_report_records = next_record;
    };

    const auto reset_femain_retail_consume_observation =
        [&](const Slus21050FemainLiveCancellationWitness* cancellation_witness) {
        using Phase = Slus21050FemainRetailConsumeInvocationBarrierPhase;
        bool barrier_released = true;
        if (femain_retail_consume_invocation_barrier != nullptr) {
            const Phase phase = femain_retail_consume_invocation_barrier->phase();
            if (phase == Phase::Armed || phase == Phase::Active || phase == Phase::Failed) {
                if (femain_retail_consume_observation_adapter != nullptr) {
                    femain_retail_consume_observation_adapter->close();
                }
                if (cancellation_witness != nullptr && cancellation_witness->valid()) {
                    persist_femain_retail_consume_observation();
                    barrier_released =
                        femain_retail_consume_invocation_barrier->release_cancelled_publication(
                            *cancellation_witness);
                } else {
                    femain_retail_consume_invocation_barrier->reject(
                        Slus21050FemainRetailConsumeInvocationBarrierFailure::
                    IncompleteInvocation);
                    barrier_released = false;
                }
            } else if (phase == Phase::Idle || phase == Phase::Complete) {
                if (femain_retail_consume_observation_adapter != nullptr) {
                    femain_retail_consume_observation_adapter->close();
                }
                if (phase == Phase::Complete) {
                    persist_femain_retail_consume_observation();
                }
                barrier_released = femain_retail_consume_invocation_barrier->reset();
            }
        }
        if (femain_retail_consume_observation_adapter != nullptr && barrier_released) {
            femain_retail_consume_observation_adapter->reset();
            femain_retail_consume_observation_adapter->note_live_binding_missing();
        }
    };

    const auto abort_failed_femain_live_source_bridge = [&]() {
        if (!host_femain_live_source_failure_pending) {
            return;
        }
        const auto failure = host_femain_live_source_producer != nullptr
                                 ? host_femain_live_source_producer->failure()
                                 : Slus21050FemainLiveSourceProducer::Failure::None;
        const std::optional<std::filesystem::path> fallback_path = host_movie_path;
        const bool fallback_looping = host_movie_player.looping();
        host_movie_player.close();
        const auto cancellation = host_femain_live_transaction.cancel_after_player_close(
            Slus21050FemainFrameLedgerCancelReason::Abort);
        const Slus21050FemainLiveCancellationWitness* const witness =
            cancellation.cancellation_witness.has_value() ? &*cancellation.cancellation_witness
                                                           : nullptr;
        bool rollback_released = cancellation.status == Slus21050FemainLiveTransactionStatus::Cancelled &&
                                 witness != nullptr && witness->valid();
        // RECOVERY: the original places the failure trampoline block before the cancel/reset chain (block order only);
        // not reproduced by this && chain.
        const auto authority_phase = host_femain_live_terminal_authority->phase();
        if (witness != nullptr && authority_phase != Slus21050FemainLiveTerminalAuthority::Phase::Idle) {
            if (authority_phase != Slus21050FemainLiveTerminalAuthority::Phase::Disabled) {
                rollback_released = host_femain_live_terminal_authority->cancel(*witness) &&
                                    host_femain_live_terminal_authority->reset_after_cancel() &&
                                    rollback_released;
            }
        }
        detach_femain_live_source_bridge();
        host_movie_clock_start_nanoseconds.reset();
        host_movie_clock_pause_start_nanoseconds.reset();
        host_movie_clock_paused_nanoseconds = 0U;
        host_movie_clock_anchor_samples.reset();
        bool fallback_reopened = false;
        if (rollback_released && fallback_path.has_value()) {
            static_cast<void>(host_movie_player.open(*fallback_path, fallback_looping));
            fallback_reopened = host_movie_player.active();
        }
        if (!fallback_reopened && rollback_released) {
            memory.write_u8(5351610U, 1U);
        }
        reset_femain_retail_consume_observation(witness);
        if (!rollback_released) {
            throw std::runtime_error(
                "FEMain live source rollback could not release its exact publication");
        }
        std::cerr << "FEMain live source bridge aborted: " << to_string(failure) << '\n';
    };

    runtime::slus_21050::VifPacketRingFlushClaimTracker vif_packet_ring_flush_claim_tracker(ui_text_vif_transport);
    runtime::slus_21050::SubmitUiDrawBucketsPacketSessionAdapter ui_draw_packet_session_adapter(
        ui_text_vif_transport);
    const auto ui_draw_packet_session_binding = ui_draw_packet_session_adapter.feed_binding();
    const auto plain_text_transport_claim_binding =
        ui_draw_packet_session_adapter.plain_text_transport_claim_binding();
    runtime::slus_21050::SubmitUiDrawBucketsSessionFeed ui_draw_session_feed{
        ui_draw_packet_session_binding.observation_sink, ui_draw_packet_session_binding.context,
        ui_draw_packet_session_binding.termination_sink, []() {
            const char* value = std::getenv("TAKEDOWN_UI_DRAW_SESSION_FEED");
            return value != nullptr && value[0] == '1';
        }()};
    runtime::NativeFunctionDispatcher native_function_dispatcher;
    runtime::CompiledBlockDispatcher compiled_block_dispatcher;
    const bool native_function_revision_matched =
        !interpreter_only &&
        runtime::register_slus_21050_native_functions(native_function_dispatcher, memory,
                                                      plain_text_transport_claim_binding);
    runtime::Slus21050AddedCars added_cars;
    if (native_function_revision_matched && mods_manager.active()) {
        if (!added_cars.register_functions(native_function_dispatcher, memory)) {
            std::cerr << "Added cars: the game's car routines differ; cars added by mods are unavailable\n";
        }
    }
    if (added_cars.registered()) {
        if (!added_cars.register_tuning(native_function_dispatcher, memory)) {
            throw std::runtime_error("Native vehicle tuning revision admission failed");
        }
    }
    if (added_cars.registered()) {
        std::error_code error;
        const auto path = content::resolve_disc_path(
            std::filesystem::path(argv[2]).parent_path(), "DATA/VDB.XML", error);
        std::ifstream file(path.value_or(std::filesystem::path{}), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                              std::istreambuf_iterator<char>());
        auto values = runtime::RetailValueDatabase::parse(bytes);
        if (!values.has_value()) {
            throw std::runtime_error("The game's DATA/VDB.XML is missing or not the retail layout");
        }
        added_cars.set_retail_values(std::move(values));
    }
    bool added_cars_dirty = true;
    bool car_select_skin_held = false;
    runtime::Slus21050CarSelect native_car_select(added_cars);
    const bool native_car_select_active =
        native_function_revision_matched && added_cars.registered() &&
        parallel_gs_stream_writer != nullptr && parallel_gs_live_fifo_path.has_value() &&
        native_car_select.register_functions(native_function_dispatcher, memory);
    if (added_cars.registered() && parallel_gs_live_fifo_path.has_value() &&
        !native_car_select_active) {
        std::cerr << "Native car select: frontend revision admission failed\n";
    }
    const auto configure_added_cars = [&]() {
        std::vector<takedown::runtime::AddedCar> cars;
        for (const auto& source : mods_manager.added_cars()) {
            if (!source.problem.empty()) {
                std::cerr << "Added cars: " << source.mod << ": " << source.problem << '\n';
                continue;
            }
            takedown::runtime::AddedCar car;
            car.vehicle = source.vehicle;
            car.template_vehicle = source.template_vehicle;
            car.skin = !source.skin_of.empty();
            car.name = source.name;
            car.mod = source.mod;
            car.class_id = source.class_id;
            car.mass_multiplier = source.mass_multiplier;
            car.top_speed_multiplier = source.top_speed_multiplier;
            car.thumbnail = source.thumbnail.string();
            for (std::size_t paint = 0U; paint < source.thumbnail_variants.size(); ++paint) {
                car.thumbnail_variants[paint] = source.thumbnail_variants[paint].string();
            }
            car.unlock.kind = source.unlock_type == "takedowns" ? takedown::runtime::AddedCarUnlock::Kind::Takedowns
                              : source.unlock_type == "event"   ? takedown::runtime::AddedCarUnlock::Kind::Event
                              : source.unlock_type == "template" ? takedown::runtime::AddedCarUnlock::Kind::Template
                                                                 : takedown::runtime::AddedCarUnlock::Kind::None;
            car.unlock.count = source.unlock_count;
            car.unlock.event = source.unlock_event;
            car.unlock.medal = source.unlock_medal;
            car.unlock.text = source.unlock_text;
            cars.push_back(std::move(car));
        }
        added_cars.set_cars(std::move(cars));
        for (const auto& native_class : mods_manager.car_classes()) {
            if (!native_class.problem.empty()) {
                std::cerr << "Car classes: " << native_class.problem << '\n';
            }
        }
        for (const auto& problem : added_cars.problems()) {
            std::cerr << "Added cars: " << problem << '\n';
        }
        for (const auto& car : added_cars.cars()) {
            std::cout << "Added cars: " << car.vehicle << " \"" << car.name << "\" ("
                      << (car.skin ? "skin of " : "template ") << car.template_vehicle << ", from " << car.mod << ")\n";
        }
    };
    if (native_model_live != nullptr &&
        !(native_function_revision_matched &&
          native_model_live->register_parent(native_function_dispatcher, memory))) {
        throw std::runtime_error("native model parent revision admission failed");
    }
    const char* const render_state_packet_flag = std::getenv("TAKEDOWN_NATIVE_RENDER_STATE_PACKET");
    const bool render_state_packet_requested =
        render_state_packet_flag != nullptr && std::string_view(render_state_packet_flag) == "1";
    // RECOVERY: in the original this flag (and native_static_object_active) is an unnamed temporary (_4485/_4486), not a named local.
    const bool native_render_state_packet_active =
        native_function_revision_matched && render_state_packet_requested &&
        runtime::slus_21050::register_render_state_packet_native_function(native_function_dispatcher,
                                                                          memory);
    const char* const static_object_flag = std::getenv("TAKEDOWN_NATIVE_STATIC_OBJECT");
    const bool native_static_object_active =
        static_object_flag != nullptr && std::string_view(static_object_flag) == "1" &&
        native_function_revision_matched &&
        runtime::slus_21050::register_static_object_native_function(native_function_dispatcher, memory);
    if (native_function_revision_matched && !interpreter_only) {
        const auto engine_functions =
            runtime::register_slus_21050_engine_functions(native_function_dispatcher,
                                                                      memory);
        std::cout << "Native game functions: " << engine_functions.registered << " registered";
        if (engine_functions.disabled != 0U) {
            std::cout << ", " << engine_functions.disabled << " disabled";
        }
        if (engine_functions.revision_mismatches != 0U) {
            std::cout << ", " << engine_functions.revision_mismatches << " revision mismatches";
        }
        std::cout << '\n';
    }
    runtime::register_native_call_captures_from_environment(native_function_dispatcher, std::cout);

    const bool join_pump_legacy_region_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_REGION");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    const bool graphics_run_ahead_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_GRAPHICS_RUN_AHEAD");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    std::uint64_t graphics_run_ahead_calls = 0U;
    std::uint64_t graphics_run_ahead_cycles = 0U;
    const bool join_pump_callback_region_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_CALLBACK_REGION");
        return flag == nullptr || std::string_view(flag) == "1";
    }();
    const bool join_pump_callback_tail_region_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_CALLBACK_TAIL_REGION");
        return flag == nullptr || std::string_view(flag) == "1";
    }();
    const bool join_pump_callback_tail_region_active =
        join_pump_callback_region_requested && join_pump_callback_tail_region_requested;
    const bool join_pump_region_requested =
        join_pump_legacy_region_requested || join_pump_callback_region_requested;
    std::optional<recompiler::JoinPumpRegion> join_pump_region;
    std::string_view join_pump_region_status = "disabled";
    if (join_pump_region_requested) {
        if (interpreter_only) {
            join_pump_region_status = "interpreter-only";
        } else if (!native_function_revision_matched) {
            join_pump_region_status = "retail-revision-mismatch";
        } else if (join_pump_callback_region_requested) {
            const std::size_t caller_source_word_count =
                join_pump_callback_tail_region_active ? 16U : 14U;
            const auto caller_extraction = recompiler::extract_elf_r5900_words(
                memory, plan,
                recompiler::R5900WordExtractionRequest{2041056U, caller_source_word_count,
                                                       caller_source_word_count});
            const auto callback_extraction = recompiler::extract_elf_r5900_words(
                memory, plan, recompiler::R5900WordExtractionRequest{2261056U, 12U, 12U});
            if (caller_extraction.succeeded() && caller_extraction.elf_executable_range_validated &&
                callback_extraction.succeeded() && callback_extraction.elf_executable_range_validated) {
                join_pump_region = recompiler::JoinPumpRegion::from_words(
                    caller_extraction.as_executable_word_span(),
                    callback_extraction.as_executable_word_span(),
                    join_pump_callback_tail_region_active);
                join_pump_region_status = join_pump_region.has_value()
                                              ? (join_pump_callback_tail_region_active
                                                     ? "active-callback-tail"
                                                     : "active-callback")
                                              : "callback-layout-mismatch";
            } else {
                join_pump_region_status = "elf-callback-source-rejected";
            }
        } else {
            const auto extraction = recompiler::extract_elf_r5900_words(
                memory, plan, recompiler::R5900WordExtractionRequest{2041056U, 13U, 13U});
            if (extraction.succeeded() && extraction.elf_executable_range_validated) {
                join_pump_region =
                    recompiler::JoinPumpRegion::from_words(extraction.as_executable_word_span());
                join_pump_region_status =
                    join_pump_region.has_value() ? "active" : "source-layout-mismatch";
            } else {
                join_pump_region_status = "elf-source-rejected";
            }
        }
    }
    std::uint64_t native_function_calls = 0U;
    std::uint64_t native_function_retired_instructions = 0U;
    std::unordered_map<guest::GuestAddress, std::uint64_t> native_function_hits;
    std::array<std::uint64_t, 9> native_function_preflight_counts{};
    std::unordered_map<std::uint32_t, std::array<std::uint64_t, 32>> native_dma_fallback_sources;
    std::uint64_t compiled_block_dispatches = 0U;
    std::uint64_t compiled_block_retired_instructions = 0U;
    std::uint64_t compiled_block_ee_cycles = 0U;
    std::uint64_t compiled_block_fused_dispatches = 0U;
    std::uint64_t compiled_block_fused_instructions = 0U;
    std::uint64_t compiled_block_code_mismatches = 0U;
    const bool join_pump_specialized = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_SPECIALIZED");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    const bool join_pump_loop_batch = []() {
        const char* const value = std::getenv("TAKEDOWN_JOIN_PUMP_LOOP_BATCH");
        return value == nullptr || std::string_view(value) == "1";
    }();
    const bool join_pump_idle_summary = []() {
        const char* const value = std::getenv("TAKEDOWN_JOIN_PUMP_IDLE_SUMMARY");
        return value == nullptr || std::string_view(value) == "1";
    }();
    const bool join_pump_resume_idle_batch = []() {
        const char* const value = std::getenv("TAKEDOWN_JOIN_PUMP_RESUME_IDLE_BATCH");
        return value != nullptr && std::string_view(value) == "1";
    }();
    static std::uint64_t join_pump_resumed_idle_loops = 0U;
    const bool native_join_drain_wait = []() {
        const char* const value = std::getenv("TAKEDOWN_NATIVE_JOIN_DRAIN_WAIT");
        return value != nullptr && std::string_view(value) == "1";
    }();
    recompiler::JoinPumpRegion::IdleCodeValidationCache native_join_wait_code_cache{};
    std::uint64_t native_join_wait_calls = 0U;
    std::uint64_t native_join_wait_instructions = 0U;
    runtime::slus_21050::ObjectPacketCapture object_packet_capture;
    runtime::slus_21050::InstanceBatchCapture instance_batch_capture;
    runtime::slus_21050::ModelSubmissionProbe model_submission_probe;
    const bool join_pump_idle_diagnostics = []() {
        const char* const value = std::getenv("TAKEDOWN_JOIN_PUMP_IDLE_DIAGNOSTICS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    const bool join_pump_idle_code_cache_enabled = []() {
        const char* const value = std::getenv("TAKEDOWN_JOIN_PUMP_IDLE_CODE_CACHE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    recompiler::JoinPumpRegion::IdleCodeValidationCache join_pump_idle_code_cache{};
    std::map<std::uint32_t, std::uint64_t> join_pump_idle_unknown_states;
    if (join_pump_region.has_value()) {
        join_pump_region->enable_specialized_execution(join_pump_specialized);
    }
    std::uint64_t join_pump_region_calls = 0U;
    std::uint64_t join_pump_loop_batch_calls = 0U;
    std::uint64_t join_pump_idle_summary_instructions = 0U;
    std::uint64_t join_pump_region_inert_dmac_admissions = 0U;
    std::uint64_t join_pump_region_retired_instructions = 0U;
    std::uint64_t join_pump_region_zero_retired_rejections = 0U;
    std::uint64_t join_pump_graphics_deadline_queries = 0U;
    std::uint64_t join_pump_graphics_deadline_admitted_spans = 0U;
    std::uint64_t join_pump_graphics_deadline_retired_cycles = 0U;
    std::uint64_t join_pump_graphics_deadline_retired_instructions = 0U;
    std::uint64_t join_pump_graphics_deadline_endpoint_services = 0U;
    std::array<std::uint64_t, static_cast<std::size_t>(JoinPumpGraphicsDeadlineFallback::Count)>
        join_pump_graphics_deadline_fallbacks{};
    std::array<std::uint64_t, static_cast<std::size_t>(recompiler::JoinPumpRegionExit::Count)>
        join_pump_region_exits{};
    const auto record_join_pump_graphics_deadline_fallbacks =
        [&join_pump_graphics_deadline_fallbacks](const std::uint32_t fallback_mask) {
        const std::array<JoinPumpGraphicsDeadlineFallback, 6> fallbacks{
            JoinPumpGraphicsDeadlineFallback::Deadline,
            JoinPumpGraphicsDeadlineFallback::ExistingBudget,
            JoinPumpGraphicsDeadlineFallback::ProfileTiming,
            JoinPumpGraphicsDeadlineFallback::GraphicsDiagnostics,
            JoinPumpGraphicsDeadlineFallback::EePcSampling,
            JoinPumpGraphicsDeadlineFallback::SubframeInput};
        for (std::size_t index = 0U; index != fallbacks.size(); ++index) {
            if ((fallback_mask & (1U << static_cast<std::uint32_t>(fallbacks[index]))) != 0U) {
                ++join_pump_graphics_deadline_fallbacks[index];
            }
        }
    };
    const bool compiled_coverage_available = !compiled_block_dispatcher.empty();
    EeDispatchPreparationCache ee_dispatch_preparation_cache(
        interpreter_only ? nullptr : &native_function_dispatcher,
        !interpreter_only && compiled_coverage_available ? &compiled_block_dispatcher : nullptr);
    const std::filesystem::path* const compiled_block_fusion_profile_directory =
        ee_host_phase_profile_directory.has_value()
            ? &*ee_host_phase_profile_directory
            : (ee_pc_profile_directory.has_value() ? &*ee_pc_profile_directory : nullptr);
    compiled_block_dispatcher.set_fusion_statistics_enabled(
        compiled_block_fusion_profile_directory != nullptr && compiled_coverage_available &&
        !interpreter_only);
    std::unique_ptr<CompiledBudgetProfiler> compiled_budget_profiler;
    const bool elide_inert_dmac_poll = []() {
        const char* const flag = std::getenv("TAKEDOWN_ELIDE_INERT_DMAC_POLL");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    const bool join_pump_graphics_deadline_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_GRAPHICS_DEADLINE");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    const bool join_pump_graphics_deadline_cache_telemetry_requested = []() {
        const char* const flag = std::getenv("TAKEDOWN_JOIN_PUMP_GRAPHICS_DEADLINE_CACHE_TELEMETRY");
        return flag != nullptr && std::string_view(flag) == "1";
    }();
    const bool join_pump_graphics_deadline_cache_telemetry_enabled =
        join_pump_graphics_deadline_requested && join_pump_graphics_deadline_cache_telemetry_requested;
    memory.enable_graphics_service_deadline_cache_counters_for_testing(
        join_pump_graphics_deadline_cache_telemetry_enabled);
    {
        const char* const flag = std::getenv("TAKEDOWN_COMPILED_BUDGET_PROFILE");
        if (flag != nullptr && std::string_view(flag) == "1") {
            if (compiled_block_fusion_profile_directory == nullptr) {
                throw std::invalid_argument(
                    "TAKEDOWN_COMPILED_BUDGET_PROFILE=1 requires --ee-pc-profile or "
                    "--ee-host-phase-profile");
            }
            compiled_budget_profiler = std::make_unique<CompiledBudgetProfiler>();
        }
    }
    {
        const char* const flag = std::getenv("TAKEDOWN_LOAD_FUSION");
        compiled_block_dispatcher.set_load_fusion_enabled(flag != nullptr && flag[0] == '1' &&
                                                          flag[1] == '\0');
    }
    const char* const delay_slot_flag = std::getenv("TAKEDOWN_DELAY_SLOT_DISPATCH");
    if (delay_slot_flag != nullptr && delay_slot_flag[0] == '1' && delay_slot_flag[1] == '\0') {
        compiled_block_dispatcher.set_delay_slot_dispatch_enabled(true);
    }
    std::uint64_t inert_dmac_poll_admissions = 0U;
    std::uint32_t inert_dmac_poll_admitted_channels = 0U;
    guest::EeKernelHle kernel(std::filesystem::path(argv[2]).parent_path());
    kernel.set_mod_overlay(&mod_overlay);
    std::optional<Slus21050VariableFrameRate> variable_frame_rate;
    std::optional<VariableFrameClock> variable_frame_clock;
    if (frame_rate_option.value_or(loaded_settings.settings.graphics.frame_rate) ==
        config::FrameRateMode::Variable) {
        if (const auto mismatch = Slus21050VariableFrameRate::signature_mismatch(memory)) {
            throw std::runtime_error("variable frame rate is unavailable: " + *mismatch);
        }
        VariableFrameClockConfiguration configuration{};
        configuration.script = frame_time_script.value_or(FrameTimeScript::live());
        configuration.lead = frame_time_script_lead;
        configuration.minimum_frame_cycles = 294912U;
        std::cout << "Frame rate: variable (one tick per displayed frame, "
                  << (configuration.script.is_live() ? "host clock" : "scripted frame times")
                  << (configuration.lead.has_value() ? ", scripted lead" : "") << ")\n"
                  << std::flush;
        variable_frame_rate.emplace();
        set_slus_21050_chase_camera_real_time_easing(true);
        variable_frame_clock.emplace(configuration);
    } else if (frame_time_script.has_value() || frame_time_script_lead.has_value() ||
               guest_input_time_keyed) {
        throw std::invalid_argument(
            "--frame-time-script, --frame-time-script-lead and --guest-input-time-keyed require the "
            "variable frame rate");
    }
    config::AppSettings pc_options_settings = loaded_settings.settings;
    if (const char* const fov = std::getenv("TAKEDOWN_FIELD_OF_VIEW")) {
        pc_options_settings.graphics.field_of_view =
            static_cast<std::uint32_t>(std::strtoul(fov, nullptr, 10));
    }
    if (const char* const panini = std::getenv("TAKEDOWN_PANINI")) {
        pc_options_settings.graphics.panini = panini[0] == '1';
    }
    ui::PcOptionsSession pc_options(ui::PcOptionsSessionConfig{
        .settings = pc_options_settings,
        .settings_path = config::default_settings_path(),
        .environment = {.supersampling_available = parallel_gs_live_fifo_path.has_value(),
                        .variable_frame_timing_active = variable_frame_rate.has_value(),
                        .frame_timing_set_by_launcher = frame_rate_option.has_value()},
        // TAKEDOWN_PC_OPTIONS=force adds the pages to scripted and windowless
        // (test) runs too.
        .enabled = [&]() {
            const char* const value = std::getenv("TAKEDOWN_PC_OPTIONS");
            const bool forced = value != nullptr && std::string_view(value) == "force";
            return forced || (window_requested && !guest_input_timeline_enabled);
        }()});
    Slus21050GraphicsOptions game_graphics_options;
    if (native_function_revision_matched &&
        !game_graphics_options.register_native_effects(native_function_dispatcher, memory)) {
        std::cerr << "PC options: native post-processing revision admission failed\n";
    }
    Slus21050NativePcOptions native_pc_options;
    Slus21050SettingsPage settings_page;
    Slus21050OnlineCount online_count;
    Slus21050MenuMouse menu_mouse;
    std::optional<Slus21050BootVolume> boot_volume;
    if ((guest_input_replay == nullptr && guest_input_recorder == nullptr) || []() {
        const char* const value = std::getenv("TAKEDOWN_BOOT_VOLUME");
        return value != nullptr && std::string_view(value) == "force";
    }()) {
        std::optional<GameAudioLevels> remembered;
        if (loaded_settings.settings.audio.game_music_level.has_value() &&
            loaded_settings.settings.audio.game_effects_level.has_value()) {
            remembered = GameAudioLevels{*loaded_settings.settings.audio.game_music_level,
                                         *loaded_settings.settings.audio.game_effects_level};
        }
        boot_volume.emplace(remembered);
    }
    std::optional<MenuMouseScript> menu_mouse_script;
    if (const char* const script = std::getenv("TAKEDOWN_MENU_MOUSE_SCRIPT");
    script != nullptr && *script != '\0') {
        menu_mouse_script = MenuMouseScript::parse(script);
        if (!menu_mouse_script.has_value()) {
            throw std::invalid_argument(
                "TAKEDOWN_MENU_MOUSE_SCRIPT: expected poll:x,y,buttons[,wheel];...");
        }
    }
    ui::ControlsEditor controls_session{
        platform::input_bindings_path_for(config::default_settings_path()),
        platform::gamepad_mappings_path_for(config::default_settings_path())};
    // The first-run question (PlayStation 2 or Xbox buttons), asked once on
    // the front end while no layout is saved. Scripted runs skip it unless
    // TAKEDOWN_CONTROL_STYLE_PROMPT=1, which also asks when one is saved.
    ui::ControlStylePrompt control_style_prompt;
    bool control_style_prompt_pending = !controls_session.style().has_value();
    if (const char* const setting = std::getenv("TAKEDOWN_CONTROL_STYLE_PROMPT");
        setting != nullptr && *setting != '\0') {
        control_style_prompt_pending = std::string_view(setting) != "0";
    } else if (std::getenv("TAKEDOWN_NAV_SCRIPT") != nullptr || std::getenv("TAKEDOWN_MENU_MOUSE_SCRIPT") != nullptr) {
        control_style_prompt_pending = false;
    }
    std::uint64_t control_style_prompt_last_poll = ~std::uint64_t{0};
    // The game gets no input while the question is open, nor after it until
    // every button is let go (else the confirming press reaches the menu).
    bool control_style_blocks_input = false;
    bool control_style_click_held = true;
    // Whether the bindings last given to the window were the menu ones
    // (ControlsEditor::game_bindings); empty when they must be sent again.
    std::optional<bool> game_bindings_menu;
    Slus21050CameraFov camera_fov;
    std::uint64_t ui_submission_step = ~std::uint64_t{0};
    std::uint64_t panini_warp_serial = 0U;
    bool native_pc_options_reported = false;
    NativeModsPageState native_mods_page_state;
    platform::ParallelGsHostUi native_mods_ui;
    std::optional<Slus21050NativeCars> native_cars;
    if (const char* const setting = std::getenv("TAKEDOWN_NATIVE_CARS");
    setting == nullptr || std::string_view(setting) != "0") {
        const char* const verify = std::getenv("TAKEDOWN_NATIVE_CARS_VERIFY");
        native_cars.emplace(verify != nullptr && std::string_view(verify) == "1");
        memory.set_vu1_launch_observer(&*native_cars, 0U, 0U);
    }
    // NS-3: native command stream (TAKEDOWN_NATIVE_CMD=0 off (default), 1 shadow, 2 tokens replace strips)
    std::unique_ptr<takedown::platform::NativeCmdEmitter> native_cmd_emitter;
    {
        auto native_cmd_config = takedown::platform::native_cmd_config_from_environment();
        // Live renderer sessions default to native command mode 2; TAKEDOWN_NATIVE_CMD=0 turns it off. The
        // capability check below still falls back to the legacy stream when the renderer does not announce v1.
        if (std::getenv("TAKEDOWN_NATIVE_CMD") == nullptr && parallel_gs_live_fifo_path.has_value()) {
            native_cmd_config.mode = 2;
        }
        if (native_cmd_config.mode != 0 && parallel_gs_stream_writer != nullptr) {
            const bool supported = native_cmd_config.force || !parallel_gs_live_fifo_path.has_value() ||
                                   takedown::platform::native_cmd_renderer_supports(*parallel_gs_live_fifo_path, 1U);
            if (supported) {
                native_cmd_emitter = std::make_unique<takedown::platform::NativeCmdEmitter>(
                    *parallel_gs_stream_writer, native_cars.has_value() ? &*native_cars : nullptr, native_cmd_config);
                memory.set_vu1_launch_observer(native_cmd_emitter.get(), 0U, 0U);
                memory.gs_capture().set_raw_gif_observer(native_cmd_emitter.get());
            } else {
                std::cerr << "[NATIVECMD] renderer did not announce native command protocol v1: legacy stream\n";
            }
        }
    }
    const auto refresh_native_mods_page_state = [&native_mods_page_state, &mods_manager]() {
        native_mods_page_state.mods.clear();
        std::uint32_t slot = 0U;
        for (const auto& mod : mods_manager.mods()) {
            NativeModsPageMod entry;
            entry.label = mod.available ? mod.display_name : mod.display_name + " (missing)";
            entry.enabled = mod.enabled && mod.available;
            entry.slot = entry.enabled ? ++slot : 0U;
            entry.information = presentation::mods_page_information(mod);
            native_mods_page_state.mods.push_back(std::move(entry));
        }
    };
    if (mods_manager.active()) {
        refresh_native_mods_page_state();
    }
    const bool game_graphics_options_available = [&memory]() {
        const auto mismatch = Slus21050GraphicsOptions::signature_mismatch(memory);
        if (mismatch.has_value()) {
            std::cerr << "PC options: game picture options are unavailable: " << *mismatch << '\n';
            return false;
        }
        return true;
    }();
    Slus21050SceneryDetail game_scenery_detail;
    const bool game_scenery_detail_available = [&memory]() {
        const auto mismatch = Slus21050SceneryDetail::signature_mismatch(memory);
        if (mismatch.has_value()) {
            std::cerr << "PC options: render distance and model quality are unavailable: "
                      << *mismatch << '\n';
            return false;
        }
        return true;
    }();
    const std::uint64_t variable_frame_floor_cycles =
        variable_frame_clock.has_value() ? variable_frame_clock->minimum_frame_cycles()
                                         : 0U;
    const bool graphics_run_ahead =
        graphics_run_ahead_requested ? true : variable_frame_rate.has_value();
    const std::uint32_t graphics_frames_in_flight = []() {
        const char* const text = std::getenv("TAKEDOWN_GRAPHICS_FRAMES_IN_FLIGHT");
        if (text == nullptr || text[0] <= '0' || text[0] > '8' || text[1] != '\0') {
            return 1U;
        }
        return static_cast<std::uint32_t>(text[0] - '0');
    }();
    const GraphicsAsyncMode graphics_async_mode = [&graphics_run_ahead]() {
        const char* const flag = std::getenv("TAKEDOWN_GRAPHICS_ASYNC");
        if (!graphics_run_ahead) {
            return GraphicsAsyncMode::Off;
        }
        if (flag == nullptr) {
            return GraphicsAsyncMode::Decoupled;
        }
        const std::string_view value(flag);
        if (value == "1" || value == "on") {
            return GraphicsAsyncMode::On;
        }
        if (value == "measure") {
            return GraphicsAsyncMode::Measure;
        }
        if (value == "decoupled") {
            return GraphicsAsyncMode::Decoupled;
        }
        return GraphicsAsyncMode::Off;
    }();
    if (graphics_async_mode != GraphicsAsyncMode::Off) {
        const auto env_u64 = [](const char* name, const std::uint64_t fallback) -> unsigned long long {
            const char* text = std::getenv(name);
            if (text == nullptr || *text == '\0') {
                return fallback;
            }
            char* end = nullptr;
            const unsigned long long value = std::strtoull(text, &end, 10);
            return end != text && *end == '\0' ? value : fallback;
        };
        memory.configure_graphics_async(graphics_async_mode,
                                        env_u64("TAKEDOWN_GRAPHICS_ASYNC_LATENCY", 1500000U),
                                        env_u64("TAKEDOWN_GRAPHICS_ASYNC_MIN_QW", 1U));
    }
    std::optional<std::uint64_t> variable_frame_target{};
    std::optional<std::uint64_t> variable_frame_hook_step{};
    std::uint64_t variable_frame_host_wait_nanoseconds = 0U;
    std::vector<audio::RwaSpu2KeyOn> recent_rwa_key_ons{};
    std::uint64_t rwa_key_on_count = 0U;
    // 0 = native SPU2 boot transaction, 1 = shadow IOP boots RWA.IRX, 2 = shadow IOP stays attached.
    const int rwa_shadow_mode = [] {
        const char* const value = std::getenv("TAKEDOWN_RWA_SHADOW");
        if (value == nullptr) {
            return 0;
        }
        if (std::string_view{value} == "1") {
            return 2;
        }
        if (std::string_view{value} == "boot") {
            return 1;
        }
        return 0;
    }();
    auto guest_spu2 = std::make_unique<audio::Spu2System>();
    audio::Spu2GuestLink guest_spu2_link(*guest_spu2);
    const bool spu2_output_enabled = [] {
        const char* const value = std::getenv("TAKEDOWN_SPU2_RENDER");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const char* const spu2_dump_path = std::getenv("TAKEDOWN_SPU2_DUMP");
    const bool audio_master_clock = [] {
        const char* const value = std::getenv("TAKEDOWN_AUDIO_MASTER_CLOCK");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const bool audio_master_pacing = audio_master_clock && !variable_frame_clock.has_value();
    const std::int64_t kAudioMasterResumeFrames = 1024;
    std::uint64_t audio_master_blocks = 0U;
    std::uint64_t audio_master_blocked_nanoseconds = 0U;
    std::optional<std::chrono::steady_clock::time_point> audio_master_anchor_time{};
    std::int64_t audio_master_anchor_samples = 0;
    std::uint64_t audio_master_checked_samples = 0U;
    audio::Spu2Renderer spu2_renderer(*guest_spu2);
    if (spu2_output_enabled) {
        audio::HostAudioSink::OpenParams params;
        params.gain = loaded_settings.settings.audio.master_volume;
        params.startup_buffer_frames = 1024U;
        params.underrun_recovery_frames = 1024U;
        if (variable_frame_clock.has_value()) {
            params.startup_buffer_frames = 2400U;
            params.underrun_recovery_frames = 2400U;
        }
        params.max_queued_frames = variable_frame_clock.has_value() ? 24000U : 4800U;
        params.time_stretch_underruns = [] {
            const char* const value = std::getenv("TAKEDOWN_AUDIO_TIME_STRETCH");
            return value == nullptr || std::string_view{value} != "0";
        }();
        static_cast<void>(spu2_renderer.sink().open(params));
    }
    const auto pace_audio_master = [&audio_master_pacing, &spu2_renderer, &audio_master_checked_samples,
                                    &audio_master_anchor_time, &audio_master_anchor_samples,
                                    &kAudioMasterResumeFrames, &audio_master_blocks,
                                    &audio_master_blocked_nanoseconds]() {
        if (!audio_master_pacing) {
            return;
        }
        auto& audio_sink = spu2_renderer.sink();
        if (audio_sink.status() != audio::HostAudioSink::Status::Playing) {
            return;
        }
        if (spu2_renderer.samples_rendered() == audio_master_checked_samples) {
            return;
        }
        audio_master_checked_samples = spu2_renderer.samples_rendered();
        const auto guest_lead_frames = [&]() {
            const auto now = std::chrono::steady_clock::now();
            const auto produced = static_cast<std::int64_t>(spu2_renderer.samples_rendered());
            if (!audio_master_anchor_time.has_value()) {
                audio_master_anchor_time = now;
                audio_master_anchor_samples = produced;
            }
            const auto elapsed = now - *audio_master_anchor_time;
            const auto microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            const auto host_frames = microseconds / 1000000 * 48000 +
                                     microseconds % 1000000 * 48000 / 1000000;
            long long lead = produced - audio_master_anchor_samples - host_frames;
            if (lead < -kAudioMasterResumeFrames) {
                audio_master_anchor_samples = produced - host_frames + kAudioMasterResumeFrames;
                lead = -kAudioMasterResumeFrames;
            }
            return lead;
        };
        if (guest_lead_frames() <= 2 * kAudioMasterResumeFrames) {
            return;
        }
        const auto block_start = std::chrono::steady_clock::now();
        ++audio_master_blocks;
        while (audio_sink.status() == audio::HostAudioSink::Status::Playing &&
               guest_lead_frames() > kAudioMasterResumeFrames &&
               std::chrono::steady_clock::now() - block_start < std::chrono::milliseconds(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // RECOVERY: the original loads the audio_master_blocked_nanoseconds closure field before
        // computing the clamped elapsed time; every spelling tried schedules the clamp first.
        const auto blocked_time = std::chrono::steady_clock::now() - block_start;
        audio_master_blocked_nanoseconds +=
            static_cast<std::uint64_t>(std::max<std::int64_t>(blocked_time.count(), 0));
        platform::record_host_wait(
            platform::HostWaitSite::AudioMasterPacing,
            static_cast<std::uint64_t>(std::max<std::int64_t>(blocked_time.count(), 0)));
    };
    if (audio_master_pacing) {
        spu2_renderer.set_submit_observer(pace_audio_master);
    }
    if (spu2_dump_path != nullptr) {
        if (!spu2_renderer.start_wav_dump(std::filesystem::path(spu2_dump_path))) {
            std::cerr << "Warning: TAKEDOWN_SPU2_DUMP path unusable: " << spu2_dump_path << '\n';
        }
    }
    audio::RwaSpu2VoiceState rwa_spu2_voices(
        [&rwa_key_on_count, &recent_rwa_key_ons, &guest_spu2_link](const audio::RwaSpu2KeyOn& event) {
        ++rwa_key_on_count;
        if (recent_rwa_key_ons.size() == 64U) {
            recent_rwa_key_ons.erase(recent_rwa_key_ons.begin());
        }
        recent_rwa_key_ons.push_back(event);
        std::fprintf(stderr,
                     "[RWA-KON] core=%u voice=%u ssa=0x%06x pitch=0x%04x vol=%04x/%04x\n",
                     event.core, event.voice, event.start_address, event.pitch,
                     event.volume_left, event.volume_right);
        guest_spu2_link.program_key_on(event);
    });
    kernel.set_rwa_voice_play_probe([&guest_spu2](const std::uint32_t ssa) {
        const unsigned int word = ssa >> 1U;
        bool any = false;
        for (std::uint32_t i = 0U; i < 64U && !any; ++i) {
            any = guest_spu2->ram_read_word(word + i) != 0U;
        }
        if (!any) {
            std::fprintf(stderr, "[NATIVE-PLAY] WARNING ssa=0x%06x targets EMPTY SPU2 RAM\n", ssa);
        }
    });
    kernel.set_rwa_voice_play([&guest_spu2_link](const guest::EeKernelHle::EeRwaVoicePlay& play) {
        switch (play.action) {
            case guest::EeKernelHle::EeRwaVoiceAction::KeyOff:
            guest_spu2_link.key_off(play.core, 1U << play.voice);
            return;
            case guest::EeKernelHle::EeRwaVoiceAction::SetPitch:
            guest_spu2_link.set_param(play.core, play.voice, 0x0200U, play.pitch);
            return;
            case guest::EeKernelHle::EeRwaVoiceAction::SetLoopAddress:
            guest_spu2_link.set_param(play.core, play.voice, 0x2140U, play.loop_address);
            return;
            case guest::EeKernelHle::EeRwaVoiceAction::SetVolumeLeft:
            guest_spu2_link.set_voice_volume_at_sample_boundary(play.core, play.voice, false,
                                                                play.volume_left);
            return;
            case guest::EeKernelHle::EeRwaVoiceAction::SetVolumeRight:
            guest_spu2_link.set_voice_volume_at_sample_boundary(play.core, play.voice, true,
                                                                play.volume_right);
            return;
            default:
            break;
        }
        // RECOVERY: the original key-on event temporary is a const-qualified unnamed object.
        guest_spu2_link.program_key_on_with_post_loop(
            audio::RwaSpu2KeyOn{play.core, play.voice, play.start_address, play.pitch,
                                play.volume_left, play.volume_right, play.adsr1, play.adsr2},
            play.loop_address);
        std::fprintf(stderr,
                     "[NATIVE-PLAY] core=%u voice=%u ssa=0x%06x lsa=0x%06x pitch=0x%04x "
                     "vol=%04x/%04x adsr=%04x/%04x\n",
                     play.core, play.voice, play.start_address, play.loop_address, play.pitch,
                     play.volume_left, play.volume_right, play.adsr1, play.adsr2);
    });
    kernel.set_spu_transfer_materialized(
        [&guest_spu2_link](const guest::EeKernelHle::EeRwaSpuTransferMaterialized& transfer) {
        if (transfer.data.empty()) {
            return;
        }
        guest_spu2_link.dma_upload(transfer.spu_destination,
                                   std::as_bytes(std::span<const std::uint8_t>(transfer.data)));
        std::fprintf(stderr, "[NATIVE-XFER] iop=0x%06x spu2=0x%06x size=%u\n",
                     transfer.iop_source, transfer.spu_destination,
                     static_cast<unsigned>(transfer.data.size()));
    });
    rwa_spu2_voices.set_key_off_sink(
        [&guest_spu2_link](const std::uint8_t core, const std::uint32_t mask) {
        guest_spu2_link.key_off(core, mask);
    });
    rwa_spu2_voices.set_param_sink([&guest_spu2_link](const std::uint8_t core, const std::uint8_t voice,
                                                      const std::uint16_t entry,
                                                      const std::uint32_t value) {
        guest_spu2_link.set_param(core, voice, entry, value);
    });
    std::optional<audio::RwaIopShadowRuntime> rwa_iop_shadow{};
    std::uint32_t rwa_shadow_spu2_uploads = 0U;
    std::uint8_t rwa_shadow_system_bank_mask = 0U;
    const std::filesystem::path rwa_irx_path =
        std::filesystem::path(argv[2]).parent_path() / "IOP" / "RWA.IRX";
    audio::RwaSpu2BootInitializer rwa_native_boot{};
    audio::RwaSpu2BootInitializer::ApplyResult rwa_native_boot_last{};
    std::uint32_t rwa_native_boot_calls = 0U;
    std::uint32_t rwa_native_boot_failures = 0U;
    if (rwa_shadow_mode == 0) {
        if (!rwa_native_boot.load(rwa_irx_path)) {
            std::cerr << "Error: native RWA opcode-0x11 setup unavailable: " << rwa_native_boot.error()
                      << '\n';
            return 1;
        }
        kernel.set_rwa_initialize_observer(
            [&rwa_native_boot, &rwa_spu2_voices, &guest_spu2_link, &rwa_native_boot_last,
             &rwa_native_boot_calls, &rwa_native_boot_failures]() {
            ++rwa_native_boot_calls;
            rwa_native_boot_last = rwa_native_boot.apply(rwa_spu2_voices, guest_spu2_link);
            if (!rwa_native_boot_last.applied) {
                ++rwa_native_boot_failures;
                std::fprintf(stderr, "[RWA-NATIVE-INIT] rejected: retail opcode-0x11 SPU2 "
                                     "transaction unavailable\n");
                return false;
            }
            std::fprintf(stderr,
                         "[RWA-NATIVE-INIT] module-start=%u/%u op11-addr=%u param=%u attr=%u "
                         "dma=%u bytes=%u\n",
                         rwa_native_boot_last.module_start_address_writes,
                         rwa_native_boot_last.module_start_core_attribute_writes,
                         rwa_native_boot_last.address_writes, rwa_native_boot_last.parameter_writes,
                         rwa_native_boot_last.core_attribute_writes,
                         rwa_native_boot_last.dma_transfers, rwa_native_boot_last.dma_bytes);
            return true;
        });
    }
    bool rwa_shadow_boot_done = rwa_shadow_mode != 1;
    kernel.set_gtfs_cdvd_interface_address(532272U);
    kernel.set_rwa_iop_work_area(471600U);
    if (rwa_shadow_mode != 0 && std::filesystem::exists(rwa_irx_path)) {
        rwa_iop_shadow.emplace(rwa_spu2_voices);
        if (rwa_iop_shadow->load(rwa_irx_path)) {
            rwa_iop_shadow->bind(kernel);
            auto& rwa_shadow_executor = rwa_iop_shadow->executor();
            rwa_shadow_executor.set_spu2_upload_sink(
                [&guest_spu2_link, &rwa_shadow_executor, &rwa_shadow_spu2_uploads,
                 &rwa_shadow_system_bank_mask](const std::uint32_t spu_address, const std::uint32_t,
                                               const std::uint32_t size) {
                const auto resident = rwa_shadow_executor.spu2_range(spu_address, size);
                if (!resident.empty()) {
                    guest_spu2_link.dma_upload(spu_address, resident);
                    ++rwa_shadow_spu2_uploads;
                    if (spu_address == 16384U && size == 2048U) {
                        rwa_shadow_system_bank_mask |= 1U;
                    }
                    if (spu_address == 18432U && size == 2048U) {
                        rwa_shadow_system_bank_mask |= 2U;
                    }
                    if (spu_address == 20496U && size == 64U) {
                        rwa_shadow_system_bank_mask |= 4U;
                    }
                    std::fprintf(stderr, "[RWA-SHADOW] boot upload spu2=0x%06x size=%u\n", spu_address,
                                 size);
                }
            });
            const std::filesystem::path gtfs_irx_path =
                std::filesystem::path(argv[2]).parent_path() / "IOP" / "GTFSCDVD.IRX";
            const std::filesystem::path disc_cache_path =
                std::filesystem::path("tmp") / "Burnout3Takedown_USA.raw.bin";
            if (std::filesystem::exists(gtfs_irx_path) &&
                rwa_iop_shadow->load_extension(gtfs_irx_path)) {
                static const int disc_fd = _open(disc_cache_path.string().c_str(), _O_BINARY);
                if (disc_fd >= 0) {
                    rwa_shadow_executor.set_disc_sector_source(
                        [](const std::uint32_t lbn, const std::uint32_t sectors, std::byte* const out) {
                        for (std::uint32_t sector = 0U; sector < sectors; ++sector) {
                            // RECOVERY: the original hoists sector * 2048 out of the read loop as an
                            // unnamed value and passes a char* temporary to _read.
                            const unsigned long long offset =
                                static_cast<std::uint64_t>(lbn + sector) * 2448U;
                            std::size_t got = 0U;
                            while (got < 2048U) {
                                if (_lseeki64(disc_fd, static_cast<long long>(offset + got),
                                              SEEK_SET) < 0) {
                                    return false;
                                }
                                const int count = _read(disc_fd,
                                                        reinterpret_cast<char*>(out + (static_cast<std::uint64_t>(sector) * 2048U + got)),
                                                        static_cast<unsigned int>(2048U - got));
                                if (count <= 0) {
                                    return false;
                                }
                                got += static_cast<std::size_t>(count);
                            }
                        }
                        return true;
                    });
                } else {
                    std::cerr << "Warning: GTFSCDVD loaded but the raw sector cache is unavailable ("
                              << disc_cache_path.string()
                              << "); executed cdvdman reads will honestly fail\n";
                }
            } else if (std::filesystem::exists(gtfs_irx_path)) {
                std::cerr << "Warning: retail GTFSCDVD.IRX extension failed to load; the HLE stream "
                             "path stays authoritative\n";
            }
        } else {
            rwa_iop_shadow.reset();
            std::cerr << "Warning: retail RWA.IRX shadow initialization failed; executed retail SPU2 "
                         "traffic is unavailable\n";
        }
    }
    std::FILE* music_journal_file = [] {
        const char* const journal_path = std::getenv("TAKEDOWN_MUSIC_JOURNAL");
        return journal_path != nullptr ? std::fopen(journal_path, "w") : nullptr;
    }();
    const auto music_journal_start = std::chrono::steady_clock::now();
    const auto music_journal_line = [&music_journal_file, &music_journal_start](
                                        const std::string_view event, const std::string_view detail) {
        if (music_journal_file == nullptr) {
            return;
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - music_journal_start)
                .count();
        std::fprintf(music_journal_file, "%.3f %.*s %.*s\n", seconds, static_cast<int>(event.size()),
                     event.data(), static_cast<int>(detail.size()), detail.data());
        std::fflush(music_journal_file);
    };
    std::uint64_t retail_music_stream_reads = 0U;
    const auto is_retail_music_stream = [](const std::string_view path) {
        std::string folded;
        folded.reserve(path.size());
        for (const char character : path) {
            char normalized = character;
            if (character >= 'A' && character <= 'Z') {
                normalized = static_cast<char>(character + ('a' - 'A'));
            } else if (character == '/') {
                normalized = '\\';
            }
            folded.push_back(normalized);
        }
        const std::string_view view{folded};
        return view.ends_with("_eatrax0.rws") || view.ends_with("_eatrax1.rws") ||
               view.ends_with("_femain.rws");
    };
    kernel.set_rwa_stream_journal(
        [&is_retail_music_stream, &music_journal_line, &music_journal_file,
         &retail_music_stream_reads](const guest::EeRwaStreamJournalEvent& event) {
        if (event.phase == guest::EeRwaStreamJournalEvent::Phase::Completed) {
            // RECOVERY: the original keeps the two state comparisons as separate branches; this
            // spelling is folded into a single range test (state - 2 <= 1).
            if (event.state == 2U || event.state == 3U) {
                if (is_retail_music_stream(event.path)) {
                    ++retail_music_stream_reads;
                }
            }
        }
        if (music_journal_file == nullptr) {
            return;
        }
        static const char* const kPhases[] = {"request", "dropped", "completed"};
        std::ostringstream detail;
        detail << "seq=" << event.sequence << " slot=" << event.slot << " state=" << event.state
               << " flags=0x" << std::hex << event.flags << std::dec << " seek=" << event.seek
               << " count=" << event.byte_count << " offset=" << event.file_offset << " dest=0x"
               << std::hex << event.destination << std::dec << " path=\"" << event.path
               << "\" phase=" << kPhases[static_cast<std::size_t>(event.phase)]
               << " detail=" << event.detail;
        music_journal_line("stream", detail.str());
    });
    guest::DirtyDnasCompatHle dirty_dnas_compat;
    std::unique_ptr<platform::HostWindow> host_window;
    std::optional<presentation::GsRendererWorker> gs_renderer_worker;
    FEmainSourcePrimaryCommandSink femain_source_primary_command_sink;
    bool femain_source_primary_command_sink_installed = false;
    if (window_requested) {
        platform::HostWindowConfig config;
        config.title = std::string(takedown::kGameTitle);
        config.stats_overlay_enabled = stats_overlay_startup;
        config.debug_hotkeys_enabled = false;
        config.width = loaded_settings.settings.graphics.resolution.width;
        config.height = loaded_settings.settings.graphics.resolution.height;
        config.timing_enabled = host_window_timing_enabled;
        config.input_only = parallel_gs_only && parallel_gs_input_fifo_path.has_value();
        config.gamepad_mappings_path =
            platform::gamepad_mappings_path_for(config::default_settings_path());
        host_window = std::make_unique<platform::HostWindow>(std::move(config));
        host_window->set_input_bindings(controls_session.bindings());
        if (loaded_settings.settings.graphics.display_mode != config::DisplayMode::Windowed) {
            static_cast<void>(host_window->set_fullscreen(true));
        }
        static_cast<void>(host_window->set_vsync(loaded_settings.settings.graphics.vsync));
    }
    if (render_gs) {
        if (femain_terminal_diagnostic != nullptr) {
            memory.gs_capture().set_diagnostic_command_observer(femain_terminal_diagnostic.get());
        }
        presentation::GsRendererWorkerConfig worker_config;
        if (femain_terminal_diagnostic != nullptr) {
            worker_config.dispatch_hook = [diagnostic = femain_terminal_diagnostic.get()](
                                              const presentation::GsRendererWorkerCommandKind kind,
                                              const std::uint64_t value) {
                diagnostic->on_renderer_dispatch(kind, value);
            };
        }
        gs_renderer_worker.reset();
        gs_renderer_worker.emplace(std::move(worker_config));
        memory.gs_capture().set_command_observer(&*gs_renderer_worker);
    } else if (femain_terminal_diagnostic != nullptr) {
        memory.gs_capture().set_diagnostic_command_observer(nullptr);
        memory.gs_capture().set_command_observer(femain_terminal_diagnostic.get());
    } else if (femain_live_transaction_enabled) {
        memory.gs_capture().set_command_observer(&femain_source_primary_command_sink);
        femain_source_primary_command_sink_installed = true;
    }
    std::size_t committed_steps = 0U;
    struct ScriptedInputPress {
        double at_seconds;
        platform::HostControllerState state;
    };
    std::vector<ScriptedInputPress> scripted_input_presses{};
    std::size_t scripted_input_next = 0U;
    if (const char* const script_path = std::getenv("TAKEDOWN_INPUT_SCRIPT")) {
        std::ifstream script(script_path);
        std::string line;
        while (std::getline(script, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream stream(line);
            double at = 0.0;
            std::string button;
            if (!(stream >> at >> button)) {
                continue;
            }
            platform::HostControllerState press{};
            if (button == "up") {
                press.dpad_up = true;
            } else if (button == "down") {
                press.dpad_down = true;
            } else if (button == "left") {
                press.dpad_left = true;
            } else if (button == "right") {
                press.dpad_right = true;
            } else if (button == "cross") {
                press.cross = true;
            } else if (button == "circle") {
                press.circle = true;
            } else if (button == "start") {
                press.start = true;
            } else if (button == "select") {
                press.select = true;
            } else if (button == "l1") {
                press.left_shoulder = true;
            } else if (button == "r1") {
                press.right_shoulder = true;
            } else {
                continue;
            }
            scripted_input_presses.push_back({at, press});
        }
        std::sort(scripted_input_presses.begin(), scripted_input_presses.end(),
                  [](const ScriptedInputPress& left, const ScriptedInputPress& right) {
            return left.at_seconds < right.at_seconds;
        });
        std::fprintf(stderr, "[INPUT-SCRIPT] %zu presses loaded\n", scripted_input_presses.size());
    }
    std::optional<SubframeInputBridge> subframe_input_bridge{};
    if (subframe_input) {
        subframe_input_bridge.emplace();
    }
    std::size_t executed_instructions = 0U;
    std::size_t handled_kernel_calls = 0U;
    std::size_t dispatched_alarm_callbacks = 0U;
    std::size_t dispatched_dmac_callbacks = 0U;
    std::size_t dispatched_intc_events = 0U;
    std::size_t dispatched_sif_command_callbacks = 0U;
    std::size_t dispatched_thread_switches = 0U;
    std::size_t completed_guest_threads = 0U;
    std::size_t raised_vblank_starts = 0U;
    std::uint64_t next_parallel_gs_completion_raw_vsync = 1U;
    bool parallel_gs_completion_shutdown_timed_out = false;
    std::optional<Slus21050FemainParallelGsCompletionEvidence>
        last_femain_parallel_gs_completion_evidence{};
    std::size_t fast_forwarded_vblank_waits = 0U;
    std::size_t fast_forwarded_alarm_waits = 0U;
    std::size_t fast_forwarded_graphics_drain_waits = 0U;
    std::uint64_t emulated_ee_cycles = 0U;
    std::uint64_t gif_dmac_serviced_ee_cycles = 0U;
    std::uint64_t vif1_dmac_serviced_ee_cycles = 0U;
    struct RwaStagingSample {
        std::uint64_t step;
        std::uint32_t pointer;
        std::uint32_t size;
    };
    std::vector<RwaStagingSample> rwa_staging_samples{};
    std::uint32_t rwa_staging_last_pointer = 0U;
    std::uint64_t rwa_first_op05_step = 0U;
    bool rwa_first_op05_step_valid = false;
    guest_spu2_link.set_write_barrier(
        [&spu2_renderer, &emulated_ee_cycles]() { spu2_renderer.drain_through(emulated_ee_cycles); });
    kernel.set_rwa_voice_status([&spu2_renderer, &emulated_ee_cycles, &guest_spu2](
                                    const std::uint8_t core, const std::uint8_t voice) {
        spu2_renderer.drain_through(emulated_ee_cycles);
        auto& spu2 = *guest_spu2;
        // RECOVERY: the original keeps the two range checks as separate branches sharing one zero
        // result block; every spelling tried here is merged by ifcombine or duplicates the zero block.
        if (core > 1U || voice > 23U) {
            return guest::EeKernelHle::EeRwaVoiceStatus{};
        }
        const auto& spu_voice = spu2.cores[core].voices[voice];
        return guest::EeKernelHle::EeRwaVoiceStatus{
            .envelope = static_cast<std::uint16_t>(std::clamp(spu_voice.adsr.value, 0, 32767)),
            .current_address = spu_voice.next_a << 1U};
    });
    kernel.set_rwa_voice_volume_status([&guest_spu2](const std::uint8_t core, const std::uint8_t voice) {
        auto& spu2 = *guest_spu2;
        if (core > 1U) {
            return guest::EeKernelHle::EeRwaVoiceVolumeStatus{};
        }
        if (voice > 23U) {
            return guest::EeKernelHle::EeRwaVoiceVolumeStatus{};
        }
        const auto& spu_voice = spu2.cores[core].voices[voice];
        guest::EeKernelHle::EeRwaVoiceVolumeStatus status{};
        status.left = spu_voice.volume_left.value;
        status.right = spu_voice.volume_right.value;
        return status;
    });
    guest_spu2->set_fixed_capture_irq_observer([&kernel](const std::uint8_t core) {
        if (core == 0U) {
            kernel.advance_rwa_voice_volume_ramps();
        }
    });
    if (rwa_iop_shadow.has_value()) {
        rwa_iop_shadow->set_guest_cycle_provider([&emulated_ee_cycles]() { return emulated_ee_cycles; });
    }
    std::uint64_t next_vblank_start = 4915200U;
    const auto drain_parallel_gs_completions =
        [&parallel_gs_completion_reader, &next_parallel_gs_completion_raw_vsync,
         &femain_parallel_gs_completion_ledger,
         &last_femain_parallel_gs_completion_evidence](const std::optional<std::uint64_t> through_raw_vsync) {
        if (parallel_gs_completion_reader == nullptr) {
            return;
        }
        parallel_gs_completion_reader->poll();
        while (!through_raw_vsync.has_value() ||
               next_parallel_gs_completion_raw_vsync <= *through_raw_vsync) {
            auto completion = parallel_gs_completion_reader->consume_complete_for_raw_vsync(
                next_parallel_gs_completion_raw_vsync);
            if (!completion.has_value()) {
                break;
            }
            if (femain_parallel_gs_completion_ledger != nullptr) {
                femain_parallel_gs_completion_ledger->observe_completion(*completion);
                while (auto evidence = femain_parallel_gs_completion_ledger->take_completed()) {
                    last_femain_parallel_gs_completion_evidence = *evidence;
                }
            }
            ++next_parallel_gs_completion_raw_vsync;
        }
    };
    bool dmac_callback_in_flight = false;
    std::uint32_t freeze_last_spin_target = 0U;
    std::uint64_t freeze_spin_entries = 0U;
    std::vector<DmacCallbackTrace> dmac_callback_trace;
    std::array<RetailPcMilestone, 76> retail_pc_milestones{{
            {1248592U, "movie controller update"},
            {1248832U, "movie filename formatted"},
            {3298544U, "movie decoder start"},
            {1248896U, "movie decoder start returned"},
            {1248936U, "movie decoder accepted"},
            {3297572U, "movie decoder stop completed"},
            {4343188U, "movie owner finished query"},
            {4343248U, "movie owner cleanup request"},
            {1248368U, "movie controller cleanup"},
            {3297088U, "movie decoder destroy"},
            {1250432U, "movie initialization update"},
            {1251072U, "movie initialization final gate"},
            {1251120U, "movie next-flow lookup"},
            {1251208U, "movie next-flow installed"},
            {1251728U, "movie controller initialize"},
            {3202832U, "frontend subsystem init"},
            {3200960U, "frontend controller frame"},
            {3200448U, "frontend screen transition"},
            {1247744U, "frontend gate update"},
            {1248000U, "frontend gate activate"},
            {1257872U, "bootstrap state machine"},
            {1258632U, "bootstrap resource poll call"},
            {1258664U, "bootstrap resource ready"},
            {2667392U, "resource readiness poll"},
            {2237632U, "resource handle request"},
            {3690640U, "resource loader advance"},
            {3687296U, "resource loader begin"},
            {3697904U, "resource loader poll"},
            {3690804U, "resource loader ready return"},
            {2667448U, "resource loader call returned"},
            {2667840U, "resource readiness success return"},
            {2667876U, "resource readiness pending return"},
            {1258644U, "bootstrap resource poll returned"},
            {3266864U, "frontend movie mode"},
            {3264304U, "frontend movie frame"},
            {3264604U, "frontend movie start call"},
            {2173072U, "abstract file open"},
            {1914592U, "GTFS file open"},
            {1913472U, "GTFS file read"},
            {2837456U, "RWA stream request type zero"},
            {2837736U, "RWA stream request type one"},
            {2838036U, "RWA stream request type two"},
            {2838268U, "RWA stream request type three"},
            {2838464U, "RWA stream completion callback"},
            {2839232U, "RWA SPU transfer completion callback"},
            {2839192U, "RWA SPU transfer marked busy"},
            {2776572U, "EATrax child state query returned"},
            {2799292U, "EATrax middleware ready store"},
            {3686992U, "resource job pool allocate"},
            {3687000U, "resource job pool allocated"},
            {3687160U, "resource job pool exhausted"},
            {2815008U, "RWA stream base store"},
            {2561216U, "intro outer controller"},
            {2561408U, "intro controller update call"},
            {2544288U, "intro controller update"},
            {2544800U, "intro controller initialize"},
            {2541396U, "intro abstract file open call"},
            {2541336U, "intro async file read call"},
            {2540292U, "intro file completion poll"},
            {2545656U, "intro decoder start"},
            {4308568U, "DirtyElf load and relocate"},
            {4308520U, "DirtyElf start trampoline"},
            {4308528U, "DirtyElf update trampoline"},
            {4308880U, "DirtyElf unload"},
            {3809536U, "special frontend event handler"},
            {3450080U, "frontend flow event handler"},
            {3456048U, "frontend state event handler"},
            {3282816U, "special frontend updater install"},
            {3282576U, "special frontend updater frame"},
            {3452092U, "intro screen install path A"},
            {3456432U, "intro screen install path B"},
            {3487008U, "intro screen install"},
            {3485936U, "intro screen frame"},
            {3486044U, "intro prompt input poll"},
            {2281440U, "frontend controller edge poll"},
            {3486096U, "intro prompt accepted"},
    }};
    std::vector<RetailEventTrace> retail_event_trace;
    std::unordered_map<std::uint64_t, RetailCallSiteTrace> retail_call_sites;
    std::vector<RetailPhaseTransition> retail_phase_transitions;
    std::vector<RetailValueTransition> retail_flow_transitions;
    std::vector<RetailValueTransition> retail_movie_phase_transitions;
    std::vector<RetailValueTransition> retail_movie_state_transitions;
    std::vector<EATraxChildQueryTrace> eatrax_child_query_trace;
    std::vector<RwaDmaSubmissionTrace> rwa_dma_submission_trace;
    std::vector<RpcBindTrace> rpc_bind_trace;
    guest::EeStepResult stop{};
    guest::EeKernelCallResult kernel_stop{};
    bool outside_guest_memory = false;
    bool stopped_at_kernel_call = false;
    bool stopped_without_runnable_thread = false;
    bool stopped_by_host_window = false;
    bool stopped_at_native_gap = false;
    guest::GuestAddress native_gap_address = 0U;
    std::string native_gap_function_name;
    std::string native_gap_block_name;
    std::optional<std::uint32_t> native_gap_expected_word;
    std::optional<std::uint32_t> native_gap_observed_word;
    bool idle_vblank_probe_active = false;
    std::optional<VirtualDs2Trace> virtual_ds2_trace;
    FrontendGatePulseHandshake virtual_ds2_gate_handshake;
    bool virtual_ds2_release_after_update = false;
    bool virtual_ds2_title_pulse_in_flight = false;
    std::optional<std::uint32_t> virtual_ds2_active_channel;
    std::uint8_t virtual_ds2_release_value = 0U;
    std::optional<std::uint64_t> virtual_ds2_edge_poll_in_flight;
    std::optional<std::uint16_t> virtual_ds2_edge_poll_sample_in_flight;
    std::optional<HeadlessFrontendReplayTrace> headless_frontend_replay_trace;
    std::optional<std::uint64_t> headless_frontend_replay_update_in_flight;
    std::optional<std::uint64_t> headless_frontend_replay_edge_poll_in_flight;
    FrontendReplayInput headless_frontend_replay_input_in_flight = FrontendReplayInput::Neutral;
    const FrontendInputReplaySchedule kHeadlessFrontendReplaySchedule{
        .down_pulse_update = 30U,
        .down_pulse_updates = 1U,
        .up_pulse_update = 170U,
        .up_pulse_updates = 1U,
        .initial_cross_pulse_update = 70U,
        .repeated_cross_first_update = 110U,
        .repeated_cross_period = 30U,
        .cross_pulse_updates = 1U,
    };
    std::vector<HeadlessMovieEosTrace> headless_movie_eos_traces;
    std::optional<Slus21050FemainParkPcHistogram> femain_park_pcs;
    std::optional<Slus21050FemainParkPcHistogram> femain_alt_phase_pcs;
    std::uint32_t femain_alt_phase = 0U;
    if (const char* const femain_park_diag = std::getenv("TAKEDOWN_FEMAIN_PARK_DIAG");
    femain_park_diag != nullptr && femain_park_diag[0] != '\0' &&
    (femain_park_diag[0] != '0' || femain_park_diag[1] != '\0')) {
        femain_park_pcs.emplace();
        femain_alt_phase_pcs = Slus21050FemainParkPcHistogram{};
    }
    std::optional<std::uint64_t> post_intro_frontend_step;
    const auto read_bounded_guest_string = [&memory](const std::uint32_t address, const std::size_t maximum_length) -> std::optional<std::string> {
        std::string value;
        value.reserve(maximum_length);
        for (std::size_t index = 0U; index < maximum_length; ++index) {
            const std::uint32_t character_address = address + static_cast<std::uint32_t>(index);
            if (!memory.contains(character_address, 1U)) {
                return std::nullopt;
            }
            const auto byte = memory.read_u8(character_address);
            if (byte == 0U) {
                return value;
            }
            value.push_back(static_cast<char>(byte));
        }
        return std::nullopt;
    };
    const auto fold_retail_path = [](const std::string_view filename) {
        std::string folded;
        folded.reserve(filename.size());
        for (const char character : filename) {
            if (character >= 'A' && character <= 'Z') {
                folded.push_back(static_cast<char>(character + ('a' - 'A')));
            } else {
                folded.push_back(character == '/' ? '\\' : character);
            }
        }
        return folded;
    };
    // RECOVERY: the original inlines fold_retail_path into the three m2v lambdas below (while keeping the
    // out-of-line fold copy); calling it here is not inlined by the same compiler, so these three differ.
    const auto is_retail_m2v = [&fold_retail_path](const std::string_view filename) {
        if (filename.size() <= 4U) {
            return false;
        }
        const auto folded = fold_retail_path(filename);
        return folded.ends_with(".m2v") && (folded.starts_with("ovid\\") || folded.starts_with("tracks\\"));
    };
    const auto is_looping_frontend_background_m2v = [&fold_retail_path](const std::string_view filename) {
        return fold_retail_path(filename) == "tracks\\_femain.m2v";
    };
    const auto is_menu_background_m2v = [&fold_retail_path](const std::string_view filename) {
        return fold_retail_path(filename).starts_with("tracks\\");
    };
    // RECOVERY: in the original the channel 35..38 branch is cold (its three writes stay calls and the address range is not
    // proven); here it is hot and the writes are inlined. Source of that profile difference unknown.
    const auto seed_virtual_ds2_profile = [&memory]() {
        for (std::uint32_t channel = 0U; channel != 40U; ++channel) {
            const unsigned int descriptor = 0x676494U + channel * 16U;
            memory.write_u32_le(descriptor, 0U);
            memory.write_u32_le(descriptor + 4U, 0U);
            memory.write_u32_le(descriptor + 8U, 0U);
            memory.write_u32_le(descriptor + 12U, 0U);
            if (channel < 16U) {
                memory.write_u32_le(descriptor, 1U);
                memory.write_u32_le(descriptor + 4U, 1U);
                memory.write_u32_le(descriptor + 8U, channel >> 3U);
                memory.write_u32_le(descriptor + 12U, channel & 7U);
            } else if (channel < 32U) {
                memory.write_u32_le(descriptor, 1U);
                memory.write_u32_le(descriptor + 4U, 8U);
                memory.write_u32_le(descriptor + 8U, channel - 14U);
            } else if (channel >= 35U && channel < 39U) {
                memory.write_u32_le(descriptor, 1U);
                memory.write_u32_le(descriptor + 4U, 8U);
                memory.write_u32_le(descriptor + 8U, channel - 17U);
            }
        }
    };
    const auto write_virtual_ds2_sample = [&memory](const guest::RetailDs2DecodedSample& sample) {
        for (std::size_t byte = 0U; byte < 32U; ++byte) {
            memory.write_u8(0x4F52D8U + static_cast<std::uint32_t>(byte), sample[byte]);
        }
    };
    const auto write_virtual_ds2_neutral_sample = [&write_virtual_ds2_sample]() {
        write_virtual_ds2_sample(guest::make_neutral_retail_ds2_sample());
    };
    // RECOVERY: the arithmetic on `packed`/`mask` is narrowed to signed short here but to unsigned short (with a separate
    // `const uint16_t mask` SSA name) in the original; the exact source expression was not recovered.
    const auto write_virtual_ds2_channel = [&memory](const std::uint32_t channel, const std::uint8_t value) {
        if (channel >= 40U) {
            return false;
        }
        const unsigned int descriptor = 0x676494U + channel * 16U;
        const unsigned int enabled = memory.read_u32_le(descriptor);
        const unsigned int width = memory.read_u32_le(descriptor + 4U);
        const unsigned int byte_offset = memory.read_u32_le(descriptor + 8U);
        const unsigned int bit_offset = memory.read_u32_le(descriptor + 12U);
        if (enabled == 0U || (width != 1U && width != 8U) || bit_offset > 7U || byte_offset > 31U ||
            byte_offset + ((width + bit_offset + 7U) >> 3U) > 32U) {
            return false;
        }
        const unsigned int address = 0x4F52D8U + byte_offset;
        std::uint16_t packed = memory.read_u8(address);
        if (width + bit_offset > 8U) {
            packed |= memory.read_u8(address + 1U) << 8U;
        }
        const auto channel_mask = static_cast<std::uint16_t>((1U << width) - 1U);
        const auto mask = static_cast<std::uint16_t>(channel_mask << bit_offset);
        packed &= ~mask;
        packed |= (channel_mask & value) << bit_offset;
        memory.write_u8(address, static_cast<std::uint8_t>(packed));
        if (width + bit_offset > 8U) {
            memory.write_u8(address + 1U, static_cast<std::uint8_t>(packed >> 8U));
        }
        return true;
    };
    const auto make_virtual_ds2_host_sample = [](const platform::HostControllerState& input) {
        auto sample = guest::make_neutral_retail_ds2_sample();
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Select, input.select);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Start, input.start);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::DpadUp, input.dpad_up);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::DpadRight, input.dpad_right);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::DpadDown, input.dpad_down);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::DpadLeft, input.dpad_left);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::LeftTrigger, input.left_trigger);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::RightTrigger, input.right_trigger);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::LeftShoulder, input.left_shoulder);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::RightShoulder, input.right_shoulder);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Triangle, input.triangle);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Circle, input.circle);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Cross, input.cross);
        guest::set_retail_ds2_button(sample, guest::RetailDs2Button::Square, input.square);
        constexpr std::array<guest::RetailDs2Button, 14> pressure_buttons{
            guest::RetailDs2Button::DpadUp, guest::RetailDs2Button::DpadRight, guest::RetailDs2Button::DpadDown,
            guest::RetailDs2Button::DpadLeft, guest::RetailDs2Button::Start, guest::RetailDs2Button::Select,
            guest::RetailDs2Button::Cross, guest::RetailDs2Button::Circle, guest::RetailDs2Button::Square,
            guest::RetailDs2Button::Triangle, guest::RetailDs2Button::LeftShoulder, guest::RetailDs2Button::RightShoulder,
            guest::RetailDs2Button::LeftTrigger, guest::RetailDs2Button::RightTrigger};
        for (std::size_t i = 0U; i < 14U; ++i) {
            if (input.button_pressure[i] != 0U) {
                guest::set_retail_ds2_pressure(sample, pressure_buttons[i], input.button_pressure[i]);
            }
        }
        guest::set_retail_ds2_left_stick(sample, input.left_stick_x, input.left_stick_y);
        return sample;
    };
    const auto write_virtual_ds2_host_input = [&write_virtual_ds2_sample, &make_virtual_ds2_host_sample](const platform::HostControllerState& input) {
        write_virtual_ds2_sample(make_virtual_ds2_host_sample(input));
    };
    runtime::Slus21050Controllers native_controllers;
    bool online_mod_prompt_active = false;
    const auto host_controller_input = [&host_window, &parallel_gs_input_reader]() {
        platform::HostControllerState pad{};
        if (host_window != nullptr && parallel_gs_input_reader != nullptr && parallel_gs_input_reader->has_raw_input()) {
            host_window->set_external_input(parallel_gs_input_reader->raw_input());
        }
        if (host_window != nullptr) {
            pad = host_window->controller_state(0);
        }
        if (parallel_gs_input_reader != nullptr) {
            pad = platform::merge_host_controller_states(pad, parallel_gs_input_reader->controller_state());
        }
        return pad;
    };
    // RECOVERY: in the original, host_controller_input's `pad = merge_host_controller_states(pad, ...)` goes through a
    // temporary (tmp2 = merge(&retval, &tmp1); retval = tmp2); here GCC applies return-slot optimization directly.
    const auto current_host_controller_state = [&guest_input_replay, &online_mod_prompt_active, &host_controller_input]() -> platform::HostControllerState {
        if (guest_input_replay != nullptr) {
            return guest_input_replay->controller_state();
        }
        if (online_mod_prompt_active) {
            return platform::HostControllerState{};
        }
        return host_controller_input();
    };
    const auto escape_key_held = [&host_window, &parallel_gs_input_reader]() {
        constexpr std::size_t kEscapeScancode = 41U; // SDL_SCANCODE_ESCAPE
        if (parallel_gs_input_reader != nullptr && parallel_gs_input_reader->has_raw_input() &&
            parallel_gs_input_reader->raw_input().keys.test(kEscapeScancode)) {
            return true;
        }
        return host_window != nullptr && host_window->raw_input_state().keys.test(kEscapeScancode);
    };
    // The imported menu fonts and artwork (cache/ui of the install).
    // The install: the folder above the game's game/SLUS_210.50.
    const std::filesystem::path install_directory = std::filesystem::path(argv[2]).parent_path().parent_path();
    const auto settings_ui_directory = [&mods_manager, &install_directory]() -> const std::string& {
        static const std::string directory = [&mods_manager, &install_directory]() {
            std::vector<std::filesystem::path> candidates{install_directory / "cache" / "ui",
                                                          mods_manager.directory().parent_path() / "cache" / "ui",
                                                          config::default_settings_path().parent_path() / "cache" / "ui"};
            for (const auto& candidate : candidates) {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate / "GlobalFont.tdfont", error)) {
                    return std::filesystem::absolute(candidate, error).string();
                }
            }
            return std::filesystem::absolute(candidates.front()).string();
        }();
        return directory;
    };
    bool guest_work_measurement_start_input_pending = false;
    std::uint64_t guest_work_measurement_start_edges_observed = 0U;
    std::size_t next_host_window_step = 0U;
    std::size_t host_window_loop_iterations = 0U;
    presentation::PresentationPacer host_window_pacer(loaded_settings.settings.graphics.presentation_fps_limit);
    bool pc_options_window_vsync = loaded_settings.settings.graphics.vsync;
    // RECOVERY: always_inline below forces the inline the original got from IPA at both of its call sites; with a plain
    // lambda the harness leaves it out of line. The original also inlines ParallelGsHostPresentation/std::function
    // destructors in the cleanup paths, which differ here.
    const auto apply_pc_options = [&pc_options, &game_graphics_options_available, &game_graphics_options,
                                   &game_scenery_detail_available, &game_scenery_detail, &camera_fov,
                                   &parallel_gs_live_fifo_path, &host_window_pacer, &host_window,
                                   &pc_options_window_vsync, &parallel_gs_stream_writer, &memory]() {
        const config::GraphicsSettings& graphics = pc_options.graphics();
        if (game_graphics_options_available) {
            game_graphics_options.configure(pc_options.game_options());
        }
        if (game_scenery_detail_available) {
            game_scenery_detail.configure(
                runtime::scenery_detail_policy(graphics.render_distance, graphics.model_quality));
        }
        camera_fov.configure(static_cast<float>(graphics.field_of_view), graphics.fov_correction,
                             graphics.panini && parallel_gs_live_fifo_path.has_value(),
                             graphics.aspect_ratio == config::AspectRatio::Widescreen ? 16.0F / 9.0F : 4.0F / 3.0F);
        const auto presentation_limit = presentation::effective_frame_rate_limit(graphics, false);
        if (host_window_pacer.fps_limit() != presentation_limit) {
            host_window_pacer = presentation::PresentationPacer(presentation_limit);
        }
        if (host_window != nullptr) {
            host_window->set_display_aspect_ratio(
                graphics.aspect_ratio == config::AspectRatio::Widescreen ? 16.0 / 9.0 : 4.0 / 3.0);
            if (pc_options_window_vsync != graphics.vsync) {
                pc_options_window_vsync = graphics.vsync;
                if (!host_window->set_vsync(graphics.vsync)) {
                    std::cerr << "PC options: the window rejected the VSync change\n";
                }
            }
        }
        if (parallel_gs_stream_writer != nullptr && parallel_gs_live_fifo_path.has_value()) {
            memory.run_in_graphics_order(
                [writer = parallel_gs_stream_writer.get(), presentation = pc_options.host_presentation()]() {
                writer->set_host_presentation(presentation);
            });
        }
    };
    apply_pc_options();
    // One retail poll of the SETTINGS page: while it is the game's page it
    // takes the input (controller state and the mouse) and the game page sees
    // none (returns true). Runs at most once per poll.
    std::uint64_t settings_page_last_poll = ~std::uint64_t{0};
    const auto settings_page_poll = [&](const std::uint64_t poll, const platform::HostControllerState& pad,
                                        const std::optional<runtime::MenuMouseInput>& mouse) -> bool {
        if (!pc_options.enabled() || !native_pc_options.installed()) {
            return false;
        }
        if (poll == settings_page_last_poll) {
            return settings_page.active();
        }
        settings_page_last_poll = poll;
        SettingsPageInput page_input{.pad = pad};
        if (mouse.has_value()) {
            if (mouse->inside) {
                page_input.pointer = std::array<float, 2>{mouse->x, mouse->y};
            }
            page_input.mouse_buttons = mouse->buttons;
            page_input.wheel = mouse->wheel;
        }
        const SettingsPageControls page_controls{controls_session.rows(), controls_session.capturing(),
                                                 controls_session.capture_prompt(), controls_session.notice()};
        config::GraphicsSettings edited = pc_options.graphics();
        const auto page_result = settings_page.update(memory, native_pc_options, edited, page_controls, page_input);
        if (page_result.graphics_changed) {
            const auto update = pc_options.apply_graphics(edited);
            if (update.save_error.has_value()) {
                std::cerr << "Settings: " << *update.save_error << '\n';
            }
            apply_pc_options();
        }
        if (page_result.controls_action.has_value()) {
            controls_session.activate(page_result.controls_action->first, page_result.controls_action->second,
                                      host_window != nullptr ? host_window->raw_input_state() : platform::RawInputState{});
        }
        return page_result.active;
    };
    // One retail poll of the controller layout question: opens it once over the
    // first front-end menu, feeds it and takes the choice. True while it is open (the
    // game then sees no input).
    const auto control_style_poll = [&](const std::uint64_t poll, const platform::HostControllerState& pad,
                                        const std::optional<runtime::MenuMouseInput>& mouse) -> bool {
        // Over the front-end menus: the menu video loops behind them (the boot
        // prompts come before the intro movies); without host movies, after
        // the first 20 seconds.
        const bool front_end = host_movie_player.active() ? host_movie_player.looping() : poll > 1200U;
        if (control_style_prompt_pending && guest_input_replay == nullptr && memory.read_u32_le(0x01e90424U) == 5U &&
            front_end && !settings_page.active() && !runtime::read_menu_mouse_screen(memory).lists.empty()) {
            control_style_prompt_pending = false;
            control_style_prompt.open();
            game_bindings_menu.reset();
        }
        if (!control_style_prompt.is_open()) {
            if (control_style_blocks_input) {
                const bool held = pad.dpad_up || pad.dpad_right || pad.dpad_down || pad.dpad_left || pad.start ||
                                  pad.select || pad.cross || pad.circle || pad.square || pad.triangle ||
                                  pad.left_shoulder || pad.right_shoulder || pad.left_trigger || pad.right_trigger ||
                                  (mouse.has_value() && mouse->buttons != 0U);
                control_style_blocks_input = held;
            }
            return control_style_blocks_input;
        }
        control_style_blocks_input = true;
        if (poll == control_style_prompt_last_poll) {
            return true;
        }
        control_style_prompt_last_poll = poll;
        ui::ControlStylePrompt::Input input;
        input.left = pad.dpad_left || pad.left_shoulder || pad.left_stick_x < -16384;
        input.right = pad.dpad_right || pad.right_shoulder || pad.left_stick_x > 16384;
        input.confirm = pad.cross || pad.start;
        const bool click_down = mouse.has_value() && (mouse->buttons & 1U) != 0U;
        if (mouse.has_value() && mouse->inside) {
            input.pointer = std::array<float, 2>{mouse->x * 640.0f, mouse->y * 448.0f};
            input.click = click_down && !control_style_click_held;
        }
        control_style_click_held = click_down;
        if (const auto chosen = control_style_prompt.update(input).chosen; chosen.has_value()) {
            controls_session.choose_style(*chosen);
            game_bindings_menu.reset();
            std::cerr << "Controls: layout chosen: " << ui::control_style_name(*chosen) << '\n';
        }
        return true;
    };
    const auto variable_frame_limit_cycles = [&variable_frame_rate, &host_movie_player, &pc_options,
                                              &variable_frame_floor_cycles]() -> std::uint64_t {
        const auto limit = presentation::effective_frame_rate_limit(
            pc_options.graphics(), variable_frame_rate->planner().retail_rate() <= 59U || host_movie_player.active());
        if (!limit.has_value()) {
            return variable_frame_floor_cycles;
        }
        return std::max<std::uint64_t>(variable_frame_floor_cycles, 294912000U / static_cast<std::uint64_t>(*limit));
    };
    bool host_frame_step_enabled = false;
    bool host_frame_step_paused = false;
    bool host_frame_step_advancing = false;
    std::optional<std::uint64_t> host_frame_step_target_vblank;
    std::vector<std::uint8_t> host_window_rgba;
    std::vector<std::uint8_t> host_window_overlay_rgba;
    std::vector<std::uint8_t> host_window_last_presented_rgba;
    std::optional<guest::GsLocalMemory> host_window_vram;
    bool host_window_texture_contains_overlay = false;
    std::deque<HostWindowPendingSnapshot> host_window_pending_snapshots;
    bool host_window_has_completed_snapshot = false;
    std::size_t host_window_completed_snapshot_vblank = 0U;
    bool host_window_has_consumed_scanout = false;
    std::uint64_t host_window_consumed_snapshot_serial = 0U;
    std::size_t host_window_consumed_snapshot_vblank = 0U;
    std::uint64_t host_window_vblank_snapshot_requests = 0U;
    std::uint64_t host_window_saturated_vblanks = 0U;
    std::uint64_t host_window_final_snapshot_requests = 0U;
    std::uint64_t host_window_completed_snapshots = 0U;
    std::uint64_t host_window_presented_scanouts = 0U;
    std::uint64_t host_window_present_calls = 0U;
    std::size_t gs_semantic_checkpoint_next_index = 1U;
    std::size_t gs_semantic_checkpoints_written = 0U;
    std::optional<std::filesystem::path> gs_semantic_last_checkpoint_path;
    bool host_window_capture_requested = false;
    std::optional<std::uint64_t> host_window_capture_required_vblank;
    bool host_window_capture_boundary_paused = false;
    std::string host_window_capture_notice;
    std::chrono::steady_clock::time_point host_window_capture_notice_until = std::chrono::steady_clock::time_point::min();
    std::uint32_t host_window_surface_width = 0U;
    std::uint32_t host_window_surface_height = 0U;
    std::uint32_t host_window_last_presented_width = 0U;
    std::uint32_t host_window_last_presented_height = 0U;
    presentation::GsScanoutPlan host_window_scanout_plan{};
    presentation::GsScanoutPlanStatus host_window_scanout_status = host_window_scanout_plan.status;
    presentation::GsScanoutRenderStatus host_window_render_status = presentation::GsScanoutRenderStatus::InvalidPlan;
    // RECOVERY: the original keeps `root` in memory and calls the path(const char* const&) constructor out of line
    // (__ct_comp.constprop); here GCC inlines it. kernel_stop is also initialised differently (clobber vs field stores).
    if (const char* const root = std::getenv("TAKEDOWN_SAVE_DATA_ROOT"); root != nullptr && root[0] != '\0') {
        save_data_root_override = std::filesystem::path(root);
    }
    const std::filesystem::path save_data_root =
        save_data_root_override.has_value()
            ? *save_data_root_override
            : loaded_settings.settings.save_data_directory.value_or(save::default_save_data_root());
    const bool save_data_enabled = [&host_window, &guest_input_timeline_enabled,
                                    &guest_work_measurement_step_budget]() {
        const char* const flag = std::getenv("TAKEDOWN_SAVE_DATA");
        if (flag != nullptr && std::string_view(flag) == "force") {
            return true;
        }
        if (host_window == nullptr || guest_input_timeline_enabled || guest_work_measurement_step_budget.has_value()) {
            return false;
        }
        return flag == nullptr || std::string_view(flag) != "0";
    }();
    std::shared_ptr<save::SaveCardDevice> save_card_device;
    ui::SaveSlotMenu save_slot_menu;
    ui::DeveloperUnlockSequence developer_unlock;
    const ui::HostPrompt developer_unlock_notice{
        "developer-car-unlock",
        "ALL CARS UNLOCKED",
        [](const std::initializer_list<const char*> lines) {
            return std::vector<std::string>(lines.begin(), lines.end());
        }({"Developer car unlock is active until you close the game.",
           "SAVING IS DISABLED. Your existing saves are protected.",
           "Restart the game to play and save normally."}),
        {{"ok", "OK", ui::PromptActionRole::Cancel}}};
    std::optional<takedown::ui::HostPrompt> developer_notice;
    takedown::ui::HostPromptEdgeTracker developer_notice_edges;
    bool developer_notice_needs_release = true;
    takedown::ui::HostPromptEdgeTracker save_slot_menu_edges;
    bool save_slot_menu_select_previous = false;
    bool save_slot_menu_select_chorded = false;
    bool save_slot_menu_opened_once = false;
    std::optional<std::chrono::steady_clock::time_point> save_slot_peer_since;
    std::optional<takedown::ui::HostPrompt> save_slot_peer_entry;
    bool save_slot_peer_persistent = false;
    // RECOVERY: one more lambda()#N closure was defined (and fully inlined) before this point in the original;
    // this empty lambda keeps the numbering of the later ones aligned.
    [[maybe_unused]] const auto recovery_lambda_slot_4 = []() {};
    const auto build_save_slot_menu_context = [&]() {
        takedown::ui::SaveSlotMenuContext context;
        context.slots = takedown::save::list_profile_slots(save_data_root);
        context.active_slot = save_card_device != nullptr
                                  ? save_card_device->attached_slot().value_or(
                                        takedown::save::read_active_profile_slot(save_data_root))
                                  : takedown::save::read_active_profile_slot(save_data_root);
        std::error_code source_error;
        for (const takedown::save::ImportSource& source :
        takedown::save::detect_import_sources(save_data_root, source_error)) {
            std::error_code summary_error;
            auto summary = takedown::save::summarize_import_source(source, {}, summary_error);
            if (summary.has_value()) {
                context.sources.push_back(std::move(*summary));
            }
        }
        return context;
    };
    if (save_data_enabled) {
        std::error_code save_error;
        if (!save_data_root_override.has_value() && !loaded_settings.settings.save_data_directory.has_value()) {
            const bool migrated = takedown::save::migrate_save_data_root(takedown::save::legacy_save_data_root(),
                                                                         save_data_root, save_error);
            if (save_error) {
                throw std::runtime_error("Could not migrate existing saves: " + save_error.message());
            }
            if (migrated) {
                std::cerr << "Save data: copied existing profiles into saves/; original data retained\n";
            }
        }
        std::filesystem::create_directories(save_data_root, save_error);
        save_card_device = std::make_shared<takedown::save::SaveCardDevice>(save_data_root);
        const std::size_t active_slot = takedown::save::read_active_profile_slot(save_data_root);
        if (save_card_device->attach_slot(active_slot, save_error)) {
            std::cerr << "Save data: profile slot " << active_slot << " in " << save_data_root.string()
                      << " (Select in the main menus opens SAVE DATA)\n";
        } else {
            std::cerr << "Save data: profile slot " << active_slot << " could not be loaded ("
                      << save_error.message() << "); the game will find no save data\n";
        }
        kernel.set_save_card_device(save_card_device);
        std::error_code source_error;
        const bool sources_found = !takedown::save::detect_import_sources(save_data_root, source_error).empty();
        takedown::ui::HostPrompt peer;
        peer.id = "save-data-entry";
        peer.title = "SAVE DATA";
        peer.body_lines.push_back(sources_found ? "PCSX2 save data was found - press Select to import it."
                                                : "Press Select for profile slots and save data imports.");
        save_slot_peer_entry = std::move(peer);
        save_slot_peer_persistent = sources_found;
    }
    takedown::ui::ModSyncSession mod_sync(std::filesystem::current_path());
    takedown::ui::HostPromptEdgeTracker mod_sync_edges;
    std::optional<takedown::ui::ModSyncSession::LobbyJoinRequest> pending_mod_join;
    takedown::net::MatchmakingConfig online_config;
    online_config.host = "play.uwucammie.com";
    online_config.game_root = std::filesystem::path(argv[2]).parent_path();
    online_config.mods_directory = *takedown::content::select_mods_directory(
        online_config.game_root, mods_directory_override.has_value() ? mods_directory_override : loaded_settings.settings.mods_directory, true);
    online_config.mods_enabled = loaded_settings.settings.mods_enabled && !mods_disabled_option;
    online_config.applied_mods = std::make_shared<const takedown::content::ModManager>(mods_manager);
    online_config.mod_sync_required = [&](const takedown::content::JsonValue& required) {
        pending_mod_join.reset();
        native_pc_options.cancel_lobby_navigation();
        if (const auto error = mod_sync.offer(required, &mods_manager); error.has_value()) {
            std::cerr << "Online mods: " << *error << '\n';
        }
        mod_sync_edges.reset();
        native_pc_options.service_lobby_prompt(developer_notice.has_value() ? &*developer_notice
                                               : mod_sync.prompt_open()     ? &mod_sync.prompt()
                                                                            : nullptr);
        online_mod_prompt_active = developer_notice.has_value() || mod_sync.prompt_open() ||
                                   native_pc_options.lobby_prompt_open();
    };
    online_config.player_name =
        takedown::save::summarize_profile_slot(save_data_root, takedown::save::read_active_profile_slot(save_data_root))
            .profile_name.value_or("Native Driver");
    if (const char* host = std::getenv("TAKEDOWN_MATCHMAKING_HOST")) {
        online_config.host = host;
    }
    if (const char* port = std::getenv("TAKEDOWN_MATCHMAKING_PORT")) {
        online_config.port =
            static_cast<std::uint16_t>(parse_bounded_count(port, "TAKEDOWN_MATCHMAKING_PORT", 65535U));
    }
    if (const char* name = std::getenv("TAKEDOWN_ONLINE_NAME")) {
        online_config.player_name = name;
    }
    online_config.browser_signin = online_config.host == "play.uwucammie.com";
    takedown::runtime::Slus21050Online native_online(std::move(online_config));
    native_online.set_mod_sync_selection_resolver(
        [&added_cars](takedown::guest::EeMemory& selection_memory, std::uint64_t vehicle_key, std::uint32_t paint)
            -> std::optional<takedown::runtime::Slus21050Online::ModSyncSelection> {
        const auto selection = added_cars.resolve_online_selection(selection_memory, vehicle_key, paint);
        if (!selection.has_value()) {
            return std::nullopt;
        }
        added_cars.apply_online_selection(selection_memory, *selection);
        return takedown::runtime::Slus21050Online::ModSyncSelection{selection->vehicle_key, selection->paint};
    });
    if (native_function_revision_matched && !native_online.register_functions(native_function_dispatcher, memory)) {
        throw std::runtime_error("Native online API revision admission failed");
    }
    const auto save_slot_peer_visible = [&](const bool in_frontend_menu_flow) {
        if (!save_slot_peer_entry.has_value() || !in_frontend_menu_flow || save_slot_menu.is_open() ||
            save_slot_menu_opened_once) {
            return false;
        }
        if (save_slot_peer_persistent) {
            return true;
        }
        return save_slot_peer_since.has_value() &&
               std::chrono::steady_clock::now() - *save_slot_peer_since < std::chrono::seconds(10);
    };

    const auto perform_save_slot_menu_command = [&](const takedown::ui::SaveSlotMenuCommand& command) {
        using takedown::ui::SaveSlotMenuCommandKind;
        if (developer_unlock.active() && (command.kind == SaveSlotMenuCommandKind::activate_slot ||
                                          command.kind == SaveSlotMenuCommandKind::import_source)) {
            save_slot_menu.close();
            developer_notice = developer_unlock_notice;
            developer_notice_edges.reset();
            developer_notice_needs_release = true;
            return;
        }
        if (command.kind == SaveSlotMenuCommandKind::activate_slot) {
            std::error_code slot_error;
            const bool attached = save_card_device->attach_slot(command.slot, slot_error);
            std::vector<std::string> lines;
            if (attached) {
                std::error_code write_error;
                takedown::save::write_active_profile_slot(save_data_root, command.slot, write_error);
                lines.push_back("Profile slot " + std::to_string(command.slot) + " is now in use.");
                lines.push_back("The game picks it up at its next save data check.");
            } else {
                lines.push_back("Profile slot " + std::to_string(command.slot) + " could not be loaded.");
            }
            save_slot_menu.show_result(attached ? "Profile slot changed" : "Profile slot not changed",
                                       std::move(lines), build_save_slot_menu_context());
        } else if (command.kind == SaveSlotMenuCommandKind::import_source) {
            const auto& sources = save_slot_menu.context().sources;
            if (command.source_index >= sources.size()) {
                return;
            }
            const takedown::save::ImportSource source = sources[command.source_index].source;
            const bool was_attached = save_card_device->attached_slot() == command.slot;
            if (was_attached) {
                save_card_device->detach();
            }
            std::error_code import_error;
            std::filesystem::path backup_path;
            const bool imported = takedown::save::import_into_profile_slot(
                save_data_root, source, command.slot,
                command.replace_occupied ? takedown::save::SlotOverwrite::backup_and_replace
                                         : takedown::save::SlotOverwrite::refuse,
                import_error, &backup_path);
            if (was_attached) {
                std::error_code attach_error;
                save_card_device->attach_slot(command.slot, attach_error);
            }
            std::vector<std::string> lines;
            if (imported) {
                lines.push_back("The save data was imported into profile slot " + std::to_string(command.slot) + ".");
                if (!backup_path.empty()) {
                    lines.push_back("The previous save data was kept in " + backup_path.filename().string() + ".");
                }
            } else {
                lines.push_back("The save data could not be imported.");
                std::cerr << "Save data: import of " << source.path.string() << " into profile slot "
                          << command.slot << " failed: " << import_error.message() << '\n';
            }
            save_slot_menu.show_result(imported ? "Save data imported" : "Save data import failed",
                                       std::move(lines), build_save_slot_menu_context());
        }
    };
    const auto host_steady_now_nanoseconds = []() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    std::uint64_t host_window_applied_transfers = 0U;
    std::uint64_t host_window_rejected_transfers = 0U;
    guest::GsLiveRendererStatistics gs_renderer_statistics{};
    presentation::GsRendererWorkerQueueStatistics gs_renderer_queue_statistics{};
    presentation::HostFpsTracker host_window_guest_fps;
    presentation::HostFpsTracker host_window_visual_fps;
    presentation::HostFpsTracker host_window_present_fps;
    const auto wait_for_host_time = [&](const std::uint64_t deadline_nanoseconds, const platform::HostWaitSite site) {
        const std::uint64_t started = host_steady_now_nanoseconds();
        if (started >= deadline_nanoseconds) {
            return;
        }
        const std::uint64_t target = std::min(deadline_nanoseconds, started + 250000000U);
        const std::uint64_t remaining = target - started;
        if (remaining > 1500000U) {
            platform::precise_sleep(std::chrono::nanoseconds(remaining - 1500000U));
        }
        while (host_steady_now_nanoseconds() < target) {
            std::this_thread::yield();
        }
        const std::uint64_t waited = host_steady_now_nanoseconds() - started;
        variable_frame_host_wait_nanoseconds += waited;
        platform::record_host_wait(site, waited);
    };
    std::optional<app::GuestWorkHostSnapshot> guest_work_host_start{};
    std::optional<app::GuestWorkHostSnapshot> guest_work_host_end{};
    const auto snapshot_guest_work_measurement = [&]() {
        app::GuestWorkHostSnapshot host{
            platform::capture_host_cpu_snapshot(),
            audio_master_blocked_nanoseconds,
            parallel_gs_stream_writer != nullptr ? parallel_gs_stream_writer->statistics().producer_wait_nanoseconds : 0U,
            join_pump_idle_summary_instructions,
            runtime::slus_21050::native_strip_prepared_jobs(),
            memory.vu1_activity().native_strip_submission_jobs,
            memory.vu1_activity().native_strip_execution_jobs,
            memory.vu1_activity().native_strip_submission_packets,
            memory.vu1_activity().native_strip_submission_quadwords};
        if (native_model_live != nullptr) {
            host.model = native_model_live->statistics();
            host.model_producer = native_model_live->producer_statistics();
            host.model_reuse = native_model_live->geometry_reuse_statistics();
            host.model_workers = native_model_live->geometry_statistics();
        }
        const auto& vu1_stats = memory.vu1_program_cache().statistics();
        host.vu1_aot_pairs = vu1_stats.aot_pairs;
        host.vu1_aot_launches = vu1_stats.aot_launches;
        host.vu1_aot_completed = vu1_stats.aot_completed;
        if (!guest_work_host_start.has_value()) {
            guest_work_host_start = host;
        } else {
            guest_work_host_end = host;
        }
        return runtime::GuestWorkMeasurementSnapshot{committed_steps, emulated_ee_cycles, raised_vblank_starts,
                                                     spu2_renderer.samples_rendered(), spu2_renderer.fnv1a64(),
                                                     host_steady_now_nanoseconds()};
    };
    const auto snapshot_femain_provenance_boundary = [&](const runtime::GuestWorkMeasurementSnapshot& measurement) {
        return runtime::Slus21050FemainProvenanceBoundary{measurement, state.pc(), kernel.current_thread_id(),
                                                           kernel.guest_callback_active(),
                                                           kernel.guest_callback_identity()};
    };
    const auto host_movie_media_elapsed_nanoseconds = [&]() -> std::uint64_t {
        if (!host_movie_clock_start_nanoseconds.has_value()) {
            return 0U;
        }
        if (guest_input_timeline_enabled) {
            if (!host_movie_clock_anchor_samples.has_value()) {
                return 0U;
            }
            const auto rendered_samples = spu2_renderer.samples_rendered();
            if (*host_movie_clock_anchor_samples >= rendered_samples) {
                return 0U;
            }
            return (rendered_samples - *host_movie_clock_anchor_samples) * 1000000000U / 48000U;
        }
        const std::uint64_t now = host_steady_now_nanoseconds();
        const auto session_elapsed = now >= *host_movie_clock_start_nanoseconds ? now - *host_movie_clock_start_nanoseconds : 0U;
        auto paused = host_movie_clock_paused_nanoseconds;
        if (host_movie_clock_pause_start_nanoseconds.has_value() && now >= *host_movie_clock_pause_start_nanoseconds) {
            const auto active_pause = now - *host_movie_clock_pause_start_nanoseconds;
            paused = active_pause > std::numeric_limits<std::uint64_t>::max() - paused
                         ? std::numeric_limits<std::uint64_t>::max()
                         : paused + active_pause;
        }
        const auto wall_elapsed = session_elapsed > paused ? session_elapsed - paused : 0U;
        if (audio_master_clock && host_movie_clock_anchor_samples.has_value()) {
            const auto rendered_samples = spu2_renderer.samples_rendered();
            if (rendered_samples <= *host_movie_clock_anchor_samples) {
                return 0U;
            }
            const auto audio_elapsed = (rendered_samples - *host_movie_clock_anchor_samples) * 1000000000U / 48000U;
            return std::min(audio_elapsed, wall_elapsed);
        }
        return wall_elapsed;
    };
    {
        const std::uint64_t host_fps_session_start = host_steady_now_nanoseconds();
        host_window_guest_fps.start_session(host_fps_session_start);
        host_window_visual_fps.start_session(host_fps_session_start);
        host_window_present_fps.start_session(host_fps_session_start);
    }
    const auto evaluate_display_policy = [&]() {
        const auto& gs_crt = kernel.gs_crt_state();
        return presentation::evaluate_gs_display_policy(
            memory.gs_privileged_display().state(),
            presentation::GsCrtcHint{gs_crt.configured, gs_crt.interlace != 0, gs_crt.field_mode != 0});
    };
    const auto host_window_pump_scale = [&]() -> std::uint64_t {
        return save_card_device != nullptr && !kernel.save_card_present() ? 1U : 16U;
    };
    // RECOVERY: present_host_window (lambda(bool)#2) is structurally complete but its emitted
    // function is not GIMPLE-identical in the segment harness. Remaining residue, believed to be
    // harness/inlining-context artifacts rather than source differences:
    //  - std::shared_ptr _M_release speculative devirtualization/outlining (.part.0 split) differs;
    //    depends on which _Sp_counted_ptr_inplace types and ModManager dtor linkage exist in the TU.
    //  - read_u32_le is not inlined at two sites in the original (IPA-VRP / caller-count effect).
    //  - publish_capture_notice is left out of line at the "no renderable scanout" and "pending"
    //    notices in the original (inline budget), inlined in the rebuild.
    //  - path(".ppm"/".gsvram") constructors, stem/filename string composition, put-vs-operator<<
    //    and emplace_back inlining differ (early-inliner budget).
    //  - the original names the SaveSlotMenu::apply result `command`; the rebuild shows it as a
    //    CLOBBER(bob) temp before the call (naming only).
    const auto present_host_window = [&](const bool force_present) {
        if (native_pc_options.consume_quit_request()) {
            std::cout << "Main menu: QUIT GAME chosen, closing the game\n" << std::flush;
            return false;
        }
        if (host_window == nullptr) {
            return true;
        }
        if (parallel_gs_input_reader != nullptr) {
            parallel_gs_input_reader->poll();
            if (parallel_gs_input_reader->consume_close_request()) {
                return false;
            }
            if (guest_work_measurement_window.has_value() &&
                parallel_gs_input_reader->consume_guest_work_measurement_start_request()) {
                ++guest_work_measurement_start_edges_observed;
                const auto arm_result = guest_work_measurement_window->arm();
                if (arm_result == runtime::GuestWorkMeasurementArmResult::Armed) {
                    guest_work_measurement_start_input_pending = true;
                    std::cout << "Guest-work measurement: B3IN bit 20 arm accepted; "
                              << "starting at next EE instruction boundary\n"
                              << std::flush;
                } else {
                    std::cerr << "Guest-work measurement: B3IN bit 20 arm rejected; window already armed, started, completed, or rejected\n";
                }
            }
        }
        if (!host_window->pump_events()) {
            return false;
        }
        const bool text_entry_focused = native_online.text_entry_focused(memory);
        host_window->set_keyboard_suppressed(text_entry_focused);
        const bool developer_menu = native_function_revision_matched && native_car_select_active &&
                                    !text_entry_focused && !online_mod_prompt_active &&
                                    !save_slot_menu.is_open() &&
                                    runtime::slus_21050_developer_unlock_menu(memory);
        if (!developer_menu) {
            developer_unlock.leave_main_menu();
        }
        const auto handle_text = [&](const platform::TextInputEvent& event) {
            if (developer_unlock.consume(event, developer_menu)) {
                if (save_card_device != nullptr) {
                    save_card_device->disable_persistence_for_session();
                }
                added_cars.enable_developer_unlock();
                native_car_select.refresh(memory, mods_manager, true);
                developer_notice = developer_unlock_notice;
                developer_notice_edges.reset();
                developer_notice_needs_release = true;
                save_slot_menu_opened_once = true;
                std::cerr << "Developer car unlock: active for this session; save persistence disabled\n";
            }
            if (!online_mod_prompt_active) {
                native_online.handle_text_input(event, memory);
            }
        };
        for (const auto& event : host_window->consume_text_input()) {
            handle_text(event);
        }
        if (parallel_gs_input_reader != nullptr) {
            for (const auto& event : parallel_gs_input_reader->consume_text_input()) {
                handle_text(event);
            }
        }
        const platform::HostControllerState prompt_input = guest_input_replay != nullptr
                                                               ? guest_input_replay->controller_state()
                                                               : host_controller_input();
        if (developer_notice.has_value()) {
            mod_sync_edges.reset();
            const auto input = developer_notice_edges.update(prompt_input.dpad_up, prompt_input.dpad_down,
                                                             prompt_input.cross, prompt_input.circle);
            if (developer_notice_needs_release) {
                developer_notice_needs_release = prompt_input.dpad_up || prompt_input.dpad_down ||
                                                 prompt_input.cross || prompt_input.circle;
            } else if (native_pc_options.lobby_prompt_open() &&
                       ui::apply_prompt_input(*developer_notice, input).has_value()) {
                developer_notice.reset();
            }
        } else if (mod_sync.prompt_open()) {
            const auto input = mod_sync_edges.update(prompt_input.dpad_up, prompt_input.dpad_down,
                                                     prompt_input.cross, prompt_input.circle);
            static_cast<void>(mod_sync.apply(input));
        }
        static_cast<void>(mod_sync.poll());
        if (mod_sync.consume_mods_page_request()) {
            if (const auto frontend = native_online.mod_sync_frontend(); frontend.has_value()) {
                native_pc_options.request_mods_page(frontend->screen, frontend->page);
            } else {
                mod_sync.abort("The original lobby page is no longer available. Please select the lobby again.");
            }
        }
        if (!mod_sync.pending() && !pending_mod_join.has_value()) {
            native_pc_options.cancel_lobby_navigation();
        }
        if (native_online.consume_mod_sync_prepared() && pending_mod_join.has_value()) {
            native_pc_options.request_lobby_return();
        }
        if (native_pc_options.consume_lobby_returned() && pending_mod_join.has_value()) {
            const auto target = std::exchange(pending_mod_join, std::nullopt);
            const auto error = native_online.request_mod_sync_rejoin(target->room, target->mods);
            if (error.has_value()) {
                mod_sync.abort(*error);
            }
        }
        if (const auto error = native_online.consume_mod_sync_error(); error.has_value()) {
            pending_mod_join.reset();
            native_pc_options.cancel_lobby_navigation();
            mod_sync.abort(*error);
        }
        native_pc_options.service_lobby_prompt(developer_notice.has_value() ? &*developer_notice
                                               : mod_sync.prompt_open()     ? &mod_sync.prompt()
                                                                            : nullptr);
        online_mod_prompt_active =
            native_pc_options.lobby_input_blocked(prompt_input.dpad_up || prompt_input.dpad_down ||
                                                  prompt_input.cross || prompt_input.circle) ||
            pending_mod_join.has_value();
        if (const auto error = native_pc_options.consume_lobby_error(); error.has_value()) {
            developer_notice.reset();
            pending_mod_join.reset();
            mod_sync.abort(*error);
            native_pc_options.cancel_lobby_navigation();
            native_mods_page_state.notice = *error;
            ++native_mods_page_state.notice_serial;
            std::cerr << "Online mods: " << *error << '\n';
        }
        const bool in_frontend_menu_flow = memory.read_u32_le(0x01E90424U) == 5U;
        if (save_slot_menu.is_open()) {
            const auto prompt_state = current_host_controller_state();
            const auto prompt_edges = save_slot_menu_edges.update(prompt_state.dpad_up, prompt_state.dpad_down,
                                                                  prompt_state.cross, prompt_state.circle);
            const auto command = save_slot_menu.apply(prompt_edges);
            perform_save_slot_menu_command(command);
            save_slot_menu_select_previous = prompt_state.select;
            save_slot_menu_select_chorded = prompt_state.select;
        } else {
            save_slot_menu_edges.reset();
            if (save_card_device != nullptr && in_frontend_menu_flow && !online_mod_prompt_active &&
                !developer_notice.has_value()) {
                const auto controller = current_host_controller_state();
                if (controller.select && controller.start) {
                    save_slot_menu_select_chorded = true;
                }
                if (!controller.select && save_slot_menu_select_previous && !save_slot_menu_select_chorded) {
                    if (developer_unlock.active()) {
                        developer_notice = developer_unlock_notice;
                        developer_notice_edges.reset();
                        developer_notice_needs_release = true;
                    } else {
                        save_slot_menu.open(build_save_slot_menu_context());
                    }
                    save_slot_menu_opened_once = true;
                }
                if (!controller.select) {
                    save_slot_menu_select_chorded = false;
                }
                save_slot_menu_select_previous = controller.select;
                if (!save_slot_peer_since.has_value()) {
                    save_slot_peer_since = std::chrono::steady_clock::now();
                }
            } else {
                save_slot_menu_select_previous = false;
                save_slot_menu_select_chorded = false;
            }
        }
        static const bool save_card_hotplug = [] {
            const char* value = std::getenv("TAKEDOWN_SAVE_CARD_HOTPLUG");
            return value == nullptr || *value != '0';
        }();
        // As in the release: with the controller bridge, the card is connected only after the
        // controller gate has accepted its synthetic press. Connecting it earlier skips the gate at
        // boot, and the pending gate press then lands on a later menu (it confirmed LOAD PROFILE).
        if (save_card_hotplug && save_card_device != nullptr && !kernel.save_card_present() &&
            (!host_controller_bridge ||
             (virtual_ds2_trace.has_value() && virtual_ds2_trace->gate_accepted_step.has_value()))) {
            kernel.set_save_card_present(memory, true);
            std::cerr << "Save data: profile slot " << save_card_device->attached_slot().value_or(0U)
                      << " connected\n";
        }
        if (save_card_device != nullptr) {
            save_card_device->flush_if_idle(std::chrono::steady_clock::now(), std::chrono::seconds(2));
        }
        static_cast<void>(host_window->consume_capture_request());
        if (parallel_gs_input_reader != nullptr) {
            static_cast<void>(parallel_gs_input_reader->consume_capture_request());
        }
        static_cast<void>(host_window->consume_frame_step_toggle_request());
        if (parallel_gs_input_reader != nullptr) {
            static_cast<void>(parallel_gs_input_reader->consume_frame_step_toggle_request());
        }
        static_cast<void>(host_window->consume_frame_advance_request());
        if (parallel_gs_input_reader != nullptr) {
            static_cast<void>(parallel_gs_input_reader->consume_frame_advance_request());
        }
        const auto publish_capture_notice = [&](std::string notice) {
            host_window_capture_notice = std::move(notice);
            host_window_capture_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            host_window->set_status(host_window_capture_notice);
        };
        const auto capture_readiness = presentation::evaluate_gs_capture_boundary(
            host_window_capture_required_vblank, raised_vblank_starts, gs_renderer_worker.has_value(),
            host_window_has_completed_snapshot, host_window_completed_snapshot_vblank);
        if (host_window_capture_requested &&
            (capture_readiness == presentation::GsCaptureBoundaryReadiness::MissingTarget ||
             capture_readiness == presentation::GsCaptureBoundaryReadiness::MissedBoundary)) {
            host_window_capture_requested = false;
            host_window_capture_required_vblank.reset();
            host_window_capture_boundary_paused = false;
            publish_capture_notice("F8 capture failed \xe2\x80\x94 exact GS boundary was not retained");
            std::cerr << "F8 GS semantic capture failed: exact requested boundary was missed\n";
        }
        if (host_window_capture_requested &&
            capture_readiness == presentation::GsCaptureBoundaryReadiness::Ready) {
            const auto capture_boundary = host_window_capture_required_vblank;
            host_window_capture_requested = false;
            host_window_capture_required_vblank.reset();
            host_window_capture_boundary_paused = false;
            try {
                const auto& retained = gs_semantic_trace_writer->statistics().retained_nonempty_vblank;
                if (!(capture_boundary.has_value() && *capture_boundary != 0U && retained.has_value() &&
                      *capture_boundary - 1U == *retained)) {
                    publish_capture_notice(
                        "F8 capture skipped \xe2\x80\x94 latched interval contained no GS activity");
                    std::cerr << "F8 GS semantic capture skipped: boundary "
                              << capture_boundary.value_or(0U)
                              << " did not retain its immediately preceding GS interval\n";
                } else if (gs_renderer_worker.has_value() &&
                           !(host_window_has_consumed_scanout &&
                             host_window_consumed_snapshot_vblank == *capture_boundary &&
                             !host_window_rgba.empty() && host_window_vram.has_value())) {
                    publish_capture_notice(
                        "F8 capture skipped \xe2\x80\x94 latched boundary had no renderable scanout");
                    std::cerr << "F8 capture skipped: GS boundary " << *capture_boundary
                              << " did not produce a renderable host scanout\n";
                } else {
                    std::filesystem::path checkpoint_path;
                    std::filesystem::path visual_path;
                    std::filesystem::path vram_path;
                    try {
                        for (;;) {
                            checkpoint_path = gs_semantic_checkpoint_filename(
                                *gs_semantic_trace_path, gs_semantic_checkpoint_next_index);
                            visual_path = [&] {
                                std::filesystem::path path = checkpoint_path;
                                path.replace_extension(".ppm");
                                return path;
                            }();
                            vram_path = [&] {
                                std::filesystem::path path = checkpoint_path;
                                path.replace_extension(".gsvram");
                                return path;
                            }();
                            std::error_code checkpoint_exists_error;
                            const bool checkpoint_exists =
                                std::filesystem::exists(checkpoint_path, checkpoint_exists_error);
                            if (checkpoint_exists_error) {
                                throw std::runtime_error("could not inspect GS semantic checkpoint path " +
                                                         checkpoint_path.string() + ": " +
                                                         checkpoint_exists_error.message());
                            }
                            std::error_code visual_exists_error;
                            const bool visual_exists =
                                std::filesystem::exists(visual_path, visual_exists_error);
                            if (visual_exists_error) {
                                throw std::runtime_error("could not inspect F8 visual capture path " +
                                                         visual_path.string() + ": " +
                                                         visual_exists_error.message());
                            }
                            std::error_code vram_exists_error;
                            const bool vram_exists = std::filesystem::exists(vram_path, vram_exists_error);
                            if (vram_exists_error) {
                                throw std::runtime_error("could not inspect F8 VRAM capture path " +
                                                         vram_path.string() + ": " +
                                                         vram_exists_error.message());
                            }
                            if (!checkpoint_exists && !visual_exists && !vram_exists) {
                                break;
                            }
                            if (gs_semantic_checkpoint_next_index ==
                                std::numeric_limits<std::uint64_t>::max()) {
                                throw std::runtime_error(
                                    "GS semantic checkpoint filename sequence is exhausted");
                            }
                            ++gs_semantic_checkpoint_next_index;
                        }
                        if (gs_semantic_trace_writer->checkpoint_last_completed_vblank(checkpoint_path) ==
                            platform::GsSemanticTraceCheckpointStatus::NoRetainedFrame) {
                            publish_capture_notice(
                                "F8 capture pending \xe2\x80\x94 no active GS VBlank has completed yet");
                            std::cerr << "F8 GS semantic capture pending: no nonempty completed VBlank is available yet\n";
                        } else {
                            const std::uint64_t captured_vblank =
                                gs_semantic_trace_writer->statistics().retained_nonempty_vblank.value();
                            if (gs_renderer_worker.has_value()) {
                                write_rgba_ppm(visual_path, host_window_surface_width,
                                               host_window_surface_height, host_window_rgba);
                                write_raw_gs_vram(vram_path, host_window_vram->raw_backing());
                            }
                            ++gs_semantic_checkpoints_written;
                            gs_semantic_last_checkpoint_path = checkpoint_path;
                            ++gs_semantic_checkpoint_next_index;
                            if (gs_renderer_worker.has_value()) {
                                publish_capture_notice("F8 trace + visual + VRAM saved \xe2\x80\x94 " +
                                                       checkpoint_path.stem().string() +
                                                       ".{gssem,ppm,gsvram}");
                            } else {
                                publish_capture_notice("F8 semantic trace saved \xe2\x80\x94 " +
                                                       checkpoint_path.filename().string() +
                                                       " (GPU-only)");
                            }
                            const auto& vif_activity = memory.vu1_activity();
                            std::cout << "GS semantic checkpoint: VBlank=" << captured_vblank
                                      << " boundary=" << *capture_boundary << " file=\""
                                      << checkpoint_path.string() << "\"";
                            if (gs_renderer_worker.has_value()) {
                                std::cout << " visual=\"" << visual_path.string() << "\"" << " vram=\""
                                          << vram_path.string() << "\"";
                            } else {
                                std::cout << " mode=semantic-only";
                            }
                            std::cout << " path2-image-qw=" << vif_activity.vif1_path2_image_quadwords
                                      << " zero=" << vif_activity.vif1_path2_zero_image_quadwords;
                            if (vif_activity.vif1_zero_image_source_span_count != 0U) {
                                const auto& span = vif_activity.vif1_zero_image_source_spans
                                                       [vif_activity.vif1_zero_image_source_span_count - 1U];
                                std::cout << " last-recorded-zero-source=" << hex32(span.first_source)
                                          << ".." << hex32(span.last_source)
                                          << " chain=" << hex32(span.chain_tadr);
                            }
                            std::cout << '\n' << std::flush;
                        }
                    } catch (...) {
                        std::error_code cleanup_error;
                        std::filesystem::remove(checkpoint_path, cleanup_error);
                        std::filesystem::remove(visual_path, cleanup_error);
                        std::filesystem::remove(vram_path, cleanup_error);
                        throw;
                    }
                }
            } catch (const std::exception& error) {
                publish_capture_notice("F8 capture failed \xe2\x80\x94 " + std::string(error.what()));
                std::cerr << "F8 GS capture failed: " << error.what() << '\n';
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (gs_renderer_worker.has_value()) {
            static_cast<void>(memory.gs_capture().take_completed_image_transfers());
            const auto consume_completed_snapshot = [&](const bool wait_for_completion) {
                if (host_window_pending_snapshots.empty()) {
                    return false;
                }
                HostWindowPendingSnapshot& next = host_window_pending_snapshots.front();
                if (!wait_for_completion &&
                    next.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    return false;
                }
                HostWindowPendingSnapshot completed = std::move(next);
                host_window_pending_snapshots.pop_front();
                presentation::GsRendererWorkerSnapshot snapshot = [&completed] {
                    if (completed.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        return completed.future.get();
                    }
                    const platform::ScopedHostWait site(platform::HostWaitSite::GsWorkerSnapshot);
                    return completed.future.get();
                }();
                ++host_window_completed_snapshots;
                if (host_window_has_consumed_scanout &&
                    snapshot.completed_serial < host_window_consumed_snapshot_serial) {
                    return true;
                }
                host_window_has_completed_snapshot = true;
                host_window_completed_snapshot_vblank = completed.vblank;
                gs_renderer_statistics = snapshot.statistics;
                gs_renderer_queue_statistics = snapshot.queue_statistics;
                host_window_applied_transfers = gs_renderer_statistics.image_transfers_applied;
                host_window_rejected_transfers = gs_renderer_statistics.image_transfers_rejected;
                if (!snapshot.scanout_plan.has_value() || !snapshot.scanout_frame.has_value()) {
                    throw std::runtime_error(
                        "ordered GS window snapshot did not contain its requested scanout");
                }
                host_window_scanout_plan = *snapshot.scanout_plan;
                host_window_scanout_status = host_window_scanout_plan.status;
                presentation::GsScanoutFrame rendered = std::move(*snapshot.scanout_frame);
                host_window_render_status = rendered.status;
                if (rendered.status == presentation::GsScanoutRenderStatus::Rendered) {
                    host_window_rgba = std::move(rendered.rgba);
                    host_window_surface_width = rendered.width;
                    host_window_surface_height = rendered.height;
                    host_window_has_consumed_scanout = true;
                    host_window_consumed_snapshot_serial = snapshot.completed_serial;
                    host_window_consumed_snapshot_vblank = completed.vblank;
                    if (!snapshot.local_memory.has_value()) {
                        throw std::runtime_error("ordered GS window snapshot did not retain its local memory");
                    }
                    host_window_vram = std::move(snapshot.local_memory);
                } else {
                    host_window_rgba.clear();
                    host_window_surface_width = 0U;
                    host_window_surface_height = 0U;
                }
                return true;
            };
            if (force_present) {
                consume_completed_snapshot(true);
            }
            while (!host_window_pending_snapshots.empty() && consume_completed_snapshot(false)) {
            }
            const auto& display_state = memory.gs_privileged_display().state();
            gs_renderer_queue_statistics = gs_renderer_worker->queue_statistics();
            if (force_present) {
                const auto scanout_plan = presentation::plan_gs_scanout(display_state);
                host_window_pending_snapshots.emplace_back(HostWindowPendingSnapshot{
                    gs_renderer_worker->request_snapshot(scanout_plan), raised_vblank_starts});
                ++host_window_final_snapshot_requests;
                consume_completed_snapshot(true);
            }
        }
        const auto now_nanoseconds = static_cast<std::uint64_t>(now.time_since_epoch().count());
        if (!host_window_pacer.should_present(now_nanoseconds, force_present)) {
            return true;
        }
        const std::uint64_t queued_image_transfers =
            skip_compat_present ? 0U : memory.gs_capture().activity().queued_image_transfers;
        const auto display_policy = evaluate_display_policy();
        std::ostringstream status;
        status << (parallel_gs_only && parallel_gs_input_reader != nullptr
                       ? "external paraLLEl-GS GPU \xe2\x80\x94 controls in GPU window"
                   : parallel_gs_only
                       ? "external paraLLEl-GS GPU \xe2\x80\x94 controls in this window"
                       : "GS local-memory scanout");
        if (parallel_gs_input_reader != nullptr) {
            status << "; input packets " << parallel_gs_input_reader->packet_count();
        }
        if (host_frame_step_enabled) {
            if (host_frame_step_paused) {
                status << " [FRAME PAUSED: F7 next, F6 resume]";
            } else if (host_frame_step_advancing) {
                status << " [FRAME ADVANCING]";
            } else {
                status << " [FRAME STEP ARMING]";
            }
        }
        status << " \xe2\x80\x94 guest steps " << committed_steps << "; uploads " << queued_image_transfers
               << "; placed " << host_window_applied_transfers;
        if (gs_renderer_worker.has_value()) {
            status << "; local-moves " << gs_renderer_statistics.local_to_local_transfers_applied << '/'
                   << gs_renderer_statistics.local_to_local_transfers_rejected;
            status << "; solid " << gs_renderer_statistics.solid_points_drawn << "P/"
                   << gs_renderer_statistics.solid_lines_drawn << "L/"
                   << gs_renderer_statistics.solid_sprites_drawn << "S/"
                   << gs_renderer_statistics.solid_triangles_drawn << "T; deferred-texture "
                   << gs_renderer_statistics.textured_primitives_deferred << "; empty "
                   << gs_renderer_statistics.empty_primitives << "; present-sprites "
                   << gs_renderer_statistics.presentation_sprites_drawn << "; textured "
                   << gs_renderer_statistics.textured_sprites_drawn << "S/"
                   << gs_renderer_statistics.textured_triangles_drawn << 'T';
        }
        status << "; host " << presentation::to_string(display_policy.host_frame_topology) << "; source "
               << presentation::to_string(display_policy.source_topology);
        if (host_window_rejected_transfers != 0U) {
            status << "; unsupported " << host_window_rejected_transfers;
        }
        status << "; plan " << presentation::to_string(host_window_scanout_status);
        if (host_window_scanout_plan.progressive_frame_extraction) {
            status << "; interlace->progressive framebuffer";
        }
        if (!host_window_rgba.empty()) {
            status << "; circuit " << host_window_scanout_plan.circuit << " source "
                   << host_window_scanout_plan.source_x << ',' << host_window_scanout_plan.source_y << ' '
                   << host_window_scanout_plan.source_width << 'x' << host_window_scanout_plan.source_height
                   << " PSM " << hex32(host_window_scanout_plan.source_surface.pixel_storage_mode)
                   << " -> " << host_window_surface_width << 'x' << host_window_surface_height;
        }
        status << "; frame " << presentation::to_string(host_window_render_status);
        if (!host_window_capture_notice.empty() &&
            now_nanoseconds <
                static_cast<std::uint64_t>(host_window_capture_notice_until.time_since_epoch().count())) {
            status << "; " << host_window_capture_notice;
        }
        if (host_window_capture_requested && host_window_capture_required_vblank.has_value()) {
            status << "; F8 waiting for VBlank " << *host_window_capture_required_vblank << " scanout";
        }
        bool visual_changed = false;
        if (!host_window_rgba.empty()) {
            if (host_window_last_presented_rgba.empty() ||
                host_window_last_presented_width != host_window_surface_width ||
                host_window_last_presented_height != host_window_surface_height ||
                host_window_rgba != host_window_last_presented_rgba) {
                host_window_visual_fps.note_frame(now_nanoseconds);
                ++host_window_presented_scanouts;
                host_window_last_presented_rgba = host_window_rgba;
                host_window_last_presented_width = host_window_surface_width;
                host_window_last_presented_height = host_window_surface_height;
                visual_changed = true;
            }
        }
        bool upload_host_frame;
        if (gs_renderer_worker.has_value() && host_window->stats_overlay_enabled() &&
            !host_window_rgba.empty()) {
            presentation::StatsOverlayValues overlay_values{};
            overlay_values.visual_fps = host_window_visual_fps.fps(now_nanoseconds);
            overlay_values.visual_average_fps = host_window_visual_fps.average_fps(now_nanoseconds);
            overlay_values.guest_fps = host_window_guest_fps.fps(now_nanoseconds);
            overlay_values.guest_average_fps = host_window_guest_fps.average_fps(now_nanoseconds);
            overlay_values.present_fps = host_window_present_fps.fps(now_nanoseconds);
            overlay_values.present_average_fps = host_window_present_fps.average_fps(now_nanoseconds);
            overlay_values.solid_sprites = gs_renderer_statistics.solid_sprites_drawn;
            overlay_values.presentation_sprites = gs_renderer_statistics.presentation_sprites_drawn;
            overlay_values.textured_sprites = gs_renderer_statistics.textured_sprites_drawn;
            overlay_values.solid_triangles = gs_renderer_statistics.solid_triangles_drawn;
            overlay_values.textured_triangles = gs_renderer_statistics.textured_triangles_drawn;
            overlay_values.written_pixels = gs_renderer_statistics.written_pixels;
            overlay_values.covered_pixels = gs_renderer_statistics.covered_pixels;
            overlay_values.image_transfers_applied = gs_renderer_statistics.image_transfers_applied;
            overlay_values.drawable_kicks = gs_renderer_statistics.drawable_kicks;
            overlay_values.textured_primitives_deferred = gs_renderer_statistics.textured_primitives_deferred;
            std::vector<std::uint8_t> next_overlay_rgba = host_window_rgba;
            presentation::composite_stats_overlay(next_overlay_rgba, host_window_surface_width,
                                                  host_window_surface_height, overlay_values);
            upload_host_frame = visual_changed || !host_window_texture_contains_overlay ||
                                next_overlay_rgba != host_window_overlay_rgba;
            host_window_overlay_rgba = std::move(next_overlay_rgba);
            host_window_texture_contains_overlay = true;
        } else {
            upload_host_frame = visual_changed || host_window_texture_contains_overlay;
            host_window_overlay_rgba.clear();
            host_window_texture_contains_overlay = false;
        }
        if (!host_window_rgba.empty() &&
            (save_slot_menu.is_open() || save_slot_peer_visible(in_frontend_menu_flow))) {
            const ui::HostPrompt& drawn_prompt =
                save_slot_menu.is_open() ? save_slot_menu.prompt() : *save_slot_peer_entry;
            std::vector<std::uint8_t> next_prompt_rgba =
                host_window_overlay_rgba.empty() ? host_window_rgba : host_window_overlay_rgba;
            ui::composite_host_prompt(next_prompt_rgba, host_window_surface_width,
                                      host_window_surface_height, drawn_prompt);
            host_window_overlay_rgba = std::move(next_prompt_rgba);
            host_window_texture_contains_overlay = true;
            upload_host_frame = true;
        }
        std::optional<platform::RgbaFrame> frame;
        if (!host_window_rgba.empty() && upload_host_frame) {
            const std::vector<std::uint8_t>& presentation_pixels =
                host_window_overlay_rgba.empty() ? host_window_rgba : host_window_overlay_rgba;
            frame = platform::RgbaFrame{host_window_surface_width, host_window_surface_height,
                                        presentation_pixels};
        }
        if (!skip_compat_present) {
            host_window->present(frame, status.str());
            ++host_window_present_calls;
            host_window_present_fps.note_frame(host_steady_now_nanoseconds());
        }
        return true;
    };
    bool profile_timing_started = false;
    bool profile_timing_active = false;
    std::uint64_t ee_pc_profile_instruction_origin = 0U;
    const bool guest_execution_profiling_requested = ee_pc_sampler != nullptr || ee_host_phase_profiler != nullptr ||
                                                     ee_graphics_service_diagnostics != nullptr ||
                                                     vu1_profile_directory.has_value();
    const auto start_guest_execution_profiles = [&](std::uint64_t instruction_origin) {
        if (profile_timing_started || profile_timing_active) {
            throw std::logic_error("guest-execution profilers may be started only once");
        }
        join_pump_region_calls = 0U;
        join_pump_region_inert_dmac_admissions = 0U;
        join_pump_region_retired_instructions = 0U;
        join_pump_region_zero_retired_rejections = 0U;
        join_pump_graphics_deadline_queries = 0U;
        join_pump_graphics_deadline_admitted_spans = 0U;
        join_pump_graphics_deadline_retired_cycles = 0U;
        join_pump_graphics_deadline_retired_instructions = 0U;
        join_pump_graphics_deadline_endpoint_services = 0U;
        join_pump_graphics_deadline_fallbacks.fill(0U);
        join_pump_region_exits.fill(0U);
        ee_pc_profile_instruction_origin = instruction_origin;
        if (ee_pc_sampler != nullptr) {
            ee_pc_sampler->start_host_timing();
        }
        if (ee_host_phase_profiler != nullptr) {
            ee_host_phase_profiler->start_host_timing();
        }
        if (ee_graphics_service_diagnostics != nullptr) {
            memory.set_graphics_service_diagnostics(ee_graphics_service_diagnostics.get());
            ee_graphics_service_diagnostics->start_window();
        }
        if (vu1_profile_directory.has_value()) {
            memory.vu1_program_cache().set_profiling_enabled(true);
        }
        profile_timing_started = true;
        profile_timing_active = true;
    };
    const auto stop_guest_execution_profiles = [&]() {
        if (!profile_timing_active) {
            return;
        }
        profile_timing_active = false;
        if (ee_pc_sampler != nullptr) {
            ee_pc_sampler->stop_host_timing();
        }
        if (ee_host_phase_profiler != nullptr) {
            ee_host_phase_profiler->stop_host_timing();
        }
        if (ee_graphics_service_diagnostics != nullptr) {
            ee_graphics_service_diagnostics->stop_window();
            memory.set_graphics_service_diagnostics(nullptr);
        }
        if (vu1_profile_directory.has_value()) {
            memory.vu1_program_cache().set_profiling_enabled(false);
        }
    };
    const auto observe_profiled_ee_span = [&](const std::uint64_t first_instruction, const takedown::guest::GuestAddress pc,
                                              const std::uint64_t retired_instructions, const runtime::EePcSampleKind kind) {
        if (ee_pc_sampler == nullptr || !profile_timing_active) {
            return;
        }
        if (ee_pc_profile_instruction_origin > first_instruction) {
            throw std::logic_error("EE PC profile instruction precedes its scope origin");
        }
        ee_pc_sampler->observe_span(first_instruction - ee_pc_profile_instruction_origin, pc, retired_instructions, kind);
    };
    const auto begin_profiled_host_phase = [&](const runtime::EeHostPhase host_phase) {
        if (ee_host_phase_profiler != nullptr && profile_timing_active) [[unlikely]] {
            return ee_host_phase_profiler->begin_phase(host_phase);
        }
        return runtime::EeHostPhaseProfiler::Scope{};
    };
    const auto present_profiled_host_window = [&](bool force_present) {
        if (ee_host_phase_profiler != nullptr && profile_timing_active) {
            auto phase = ee_host_phase_profiler->begin_phase(runtime::EeHostPhase::HostPresentation);
            return present_host_window(force_present);
        }
        return present_host_window(force_present);
    };
    const auto drain_femain_live_source_context = [](FEmainLiveSourceDrainContext& context) {
        using SourcePhase = runtime::Slus21050FemainLiveSourceProducer::Phase;
        using TerminalPhase = runtime::Slus21050FemainLiveTerminalCoordinator::Phase;
        if (context.source == nullptr || context.failure_pending) {
            return !context.failure_pending;
        }
        if (context.source->phase() == SourcePhase::MarkerResolved && !context.source->drain_resolved_marker()) {
            context.failure_pending = true;
        }
        if (!context.failure_pending && context.source->source_completion_pending() &&
            !context.source->drain_source_completion()) {
            context.failure_pending = true;
        }
        if (context.source->phase() == SourcePhase::Failed) {
            context.failure_pending = true;
        }
        if (!context.failure_pending && context.terminal != nullptr && context.terminal->phase() == TerminalPhase::Failed) {
            context.failure_pending = true;
        }
        if (!context.failure_pending && context.source->phase() == SourcePhase::SourceCompleted &&
            (context.terminal == nullptr || context.terminal->phase() != TerminalPhase::EopReady)) {
            context.failure_pending = true;
        }
        if (!context.failure_pending && context.source->phase() == SourcePhase::SourceCompleted &&
            context.terminal_attached && context.terminal != nullptr) {
            if (context.completion_ledger != nullptr && context.pending_correlation.has_value()) {
                static_cast<void>(context.completion_ledger->arm_after_source_eop(
                    context.pending_correlation->publication, context.pending_correlation->source,
                    runtime::Slus21050FemainFrameGuestBoundary{context.raised_vblank_starts + 1U,
                                                               context.next_vblank_start / 6144U + 1U}));
                context.pending_correlation.reset();
            }
            context.terminal->detach(context.capture, *context.source);
            context.terminal_attached = false;
        }
        return !context.failure_pending;
    };
    FEmainLiveSourceDrainContext host_femain_live_source_drain_context{
        host_femain_live_source_producer,
        host_femain_live_terminal_coordinator,
        memory.gs_capture(),
        host_femain_live_terminal_coordinator_attached,
        host_femain_live_source_failure_pending,
        femain_parallel_gs_completion_ledger.get(),
        pending_femain_parallel_gs_correlation,
        raised_vblank_starts,
        next_vblank_start};
    const auto drain_femain_live_source = [&]() {
        return drain_femain_live_source_context(host_femain_live_source_drain_context);
    };
    const auto advance_profiled_vif1_dmac = [&](std::uint64_t cycles) {
        if (ee_host_phase_profiler != nullptr && profile_timing_active) {
            auto phase = ee_host_phase_profiler->begin_phase(runtime::EeHostPhase::Vif1DmacVu1Inclusive);
            memory.advance_vif1_dmac(cycles);
            return;
        }
        memory.advance_vif1_dmac(cycles);
    };
    const auto advance_profiled_gif_dmac = [&](std::uint64_t cycles) {
        if (ee_host_phase_profiler != nullptr && profile_timing_active) {
            auto phase = ee_host_phase_profiler->begin_phase(runtime::EeHostPhase::GifDmacStreamInclusive);
            memory.advance_gif_dmac(cycles);
            return;
        }
        memory.advance_gif_dmac(cycles);
    };
    const auto service_graphics_dmacs_until_with_femain = [&](std::uint64_t target_cycle) {
        abort_failed_femain_live_source_bridge();
        drain_femain_live_source();
        abort_failed_femain_live_source_bridge();
        if (target_cycle > vif1_dmac_serviced_ee_cycles) {
            advance_profiled_vif1_dmac(target_cycle - vif1_dmac_serviced_ee_cycles);
            vif1_dmac_serviced_ee_cycles = target_cycle;
            drain_femain_live_source();
            abort_failed_femain_live_source_bridge();
        }
        if (target_cycle > gif_dmac_serviced_ee_cycles) {
            advance_profiled_gif_dmac(target_cycle - gif_dmac_serviced_ee_cycles);
            gif_dmac_serviced_ee_cycles = target_cycle;
            drain_femain_live_source();
            abort_failed_femain_live_source_bridge();
        }
        activate_femain_live_source_bridge();
    };
    const auto timed_graphics_scheduler_work_pending = [&]() {
        memory.graphics_async_fence(runtime::GraphicsAsyncFence::SchedulerPoll);
        if (memory.graphics_async_mode() == runtime::GraphicsAsyncMode::Decoupled) {
            return memory.queued_dmac_completion_count() != 0U;
        }
        return memory.gif_dmac_can_progress() || memory.vif1_dmac_can_progress() ||
               memory.queued_dmac_completion_count() != 0U;
    };
    const auto current_ui_execution_context = [&]() {
        return runtime::slus_21050::SubmitUiDrawBucketsExecutionContext{
            kernel.current_thread_id(), kernel.guest_callback_active(), kernel.guest_callback_identity()};
    };
    const auto current_retail_timer_consumer_context = [&]() {
        return runtime::RetailTimerConsumerTraceContext{
            .thread_id = kernel.current_thread_id(),
            .callback_active = kernel.guest_callback_active(),
            .callback_identity = kernel.guest_callback_identity()};
    };
    const auto current_retail_timer_owner_context = [&]() {
        return runtime::RetailTimerOwnerAbiTraceContext{
            .thread_id = kernel.current_thread_id(),
            .callback_active = kernel.guest_callback_active(),
            .callback_identity = kernel.guest_callback_identity()};
    };
    if (!profile_guest_work_window && guest_execution_profiling_requested) {
        start_guest_execution_profiles(0U);
    }
    constexpr std::uint32_t kRetailControllerAcceptEdge = 0x004f5e4cU;
    constexpr std::uint32_t kRetailControllerTransportPollReturn = 0x00214a9cU;
    constexpr std::uint32_t kRetailMovieDecoderUpdate = 0x0030d560U;
    constexpr std::uint32_t kRetailVblankPollBegin = 0x0010c2f0U;
    constexpr std::uint32_t kRetailVblankPollBranch = 0x0010c304U;
    constexpr std::uint32_t kRetailVblankPollEnd = 0x0010c308U;
    constexpr std::uint32_t kRetailVblankCallbackPollBegin = 0x0010c390U;
    constexpr std::uint32_t kRetailVblankCallbackPollBranch = 0x0010c3a4U;
    constexpr std::uint32_t kRetailVblankCallbackPollEnd = 0x0010c3a8U;
    constexpr std::uint64_t kHostWindowLoopIterationInterval = 4096U;
    const bool scalar_cold_observers_enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_SCALAR_COLD_OBSERVERS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    const bool scalar_observers_configured = retail_trace || collision_trace != nullptr ||
                                             vehicle_world_contact_probe != nullptr || spark_trace != nullptr ||
                                             retail_timer_owner_abi_trace.enabled() ||
                                             retail_timer_consumer_trace.enabled() || ee_host_phase_profiler != nullptr ||
                                             femain_dynamic_callback_probe != nullptr ||
                                             femain_frame_closure_diagnostic != nullptr ||
                                             femain_retail_consume_observation_adapter != nullptr;
    const bool scalar_burst_requested = [] { const auto* value = std::getenv("TAKEDOWN_SCALAR_BURST"); return value != nullptr && std::string_view{value} == "1"; }();
    // SCALAR-BURST-ELIGIBILITY-BEGIN
    const bool scalar_burst_statically_eligible =
        scalar_cold_observers_enabled &&
        !scalar_observers_configured &&
        !retail_trace &&
        collision_trace == nullptr &&
        vehicle_world_contact_probe == nullptr &&
        spark_trace == nullptr &&
        !retail_timer_owner_abi_trace.enabled() &&
        !retail_timer_consumer_trace.enabled() &&
        ee_host_phase_profiler == nullptr &&
        femain_dynamic_callback_probe == nullptr &&
        femain_frame_closure_diagnostic == nullptr &&
        femain_retail_consume_observation_adapter == nullptr &&
        ee_pc_sampler == nullptr &&
        ee_graphics_service_diagnostics == nullptr &&
        !vu1_profile_directory.has_value() &&
        compiled_budget_profiler == nullptr &&
        ee_pc_hit_watch.empty() &&
        !object_packet_capture.enabled() &&
        !instance_batch_capture.enabled() &&
        !model_submission_probe.enabled() &&
        !retail_timer_owner_observer_enabled &&
        native_simulation_shadow == nullptr &&
        femain_retail_consume_invocation_barrier == nullptr &&
        femain_terminal_diagnostic == nullptr &&
        !femain_park_pcs.has_value() &&
        gs_semantic_trace_writer == nullptr &&
        !memory.ram_write_watch_active() &&
        !femain_live_transaction_enabled &&
        !subframe_input_bridge.has_value() &&
        scripted_input_presses.empty() &&
        !rwa_iop_shadow.has_value() &&
        !native_only &&
        !compiled_coverage_available &&
        !vu1_worker_differential &&
        !vu1_worker_consume;
    // SCALAR-BURST-ELIGIBILITY-END
    const bool scalar_burst_enabled = scalar_burst_requested && scalar_burst_statically_eligible;
    takedown::runtime::ScalarBurstStatistics scalar_burst{};
    const bool scalar_burst_audit = [] {
        const auto* value = std::getenv("TAKEDOWN_SCALAR_BURST_AUDIT");
        return value != nullptr && std::string_view{value} == "1";
    }();
    const bool burst_native_calls = [] {
        const auto* value = std::getenv("TAKEDOWN_BURST_NATIVE_CALLS");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const bool burst_native_decline_steps = [] {
        const auto* value = std::getenv("TAKEDOWN_BURST_NATIVE_DECLINE");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const bool keep_diagnostic_hook_marks = [] {
        const auto* value = std::getenv("TAKEDOWN_EE_AOT_DIAGNOSTIC_MARKS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    takedown::runtime::ScalarBurstNativeEntries scalar_burst_native_entries(memory.size());
    // SCALAR-BURST-HOOKS-BEGIN
    const takedown::runtime::ScalarBurstHookFilter scalar_burst_hooks = [&]() {
        takedown::runtime::ScalarBurstHookFilter hooks{memory.size()};
        hooks.mark_pc(0U);
        hooks.mark_pc(takedown::guest::EeKernelHle::kGuestCallbackReturnAddress);
        if (ui_draw_session_feed.enabled()) {
            hooks.mark_pc(takedown::runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushEntryAddress);
            hooks.mark_pc(takedown::runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushCommitAddress);
            hooks.mark_pc(takedown::runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushEndAddress);
            hooks.mark_pc(takedown::runtime::slus_21050::SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress);
            hooks.mark_pc(takedown::runtime::slus_21050::kSubmitPlainTextLinesAddress);
        }
        hooks.mark_pc(takedown::guest::DirtyDnasCompatHle::kStartTrampoline);
        hooks.mark_pc(takedown::guest::DirtyDnasCompatHle::kUpdateTrampoline);
        hooks.mark_pc(takedown::guest::DirtyDnasCompatHle::kStopTrampoline);
        static_assert(JoinPumpRegion::kCallbackTailEffectEndAddress >= JoinPumpRegion::kExtendedCallerEffectEndAddress && JoinPumpRegion::kExtendedCallerEffectEndAddress >= JoinPumpRegion::kDirectEffectEndAddress);
        hooks.mark_half_open_range(takedown::recompiler::JoinPumpRegion::kEntryAddress,
                                   takedown::recompiler::JoinPumpRegion::kCallbackTailEffectEndAddress);
        hooks.mark_pc(0x003254f0U);
        hooks.mark_pc(0x00130ea8U);
        hooks.mark_pc(0x00325030U);
        hooks.mark_pc(0x00107d8cU);
        hooks.mark_pc(0x00325124U);
        hooks.mark_pc(0x00324f40U);
        hooks.mark_pc(0x0031cf30U);
        hooks.mark_pc(kRetailControllerTransportPollReturn);
        hooks.mark_pc(takedown::runtime::Slus21050Controllers::decode_boundary);
        hooks.mark_pc(0x00214064U);
        hooks.mark_pc(0x00214294U);
        hooks.mark_pc(0x0042e608U);
        hooks.mark_pc(0x0022cfe0U);
        hooks.mark_pc(0x0022cffcU);
        hooks.mark_pc(0x00449250U);
        hooks.mark_pc(0x00437370U);
        hooks.mark_pc(0x00438898U);
        hooks.mark_pc(0x004388a0U);
        hooks.mark_pc(0x004388ccU);
        hooks.mark_pc(kRetailMovieDecoderUpdate);
        hooks.mark_pc(0x004389b0U);
        hooks.mark_pc(0x00438becU);
        hooks.mark_pc(0x001f2518U);
        hooks.mark_pc(kRetailVblankPollBranch);
        hooks.mark_pc(kRetailVblankCallbackPollBranch);
        hooks.mark_closed_range(kRetailVblankPollBegin, kRetailVblankPollEnd);
        hooks.mark_closed_range(kRetailVblankCallbackPollBegin, kRetailVblankCallbackPollEnd);
        hooks.mark_closed_range(0x00237180U, 0x0023719cU);
        if (object_packet_capture.enabled() || keep_diagnostic_hook_marks) {
            hooks.mark_pc(takedown::runtime::slus_21050::kObjectPacketBegin);
            hooks.mark_pc(takedown::runtime::slus_21050::kObjectPacketEnd);
        }
        if (instance_batch_capture.enabled()) {
            for (const auto address : takedown::runtime::slus_21050::InstanceBatchCapture::hook_addresses()) {
                hooks.mark_pc(address);
            }
        }
        if (retail_timer_owner_observer_enabled || keep_diagnostic_hook_marks) {
            hooks.mark_pc(0x001d3a60U);
            hooks.mark_pc(0x001d3c04U);
        }
        if (retail_trace || keep_diagnostic_hook_marks) {
            hooks.mark_pc(0x002a5dfcU);
            hooks.mark_pc(0x003a2100U);
            hooks.mark_pc(0x0034a4e0U);
            hooks.mark_pc(0x0034bc30U);
            for (const auto& milestone : retail_pc_milestones) {
                hooks.mark_pc(milestone.address);
            }
        }
        for (const auto& watch : ee_pc_hit_watch) {
            hooks.mark_pc(watch.address);
        }
        for (const auto& block : compiled_block_dispatcher.registered_blocks()) {
            hooks.mark_pc(block.address);
        }
        if (variable_frame_rate.has_value()) {
            for (const auto address : takedown::runtime::Slus21050VariableFrameRate::hook_addresses()) {
                hooks.mark_pc(address);
            }
        }
        for (const auto address : takedown::runtime::Slus21050CameraFov::hook_addresses()) { hooks.mark_pc(address); }
        if (pc_options.enabled()) {
            for (const auto address : takedown::runtime::Slus21050NativePcOptions::hook_addresses()) {
                hooks.mark_pc(address);
            }
        }
        if (native_car_select_active) {
            for (const auto address : takedown::runtime::Slus21050CarSelect::hook_addresses()) {
                hooks.mark_pc(address);
            }
        }
        if (native_model_live) hooks.mark_pc(0x0021d2e0U);
        for (const auto& function : native_function_dispatcher.registered_functions()) { if (!hooks.marked(function.address)) { scalar_burst_native_entries.add(function.address); } hooks.mark_pc(function.address); }
        return hooks;
    }();
    // SCALAR-BURST-HOOKS-END
    if (const char* list_path = std::getenv("TAKEDOWN_NATIVE_FUNCTION_LIST"); list_path != nullptr && *list_path != '\0') {
        std::ofstream list(list_path);
        for (const auto& function : native_function_dispatcher.registered_functions()) {
            list << "0x" << std::hex << std::setw(8) << std::setfill('0') << function.address << std::dec << ' '
                 << function.name << '\n';
        }
    }
    if (const char* hooks_path = std::getenv("TAKEDOWN_EE_AOT_HOOKS"); hooks_path != nullptr && *hooks_path != '\0') {
        std::ofstream hooks_out(hooks_path);
        hooks_out << "pc\tkind\n";
        for (std::uint32_t pc = 4U; pc < scalar_burst_hooks.ordinary_code_limit(); pc += 4U) {
            if (scalar_burst_hooks.marked(pc)) {
                char line[32];
                std::snprintf(line, sizeof line, "0x%08x\t", pc);
                hooks_out << line << (scalar_burst_native_entries.contains(pc) ? "native" : "other") << '\n';
            }
        }
    }
    const bool ee_aot_enabled = [&]() {
        if (!scalar_burst_enabled || !takedown::guest::ee_aot_module_linked() || std::getenv("TAKEDOWN_CFLOW") != nullptr) {
            return false;
        }
        takedown::guest::ee_aot_install(
            memory,
            [&scalar_burst_hooks](const takedown::guest::GuestAddress pc) { return scalar_burst_hooks.marked(pc); },
            [&scalar_burst_native_entries, &native_function_dispatcher](const takedown::guest::GuestAddress pc) {
            return scalar_burst_native_entries.contains(pc) && native_function_dispatcher.lookup(pc) != nullptr &&
                   native_function_dispatcher.lookup(pc)->traits.scheduler_state_transparent;
        });
        takedown::guest::ee_aot_set_session_stops(
            takedown::runtime::slus_21050::SubmitUiDrawBucketsSessionFeed::static_boundaries());
        memory.set_ee_aot_watch_range(0U, 0x004e2ce8U, 0x004e2cecU);
        memory.set_ee_aot_watch_range(1U, kRetailControllerAcceptEdge, kRetailControllerAcceptEdge + 2U);
        const char* const value = std::getenv("TAKEDOWN_EE_AOT");
        return value == nullptr || *value != '0';
    }();
    const bool ee_aot_in_ui_session = [] {
        const char* const value = std::getenv("TAKEDOWN_EE_AOT_UI_SESSION");
        return value == nullptr || *value != '0';
    }();
    struct ScalarBurstRamWatch {
        const std::byte* ram{};
        bool unmirrored{};
        bool staging_live{};
        std::uint32_t staging_last_pointer{};
        std::uint8_t edge_mode{};
        [[nodiscard]] bool fires() const {
            if (unmirrored) {
                return true;
            }
            if (staging_live) {
                const auto* const word = ram + 0x004e2ce8U;
                const auto pointer = std::to_integer<std::uint32_t>(word[0]) | (std::to_integer<std::uint32_t>(word[1]) << 8U) | (std::to_integer<std::uint32_t>(word[2]) << 16U) | (std::to_integer<std::uint32_t>(word[3]) << 24U);
                if (pointer != staging_last_pointer) {
                    return true;
                }
            }
            if (edge_mode != 0U) {
                const auto* const word = ram + kRetailControllerAcceptEdge;
                const bool edge = (std::to_integer<std::uint32_t>(word[0]) | std::to_integer<std::uint32_t>(word[1])) != 0U;
                if (edge == (edge_mode == 1U)) {
                    return true;
                }
            }
            return false;
        }
    };
    const auto scalar_burst_ram_watch = [&](const std::uint64_t first_skipped_step) {
        const auto ram = std::as_const(memory).bytes();
        ScalarBurstRamWatch watch{};
        watch.ram = ram.data();
        constexpr std::size_t kMirroredBytes = 0x00500000U;
        watch.staging_live = rwa_staging_samples.size() < 8U && memory.contains(0x4e2ce8U, 8U);
        watch.staging_last_pointer = rwa_staging_last_pointer;
        if (virtual_ds2_trace.has_value()) {
            if (!virtual_ds2_trace->action_edge_step.has_value()) {
                watch.edge_mode = 1U;
            } else {
                if (!virtual_ds2_trace->action_edge_clear_step.has_value()) {
                    if (first_skipped_step > *virtual_ds2_trace->action_edge_step) {
                        watch.edge_mode = 2U;
                    } else {
                        watch.unmirrored = true;
                    }
                }
            }
        }
        if ((watch.staging_live || watch.edge_mode != 0U) && ram.size() < kMirroredBytes) {
            watch.unmirrored = true;
        }
        return watch;
    };
    // SCALAR-BURST-START-PREDICATE-BEGIN
    const auto scalar_burst_may_start = [&]() {
        if (native_model_live && native_model_live->failed()) {
            return false;
        }
        if (!memory.graphics_dmacs_idle()) {
            return false;
        }
        if (kernel.sif_command_callback_poll_can_progress()) {
            return false;
        }
        if (!kernel.guest_callback_active() && (memory.queued_dmac_completion_count() != 0U || kernel.pending_dmac_callback_can_dispatch(state) || kernel.pending_intc_callback_can_dispatch(state, memory))) {
            return false;
        }
        if (guest_work_measurement_start_input_pending || host_frame_step_paused || host_window_capture_boundary_paused || native_simulation_shadow_stop_requested) {
            return false;
        }
        if (guest_input_replay != nullptr && guest_input_replay->instruction_boundary_action_pending()) {
            return false;
        }
        if (vif_packet_ring_flush_claim_tracker.active()) {
            return false;
        }
        if (!rwa_first_op05_step_valid && kernel.rwa_activity().opcode_counts[0x05U] != 0U) {
            return false;
        }
        return memory.ee_memory_write_observer() == nullptr;
    };
    // SCALAR-BURST-START-PREDICATE-END
    const auto ee_aot_burst_step = [&](EeStepResult& result, const std::uint64_t budget, const takedown::guest::GuestAddress declined_entry,
                                       std::uint8_t& outcome) -> std::uint64_t {
        const auto run = takedown::guest::run_ee_aot(state, memory, budget, committed_steps, declined_entry);
        outcome = run.outcome;
        if (run.executed == 0U) {
            return 0U;
        }
        const std::uint64_t skipped = run.outcome == takedown::guest::kEeAotRunAccounted ? 0U : run.executed - 1U;
        if (skipped != 0U) {
            committed_steps += skipped;
            executed_instructions += skipped;
            emulated_ee_cycles += skipped;
            runtime::service_graphics_dmacs_until(
                emulated_ee_cycles, vif1_dmac_serviced_ee_cycles, gif_dmac_serviced_ee_cycles,
                [&memory](const std::uint64_t cycles) { memory.advance_idle_vif1_dmac(cycles); },
                [&memory](const std::uint64_t cycles) { memory.advance_idle_gif_dmac(cycles); });
            if (host_window != nullptr) {
                host_window_loop_iterations += skipped;
            }
            kernel.set_ee_cycle_count(emulated_ee_cycles);
            memory.set_current_guest_step(committed_steps);
        }
        memory.set_current_ee_pc(run.last_pc);
        result = run.last;
        return run.executed;
    };
    const bool burst_native_fast = [] {
        const auto* value = std::getenv("TAKEDOWN_BURST_NATIVE_FAST");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const bool burst_dispatch_trust = [] {
        const auto* value = std::getenv("TAKEDOWN_BURST_DISPATCH_TRUST");
        return value == nullptr || std::string_view{value} != "0";
    }();
    const auto scalar_burst_horizon = [&](const std::size_t execution_step_limit) -> std::uint64_t {
        using takedown::runtime::scalar_burst_boundaries_before;
        const auto first_step = static_cast<std::uint64_t>(committed_steps) + 1U;
        const auto first_cycle = emulated_ee_cycles + 1U;
        auto horizon = scalar_burst_boundaries_before(first_step, execution_step_limit);
        horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, next_vblank_start));
        horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, spu2_renderer.next_sample_cycle()));
        if (const auto deadline = kernel.next_sif_dma_completion_deadline(); deadline.has_value()) {
            horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, *deadline));
        }
        if (!kernel.guest_callback_active()) {
            if (const auto deadline = kernel.next_alarm_deadline(); deadline.has_value()) {
                horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, *deadline));
            }
            if (const auto deadline = kernel.next_thread_time_slice_deadline(); deadline.has_value()) {
                horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, *deadline));
            }
        }
        if (host_window != nullptr) {
            horizon = std::min(horizon, scalar_burst_boundaries_before(first_step, next_host_window_step));
            horizon = std::min(horizon, scalar_burst_boundaries_before(static_cast<std::uint64_t>(host_window_loop_iterations) + 1U, kHostWindowLoopIterationInterval * host_window_pump_scale()));
        }
        if (guest_input_replay != nullptr) {
            if (const auto limit = guest_input_replay->next_boundary_cycle_limit(); limit.has_value()) {
                horizon = std::min(horizon, scalar_burst_boundaries_before(first_cycle, *limit));
            }
        }
        return horizon;
    };
    const auto current_guest_input_timeline_boundary = [&]() {
        return runtime::GuestInputTimelineBoundary{committed_steps, emulated_ee_cycles};
    };
    std::array<bool, 3> host_movie_start_skip_latch{};
    std::optional<runtime::NativeFunctionPreflightStatus> native_gap_preflight;
    std::optional<takedown::guest::EeStepResult> compiled_execution_boundary;
    bool vehicle_world_contact_probe_bound_to_shadow = false;
    std::uint64_t scalar_observer_bypass_steps = 0U;
    std::uint64_t scalar_burst_audit_checks = 0U;
    std::chrono::steady_clock::time_point next_thumbnail_cache_poll{};
    while (committed_steps < maximum_steps) {

        if (native_model_live) native_model_live->throw_if_failed();
        if (native_simulation_shadow_stop_requested) {
            break;
        }
        if (guest_input_replay != nullptr) {
            const auto replay_advance =
                guest_input_replay->apply_instruction_boundary(current_guest_input_timeline_boundary());
            if (replay_advance.measurement_start) {
                if (!guest_work_measurement_window.has_value()) {
                    throw std::logic_error("B3IR measurement marker reached without a configured window");
                }
                const auto arm_result = guest_work_measurement_window->arm();
                if (arm_result != takedown::runtime::GuestWorkMeasurementArmResult::Armed) {
                    throw std::runtime_error("B3IR measurement marker could not arm the guest-work window");
                }
                guest_work_measurement_start_input_pending = true;
                if (variable_frame_clock.has_value()) {
                    variable_frame_clock->begin_window(
                        emulated_ee_cycles,
                        frame_time_script_lead.has_value() ? host_steady_now_nanoseconds() : 0U);
                    std::cout << "Variable frame rate window start: frames="
                              << variable_frame_clock->statistics().frames
                              << " ticks=" << variable_frame_rate->planner().statistics().ticks
                              << " split-frames=" << variable_frame_rate->planner().statistics().split_frames
                              << " game-clock-microseconds="
                              << static_cast<std::uint64_t>(variable_frame_rate->planner().game_clock_seconds() * 1.0e6)
                              << " dropped-microseconds="
                              << static_cast<std::uint64_t>(variable_frame_rate->planner().statistics().dropped_seconds * 1.0e6)
                              << " ee-cycles=" << emulated_ee_cycles << '\n';
                }
                std::cout << "Guest input timeline: A marker accepted at steps=" << committed_steps
                          << " cycles=" << emulated_ee_cycles
                          << "; starting measurement at this EE boundary\n"
                          << std::flush;
            }
            if (replay_advance.completion) {
                if (!guest_input_replay->poll_window() || !guest_work_measurement_window.has_value() ||
                    !guest_work_measurement_window->stop_at_instruction_boundary(committed_steps)) {
                    throw std::runtime_error("B3IR completion boundary arrived before the measurement step limit");
                }
                maximum_steps = committed_steps;
                break;
            }
        }
        if (guest_work_measurement_window.has_value() && guest_work_measurement_start_input_pending) {
            guest_work_measurement_start_input_pending = false;
            if (guest_input_recorder != nullptr) {
                guest_input_recorder->record_measurement_start(current_guest_input_timeline_boundary());
            }
            const auto start_result = guest_work_measurement_window->start_at_instruction_boundary(
                snapshot_guest_work_measurement(), maximum_steps);
            if (start_result == takedown::runtime::GuestWorkMeasurementStartResult::Started) {
                if (femain_terminal_diagnostic_guest_work_only && femain_terminal_diagnostic != nullptr) {
                    if (gs_renderer_worker.has_value()) {
                        static_cast<void>(gs_renderer_worker->synchronize());
                    }
                    femain_terminal_diagnostic->reset_for_guest_work();
                    if (femain_dynamic_callback_probe != nullptr) {
                        femain_dynamic_callback_probe->reset();
                    }
                    if (femain_frame_closure_diagnostic != nullptr) {
                        femain_frame_closure_diagnostic->reset();
                    }
                    if (femain_provenance_window != nullptr) {
                        static_cast<void>(femain_provenance_window->begin(
                            snapshot_femain_provenance_boundary(
                                *guest_work_measurement_window->start_snapshot()),
                            femain_terminal_diagnostic->observation_epoch()));
                    }
                }
                if (vu1_lower_plan_window_telemetry.has_value()) {
                    const auto stats = memory.vu1_program_cache().statistics();
                    auto& telemetry = *vu1_lower_plan_window_telemetry;
                    telemetry.native_start = {memory.vu1_activity().micro_pairs_committed,
                                              stats.native_scheduled_pairs,
                                              stats.native_transfer_blocks,
                                              stats.native_control_pairs,
                                              stats.native_control_fast_pairs};
                    telemetry.start_hits = stats.opcode_threaded_lower_stage_hits;
                    telemetry.start_fallbacks = stats.opcode_threaded_lower_stage_fallbacks;
                    telemetry.branch_start_hits = stats.branch_lower_plan_hits;
                    telemetry.branch_start_fallbacks = stats.branch_lower_plan_fallbacks;
                    telemetry.flag_read_start_hits = stats.flag_read_lower_plan_hits;
                    telemetry.flag_read_start_fallbacks = stats.flag_read_lower_plan_fallbacks;
                    telemetry.fmand_start_hits = stats.fmand_lower_plan_hits;
                    telemetry.fmand_start_fallbacks = stats.fmand_lower_plan_fallbacks;
                    telemetry.fcget_start_hits = stats.fcget_lower_plan_hits;
                    telemetry.fcget_start_fallbacks = stats.fcget_lower_plan_fallbacks;
                    telemetry.clip_predicate_start_hits = stats.clip_predicate_lower_plan_hits;
                    telemetry.clip_predicate_start_fallbacks = stats.clip_predicate_lower_plan_fallbacks;
                    telemetry.fcand_start_hits = stats.fcand_lower_plan_hits;
                    telemetry.fcand_start_fallbacks = stats.fcand_lower_plan_fallbacks;
                    telemetry.fcor_start_hits = stats.fcor_lower_plan_hits;
                    telemetry.fcor_start_fallbacks = stats.fcor_lower_plan_fallbacks;
                    telemetry.start_captured = true;
                }
                maximum_steps = *guest_work_measurement_window->stop_step();
                retail_timer_owner_abi_trace.begin_guest_work_collection(committed_steps, emulated_ee_cycles,
                                                                         maximum_steps);
                if (retail_timer_owner_abi_trace.enabled() &&
                    (!retail_timer_owner_abi_trace.collecting() ||
                     retail_timer_owner_abi_trace.summary().failure !=
                         takedown::runtime::RetailTimerOwnerAbiTraceFailure::None)) {
                    throw std::logic_error("retail timer owner ABI trace could not open at guest-work A");
                }
                if (retail_timer_consumer_trace.enabled() &&
                    !retail_timer_consumer_trace.begin_guest_work_collection(committed_steps, emulated_ee_cycles,
                                                                             maximum_steps)) {
                    throw std::logic_error("retail timer consumer trace could not open at guest-work A");
                }
                if (retail_timer_consumer_trace.enabled() && retail_timer_consumer_trace.collecting() &&
                    !retail_timer_consumer_observer_session.begin()) {
                    throw std::logic_error(
                        "retail timer consumer temporal write coverage could not open at guest-work A");
                }
                if (guest_input_replay != nullptr && !guest_input_replay->poll_window()) {
                    const auto replay_completion = guest_input_replay->completion_boundary();
                    if (replay_completion.committed_steps != maximum_steps) {
                        throw std::runtime_error("B3IR C boundary does not match the configured measurement stop");
                    }
                }
                if (profile_guest_work_window) {
                    start_guest_execution_profiles(committed_steps);
                    std::cout << "Profile scope: started at B3IR A steps=" << committed_steps
                              << " stop-step=" << maximum_steps << '\n'
                              << std::flush;
                }
                const auto& start = *guest_work_measurement_window->start_snapshot();
                std::cout << "Guest-work measurement: started scope=EE instruction-boundary fixed guest work "
                             "requested-steps="
                          << guest_work_measurement_window->guest_step_budget()
                          << " start={steps=" << start.committed_steps << " ee-cycles=" << start.ee_cycles
                          << " vblank-starts=" << start.vblank_starts << " audio-frames=" << start.audio_frames
                          << " audio-fnv1a64-cumulative=" << hex64(start.audio_fnv1a64)
                          << " monotonic-ns=" << start.monotonic_nanoseconds << "}"
                          << " stop-step=" << maximum_steps << '\n'
                          << std::flush;
            } else {
                std::cerr << "Guest-work measurement: start rejected: "
                          << takedown::runtime::to_string(start_result) << '\n';
                if (guest_input_timeline_enabled) {
                    throw std::runtime_error("guest input timeline measurement start was rejected");
                }
            }
        }
        if (vif_packet_ring_flush_claim_tracker.active() &&
            state.pc() != takedown::runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushEntryAddress) {
            const auto context = current_ui_execution_context();
            if (vif_packet_ring_flush_claim_tracker.requires_pre_instruction(context, state.pc())) {
                vif_packet_ring_flush_claim_tracker.observe_pre_instruction(state, memory, context);
            }
            if (vif_packet_ring_flush_claim_tracker.active() && state.pc() == 0xfffffff0U &&
                context.kernel_callback_active) {
                vif_packet_ring_flush_claim_tracker.retire_context(context);
            } else if (vif_packet_ring_flush_claim_tracker.active() && state.pc() == 0U &&
                       !context.kernel_callback_active) {
                vif_packet_ring_flush_claim_tracker.retire_thread(context.thread_id);
            }
        }

        if (host_frame_step_paused || host_window_capture_boundary_paused) {
            if (host_movie_player.active() && host_movie_clock_start_nanoseconds.has_value() &&
                !host_movie_clock_pause_start_nanoseconds.has_value()) {
                host_movie_clock_pause_start_nanoseconds = host_steady_now_nanoseconds();
            }
            if (ee_pc_sampler != nullptr && profile_timing_active) ee_pc_sampler->pause_host_timing();
            if (ee_host_phase_profiler != nullptr && profile_timing_active) ee_host_phase_profiler->pause_host_timing();
            if (!present_profiled_host_window(false)) {
                break;
            }
            next_host_window_step = host_window_pump_scale() * 4096U + committed_steps;
            host_window_loop_iterations = 0U;
            if (host_frame_step_paused || host_window_capture_boundary_paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ee_pc_sampler != nullptr && profile_timing_active) ee_pc_sampler->resume_host_timing();
            if (ee_host_phase_profiler != nullptr && profile_timing_active) ee_host_phase_profiler->resume_host_timing();
            if (host_movie_clock_pause_start_nanoseconds.has_value()) {
                const auto now_nanoseconds = host_steady_now_nanoseconds();
                if (*host_movie_clock_pause_start_nanoseconds <= now_nanoseconds) {
                    const auto pause_duration = now_nanoseconds - *host_movie_clock_pause_start_nanoseconds;
                    host_movie_clock_paused_nanoseconds =
                        host_movie_clock_paused_nanoseconds > std::numeric_limits<std::uint64_t>::max() - pause_duration
                            ? std::numeric_limits<std::uint64_t>::max()
                            : host_movie_clock_paused_nanoseconds + pause_duration;
                }
                host_movie_clock_pause_start_nanoseconds.reset();
            }
        }

        if (femain_live_transaction_enabled) {
            service_graphics_dmacs_until_with_femain(emulated_ee_cycles);
        } else {
            if (emulated_ee_cycles > vif1_dmac_serviced_ee_cycles) {
                advance_profiled_vif1_dmac(emulated_ee_cycles - vif1_dmac_serviced_ee_cycles);
                vif1_dmac_serviced_ee_cycles = emulated_ee_cycles;
            }
            if (emulated_ee_cycles > gif_dmac_serviced_ee_cycles) {
                advance_profiled_gif_dmac(emulated_ee_cycles - gif_dmac_serviced_ee_cycles);
                gif_dmac_serviced_ee_cycles = emulated_ee_cycles;
            }
            if (graphics_run_ahead && (memory.vif1_dmac_can_progress() || memory.gif_dmac_can_progress())) {
                ++graphics_run_ahead_calls;
                graphics_run_ahead_cycles += memory.run_graphics_ahead(314572800U);
            }
        }

        if (host_window != nullptr) {
            host_window_loop_iterations += host_window_pump_scale();
            if (committed_steps >= next_host_window_step ||
                host_window_loop_iterations >= (host_window_pump_scale() == 1U ? 4096U : 65536U)) {
                if (!present_profiled_host_window(false)) {
                    break;
                }
                next_host_window_step = host_window_pump_scale() * 4096U + committed_steps;
                host_window_loop_iterations = 0U;
            }
        }

        if (subframe_input_bridge.has_value()) {
            if (host_window != nullptr || parallel_gs_input_reader != nullptr) {
                subframe_input_bridge->observe(control_style_blocks_input ? platform::HostControllerState{}
                                                                          : current_host_controller_state(),
                                               emulated_ee_cycles);
            }
            if (subframe_input_bridge->pending() != 0U) {
                runtime::SubframeInputEvent subframe_event{};
                while (subframe_input_bridge->due(emulated_ee_cycles, subframe_event)) {
                    write_virtual_ds2_host_input(subframe_event.state);
                }
            }
        }

        spu2_renderer.pump_due(emulated_ee_cycles);
        pace_audio_master();

        const auto& current_rwa_activity = kernel.rwa_activity();
        const bool rwa_boot_activity_observed =
            current_rwa_activity.opcode_counts[0x32U] != 0U || current_rwa_activity.opcode_counts[0x14U] > 1U;
        if (!rwa_shadow_boot_done && rwa_iop_shadow.has_value() && rwa_shadow_system_bank_mask == 7U &&
            rwa_boot_activity_observed && current_rwa_activity.voice_snapshots_deferred_pending == 0U) {
            kernel.set_rwa_execution_observers([](std::span<const std::byte>) {},
                                               [](std::uint32_t, std::span<const std::byte>) {},
                                               [](std::uint32_t) -> std::optional<takedown::guest::EeRwaShadowResponse> { return {}; });
            std::fprintf(stderr, "[RWA-SHADOW] boot complete: system bank captured, shadow detached (native path owns gameplay)\n");
            rwa_shadow_boot_done = true;
        }

        if (rwa_staging_samples.size() <= 7U && memory.contains(5123304U, 8U)) {
            const auto pointer = memory.read_u32_le(5123304U);
            if (pointer != rwa_staging_last_pointer) {
                rwa_staging_last_pointer = pointer;
                rwa_staging_samples.emplace_back(RwaStagingSample{committed_steps, pointer, memory.read_u32_le(5123308U)});
            }
        }

        if (!rwa_first_op05_step_valid && kernel.rwa_activity().opcode_counts[0x05U] != 0U) {
            rwa_first_op05_step = committed_steps;
            rwa_first_op05_step_valid = true;
        }

        if (!scripted_input_presses.empty()) {
            const double now_seconds = static_cast<double>(emulated_ee_cycles) / 294912000.0;
            while (scripted_input_next < scripted_input_presses.size() &&
                   scripted_input_presses[scripted_input_next].at_seconds <= now_seconds) {
                const auto& press = scripted_input_presses[scripted_input_next++];
                write_virtual_ds2_host_input(press.state);
                std::fprintf(stderr, "[INPUT-SCRIPT] fired %.2fs\n", press.at_seconds);
            }
            if (scripted_input_next != 0U &&
                now_seconds - scripted_input_presses[scripted_input_next - 1U].at_seconds > 0.1) {
                write_virtual_ds2_host_input(platform::HostControllerState{});
            }
        }

        if (femain_park_pcs.has_value()) {
            if (memory.contains(32048164U, 4U)) {
                const auto phase = memory.read_u32_le(32048164U);
                if (phase == 5U) {
                    femain_park_pcs->observe(committed_steps, state.pc());
                }
                if (phase != 0U) {
                    femain_alt_phase = phase;
                    if (femain_alt_phase_pcs.has_value()) {
                        femain_alt_phase_pcs->observe(committed_steps, state.pc());
                    }
                }
            }
        }

        if (host_movie_playback || headless_movie_eos) {
            if (headless_movie_eos_traces.empty() || headless_movie_eos_traces.back().destroyed_step.has_value() ||
                headless_movie_eos_traces.back().paused_step.has_value() ||
                headless_movie_eos_traces.back().looping_background) {
                if (state.pc() == 3298544U && memory.read_u32_le(3298544U) == 666763104U &&
                    state.read_gpr_u32(4U) == 5351536U && state.read_gpr_u32(5U) == 5351872U &&
                    state.read_gpr_u32(8U) == 5169136U) {
                    const std::optional<std::string> filename =
                        read_bounded_guest_string(state.read_gpr_u32(8U), 128U);
                    if (filename.has_value() && is_retail_m2v(*filename)) {
                        const auto& activity = memory.ipu_activity();
                        HeadlessMovieEosTrace trace{
                            .start_step = committed_steps,
                            .looping_background = is_looping_frontend_background_m2v(*filename),
                            .start_skip_exempt = is_menu_background_m2v(*filename),
                            .host_playback = host_movie_playback,
                            .command_baseline = activity.command_writes,
                            .fdec_baseline = activity.command_opcode_counts[4U],
                            .to_ipu_baseline = activity.to_ipu_dma_starts,
                            .filename = *filename,
                        };
                        host_movie_start_skip_latch = {trace.start_skip_exempt, false, false};
                        if (host_movie_playback) {
                            host_movie_player.close();
                            const auto live_cancellation = host_femain_live_transaction.cancel_after_player_close(
                                runtime::Slus21050FemainFrameLedgerCancelReason::Abort);
                            if (live_cancellation.cancellation_witness.has_value()) {
                                static_cast<void>(host_femain_live_terminal_authority->cancel(*live_cancellation.cancellation_witness));
                                static_cast<void>(host_femain_live_terminal_authority->reset_after_cancel());
                            }
                            detach_femain_live_source_bridge();
                            reset_femain_retail_consume_observation(
                                live_cancellation.status == runtime::Slus21050FemainLiveTransactionStatus::Cancelled &&
                                        live_cancellation.cancellation_witness.has_value()
                                    ? &*live_cancellation.cancellation_witness
                                    : nullptr);
                            host_movie_path.reset();
                            host_movie_path_error.clear();
                            host_movie_clock_start_nanoseconds.reset();
                            host_movie_clock_pause_start_nanoseconds.reset();
                            host_movie_clock_paused_nanoseconds = 0U;
                            host_movie_clock_anchor_samples.reset();
                            host_movie_path = content::resolve_game_path(std::filesystem::path(argv[2]).parent_path(),
                                                                          &mod_overlay, *filename, host_movie_path_error);
                            if (host_movie_path.has_value()) {
                                static_cast<void>(host_movie_player.open(*host_movie_path, trace.looping_background));
                            }
                            trace.host_playback_opened = host_movie_player.active();
                            if (trace.host_playback_opened) {
                                host_femain_live_source_activation_pending = femain_live_transaction_enabled;
                                activate_femain_live_source_bridge();
                                ++host_movie_sessions;
                                std::cout << "Host movie playback: file=\"" << *filename << "\" path=";
                                if (host_movie_path.has_value()) {
                                    std::cout << '"' << host_movie_path->string() << '"';
                                } else if (host_movie_path_error) {
                                    std::cout << "unresolved:" << host_movie_path_error.message();
                                } else {
                                    std::cout << "unresolved";
                                }
                                std::cout << " mode=" << (trace.looping_background ? "loop" : "finite")
                                          << " status=" << to_string(host_movie_player.status()) << '\n';
                            }
                        }
                        headless_movie_eos_traces.emplace_back(std::move(trace));
                    }
                }
            }
        }

        if (!headless_movie_eos_traces.empty() && !headless_movie_eos_traces.back().destroyed_step.has_value() &&
            !headless_movie_eos_traces.back().accepted_step.has_value() && state.pc() == 1248936U &&
            memory.read_u32_le(1248936U) == 3864265228U && memory.read_u32_le(1248940U) == 604110871U &&
            memory.read_u32_le(1248944U) == 268500983U && memory.read_u32_le(1248948U) == 2923561720U &&
            state.read_gpr_u32(18U) == 5351080U && state.read_gpr_u32(2U) != 0U) {
            const auto& activity = memory.ipu_activity();
            auto& trace = headless_movie_eos_traces.back();
            trace.accepted_step = committed_steps;
            trace.accepted_fdec_count = activity.command_opcode_counts[4U];
            trace.accepted_to_ipu_count = activity.to_ipu_dma_starts;
        }

        if (!headless_movie_eos_traces.empty() && headless_movie_eos_traces.back().host_playback &&
            headless_movie_eos_traces.back().host_playback_opened &&
            headless_movie_eos_traces.back().accepted_step.has_value() &&
            !headless_movie_eos_traces.back().destroyed_step.has_value() && state.pc() == 3297328U &&
            memory.read_u32_le(3297328U) == 666763184U && memory.read_u32_le(3297356U) == 2451767374U &&
            memory.read_u32_le(3297360U) == 274726916U && memory.read_u32_le(3297368U) == 2451767370U &&
            memory.read_u32_le(3297372U) == 274727197U && memory.read_u32_le(3296960U) == 604176385U &&
            memory.read_u32_le(3296964U) == 65011720U && memory.read_u32_le(3296968U) == 2692939854U &&
            state.read_gpr_u32(4U) == 5351536U && memory.read_u32_le(5351840U) == 23U &&
            memory.read_u32_le(5351828U) == 23U && memory.read_u8(5351608U) == 1U &&
            memory.read_u8(5351609U) == 0U && memory.read_u8(5351610U) == 0U && memory.read_u32_le(5351644U) != 0U) {
            auto& trace = headless_movie_eos_traces.back();
            const bool movie_start_down =
                guest_input_recorder != nullptr
                    ? guest_input_recorder->retail_poll_controller_state().start
                    : (guest_input_replay != nullptr ? guest_input_replay->retail_poll_controller_state().start
                                                     : current_host_controller_state().start);
            if ((guest_input_recorder != nullptr
                     ? guest_input_recorder->format() == runtime::GuestInputTimelineFormat::B3IR2
                     : (guest_input_replay == nullptr ||
                        guest_input_replay->format() == runtime::GuestInputTimelineFormat::B3IR2)) &&
                !trace.start_skip_exempt && !trace.start_skip_requested_step.has_value() &&
                host_movie_player.active()) {
                const auto advance_start_skip_latch = [](std::array<bool, 3>& latch, const bool start_down) {
                    if (latch[0] || latch[2]) {
                        return false;
                    }
                    if (!start_down) {
                        latch[1] = true;
                        return false;
                    }
                    if (!latch[1]) {
                        return false;
                    }
                    latch[2] = true;
                    return true;
                };
                const bool released_before = host_movie_start_skip_latch[1];
                const bool start_skip_ready = advance_start_skip_latch(host_movie_start_skip_latch, movie_start_down);
                if (guest_input_timeline_enabled && !released_before && host_movie_start_skip_latch[1]) {
                    const auto authenticated_poll = guest_input_recorder != nullptr
                                                        ? guest_input_recorder->observed_poll_count()
                                                        : guest_input_replay->observed_poll_count();
                    std::cout << "Guest input timeline movie: ready session=" << headless_movie_eos_traces.size() - 1U
                              << " file=\"" << trace.filename << "\" step=" << committed_steps
                              << " input-poll=" << authenticated_poll << " start-released=yes\n"
                              << std::flush;
                }
                if (start_skip_ready) {
                    trace.start_skip_requested_step = committed_steps;
                    trace.start_skip_audio_frames = spu2_renderer.samples_rendered();
                    trace.start_skip_audio_non_silent_frames = spu2_renderer.non_silent_frames();
                    trace.start_skip_audio_fnv1a64 = spu2_renderer.fnv1a64();
                    trace.start_skip_key_ons = guest_spu2_link.key_ons_programmed();
                    trace.start_skip_active_voices = static_cast<std::uint64_t>(guest_spu2_link.active_voices());
                    if (guest_input_recorder != nullptr) {
                        trace.start_skip_input_poll = guest_input_recorder->observed_poll_count();
                    }
                    if (guest_input_replay != nullptr) {
                        trace.start_skip_input_poll = guest_input_replay->observed_poll_count();
                    }
                    if (guest_input_timeline_enabled) {
                        std::cout << "Guest input timeline movie: skipped session="
                                  << headless_movie_eos_traces.size() - 1U << " file=\"" << trace.filename
                                  << "\" step=" << committed_steps
                                  << " input-poll=" << *trace.start_skip_input_poll
                                  << " retail-poll-authenticated=yes\n"
                                  << std::flush;
                    }
                    host_movie_player.close();
                    const auto live_cancellation = host_femain_live_transaction.cancel_after_player_close(
                        runtime::Slus21050FemainFrameLedgerCancelReason::StartSkip);
                    if (live_cancellation.cancellation_witness.has_value()) {
                        static_cast<void>(host_femain_live_terminal_authority->cancel(*live_cancellation.cancellation_witness));
                        static_cast<void>(host_femain_live_terminal_authority->reset_after_cancel());
                    }
                    detach_femain_live_source_bridge();
                    reset_femain_retail_consume_observation(
                        live_cancellation.status == runtime::Slus21050FemainLiveTransactionStatus::Cancelled &&
                                live_cancellation.cancellation_witness.has_value()
                            ? &*live_cancellation.cancellation_witness
                            : nullptr);
                    host_movie_clock_start_nanoseconds.reset();
                    host_movie_clock_pause_start_nanoseconds.reset();
                    host_movie_clock_paused_nanoseconds = 0U;
                    host_movie_clock_anchor_samples.reset();
                }
            }
            if (memory.read_u8(5351614U) == 0U) {
                memory.write_u8(5351614U, 1U);
                ++trace.pause_assertions;
            }
            if (memory.read_u8(5351614U) == 1U && !trace.paused_step.has_value()) {
                trace.paused_step = committed_steps;
            }
            if (!trace.looping_background && !host_movie_player.active() && !trace.abort_requested_step.has_value()) {
                memory.write_u8(5351610U, 1U);
                trace.abort_requested_step = committed_steps;
            }
        }

        if (!headless_movie_eos_traces.empty() && headless_movie_eos_traces.back().looping_background &&
            headless_movie_eos_traces.back().accepted_step.has_value() &&
            !headless_movie_eos_traces.back().destroyed_step.has_value() && state.pc() == 3297328U &&
            memory.read_u32_le(3297328U) == 666763184U && memory.read_u32_le(3297356U) == 2451767374U &&
            memory.read_u32_le(3297360U) == 274726916U && memory.read_u32_le(3297368U) == 2451767370U &&
            memory.read_u32_le(3297372U) == 274727197U && memory.read_u32_le(3296960U) == 604176385U &&
            memory.read_u32_le(3296964U) == 65011720U && memory.read_u32_le(3296968U) == 2692939854U &&
            memory.read_u32_le(4936552U) == 4933152U && memory.read_u32_le(4937128U) == 34U &&
            memory.read_u32_le(4937656U) == 1U && memory.read_u32_le(4938184U) == 352U &&
            state.read_gpr_u32(4U) == 5351536U && memory.read_u32_le(5351844U) == 26U &&
            memory.read_u32_le(5351840U) == 23U && memory.read_u32_le(5351828U) == 23U &&
            memory.read_u8(5351608U) == 1U && memory.read_u8(5351609U) == 0U && memory.read_u8(5351610U) == 0U &&
            memory.read_u8(5351611U) == 1U && memory.read_u32_le(5351644U) != 0U &&
            memory.read_u32_le(5351632U) != 0U && memory.read_u32_le(5351636U) != 0U &&
            memory.read_u32_le(5351640U) != 0U) {
            static const int femain_background_run = []() {
                const char* const value = std::getenv("TAKEDOWN_FEMAIN_BACKGROUND_RUN");
                if (value == nullptr) {
                    return 0;
                }
                const unsigned char digit = static_cast<unsigned char>(*value);
                return digit >= '0' && digit <= '3' ? static_cast<int>(digit - '0') : 0;
            }();
            const bool background_released =
                femain_background_run == 2 ||
                (femain_background_run == 1 && virtual_ds2_trace.has_value() && virtual_ds2_trace->title_next_flow_step.has_value());
            auto& trace = headless_movie_eos_traces.back();
            if (femain_background_run != 0 && background_released) {
                if (memory.read_u8(5351614U) != 0U) {
                    memory.write_u8(5351614U, 0U);
                }
            } else if (femain_background_run != 2) {
                if (memory.read_u8(5351614U) == 0U) {
                    memory.write_u8(5351614U, 1U);
                    static_cast<void>(headless_femain_bridge.seed_guest_buffers(memory));
                    ++trace.pause_assertions;
                    if (!trace.paused_step.has_value()) {
                        trace.paused_step = committed_steps;
                    }
                }
            }
        }

        if (!headless_movie_eos_traces.empty() && !headless_movie_eos_traces.back().looping_background &&
            !headless_movie_eos_traces.back().destroyed_step.has_value() &&
            headless_movie_eos_traces.back().accepted_step.has_value() &&
            !headless_movie_eos_traces.back().abort_requested_step.has_value() && state.pc() == 1080716U &&
            memory.read_u32_le(1080716U) == 2382500000U && memory.read_u32_le(1080708U) == 273743876U &&
            memory.read_u32_le(1080720U) == 272695287U && memory.read_u32_le(1089516U) == 2892107936U &&
            memory.read_u32_le(3296976U) == 604176385U && memory.read_u32_le(3296984U) == 2692939850U) {
            const auto& activity = memory.ipu_activity();
            auto& trace = headless_movie_eos_traces.back();
            const std::uint32_t decoder = memory.read_u32_le(5351720U);
            if (!trace.abort_poll_step.has_value()) {
                trace.abort_poll_step = committed_steps;
                trace.abort_decoder = decoder;
                trace.abort_s0 = state.read_gpr_u32(16U);
                trace.abort_movie_state = memory.read_u32_le(5351840U);
                trace.abort_work_state = memory.read_u32_le(5351828U);
                trace.abort_stream = memory.read_u32_le(5351644U);
                trace.abort_flags = {memory.read_u8(5351608U), memory.read_u8(5351609U), memory.read_u8(5351610U)};
                trace.abort_fdec_count = activity.command_opcode_counts[4U];
                trace.abort_to_ipu_count = activity.to_ipu_dma_starts;
            }
            if (activity.command_writes > trace.command_baseline && activity.command_opcode_counts[4U] > trace.fdec_baseline &&
                activity.to_ipu_dma_starts > trace.to_ipu_baseline && decoder != 0U &&
                (!trace.host_playback || !host_movie_player.active()) && state.read_gpr_u32(16U) == decoder &&
                memory.contains(decoder + 4256U, 4U) && memory.read_u32_le(5351840U) == 23U &&
                memory.read_u32_le(5351828U) == 23U && memory.read_u8(5351608U) == 1U &&
                memory.read_u8(5351609U) == 0U && memory.read_u8(5351610U) == 0U &&
                memory.read_u32_le(5351644U) != 0U) {
                memory.write_u32_le(decoder + 4256U, 1U);
                memory.write_u8(5351610U, 1U);
                trace.abort_requested_step = committed_steps;
            }
        }

        if (!headless_movie_eos_traces.empty() && !headless_movie_eos_traces.back().destroyed_step.has_value() &&
            headless_movie_eos_traces.back().accepted_step.has_value() &&
            !headless_movie_eos_traces.back().stop_completed_step.has_value() && state.pc() == 0x00325124U &&
            memory.read_u32_le(0x00325124U) == 0xae20006cU && state.read_gpr_u32(17U) == 0x0051a870U) {
            headless_movie_eos_traces.back().stop_completed_step = committed_steps;
        }

        if (!headless_movie_eos_traces.empty() && headless_movie_eos_traces.back().stop_completed_step.has_value() &&
            !headless_movie_eos_traces.back().destroyed_step.has_value() && state.pc() == 0x00324f40U &&
            state.read_gpr_u32(4U) == 0x0051a870U) {
            auto& trace = headless_movie_eos_traces.back();
            trace.destroyed_step = committed_steps;
        }

        if (!post_intro_frontend_step.has_value() && !headless_movie_eos_traces.empty() &&
            headless_movie_eos_traces.back().looping_background &&
            headless_movie_eos_traces.back().paused_step.has_value() &&
            *headless_movie_eos_traces.back().paused_step < committed_steps && state.pc() == 0x0031cf30U &&
            memory.read_u32_le(0x0031cf30U) == 0x27bdff30U && memory.read_u32_le(0x01ea2974U) == 0x01ea2d50U &&
            memory.read_u32_le(0x01e90430U) == 0x01e97e1cU && memory.read_u32_le(0x01e97e1cU) == 0x004e0210U &&
            memory.read_u32_le(0x004e0218U) == 0x00438840U && memory.read_u32_le(0x01ea2d5cU) == 0x004e0220U &&
            memory.read_u32_le(0x0051a9a4U) == 26U && memory.read_u32_le(0x0051a9a0U) == 23U &&
            memory.read_u8(0x0051a8beU) == 1U) {
            post_intro_frontend_step = committed_steps;
        }

        if (host_controller_bridge && virtual_ds2_trace.has_value() && state.pc() == 0x00214aacU &&
            state.read_gpr_u32(16U) == 0x004f50c0U && state.read_gpr_u32(17U) == 0U) {
            const bool connected = host_window != nullptr && host_window->controller_connected(1U);
            const auto input = connected && !save_slot_menu.is_open() ? host_window->controller_state(1U)
                                                                      : platform::HostControllerState{};
            static_cast<void>(native_controllers.publish_second(memory, connected, make_virtual_ds2_host_sample(input)));
        }

        if (host_controller_bridge && state.pc() == kRetailControllerTransportPollReturn && state.read_gpr_u32(16U) == 0x004f50c0U &&
            memory.read_u32_le(kRetailControllerTransportPollReturn) == 0x92030260U && memory.read_u32_le(0x004f5340U) == 0x004ddff0U &&
            (virtual_ds2_trace.has_value() ||
             (memory.read_u32_le(0x01e90424U) == 5U && memory.read_u32_le(0x01ea2974U) == 0x01ea2db8U))) {
            if (!virtual_ds2_trace.has_value()) {
                virtual_ds2_trace = VirtualDs2Trace{.first_step = committed_steps};
            }
            ++virtual_ds2_trace->update_count;
            seed_virtual_ds2_profile();
            write_virtual_ds2_neutral_sample();
            if (parallel_gs_input_reader != nullptr) {
                parallel_gs_input_reader->poll();
            }
            if (guest_input_replay != nullptr) {
                guest_input_replay->observe_retail_poll(virtual_ds2_trace->update_count,
                                                        current_guest_input_timeline_boundary());
            }
            if (boot_volume.has_value()) {
                const bool boot_volume_applied_before = boot_volume->applied();
                if (const auto levels = boot_volume->poll(memory); levels.has_value()) {
                    const auto saved = pc_options.remember_game_audio_levels(levels->music, levels->effects);
                    if (saved.save_error.has_value()) {
                        std::cerr << "Game audio levels: " << *saved.save_error << '\n';
                    } else {
                        std::cout << "Game audio levels: remembered music " << levels->music << ", sfx "
                                  << levels->effects << '\n';
                    }
                }
                if (!boot_volume_applied_before && boot_volume->applied()) {
                    std::cout << "Game audio levels: music " << boot_volume->remembered()->music << ", sfx "
                              << boot_volume->remembered()->effects << " before a profile loads\n";
                }
            }
            platform::HostControllerState effective_retail_input{};
            bool effective_retail_input_available = false;
            if (host_window != nullptr || parallel_gs_input_reader != nullptr || guest_input_replay != nullptr) {
                if (save_slot_menu.is_open() || online_mod_prompt_active) {
                    write_virtual_ds2_neutral_sample();
                } else {
                    if (!control_style_blocks_input && subframe_input_bridge.has_value() &&
                        subframe_input_bridge->last_applied_state().has_value()) {
                        effective_retail_input = *subframe_input_bridge->last_applied_state();
                    } else {
                        effective_retail_input = current_host_controller_state();
                    }
                    // Escape is Back (Triangle) in menus and pauses everywhere else; it never quits.
                    if (guest_input_replay == nullptr && escape_key_held() &&
                        memory.read_u32_le(0x01e90424U) != 5U) {
                        effective_retail_input.triangle = false;
                        effective_retail_input.start = true;
                    }
                    std::optional<runtime::MenuMouseInput> mouse_input;
                    if (menu_mouse_script.has_value()) {
                        mouse_input = menu_mouse_script->at(virtual_ds2_trace->update_count);
                    } else if (parallel_gs_input_reader != nullptr && guest_input_replay == nullptr &&
                               guest_input_recorder == nullptr) {
                        const auto pointer = parallel_gs_input_reader->pointer();
                        mouse_input = runtime::MenuMouseInput{
                            pointer.x, pointer.y, pointer.inside, parallel_gs_input_reader->raw_input().mouse_buttons,
                            parallel_gs_input_reader->consume_wheel()};
                    }
                    // The controller layout question takes the input while it is open.
                    if (control_style_poll(virtual_ds2_trace->update_count, effective_retail_input, mouse_input)) {
                        mouse_input.reset();
                        effective_retail_input = {};
                    }
                    // The SETTINGS page takes the input itself while it is open; the
                    // game page then sees none.
                    const bool settings_page_input =
                        settings_page_poll(virtual_ds2_trace->update_count, effective_retail_input, mouse_input);
                    if (settings_page_input) {
                        mouse_input.reset();
                        effective_retail_input = {};
                    }
                    if (mouse_input.has_value()) {
                        const bool title_screen = is_menu_title_screen(memory, memory.read_u32_le(0x01e90424U),
                                                                       memory.read_u32_le(0x01e90430U),
                                                                       memory.read_u32_le(0x01ea2974U));
                        const auto screen = runtime::read_menu_mouse_screen(memory);
                        if (menu_mouse_script.has_value() && mouse_input->buttons != 0U) {
                            std::cerr << "Menu mouse script: poll " << virtual_ds2_trace->update_count << " buttons "
                                      << static_cast<unsigned>(mouse_input->buttons) << " title " << title_screen
                                      << " phase " << memory.read_u32_le(0x01e90424U) << std::hex << " flow "
                                      << memory.read_u32_le(0x01ea2974U) << " updater "
                                      << memory.read_u32_le(0x01e90430U) << std::dec << " lists "
                                      << screen.lists.size() << " footer " << screen.footer << '\n';
                        }
                        effective_retail_input = platform::merge_host_controller_states(
                            effective_retail_input, menu_mouse.poll(screen, *mouse_input, title_screen));
                    }
                    write_virtual_ds2_host_input(effective_retail_input);
                }
                effective_retail_input_available = true;
            }
            if (guest_input_recorder != nullptr) {
                if (!effective_retail_input_available) {
                    throw std::logic_error("B3IR recording reached a retail poll without an input source");
                }
                static_cast<void>(guest_input_recorder->observe_retail_poll(
                    virtual_ds2_trace->update_count, current_guest_input_timeline_boundary(), effective_retail_input));
            }
            if (headless_frontend_replay && virtual_ds2_trace->title_next_flow_step.has_value()) {
                if (!headless_frontend_replay_trace.has_value()) {
                    headless_frontend_replay_trace =
                        HeadlessFrontendReplayTrace{.start_step = committed_steps,
                                                    .start_update_count = virtual_ds2_trace->update_count};
                }
                const auto replay_input = kHeadlessFrontendReplaySchedule.input_for_update(
                    virtual_ds2_trace->update_count - headless_frontend_replay_trace->start_update_count);
                headless_frontend_replay_update_in_flight =
                    virtual_ds2_trace->update_count - headless_frontend_replay_trace->start_update_count;
                headless_frontend_replay_input_in_flight = replay_input;
                platform::HostControllerState input{};
                switch (replay_input) {
                    case runtime::FrontendReplayInput::DpadDown:
                    input.dpad_down = true;
                    if (!headless_frontend_replay_trace->first_down_step.has_value()) {
                        headless_frontend_replay_trace->first_down_step = committed_steps;
                    }
                    break;
                    case runtime::FrontendReplayInput::DpadUp:
                    input.dpad_up = true;
                    if (!headless_frontend_replay_trace->first_up_step.has_value()) {
                        headless_frontend_replay_trace->first_up_step = committed_steps;
                    }
                    break;
                    case runtime::FrontendReplayInput::Cross:
                    input.cross = true;
                    if (!headless_frontend_replay_trace->first_cross_step.has_value()) {
                        headless_frontend_replay_trace->first_cross_step = committed_steps;
                    }
                    headless_frontend_replay_trace->last_cross_step = committed_steps;
                    if (*headless_frontend_replay_update_in_flight == 0U ||
                        kHeadlessFrontendReplaySchedule.input_for_update(*headless_frontend_replay_update_in_flight - 1U) !=
                            runtime::FrontendReplayInput::Cross) {
                        ++headless_frontend_replay_trace->cross_pulse_count;
                    }
                    break;
                    default:
                    break;
                }
                write_virtual_ds2_host_input(input);
            }
            if (memory.read_u32_le(0x01ea2974U) == 0x01ea2db8U) {
                static_cast<void>(write_virtual_ds2_channel(19U, 0U));
            }
            for (std::size_t word = 0U; word < 4U; ++word) {
                virtual_ds2_trace->profile_words[word] = memory.read_u32_le(0x00676634U + static_cast<std::uint32_t>(word) * 4U);
            }
            const bool gate_registered = memory.read_u32_le(0x004f52c0U) == 0x004f5340U &&
                                         memory.read_u32_le(0x004f5424U) == 0x004f50c0U &&
                                         memory.read_u32_le(0x004f5428U) == 0U;
            if (gate_registered && !virtual_ds2_trace->registered_step.has_value()) {
                virtual_ds2_trace->registered_step = committed_steps;
            }
            memory.write_u32_le(0x004f52d0U, 4U);
            // The release object writes 0 here (EeMemory::write_u32_le.constprop.0 in its assembly).
            memory.write_u32_le(0x004f5318U, 0U);
            memory.write_u8(0x004f6065U, 1U);
            if (gate_registered && virtual_ds2_trace->armed_step.has_value() &&
                *virtual_ds2_trace->armed_step < committed_steps && virtual_ds2_gate_handshake.pulse_ready()) {
                if (write_virtual_ds2_channel(26U, 255U)) {
                    static_cast<void>(virtual_ds2_gate_handshake.begin_pulse());
                    if (!virtual_ds2_trace->pulse_step.has_value()) {
                        virtual_ds2_trace->pulse_step = committed_steps;
                    }
                    ++virtual_ds2_trace->pulse_attempt_count;
                    virtual_ds2_active_channel = {26U};
                    virtual_ds2_release_after_update = true;
                    virtual_ds2_title_pulse_in_flight = false;
                    virtual_ds2_release_value = 0U;
                }
            } else if (auto_start && gate_registered && virtual_ds2_trace->gate_accepted_step.has_value() &&
                       virtual_ds2_trace->title_armed_step.has_value() &&
                       !virtual_ds2_trace->title_pulse_step.has_value() && memory.read_u32_le(0x01e90424U) == 5U &&
                       memory.read_u32_le(0x01ea2974U) == 0x01ea2d50U && memory.read_u32_le(0x01e90430U) == 0x01e97e1cU &&
                       memory.read_u32_le(0x01e97e1cU) == 0x004e0210U && memory.read_u32_le(0x004e0218U) == 0x00438840U &&
                       memory.read_u32_le(0x00438898U) == 0x0c08b2d0U && memory.read_u16_le(kRetailControllerAcceptEdge) == 0U) {
                const std::uint32_t action_source = memory.read_u32_le(0x004f5e0cU);
                virtual_ds2_trace->title_action_source = action_source;
                virtual_ds2_trace->title_action_channel = 3U;
                if (action_source != 0U && memory.contains(action_source, 10U)) {
                    virtual_ds2_trace->title_action_source_active = memory.read_u8(action_source + 8U);
                    virtual_ds2_trace->title_action_source_mode = memory.read_u8(action_source + 9U);
                }
                const bool title_controls_ready =
                    memory.read_u32_le(0x006764c4U) == 1U && memory.read_u32_le(0x006764c8U) == 1U &&
                    memory.read_u32_le(0x006764ccU) == 0U && memory.read_u32_le(0x006764d0U) == 3U;
                if (action_source == 0x004f5340U && virtual_ds2_trace->title_action_source_active != 0U &&
                    title_controls_ready && write_virtual_ds2_channel(3U, 0U)) {
                    virtual_ds2_trace->title_pulse_step = committed_steps;
                    virtual_ds2_active_channel = {3U};
                    virtual_ds2_release_after_update = true;
                    virtual_ds2_title_pulse_in_flight = true;
                    virtual_ds2_release_value = 1U;
                }
            }
            if (!virtual_ds2_release_after_update && host_window == nullptr &&
                memory.read_u32_le(0x01ea2974U) == 0x01ea2db8U) {
                static_cast<void>(write_virtual_ds2_channel(26U, 0U));
            }
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_release_after_update && state.pc() == 0x00214064U) {
            if (virtual_ds2_title_pulse_in_flight) {
                virtual_ds2_trace->title_decoded_step = committed_steps;
                virtual_ds2_trace->title_decoded_action_bits = memory.read_u32_le(0x004f53a8U);
                virtual_ds2_trace->title_action_previous = memory.read_u8(0x004f5352U);
                virtual_ds2_trace->title_action_current = memory.read_u8(0x004f536eU);
                if (virtual_ds2_trace->title_action_previous == 0U && virtual_ds2_trace->title_action_current != 0U &&
                    !virtual_ds2_trace->title_edge_step.has_value()) {
                    virtual_ds2_trace->title_edge_step = committed_steps;
                }
            } else {
                if (!virtual_ds2_trace->decoded_step.has_value()) {
                    virtual_ds2_trace->decoded_step = committed_steps;
                    virtual_ds2_trace->decoded_gate_bits = memory.read_u32_le(0x004f5390U);
                }
                static_cast<void>(virtual_ds2_gate_handshake.finish_pulse_update());
            }
            if (virtual_ds2_active_channel.has_value()) {
                static_cast<void>(write_virtual_ds2_channel(*virtual_ds2_active_channel, virtual_ds2_release_value));
            }
            virtual_ds2_active_channel.reset();
            virtual_ds2_release_after_update = false;
            virtual_ds2_title_pulse_in_flight = false;
        }

        if (headless_frontend_replay_trace.has_value() && headless_frontend_replay_update_in_flight.has_value() &&
            state.pc() == 0x00214294U) {
            auto& replay_trace = *headless_frontend_replay_trace;
            if (replay_trace.manager_update_count != 0U &&
                replay_trace.manager_updates[replay_trace.manager_update_count - 1U].replay_update ==
                    *headless_frontend_replay_update_in_flight) {
                // One record per frontend update.
            } else if (replay_trace.manager_update_count < replay_trace.manager_updates.size()) {
                const std::array<std::uint32_t, 15> kActionOffsets{20U,  32U,  44U,  56U,  68U,  92U,  140U, 152U,
                                                                    164U, 176U, 212U, 224U, 248U, 260U, 272U};
                auto& update = replay_trace.manager_updates[replay_trace.manager_update_count++];
                update.step = committed_steps;
                update.replay_update = *headless_frontend_replay_update_in_flight;
                update.input = headless_frontend_replay_input_in_flight;
                update.decoded = {memory.read_u8(0x004f52d8U), memory.read_u8(0x004f52d9U), memory.read_u8(0x004f52e1U),
                                  memory.read_u8(0x004f52e4U)};
                update.action_device = memory.read_u32_le(0x004f5e0cU);
                update.fallback_device = memory.read_u32_le(0x004f5e10U);
                if (update.action_device != 0U && memory.contains(update.action_device, 10U)) {
                    update.device_flags = {memory.read_u8(update.action_device + 8U),
                                           memory.read_u8(update.action_device + 9U)};
                }
                if (update.fallback_device != 0U && memory.contains(update.fallback_device, 10U)) {
                    update.fallback_device_flags = {memory.read_u8(update.fallback_device + 8U),
                                                    memory.read_u8(update.fallback_device + 9U)};
                }
                update.confirm_logical_action = static_cast<std::int32_t>(memory.read_u32_le(0x004b9bd0U));
                if (static_cast<std::uint32_t>(update.confirm_logical_action) <= 27U) {
                    const std::uint32_t logical_action = static_cast<std::uint32_t>(update.confirm_logical_action);
                    update.confirm_mapped_channels = {
                        static_cast<std::int32_t>(memory.read_u32_le((logical_action + 0x00121ef4U) * 4U)),
                        static_cast<std::int32_t>(memory.read_u32_le((logical_action + 0x00121ed8U) * 4U))};
                    update.confirm_fallback_channel = update.fallback_device_flags[1] == 0U
                                                          ? update.confirm_mapped_channels[0]
                                                          : update.confirm_mapped_channels[1];
                    if (update.action_device != 0U) {
                        const std::uint32_t value_address = update.action_device + logical_action * 4U + 72U;
                        const std::uint32_t previous_address = update.action_device + logical_action + 10U;
                        const std::uint32_t current_address = update.action_device + logical_action + 38U;
                        if (memory.contains(value_address, 4U) && memory.contains(previous_address, 1U) &&
                            memory.contains(current_address, 1U)) {
                            update.confirm_direct_value = memory.read_u32_le(value_address);
                            update.confirm_direct_edge_state = {memory.read_u8(previous_address),
                                                                memory.read_u8(current_address)};
                        }
                    }
                }
                if (update.fallback_device != 0U && update.confirm_fallback_channel >= 0) {
                    const auto channel = static_cast<std::uint32_t>(update.confirm_fallback_channel);
                    const std::uint32_t value_address = update.fallback_device + channel * 4U + 80U;
                    const std::uint32_t previous_address = update.fallback_device + channel + 10U;
                    const std::uint32_t current_address = update.fallback_device + channel + 45U;
                    if (memory.contains(value_address, 4U) && memory.contains(previous_address, 1U) &&
                        memory.contains(current_address, 1U)) {
                        update.confirm_fallback_value = memory.read_u32_le(value_address);
                        update.confirm_fallback_edge_state = {memory.read_u8(previous_address),
                                                              memory.read_u8(current_address)};
                    }
                }
                for (std::size_t action = 0U; action < kActionOffsets.size(); ++action) {
                    update.actions[action] = memory.read_u16_le(kActionOffsets[action] + 0x004f5e08U);
                }
                update.accept_edge = memory.read_u16_le(kRetailControllerAcceptEdge);
                update.flow = memory.read_u32_le(0x01ea2974U);
                update.updater = memory.read_u32_le(0x01e90430U);
                if (update.updater != 0U && memory.contains(update.updater, 11U)) {
                    update.updater_state = memory.read_u32_le(update.updater + 4U);
                    update.updater_flags = {memory.read_u8(update.updater + 8U), memory.read_u8(update.updater + 9U),
                                            memory.read_u8(update.updater + 10U)};
                }
                update.menu_fields = {memory.read_u32_le(0x01e8f368U), memory.read_u32_le(0x01e8f36cU),
                                      memory.read_u32_le(0x01e8f3c8U), memory.read_u32_le(0x01e8f3ccU),
                                      memory.read_u32_le(0x01e8f3e8U), memory.read_u32_le(0x01e8f3f0U)};
            }
        }

        if (headless_frontend_replay_trace.has_value() && headless_frontend_replay_update_in_flight.has_value() &&
            state.pc() == 0x0042e608U && headless_frontend_replay_trace->menu_poll_count < 192U) {
            auto& poll = headless_frontend_replay_trace->menu_polls[headless_frontend_replay_trace->menu_poll_count++];
            poll.step = committed_steps;
            poll.replay_update = *headless_frontend_replay_update_in_flight;
            poll.input = headless_frontend_replay_input_in_flight;
            poll.result = state.read_gpr_u32(2U);
            poll.fields = {memory.read_u32_le(0x01e8f368U), memory.read_u32_le(0x01e8f36cU), memory.read_u32_le(0x01e8f3c8U),
                           memory.read_u32_le(0x01e8f3ccU), memory.read_u32_le(0x01e8f3e8U), memory.read_u32_le(0x01e8f3f0U)};
        }

        if (virtual_ds2_trace.has_value() && !virtual_ds2_trace->action_edge_step.has_value() &&
            memory.read_u16_le(kRetailControllerAcceptEdge) != 0U) {
            virtual_ds2_trace->action_edge_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->action_edge_step.has_value() &&
            !virtual_ds2_trace->action_edge_clear_step.has_value() &&
            committed_steps > *virtual_ds2_trace->action_edge_step && memory.read_u16_le(kRetailControllerAcceptEdge) == 0U) {
            virtual_ds2_trace->action_edge_clear_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && state.pc() == 0x0022cfe0U && state.read_gpr_u32(31U) == 0x00449250U) {
            virtual_ds2_edge_poll_in_flight.reset();
            virtual_ds2_edge_poll_sample_in_flight = memory.read_u16_le(kRetailControllerAcceptEdge);
            if (virtual_ds2_trace->edge_poll_count < virtual_ds2_trace->edge_polls.size()) {
                virtual_ds2_edge_poll_in_flight = virtual_ds2_trace->edge_poll_count++;
                auto& poll = virtual_ds2_trace->edge_polls[*virtual_ds2_edge_poll_in_flight];
                poll.step = committed_steps;
                poll.selector = state.read_gpr_u32(5U);
                poll.accept_edge = *virtual_ds2_edge_poll_sample_in_flight;
            }
        }

        if (headless_frontend_replay_trace.has_value() && headless_frontend_replay_update_in_flight.has_value() &&
            state.pc() == 0x0022cfe0U && state.read_gpr_u32(31U) == 0x00449250U &&
            headless_frontend_replay_trace->edge_poll_count < 192U) {
            headless_frontend_replay_edge_poll_in_flight = headless_frontend_replay_trace->edge_poll_count++;
            auto& poll = headless_frontend_replay_trace->edge_polls[*headless_frontend_replay_edge_poll_in_flight];
            poll.step = committed_steps;
            poll.replay_update = *headless_frontend_replay_update_in_flight;
            poll.input = headless_frontend_replay_input_in_flight;
            poll.selector = state.read_gpr_u32(5U);
            poll.accept_edge = memory.read_u16_le(kRetailControllerAcceptEdge);
        }

        if (virtual_ds2_trace.has_value() && state.pc() == 0x0022cffcU && virtual_ds2_edge_poll_in_flight.has_value()) {
            auto& poll = virtual_ds2_trace->edge_polls[*virtual_ds2_edge_poll_in_flight];
            poll.action_blocks[0] = memory.read_u32_le(state.read_gpr_u32(29U) + 40U);
            poll.action_blocks[1] = memory.read_u32_le(state.read_gpr_u32(29U) + 44U);
        }

        if (headless_frontend_replay_trace.has_value() && state.pc() == 0x0022cffcU &&
            headless_frontend_replay_edge_poll_in_flight.has_value()) {
            auto& poll = headless_frontend_replay_trace->edge_polls[*headless_frontend_replay_edge_poll_in_flight];
            poll.action_blocks[0] = memory.read_u32_le(state.read_gpr_u32(29U) + 40U);
            poll.action_blocks[1] = memory.read_u32_le(state.read_gpr_u32(29U) + 44U);
        }

        if (virtual_ds2_trace.has_value() && state.pc() == 0x00449250U) {
            const std::uint32_t result = state.read_gpr_u32(2U);
            if (virtual_ds2_edge_poll_in_flight.has_value()) {
                virtual_ds2_trace->edge_polls[*virtual_ds2_edge_poll_in_flight].result = result;
            }
            const bool gate_registered = memory.read_u32_le(0x004f52c0U) == 0x004f5340U &&
                                         memory.read_u32_le(0x004f5424U) == 0x004f50c0U &&
                                         memory.read_u32_le(0x004f5428U) == 0U;
            if (virtual_ds2_edge_poll_sample_in_flight.has_value()) {
                if (virtual_ds2_gate_handshake.observe_gate_poll(
                        gate_registered, *virtual_ds2_edge_poll_sample_in_flight != 0U, result != 0U) &&
                    !virtual_ds2_trace->armed_step.has_value()) {
                    virtual_ds2_trace->armed_step = committed_steps;
                }
            }
            if (result != 0U && !virtual_ds2_trace->gate_accepted_step.has_value()) {
                virtual_ds2_trace->gate_accepted_step = committed_steps;
            }
            virtual_ds2_edge_poll_in_flight.reset();
            virtual_ds2_edge_poll_sample_in_flight.reset();
        }

        if (headless_frontend_replay_trace.has_value() && state.pc() == 0x00449250U &&
            headless_frontend_replay_edge_poll_in_flight.has_value()) {
            headless_frontend_replay_trace->edge_polls[*headless_frontend_replay_edge_poll_in_flight].result =
                state.read_gpr_u32(2U);
            headless_frontend_replay_edge_poll_in_flight.reset();
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->pulse_step.has_value() && state.pc() == 0x00437370U &&
            virtual_ds2_trace->frontend_event_count < virtual_ds2_trace->frontend_events.size()) {
            virtual_ds2_trace->frontend_events[virtual_ds2_trace->frontend_event_count++] = {
                committed_steps, state.read_gpr_u32(5U), state.read_gpr_u32(7U)};
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->gate_accepted_step.has_value() &&
            !virtual_ds2_trace->title_candidate_step.has_value() && state.pc() == 0x00438898U &&
            memory.read_u32_le(0x00438898U) == 0x0c08b2d0U && memory.read_u32_le(0x0043889cU) == 0x2406ffffU &&
            state.read_gpr_u32(4U) == 0x004f50c0U && state.read_gpr_u32(5U) == 1U &&
            state.read_gpr_u32(16U) == 0x01e97e1cU && memory.read_u32_le(0x01e90424U) == 5U &&
            memory.read_u32_le(0x01ea2974U) == 0x01ea2d50U && memory.read_u32_le(0x01e90430U) == 0x01e97e1cU &&
            virtual_ds2_trace->action_edge_clear_step.has_value() && memory.read_u16_le(kRetailControllerAcceptEdge) == 0U) {
            virtual_ds2_trace->title_candidate_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_candidate_step.has_value() &&
            !virtual_ds2_trace->title_armed_step.has_value() && state.pc() == 0x004388a0U &&
            memory.read_u32_le(0x004388a0U) == 0x10400016U && state.read_gpr_u32(2U) == 0U) {
            const float elapsed = std::bit_cast<float>(memory.read_u32_le(0x004f50a0U));
            const float limit = std::bit_cast<float>(memory.read_u32_le(0x01e97e30U));
            if (std::isfinite(elapsed) && std::isfinite(limit) && limit + 2.0F < elapsed) {
                virtual_ds2_trace->title_armed_step = committed_steps;
            }
        }

        if (virtual_ds2_trace.has_value() &&
            (virtual_ds2_trace->title_pulse_step.has_value() ||
             (!auto_start && virtual_ds2_trace->title_armed_step.has_value())) &&
            !virtual_ds2_trace->title_poll_step.has_value() && state.pc() == 0x00438898U &&
            memory.read_u32_le(0x00438898U) == 0x0c08b2d0U && state.read_gpr_u32(4U) == 0x004f50c0U &&
            state.read_gpr_u32(5U) == 1U) {
            virtual_ds2_trace->title_poll_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_poll_step.has_value() &&
            !virtual_ds2_trace->title_result_step.has_value() && state.pc() == 0x004388a0U &&
            memory.read_u32_le(0x004388a0U) == 0x10400016U && (auto_start || state.read_gpr_u32(2U) != 0U)) {
            virtual_ds2_trace->title_result_step = committed_steps;
            virtual_ds2_trace->title_poll_result = state.read_gpr_u32(2U);
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_result_step.has_value() &&
            virtual_ds2_trace->title_poll_result != 0U && !virtual_ds2_trace->title_accepted_step.has_value() &&
            state.pc() == 0x004388ccU && memory.read_u32_le(0x004388ccU) == 0x3c0401e9U) {
            virtual_ds2_trace->title_accepted_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_accepted_step.has_value() &&
            !virtual_ds2_trace->title_queued_step.has_value() && state.pc() == kRetailMovieDecoderUpdate &&
            memory.read_u32_le(kRetailMovieDecoderUpdate) == 0x27bdfff0U && state.read_gpr_u32(31U) == 0x004388e0U &&
            state.read_gpr_u32(4U) == 0x01e90240U && state.read_gpr_u32(5U) == 1U && state.read_gpr_u32(6U) == 0U) {
            virtual_ds2_trace->title_queued_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_queued_step.has_value() &&
            !virtual_ds2_trace->title_event_step.has_value() && state.pc() == 0x004389b0U &&
            memory.read_u32_le(0x004389b0U) == 0x27bdffe0U && state.read_gpr_u32(4U) == 0x01ea2d50U &&
            state.read_gpr_u32(5U) == 5U && state.read_gpr_u32(7U) == 1U) {
            virtual_ds2_trace->title_event_step = committed_steps;
        }

        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_event_step.has_value() &&
            !virtual_ds2_trace->title_next_flow_step.has_value() && state.pc() == 0x00438becU &&
            memory.read_u32_le(0x00438becU) == 0xc4800020U && memory.read_u32_le(0x004c4b30U) == 0x35b0f871U &&
            memory.read_u32_le(0x004c4b34U) == 0x94413fc0U && memory.read_u32_le(0x004c4b38U) == 160U &&
            memory.read_u32_le(0x01ea2974U) == 0x01ea2a10U && state.read_gpr_u32(16U) == 0x01ea2a10U) {
            virtual_ds2_trace->title_next_flow_step = committed_steps;
        }

        if (retail_trace) {
            for (auto& milestone : retail_pc_milestones) {
                if (milestone.address != state.pc()) {
                    continue;
                }
                if (milestone.hits == 0U) {
                    milestone.first_step = committed_steps;
                }
                ++milestone.hits;
                milestone.last_step = committed_steps;
            }
            if (state.pc() == 0x002a5dfcU && eatrax_child_query_trace.size() * sizeof(EATraxChildQueryTrace) <= 744U) {
                const std::uint32_t node = state.read_gpr_u32(18U);
                eatrax_child_query_trace.push_back(EATraxChildQueryTrace{
                    committed_steps, node, node >= 68U ? node - 68U : 0U,
                    node > 3U && memory.contains(node - 4U, 4U) ? memory.read_u32_le(node - 4U) : 0U,
                    state.read_gpr_u32(2U)});
            }
            if ((state.pc() == 0x003a2100U || state.pc() == 0x0034a4e0U || state.pc() == 0x0034bc30U) &&
                retail_event_trace.size() * sizeof(RetailEventTrace) <= 992U) {
                retail_event_trace.push_back(RetailEventTrace{
                    committed_steps, state.pc(), state.read_gpr_u32(4U), state.read_gpr_u32(5U),
                    state.read_gpr_u32(6U), state.read_gpr_u32(7U), state.read_gpr_u32(31U)});
            }
        }

        if (state.pc() == 0x001f2518U && memory.read_u32_le(0x001f2518U) == 0x1600fff1U && state.read_gpr_u32(16U) != 0U &&
            memory.read_u32_le(0x004e2838U) != memory.read_u32_le(0x004e283cU) && memory.read_u8(0x004e2830U) == 0U &&
            !timed_graphics_scheduler_work_pending() && (memory.intc_status() & 4U) == 0U &&
            emulated_ee_cycles < next_vblank_start) {
            if (variable_frame_clock.has_value()) {
                variable_frame_clock->set_minimum_frame_cycles(variable_frame_limit_cycles());
                const std::uint64_t retail_frame_cycles = 294912000U / variable_frame_rate->planner().retail_rate();
                const std::uint64_t target = variable_frame_clock->frame_target(
                    emulated_ee_cycles, variable_frame_clock->live() ? host_steady_now_nanoseconds() : 0U,
                    retail_frame_cycles);
                variable_frame_target = target;
                next_vblank_start = target;
                std::uint64_t until = target;
                if (!kernel.guest_callback_active()) {
                    const auto deadline = kernel.next_alarm_deadline();
                    if (deadline.has_value() && *deadline > emulated_ee_cycles) {
                        until = std::min(*deadline, until);
                    }
                }
                const auto deadline = kernel.next_sif_dma_completion_deadline();
                if (deadline.has_value() && *deadline > emulated_ee_cycles) {
                    until = std::min(*deadline, until);
                }
                emulated_ee_cycles = until;
            } else {
                emulated_ee_cycles = next_vblank_start;
            }
            ++fast_forwarded_graphics_drain_waits;
        }

        if ((state.pc() == kRetailVblankPollBranch && memory.read_u32_le(kRetailVblankPollBranch) == 0x1040fffaU) ||
            (state.pc() == kRetailVblankCallbackPollBranch && memory.read_u32_le(0x0010c398U) == 0x14400004U &&
             memory.read_u32_le(0x0010c3a0U) == 0x8fa20000U && memory.read_u32_le(kRetailVblankCallbackPollBranch) == 0x1040fffaU &&
             state.read_gpr_u32(2U) == 0U)) {
            if ((memory.intc_status() & 4U) == 0U && !timed_graphics_scheduler_work_pending() &&
                emulated_ee_cycles < next_vblank_start) {
                emulated_ee_cycles = next_vblank_start;
                ++fast_forwarded_vblank_waits;
            }
        }

        bool reached_host_frame_step_boundary = false;
        bool reached_host_capture_boundary = false;
        bool variable_frame_busy_vblank = false;
        while (emulated_ee_cycles >= next_vblank_start) {
            if (variable_frame_clock.has_value()) {
                if (variable_frame_target.has_value() && next_vblank_start >= *variable_frame_target) {
                    const auto deadline = variable_frame_clock->host_deadline_nanoseconds(next_vblank_start);
                    if (deadline.has_value()) {
                        const std::uint64_t deadline_nanoseconds = *deadline;
                        wait_for_host_time(deadline_nanoseconds, platform::HostWaitSite::FramePacingDeadline);
                    }
                    variable_frame_clock->note_frame_boundary(next_vblank_start);
                    variable_frame_target.reset();
                } else {
                    variable_frame_busy_vblank = true;
                }
                variable_frame_rate->on_vblank(memory);
            }

            if (femain_live_transaction_enabled) {
                service_graphics_dmacs_until_with_femain(next_vblank_start);
            } else {
                if (next_vblank_start > vif1_dmac_serviced_ee_cycles) {
                    advance_profiled_vif1_dmac(next_vblank_start - vif1_dmac_serviced_ee_cycles);
                    vif1_dmac_serviced_ee_cycles = next_vblank_start;
                }
                if (next_vblank_start > gif_dmac_serviced_ee_cycles) {
                    advance_profiled_gif_dmac(next_vblank_start - gif_dmac_serviced_ee_cycles);
                    gif_dmac_serviced_ee_cycles = next_vblank_start;
                }
            }

            if (!headless_movie_eos_traces.empty() && headless_movie_eos_traces.back().host_playback &&
                headless_movie_eos_traces.back().host_playback_opened &&
                headless_movie_eos_traces.back().accepted_step.has_value() &&
                headless_movie_eos_traces.back().paused_step.has_value() &&
                !headless_movie_eos_traces.back().abort_requested_step.has_value() &&
                !headless_movie_eos_traces.back().destroyed_step.has_value() && memory.read_u8(0x0051a8baU) == 0U &&
                host_movie_player.active()) {
                const bool host_movie_clock_running = host_movie_clock_start_nanoseconds.has_value();
                const auto before = host_movie_player.statistics();
                const auto live_transaction = host_femain_live_transaction.step(
                    memory, state, host_movie_clock_running ? host_movie_media_elapsed_nanoseconds() : 0U,
                {raised_vblank_starts, next_vblank_start / 6144U + 1U},
                {{raised_vblank_starts, next_vblank_start / 6144U + 1U},
                    {raised_vblank_starts, next_vblank_start / 6144U + 1U}});
                bool live_source_publication_failed = false;
                if (live_transaction.published() &&
                    (host_femain_live_source_producer == nullptr || !live_transaction.transaction.has_value() ||
                     !live_transaction.transaction->publication_packet.has_value())) {
                    live_source_publication_failed = true;
                }
                if (live_transaction.published() && !live_source_publication_failed) {
                    const runtime::Slus21050FemainMoviePublicationPacket packet =
                        *live_transaction.transaction->publication_packet;
                    const runtime::Slus21050FemainLiveSourceCandidateResolver resolver(true);
                    const auto candidate = resolver.resolve(memory, packet);
                    if (candidate.resolved()) {
                        const std::uint64_t session_generation = host_movie_player.session_generation();
                        const runtime::Slus21050FemainSerializedExecutionIdentity execution{
                            host_femain_emulation_thread_identity, session_generation, session_generation};
                        live_source_publication_failed = !host_femain_live_source_producer->bind_publication(
                            *host_femain_live_terminal_authority, host_femain_live_transaction.owner(),
                            host_femain_live_transaction.ledger(), host_movie_player, memory, packet, execution,
                            raised_vblank_starts + 1U, host_movie_clock_running ? host_movie_media_elapsed_nanoseconds() : 0U,
                            session_generation, *candidate.candidate);
                        if (!live_source_publication_failed && host_femain_live_terminal_coordinator == nullptr) {
                            live_source_publication_failed = true;
                        }
                        if (live_source_publication_failed ||
                            !host_femain_live_terminal_coordinator->arm_for_source(*candidate.candidate)) {
                            live_source_publication_failed = true;
                        }
                        if (live_source_publication_failed || !memory.gs_capture().gif_observation_quiescent()) {
                            live_source_publication_failed = true;
                        }
                        if (!live_source_publication_failed) {
                            host_femain_live_terminal_coordinator->attach(memory.gs_capture(),
                                                                          *host_femain_live_source_producer);
                            host_femain_live_terminal_coordinator_attached = true;
                            if (femain_parallel_gs_completion_ledger != nullptr) {
                                pending_femain_parallel_gs_correlation =
                                    PendingFemainParallelGsCorrelation{packet, *candidate.candidate};
                            }
                        }
                    } else {
                        live_source_publication_failed = true;
                    }
                }
                if (live_source_publication_failed) {
                    host_femain_live_source_failure_pending = true;
                    abort_failed_femain_live_source_bridge();
                }
                if (live_transaction.published() && !live_source_publication_failed &&
                    femain_retail_consume_invocation_barrier != nullptr) {
                    if (femain_retail_consume_invocation_barrier->phase() ==
                            runtime::Slus21050FemainRetailConsumeInvocationBarrier::Phase::Complete &&
                        femain_retail_consume_observation_adapter->attachment() ==
                            runtime::Slus21050FemainRetailConsumeAttachment::Complete) {
                        persist_femain_retail_consume_observation();
                        femain_retail_consume_observation_adapter->reset();
                        femain_retail_consume_invocation_barrier->reset();
                    }
                    const std::optional<runtime::Slus21050FemainMoviePublicationPacket> packet =
                        live_transaction.transaction.has_value() ? live_transaction.transaction->publication_packet
                                                                 : std::nullopt;
                    const std::uint64_t observation_epoch =
                        femain_terminal_diagnostic != nullptr ? femain_terminal_diagnostic->observation_epoch() : 0U;
                    if (!packet.has_value() ||
                        !femain_retail_consume_invocation_barrier->arm(*packet, observation_epoch)) {
                        femain_retail_consume_invocation_barrier->reject(
                            runtime::Slus21050FemainRetailConsumeInvocationBarrierFailure::AdapterFailed);
                        femain_retail_consume_observation_adapter->note_live_binding_missing();
                    }
                }
                if (live_transaction.use_legacy_path()) {
                    static_cast<void>(host_movie_player.step_at_media_time_with_guest_boundary(
                        memory, host_movie_clock_running ? host_movie_media_elapsed_nanoseconds() : 0U,
                    {raised_vblank_starts, next_vblank_start / 6144U + 1U}));
                }
                const auto& after = host_movie_player.statistics();
                if (!host_movie_clock_running && after.frames_staged > before.frames_staged) {
                    host_movie_clock_start_nanoseconds = host_steady_now_nanoseconds();
                    host_movie_clock_paused_nanoseconds = 0U;
                    host_movie_clock_pause_start_nanoseconds.reset();
                    host_movie_clock_anchor_samples = spu2_renderer.samples_rendered();
                }
                if (femain_background_run && after.staged_buffers > before.staged_buffers) {
                    const std::uint32_t staged = memory.read_u32_le(0x0051a88cU);
                    memory.write_u32_le(0x004e2838U, staged);
                    memory.write_u32_le(0x004e283cU, staged);
                    memory.write_u8(0x004e2830U, 0U);
                    memory.write_u8(0x004e2804U, static_cast<std::uint8_t>(memory.read_u8(0x004e2804U) ^ 1U));
                }
                host_movie_frames_staged += after.frames_staged - before.frames_staged;
                host_movie_buffers_staged += after.staged_buffers - before.staged_buffers;
                host_movie_bytes_staged += after.staged_bytes - before.staged_bytes;
                host_movie_loop_restarts += after.loop_restarts - before.loop_restarts;
            }

            memory.raise_intc_cause(2U);
            ++raised_vblank_starts;

            if (rwa_iop_shadow.has_value() && (rwa_shadow_mode == 2U || !rwa_shadow_boot_done)) {
                rwa_iop_shadow->advance_master_clock(next_vblank_start);
                for (;;) {
                    const auto deferred = rwa_iop_shadow->take_response(20U);
                    if (!deferred.has_value() || !kernel.apply_deferred_rwa_shadow_response(*deferred, memory)) {
                        break;
                    }
                }
            }

            if (host_window != nullptr) {
                host_window_guest_fps.note_frame(host_steady_now_nanoseconds());
            }
            const bool host_frame_step_boundary_due = host_frame_step_enabled &&
                                                      host_frame_step_target_vblank.has_value() &&
                                                      *host_frame_step_target_vblank <= raised_vblank_starts;
            const bool host_capture_boundary_due = host_window_capture_requested &&
                                                   host_window_capture_required_vblank.has_value() &&
                                                   *host_window_capture_required_vblank == raised_vblank_starts;
            game_graphics_options.on_vblank(memory);
            game_scenery_detail.on_vblank(memory);

            if (pc_options.enabled() && !native_pc_options.unavailable_reason().has_value()) {
                const platform::RawInputState raw_controls =
                    host_window != nullptr ? host_window->raw_input_state() : platform::RawInputState{};
                if (host_window != nullptr) {
                    controls_session.set_controllers(host_window->controllers());
                }
                if (const auto action = native_pc_options.consume_controls_action(); action.has_value()) {
                    controls_session.activate(action->first, action->second, raw_controls);
                }
                const auto layout_device = controls_session.layout_device();
                const platform::RawJoystickState joystick = host_window != nullptr && layout_device.has_value()
                                                                ? host_window->joystick_state(*layout_device)
                                                                : platform::RawJoystickState{};
                if (controls_session.poll(raw_controls, joystick)) {
                    game_bindings_menu.reset();
                }
                if (const auto mapping = controls_session.take_layout_mapping(); mapping.has_value()) {
                    std::string error = "no input window";
                    const bool applied = host_window != nullptr && host_window->add_gamepad_mapping(*mapping, error);
                    controls_session.layout_applied(applied, error);
                    std::cerr << "Controls: layout "
                              << (applied ? std::string("applied: ") : "rejected (" + error + "): ") << *mapping
                              << '\n';
                }
                if (host_window != nullptr) {
                    host_window->set_input_suppressed(controls_session.blocks_game_input());
                }
                native_pc_options.service_controls(memory, controls_session.rows(), controls_session.capturing(),
                                                   controls_session.notice(), controls_session.capture_prompt());
                config::GraphicsSettings edited = pc_options.graphics();
                const auto native_page = mods_manager.active()
                                             ? native_pc_options.service(memory, edited, &native_mods_page_state)
                                             : native_pc_options.service(memory, edited);
                if (native_page.installed_now) {
                    std::cerr << "PC options: added to the game's Options menu\n";
                }
                if (native_page.settings_changed) {
                    const auto update = pc_options.apply_graphics(edited);
                    if (update.save_error.has_value()) {
                        std::cerr << "Settings: " << *update.save_error << '\n';
                    }
                    apply_pc_options();
                }
                if (native_page.mods_rescan_requested) {
                    std::optional<std::string> sync_error;
                    if (mod_sync.pending()) {
                        sync_error = mod_sync.refresh(mods_manager);
                    } else {
                        mods_manager.rescan();
                        sync_error = mods_manager.save();
                    }
                    if (const auto error = mods_manager.request_thumbnail_refresh(); error.has_value()) {
                        std::cerr << "Mod thumbnails: " << *error << '\n';
                    }
                    mod_overlay = mods_manager.build_overlay();
                    refresh_native_mods_page_state();
                    std::size_t enabled = 0U;
                    for (const auto& mod : mods_manager.mods()) {
                        enabled += mod.enabled ? 1U : 0U;
                    }
                    native_mods_page_state.notice = "Mod list refreshed: " + std::to_string(mods_manager.mods().size()) +
                                                    (mods_manager.mods().size() == 1U ? " mod, " : " mods, ") +
                                                    std::to_string(enabled) + " enabled";
                    if (sync_error.has_value()) {
                        native_mods_page_state.notice = *sync_error;
                    }
                    if (!sync_error.has_value()) {
                        native_online.set_applied_mods(std::make_shared<const content::ModManager>(mods_manager));
                        auto target = mod_sync.consume_join_request();
                        if (target.has_value()) {
                            const auto error = native_online.prepare_mod_sync_rejoin(target->room, target->mods);
                            if (error.has_value()) {
                                mod_sync.abort(*error);
                                pending_mod_join.reset();
                                native_mods_page_state.notice = *error;
                            } else {
                                pending_mod_join = std::move(target);
                                native_mods_page_state.notice = "Connecting to lobby...";
                            }
                        }
                    }
                    ++native_mods_page_state.notice_serial;
                    added_cars_dirty = true;
                }
                if (native_page.mod_edit.has_value()) {
                    if (mods_manager.apply_page_value(native_page.mod_edit->row, native_page.mod_edit->value)) {
                        if (const auto save_error = mods_manager.save(); save_error.has_value()) {
                            std::cerr << "Mods: " << *save_error << '\n';
                        }
                        mod_overlay = mods_manager.build_overlay();
                        refresh_native_mods_page_state();
                        native_online.set_applied_mods(std::make_shared<const content::ModManager>(mods_manager));
                        added_cars_dirty = true;
                    }
                }
                if (mods_manager.active() && parallel_gs_stream_writer != nullptr && parallel_gs_live_fifo_path.has_value()) {
                    native_mods_ui = native_pc_options.mods_information_open()
                                         ? platform::ParallelGsHostUi{}
                    : presentation::build_mods_page_ui(mods_manager, native_pc_options.mods_page_window(),
                                                      native_pc_options.mods_page_rows(),
                                                      native_pc_options.mods_page_focus(memory));
                }
            }

            // With the Xbox layout the gamepad uses its menu buttons (A confirms,
            // B goes back) while the game shows a menu.
            if (host_window != nullptr) {
                bool menu = false;
                if (controls_session.style() == ui::ControlStyle::Xbox) {
                    menu = memory.read_u32_le(0x01e90424U) == 5U || settings_page.active() ||
                           control_style_prompt.is_open();
                    if (!menu) {
                        const auto screen = runtime::read_menu_mouse_screen(memory);
                        menu = !screen.lists.empty() || screen.footer;
                    }
                }
                if (game_bindings_menu != menu) {
                    host_window->set_input_bindings(controls_session.game_bindings(menu));
                    game_bindings_menu = menu;
                }
            }
            // The controller layout question, else the SETTINGS page's UI, else
            // the online player count on the main menu, else the MODS page's.
            online_count.update(memory);
            const auto page_host_ui = [&]() -> platform::ParallelGsHostUi {
                if (control_style_prompt.is_open()) {
                    return ui::control_style_prompt_ui(settings_ui_directory(), settings_ui_directory(),
                                                       control_style_prompt);
                }
                if (settings_page.active()) {
                    return settings_page.host_ui(settings_ui_directory());
                }
                if (online_count.visible()) {
                    return online_count.host_ui(settings_ui_directory());
                }
                return native_mods_ui;
            };
            if (added_cars.registered()) {
                if (added_cars_dirty) {
                    configure_added_cars();
                }
                added_cars.service(memory);
                if (native_car_select_active) {
                    const auto preview_poll_time = std::chrono::steady_clock::now();
                    bool previews_changed = false;
                    if (preview_poll_time >= next_thumbnail_cache_poll) {
                        next_thumbnail_cache_poll = preview_poll_time + std::chrono::seconds(1);
                        previews_changed = mods_manager.thumbnail_cache_changed();
                    }
                    native_car_select.refresh(memory, mods_manager, added_cars_dirty || previews_changed);
                    const platform::HostControllerState pad =
                        guest_input_replay != nullptr ? guest_input_replay->controller_state() : host_controller_input();
                    if (pad.right_shoulder && !car_select_skin_held) {
                        native_car_select.next_skin(memory);
                    }
                    auto ui = native_car_select.host_ui(memory);
                    // Car select's UI, else the SETTINGS page's or the MODS page's.
                    if (ui.items.empty()) {
                        ui = page_host_ui();
                    }
                    memory.run_in_graphics_order(
                        [writer = parallel_gs_stream_writer.get(), host_ui = std::move(ui)]() {
                        writer->set_host_ui(host_ui);
                    });
                    car_select_skin_held = pad.right_shoulder;
                }
                added_cars_dirty = false;
            }
            if (!native_car_select_active && parallel_gs_stream_writer != nullptr) {
                memory.run_in_graphics_order(
                    [writer = parallel_gs_stream_writer.get(),
                     host_ui = page_host_ui()]() {
                        writer->set_host_ui(host_ui);
                    });
            }
            if (native_pc_options.unavailable_reason().has_value()) {
                if (!native_pc_options_reported) {
                    std::cerr << "PC options: not added to the game's Options menu: " << *native_pc_options.unavailable_reason()
                              << '\n';
                }
                native_pc_options_reported = true;
            }

            if (parallel_gs_stream_writer != nullptr) {
                memory.run_frame_in_graphics_order(
                    [writer = parallel_gs_stream_writer.get(),
                     phase = static_cast<std::uint8_t>((raised_vblank_starts - 1U) & 1U),
                     display_state = memory.gs_privileged_display().state(),
                     hint = platform::ParallelGsCrtcHint{kernel.gs_crt_state().configured, kernel.gs_crt_state().interlace,
                                                         kernel.gs_crt_state().mode, kernel.gs_crt_state().field_mode}]() {
                    writer->write_vblank(phase, display_state, hint);
                },
                graphics_frames_in_flight);
            }
            if (parallel_gs_completion_reader != nullptr) {
                drain_parallel_gs_completions(static_cast<std::uint64_t>(raised_vblank_starts));
            }
            if (gs_semantic_trace_writer != nullptr) {
                gs_semantic_trace_writer->note_vblank(raised_vblank_starts);
            }

            if (host_window != nullptr && gs_renderer_worker.has_value()) {
                if (host_window_pending_snapshots.size() <= 1U || host_frame_step_boundary_due || host_capture_boundary_due) {
                    const auto scanout_plan = presentation::plan_gs_scanout(memory.gs_privileged_display().state());
                    host_window_pending_snapshots.emplace_back(
                        HostWindowPendingSnapshot{gs_renderer_worker->request_snapshot(scanout_plan), raised_vblank_starts});
                    ++host_window_vblank_snapshot_requests;
                } else {
                    ++host_window_saturated_vblanks;
                }
            }
            if (host_capture_boundary_due) {
                host_window_capture_boundary_paused = true;
                std::cout << "F8 capture: latched GS boundary " << raised_vblank_starts << " (trace VBlank "
                          << raised_vblank_starts - 1U << ")\n"
                          << std::flush;
                reached_host_capture_boundary = true;
            }
            if (host_frame_step_boundary_due) {
                host_frame_step_paused = true;
                host_frame_step_advancing = false;
                host_frame_step_target_vblank.reset();
                std::cout << "Host frame step: paused at VBlank " << raised_vblank_starts << '\n' << std::flush;
                reached_host_frame_step_boundary = true;
            }
            next_vblank_start += 4915200U;
            if (reached_host_frame_step_boundary || reached_host_capture_boundary) {
                break;
            }
        }
        if (variable_frame_busy_vblank) {
            const auto busy = variable_frame_clock->note_busy_vblank(
                emulated_ee_cycles, variable_frame_clock->live() ? host_steady_now_nanoseconds() : 0U);
            if (busy.wait_until_nanoseconds.has_value()) {
                wait_for_host_time(*busy.wait_until_nanoseconds, platform::HostWaitSite::FramePacingBusyVblank);
            }
            emulated_ee_cycles += busy.advance_cycles;
            next_vblank_start += busy.advance_cycles;
        }

        if (retail_timer_owner_abi_trace.active()) {
            retail_timer_owner_abi_trace.observe_scheduler_boundary(current_retail_timer_owner_context());
        }
        auto scheduler_phase = begin_profiled_host_phase(runtime::EeHostPhase::Scheduler);
        kernel.set_ee_cycle_count(emulated_ee_cycles);
        {
            std::optional<ScopedRetailTimerConsumerMemoryWriteCapture> scheduler_sif_dma_write_capture;
            if (retail_timer_consumer_trace.collecting()) {
                runtime::RetailTimerConsumerTraceContext sif_context{};
                scheduler_sif_dma_write_capture.emplace(retail_timer_consumer_trace, memory,
                                                        ScopedRetailTimerConsumerMemoryWriteCapture::Kind::SchedulerSifDma,
                                                        sif_context);
            }
            kernel.service_sif_dma_completions(memory);
        }
        if (reached_host_frame_step_boundary || reached_host_capture_boundary) {
            continue;
        }

        runtime::slus_21050::SubmitUiDrawBucketsExecutionContext completing_callback_context{};
        bool completing_callback_observed = false;
        if (ui_draw_session_feed.active() && state.pc() == guest::EeKernelHle::kGuestCallbackReturnAddress &&
            kernel.guest_callback_active()) {
            completing_callback_context = current_ui_execution_context();
            completing_callback_observed = true;
            if (ui_draw_session_feed.requires_pre_instruction(completing_callback_context, state.pc())) {
                ui_draw_session_feed.observe_pre_instruction(state, memory, completing_callback_context);
            }
        }
        if (kernel.finish_guest_callback(state)) {
            if (completing_callback_observed) {
                ui_draw_session_feed.retire_execution_context(completing_callback_context);
            }
            if (retail_trace && dmac_callback_in_flight && !dmac_callback_trace.empty()) {
                auto& trace = dmac_callback_trace.back();
                trace.return_step = committed_steps;
                trace.read_after = memory.read_u32_le(0x004e2838U);
                trace.end_after = memory.read_u32_le(0x004e283cU);
                trace.active_after = memory.read_u8(0x004e2830U);
                dmac_callback_in_flight = false;
            }
            continue;
        }

        std::int32_t completing_thread_id = 0;
        bool completing_thread_observed = false;
        runtime::slus_21050::SubmitUiDrawBucketsExecutionContext completing_thread_context{};
        if (ui_draw_session_feed.active() && state.pc() == 0U && !kernel.guest_callback_active()) {
            completing_thread_id = kernel.current_thread_id();
            completing_thread_observed = true;
            completing_thread_context.thread_id = completing_thread_id;
            if (ui_draw_session_feed.requires_pre_instruction(completing_thread_context, state.pc())) {
                ui_draw_session_feed.observe_pre_instruction(state, memory, completing_thread_context);
            }
        }
        if (kernel.finish_guest_thread(state)) {
            if (completing_thread_observed) {
                ui_draw_session_feed.retire_thread(completing_thread_id);
            }
            ++completed_guest_threads;
            continue;
        }

        if (kernel.dispatch_pending_alarm_callback(state)) {
            ++dispatched_alarm_callbacks;
            continue;
        }
        const auto thread_dispatch = kernel.dispatch_guest_threads(state);
        if (thread_dispatch == guest::EeThreadDispatchResult::Switched) {
            ++dispatched_thread_switches;
            continue;
        }
        if (kernel.dispatch_pending_sif_command_callback(state)) {
            ++dispatched_sif_command_callbacks;
            continue;
        }
        if (kernel.dispatch_pending_dmac_callback(state, memory)) {
            ++dispatched_dmac_callbacks;
            if (retail_trace) {
                dmac_callback_trace.push_back(DmacCallbackTrace{
                    committed_steps, state.read_gpr_u32(4U), memory.read_u32_le(0x004e2838U),
                    memory.read_u32_le(0x004e283cU), memory.read_u8(0x004e2830U), 0U, 0U, 0U, 0U});
                dmac_callback_in_flight = true;
            }
            continue;
        }
        const bool at_retail_vblank_poll =
            (state.pc() >= kRetailVblankPollBegin && state.pc() <= kRetailVblankPollEnd) ||
            (state.pc() >= kRetailVblankCallbackPollBegin && state.pc() <= kRetailVblankCallbackPollEnd);
        if (!at_retail_vblank_poll && kernel.dispatch_pending_intc_callback(state, memory)) {
            ++dispatched_intc_events;
            continue;
        }
        if (thread_dispatch == guest::EeThreadDispatchResult::NoRunnable) {
            memory.graphics_async_fence(runtime::GraphicsAsyncFence::SchedulerIdle);
            const std::size_t queued_dmac_completions = memory.queued_dmac_completion_count();
            if (idle_vblank_probe_active && !memory.gif_dmac_can_progress() && !memory.vif1_dmac_can_progress() &&
                queued_dmac_completions == 0U) {
                stopped_without_runnable_thread = true;
                break;
            }
            std::uint64_t next_idle_event = next_vblank_start;
            bool waiting_for_alarm = false;
            bool waiting_for_gif_dmac = false;
            bool waiting_for_dmac_completion = false;
            const auto alarm_deadline = kernel.next_alarm_deadline();
            if (alarm_deadline.has_value() && *alarm_deadline > emulated_ee_cycles && *alarm_deadline < next_idle_event) {
                next_idle_event = *alarm_deadline;
                waiting_for_alarm = true;
            }
            if (memory.gif_dmac_can_progress() || memory.vif1_dmac_can_progress()) {
                const std::uint64_t gif_deadline = emulated_ee_cycles + 1U;
                if (next_idle_event > gif_deadline) {
                    next_idle_event = gif_deadline;
                    waiting_for_alarm = false;
                    waiting_for_gif_dmac = true;
                }
            }
            if (queued_dmac_completions != 0U) {
                const std::uint64_t completion_deadline = emulated_ee_cycles + 1U;
                if (next_idle_event > completion_deadline) {
                    next_idle_event = completion_deadline;
                    waiting_for_alarm = false;
                    waiting_for_gif_dmac = false;
                    waiting_for_dmac_completion = true;
                }
            }
            if (emulated_ee_cycles < next_idle_event) {
                emulated_ee_cycles = next_idle_event;
                if (waiting_for_gif_dmac || waiting_for_dmac_completion) {
                    ++fast_forwarded_graphics_drain_waits;
                } else if (waiting_for_alarm) {
                    ++fast_forwarded_alarm_waits;
                } else {
                    ++fast_forwarded_vblank_waits;
                }
            }
            idle_vblank_probe_active = !(waiting_for_gif_dmac || waiting_for_dmac_completion);
            continue;
        }
        if (!kernel.guest_callback_active() && kernel.current_thread_id() != 0) {
            idle_vblank_probe_active = false;
        }
        scheduler_phase.finish();
        if (retail_timer_owner_abi_trace.active()) {
            retail_timer_owner_abi_trace.observe_scheduler_boundary(current_retail_timer_owner_context());
        }
        if (!memory.contains(state.pc(), 4U)) {
            outside_guest_memory = true;
            break;
        }
        const std::uint32_t dispatch_pc = state.pc();
        if (instance_batch_capture.active() || instance_batch_capture.wants_pc(dispatch_pc)) {
            instance_batch_capture.observe(
                state, memory,
                runtime::slus_21050::InstanceBatchCaptureContext{
                .scheduler = {.committed_instructions = committed_steps,
                              .instruction_budget = guest_input_replay != nullptr
                                                        ? guest_input_replay->clip_step_limit(committed_steps, maximum_steps)
                                                        : maximum_steps,
                              .ee_cycles = emulated_ee_cycles,
                              .next_vblank_cycle = next_vblank_start,
                              .next_alarm_cycle = kernel.next_alarm_deadline(),
                              .queued_dmac_completions = memory.queued_dmac_completion_count(),
                              .gif_dma_in_flight = memory.gif_dmac_in_flight(),
                              .vif1_dma_in_flight = memory.vif1_dmac_in_flight(),
                              .pending_dmac_callbacks = kernel.pending_dmac_callback_count(),
                              .pending_sif_callbacks = kernel.pending_sif_command_callback_count(),
                              .sif_callback_poll_can_progress = kernel.sif_command_callback_poll_can_progress(),
                              .sif_dma_completion_pending = kernel.sif_dma_completion_pending(),
                              .next_sif_dma_completion_cycle = kernel.next_sif_dma_completion_deadline(),
                              .competing_ready_thread = kernel.has_competing_ready_thread(),
                              .pending_intc_causes = memory.pending_intc_causes(),
                              .executing_delay_slot = state.is_executing_delay_slot()},
                .thread_id = kernel.current_thread_id(),
                .guest_callback_active = kernel.guest_callback_active()},
            guest_work_host_start.has_value());
        }
        if (object_packet_capture.enabled() && (dispatch_pc == 0x0026af50U || dispatch_pc == 0x0026b1a8U)) {
            object_packet_capture.observe(state, memory, committed_steps, guest_work_host_start.has_value());
        }
        if (model_submission_probe.enabled() && model_submission_probe.wants_pc(dispatch_pc)) {
            model_submission_probe.observe(state, memory, guest_work_host_start.has_value());
        }
        memory.set_current_ee_pc(dispatch_pc);
        memory.set_current_guest_step(committed_steps);
        const auto femain_retail_consume_execution_context = [&kernel]() {
            return runtime::Slus21050FemainRetailConsumeExecutionContext{
                kernel.current_thread_id(), kernel.guest_callback_active(), kernel.guest_callback_identity()};
        };
        if (femain_retail_consume_invocation_barrier != nullptr &&
            femain_retail_consume_invocation_barrier->blocks_accelerated_dispatch()) {
            const std::uint32_t dispatch_instruction =
                memory.contains(dispatch_pc, 4U) ? memory.read_u32_le(dispatch_pc) : 0U;
            const auto barrier_phase = femain_retail_consume_invocation_barrier->phase();
            const bool dispatch_valid = femain_retail_consume_invocation_barrier->observe_dispatch(
                dispatch_pc, dispatch_instruction, femain_retail_consume_execution_context(),
                femain_retail_consume_observation_adapter->attachment());
            if (barrier_phase == runtime::Slus21050FemainRetailConsumeInvocationBarrierPhase::Armed &&
                femain_retail_consume_invocation_barrier->phase() ==
                    runtime::Slus21050FemainRetailConsumeInvocationBarrierPhase::Active &&
                dispatch_valid) {
                const auto packet = host_femain_live_transaction.owner().publication_packet();
                if (!(packet.has_value() &&
                      packet->publication_serial == femain_retail_consume_invocation_barrier->publication_serial() &&
                      femain_retail_consume_observation_adapter->bind_publication(
                          host_femain_live_transaction.owner(), host_femain_live_transaction.ledger(), *packet,
                          femain_retail_consume_invocation_barrier->identity()) &&
                      femain_retail_consume_invocation_barrier->confirm_binding(
                          *femain_retail_consume_observation_adapter))) {
                    femain_retail_consume_invocation_barrier->reject(
                        runtime::Slus21050FemainRetailConsumeInvocationBarrierFailure::AdapterFailed);
                    femain_retail_consume_observation_adapter->close();
                }
            } else if (!dispatch_valid) {
                femain_retail_consume_observation_adapter->close();
            }
        }
        const bool femain_retail_consume_acceleration_blocked =
            femain_retail_consume_invocation_barrier != nullptr &&
            femain_retail_consume_invocation_barrier->blocks_accelerated_dispatch();

        if (!ee_pc_hit_watch.empty() && dispatch_pc < 0x02000000U &&
            (dispatch_pc & 3U) == 0U) {
            const std::uint8_t slot = ee_pc_hit_watch_slots[dispatch_pc >> 2U];
            if (slot != 0xffU) {
                auto& watch = ee_pc_hit_watch[slot];
                ++watch.total;
                if (guest_work_measurement_window.has_value() && guest_work_measurement_window->started()) {
                    ++watch.guest_work_window;
                }
            }
        }

        if (retail_timer_owner_observer_enabled) {
            constexpr std::uint32_t kRetailTimerUpdate = 0x001d3a60U;
            constexpr std::uint32_t kRetailTimerReturn = 0x001d3c04U;
            const bool native_simulation_shadow_active = native_simulation_shadow != nullptr &&
                                                         guest_work_measurement_window.has_value() &&
                                                         guest_work_measurement_window->started();
            if (dispatch_pc == kRetailTimerUpdate) {
                if (retail_timer_differential_enabled && retail_timer_projection.has_value()) {
                    throw std::logic_error("retail timer differential observed a nested update");
                }
                const std::uint32_t object = state.read_gpr_u32(4U);
                const std::uint32_t global_pointer = state.read_gpr_u32(28U);
                if (global_pointer < 24372U || !memory.contains(global_pointer - 24372U, 8U)) {
                    throw std::runtime_error("retail timer differential globals are outside guest memory");
                }
                const std::uint32_t shared_tick = memory.read_u32_le(global_pointer - 24372U);
                const float reference_delta_seconds = std::bit_cast<float>(memory.read_u32_le(global_pointer - 24368U));
                std::optional<runtime::RetailReferenceTimerState> before;
                if (retail_timer_differential_enabled || (native_simulation_shadow_active && object == 0x004f5080U)) {
                    before = read_retail_timer_state(object);
                }
                if (retail_timer_differential_enabled) {
                    runtime::RetailReferenceTimer model(reference_delta_seconds, *before);
                    model.update(shared_tick);
                    retail_timer_projection = RetailTimerProjection{
                        object, model.state(),
                        guest_work_measurement_window.has_value() && guest_work_measurement_window->started()};
                    ++retail_timer_differential_calls;
                    if (retail_timer_projection->guest_work_window) {
                        ++retail_timer_differential_window_calls;
                    }
                }
                if (native_simulation_shadow_active && object == 0x004f5080U) {
                    native_simulation_shadow_object = object;
                    if (!native_simulation_shadow->observe_owner_entry(shared_tick, reference_delta_seconds, *before,
                                                                       emulated_ee_cycles)) {
                        native_simulation_shadow_stop_requested = true;
                    }
                }
            } else if (dispatch_pc == kRetailTimerReturn) {
                std::optional<runtime::RetailReferenceTimerState> actual;
                if (retail_timer_projection.has_value()) {
                    actual = read_retail_timer_state(retail_timer_projection->object);
                }
                if (native_simulation_shadow_active && native_simulation_shadow->owner_active() &&
                    native_simulation_shadow_object.has_value()) {
                    if (!(actual.has_value() && retail_timer_projection.has_value() &&
                          retail_timer_projection->object == *native_simulation_shadow_object)) {
                        actual = read_retail_timer_state(*native_simulation_shadow_object);
                    }
                    if (actual.has_value()) {
                        const bool owner_return_observed =
                            native_simulation_shadow->observe_owner_return(*actual, emulated_ee_cycles);
                        if (!owner_return_observed) {
                            native_simulation_shadow_stop_requested = true;
                        } else if (vehicle_world_contact_probe != nullptr &&
                                   !vehicle_world_contact_probe_bound_to_shadow) {
                            const auto epoch = native_simulation_shadow->state_epoch();
                            vehicle_world_contact_probe_bound_to_shadow =
                                epoch != 0U && vehicle_world_contact_probe->bind_session_epoch(epoch);
                            if (!vehicle_world_contact_probe_bound_to_shadow) {
                                native_simulation_shadow->reject_guest_snapshot();
                                native_simulation_shadow_stop_requested = true;
                            }
                        }
                    }
                }
                if (retail_timer_projection.has_value() && actual.has_value()) {
                    if (*actual == retail_timer_projection->expected) {
                        ++retail_timer_differential_matches;
                        if (retail_timer_projection->guest_work_window) {
                            ++retail_timer_differential_window_matches;
                        }
                    } else {
                        ++retail_timer_differential_mismatches;
                        if (retail_timer_projection->guest_work_window) {
                            ++retail_timer_differential_window_mismatches;
                        }
                        if (!retail_timer_first_mismatch.has_value()) {
                            const auto& expected = retail_timer_projection->expected;
                            std::ostringstream detail;
                            detail << "object=" << hex32(retail_timer_projection->object)
                                   << " expected={tick=" << expected.observed_tick
                                   << ",offset=" << expected.consumed_tick_offset
                                   << ",paused=" << expected.paused_ticks << ",whole=" << expected.whole_ticks
                                   << ",remainder=" << expected.subdivision_remainder
                                   << ",baseline=" << expected.baseline_tick
                                   << ",subdivisions=" << expected.subdivisions
                                   << ",step=" << hex32(std::bit_cast<std::uint32_t>(expected.step_seconds))
                                   << ",elapsed=" << hex32(std::bit_cast<std::uint32_t>(expected.elapsed_seconds))
                                   << ",pending=" << expected.pending_subdivisions
                                   << ",accumulating=" << expected.accumulating
                                   << "} actual={tick=" << actual->observed_tick
                                   << ",offset=" << actual->consumed_tick_offset
                                   << ",paused=" << actual->paused_ticks << ",whole=" << actual->whole_ticks
                                   << ",remainder=" << actual->subdivision_remainder
                                   << ",baseline=" << actual->baseline_tick
                                   << ",subdivisions=" << actual->subdivisions
                                   << ",step=" << hex32(std::bit_cast<std::uint32_t>(actual->step_seconds))
                                   << ",elapsed=" << hex32(std::bit_cast<std::uint32_t>(actual->elapsed_seconds))
                                   << ",pending=" << actual->pending_subdivisions
                                   << ",accumulating=" << actual->accumulating << '}';
                            retail_timer_first_mismatch = detail.str();
                        }
                    }
                    retail_timer_projection.reset();
                }
            }
        }
        if (native_simulation_shadow_stop_requested) {
            break;
        }
        const std::size_t execution_step_limit = guest_input_replay != nullptr
                                                     ? guest_input_replay->clip_step_limit(committed_steps, maximum_steps)
                                                     : maximum_steps;
        runtime::slus_21050::SubmitUiDrawBucketsExecutionContext ui_draw_execution_context{};
        const bool flush_claim_tracking =
            vif_packet_ring_flush_claim_tracker.active() ||
            (dispatch_pc == runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushEntryAddress &&
             ui_text_vif_transport.has_unarmed_claims());
        if (ui_draw_session_feed.active() ||
            dispatch_pc == runtime::slus_21050::SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress ||
            flush_claim_tracking) {
            ui_draw_execution_context = current_ui_execution_context();
        }
        if (ui_draw_session_feed.requires_pre_instruction(ui_draw_execution_context, dispatch_pc)) {
            ui_draw_session_feed.observe_pre_instruction(state, memory, ui_draw_execution_context);
        }
        if (flush_claim_tracking &&
            vif_packet_ring_flush_claim_tracker.requires_pre_instruction(ui_draw_execution_context, dispatch_pc)) {
            vif_packet_ring_flush_claim_tracker.observe_pre_instruction(state, memory, ui_draw_execution_context);
        }
        if (variable_frame_rate.has_value() && scalar_burst_hooks.marked(dispatch_pc) &&
            (!variable_frame_hook_step.has_value() || *variable_frame_hook_step != committed_steps)) {
            const auto frame_rate_hook = variable_frame_rate->on_dispatch(state, memory, emulated_ee_cycles);
            if (frame_rate_hook == runtime::Slus21050FrameRateHookResult::Replaced) {
                observe_profiled_ee_span(committed_steps, dispatch_pc, 1U, runtime::EePcSampleKind::Hle);
                ++committed_steps;
                ++executed_instructions;
                ++emulated_ee_cycles;
                continue;
            }
            if (frame_rate_hook == runtime::Slus21050FrameRateHookResult::Continue) {
                variable_frame_hook_step = committed_steps;
            }
        }
        if (dispatch_pc == runtime::Slus21050CameraFov::kUiSubmissionBegin && camera_fov.panini() &&
            parallel_gs_stream_writer != nullptr && committed_steps != ui_submission_step) {
            ui_submission_step = committed_steps;
            const auto& camera_view = camera_fov.view();
            if (camera_view.chasing && camera_view.serial != panini_warp_serial) {
                panini_warp_serial = camera_view.serial;
                const auto panini =
                    presentation::panini_parameters(camera_view.game_fov, camera_view.fov, camera_fov.aspect());
                if (panini.tangent != 0U) {
                    memory.run_in_graphics_order(
                        [writer = parallel_gs_stream_writer.get(), panini, widescreen = camera_fov.aspect() > 1.5F]() {
                        writer->write_panini_warp(panini.tangent, panini.strength, panini.extent, widescreen);
                    });
                }
            }
        }
        if (dispatch_pc == runtime::Slus21050CameraFov::kFovUse &&
            camera_fov.on_dispatch(state, memory) == runtime::Slus21050CameraFov::HookResult::Replaced) {
            ++committed_steps;
            ++executed_instructions;
            ++emulated_ee_cycles;
            continue;
        }
        if (native_car_select_active && scalar_burst_hooks.marked(dispatch_pc) &&
            native_car_select.on_dispatch(state, memory)) {
            observe_profiled_ee_span(committed_steps, dispatch_pc, 1U, runtime::EePcSampleKind::Hle);
            ++committed_steps;
            ++executed_instructions;
            ++emulated_ee_cycles;
            continue;
        }
        if (scalar_burst_hooks.marked(dispatch_pc) && native_pc_options.installed() &&
            native_pc_options.on_dispatch(state, memory) == runtime::Slus21050NativePcOptions::HookResult::Replaced) {
            observe_profiled_ee_span(committed_steps, dispatch_pc, 1U, runtime::EePcSampleKind::Hle);
            ++committed_steps;
            ++executed_instructions;
            ++emulated_ee_cycles;
            continue;
        }
        if (!femain_retail_consume_acceleration_blocked &&
            dirty_dnas_compat.try_handle(state, memory) != guest::DirtyDnasCompatCall::NotHandled) {
            observe_profiled_ee_span(committed_steps, dispatch_pc, 1U, runtime::EePcSampleKind::Hle);
            ++committed_steps;
            ++emulated_ee_cycles;
            continue;
        }
        const auto join_pump_candidate_end =
            join_pump_callback_tail_region_active
                ? takedown::recompiler::JoinPumpRegion::kCallbackTailEffectEndAddress
                : join_pump_callback_region_requested
                      ? takedown::recompiler::JoinPumpRegion::kExtendedCallerEffectEndAddress
                      : takedown::recompiler::JoinPumpRegion::kDirectEffectEndAddress;
        const bool join_pump_candidate_pc = dispatch_pc >= takedown::recompiler::JoinPumpRegion::kEntryAddress &&
            dispatch_pc < join_pump_candidate_end;
        const bool join_pump_inert_dmac_callback =
            join_pump_region.has_value() && join_pump_candidate_pc &&
            (join_pump_callback_region_requested || elide_inert_dmac_poll) &&
            runtime::pending_dmac_callback_is_inert(state, memory, kernel);
        const bool join_pump_candidate = join_pump_region.has_value() && join_pump_candidate_pc;
        if (join_pump_candidate && ee_graphics_service_diagnostics != nullptr) {
            ee_graphics_service_diagnostics->record_join_pump_scheduler_query();
        }
        if (join_pump_region.has_value() && join_pump_candidate_pc && !retail_trace && ee_pc_hit_watch.empty() &&
            !retail_timer_differential_enabled && !retail_timer_consumer_trace.enabled() &&
            femain_dynamic_callback_probe == nullptr && !femain_retail_consume_acceleration_blocked &&
            !ui_draw_session_feed.active() && !flush_claim_tracking && memory.queued_dmac_completion_count() == 0U &&
            memory.pending_intc_causes() == 0U &&
            (kernel.pending_dmac_callback_count() == 0U || join_pump_inert_dmac_callback) &&
            kernel.pending_sif_command_callback_count() == 0U && !kernel.sif_command_callback_poll_can_progress() &&
            !kernel.has_competing_ready_thread()) {
            if (ee_graphics_service_diagnostics != nullptr) {
                ee_graphics_service_diagnostics->record_join_pump_admission(true);
            }
            const std::uint64_t remaining_instructions =
                execution_step_limit > committed_steps ? execution_step_limit - committed_steps : 0U;
            std::uint64_t remaining_cycles = next_vblank_start > emulated_ee_cycles ? next_vblank_start - emulated_ee_cycles : 0U;
            if (ee_graphics_service_diagnostics != nullptr) {
                ee_graphics_service_diagnostics->record_join_pump_deadline_query();
            }
            if (const auto alarm_deadline = kernel.next_alarm_deadline(); alarm_deadline.has_value()) {
                remaining_cycles =
                    std::min(remaining_cycles, *alarm_deadline > emulated_ee_cycles ? *alarm_deadline - emulated_ee_cycles : 0U);
            }
            if (ee_graphics_service_diagnostics != nullptr) {
                ee_graphics_service_diagnostics->record_join_pump_deadline_query();
            }
            if (const auto sif_deadline = kernel.next_sif_dma_completion_deadline(); sif_deadline.has_value()) {
                remaining_cycles =
                    std::min(remaining_cycles, *sif_deadline > emulated_ee_cycles ? *sif_deadline - emulated_ee_cycles : 0U);
            }
            const std::uint32_t region_capacity =
                join_pump_callback_tail_region_active && join_pump_loop_batch && ee_host_phase_profiler == nullptr &&
                        ee_graphics_service_diagnostics == nullptr && !subframe_input_bridge.has_value() &&
                        !join_pump_graphics_deadline_requested
                    ? takedown::recompiler::JoinPumpRegion::kMaximumLoopRetiredInstructions
                    : takedown::recompiler::JoinPumpRegion::kMaximumRetiredInstructions;
            const std::uint32_t instruction_capacity =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(region_capacity, remaining_instructions));
            const std::uint32_t cycle_capacity =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(region_capacity, remaining_cycles));
            if (std::min(instruction_capacity, cycle_capacity) > 1U) {
                runtime::GraphicsDmacInstructionProgress graphics_progress{};
                graphics_progress.callback_route = runtime::EeGraphicsCallbackRoute::JoinPump;
                graphics_progress.boundary_drain = [](void* context) noexcept {
                    return decltype(drain_femain_live_source_context){}(*static_cast<FEmainLiveSourceDrainContext*>(context));
                };
                graphics_progress.memory = &memory;
                graphics_progress.kernel = &kernel;
                graphics_progress.diagnostic_kernel = profile_timing_active ? &kernel : nullptr;
                graphics_progress.profiler = profile_timing_active ? ee_host_phase_profiler.get() : nullptr;
                graphics_progress.diagnostics = profile_timing_active ? ee_graphics_service_diagnostics.get() : nullptr;
                graphics_progress.dispatch_start_cycle = emulated_ee_cycles;
                graphics_progress.vif1_serviced_cycle = &vif1_dmac_serviced_ee_cycles;
                graphics_progress.gif_serviced_cycle = &gif_dmac_serviced_ee_cycles;
                graphics_progress.boundary_drain_context = &host_femain_live_source_drain_context;
                const bool profiled_graphics_service =
                    profile_timing_active && (ee_host_phase_profiler != nullptr || ee_graphics_service_diagnostics != nullptr);
                const takedown::recompiler::JoinPumpRegionProgressCallback graphics_service_callback =
                    femain_live_transaction_enabled
                        ? (profiled_graphics_service
                               ? runtime::service_profiled_graphics_after_direct_instruction_with_boundary_drain
                               : runtime::service_graphics_after_direct_instruction_with_boundary_drain)
                        : (profiled_graphics_service ? runtime::service_profiled_graphics_after_direct_instruction
                                                     : runtime::service_graphics_after_direct_instruction);
                runtime::JoinPumpGraphicsDeadlinePlan deadline_plan{};
                if (join_pump_graphics_deadline_requested) {
                    ++join_pump_graphics_deadline_queries;
                    guest::EeGraphicsServiceDeadline graphics_deadline{};
                    if (join_pump_graphics_deadline_cache_telemetry_enabled) {
                        graphics_deadline = memory.graphics_service_deadline();
                    } else {
                        const auto internal_graphics_deadline = memory.next_internal_graphics_deadline_for_join_pump();
                        if (internal_graphics_deadline.has_value()) {
                            graphics_deadline = {guest::EeGraphicsServiceDeadlineKind::InternalDeadline,
                                                 *internal_graphics_deadline};
                        }
                    }
                    deadline_plan = runtime::plan_join_pump_graphics_deadline(
                    {.enabled = true,
                     .deadline = graphics_deadline,
                     .existing_instruction_capacity = instruction_capacity,
                     .existing_cycle_capacity = cycle_capacity,
                     .maximum_region_capacity = takedown::recompiler::JoinPumpRegion::kMaximumRetiredInstructions,
                     .profile_timing_active = profile_timing_active,
                     .graphics_diagnostics_active = ee_graphics_service_diagnostics != nullptr,
                     .ee_pc_sampling_active = ee_pc_sampler != nullptr,
                     .subframe_input_active = subframe_input_bridge.has_value()});
                    if (deadline_plan.admitted) {
                        ++join_pump_graphics_deadline_admitted_spans;
                    } else {
                        record_join_pump_graphics_deadline_fallbacks(deadline_plan.fallback_mask);
                    }
                }
                const takedown::recompiler::JoinPumpRegionBudget execution_budget{
                    deadline_plan.admitted ? deadline_plan.instruction_capacity : instruction_capacity,
                    deadline_plan.admitted ? deadline_plan.cycle_capacity : cycle_capacity};
                auto join_pump_phase = begin_profiled_host_phase(runtime::EeHostPhase::CompiledStage);
                takedown::recompiler::JoinPumpRegionProgressCheckpoint checkpoint{};
                std::uint32_t summarized_instructions = 0U;
                bool native_wait_candidate = false;
                if (region_capacity == takedown::recompiler::JoinPumpRegion::kMaximumLoopRetiredInstructions) {
                    checkpoint.continue_loop = [](void*, const auto&, const auto& mem) {
                        return mem.gif_dmac_can_progress() || mem.vif1_dmac_can_progress();
                    };
                    if (join_pump_idle_summary) {
                        if (!join_pump_resume_idle_batch) {
                            checkpoint.continue_loop = [](void*, const auto& cpu, const auto& mem) {
                                return cpu.pc() != takedown::recompiler::JoinPumpRegion::kEntryAddress &&
                                       (mem.gif_dmac_can_progress() || mem.vif1_dmac_can_progress());
                            };
                        } else {
                            checkpoint.continue_loop = [](void*, const auto& cpu, const auto& mem) {
                                if (!(mem.gif_dmac_can_progress() || mem.vif1_dmac_can_progress())) {
                                    return false;
                                }
                                if (cpu.pc() != takedown::recompiler::JoinPumpRegion::kEntryAddress) {
                                    return true;
                                }
                                if (mem.graphics_service_deadline().kind != guest::EeGraphicsServiceDeadlineKind::Unknown) {
                                    return false;
                                }
                                ++join_pump_resumed_idle_loops;
                                return true;
                            };
                        }
                        if (state.pc() == takedown::recompiler::JoinPumpRegion::kEntryAddress) {
                            const auto deadline = memory.graphics_service_deadline();
                            native_wait_candidate = native_join_drain_wait && ee_pc_sampler == nullptr &&
                                                    !femain_live_transaction_enabled &&
                                                    deadline.kind == guest::EeGraphicsServiceDeadlineKind::Unknown;
                            if (join_pump_idle_diagnostics && guest_work_host_start.has_value() &&
                                deadline.kind == guest::EeGraphicsServiceDeadlineKind::Unknown) {
                                ++join_pump_idle_unknown_states[memory.graphics_idle_diagnostic_state()];
                            }
                            if (deadline.kind == guest::EeGraphicsServiceDeadlineKind::InternalDeadline &&
                                deadline.cycles_until_event > 1U) {
                                const std::uint32_t safe_cycles = static_cast<std::uint32_t>(
                                    std::min<std::uint64_t>(deadline.cycles_until_event - 1U, cycle_capacity));
                                summarized_instructions = join_pump_region->summarize_idle_iterations(
                                    state, memory, {instruction_capacity, safe_cycles},
                                    join_pump_idle_code_cache_enabled ? &join_pump_idle_code_cache : nullptr);
                            }
                        }
                    }
                }
                std::optional<takedown::recompiler::JoinPumpRegionResult> native_wait_execution;
                if (native_wait_candidate) {
                    const takedown::recompiler::JoinPumpRegionProgressCheckpoint native_checkpoint{
                        [](void* progress, const std::uint32_t retired, const std::uint32_t cycles) {
                            return runtime::service_graphics_after_direct_instruction(progress, retired, cycles);
                        },
                        &graphics_progress, checkpoint.continue_loop};
                    const auto wait = join_pump_region->execute_native_wait(state, memory, execution_budget,
                                                                            native_checkpoint, native_join_wait_code_cache);
                    if (wait.retired_instructions != 0U) {
                        ++native_join_wait_calls;
                        native_join_wait_instructions += wait.retired_instructions;
                        native_wait_execution = wait;
                    }
                }
                const takedown::recompiler::JoinPumpRegionResult execution =
                    native_wait_execution.has_value()
                        ? *native_wait_execution
                    : summarized_instructions != 0U
                        ? takedown::recompiler::JoinPumpRegionResult{takedown::recompiler::JoinPumpRegionExit::BudgetExhausted,
                                                                      summarized_instructions, summarized_instructions}
                : deadline_plan.admitted
                    ? join_pump_region->execute(state, memory, execution_budget, {})
                    : join_pump_region->execute(state, memory, execution_budget,
                {graphics_service_callback, &graphics_progress,
                 checkpoint.continue_loop});
                if ((deadline_plan.admitted || summarized_instructions != 0U) && execution.retired_instructions != 0U) {
                    if (femain_live_transaction_enabled) {
                        runtime::service_graphics_after_direct_instruction_with_boundary_drain(
                            &graphics_progress, execution.retired_instructions, execution.ee_cycles);
                    } else {
                        runtime::service_graphics_after_direct_instruction(
                            &graphics_progress, execution.retired_instructions, execution.ee_cycles);
                    }
                    if (deadline_plan.admitted) {
                        ++join_pump_graphics_deadline_endpoint_services;
                    }
                }
                join_pump_idle_summary_instructions += summarized_instructions;
                join_pump_phase.finish();
                if (region_capacity == takedown::recompiler::JoinPumpRegion::kMaximumLoopRetiredInstructions &&
                    execution.retired_instructions > takedown::recompiler::JoinPumpRegion::kMaximumRetiredInstructions) {
                    ++join_pump_loop_batch_calls;
                }
                ++join_pump_region_calls;
                if (join_pump_inert_dmac_callback) {
                    ++join_pump_region_inert_dmac_admissions;
                }
                ++join_pump_region_exits[static_cast<std::size_t>(execution.exit)];
                if (execution.retired_instructions == 0U) {
                    ++join_pump_region_zero_retired_rejections;
                } else {
                    const std::uint64_t retired = execution.retired_instructions;
                    observe_profiled_ee_span(committed_steps, dispatch_pc, retired, runtime::EePcSampleKind::CompiledBlock);
                    committed_steps += retired;
                    executed_instructions += retired;
                    emulated_ee_cycles += execution.ee_cycles;
                    join_pump_region_retired_instructions += retired;
                    if (deadline_plan.admitted) {
                        join_pump_graphics_deadline_retired_instructions += retired;
                        join_pump_graphics_deadline_retired_cycles += execution.ee_cycles;
                    }
                    continue;
                }
            }
        } else if (join_pump_candidate && ee_graphics_service_diagnostics != nullptr) {
            ee_graphics_service_diagnostics->record_join_pump_admission(false);
        }
        auto native_stage_phase = interpreter_only ? runtime::EeHostPhaseProfiler::Scope{}
        : begin_profiled_host_phase(runtime::EeHostPhase::NativeStage);
        native_gap_preflight.reset();
        runtime::NativeFunctionPreparation native_preparation{};
        runtime::CompiledBlockPreparation compiled_preparation{};
        const bool collision_scalar_target = collision_trace != nullptr && runtime::Slus21050CollisionTrace::is_target(state.pc());
        const bool vehicle_scalar_target = vehicle_world_contact_probe != nullptr &&
            runtime::Slus21050VehicleWorldContactEndpointProbe::is_target(state.pc());
        const bool spark_scalar_target = spark_trace != nullptr && runtime::Slus21050SparkTrace::is_target(state.pc());
        const bool femain_scalar_target = femain_dynamic_callback_probe != nullptr &&
            is_femain_dynamic_callback_scalar_target(state.pc());
        const runtime::NativeFunctionMetadata* native_candidate = nullptr;
        const runtime::CompiledBlockMetadata* compiled_candidate = nullptr;
        if (!interpreter_only) {
            const auto dispatch_preparation = ee_dispatch_preparation_cache.prepare(state.pc());
            compiled_preparation = dispatch_preparation.compiled_preparation;
            compiled_candidate = femain_retail_consume_acceleration_blocked ? nullptr : compiled_preparation.block();
            if (dispatch_preparation.native_candidate != nullptr && !collision_scalar_target && !vehicle_scalar_target &&
                !spark_scalar_target && !femain_scalar_target && !femain_retail_consume_acceleration_blocked &&
                state.pc() != runtime::slus_21050::SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress &&
                ui_draw_session_feed.can_dispatch_atomic_native_function(ui_draw_execution_context, state.pc())) {
                native_candidate = dispatch_preparation.native_candidate;
            }
        }
        static const bool relaxed_pending_poll = []() {
            const char* const value = std::getenv("TAKEDOWN_SCALAR_POLL_IGNORE_PENDING");
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }();
        static const bool relaxed_delay_slot_dispatch = []() {
            const char* const value = std::getenv("TAKEDOWN_DELAY_SLOT_DISPATCH");
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }();
        std::optional<std::uint64_t> active_alarm_deadline{};
        if (!interpreter_only && (native_candidate != nullptr || compiled_candidate != nullptr)) {
            active_alarm_deadline = kernel.next_alarm_deadline();
        }
        runtime::NativeFunctionDispatchResult native_dispatch{};
        if (!interpreter_only && native_candidate != nullptr) {
            runtime::NativeFunctionPreflightState preflight_state{
                .committed_instructions = committed_steps,
                .instruction_budget = execution_step_limit,
                .ee_cycles = emulated_ee_cycles,
                .next_vblank_cycle = next_vblank_start,
                .next_alarm_cycle = active_alarm_deadline,
                .queued_dmac_completions = relaxed_pending_poll ? 0U : memory.queued_dmac_completion_count(),
                .gif_dma_in_flight = !relaxed_pending_poll && memory.gif_dmac_in_flight(),
                .vif1_dma_in_flight = !relaxed_pending_poll && memory.vif1_dmac_in_flight(),
                .pending_dmac_callbacks = relaxed_pending_poll ? 0U : kernel.pending_dmac_callback_count(),
                .pending_sif_callbacks = relaxed_pending_poll ? 0U : kernel.pending_sif_command_callback_count(),
                .sif_callback_poll_can_progress = relaxed_pending_poll ? false : kernel.sif_command_callback_poll_can_progress(),
                .sif_dma_completion_pending = kernel.sif_dma_completion_pending(),
                .next_sif_dma_completion_cycle = kernel.next_sif_dma_completion_deadline(),
                .competing_ready_thread = kernel.has_competing_ready_thread(),
                .pending_intc_causes = relaxed_pending_poll ? 0U : memory.pending_intc_causes(),
                .executing_delay_slot = state.is_executing_delay_slot(),
            };
            auto preflight = runtime::check_native_function_preflight(native_candidate->limits, native_candidate->traits,
                                                                      preflight_state);
            const bool exact_cost_probe =
                native_candidate->has_dynamic_cost_resolver && preflight != runtime::NativeFunctionPreflightStatus::Ready &&
                runtime::native_function_preflight_allows_exact_cost_probe(preflight, native_candidate->traits,
                                                                           preflight_state);
            bool preflight_blocked = preflight != runtime::NativeFunctionPreflightStatus::Ready && !exact_cost_probe;
            if (preflight == runtime::NativeFunctionPreflightStatus::Ready || exact_cost_probe) {
                native_preparation = native_function_dispatcher.prepare(state.pc(), state, memory);
                if (native_preparation.prepared()) {
                    if (exact_cost_probe) {
                        preflight = runtime::check_native_function_preflight(native_preparation.limits,
                                                                             native_candidate->traits, preflight_state);
                        preflight_blocked = preflight != runtime::NativeFunctionPreflightStatus::Ready;
                    }
                    if (preflight == runtime::NativeFunctionPreflightStatus::Ready) {
                        native_dispatch = native_function_dispatcher.dispatch(native_preparation, state, memory);
                    }
                }
            }
            if (!native_dispatch.handled() && preflight_blocked) {
                ++native_function_preflight_counts[static_cast<std::size_t>(preflight)];
                if (compiled_block_fusion_profile_directory != nullptr &&
                    preflight == runtime::NativeFunctionPreflightStatus::DmacWork) {
                    const auto sources = runtime::native_function_dma_work_sources(native_candidate->traits, preflight_state);
                    ++native_dma_fallback_sources[dispatch_pc][static_cast<std::size_t>(sources)];
                }
                native_gap_preflight = preflight;
            }
        }
        native_stage_phase.finish();
        if (native_dispatch.handled() && flush_claim_tracking) {
            vif_packet_ring_flush_claim_tracker.observe_post_native_execution(dispatch_pc, state, memory,
                                                                              ui_draw_execution_context);
        }
        if (native_dispatch.handled() && dispatch_pc == runtime::slus_21050::kSubmitPlainTextLinesAddress) {
            ui_draw_packet_session_adapter.expire_unclaimed_plain_text_entry(
                runtime::slus_21050::kSubmitPlainTextLinesAddress);
        }
        if (native_dispatch.handled()) {
            const std::uint64_t retired = native_dispatch.execution.retired_instructions;
            observe_profiled_ee_span(committed_steps, dispatch_pc, retired, runtime::EePcSampleKind::NativeFunction);
            committed_steps += retired;
            executed_instructions += retired;
            emulated_ee_cycles += native_dispatch.execution.ee_cycles;
            ++native_function_calls;
            native_function_retired_instructions += retired;
            ++native_function_hits[native_dispatch.function->address];
            if (native_dispatch.function->traits.defers_dmac_completions &&
                native_dispatch.execution.retired_instructions > 1U) {
                memory.advance_dmac_completion_delays(native_dispatch.execution.retired_instructions - 1U);
            }
            continue;
        }
        compiled_execution_boundary = {};
        runtime::CompiledBlockDispatchResult compiled_dispatch{};
        auto compiled_stage_phase = interpreter_only ? runtime::EeHostPhaseProfiler::Scope{}
        : begin_profiled_host_phase(runtime::EeHostPhase::CompiledStage);
        if (compiled_candidate != nullptr) {
            std::uint64_t compiled_budget = 8U;
            const std::uint64_t remaining_step_budget = execution_step_limit - committed_steps;
            if (remaining_step_budget < compiled_budget) {
                compiled_budget = remaining_step_budget;
            }
            if (next_vblank_start > emulated_ee_cycles) {
                compiled_budget = std::min<std::uint64_t>(next_vblank_start - emulated_ee_cycles, compiled_budget);
            } else {
                compiled_budget = 1U;
            }
            if (active_alarm_deadline.has_value()) {
                if (*active_alarm_deadline > emulated_ee_cycles) {
                    compiled_budget = std::min<std::uint64_t>(*active_alarm_deadline - emulated_ee_cycles, compiled_budget);
                } else {
                    compiled_budget = 1U;
                }
            }
            const bool graphics_dmac_in_flight = memory.gif_dmac_in_flight() || memory.vif1_dmac_in_flight();
            bool inert_dmac_poll = false;
            if (elide_inert_dmac_poll &&
                runtime::can_elide_inert_dmac_poll(state, memory, kernel, compiled_candidate->traits,
                                                   compiled_block_dispatcher.load_fusion_enabled())) {
                ++inert_dmac_poll_admissions;
                inert_dmac_poll_admitted_channels |= kernel.pending_dmac_callback_mask();
                inert_dmac_poll = true;
            }
            const runtime::CompiledSchedulerPollState scheduler_poll =
                runtime::inspect_compiled_scheduler_poll(memory, kernel, inert_dmac_poll);
            if (retail_trace || (instance_batch_capture.enabled() && instance_batch_capture.active()) ||
                (state.is_executing_delay_slot() &&
                 (!relaxed_delay_slot_dispatch || state.next_pc() != state.pc() + 4U)) ||
                native_candidate != nullptr || scheduler_poll.requires_scalar_poll(relaxed_pending_poll)) {
                compiled_budget = 1U;
            }
            bool successor_entry_limit = false;
            bool ui_session_boundary_limit = false;
            for (std::uint64_t offset = 1U; offset < compiled_budget; ++offset) {
                const guest::GuestAddress candidate = state.pc() + static_cast<std::uint32_t>(offset) * 4U;
                const bool dirty_dnas_trampoline = candidate == guest::DirtyDnasCompatHle::kStartTrampoline ||
                    candidate == guest::DirtyDnasCompatHle::kUpdateTrampoline ||
                    candidate == guest::DirtyDnasCompatHle::kStopTrampoline;
                const bool ui_session_entry_boundary =
                    candidate == runtime::slus_21050::SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress;
                if (native_function_dispatcher.lookup(candidate) != nullptr || dirty_dnas_trampoline ||
                    ui_session_entry_boundary ||
                    candidate == runtime::slus_21050::VifPacketRingFlushClaimTracker::kFlushEntryAddress ||
                    instance_batch_capture.wants_pc(candidate)) {
                    compiled_budget = offset;
                    successor_entry_limit = offset == 1U;
                    ui_session_boundary_limit = ui_session_entry_boundary;
                    break;
                }
            }
            if (ui_draw_session_feed.active()) {
                const std::uint64_t budget_before_ui_boundary = compiled_budget;
                compiled_budget = ui_draw_session_feed.clip_compiled_budget(
                    state.pc(), static_cast<std::uint32_t>(budget_before_ui_boundary), ui_draw_execution_context);
                ui_session_boundary_limit = ui_session_boundary_limit || compiled_budget < budget_before_ui_boundary;
            }
            if (flush_claim_tracking) {
                compiled_budget = vif_packet_ring_flush_claim_tracker.clip_compiled_budget(
                    state.pc(), static_cast<std::uint32_t>(compiled_budget), ui_draw_execution_context);
            }
            compiled_budget = std::max<std::uint64_t>(compiled_budget, 1U);
            if (compiled_budget_profiler != nullptr && profile_timing_active &&
                compiled_budget_profiler->observe_budget(compiled_budget)) {
                std::uint32_t limiters = 0U;
                const auto mark = [&limiters](const std::uint32_t bit) { limiters |= bit; };
                if (execution_step_limit - committed_steps <= 1U) {
                    mark(1U);
                }
                if (next_vblank_start <= emulated_ee_cycles || next_vblank_start - emulated_ee_cycles == 1U) {
                    mark(2U);
                }
                if (active_alarm_deadline.has_value() &&
                    (*active_alarm_deadline <= emulated_ee_cycles || *active_alarm_deadline - emulated_ee_cycles == 1U)) {
                    mark(4U);
                }
                if (retail_trace) {
                    mark(8U);
                }
                if (state.is_executing_delay_slot() &&
                    (!relaxed_delay_slot_dispatch || state.next_pc() != state.pc() + 4U)) {
                    mark(16U);
                }
                if (native_candidate != nullptr) {
                    mark(32U);
                }
                if (scheduler_poll.sif_dma_completion_pending) {
                    mark(4096U);
                }
                if (scheduler_poll.competing_ready_thread) {
                    mark(8192U);
                }
                const std::uint32_t pending_intc = memory.pending_intc_causes();
                if (!relaxed_pending_poll) {
                    if (memory.queued_dmac_completion_count() != 0U) {
                        mark(64U);
                    }
                    if (kernel.pending_dmac_callback_count() != 0U && !inert_dmac_poll) {
                        mark(128U);
                    }
                    if (kernel.pending_sif_command_callback_count() != 0U) {
                        mark(256U);
                    }
                    if (kernel.sif_command_callback_poll_can_progress()) {
                        mark(512U);
                    }
                    if (pending_intc != 0U) {
                        mark(1024U);
                    }
                }
                if (successor_entry_limit) {
                    mark(2048U);
                }
                if (ui_session_boundary_limit) {
                    mark(16384U);
                }
                runtime::CompiledLoadGuardState guard = runtime::CompiledLoadGuardState::Absent;
                if (compiled_candidate->traits.load_fusion_guard != nullptr) {
                    guard = !compiled_block_dispatcher.load_fusion_enabled()
                        ? runtime::CompiledLoadGuardState::Disabled
                        : compiled_candidate->traits.load_fusion_guard(state, memory)
                            ? runtime::CompiledLoadGuardState::Passed
                            : runtime::CompiledLoadGuardState::Denied;
                }
                compiled_budget_profiler->record_sample({
                    .pc = state.pc(),
                    .budget = static_cast<std::uint32_t>(compiled_budget),
                    .limiters = limiters,
                    .pending_intc_causes = pending_intc,
                    .load_guard = guard,
                });
            }
            // RECOVERY: parts/010_helpers.cpp declares the *_scalar_target helpers without noexcept, so each pointer is
            // converted to the noexcept barrier type with reinterpret_cast; the original helpers must be noexcept.
            using ScalarTarget = bool (*)(guest::GuestAddress);
            ScalarTarget scalar_target = nullptr;
            const bool vehicle_probe = vehicle_world_contact_probe != nullptr;
            if (femain_dynamic_callback_probe != nullptr) {
                if (collision_trace != nullptr && spark_trace != nullptr) {
                    scalar_target = vehicle_probe ? focused_or_vehicle_world_contact_or_femain_scalar_target
                                                  : focused_or_femain_dynamic_callback_scalar_target;
                } else if (collision_trace != nullptr) {
                    scalar_target = vehicle_probe ? collision_or_vehicle_world_contact_or_femain_scalar_target
                                                  : collision_or_femain_dynamic_callback_scalar_target;
                } else if (spark_trace != nullptr) {
                    scalar_target = vehicle_probe ? spark_or_vehicle_world_contact_or_femain_scalar_target
                                                  : spark_or_femain_dynamic_callback_scalar_target;
                } else {
                    scalar_target = vehicle_probe ? vehicle_world_contact_or_femain_scalar_target
                                                  : is_femain_dynamic_callback_scalar_target;
                }
            } else if (collision_trace != nullptr && spark_trace != nullptr) {
                scalar_target = vehicle_probe ? focused_or_vehicle_world_contact_scalar_target : focused_trace_target;
            } else if (collision_trace != nullptr) {
                scalar_target = vehicle_probe ? collision_or_vehicle_world_contact_scalar_target
                                              : runtime::Slus21050CollisionTrace::is_target;
            } else if (spark_trace != nullptr) {
                scalar_target = vehicle_probe ? spark_or_vehicle_world_contact_scalar_target
                                              : runtime::Slus21050SparkTrace::is_target;
            } else if (vehicle_probe) {
                scalar_target = vehicle_world_contact_probe_scalar_target;
            }
            const runtime::CompiledBlockScalarBarrier focused_scalar_barrier =
                reinterpret_cast<runtime::CompiledBlockScalarBarrier>(scalar_target);
            runtime::GraphicsDmacInstructionProgress graphics_progress{};
            graphics_progress.callback_route = runtime::EeGraphicsCallbackRoute::CompiledBlock;
            graphics_progress.boundary_drain = [](void* context) noexcept {
                return decltype(drain_femain_live_source_context){}(*static_cast<FEmainLiveSourceDrainContext*>(context));
            };
            graphics_progress.memory = &memory;
            graphics_progress.diagnostic_kernel = profile_timing_active ? &kernel : nullptr;
            graphics_progress.profiler = profile_timing_active ? ee_host_phase_profiler.get() : nullptr;
            graphics_progress.diagnostics = profile_timing_active ? ee_graphics_service_diagnostics.get() : nullptr;
            graphics_progress.dispatch_start_cycle = emulated_ee_cycles;
            graphics_progress.vif1_serviced_cycle = &vif1_dmac_serviced_ee_cycles;
            graphics_progress.gif_serviced_cycle = &gif_dmac_serviced_ee_cycles;
            graphics_progress.boundary_drain_context = &host_femain_live_source_drain_context;
            const bool profiled_graphics_service =
                profile_timing_active && (ee_host_phase_profiler != nullptr || ee_graphics_service_diagnostics != nullptr);
            const runtime::CompiledBlockProgressCallback graphics_service_callback =
                femain_live_transaction_enabled
                    ? (profiled_graphics_service
                           ? runtime::service_profiled_graphics_after_direct_instruction_with_boundary_drain
                           : runtime::service_graphics_after_direct_instruction_with_boundary_drain)
                    : (profiled_graphics_service ? runtime::service_profiled_graphics_after_direct_instruction
                                                 : runtime::service_graphics_after_direct_instruction);
            const runtime::CompiledBlockProgressCheckpoint graphics_progress_checkpoint{
                !graphics_dmac_in_flight ? nullptr : graphics_service_callback,
                !graphics_dmac_in_flight ? nullptr : &graphics_progress};
            const bool delay_slot_pair = relaxed_delay_slot_dispatch && compiled_budget != 1U &&
                state.is_executing_delay_slot() && state.next_pc() == state.pc() + 4U;
            if (compiled_budget == 1U && !delay_slot_pair) {
                compiled_dispatch = compiled_block_dispatcher.dispatch_one(compiled_preparation, state, memory,
                                                                           focused_scalar_barrier);
            } else {
                compiled_dispatch = compiled_block_dispatcher.dispatch(
                    compiled_preparation, state, memory,
                {static_cast<std::uint32_t>(compiled_budget), static_cast<std::uint32_t>(compiled_budget)},
                focused_scalar_barrier, graphics_progress_checkpoint);
            }
        }
        compiled_stage_phase.finish();
        if (compiled_dispatch.covered() && flush_claim_tracking) {
            vif_packet_ring_flush_claim_tracker.observe_post_instruction(dispatch_pc, state, memory,
                                                                        ui_draw_execution_context);
        }
        if (compiled_dispatch.covered()) {
            ++compiled_block_dispatches;
            const std::uint32_t retired = compiled_dispatch.execution.retired_instructions;
            observe_profiled_ee_span(committed_steps, dispatch_pc, retired,
                                     takedown::runtime::EePcSampleKind::CompiledBlock);
            committed_steps += retired;
            executed_instructions += retired;
            const std::uint32_t cycles = compiled_dispatch.execution.ee_cycles;
            emulated_ee_cycles += cycles;
            compiled_block_retired_instructions += retired;
            compiled_block_ee_cycles += cycles;
            if (retired > 1U) {
                ++compiled_block_fused_dispatches;
                compiled_block_fused_instructions += retired;
            }
            switch (compiled_dispatch.execution.exit) {
                case takedown::runtime::CompiledBlockExit::Safepoint:
                case takedown::runtime::CompiledBlockExit::BudgetExhausted:
                continue;
                case takedown::runtime::CompiledBlockExit::GuestSystemCall:
                compiled_execution_boundary = takedown::guest::EeStepResult{
                    .status = takedown::guest::EeStepStatus::GuestSystemCall,
                    .instruction = takedown::guest::decode_r5900(state.pc(), memory.read_u32_le(state.pc()))};
                break;
                case takedown::runtime::CompiledBlockExit::Fault: {
                    const auto& fault = *compiled_dispatch.execution.fault;
                    compiled_execution_boundary =
                        takedown::guest::EeStepResult{fault.status, fault.instruction, fault.vu0_micro};
                    break;
                }
            }
        }
        if (compiled_dispatch.status == takedown::runtime::CompiledBlockDispatchStatus::CodeMismatch) {
            ++compiled_block_code_mismatches;
        }
        if (native_only && !compiled_dispatch.covered()) {
            stopped_at_native_gap = true;
            native_gap_address = state.pc();
            if (native_candidate != nullptr) {
                native_gap_function_name = native_candidate->name;
            }
            if (compiled_dispatch.status == takedown::runtime::CompiledBlockDispatchStatus::CodeMismatch) {
                native_gap_block_name = compiled_dispatch.block->name;
                native_gap_expected_word = compiled_dispatch.block->expected_retail_word;
                if (memory.contains(state.pc(), sizeof(std::uint32_t))) {
                    native_gap_observed_word = memory.read_u32_le(state.pc());
                }
            }
            break;
        }
        takedown::guest::EeStepResult result{};
        if (scalar_cold_observers_enabled && !scalar_observers_configured && !flush_claim_tracking) {
            memory.set_current_ee_pc(state.pc());
            if (compiled_execution_boundary.has_value()) {
                result = *compiled_execution_boundary;
            } else {
                result = takedown::guest::step_r5900(state, memory, instruction_cache);
                ++scalar_observer_bypass_steps;
#if defined(TAKEDOWN_NATIVE_ONLY_EXECUTABLE)
#else
                if (scalar_burst_enabled && result.status == takedown::guest::EeStepStatus::Executed) {
                    auto burst_pc = state.pc();
                    if (!scalar_burst_hooks.marked(burst_pc) && scalar_burst_may_start()) {
                        auto burst_budget = scalar_burst_horizon(execution_step_limit);
                        const bool burst_ui_session = ui_draw_session_feed.active();
                        const auto burst_ui_context = burst_ui_session
                                                          ? current_ui_execution_context()
                                                          : takedown::runtime::slus_21050::SubmitUiDrawBucketsExecutionContext{};
                        const auto burst_ram_watch = scalar_burst_ram_watch(committed_steps + 1U);
                        if (burst_budget != 0U && !burst_ram_watch.fires() &&
                            !(burst_ui_session &&
                              ui_draw_session_feed.requires_pre_instruction(burst_ui_context, burst_pc))) {
                            constexpr std::size_t kCop0Status = 12U;
                            const auto burst_hardware_activity = memory.hardware_activity_epoch();
                            auto burst_cop0_status = state.read_cop0(kCop0Status);
                            const auto advance_idle_vif1 = [&memory](const std::uint64_t cycles) {
                                memory.advance_idle_vif1_dmac(cycles);
                            };
                            const auto advance_idle_gif = [&memory](const std::uint64_t cycles) {
                                memory.advance_idle_gif_dmac(cycles);
                            };
                            ++scalar_burst.bursts;
                            scalar_burst.note_start(burst_budget);
                            auto burst_stop = takedown::runtime::ScalarBurstStop::Horizon;
                            const bool burst_ee_aot = ee_aot_enabled && (ee_aot_in_ui_session || !burst_ui_session) &&
                                                      !burst_ram_watch.unmirrored;
                            if (burst_ee_aot) {
                                takedown::guest::ee_aot_update_session_stops(
                                    burst_ui_session, ui_draw_session_feed.learned_callback_target(),
                                    ui_draw_session_feed.learned_caller_return());
                            }
                            const auto burst_prepare_native = [&](const takedown::guest::GuestAddress pc, const std::size_t pending,
                                                                  bool& outer_loop_declines, const bool count_preflight = true)
                                -> takedown::runtime::NativeFunctionPreparation {
                                outer_loop_declines = false;
                                const auto* const candidate = native_function_dispatcher.lookup(pc);
                                if (candidate == nullptr || !candidate->traits.scheduler_state_transparent) {
                                    return {};
                                }
                                if (interpreter_only || femain_retail_consume_acceleration_blocked ||
                                    !ui_draw_session_feed.can_dispatch_atomic_native_function(
                                        takedown::runtime::slus_21050::SubmitUiDrawBucketsExecutionContext{}, pc)) {
                                    outer_loop_declines = true;
                                    return {};
                                }
                                const takedown::runtime::NativeFunctionPreflightState burst_preflight_state{
                                    .committed_instructions = committed_steps + pending,
                                    .instruction_budget = execution_step_limit,
                                    .ee_cycles = emulated_ee_cycles + pending,
                                    .next_vblank_cycle = next_vblank_start,
                                    .next_alarm_cycle = kernel.next_alarm_deadline(),
                                    .queued_dmac_completions =
                                        relaxed_pending_poll ? 0U : memory.queued_dmac_completion_count(),
                                    .gif_dma_in_flight = !relaxed_pending_poll && memory.gif_dmac_in_flight(),
                                    .vif1_dma_in_flight = !relaxed_pending_poll && memory.vif1_dmac_in_flight(),
                                    .pending_dmac_callbacks =
                                        relaxed_pending_poll ? 0U : kernel.pending_dmac_callback_count(),
                                    .pending_sif_callbacks =
                                        relaxed_pending_poll ? 0U : kernel.pending_sif_command_callback_count(),
                                    .sif_callback_poll_can_progress =
                                        relaxed_pending_poll ? false : kernel.sif_command_callback_poll_can_progress(),
                                    .sif_dma_completion_pending = kernel.sif_dma_completion_pending(),
                                    .next_sif_dma_completion_cycle = kernel.next_sif_dma_completion_deadline(),
                                    .competing_ready_thread = kernel.has_competing_ready_thread(),
                                    .pending_intc_causes = relaxed_pending_poll ? 0U : memory.pending_intc_causes(),
                                    .executing_delay_slot = state.is_executing_delay_slot(),
                                };
                                const auto preflight = takedown::runtime::check_native_function_preflight(
                                    candidate->limits, candidate->traits, burst_preflight_state);
                                if (preflight != takedown::runtime::NativeFunctionPreflightStatus::Ready) {
                                    outer_loop_declines =
                                        !(candidate->has_dynamic_cost_resolver &&
                                          takedown::runtime::native_function_preflight_allows_exact_cost_probe(
                                              preflight, candidate->traits, burst_preflight_state));
                                    if (outer_loop_declines && count_preflight) {
                                        ++native_function_preflight_counts[static_cast<std::size_t>(preflight)];
                                    }
                                    return {};
                                }
                                const auto preparation = native_function_dispatcher.prepare(pc, state, memory);
                                outer_loop_declines = !preparation.prepared();
                                return preparation;
                            };
                            // The burst loop below, for the case the compiled run stops at a native entry
                            // (see takedown::guest::EeAotNativeHook): the post-instruction checks, the native
                            // call and the loop's bookkeeping around it, done without leaving the run, then
                            // on in compiled code. Anything not plainly the ordinary path is handed back
                            // unchanged for the loop itself to decide.
                            const auto burst_native_hook = [&](const std::uint64_t executed,
                                                               const takedown::guest::GuestAddress last_pc)
                                -> takedown::guest::EeAotNativeHookResult {
                                using Kind = takedown::guest::EeAotNativeHookResult::Kind;
                                if (state.is_executing_delay_slot() || burst_budget <= executed) {
                                    return {};
                                }
                                if (!scalar_burst_native_entries.contains(state.pc())) {
                                    return {};
                                }
                                // The progress of the instructions before the last one (ee_aot_burst_step's).
                                if (const std::uint64_t skipped = executed - 1U; skipped != 0U) {
                                    committed_steps += skipped;
                                    executed_instructions += skipped;
                                    emulated_ee_cycles += skipped;
                                    takedown::runtime::service_graphics_dmacs_until(
                                        emulated_ee_cycles, vif1_dmac_serviced_ee_cycles, gif_dmac_serviced_ee_cycles,
                                        advance_idle_vif1, advance_idle_gif);
                                    if (host_window != nullptr) {
                                        host_window_loop_iterations += skipped;
                                    }
                                    kernel.set_ee_cycle_count(emulated_ee_cycles);
                                    memory.set_current_guest_step(committed_steps);
                                }
                                memory.set_current_ee_pc(last_pc);
                                bool pending = true;
                                std::uint64_t counted = executed; // not yet in the burst's counters
                                bool called = false;
                                for (;;) {
                                    const auto hand_back = [&]() -> takedown::guest::EeAotNativeHookResult {
                                        return {called ? Kind::HandOffNativeDone : Kind::HandOffAccounted, 0U, 0U};
                                    };
                                    // What the loop checks after an instruction or a native call.
                                    if (memory.hardware_activity_epoch() != burst_hardware_activity ||
                                        burst_ram_watch.fires()) {
                                        return hand_back();
                                    }
                                    if (state.read_cop0(kCop0Status) != burst_cop0_status) {
                                        if (!scalar_burst_may_start()) {
                                            return hand_back();
                                        }
                                        burst_cop0_status = state.read_cop0(kCop0Status);
                                    }
                                    if (!pending && !scalar_burst_may_start()) {
                                        return hand_back();
                                    }
                                    const auto next_pc = state.pc();
                                    takedown::runtime::NativeFunctionPreparation prepared{};
                                    if (scalar_burst_hooks.marked(next_pc)) {
                                        if (!scalar_burst_native_entries.contains(next_pc) ||
                                            burst_budget - counted == 0U) {
                                            return hand_back();
                                        }
                                        bool declines = false;
                                        prepared = burst_prepare_native(next_pc, pending ? 1U : 0U, declines, false);
                                        if (!prepared.prepared()) {
                                            return hand_back();
                                        }
                                    } else if (burst_budget - counted == 0U) {
                                        return hand_back();
                                    }
                                    // The loop's next iteration.
                                    if (counted != 0U) {
                                        scalar_observer_bypass_steps += counted;
                                        scalar_burst.instructions += counted;
                                        burst_budget -= counted;
                                        counted = 0U;
                                    }
                                    if (pending) {
                                        ++committed_steps;
                                        ++executed_instructions;
                                        ++emulated_ee_cycles;
                                    }
                                    takedown::runtime::service_graphics_dmacs_until(
                                        emulated_ee_cycles, vif1_dmac_serviced_ee_cycles, gif_dmac_serviced_ee_cycles,
                                        advance_idle_vif1, advance_idle_gif);
                                    if (host_window != nullptr) {
                                        ++host_window_loop_iterations;
                                    }
                                    kernel.set_ee_cycle_count(emulated_ee_cycles);
                                    memory.set_current_ee_pc(next_pc);
                                    memory.set_current_guest_step(committed_steps);
                                    if (!prepared.prepared()) {
                                        return {Kind::Continue, burst_budget, committed_steps};
                                    }
                                    const auto native_call = burst_dispatch_trust
                                                                 ? native_function_dispatcher.dispatch_prepared(prepared, state, memory)
                                                                 : native_function_dispatcher.dispatch(prepared, state, memory);
                                    pending = false;
                                    called = true;
                                    if (!native_call.handled()) {
                                        if (host_window != nullptr) {
                                            --host_window_loop_iterations;
                                        }
                                        return {Kind::HandOffUnhandled, 0U, 0U};
                                    }
                                    const auto retired = native_call.execution.retired_instructions;
                                    const auto cycles = native_call.execution.ee_cycles;
                                    observe_profiled_ee_span(committed_steps, next_pc, retired,
                                                             takedown::runtime::EePcSampleKind::NativeFunction);
                                    committed_steps += retired;
                                    executed_instructions += retired;
                                    emulated_ee_cycles += cycles;
                                    ++native_function_calls;
                                    native_function_retired_instructions += retired;
                                    ++native_function_hits[native_call.function->address];
                                    ++scalar_burst.native_calls;
                                    if (native_call.function->traits.defers_dmac_completions && retired > 1U) {
                                        memory.advance_dmac_completion_delays(retired - 1U);
                                    }
                                    const std::uint64_t native_cost = std::max(retired, cycles);
                                    burst_budget = burst_budget > native_cost ? burst_budget - native_cost : 0U;
                                }
                            };
                            struct BurstNativeHookScope {
                                bool installed{};
                                ~BurstNativeHookScope() {
                                    if (installed) {
                                        takedown::guest::ee_aot_set_native_hook(nullptr, nullptr);
                                    }
                                }
                            } burst_native_hook_scope;
                            if (burst_ee_aot && burst_native_fast && burst_native_calls && !burst_ui_session &&
                                !scalar_burst_audit) {
                                takedown::guest::ee_aot_set_native_hook(
                                    [](void* const user, const std::uint64_t executed, const takedown::guest::GuestAddress last_pc) {
                                        return (*static_cast<decltype(burst_native_hook)*>(user))(executed, last_pc);
                                    },
                                    const_cast<void*>(static_cast<const void*>(&burst_native_hook)));
                                burst_native_hook_scope.installed = true;
                            }
                            bool burst_pending = true;
                            takedown::runtime::NativeFunctionPreparation burst_native{};
                            auto burst_declined_entry = takedown::guest::kEeAotNoDeclinedEntry;
                            for (;;) {
                                if (burst_pending) {
                                    ++committed_steps;
                                    ++executed_instructions;
                                    ++emulated_ee_cycles;
                                }
                                takedown::runtime::service_graphics_dmacs_until(
                                    emulated_ee_cycles, vif1_dmac_serviced_ee_cycles, gif_dmac_serviced_ee_cycles,
                                    advance_idle_vif1, advance_idle_gif);
                                if (host_window != nullptr) {
                                    ++host_window_loop_iterations;
                                }
                                kernel.set_ee_cycle_count(emulated_ee_cycles);
                                memory.set_current_ee_pc(burst_pc);
                                memory.set_current_guest_step(committed_steps);
                                if (burst_native.prepared()) {
                                    const auto native_call = burst_dispatch_trust
                                                                 ? native_function_dispatcher.dispatch_prepared(burst_native, state, memory)
                                                                 : native_function_dispatcher.dispatch(burst_native, state, memory);
                                    burst_native = {};
                                    burst_pending = false;
                                    if (!native_call.handled()) {
                                        if (host_window != nullptr) {
                                            --host_window_loop_iterations;
                                        }
                                        burst_stop = takedown::runtime::ScalarBurstStop::Hook;
                                        break;
                                    }
                                    const auto retired = native_call.execution.retired_instructions;
                                    const auto cycles = native_call.execution.ee_cycles;
                                    observe_profiled_ee_span(committed_steps, burst_pc, retired,
                                                             takedown::runtime::EePcSampleKind::NativeFunction);
                                    committed_steps += retired;
                                    executed_instructions += retired;
                                    emulated_ee_cycles += cycles;
                                    ++native_function_calls;
                                    native_function_retired_instructions += retired;
                                    ++native_function_hits[native_call.function->address];
                                    ++scalar_burst.native_calls;
                                    if (native_call.function->traits.defers_dmac_completions && retired > 1U) {
                                        memory.advance_dmac_completion_delays(retired - 1U);
                                    }
                                    const std::uint64_t native_cost = std::max(retired, cycles);
                                    burst_budget = burst_budget > native_cost ? burst_budget - native_cost : 0U;
                                } else {
                                    std::uint8_t aot_outcome = takedown::guest::kEeAotRunOrdinary;
                                    if (const auto compiled = burst_ee_aot
                                                                  ? ee_aot_burst_step(result, burst_budget, burst_declined_entry, aot_outcome)
                                                                  : 0U;
                                    aot_outcome == takedown::guest::kEeAotRunNativeDone) {
                                        // The run made native calls and ended with none of its own instructions pending.
                                        burst_pending = false;
                                    } else if (aot_outcome == takedown::guest::kEeAotRunNativeUnhandled) {
                                        burst_pending = false;
                                        burst_stop = takedown::runtime::ScalarBurstStop::Hook;
                                        break;
                                    } else if (compiled != 0U) {
                                        burst_pending = true;
                                        scalar_observer_bypass_steps += compiled;
                                        scalar_burst.instructions += compiled;
                                        burst_budget -= compiled;
                                    } else {
                                        burst_pending = true;
                                        result = takedown::guest::step_r5900(state, memory, instruction_cache);
                                        ++scalar_observer_bypass_steps;
                                        ++scalar_burst.instructions;
                                        --burst_budget;
                                    }
                                }
                                if (burst_pending && result.status != takedown::guest::EeStepStatus::Executed) {
                                    // System call or fault: the ordinary tail below runs for real.
                                    burst_stop = takedown::runtime::ScalarBurstStop::Result;
                                    break;
                                }
                                if (memory.hardware_activity_epoch() != burst_hardware_activity ||
                                    (burst_pending && result.vu0_micro.has_value())) {
                                    burst_stop = takedown::runtime::ScalarBurstStop::Hardware;
                                    break;
                                }
                                if (burst_ram_watch.fires()) {
                                    // A polled guest RAM word changed.
                                    burst_stop = takedown::runtime::ScalarBurstStop::State;
                                    break;
                                }
                                if (state.read_cop0(kCop0Status) != burst_cop0_status) {
                                    if (!scalar_burst_may_start()) {
                                        burst_stop = takedown::runtime::ScalarBurstStop::State;
                                        break;
                                    }
                                    burst_cop0_status = state.read_cop0(kCop0Status);
                                }
                                if (!burst_pending && !scalar_burst_may_start()) {
                                    burst_stop = takedown::runtime::ScalarBurstStop::State;
                                    break;
                                }
                                burst_pc = state.pc();
                                burst_declined_entry = takedown::guest::kEeAotNoDeclinedEntry;
                                if (scalar_burst_hooks.marked(burst_pc) ||
                                    (burst_ui_session &&
                                     ui_draw_session_feed.requires_pre_instruction(burst_ui_context, burst_pc))) {
                                    bool burst_outer_loop_declines = false;
                                    if (burst_native_calls && burst_budget != 0U && !burst_ui_session &&
                                        scalar_burst_native_entries.contains(burst_pc)) {
                                        burst_native = burst_prepare_native(burst_pc, burst_pending ? 1U : 0U,
                                                                            burst_outer_loop_declines);
                                    }
                                    if (burst_outer_loop_declines && burst_native_decline_steps) {
                                        ++scalar_burst.native_declines;
                                        takedown::guest::ee_aot_note_native_decline(burst_pc);
                                        burst_declined_entry = burst_pc;
                                    } else {
                                        if (!burst_native.prepared()) {
                                            takedown::guest::ee_aot_note_hook_stop(burst_pc);
                                            burst_stop = takedown::runtime::ScalarBurstStop::Hook;
                                            break;
                                        }
                                    }
                                }
                                if (scalar_burst_audit && burst_pending) {
                                    ++scalar_burst_audit_checks;
                                    const bool quiescent = scalar_burst_may_start();
                                    const auto horizon = scalar_burst_horizon(execution_step_limit);
                                    if (!quiescent || horizon != burst_budget) {
                                        std::ostringstream detail;
                                        detail << "scalar burst audit failed after step " << committed_steps << " at pc "
                                               << hex32(result.instruction.pc) << ": quiescent=" << (quiescent ? "yes" : "no")
                                               << " horizon=" << horizon << " remaining-budget=" << burst_budget;
                                        throw std::logic_error(detail.str());
                                    }
                                }
                                if (burst_budget == 0U) {
                                    break;
                                }
                            }
                            scalar_burst.note_stop(burst_stop);
                            {
                                static const bool report_hook = std::getenv("TAKEDOWN_AOTNATIVE_STATS") != nullptr;
                                if (report_hook && scalar_burst.bursts % 20000U == 0U) {
                                    const auto& stats = takedown::guest::ee_aot_statistics();
                                    std::fprintf(stderr, "[AOTNATIVE] bursts=%llu native_calls=%llu hook_taken=%llu hook_continued=%llu retired=%llu steps=%llu cycles=%llu\n",
                                                 static_cast<unsigned long long>(scalar_burst.bursts), static_cast<unsigned long long>(scalar_burst.native_calls),
                                                 static_cast<unsigned long long>(stats.native_hook_taken), static_cast<unsigned long long>(stats.native_hook_continued),
                                                 static_cast<unsigned long long>(native_function_retired_instructions),
                                                 static_cast<unsigned long long>(committed_steps), static_cast<unsigned long long>(emulated_ee_cycles));
                                }
                            }
                            if (!burst_pending) {
                                continue;
                            }
                        }
                    }
                }
#endif
            }
        } else {
            std::array<std::uint32_t, 4> retail_values_before{};
            if (retail_trace) {
                const std::uint32_t instruction_word = memory.read_u32_le(state.pc());
                if ((instruction_word >> 26U) == 3U) {
                    const takedown::guest::GuestAddress target =
                        ((state.pc() + 4U) & 0xf0000000U) | ((instruction_word << 2U) & 0x0ffffffcU);
                    if (target >= 0x100000U && target < plan.required_memory_size()) {
                        const std::uint64_t key = (static_cast<std::uint64_t>(state.pc()) << 32U) | target;
                        auto& call_site = retail_call_sites
                                              .try_emplace(key, RetailCallSiteTrace{state.pc(), target, 0U, committed_steps, 0U})
                                              .first->second;
                        ++call_site.hits;
                        call_site.last_step = committed_steps;
                    }
                }
                retail_values_before = {memory.read_u32_le(0x01e90424U), memory.read_u32_le(0x01ea2974U),
                                        memory.read_u32_le(0x0051a99cU), memory.read_u32_le(0x0051a9a0U)};
            }
            std::optional<takedown::runtime::Slus21050CollisionTrace::SampleToken> collision_trace_token;
            if (collision_trace != nullptr && collision_trace->is_target(state.pc())) {
                collision_trace_token = collision_trace->begin_sample(
                    state, memory,
                    takedown::runtime::Slus21050CollisionTraceContext{committed_steps, emulated_ee_cycles,
                                                                      raised_vblank_starts, kernel.current_thread_id()});
            }
            std::optional<takedown::runtime::Slus21050VehicleWorldContactEndpointProbe::SampleToken>
                vehicle_world_contact_probe_token;
            if (vehicle_world_contact_probe != nullptr && vehicle_world_contact_probe->is_target(state.pc())) {
                vehicle_world_contact_probe_token = vehicle_world_contact_probe->begin_sample(
                    state, memory,
                    takedown::runtime::Slus21050CollisionTraceContext{committed_steps, emulated_ee_cycles,
                                                                      raised_vblank_starts, kernel.current_thread_id()});
            }
            std::optional<takedown::runtime::Slus21050SparkTrace::SampleToken> spark_trace_token;
            if (spark_trace != nullptr && spark_trace->is_target(state.pc())) {
                spark_trace_token = spark_trace->begin_sample(
                    state, memory,
                    takedown::runtime::Slus21050SparkTraceContext{committed_steps, emulated_ee_cycles,
                                                                  raised_vblank_starts, kernel.current_thread_id()});
            }
            memory.set_current_ee_pc(state.pc());
            takedown::runtime::RetailTimerOwnerAbiTraceContext owner_scalar_context{};
            if (retail_timer_owner_abi_trace.collecting()) {
                owner_scalar_context = current_retail_timer_owner_context();
                retail_timer_owner_abi_trace.observe_pre_instruction(state, memory, committed_steps,
                                                                     emulated_ee_cycles, owner_scalar_context);
            }
            takedown::runtime::RetailTimerConsumerTraceContext scalar_context{};
            if (retail_timer_consumer_trace.collecting()) {
                scalar_context = current_retail_timer_consumer_context();
                retail_timer_consumer_trace.observe_pre_instruction(state, memory, scalar_context, committed_steps,
                                                                    emulated_ee_cycles);
            }
            if (compiled_execution_boundary.has_value()) {
                result = *compiled_execution_boundary;
            } else {
                auto scalar_step_phase = begin_profiled_host_phase(runtime::EeHostPhase::ScalarStep);
                std::optional<ScopedRetailTimerConsumerMemoryWriteCapture> scalar_write_capture;
                if (retail_timer_consumer_trace.collecting()) {
                    scalar_write_capture.emplace(retail_timer_consumer_trace, memory,
                                                 ScopedRetailTimerConsumerMemoryWriteCapture::Kind::Scalar,
                                                 scalar_context);
                }
                const bool femain_dynamic_callback_scalar =
                    femain_dynamic_callback_probe != nullptr && !kernel.guest_callback_active();
                const bool femain_dynamic_callback_hle_dmac =
                    femain_dynamic_callback_probe != nullptr && kernel.guest_callback_active() &&
                    kernel.guest_callback_kind() == takedown::guest::EeKernelHle::GuestCallbackKind::Dmac;
                const auto femain_hle_dmac_callback_identity = kernel.guest_callback_identity();
                const bool femain_frame_closure_scalar =
                    femain_frame_closure_diagnostic != nullptr &&
                    takedown::runtime::Slus21050FemainFrameClosureDiagnostic::is_scalar_target(state.pc()) &&
                    (!kernel.guest_callback_active() ||
                     femain_frame_closure_diagnostic->phase() != takedown::runtime::Slus21050FemainFrameClosurePhase::Idle);
                const bool femain_retail_consume_scalar =
                    femain_retail_consume_observation_adapter != nullptr &&
                    femain_retail_consume_observation_adapter->attachment() ==
                        takedown::runtime::Slus21050FemainRetailConsumeAttachment::Bound &&
                    femain_retail_consume_invocation_barrier != nullptr &&
                    femain_retail_consume_invocation_barrier->scalar_observation_active() &&
                    !kernel.guest_callback_active();
                if (femain_retail_consume_scalar) {
                    femain_retail_consume_observation_adapter->observe_pre_instruction(
                        state, memory,
                        takedown::runtime::Slus21050FemainRetailConsumeObservationContext{
                        committed_steps,
                        femain_retail_consume_invocation_barrier != nullptr
                            ? femain_retail_consume_invocation_barrier->identity()
                            : takedown::runtime::Slus21050FemainRetailConsumeObservationIdentity{},
                        takedown::runtime::Slus21050FemainFrameGuestBoundary{raised_vblank_starts,
                                                                             emulated_ee_cycles / 6144U + 1U}});
                }
                if (femain_frame_closure_scalar) {
                    const auto diagnostic_counters = femain_terminal_diagnostic->counter_snapshot();
                    takedown::runtime::Slus21050FemainFrameClosureSnapshot snapshot{};
                    snapshot.guest_step = committed_steps;
                    snapshot.guest_cycle = emulated_ee_cycles;
                    snapshot.guest_vblank = raised_vblank_starts;
                    snapshot.audio_sample = emulated_ee_cycles / 6144U + 1U;
                    snapshot.execution.thread_id = static_cast<std::uint64_t>(kernel.current_thread_id());
                    snapshot.execution.callback_active = kernel.guest_callback_active();
                    snapshot.execution.callback_identity = kernel.guest_callback_identity();
                    snapshot.execution.callback_activation_generation = kernel.guest_callback_activation_generation();
                    snapshot.observation_epoch = diagnostic_counters.observation_epoch;
                    snapshot.timeline_event_count = diagnostic_counters.timeline_events;
                    snapshot.raw_event_count = diagnostic_counters.raw_quadwords;
                    snapshot.eop_event_count = diagnostic_counters.packet_eop_events;
                    snapshot.primary_command_count = diagnostic_counters.primary_commands;
                    snapshot.worker_dispatch_count = diagnostic_counters.worker_dispatches;
                    snapshot.queue_read = memory.read_u32_le(0x004e2838U);
                    snapshot.queue_end = memory.read_u32_le(0x004e283cU);
                    snapshot.transfers_active = memory.read_u8(0x004e2830U);
                    snapshot.s0 = state.read_gpr_u64(16U);
                    snapshot.s1 = state.read_gpr_u64(17U);
                    snapshot.s2 = state.read_gpr_u64(18U);
                    if (const auto packet = host_femain_live_transaction.owner().publication_packet(); packet.has_value()) {
                        snapshot.publication = {packet->owner_identity,
                                                packet->publication_serial,
                                                packet->player_identity,
                                                packet->player_session_generation,
                                                packet->ledger_identity,
                                                packet->ledger_session_generation,
                                                packet->published_reservation.frame_generation,
                                                packet->queue.queue_read,
                                                packet->queue.queue_end,
                                                packet->queue.queue_base,
                                                packet->queue.buffer_index,
                                                true};
                    }
                    static_cast<void>(femain_frame_closure_diagnostic->observe(state, memory, snapshot));
                }
                if (femain_dynamic_callback_scalar) {
                    femain_dynamic_callback_probe->observe_pre_instruction(state, memory);
                } else if (femain_dynamic_callback_hle_dmac) {
                    femain_dynamic_callback_probe->observe_hle_dmac_callback_pre_instruction(
                        state, memory, femain_hle_dmac_callback_identity);
                }
                result = takedown::guest::step_r5900(state, memory, instruction_cache);
                if (femain_dynamic_callback_scalar) {
                    femain_dynamic_callback_probe->observe_post_instruction(state, memory, result);
                } else if (femain_dynamic_callback_hle_dmac) {
                    femain_dynamic_callback_probe->observe_hle_dmac_callback_post_instruction(
                        state, memory, result, femain_hle_dmac_callback_identity);
                }
                if (femain_retail_consume_scalar) {
                    femain_retail_consume_observation_adapter->observe_post_instruction(
                        state, memory, result,
                        takedown::runtime::Slus21050FemainRetailConsumeObservationContext{
                        committed_steps,
                        femain_retail_consume_invocation_barrier != nullptr
                            ? femain_retail_consume_invocation_barrier->identity()
                            : takedown::runtime::Slus21050FemainRetailConsumeObservationIdentity{},
                        takedown::runtime::Slus21050FemainFrameGuestBoundary{raised_vblank_starts,
                                                                             emulated_ee_cycles / 6144U + 1U}});
                    if (!femain_retail_consume_invocation_barrier->observe_scalar_step(
                            result, *femain_retail_consume_observation_adapter)) {
                        femain_retail_consume_observation_adapter->close();
                    }
                }
                scalar_write_capture.reset();
                scalar_step_phase.finish();
            }
            if (retail_timer_consumer_trace.collecting()) {
                const std::uint64_t post_guest_steps =
                    result.status == takedown::guest::EeStepStatus::Executed &&
                            committed_steps != std::numeric_limits<std::uint64_t>::max()
                        ? committed_steps + 1U
                        : committed_steps;
                const std::uint64_t post_ee_cycles =
                    result.status == takedown::guest::EeStepStatus::Executed &&
                            emulated_ee_cycles != std::numeric_limits<std::uint64_t>::max()
                        ? emulated_ee_cycles + 1U
                        : emulated_ee_cycles;
                retail_timer_consumer_trace.observe_post_instruction(state, memory, result,
                                                                     current_retail_timer_consumer_context(),
                                                                     post_guest_steps, post_ee_cycles);
            }
            if (retail_timer_owner_abi_trace.active()) {
                const std::uint64_t post_guest_steps =
                    result.status == takedown::guest::EeStepStatus::Executed &&
                            committed_steps != std::numeric_limits<std::uint64_t>::max()
                        ? committed_steps + 1U
                        : committed_steps;
                const std::uint64_t post_ee_cycles =
                    result.status == takedown::guest::EeStepStatus::Executed &&
                            emulated_ee_cycles != std::numeric_limits<std::uint64_t>::max()
                        ? emulated_ee_cycles + 1U
                        : emulated_ee_cycles;
                retail_timer_owner_abi_trace.observe_post_instruction(state, memory, result, post_guest_steps,
                                                                      post_ee_cycles,
                                                                      current_retail_timer_owner_context());
            }
            if (result.status == takedown::guest::EeStepStatus::Executed && flush_claim_tracking) {
                vif_packet_ring_flush_claim_tracker.observe_post_instruction(result.instruction.pc, state, memory,
                                                                            ui_draw_execution_context);
            }
            if (collision_trace_token.has_value()) {
                collision_trace->finish_sample(*collision_trace_token, state,
                                               static_cast<std::uint32_t>(result.status));
            }
            if (vehicle_world_contact_probe_token.has_value()) {
                takedown::runtime::Slus21050CollisionTraceContext vehicle_world_contact_post_context{
                    committed_steps, emulated_ee_cycles, raised_vblank_starts, kernel.current_thread_id()};
                if (result.status == takedown::guest::EeStepStatus::Executed &&
                    committed_steps != std::numeric_limits<std::uint64_t>::max() &&
                    emulated_ee_cycles != std::numeric_limits<std::uint64_t>::max()) {
                    ++vehicle_world_contact_post_context.committed_steps;
                    ++vehicle_world_contact_post_context.ee_cycles;
                }
                const auto vehicle_world_contact_publication_token = vehicle_world_contact_probe->finish_sample(
                    *vehicle_world_contact_probe_token, state, static_cast<std::uint32_t>(result.status),
                    vehicle_world_contact_post_context);
                if (vehicle_world_contact_publication_token.has_value() && native_simulation_shadow != nullptr) {
                    if (!native_simulation_shadow->observe_guest_cycles(vehicle_world_contact_post_context.ee_cycles) ||
                        native_simulation_shadow->publish_vehicle_snapshot_result(
                            *vehicle_world_contact_publication_token) ==
                            takedown::runtime::NativeSimulationVehiclePublicationResult::Rejected) {
                        native_simulation_shadow_stop_requested = true;
                    }
                }
            }
            if (spark_trace_token.has_value()) {
                spark_trace->finish_sample(*spark_trace_token, state, memory, static_cast<std::uint32_t>(result.status));
            }
            if (retail_trace) {
                const std::array<std::uint32_t, 4> retail_values_after{memory.read_u32_le(0x01e90424U),
                                                                 memory.read_u32_le(0x01ea2974U),
                                                                 memory.read_u32_le(0x0051a99cU),
                                                                 memory.read_u32_le(0x0051a9a0U)};
                if (retail_values_before[0] != retail_values_after[0] &&
                    retail_phase_transitions.size() * sizeof(RetailPhaseTransition) <= 360U) {
                    retail_phase_transitions.push_back(RetailPhaseTransition{
                        committed_steps, result.instruction.pc, retail_values_before[0], retail_values_after[0]});
                }
                const auto record_value_transition = [&](auto& transitions, const std::size_t index) {
                    if (retail_values_before[index] != retail_values_after[index] &&
                        transitions.size() * sizeof(RetailValueTransition) <= 744U) {
                        transitions.push_back(RetailValueTransition{committed_steps, result.instruction.pc,
                                                                    retail_values_before[index],
                                                                    retail_values_after[index]});
                    }
                };
                record_value_transition(retail_flow_transitions, 1U);
                record_value_transition(retail_movie_phase_transitions, 2U);
                record_value_transition(retail_movie_state_transitions, 3U);
            }
        }
        if (state.pc() >= 0x00237180U && state.pc() <= 0x0023719cU) {
            ++freeze_spin_entries;
            const std::uint32_t spin_target = state.read_gpr_u32(16U);
            if (freeze_last_spin_target != spin_target) {
                if (retail_trace) {
                    std::cout << "  FREEZE-PROBE: spin at pc=" << hex32(state.pc()) << " step=" << committed_steps
                              << " s0=" << hex32(spin_target) << " flag-bytes=";
                    for (takedown::guest::GuestAddress offset = 0U; offset != 8U; ++offset) {
                        const takedown::guest::GuestAddress address = spin_target + offset;
                        std::cout << (memory.contains(address, 1U) ? std::to_string(static_cast<int>(memory.read_u8(address)))
                                                                   : std::string{"?"})
                                  << (offset + 1U != 8U ? "," : "");
                    }
                    std::cout << '\n';
                }
                freeze_last_spin_target = spin_target;
            }
        }
        if (result.status == takedown::guest::EeStepStatus::Executed) {
            observe_profiled_ee_span(committed_steps, result.instruction.pc, 1U,
                                     takedown::runtime::EePcSampleKind::Scalar);
            ++committed_steps;
            ++executed_instructions;
            ++emulated_ee_cycles;
            continue;
        }
        stop = result;
        if (result.status != takedown::guest::EeStepStatus::GuestSystemCall) {
            break;
        }
        if (retail_timer_owner_abi_trace.active()) {
            retail_timer_owner_abi_trace.observe_hle_boundary();
        }
        std::size_t sif_transfer_count_before = 0U;
        if (retail_trace) {
            sif_transfer_count_before = kernel.sif_dma_transfers().size();
        }
        takedown::runtime::slus_21050::SubmitUiDrawBucketsExecutionContext kernel_call_context{};
        std::int32_t kernel_call_target_thread = 0;
        if (ui_draw_session_feed.active() || vif_packet_ring_flush_claim_tracker.active()) {
            kernel_call_context = current_ui_execution_context();
            kernel_call_target_thread = static_cast<int>(state.read_gpr_u32(4U));
        }
        auto kernel_syscall_phase = begin_profiled_host_phase(runtime::EeHostPhase::KernelSyscall);
        std::optional<ScopedRetailTimerConsumerMemoryWriteCapture> hle_write_capture;
        if (retail_timer_consumer_trace.collecting()) {
            hle_write_capture.emplace(retail_timer_consumer_trace, memory,
                                      ScopedRetailTimerConsumerMemoryWriteCapture::Kind::Hle,
                                      current_retail_timer_consumer_context());
        }
        kernel_stop = kernel.handle_system_call(state, memory);
        hle_write_capture.reset();
        kernel_syscall_phase.finish();
        if (ui_draw_session_feed.active() || vif_packet_ring_flush_claim_tracker.active()) {
            if (kernel_stop.status == takedown::guest::EeKernelCallStatus::Handled) {
                if (kernel_stop.number == 60U) {
                    if (ui_draw_session_feed.active()) {
                        ui_draw_session_feed.retire_all_execution_contexts();
                    }
                    if (vif_packet_ring_flush_claim_tracker.active()) {
                        vif_packet_ring_flush_claim_tracker.retire_all();
                    }
                } else if (kernel_stop.number == 35U || kernel_stop.number == 36U) {
                    if (ui_draw_session_feed.active()) {
                        ui_draw_session_feed.retire_execution_context(kernel_call_context);
                        ui_draw_session_feed.retire_thread(kernel_call_context.thread_id);
                    }
                    if (vif_packet_ring_flush_claim_tracker.active()) {
                        vif_packet_ring_flush_claim_tracker.retire_context(kernel_call_context);
                        vif_packet_ring_flush_claim_tracker.retire_thread(kernel_call_context.thread_id);
                    }
                } else if ((kernel_stop.number == 33U || kernel_stop.number == 37U) && state.read_gpr_u32(2U) == 0U) {
                    if (ui_draw_session_feed.active()) {
                        ui_draw_session_feed.retire_thread(kernel_call_target_thread);
                    }
                    if (vif_packet_ring_flush_claim_tracker.active()) {
                        vif_packet_ring_flush_claim_tracker.retire_thread(kernel_call_target_thread);
                    }
                }
            }
        }
        if (retail_trace) {
            const auto transfers_after_call = kernel.sif_dma_transfers();
            for (std::size_t index = sif_transfer_count_before; index < transfers_after_call.size(); ++index) {
                const auto& transfer = transfers_after_call[index];
                if (transfer.destination == 0x000f0000U && transfer.size > 35U &&
                    memory.contains(transfer.source, 36U) && memory.read_u32_le(transfer.source + 8U) == 0x80000009U) {
                    rpc_bind_trace.push_back(RpcBindTrace{committed_steps, transfer.source,
                                                          memory.read_u32_le(transfer.source + 28U),
                                                          memory.read_u32_le(transfer.source + 32U)});
                }
                if (transfer.destination == 0x000f0000U && transfer.size == 48U && memory.contains(transfer.source, 48U)) {
                    RwaDmaSubmissionTrace trace{.step = committed_steps, .id = transfer.id, .source = transfer.source};
                    for (std::size_t word = 0U; word != 12U; ++word) {
                        trace.words[word] =
                            memory.read_u32_le(transfer.source + static_cast<takedown::guest::GuestAddress>(word) * 4U);
                    }
                    for (std::size_t descriptor_index = sif_transfer_count_before;
                    descriptor_index < transfers_after_call.size(); ++descriptor_index) {
                        const auto& descriptor = transfers_after_call[descriptor_index];
                        if (descriptor.id == transfer.id) {
                            trace.descriptors.push_back(descriptor);
                        }
                        if (descriptor.id == transfer.id && descriptor.source == trace.words[6] && descriptor.size == 112U &&
                            memory.contains(descriptor.source, 112U)) {
                            std::array<std::uint32_t, 28> object_words{};
                            for (std::size_t word = 0U; word != 28U; ++word) {
                                object_words[word] = memory.read_u32_le(
                                    descriptor.source + static_cast<takedown::guest::GuestAddress>(word) * 4U);
                            }
                            trace.extra_words = object_words;
                        }
                    }
                    rwa_dma_submission_trace.push_back(std::move(trace));
                }
            }
        }
        if (kernel_stop.status != takedown::guest::EeKernelCallStatus::Handled) {
            stopped_at_kernel_call = true;
            if (retail_timer_consumer_trace.collecting()) {
                takedown::runtime::RetailTimerConsumerHleStatus hle_status =
                    takedown::runtime::RetailTimerConsumerHleStatus::Unsupported;
                switch (kernel_stop.status) {
                    case takedown::guest::EeKernelCallStatus::Handled:
                    hle_status = takedown::runtime::RetailTimerConsumerHleStatus::Handled;
                    break;
                    case takedown::guest::EeKernelCallStatus::Blocked:
                    hle_status = takedown::runtime::RetailTimerConsumerHleStatus::Blocked;
                    break;
                    case takedown::guest::EeKernelCallStatus::Unsupported:
                    hle_status = takedown::runtime::RetailTimerConsumerHleStatus::Unsupported;
                    break;
                    case takedown::guest::EeKernelCallStatus::InvalidArguments:
                    hle_status = takedown::runtime::RetailTimerConsumerHleStatus::InvalidArguments;
                    break;
                }
                retail_timer_consumer_trace.observe_hle_boundary(state, memory, hle_status, kernel_stop.number,
                                                                 current_retail_timer_consumer_context(),
                                                                 committed_steps, emulated_ee_cycles);
            }
            break;
        }
        observe_profiled_ee_span(committed_steps, result.instruction.pc, 1U, takedown::runtime::EePcSampleKind::Hle);
        ++committed_steps;
        ++handled_kernel_calls;
        ++emulated_ee_cycles;
        if (retail_timer_consumer_trace.collecting()) {
            retail_timer_consumer_trace.observe_hle_boundary(state, memory,
                                                             takedown::runtime::RetailTimerConsumerHleStatus::Handled,
                                                             kernel_stop.number,
                                                             current_retail_timer_consumer_context(),
                                                             committed_steps, emulated_ee_cycles);
        }
    }
    instance_batch_capture.finish();
    std::cout << "Scalar observer bypass: " << (scalar_cold_observers_enabled ? "on" : "off")
              << " steps=" << scalar_observer_bypass_steps << " observers-configured="
              << (scalar_observers_configured ? "yes" : "no")
              << '\n';
    if (retail_timer_consumer_trace.collecting() && guest_work_measurement_window.has_value() &&
        guest_work_measurement_window->started() &&
        *guest_work_measurement_window->stop_step() == committed_steps) {
        if (!retail_timer_consumer_trace.complete_guest_work_collection(committed_steps, emulated_ee_cycles)) {
            throw std::logic_error("retail timer consumer trace could not close at guest-work C");
        }
    }
    if (retail_timer_owner_abi_trace.collecting() && guest_work_measurement_window.has_value() &&
        guest_work_measurement_window->started() &&
        *guest_work_measurement_window->stop_step() == committed_steps) {
        retail_timer_owner_abi_trace.complete_guest_work_collection(committed_steps, emulated_ee_cycles);
    }
    retail_timer_consumer_observer_session.reset();
    const auto retail_timer_consumer_trace_summary = retail_timer_consumer_trace.finalize();
    const auto retail_timer_owner_abi_trace_summary = retail_timer_owner_abi_trace.finalize();
    const auto native_simulation_shadow_summary = native_simulation_shadow != nullptr
                                                      ? native_simulation_shadow->finalize()
                                                      : runtime::NativeSimulationShadowSummary{};
    std::optional<runtime::GuestWorkMeasurementCompletionResult> guest_work_completion_result;
    if (guest_work_measurement_window.has_value() && guest_work_measurement_window->started()) {
        guest_work_completion_result = guest_work_measurement_window->complete_at_instruction_boundary(
            snapshot_guest_work_measurement());
        if (*guest_work_completion_result == runtime::GuestWorkMeasurementCompletionResult::Completed &&
            vu1_lower_plan_window_telemetry.has_value()) {
            const auto stats = memory.vu1_program_cache().statistics();
            auto& telemetry = *vu1_lower_plan_window_telemetry;
            telemetry.native_end = {
                memory.vu1_activity().micro_pairs_committed, stats.native_scheduled_pairs,
                stats.native_transfer_blocks, stats.native_control_pairs, stats.native_control_fast_pairs};
            telemetry.end_hits = stats.opcode_threaded_lower_stage_hits;
            telemetry.end_fallbacks = stats.opcode_threaded_lower_stage_fallbacks;
            telemetry.branch_end_hits = stats.branch_lower_plan_hits;
            telemetry.branch_end_fallbacks = stats.branch_lower_plan_fallbacks;
            telemetry.flag_read_end_hits = stats.flag_read_lower_plan_hits;
            telemetry.flag_read_end_fallbacks = stats.flag_read_lower_plan_fallbacks;
            telemetry.fmand_end_hits = stats.fmand_lower_plan_hits;
            telemetry.fmand_end_fallbacks = stats.fmand_lower_plan_fallbacks;
            telemetry.fcget_end_hits = stats.fcget_lower_plan_hits;
            telemetry.fcget_end_fallbacks = stats.fcget_lower_plan_fallbacks;
            telemetry.clip_predicate_end_hits = stats.clip_predicate_lower_plan_hits;
            telemetry.clip_predicate_end_fallbacks = stats.clip_predicate_lower_plan_fallbacks;
            telemetry.fcand_end_hits = stats.fcand_lower_plan_hits;
            telemetry.fcand_end_fallbacks = stats.fcand_lower_plan_fallbacks;
            telemetry.fcor_end_hits = stats.fcor_lower_plan_hits;
            telemetry.fcor_end_fallbacks = stats.fcor_lower_plan_fallbacks;
            telemetry.end_captured = true;
        }
    }
    if (profile_guest_work_window) {
        stop_guest_execution_profiles();
    }
    bool guest_work_measurement_completed_normally = false;
    if (guest_work_completion_result.has_value()) {
        if (*guest_work_completion_result == runtime::GuestWorkMeasurementCompletionResult::Completed) {
            if (femain_terminal_diagnostic_guest_work_only && femain_terminal_diagnostic != nullptr) {
                std::uint64_t worker_fence_serial = 0U;
                if (gs_renderer_worker.has_value()) {
                    worker_fence_serial = gs_renderer_worker->synchronize({}).completed_serial;
                }
                femain_terminal_diagnostic->complete_guest_work_scope(worker_fence_serial);
                if (femain_provenance_window != nullptr) {
                    static_cast<void>(femain_provenance_window->complete(
                        snapshot_femain_provenance_boundary(*guest_work_measurement_window->end_snapshot()),
                        femain_terminal_diagnostic->seal_immutable_snapshot()));
                }
            }
            if (vu1_lower_plan_window_telemetry.has_value()) {
                auto& telemetry = *vu1_lower_plan_window_telemetry;
                if (telemetry.start_captured && telemetry.end_captured &&
                    telemetry.end_hits >= telemetry.start_hits &&
                    telemetry.end_fallbacks >= telemetry.start_fallbacks) {
                    const auto delta_hits = telemetry.end_hits - telemetry.start_hits;
                    const auto delta_fallbacks = telemetry.end_fallbacks - telemetry.start_fallbacks;
                    if (delta_hits <= std::numeric_limits<std::uint64_t>::max() - delta_fallbacks) {
                        telemetry.delta_hits = delta_hits;
                        telemetry.delta_fallbacks = delta_fallbacks;
                        telemetry.delta_total = delta_hits + delta_fallbacks;
                        telemetry.complete = true;
                    }
                }
                if (telemetry.start_captured && telemetry.end_captured &&
                    telemetry.branch_end_hits >= telemetry.branch_start_hits &&
                    telemetry.branch_end_fallbacks >= telemetry.branch_start_fallbacks) {
                    const auto delta_hits = telemetry.branch_end_hits - telemetry.branch_start_hits;
                    const auto delta_fallbacks = telemetry.branch_end_fallbacks - telemetry.branch_start_fallbacks;
                    if (delta_hits <= std::numeric_limits<std::uint64_t>::max() - delta_fallbacks) {
                        telemetry.branch_delta_hits = delta_hits;
                        telemetry.branch_delta_fallbacks = delta_fallbacks;
                        telemetry.branch_delta_total = delta_hits + delta_fallbacks;
                        telemetry.branch_complete = true;
                    }
                }
                const auto finish_delta = [](const std::uint64_t start_hits, const std::uint64_t start_fallbacks,
                                             const std::uint64_t end_hits, const std::uint64_t end_fallbacks,
                                             std::uint64_t& delta_hits, std::uint64_t& delta_fallbacks,
                                             std::uint64_t& delta_total) {
                    if (end_hits < start_hits || end_fallbacks < start_fallbacks) {
                        return false;
                    }
                    delta_hits = end_hits - start_hits;
                    delta_fallbacks = end_fallbacks - start_fallbacks;
                    if (delta_hits > std::numeric_limits<std::uint64_t>::max() - delta_fallbacks) {
                        return false;
                    }
                    delta_total = delta_hits + delta_fallbacks;
                    return true;
                };
                telemetry.flag_read_complete = finish_delta(
                    telemetry.flag_read_start_hits, telemetry.flag_read_start_fallbacks, telemetry.flag_read_end_hits,
                    telemetry.flag_read_end_fallbacks, telemetry.flag_read_delta_hits,
                    telemetry.flag_read_delta_fallbacks, telemetry.flag_read_delta_total);
                telemetry.fmand_complete = finish_delta(
                    telemetry.fmand_start_hits, telemetry.fmand_start_fallbacks, telemetry.fmand_end_hits,
                    telemetry.fmand_end_fallbacks, telemetry.fmand_delta_hits, telemetry.fmand_delta_fallbacks,
                    telemetry.fmand_delta_total);
                telemetry.fcget_complete = finish_delta(
                    telemetry.fcget_start_hits, telemetry.fcget_start_fallbacks, telemetry.fcget_end_hits,
                    telemetry.fcget_end_fallbacks, telemetry.fcget_delta_hits, telemetry.fcget_delta_fallbacks,
                    telemetry.fcget_delta_total);
                telemetry.clip_predicate_complete = finish_delta(
                    telemetry.clip_predicate_start_hits, telemetry.clip_predicate_start_fallbacks,
                    telemetry.clip_predicate_end_hits, telemetry.clip_predicate_end_fallbacks,
                    telemetry.clip_predicate_delta_hits, telemetry.clip_predicate_delta_fallbacks,
                    telemetry.clip_predicate_delta_total);
                telemetry.fcand_complete = finish_delta(
                    telemetry.fcand_start_hits, telemetry.fcand_start_fallbacks, telemetry.fcand_end_hits,
                    telemetry.fcand_end_fallbacks, telemetry.fcand_delta_hits, telemetry.fcand_delta_fallbacks,
                    telemetry.fcand_delta_total);
                telemetry.fcor_complete = finish_delta(
                    telemetry.fcor_start_hits, telemetry.fcor_start_fallbacks, telemetry.fcor_end_hits,
                    telemetry.fcor_end_fallbacks, telemetry.fcor_delta_hits, telemetry.fcor_delta_fallbacks,
                    telemetry.fcor_delta_total);
            }
            const auto& start_snap = *guest_work_measurement_window->start_snapshot();
            const auto& end_snap = *guest_work_measurement_window->end_snapshot();
            const auto actual = *guest_work_measurement_window->actual_counts();
            runtime::GuestInputTimelineBoundary completion_boundary;
            completion_boundary.committed_steps = committed_steps;
            completion_boundary.ee_cycles = emulated_ee_cycles;
            if (guest_input_timeline_enabled) {
                if (!virtual_ds2_trace.has_value()) {
                    throw std::logic_error("guest input timeline completed without a retail DS2 trace");
                }
                if (guest_input_recorder != nullptr) {
                    guest_input_recorder->finish(virtual_ds2_trace->update_count, completion_boundary);
                    std::cout << "Guest input timeline: recording completed polls=" << virtual_ds2_trace->update_count
                              << " steps=" << completion_boundary.committed_steps
                              << " cycles=" << completion_boundary.ee_cycles << '\n';
                }
                if (guest_input_replay != nullptr) {
                    guest_input_replay->validate_finish(virtual_ds2_trace->update_count, completion_boundary);
                    std::cout << (guest_input_replay->poll_diagnostic()
                                      ? "Guest input timeline: poll diagnostic recorded polls="
                                      : "Guest input timeline: replay validated polls=")
                              << virtual_ds2_trace->update_count << " steps=" << completion_boundary.committed_steps
                              << " cycles=" << completion_boundary.ee_cycles << '\n';
                }
            }
            std::cout << "Guest-work measurement: completed scope=EE instruction-boundary fixed guest work "
                         "normal-window-completion=yes requested-steps="
                      << guest_work_measurement_window->guest_step_budget() << " actual={steps="
                      << actual.committed_steps << " ee-cycles=" << actual.ee_cycles
                      << " vblank-starts=" << actual.vblank_starts << " audio-frames=" << actual.audio_frames
                      << " monotonic-ns=" << actual.monotonic_nanoseconds << "}\n"
                      << "  Guest-work start:    steps=" << start_snap.committed_steps
                      << " ee-cycles=" << start_snap.ee_cycles << " vblank-starts=" << start_snap.vblank_starts
                      << " audio-frames=" << start_snap.audio_frames
                      << " audio-fnv1a64-cumulative=" << hex64(start_snap.audio_fnv1a64)
                      << " monotonic-ns=" << start_snap.monotonic_nanoseconds << '\n'
                      << "  Guest-work end:      steps=" << end_snap.committed_steps
                      << " ee-cycles=" << end_snap.ee_cycles << " vblank-starts=" << end_snap.vblank_starts
                      << " audio-frames=" << end_snap.audio_frames
                      << " audio-fnv1a64-cumulative=" << hex64(end_snap.audio_fnv1a64)
                      << " monotonic-ns=" << end_snap.monotonic_nanoseconds << '\n'
                      << "  Guest-work hash scope: audio-fnv1a64 endpoints are cumulative from Spu2Renderer "
                         "construction; no interval or delta FNV is claimed\n";
            if (guest_work_host_start.has_value() && guest_work_host_end.has_value()) {
                print_guest_work_host_statistics(*guest_work_host_start, *guest_work_host_end,
                                                 native_model_live != nullptr);
            }
            guest_work_measurement_completed_normally = true;
        } else {
            std::cout << "Guest-work measurement: incomplete scope=EE instruction-boundary fixed guest work "
                         "completion="
                      << runtime::to_string(*guest_work_completion_result)
                      << " arm-edges-observed=" << guest_work_measurement_start_edges_observed << '\n';
        }
    } else if (guest_work_measurement_window.has_value()) {
        const char* state_name = "not-started";
        if (guest_work_measurement_window->awaiting_arm()) {
            state_name = "awaiting-B3IN-bit-20";
        } else if (guest_work_measurement_window->armed()) {
            state_name = "armed-awaiting-instruction-boundary";
        } else if (guest_work_measurement_window->rejected()) {
            state_name = "rejected";
        }
        std::cout << "Guest-work measurement: incomplete scope=EE instruction-boundary fixed guest work state="
                  << state_name << " arm-edges-observed=" << guest_work_measurement_start_edges_observed << '\n';
    }
    vif_packet_ring_flush_claim_tracker.retire_all();
    stop_guest_execution_profiles();
    if (!stopped_at_native_gap && host_window != nullptr && !scanout_ppm_path.has_value()) {
        stopped_by_host_window = stopped_by_host_window || !present_host_window(true);
    }
    if (!stopped_at_native_gap && scanout_ppm_path.has_value()) {
        const auto scanout_plan = presentation::plan_gs_scanout(memory.gs_privileged_display().state());
        host_window_scanout_plan = scanout_plan;
        host_window_scanout_status = scanout_plan.status;
        if (scanout_plan.status != presentation::GsScanoutPlanStatus::Ready) {
            throw std::runtime_error("final GS scanout is not ready: " +
                                     std::string(presentation::to_string(scanout_plan.status)));
        }
        auto snapshot = gs_renderer_worker->synchronize(scanout_plan);
        gs_renderer_statistics = snapshot.statistics;
        gs_renderer_queue_statistics = snapshot.queue_statistics;
        if (!snapshot.scanout_frame.has_value()) {
            throw std::runtime_error("GS renderer worker did not return the requested scanout");
        }
        presentation::GsScanoutFrame rendered = std::move(*snapshot.scanout_frame);
        host_window_render_status = rendered.status;
        if (rendered.status != presentation::GsScanoutRenderStatus::Rendered) {
            throw std::runtime_error("final GS scanout could not be rendered: " +
                                     std::string(presentation::to_string(rendered.status)));
        }
        if ((stats_overlay_startup || (host_window != nullptr && host_window->stats_overlay_enabled())) &&
            gs_renderer_worker.has_value()) {
            presentation::StatsOverlayValues overlay_values{};
            const auto overlay_now = std::chrono::steady_clock::now();
            const unsigned long long overlay_now_nanoseconds = static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(overlay_now.time_since_epoch()).count());
            overlay_values.visual_fps = host_window_visual_fps.fps(overlay_now_nanoseconds);
            overlay_values.visual_average_fps = host_window_visual_fps.average_fps(overlay_now_nanoseconds);
            overlay_values.guest_fps = host_window_guest_fps.fps(overlay_now_nanoseconds);
            overlay_values.guest_average_fps = host_window_guest_fps.average_fps(overlay_now_nanoseconds);
            overlay_values.present_fps = host_window_present_fps.fps(overlay_now_nanoseconds);
            overlay_values.present_average_fps = host_window_present_fps.average_fps(overlay_now_nanoseconds);
            overlay_values.solid_sprites = gs_renderer_statistics.solid_sprites_drawn;
            overlay_values.presentation_sprites = gs_renderer_statistics.presentation_sprites_drawn;
            overlay_values.textured_sprites = gs_renderer_statistics.textured_sprites_drawn;
            overlay_values.solid_triangles = gs_renderer_statistics.solid_triangles_drawn;
            overlay_values.textured_triangles = gs_renderer_statistics.textured_triangles_drawn;
            overlay_values.written_pixels = gs_renderer_statistics.written_pixels;
            overlay_values.covered_pixels = gs_renderer_statistics.covered_pixels;
            overlay_values.image_transfers_applied = gs_renderer_statistics.image_transfers_applied;
            overlay_values.drawable_kicks = gs_renderer_statistics.drawable_kicks;
            overlay_values.textured_primitives_deferred = gs_renderer_statistics.textured_primitives_deferred;
            presentation::composite_stats_overlay(rendered.rgba, rendered.width, rendered.height, overlay_values);
        }
        write_rgba_ppm(*scanout_ppm_path, rendered.width, rendered.height, rendered.rgba);
        host_window_surface_width = rendered.width;
        host_window_surface_height = rendered.height;
        host_window_rgba = std::move(rendered.rgba);
    }
    if (gs_renderer_worker.has_value()) {
        const auto snapshot = gs_renderer_worker->synchronize({});
        gs_renderer_statistics = snapshot.statistics;
        gs_renderer_queue_statistics = snapshot.queue_statistics;
        memory.gs_capture().set_command_observer(nullptr);
    } else if (femain_source_primary_command_sink_installed) {
        memory.gs_capture().set_command_observer(nullptr);
    }
    if (host_femain_live_source_producer != nullptr) {
        host_movie_player.close();
        const auto live_cancellation = host_femain_live_transaction.cancel_after_player_close(runtime::Slus21050FemainFrameLedgerCancelReason::Abort);
        if (live_cancellation.cancellation_witness.has_value()) {
            host_femain_live_terminal_authority->cancel(*live_cancellation.cancellation_witness);
            host_femain_live_terminal_authority->reset_after_cancel();
        }
        detach_femain_live_source_bridge();
    }
    if (femain_dynamic_callback_probe != nullptr) {
        femain_dynamic_callback_probe->finish();
    }
    if (femain_frame_closure_diagnostic != nullptr) {
        femain_frame_closure_diagnostic->finish();
    }
    if (femain_terminal_diagnostic != nullptr) {
        if (femain_retail_consume_observation_adapter != nullptr) {
            femain_retail_consume_observation_adapter->close();
        }
        if (femain_retail_consume_invocation_barrier != nullptr) {
            if (femain_retail_consume_invocation_barrier->phase() ==
                    runtime::Slus21050FemainRetailConsumeInvocationBarrierPhase::Armed ||
                femain_retail_consume_invocation_barrier->phase() ==
                    runtime::Slus21050FemainRetailConsumeInvocationBarrierPhase::Active) {
                femain_retail_consume_invocation_barrier->reject(
                    runtime::Slus21050FemainRetailConsumeInvocationBarrierFailure::IncompleteInvocation);
            }
        }
        if (!gs_renderer_worker.has_value()) {
            memory.gs_capture().set_command_observer(nullptr);
        }
        memory.gs_capture().set_diagnostic_command_observer(gs_semantic_trace_writer.get());
        memory.set_vif1_path2_source_marker_observer(base_marker_observer);
        memory.set_vif1_dmac_chain_observer(nullptr);
        memory.gs_capture().set_diagnostic_raw_gif_observer(gs_semantic_trace_writer.get());
        femain_terminal_diagnostic->set_retail_consume_scalar_write_observer(nullptr);
        femain_terminal_diagnostic->detach_scalar_write_observer();
        femain_terminal_diagnostic->set_marker_forward_observer(nullptr);
        femain_terminal_diagnostic->set_chain_forward_observer(nullptr);
        femain_terminal_diagnostic->set_raw_forward_observer(nullptr);
        femain_terminal_diagnostic->set_primary_forward_observer(nullptr);
        if (!femain_terminal_diagnostic->write_report(*femain_terminal_diagnostic_path)) {
            throw std::runtime_error("FEMain terminal producer diagnostic could not write its report");
        }
        persist_femain_retail_consume_observation();
        if (femain_dynamic_callback_report_path.has_value() &&
            !write_femain_dynamic_callback_report(*femain_dynamic_callback_report_path,
                                                  *femain_dynamic_callback_probe)) {
            throw std::runtime_error("FEMain dynamic callback candidate report could not write its report");
        }
        if (femain_frame_closure_report_path.has_value() &&
            !write_femain_frame_closure_report(*femain_frame_closure_report_path,
                                               *femain_frame_closure_diagnostic)) {
            throw std::runtime_error("FEMain frame closure candidate report could not write its report");
        }
        if (femain_provenance_window != nullptr) {
            if (femain_provenance_window->phase() == runtime::Slus21050FemainProvenanceWindow::Phase::Completed) {
                if (!femain_provenance_window->append_report(*femain_terminal_diagnostic_path)) {
                    throw std::runtime_error("FEMain provenance window could not append its report");
                }
            } else {
                std::cerr << "FEMain provenance window incomplete: "
                          << runtime::to_string(femain_provenance_window->failure()) << '\n';
            }
        }
    }
    if (parallel_gs_stream_writer != nullptr) {
        memory.gs_capture().set_raw_gif_observer(nullptr);
        parallel_gs_stream_writer->close();
    }
    if (parallel_gs_completion_reader != nullptr &&
        parallel_gs_completion_reader->state() != platform::ParallelGsCompletionReaderState::Terminal &&
        parallel_gs_completion_reader->failure() == platform::ParallelGsCompletionReaderFailure::None) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (parallel_gs_completion_reader->state() != platform::ParallelGsCompletionReaderState::Terminal &&
               parallel_gs_completion_reader->failure() == platform::ParallelGsCompletionReaderFailure::None &&
               std::chrono::steady_clock::now() < deadline) {
            drain_parallel_gs_completions({});
            if (parallel_gs_completion_reader->state() == platform::ParallelGsCompletionReaderState::Terminal ||
                parallel_gs_completion_reader->failure() != platform::ParallelGsCompletionReaderFailure::None) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (parallel_gs_completion_reader->state() != platform::ParallelGsCompletionReaderState::Terminal &&
            parallel_gs_completion_reader->failure() == platform::ParallelGsCompletionReaderFailure::None) {
            parallel_gs_completion_shutdown_timed_out = true;
            std::cerr << "paraLLEl-GS completion shutdown drain timed out; state="
                      << platform::to_string(parallel_gs_completion_reader->state())
                      << " pending=" << parallel_gs_completion_reader->statistics().pending_complete_records
                      << " next-raw-vsync=" << next_parallel_gs_completion_raw_vsync << '\n';
        }
    }
    if (gs_semantic_trace_writer != nullptr) {
        memory.set_vu0_micro_call_observer(nullptr);
        if (spark_trace == nullptr) {
            memory.set_vu1_xgkick_observer(nullptr);
        }
        memory.set_vif1_dmac_chain_observer(nullptr);
        memory.gs_capture().set_diagnostic_raw_gif_observer(nullptr);
        memory.gs_capture().set_diagnostic_command_observer(nullptr);
        gs_semantic_trace_writer->close();
    }
    if (collision_trace != nullptr) {
        collision_trace->finalize();
    }
    if (spark_trace != nullptr) {
        memory.set_vu1_xgkick_observer(nullptr);
        spark_trace->finalize();
    }
    const auto compiled_block_fusion_statistics = compiled_block_dispatcher.fusion_statistics();
    if (elide_inert_dmac_poll) {
        std::cout << "  Inert DMAC poll:     admissions=" << inert_dmac_poll_admissions
                  << " retained-channels=" << hex32(inert_dmac_poll_admitted_channels) << '\n';
    }
    if (compiled_block_fusion_profile_directory != nullptr) {
        write_compiled_block_fusion_profile(*compiled_block_fusion_profile_directory,
                                            compiled_block_fusion_statistics);
        const auto path = *compiled_block_fusion_profile_directory / "native-dma-fallbacks.tsv";
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            throw std::runtime_error("refusing to overwrite native DMA profile: " + path.string());
        }
        std::ofstream output(path);
        output << "entry\tname\tfallbacks\tcompletion_queue\tgif_transfer\tvif1_transfer\tsif_transfer\tlatched_"
                  "callback\n";
        for (const auto& metadata : native_function_dispatcher.registered_functions()) {
            const auto found = native_dma_fallback_sources.find(metadata.address);
            if (found == native_dma_fallback_sources.end()) {
                continue;
            }
            for (std::size_t sources = 0U; sources < 32U; ++sources) {
                if (found->second[sources] == 0U) {
                    continue;
                }
                output << hex32(metadata.address) << '\t' << metadata.name << '\t' << found->second[sources];
                for (const auto source : {runtime::NativeDmaCompletionQueue,
                                          runtime::NativeDmaGifTransfer,
                                          runtime::NativeDmaVif1Transfer,
                                          runtime::NativeDmaSifTransfer,
                                          runtime::NativeDmaLatchedCallback}) {
                    output << '\t' << ((static_cast<std::uint64_t>(source) & sources) != 0U);
                }
                output << '\n';
            }
        }
        output.close();
        if (!output) {
            throw std::runtime_error("unable to write native DMA profile: " + path.string());
        }
    }
    if (compiled_budget_profiler != nullptr) {
        for (const bool summary : {true, false}) {
            const auto path = *compiled_block_fusion_profile_directory /
                              (summary ? "compiled-budget-summary.tsv" : "compiled-budget.tsv");
            std::error_code error;
            if (std::filesystem::exists(path, error) || error) {
                throw std::runtime_error("refusing to overwrite compiled budget profile: " + path.string());
            }
            std::ofstream output(path);
            if (summary) {
                compiled_budget_profiler->write_summary(output);
            } else {
                compiled_budget_profiler->write_samples(output);
            }
            output.close();
            if (!output) {
                throw std::runtime_error("unable to write compiled budget profile: " + path.string());
            }
        }
    }
    if (ee_host_phase_profiler != nullptr) {
        runtime::write_ee_host_phase_profile_artifacts(*ee_host_phase_profile_directory, *ee_host_phase_profiler);
    }
    if (ee_graphics_service_diagnostics != nullptr) {
        runtime::write_ee_graphics_service_profile_artifacts(*ee_graphics_service_profile_directory,
                                                             *ee_graphics_service_diagnostics);
        runtime::write_join_pump_operation_profile_artifact(*ee_graphics_service_profile_directory,
                                                            *ee_graphics_service_diagnostics, join_pump_region_calls,
                                                            join_pump_region_retired_instructions,
                                                            join_pump_region_zero_retired_rejections,
                                                            join_pump_region_exits);
    }
    if (ee_pc_sampler != nullptr) {
        const auto profile_statistics = ee_pc_sampler->statistics();
        const std::uint64_t expected_profiled_instructions =
            profile_timing_started ? committed_steps - ee_pc_profile_instruction_origin : 0U;
        if (profile_statistics.retired_instructions != expected_profiled_instructions ||
            profile_statistics.ordinal_discontinuities != 0U) {
            throw std::logic_error("EE PC sampler did not observe every committed guest instruction exactly once");
        }
        runtime::write_ee_pc_profile_artifacts(*ee_pc_profile_directory, *ee_pc_sampler);
    }
    std::optional<std::filesystem::path> first_gs_capture;
    std::size_t exported_gs_images = 0U;
    if (!stopped_at_native_gap && gs_capture_directory.has_value()) {
        for (const auto& image : memory.gs_capture().image_transfers()) {
            const auto output_path = gs_upload_capture_path(*gs_capture_directory, image, exported_gs_images);
            std::error_code error;
            if (std::filesystem::exists(output_path, error) || error) {
                throw std::runtime_error("refusing to overwrite GS capture: " + output_path.string());
            }
            guest::write_gs_image_ppm(image, output_path);
            if (!first_gs_capture.has_value()) {
                first_gs_capture = output_path;
            }
            ++exported_gs_images;
        }
    }
    const auto& startup = kernel.startup_state();
    std::cout << "Takedown EE boot runner\n"
              << "  entry point:         " << hex32(plan.executable.entry_point) << '\n'
              << "  EE instructions:     " << executed_instructions << '\n'
              << "  kernel calls handled:" << ' ' << handled_kernel_calls << '\n'
              << "  alarm callbacks:     " << dispatched_alarm_callbacks << '\n'
              << "  DMAC callbacks:      " << dispatched_dmac_callbacks << '\n';
    std::cout << "  INTC events:         " << dispatched_intc_events << '\n';
    std::cout << "  SIF command callbacks: " << dispatched_sif_command_callbacks << '\n';
    std::cout << "  EE thread switches:  " << dispatched_thread_switches << '\n';
    std::cout << "  GIF status fast read: " << (fast_gif_status_read ? "enabled" : "disabled") << '\n';
    std::cout << "  GIF blocked arbiter cache: " << (gif_blocked_arbiter_cache ? "enabled" : "disabled")
              << " skips=" << memory.gif_blocked_arbiter_cache_skips() << '\n';
    std::cout << "  GIF PATH3 run-ahead batch: " << (gif_path3_batch ? "enabled" : "disabled")
              << " spans=" << memory.gif_path3_batched_spans()
              << " quadwords=" << memory.gif_path3_batched_quadwords() << '\n';
    std::cout << "  GIF idle pump gate:  " << (gif_idle_pump_gate ? "enabled" : "disabled")
              << " hits=" << memory.gif_idle_pump_gate_hits() << '\n';
    std::cout << "  EE threads completed:" << ' ' << completed_guest_threads << '\n';
    std::cout << "  VBlank starts:       " << raised_vblank_starts << '\n';
    std::cout << "  emulated EE cycles:  " << emulated_ee_cycles << '\n';
    if (parallel_gs_completion_reader != nullptr) {
        const auto completion = parallel_gs_completion_reader->statistics();
        std::cout << "  paraLLEl-GS completion: state=" << platform::to_string(parallel_gs_completion_reader->state())
                  << " healthy="
                  << (parallel_gs_completion_reader->failure() == platform::ParallelGsCompletionReaderFailure::None ? 1
                                                                                                                    : 0)
                  << " failure=" << platform::to_string(parallel_gs_completion_reader->failure())
                  << " hello=" << completion.hello_records << " complete=" << completion.complete_records << '/'
                  << completion.complete_records_consumed << " pending=" << completion.pending_complete_records
                  << " error=" << completion.error_records << " eos=" << completion.eos_records
                  << " clean=" << (completion.clean_eos ? 1 : 0) << " session=" << completion.session_epoch
                  << " last-sequence=" << completion.last_sequence
                  << " last-raw-vsync=" << completion.last_raw_vsync_index
                  << " eos-raw-vsync=" << completion.eos_raw_vsync_index
                  << " next-raw-vsync=" << next_parallel_gs_completion_raw_vsync
                  << " shutdown-timeout=" << (parallel_gs_completion_shutdown_timed_out ? 1 : 0)
                  << " protocol-failures=" << completion.protocol_failures << " bytes=" << completion.bytes_read
                  << '\n';
    }
    if (femain_parallel_gs_completion_ledger != nullptr) {
        const auto* const evidence = last_femain_parallel_gs_completion_evidence.has_value()
                                         ? &*last_femain_parallel_gs_completion_evidence
                                         : nullptr;
        std::cout << "  FEMain GPU correlation: healthy="
                  << (femain_parallel_gs_completion_ledger->failure() ==
                              runtime::Slus21050FemainParallelGsCompletionFailure::None
                          ? 1
                          : 0)
                  << " failure=" << runtime::to_string(femain_parallel_gs_completion_ledger->failure())
                  << " pending=" << femain_parallel_gs_completion_ledger->pending_count()
                  << " matched=" << femain_parallel_gs_completion_ledger->matched_count()
                  << " cancelled=" << femain_parallel_gs_completion_ledger->cancelled_count()
                  << " last-observed-raw-vsync=" << femain_parallel_gs_completion_ledger->last_observed_raw_vsync_index()
                  << " claim=" << (evidence != nullptr ? "gpu-submission-complete-through-raw-vblank" : "none")
                  << " terminal-authority=none presentation=unproven retirement=none";
        if (evidence != nullptr) {
            std::cout << " publication=" << evidence->publication.publication_serial
                      << " source-auth=" << evidence->source.authentication_tag
                      << " completion-session=" << evidence->completion_session_epoch
                      << " completion-sequence=" << evidence->completion_sequence
                      << " completion-raw-vsync=" << evidence->raw_vsync_index;
            std::cout << " completion-audio-sample=" << evidence->graphics_completed_at.audio_sample;
        }
        std::cout << '\n';
    }
    for (const auto& watch : ee_pc_hit_watch) {
        std::cout << "  EE PC hit watch:     exact-address=" << hex32(watch.address) << " total=" << watch.total
                  << " guest-work-window=" << watch.guest_work_window << '\n';
    }
    if (retail_timer_differential_enabled) {
        const bool window_passed =
            !guest_work_measurement_step_budget.has_value() ||
            (retail_timer_differential_window_calls != 0U &&
             retail_timer_differential_window_calls == retail_timer_differential_window_matches &&
             retail_timer_differential_window_mismatches == 0U);
        retail_timer_differential_passed = retail_timer_differential_calls != 0U &&
                                           retail_timer_differential_calls == retail_timer_differential_matches &&
                                           retail_timer_differential_mismatches == 0U &&
                                           !retail_timer_projection.has_value() && window_passed;
        std::cout << "  Retail timer diff:   calls=" << retail_timer_differential_calls
                  << " matches=" << retail_timer_differential_matches
                  << " mismatches=" << retail_timer_differential_mismatches
                  << " window=" << retail_timer_differential_window_matches << '/'
                  << retail_timer_differential_window_calls
                  << " window-mismatches=" << retail_timer_differential_window_mismatches
                  << " pending=" << (retail_timer_projection.has_value() ? 1 : 0)
                  << " gate=" << (retail_timer_differential_passed ? "PASS" : "FAIL") << '\n';
        if (retail_timer_first_mismatch.has_value()) {
            std::cout << "    first-mismatch: " << *retail_timer_first_mismatch << '\n';
        }
    }
    if (native_simulation_shadow_summary.enabled) {
        std::cout << "  Native simulation shadow: cycles=" << native_simulation_shadow_summary.cycle_samples
                  << " simulation_hz=" << native_simulation_shadow_hz
                  << " batches=" << native_simulation_shadow_summary.guest_cycle_batches
                  << " entries=" << native_simulation_shadow_summary.owner_entries
                  << " returns=" << native_simulation_shadow_summary.owner_returns
                  << " matches=" << native_simulation_shadow_summary.matches
                  << " mismatches=" << native_simulation_shadow_summary.mismatches
                  << " dropped=" << native_simulation_shadow_summary.dropped_owner_samples
                  << " state-epoch=" << native_simulation_shadow_summary.state_epoch
                  << " state-publications=" << native_simulation_shadow_summary.state_publications
                  << " state-capacity=" << native_simulation_shadow_summary.state_capacity
                  << " state-failure=" << runtime::to_string(native_simulation_shadow_summary.state_failure)
                  << " trace=" << native_simulation_shadow_summary.trace_steps << '/'
                  << native_simulation_shadow_summary.trace_capacity
                  << " failure=" << runtime::to_string(native_simulation_shadow_summary.failure)
                  << " gate=" << (native_simulation_shadow_summary.passed ? "PASS" : "FAIL") << '\n';
        if (native_simulation_shadow_summary.mismatches != 0U && native_simulation_shadow != nullptr) {
            const auto samples = native_simulation_shadow->owner_samples();
            std::cout << "    sample-boundaries:";
            for (std::size_t index = 0U; index < samples.size(); ++index) {
                const auto& sample = samples[index];
                std::cout << ' ' << index << "={cycles=" << sample.ee_cycles << ",owner-delta=" << sample.owner_cycle_delta
                          << ",shared=" << sample.shared_tick << ",expected=" << sample.expected.observed_tick
                          << ",shadow=" << sample.shadow.observed_tick << ",matched=" << sample.matched << '}';
            }
            std::cout << '\n';
            const auto first_mismatch =
                std::find_if(samples.begin(), samples.end(), [](const auto& sample) { return !sample.matched; });
            if (first_mismatch != samples.end()) {
                const auto print_timer = [](const std::string_view label, const auto& timer) {
                    std::cout << ' ' << label << "={tick=" << timer.observed_tick << ",offset=" << timer.consumed_tick_offset
                              << ",paused=" << timer.paused_ticks << ",whole=" << timer.whole_ticks
                              << ",remainder=" << timer.subdivision_remainder << ",baseline=" << timer.baseline_tick
                              << ",subdivisions=" << timer.subdivisions
                              << ",step=" << hex32(std::bit_cast<std::uint32_t>(timer.step_seconds))
                              << ",elapsed=" << hex32(std::bit_cast<std::uint32_t>(timer.elapsed_seconds))
                              << ",pending=" << timer.pending_subdivisions << ",accumulating=" << timer.accumulating << '}';
                };
                std::cout << "    first-mismatch: cycles=" << first_mismatch->ee_cycles
                          << " owner-delta=" << first_mismatch->owner_cycle_delta
                          << " shared-tick=" << first_mismatch->shared_tick;
                print_timer("expected", first_mismatch->expected);
                print_timer("observed", first_mismatch->observed);
                print_timer("shadow", first_mismatch->shadow);
                std::cout << '\n';
            }
        }
    }
    if (retail_timer_consumer_trace_summary.enabled) {
        std::cout << "  Retail timer consumer trace: records=" << retail_timer_consumer_trace_summary.complete_invocations
                  << " nested-jalr=" << retail_timer_consumer_trace_summary.nested_jalr_targets
                  << " attempted-nested-jalr=" << retail_timer_consumer_trace_summary.attempted_nested_jalr_count
                  << " nested-capacity=" << retail_timer_consumer_trace_summary.nested_jalr_capacity
                  << " hle-boundaries=" << retail_timer_consumer_trace_summary.hle_boundaries
                  << " attempted-hle-boundaries=" << retail_timer_consumer_trace_summary.attempted_hle_boundary_count
                  << " hle-capacity=" << retail_timer_consumer_trace_summary.hle_boundary_capacity
                  << " steps=" << retail_timer_consumer_trace_summary.observed_scalar_steps
                  << " cycles=" << retail_timer_consumer_trace_summary.observed_scalar_cycles << " journal="
                  << (retail_timer_consumer_trace_summary.memory_write_journal_available ? "available"
                                                                                           : "unavailable-next-milestone")
                  << " output=" << retail_timer_consumer_trace_summary.output_path.string()
                  << " failure=" << runtime::to_string(retail_timer_consumer_trace_summary.failure) << " gate="
                  << (retail_timer_consumer_trace_summary.gate_applicable
                          ? (retail_timer_consumer_trace_summary.passed ? "PASS" : "FAIL")
                          : "SKIP")
                  << '\n';
    }
    if (retail_timer_owner_abi_trace_summary.enabled) {
        std::cout << "  Retail timer owner ABI trace: records=" << retail_timer_owner_abi_trace_summary.completed_invocations
                  << " retained=" << retail_timer_owner_abi_trace_summary.retained_invocations
                  << " dropped=" << retail_timer_owner_abi_trace_summary.dropped_invocations
                  << " instructions=" << retail_timer_owner_abi_trace_summary.observed_instructions
                  << " cycles=" << retail_timer_owner_abi_trace_summary.observed_cycles
                  << " scheduler-boundaries=" << retail_timer_owner_abi_trace_summary.scheduler_boundaries
                  << " path-digest=0x" << std::hex << retail_timer_owner_abi_trace_summary.completed_path_digest << std::dec
                  << " output=" << retail_timer_owner_abi_trace_summary.output_path.string()
                  << " failure=" << runtime::to_string(retail_timer_owner_abi_trace_summary.failure)
                  << " gate=" << (retail_timer_owner_abi_trace_summary.passed ? "PASS" : "FAIL") << '\n';
    }
    if (subframe_input_bridge.has_value()) {
        std::cout << "  sub-frame input:     observed " << subframe_input_bridge->observed_events() << ", applied "
                  << subframe_input_bridge->applied_events() << ", dropped " << subframe_input_bridge->dropped_events()
                  << '\n';
    }
    if (ee_host_phase_profiler != nullptr) {
        const auto profile_statistics = ee_host_phase_profiler->statistics();
        std::cout << "  EE host phase profile: scope=" << (profile_guest_work_window ? "B3IR-A-to-C" : "guest-loop")
                  << " interval=" << profile_statistics.sample_interval << " host=" << profile_statistics.host_nanoseconds
                  << "ns" << " estimated-profiled=" << profile_statistics.estimated_profiled_nanoseconds
                  << "ns" << " output=" << ee_host_phase_profile_directory->string() << '\n';
        const auto ranked_phases = ee_host_phase_profiler->ranked_phase_samples();
        const auto shown = std::min<std::size_t>(ranked_phases.size(), 8U);
        for (std::size_t index = 0U; index < shown; ++index) {
            const auto& sample = ranked_phases[index];
            std::cout << "    hot-host-phase[#" << index << "] phase=" << runtime::to_string(sample.phase)
                      << " calls=" << sample.calls << " timed=" << sample.timed_calls
                      << " estimated=" << sample.estimated_nanoseconds << "ns\n";
        }
    }
    if (ee_pc_sampler != nullptr) {
        const auto profile_statistics = ee_pc_sampler->statistics();
        std::cout << "  EE PC sample profile: scope=" << (profile_guest_work_window ? "B3IR-A-to-C" : "guest-loop")
                  << " interval=" << profile_statistics.interval << " retired=" << profile_statistics.retired_instructions
                  << " samples=" << profile_statistics.samples << " host-windows=" << profile_statistics.host_timing_windows
                  << " host=" << profile_statistics.host_nanoseconds
                  << "ns" << " output=" << ee_pc_profile_directory->string() << '\n';
        const auto ranked_host_pcs = ee_pc_sampler->ranked_host_pc_samples();
        const auto shown = std::min<std::size_t>(ranked_host_pcs.size(), 8U);
        for (std::size_t index = 0U; index < shown; ++index) {
            const auto& sample = ranked_host_pcs[index];
            std::cout << "    hot-ee-host[#" << index << "] kind=" << runtime::to_string(sample.kind)
                      << " pc=" << hex32(sample.pc) << " windows=" << sample.host_windows
                      << " host=" << sample.host_nanoseconds << "ns";
            if (sample.kind == runtime::EePcSampleKind::NativeFunction) {
                if (const auto* const metadata = native_function_dispatcher.lookup(sample.pc); metadata != nullptr) {
                    std::cout << " name=" << metadata->name;
                }
            } else if (sample.kind == runtime::EePcSampleKind::CompiledBlock) {
                if (const auto* const metadata = compiled_block_dispatcher.lookup(sample.pc); metadata != nullptr) {
                    std::cout << " name=" << metadata->name;
                }
            }
            std::cout << '\n';
        }
    }
    if (collision_trace != nullptr) {
        const auto trace_statistics = collision_trace->statistics();
        std::cout << "  collision trace:    " << collision_trace->output_path().string()
                  << " samples=" << trace_statistics.total_samples << " retained=" << trace_statistics.retained_samples
                  << " overwritten=" << trace_statistics.overwritten_samples << '\n';
    }
    if (vehicle_world_contact_probe != nullptr) {
        const auto probe_diagnostics = vehicle_world_contact_probe->diagnostics();
        std::cout << "  vehicle contact probe: enabled observations=" << probe_diagnostics.return_observations
                  << " valid=" << probe_diagnostics.valid_completions
                  << " instruction-rejects=" << probe_diagnostics.instruction_mismatches
                  << " pointer-rejects=" << probe_diagnostics.pointer_rejections
                  << " cross-link-rejects=" << probe_diagnostics.cross_link_rejections
                  << " player-rejects=" << probe_diagnostics.player_identity_rejections
                  << " incomplete-reads=" << probe_diagnostics.incomplete_read_rejections
                  << " incomplete-completions=" << probe_diagnostics.incomplete_completions
                  << " older=" << probe_diagnostics.older_completions
                  << " latest=" << (vehicle_world_contact_probe->latest_snapshot().has_value() ? 1 : 0) << '\n';
    }
    if (spark_trace != nullptr) {
        const auto trace_statistics = spark_trace->statistics();
        std::cout << "  spark trace:        " << spark_trace->output_path().string()
                  << " samples=" << trace_statistics.total_samples << " retained=" << trace_statistics.retained_samples
                  << " overwritten=" << trace_statistics.overwritten_samples
                  << " filtered=" << trace_statistics.runtime_filtered_targets
                  << " packets=" << trace_statistics.render_packets
                  << " packet-bytes=" << trace_statistics.render_packet_bytes
                  << " truncated=" << trace_statistics.truncated_render_packets << '\n';
    }
    if (native_cars.has_value() && native_cars->statistics().launches != 0U) {
        std::cout << "  Native cars:         launches=" << native_cars->statistics().launches
                  << " batches=" << native_cars->statistics().batches
                  << " vertices=" << native_cars->statistics().vertices
                  << " damaged=" << native_cars->statistics().damaged
                  << " reflective=" << native_cars->statistics().reflective
                  << " clipped=" << native_cars->statistics().clipped
                  << " declined=" << native_cars->statistics().declined;
        if (native_cars->statistics().verified != 0U) {
            std::cout << " verified=" << native_cars->statistics().verified
                      << " mismatched=" << native_cars->statistics().mismatched
                      << " mismatched-words=" << native_cars->statistics().mismatched_words;
        }
        std::cout << '\n';
    }
    if (added_cars.registered() && (!added_cars.cars().empty() || added_cars.statistics().retail_handling != 0U)) {
        const auto added = added_cars.statistics();
        std::cout << "  Added cars:          cars=" << added_cars.cars().size()
                  << " registry-patches=" << added.registry_patches << " handling-aliases=" << added.hash_aliases
                  << " names-shown=" << added.names_shown << " retail-handling=" << added.retail_handling << '\n';
    }
    if (menu_mouse.statistics().moves + menu_mouse.statistics().confirms + menu_mouse.statistics().backs +
            menu_mouse.statistics().starts !=
        0U) {
        std::cout << "  Menu mouse:          moves=" << menu_mouse.statistics().moves
                  << " confirms=" << menu_mouse.statistics().confirms << " backs=" << menu_mouse.statistics().backs
                  << " starts=" << menu_mouse.statistics().starts << '\n';
    }
    if (camera_fov.statistics().calls != 0U) {
        std::cout << "  Camera FOV:          calls=" << camera_fov.statistics().calls
                  << " game-min=" << static_cast<double>(camera_fov.statistics().minimum_fov)
                  << " game-max=" << static_cast<double>(camera_fov.statistics().maximum_fov)
                  << " game-last=" << static_cast<double>(camera_fov.statistics().last_fov)
                  << " target=" << static_cast<double>(camera_fov.target()) << " histogram=";
        for (std::size_t degree = 0U; degree < 180U; ++degree) {
            if (camera_fov.statistics().histogram[degree] != 0U) {
                std::cout << degree << ':' << camera_fov.statistics().histogram[degree] << ' ';
            }
        }
        std::cout << '\n';
    }
    std::cout << "  Native C++ functions: revision="
              << (interpreter_only ? "disabled" : (native_function_revision_matched ? "SLUS-21050" : "unmatched"))
              << " registered=" << native_function_dispatcher.size() << " calls=" << native_function_calls
              << " retired=" << native_function_retired_instructions << '\n';
    std::cout << "  Native render-state packet: active=" << (native_render_state_packet_active ? 1 : 0)
              << " calls=" << native_function_hits[runtime::slus_21050::kBuildRenderStatePacketAddress] << '\n';
    std::cout << "  Native static object: active=" << (native_static_object_active ? 1 : 0)
              << " calls=" << native_function_hits[runtime::slus_21050::kSubmitStaticObjectAddress] << '\n';
    std::cout << "  Join-pump region:    requested=" << (join_pump_region_requested ? "yes" : "no")
              << " status=" << join_pump_region_status << " calls=" << join_pump_region_calls
              << " inert-dmac=" << join_pump_region_inert_dmac_admissions
              << " retired=" << join_pump_region_retired_instructions
              << " zero-retired=" << join_pump_region_zero_retired_rejections
              << " exits={boundary:" << join_pump_region_exits[0] << ",jalr:" << join_pump_region_exits[1]
              << ",budget:" << join_pump_region_exits[2] << ",event:" << join_pump_region_exits[3]
              << ",code-mismatch:" << join_pump_region_exits[4] << ",fault:" << join_pump_region_exits[5] << "}\n";
    std::cout << "  Join-pump executor: specialized=" << (join_pump_specialized ? "on" : "off") << '\n';
    std::cout << "  Join-pump loop batch: " << (join_pump_loop_batch ? "on" : "off")
              << " calls=" << join_pump_loop_batch_calls << '\n';
    std::cout << "  Join-pump idle batch resume: " << (join_pump_resume_idle_batch ? "on" : "off")
              << " loops=" << join_pump_resumed_idle_loops << '\n';
    std::cout << "  Join-pump idle summary: " << (join_pump_idle_summary ? "on" : "off")
              << " instructions=" << join_pump_idle_summary_instructions << '\n';
    std::cout << "  Native join-drain wait: " << (native_join_drain_wait ? "on" : "off")
              << " calls=" << native_join_wait_calls << " instructions=" << native_join_wait_instructions << '\n';
    std::cout << "  Join-pump idle code cache: " << (join_pump_idle_code_cache_enabled ? "on" : "off")
              << " hits=" << join_pump_idle_code_cache.hits << " misses=" << join_pump_idle_code_cache.misses << '\n';
    std::cout << "  VU0 SIMD arithmetic: " << (guest::vu0_detail::simd_arithmetic_enabled() ? "on" : "off") << '\n';
    std::cout << "  VU1 SIMD arithmetic: " << (guest::vu1_detail::simd_arithmetic_enabled() ? "on" : "off") << '\n';
    std::cout << "  VU1 AVX512 arithmetic: " << (guest::vu1_detail::avx512_arithmetic_enabled() ? "on" : "off") << '\n';
    std::cout << "  VU1 native float arithmetic: " << (guest::vu1_detail::native_float_arithmetic_enabled() ? "on" : "off")
              << '\n';
    if (guest_work_host_start.has_value() && guest_work_host_end.has_value()) {
        std::cout << "  Guest-work summarized instructions: "
                  << guest_work_host_end->summarized_instructions - guest_work_host_start->summarized_instructions << '\n';
    }
    for (const auto& [mask, count] : join_pump_idle_unknown_states) {
        std::cout << "  Idle-summary unknown state: mask=" << mask << " count=" << count << '\n';
    }
    std::cout << "    graphics-deadline=" << (join_pump_graphics_deadline_requested ? "on" : "off")
              << " queries=" << join_pump_graphics_deadline_queries
              << " admitted-spans=" << join_pump_graphics_deadline_admitted_spans
              << " retired-cycles=" << join_pump_graphics_deadline_retired_cycles
              << " retired-instructions=" << join_pump_graphics_deadline_retired_instructions
              << " endpoint-services=" << join_pump_graphics_deadline_endpoint_services
              << " fallbacks={deadline:" << join_pump_graphics_deadline_fallbacks[0]
              << ",budget:" << join_pump_graphics_deadline_fallbacks[1]
              << ",profile:" << join_pump_graphics_deadline_fallbacks[2]
              << ",diagnostics:" << join_pump_graphics_deadline_fallbacks[3]
              << ",ee-pc:" << join_pump_graphics_deadline_fallbacks[4]
              << ",subframe-input:" << join_pump_graphics_deadline_fallbacks[5]
              << "} cache-telemetry=" << (join_pump_graphics_deadline_cache_telemetry_enabled ? "on" : "off")
              << " hits=" << memory.graphics_service_deadline_cache_hits()
              << " recomputes=" << memory.graphics_service_deadline_cache_recomputes() << '\n';
    if (join_pump_graphics_deadline_cache_telemetry_enabled) {
        using DeadlineKind = guest::EeGraphicsServiceDeadlineKind;
        const auto print_kind = [&memory](const std::string_view name, const DeadlineKind kind) {
            std::cout << ' ' << name << " hits=" << memory.graphics_service_deadline_cache_hits(kind)
                      << " entry-hits=" << memory.graphics_service_deadline_cache_entry_hits(kind)
                      << " recomputes=" << memory.graphics_service_deadline_cache_recomputes(kind);
        };
        std::cout << "    graphics-deadline-cache-kinds:";
        print_kind("Immediate", guest::EeGraphicsServiceDeadlineKind::Immediate);
        print_kind("InternalDeadline", guest::EeGraphicsServiceDeadlineKind::InternalDeadline);
        print_kind("ExternalWake", guest::EeGraphicsServiceDeadlineKind::ExternalWake);
        print_kind("Unknown", guest::EeGraphicsServiceDeadlineKind::Unknown);
        std::cout << '\n';
    }
    const auto ui_draw_feed_diagnostics = ui_draw_session_feed.diagnostics();
    std::cout << "  UI draw session feed: attempts=" << ui_draw_feed_diagnostics.attempted_invocations
              << " completed=" << ui_draw_feed_diagnostics.completed_invocations
              << " sealed=" << ui_draw_feed_diagnostics.sealed_invocations
              << " overlaps=" << ui_draw_feed_diagnostics.overlapping_invocations
              << " events=" << ui_draw_feed_diagnostics.observed_events
              << " profile-refreshes=" << ui_draw_feed_diagnostics.code_profile_refreshes
              << " compiled-clips=" << ui_draw_feed_diagnostics.compiled_boundary_clips
              << " internal-failures=" << ui_draw_feed_diagnostics.internal_failures
              << " active=" << (ui_draw_feed_diagnostics.active ? "yes" : "no")
              << " sealed-active=" << (ui_draw_feed_diagnostics.awaiting_sealed_return ? "yes" : "no")
              << " first-failure=" << runtime::slus_21050::to_string(ui_draw_feed_diagnostics.first_failure) << '\n';
    const auto vif_packet_ring_flush_claim_diagnostics = vif_packet_ring_flush_claim_tracker.diagnostics();
    const auto ui_text_vif_transport_diagnostics = ui_text_vif_transport.diagnostics();
    std::cout << "  VIF ring claim feed:  gated=" << vif_packet_ring_flush_claim_diagnostics.gated_entries_without_claims
              << " planned=" << vif_packet_ring_flush_claim_diagnostics.plan_attempts
              << " captured=" << vif_packet_ring_flush_claim_diagnostics.captured_flushes
              << " reported=" << vif_packet_ring_flush_claim_diagnostics.reported_flushes
              << " completed=" << vif_packet_ring_flush_claim_diagnostics.completed_flushes
              << " relocated=" << vif_packet_ring_flush_claim_diagnostics.relocated_claims
              << " compiled-clips=" << vif_packet_ring_flush_claim_diagnostics.compiled_boundary_clips
              << " fail-closed=" << vif_packet_ring_flush_claim_diagnostics.fail_closed_events
              << " retired-claims=" << vif_packet_ring_flush_claim_diagnostics.fail_closed_claims
              << " transport-retire-calls=" << ui_text_vif_transport_diagnostics.fail_closed_retire_calls
              << " active=" << vif_packet_ring_flush_claim_diagnostics.active_contexts
              << " first-failure=" << runtime::slus_21050::to_string(vif_packet_ring_flush_claim_diagnostics.first_failure)
              << '\n';
    const auto ui_draw_packet_session_diagnostics = ui_draw_packet_session_adapter.diagnostics();
    const std::string_view last_ui_text_payload_profile =
        ui_draw_packet_session_diagnostics.last_text_callback.has_value()
            ? runtime::slus_21050::to_string(ui_draw_packet_session_diagnostics.last_text_callback->payload_profile)
            : std::string_view{"none"};
    std::cout << "  UI packet sessions:   started=" << ui_draw_packet_session_diagnostics.started_invocations
              << " completed=" << ui_draw_packet_session_diagnostics.completed_invocations
              << " abandoned=" << ui_draw_packet_session_diagnostics.abandoned_invocations
              << " callbacks=" << ui_draw_packet_session_diagnostics.callback_calls
              << " text-entered=" << ui_draw_packet_session_diagnostics.text_callback_entries
              << " text-returned=" << ui_draw_packet_session_diagnostics.text_callback_returns
              << " compatibility=" << ui_draw_packet_session_diagnostics.compatibility_callback_entries
              << " payload-profile-available=" << ui_draw_packet_session_diagnostics.payload_profile_available_callbacks
              << " payload-profile-unavailable="
              << ui_draw_packet_session_diagnostics.payload_profile_unavailable_callbacks
              << " transport-claims=" << ui_draw_packet_session_diagnostics.shared_transport_claims
              << " transport-published=" << ui_draw_packet_session_diagnostics.shared_transport_published_claims
              << " transport-ledger-tickets=" << ui_draw_packet_session_diagnostics.shared_transport_ledger_tickets
              << " transport-ledger-reservations="
              << ui_draw_packet_session_diagnostics.shared_transport_ledger_reservations
              << " session-releases=" << ui_draw_packet_session_diagnostics.terminal_session_releases
              << " last-payload-profile=" << last_ui_text_payload_profile
              << " active=" << (ui_draw_packet_session_diagnostics.active ? "yes" : "no")
              << " first-failure=" << runtime::slus_21050::to_string(ui_draw_packet_session_diagnostics.first_failure)
              << '\n';
    std::size_t native_function_unreached = 0U;
    for (const auto& function : native_function_dispatcher.registered_functions()) {
        const auto hit = native_function_hits.find(function.address);
        if (hit == native_function_hits.end() || hit->second == 0U) {
            ++native_function_unreached;
            continue;
        }
        std::cout << "    " << hex32(function.address) << ' ' << function.name << " hits=" << hit->second << '\n';
    }
    if (native_function_unreached != 0U) {
        std::cout << "    registered but unreached=" << native_function_unreached << '\n';
    }
    std::cout << "  Local leaf C++ module: linked=" << "no status=not-configured" << " registered=" << std::size_t{0U}
    << '\n';
    const std::size_t compiled_block_reached = compiled_block_dispatcher.reached_entry_count();
    const std::size_t compiled_block_unreached = compiled_block_dispatcher.size() - compiled_block_reached;
    std::cout << "  Compiled C++ blocks: registered=" << compiled_block_dispatcher.size()
              << " dispatches=" << compiled_block_dispatches << " retired=" << compiled_block_retired_instructions
              << " cycles=" << compiled_block_ee_cycles << " fused-dispatches=" << compiled_block_fused_dispatches
              << " fused-instructions=" << compiled_block_fused_instructions
              << " code-mismatches=" << compiled_block_code_mismatches << " reached=" << compiled_block_reached
              << " unreached=" << compiled_block_unreached << '\n';
    if (compiled_block_fusion_profile_directory != nullptr) {
        std::cout << "    fusion-limiters: classified=" << classified_fusion_dispatches(compiled_block_fusion_statistics)
                  << " fused-dispatches=" << compiled_block_fusion_statistics.fused_dispatches
                  << " fused-successors=" << compiled_block_fusion_statistics.fused_successor_instructions
                  << " missing-successor=" << compiled_block_fusion_statistics.missing_successor_stops
                  << " nonfusible-successor=" << compiled_block_fusion_statistics.nonfusible_successor_stops
                  << " one-instruction-budget=" << compiled_block_fusion_statistics.one_instruction_budget_stops
                  << " output=" << (*compiled_block_fusion_profile_directory / "compiled-fusion.tsv").string() << '\n';
    }
    std::cout << "  Local block C++ module: linked=" << "no status=not-configured" << " registered=" << std::size_t{0U}
    << '\n';
    std::uint64_t native_function_preflight_fallbacks = 0U;
    for (std::size_t index = 1U; index < 9U; ++index) {
        native_function_preflight_fallbacks += native_function_preflight_counts[index];
    }
    std::cout << "  Native safe fallbacks:" << ' ' << native_function_preflight_fallbacks;
    for (std::size_t index = 1U; index < 9U; ++index) {
        if (native_function_preflight_counts[index] == 0U) {
            continue;
        }
        const auto status = static_cast<runtime::NativeFunctionPreflightStatus>(index);
        std::cout << ' ' << runtime::native_function_preflight_status_name(status) << '=' << native_function_preflight_counts[index];
    }
    std::cout << '\n';
    if (stopped_at_native_gap) {
        std::cout << "  stop reason:         native-only C++ coverage gap\n";
        std::cout << "  stop PC:             " << hex32(native_gap_address) << '\n';
        if (!native_gap_block_name.empty()) {
            std::cout << "  compiled block:      " << native_gap_block_name << '\n'
                      << "  native gap:          live guest code no longer matches generated C++";
            if (native_gap_expected_word.has_value()) {
                std::cout << " (expected " << hex32(*native_gap_expected_word);
                if (native_gap_observed_word.has_value()) {
                    std::cout << ", observed " << hex32(*native_gap_observed_word);
                } else {
                    std::cout << ", observed unavailable";
                }
                std::cout << ')';
            }
            std::cout << '\n';
        } else if (native_gap_function_name.empty()) {
            std::cout << "  native gap:          no compiled C++ function or block owns this PC\n";
        } else if (native_gap_preflight.has_value()) {
            std::cout << "  native function:     " << native_gap_function_name << '\n'
                      << "  native gap:          scheduler-safe preflight blocked atomic entry ("
                      << runtime::native_function_preflight_status_name(*native_gap_preflight) << ")\n";
        } else {
            std::cout << "  native function:     " << native_gap_function_name << '\n'
                      << "  native gap:          registered C++ dispatch was not accepted\n";
        }
        std::cout << "  next action:         "
                  << (native_gap_block_name.empty() ? "generate a resumable C++ block for this PC\n"
                                                    : "analyze and compile the live patched-code variant\n");
        return 5;
    }
    std::cout << "  VBlank idle skips:   " << fast_forwarded_vblank_waits << '\n';
    std::cout << "  alarm idle skips:    " << fast_forwarded_alarm_waits << '\n';
    std::cout << "  Graphics drain skips:" << ' ' << fast_forwarded_graphics_drain_waits << '\n';
    platform::write_host_wait_report(std::cout);
    if (variable_frame_rate.has_value()) {
        const auto hooks = variable_frame_rate->statistics();
        std::cout << "  Variable frame rate: frames=" << variable_frame_clock->statistics().frames
                  << " busy-vblanks=" << variable_frame_clock->statistics().busy_vblanks
                  << " frame-cycles=" << variable_frame_clock->statistics().frame_cycles_total
                  << " shortest-frame-cycles=" << variable_frame_clock->statistics().shortest_frame_cycles
                  << " longest-frame-cycles=" << variable_frame_clock->statistics().longest_frame_cycles
                  << " dropped-cycles=" << variable_frame_clock->statistics().dropped_cycles
                  << " reanchors=" << variable_frame_clock->statistics().reanchors
                  << " host-wait-ns=" << variable_frame_host_wait_nanoseconds << '\n';
        const auto& planner = variable_frame_rate->planner();
        std::cout << "  Variable frame rate ticks: planned-frames=" << planner.statistics().frames
                  << " ticks=" << planner.statistics().ticks
                  << " split-frames=" << planner.statistics().split_frames
                  << " unplanned-ticks=" << planner.statistics().unplanned_ticks
                  << " game-clock-microseconds=" << static_cast<std::uint64_t>(planner.game_clock_seconds() * 1.0e6)
                  << " dropped-microseconds=" << static_cast<std::uint64_t>(planner.statistics().dropped_seconds * 1.0e6)
                  << " shortest-tick-microseconds="
                  << static_cast<std::uint64_t>(planner.statistics().shortest_tick_seconds * 1.0e6)
                  << " longest-tick-microseconds="
                  << static_cast<std::uint64_t>(planner.statistics().longest_tick_seconds * 1.0e6)
                  << " retail-rate=" << planner.retail_rate() << '\n';
        std::cout << "  Variable frame rate hooks: set-tick-rate=" << hooks.set_tick_rate_calls
                  << " increment-tick=" << hooks.increment_tick_calls << " timer-updates=" << hooks.timer_updates
                  << " timer-updates-left-to-guest=" << hooks.timer_updates_left_to_guest
                  << " timer-resets=" << hooks.timer_resets << " frame-skip=" << hooks.frame_skip_computes
                  << " swap-interval=" << hooks.swap_interval_overrides
                  << " per-frame-step=" << hooks.per_frame_step_scalings
                  << " follow-camera=" << hooks.follow_camera_corrections << " tracked-timers=" << hooks.tracked_timers
                  << " witness-violations=" << hooks.witness_violations << '\n';
    }
    {
        std::cout << "  Scalar burst: enabled=" << (scalar_burst_enabled ? "yes" : "no") << " bursts=" << scalar_burst.bursts
                  << " instructions=" << scalar_burst.instructions << " native-calls=" << scalar_burst.native_calls
                  << " native-declines=" << scalar_burst.native_declines << " stops={hook:" << scalar_burst.stop_count(runtime::ScalarBurstStop::Hook)
                  << ",horizon:" << scalar_burst.stop_count(runtime::ScalarBurstStop::Horizon) << ",hardware:" << scalar_burst.stop_count(runtime::ScalarBurstStop::Hardware) << ",result:" << scalar_burst.stop_count(runtime::ScalarBurstStop::Result)
                  << ",state:" << scalar_burst.stop_count(runtime::ScalarBurstStop::State) << "}\n";
    }
    std::cout << "  Scalar burst start horizon (bit_width buckets, 0.." << scalar_burst.start_horizon.size() - 1U
              << "): {";
    for (std::size_t bucket = 0U; bucket < scalar_burst.start_horizon.size(); ++bucket) {
        std::cout << (bucket == 0U ? "" : ",") << scalar_burst.start_horizon[bucket];
    }
    std::cout << "}\n";
    if (const char* const dump = std::getenv("TAKEDOWN_EXIT_RAM_DUMP")) {
        const auto ram = memory.bytes();
        std::ofstream out(dump, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ram.data()), static_cast<std::streamsize>(ram.size()));
        std::cout << "  RAM dump: " << ram.size() << " bytes to " << dump << (out ? "" : " (write failed)") << '\n';
    }
    if (const char* const peek = std::getenv("TAKEDOWN_EXIT_PEEK")) {
        std::string_view list(peek);
        while (!list.empty()) {
            const auto comma = list.find(',');
            const std::string item(list.substr(0U, comma));
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1U);
            const auto colon = item.find(':');
            const unsigned long address = std::stoul(item.substr(0U, colon), nullptr, 0);
            const unsigned long words = colon == std::string::npos ? 1UL : std::stoul(item.substr(colon + 1U), nullptr, 10);
            std::cout << "  Peek 0x" << std::hex << address << ":";
            for (unsigned long word = 0U; words > word; ++word) {
                const std::uint32_t at = static_cast<std::uint32_t>(word * 4U + address);
                std::cout << ' ' << (memory.contains(at, 4U) ? memory.read_u32_le(at) : 0xdeadbeefU);
            }
            std::cout << std::dec << '\n';
        }
        for (const auto& semaphore : kernel.semaphores()) {
            std::cout << "  Peek semaphore id=" << semaphore.id << " count=" << semaphore.count
                      << " max=" << semaphore.max_count << " waiters=" << semaphore.wait_threads << '\n';
        }
        for (const auto& thread : kernel.thread_runtime_snapshots(state)) {
            std::cout << "  Peek thread id=" << thread.id << " pc=0x" << std::hex << thread.program_counter << " ra=0x"
                      << thread.return_address << std::dec << " wait=" << thread.wait_type << ':' << thread.wait_id
                      << " wakeups=" << thread.wakeup_count << '\n';
        }
    }
    const auto& aot = guest::ee_aot_statistics();
    std::cout << "  EE AOT: module=" << (guest::ee_aot_module_linked() ? "linked" : "none")
              << " enabled=" << (ee_aot_enabled ? "yes" : "no") << " functions=" << guest::ee_aot_compiled_function_count()
              << " calls=" << aot.calls << " runs=" << aot.runs << " instructions=" << aot.instructions
              << " entries=" << aot.function_entries << " declined-entries=" << aot.native_declined_entries
              << " stops={uncompiled:" << aot.stop_uncompiled << ",budget:" << aot.stop_budget
              << ",excluded:" << aot.stop_excluded << ",untranslated:" << aot.stop_untranslated
              << ",memory:" << aot.stop_memory << ",device:" << aot.stop_device << ",fault:" << aot.stop_fault
              << "} register-loads=" << aot.register_loads << " register-stores=" << aot.register_stores
              << " identity-checks=" << aot.identity_checks << " identity-failures=" << aot.identity_failures
              << " native-hook={taken:" << aot.native_hook_taken << ",continued:" << aot.native_hook_continued << "}\n";
    std::cout << "  EE AOT budget refusals: at-entry=" << aot.stop_budget_at_entry << " remaining={";
    for (std::size_t bucket = 0U; bucket < aot.stop_budget_remaining.size(); ++bucket) {
        std::cout << (bucket == 0U ? "" : ",") << aot.stop_budget_remaining[bucket];
    }
    std::cout << "}\n";
    std::cout << "  EE AOT memory stops: vu=" << aot.stop_memory_vu << " gs=" << aot.stop_memory_gs
              << " unaligned=" << aot.stop_memory_unaligned << " other=" << aot.stop_memory_other << " ee-registers={";
    {
        auto remaining = aot.stop_memory_ee_register_kib;
        for (unsigned int shown = 0U; shown < 8U; ++shown) {
            const auto largest = std::max_element(remaining.begin(), remaining.end());
            if (*largest == 0U) {
                break;
            }
            std::cout << (shown != 0U ? "," : "") << "0x" << std::hex
                      << (0x10000000U + static_cast<std::uint32_t>(largest - remaining.begin()) * 0x400U) << std::dec << ':' << *largest;
            *largest = 0U;
        }
        std::cout << "}\n";
    }
    guest::ee_aot_write_misses();
    const auto& vu0 = guest::vu0_micro_statistics();
    std::cout << "  VU0 micro: completed-launches=" << vu0.completed_launches
              << " completed-pairs=" << vu0.completed_pairs << " compiled-launches=" << vu0.compiled_launches
              << " compiled-pairs=" << vu0.compiled_pairs << '\n';
    guest::vu0_micro_write_capture();
    if (scalar_burst_audit) {
        std::cout << "  Scalar burst audit: on checks=" << scalar_burst_audit_checks << '\n';
    }
    if (scalar_burst_requested && !scalar_burst_enabled) {
        std::cout << "  Scalar burst request ignored: an observer, diagnostic or execution mode that needs every "
                     "outer iteration is configured ("
                  << scalar_burst_hooks.marked_ordinary_pc_count() << " hook PCs registered)\n";
    }
    std::cout << "  Graphics run-ahead: enabled=" << (graphics_run_ahead ? "yes" : "no")
              << " calls=" << graphics_run_ahead_calls << " device-cycles=" << graphics_run_ahead_cycles << '\n';
    if (memory.graphics_async_mode() != runtime::GraphicsAsyncMode::Off) {
        const auto stats = memory.graphics_async_statistics();
        std::cout << "  Graphics async:     mode="
                  << (memory.graphics_async_mode() == runtime::GraphicsAsyncMode::On          ? "on"
                      : memory.graphics_async_mode() == runtime::GraphicsAsyncMode::Decoupled ? "decoupled"
                                                                                              : "measure")
                  << " windows=" << stats.windows << " declined=" << stats.declined_total
                  << " ee-cycles-in-windows=" << stats.ee_cycles_in_windows
                  << " job-ms=" << static_cast<double>(stats.job_nanoseconds) / 1.0e6
                  << " open-ms=" << static_cast<double>(stats.open_nanoseconds) / 1.0e6
                  << " overlap-bound-ms=" << static_cast<double>(stats.overlap_bound_nanoseconds) / 1.0e6
                  << " ee-wait-ms=" << static_cast<double>(stats.wait_nanoseconds) / 1.0e6
                  << " ordered=" << stats.ordered_tasks << "/" << stats.ordered_inline
                  << " worker-exceptions=" << stats.worker_exceptions << '\n';
        for (std::size_t index = 0U; index < stats.closed_by.size(); ++index) {
            if (stats.closed_by[index] == 0U) {
                continue;
            }
            std::cout << "    closed-by " << runtime::graphics_async_fence_name(static_cast<runtime::GraphicsAsyncFence>(index))
                      << ": " << stats.closed_by[index] << " windows, mean-open-us="
                      << stats.open_nanoseconds_by[index] / stats.closed_by[index] / 1000U << '\n';
        }
        for (std::size_t index = 0U; index < stats.declined.size(); ++index) {
            if (stats.declined[index] != 0U) {
                std::cout << "    declined "
                          << runtime::graphics_async_decline_name(static_cast<runtime::GraphicsAsyncDecline>(index))
                          << ": " << stats.declined[index] << '\n';
            }
        }
        for (const auto& fence : stats.fence_addresses) {
            if (fence.count != 0U) {
                std::cout << "    fence-address 0x" << std::hex << fence.address << std::dec << ": " << fence.count
                          << '\n';
            }
        }
    }
    if (host_controller_bridge) {
        std::cout << "  controller input:    ";
        if (!virtual_ds2_trace.has_value() || !virtual_ds2_trace->pulse_step.has_value()) {
            std::cout << "controller gate not reached\n";
        } else {
            std::cout << "virtual DS2 pulse at step " << *virtual_ds2_trace->pulse_step << '\n';
        }
        if (virtual_ds2_trace.has_value()) {
            std::cout << "  virtual DS2 trace:   first=" << virtual_ds2_trace->first_step
                      << " updates=" << virtual_ds2_trace->update_count << " registered=";
            if (virtual_ds2_trace->registered_step.has_value()) {
                std::cout << *virtual_ds2_trace->registered_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " armed=";
            if (virtual_ds2_trace->armed_step.has_value()) {
                std::cout << *virtual_ds2_trace->armed_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " pulse=";
            if (virtual_ds2_trace->pulse_step.has_value()) {
                std::cout << *virtual_ds2_trace->pulse_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " attempts=" << virtual_ds2_trace->pulse_attempt_count;
            std::cout << " decoded=";
            if (virtual_ds2_trace->decoded_step.has_value()) {
                std::cout << *virtual_ds2_trace->decoded_step << ':' << hex32(virtual_ds2_trace->decoded_gate_bits);
            } else {
                std::cout << "pending";
            }
            std::cout << " action-edge=";
            if (virtual_ds2_trace->action_edge_step.has_value()) {
                std::cout << *virtual_ds2_trace->action_edge_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " cleared=";
            if (virtual_ds2_trace->action_edge_clear_step.has_value()) {
                std::cout << *virtual_ds2_trace->action_edge_clear_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " gate-accepted=";
            if (virtual_ds2_trace->gate_accepted_step.has_value()) {
                std::cout << *virtual_ds2_trace->gate_accepted_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " profile=";
            for (std::size_t word = 0U; word < virtual_ds2_trace->profile_words.size(); ++word) {
                std::cout << (word == 0U ? "" : "/") << virtual_ds2_trace->profile_words[word];
            }
            std::cout << '\n';
            std::cout << "  virtual DS2 title:   candidate=";
            if (virtual_ds2_trace->title_candidate_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_candidate_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " armed=";
            if (virtual_ds2_trace->title_armed_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_armed_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " pulse=";
            if (virtual_ds2_trace->title_pulse_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_pulse_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " decoded=";
            if (virtual_ds2_trace->title_decoded_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_decoded_step << ':'
                          << hex32(virtual_ds2_trace->title_decoded_action_bits);
            } else {
                std::cout << "pending";
            }
            std::cout << " action=" << 8U << " raw=";
            if (virtual_ds2_trace->title_action_channel != 0xffffffffU) {
                std::cout << virtual_ds2_trace->title_action_channel;
            } else {
                std::cout << "pending";
            }
            std::cout << " source=" << hex32(virtual_ds2_trace->title_action_source) << ':'
                      << static_cast<unsigned>(virtual_ds2_trace->title_action_source_active) << '/'
                      << static_cast<unsigned>(virtual_ds2_trace->title_action_source_mode)
                      << " edge-state=" << static_cast<unsigned>(virtual_ds2_trace->title_action_previous) << '/'
                      << static_cast<unsigned>(virtual_ds2_trace->title_action_current);
            std::cout << " edge=";
            if (virtual_ds2_trace->title_edge_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_edge_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " poll=";
            if (virtual_ds2_trace->title_poll_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_poll_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " result=";
            if (virtual_ds2_trace->title_result_step.has_value()) {
                std::cout << virtual_ds2_trace->title_poll_result << '@' << *virtual_ds2_trace->title_result_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " accepted=";
            if (virtual_ds2_trace->title_accepted_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_accepted_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " queued=";
            if (virtual_ds2_trace->title_queued_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_queued_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " event=";
            if (virtual_ds2_trace->title_event_step.has_value()) {
                std::cout << *virtual_ds2_trace->title_event_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " next-flow=";
            if (virtual_ds2_trace->title_next_flow_step.has_value()) {
                std::cout << hex32(0x1ea2a10U) << '@' << *virtual_ds2_trace->title_next_flow_step;
            } else {
                std::cout << "pending";
            }
            std::cout << '\n';
            std::cout << "  virtual DS2 polls:   (step:selector:edge:blocks:result)=";
            const auto poll_count = std::min<std::uint64_t>(virtual_ds2_trace->edge_poll_count, 16U);
            for (std::size_t index = 0U; index < poll_count; ++index) {
                const auto& poll = virtual_ds2_trace->edge_polls[index];
                std::cout << (index == 0U ? "" : " ") << poll.step << ':' << poll.selector << ':'
                          << static_cast<unsigned long>(poll.accept_edge) << ':' << hex32(poll.action_blocks[0]) << '/'
                          << hex32(poll.action_blocks[1]) << ':';
                if (poll.result == 0xffffffffU) {
                    std::cout << "pending";
                } else {
                    std::cout << poll.result;
                }
            }
            std::cout << '\n';
            std::cout << "  virtual DS2 events:  (step:event:a3)=";
            const auto event_count = std::min<std::uint64_t>(virtual_ds2_trace->frontend_event_count, 16U);
            for (std::size_t index = 0U; index < event_count; ++index) {
                const auto& event = virtual_ds2_trace->frontend_events[index];
                std::cout << (index == 0U ? "" : " ") << event.step << ':' << event.event << ':' << event.argument3;
            }
            std::cout << '\n';
        }
        if (headless_frontend_replay) {
            std::cout << "  frontend replay:     ";
            if (!headless_frontend_replay_trace.has_value()) {
                std::cout << "waiting for post-title flow\n";
            } else {
                const auto& trace = *headless_frontend_replay_trace;
                std::cout << "start=" << trace.start_step << " update=" << trace.start_update_count << " down=";
                if (trace.first_down_step.has_value()) {
                    std::cout << *trace.first_down_step;
                } else {
                    std::cout << "pending";
                }
                std::cout << " up=";
                if (trace.first_up_step.has_value()) {
                    std::cout << *trace.first_up_step;
                } else {
                    std::cout << "pending";
                }
                std::cout << " first-cross=";
                if (trace.first_cross_step.has_value()) {
                    std::cout << *trace.first_cross_step;
                } else {
                    std::cout << "pending";
                }
                std::cout << " crosses=" << trace.cross_pulse_count << " last-cross=";
                if (trace.last_cross_step.has_value()) {
                    std::cout << *trace.last_cross_step;
                } else {
                    std::cout << "pending";
                }
                std::cout << '\n';
                const auto manager_count = std::min<std::uint64_t>(trace.manager_update_count, trace.manager_updates.size());
                std::cout << "  replay manager:      captured=" << manager_count
                          << " (update:input:step:raw:confirm-logical:physical/fallback:physical-active/mode:fallback-active/"
                             "mode:direct-value:direct-previous/current:map0/map1>fallback-channel:fallback-value:fallback-"
                             "previous/current:edge:flow:updater:flags:menu:actions)\n";
                for (std::size_t index = 0U; index < manager_count; ++index) {
                    const auto& update = trace.manager_updates[index];
                    const auto previous_input =
                        update.replay_update != 0U ? kHeadlessFrontendReplaySchedule.input_for_update(update.replay_update - 1U)
                                                   : runtime::FrontendReplayInput::Neutral;
                    const bool changed = index == 0U || update.flow != trace.manager_updates[index - 1U].flow ||
                                         update.updater != trace.manager_updates[index - 1U].updater ||
                                         update.updater_state != trace.manager_updates[index - 1U].updater_state ||
                                         update.updater_flags != trace.manager_updates[index - 1U].updater_flags ||
                                         update.menu_fields != trace.manager_updates[index - 1U].menu_fields ||
                                         update.actions != trace.manager_updates[index - 1U].actions;
                    const bool periodic = update.replay_update % 10U == 0U;
                    if (update.input == runtime::FrontendReplayInput::Neutral &&
                        previous_input == runtime::FrontendReplayInput::Neutral && !changed && !periodic) {
                        continue;
                    }
                    std::cout << "    " << update.replay_update << ':' << frontend_replay_input_symbol(update.input) << ':'
                              << update.step << ':';
                    for (std::size_t byte = 0U; byte < update.decoded.size(); ++byte) {
                        std::cout << (byte == 0U ? "" : "/") << static_cast<unsigned>(update.decoded[byte]);
                    }
                    std::cout << ':' << update.confirm_logical_action << ':' << hex32(update.action_device) << '/'
                              << hex32(update.fallback_device) << ':' << static_cast<unsigned>(update.device_flags[0])
                              << '/' << static_cast<unsigned>(update.device_flags[1]) << ':'
                              << static_cast<unsigned>(update.fallback_device_flags[0]) << '/'
                              << static_cast<unsigned>(update.fallback_device_flags[1]) << ':'
                              << hex32(update.confirm_direct_value) << ':'
                              << static_cast<unsigned>(update.confirm_direct_edge_state[0]) << '/'
                              << static_cast<unsigned>(update.confirm_direct_edge_state[1]) << ':'
                              << update.confirm_mapped_channels[0] << '/' << update.confirm_mapped_channels[1] << '>'
                              << update.confirm_fallback_channel << ':' << hex32(update.confirm_fallback_value) << ':'
                              << static_cast<unsigned>(update.confirm_fallback_edge_state[0]) << '/'
                              << static_cast<unsigned>(update.confirm_fallback_edge_state[1]) << ':'
                              << update.accept_edge << ':' << hex32(update.flow) << ':' << hex32(update.updater) << ':'
                              << update.updater_state << '/' << static_cast<unsigned>(update.updater_flags[0]) << '/'
                              << static_cast<unsigned>(update.updater_flags[1]) << '/'
                              << static_cast<unsigned>(update.updater_flags[2]) << ':';
                    for (std::size_t field = 0U; field < update.menu_fields.size(); ++field) {
                        std::cout << (field == 0U ? "" : "/") << hex32(update.menu_fields[field]);
                    }
                    std::cout << ':';
                    bool printed_action = false;
                    for (std::size_t action = 0U; action < update.actions.size(); ++action) {
                        if (update.actions[action] == 0U) {
                            continue;
                        }
                        std::cout << (printed_action ? "," : "") << action << '=' << update.actions[action];
                        printed_action = true;
                    }
                    if (!printed_action) {
                        std::cout << '-';
                    }
                    std::cout << '\n';
                }
                const auto menu_count = std::min<std::uint64_t>(trace.menu_poll_count, trace.menu_polls.size());
                std::cout << "  replay menu polls:   captured=" << menu_count << " (update:input:step:result:fields)\n";
                for (std::size_t index = 0U; index < menu_count; ++index) {
                    const auto& poll = trace.menu_polls[index];
                    const auto previous_input =
                        poll.replay_update != 0U ? kHeadlessFrontendReplaySchedule.input_for_update(poll.replay_update - 1U)
                                                 : runtime::FrontendReplayInput::Neutral;
                    const bool changed = index == 0U || poll.result != trace.menu_polls[index - 1U].result ||
                                         poll.fields != trace.menu_polls[index - 1U].fields;
                    if (poll.input == runtime::FrontendReplayInput::Neutral &&
                        previous_input == runtime::FrontendReplayInput::Neutral && !changed &&
                        poll.replay_update % 10U != 0U) {
                        continue;
                    }
                    std::cout << "    " << poll.replay_update << ':' << frontend_replay_input_symbol(poll.input) << ':'
                              << poll.step << ':' << poll.result << ':';
                    for (std::size_t field = 0U; field < poll.fields.size(); ++field) {
                        std::cout << (field == 0U ? "" : "/") << hex32(poll.fields[field]);
                    }
                    std::cout << '\n';
                }
                const auto edge_count = std::min<std::uint64_t>(trace.edge_poll_count, trace.edge_polls.size());
                std::cout << "  replay edge polls:   captured=" << edge_count
                          << " (update:input:step:selector:edge:blocks:result)=";
                bool printed_poll = false;
                for (std::size_t index = 0U; index < edge_count; ++index) {
                    const auto& poll = trace.edge_polls[index];
                    if (!(index == 0U || index + 1U == edge_count || poll.input != runtime::FrontendReplayInput::Neutral ||
                          (poll.result != 0U && poll.result != 0xffffffffU))) {
                        continue;
                    }
                    std::cout << (printed_poll ? " " : "") << poll.replay_update << ':'
                              << frontend_replay_input_symbol(poll.input) << ':' << poll.step << ':' << poll.selector
                              << ':' << static_cast<unsigned long>(poll.accept_edge) << ':'
                              << hex32(poll.action_blocks[0]) << '/' << hex32(poll.action_blocks[1]) << ':';
                    if (poll.result == 0xffffffffU) {
                        std::cout << "pending";
                    } else {
                        std::cout << poll.result;
                    }
                    printed_poll = true;
                }
                if (!printed_poll) {
                    std::cout << "none";
                }
                std::cout << '\n';
            }
        }
    }
    if (host_movie_playback || headless_movie_eos) {
        if (headless_movie_eos_traces.empty()) {
            std::cout << "  movie bridge:         retail M2V decoder not reached\n";
        }
        for (std::size_t index = 0U; index < headless_movie_eos_traces.size(); ++index) {
            const auto& trace = headless_movie_eos_traces[index];
            std::cout << "  movie bridge[" << index << "]: mode=" << (trace.host_playback ? "host-playback" : "headless-eos")
                      << " file=\"" << trace.filename << "\" start=" << trace.start_step << " accepted=";
            if (trace.accepted_step.has_value()) {
                std::cout << *trace.accepted_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " accepted-ipu=" << trace.accepted_fdec_count << '/' << trace.accepted_to_ipu_count;
            std::cout << " start-skip=";
            if (trace.start_skip_requested_step.has_value()) {
                std::cout << *trace.start_skip_requested_step;
                if (trace.start_skip_input_poll.has_value()) {
                    std::cout << " input-poll=" << *trace.start_skip_input_poll;
                }
                if (trace.start_skip_audio_frames.has_value() && trace.start_skip_audio_non_silent_frames.has_value() &&
                    trace.start_skip_audio_fnv1a64.has_value()) {
                    std::cout << " skip-audio=" << *trace.start_skip_audio_frames << '/'
                              << *trace.start_skip_audio_non_silent_frames << '/' << hex64(*trace.start_skip_audio_fnv1a64);
                }
                if (trace.start_skip_key_ons.has_value() && trace.start_skip_active_voices.has_value()) {
                    std::cout << " skip-voices=" << *trace.start_skip_key_ons << '/' << *trace.start_skip_active_voices;
                }
            } else {
                if (trace.start_skip_exempt) {
                    std::cout << "exempt";
                } else {
                    std::cout << "unused";
                }
            }
            if (trace.looping_background || trace.host_playback) {
                if (trace.looping_background) {
                    std::cout << " stream=looping-background";
                }
                std::cout << " paused=";
                if (trace.paused_step.has_value()) {
                    std::cout << *trace.paused_step;
                } else {
                    std::cout << "pending";
                }
                std::cout << " assertions=" << trace.pause_assertions;
            }
            std::cout << " abort=";
            if (trace.abort_requested_step.has_value()) {
                std::cout << *trace.abort_requested_step;
            } else {
                std::cout << "pending";
            }
            if (trace.abort_poll_step.has_value()) {
                std::cout << " poll=" << *trace.abort_poll_step << ':' << hex32(trace.abort_decoder) << '/'
                          << hex32(trace.abort_s0) << ':' << trace.abort_movie_state << '/' << trace.abort_work_state
                          << ':' << static_cast<unsigned>(trace.abort_flags[0]) << '/'
                          << static_cast<unsigned>(trace.abort_flags[1]) << '/'
                          << static_cast<unsigned>(trace.abort_flags[2]) << ':' << hex32(trace.abort_stream) << ':'
                          << trace.abort_fdec_count << '/' << trace.abort_to_ipu_count;
            }
            std::cout << " stop-complete=";
            if (trace.stop_completed_step.has_value()) {
                std::cout << *trace.stop_completed_step;
            } else {
                std::cout << "pending";
            }
            std::cout << " destroyed=";
            if (trace.destroyed_step.has_value()) {
                std::cout << *trace.destroyed_step;
            } else {
                std::cout << "pending";
            }
            std::cout << '\n';
        }
        const auto& femain = headless_femain_bridge.statistics();
        std::cout << "  FEMain fallback frame: asset=";
        if (headless_femain_movie_path.has_value()) {
            std::cout << "resolved";
        } else if (headless_femain_path_error) {
            std::cout << "unresolved:" << headless_femain_path_error.message();
        } else {
            std::cout << "not-requested";
        }
        std::cout << " load=" << to_string(headless_femain_bridge.load_status()) << " input-bytes=" << femain.input_bytes
                  << " frames=" << femain.frames_received << " concealed=" << femain.concealed_frames;
        if (headless_femain_bridge.ready()) {
            std::cout << " source=" << femain.source_sequence << ':' << hex32(femain.source_checksum);
        }
        std::cout << " seed=" << to_string(femain.last_seed_status) << " attempts=" << femain.seed_attempts
                  << " calls=" << femain.seed_calls << " buffers=" << femain.seeded_buffers
                  << " bytes=" << femain.seeded_bytes << '\n';
        if (femain_park_pcs.has_value()) {
            const HeadlessMovieEosTrace* background = nullptr;
            for (const auto& trace : headless_movie_eos_traces) {
                if (trace.looping_background) {
                    background = &trace;
                }
            }
            runtime::Slus21050FemainParkObservation park_observation{};
            if (background != nullptr) {
                park_observation.bootstrap_called = background->accepted_step.has_value();
                park_observation.pause_asserted = background->pause_assertions != 0U;
                park_observation.owner_stop_requested = background->abort_requested_step.has_value() ||
                                                        background->stop_completed_step.has_value() ||
                                                        background->destroyed_step.has_value();
                park_observation.unpause_observed =
                    park_observation.pause_asserted && !park_observation.owner_stop_requested &&
                    !background->destroyed_step.has_value() && memory.contains(0x0051A8BEU, 1U) &&
                    memory.read_u8(0x0051A8BEU) == 0U;
            }
            if (memory.contains(0x01E90424U, 4U)) {
                park_observation.frontend_phase_valid = true;
                park_observation.frontend_phase = memory.read_u32_le(0x01E90424U);
            }
            const auto park_verdict = runtime::classify_slus_21050_femain_park(park_observation);
            std::cout << "  FEMain park diag:    verdict=" << runtime::to_string(park_verdict)
                      << " bootstrap=" << (park_observation.bootstrap_called ? "yes" : "no")
                      << " paused=" << (park_observation.pause_asserted ? "yes" : "no")
                      << " unpause=" << (park_observation.unpause_observed ? "yes" : "no")
                      << " owner=" << (park_observation.owner_stop_requested ? "yes" : "no") << " phase=";
            if (park_observation.frontend_phase_valid) {
                std::cout << park_observation.frontend_phase;
            } else {
                std::cout << "unreadable";
            }
            std::cout << " phase5-park-pc=";
            const auto park_mode = femain_park_pcs->most_frequent();
            if (park_mode.has_value()) {
                std::cout << hex32(park_mode->first) << " park-samples=" << femain_park_pcs->samples() << '/'
                          << park_mode->second << " park-pcs=" << femain_park_pcs->distinct_pcs();
            } else {
                std::cout << "unsampled";
            }
            std::cout << '\n';
            if (femain_alt_phase_pcs.has_value() && femain_alt_phase_pcs->samples() != 0U) {
                const auto alt_mode = femain_alt_phase_pcs->most_frequent();
                std::cout << "  FEMain alt-phase:    phase=" << femain_alt_phase << " pc=";
                if (alt_mode.has_value()) {
                    std::cout << hex32(alt_mode->first) << " samples=" << femain_alt_phase_pcs->samples() << '/'
                              << alt_mode->second << " pcs=" << femain_alt_phase_pcs->distinct_pcs() << " top:[";
                    auto snap = femain_alt_phase_pcs->snapshot();
                    std::sort(snap.begin(), snap.end(),
                              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
                    bool first_pc = true;
                    for (const auto& item : snap) {
                        if (item.second == 0U) {
                            break;
                        }
                        std::cout << (first_pc ? "" : ",") << hex32(item.first) << '=' << item.second;
                        first_pc = false;
                    }
                    std::cout << "]";
                } else {
                    std::cout << "unsampled";
                }
                std::cout << '\n';
            }
        }
        std::cout << "  post-intro frontend: ";
        if (post_intro_frontend_step.has_value()) {
            std::cout << "reached step=" << *post_intro_frontend_step << " flow=" << hex32(0x01EA2D50U)
                      << " updater=" << hex32(0x00438840U) << " background=paused";
        } else {
            std::cout << "pending";
        }
        std::cout << '\n';
        if (host_movie_playback) {
            const auto& movie = host_movie_player.statistics();
            std::cout << "  host movie player:   sessions=" << host_movie_sessions
                      << " status=" << to_string(host_movie_player.status())
                      << " active=" << (host_movie_player.active() ? "yes" : "no") << " input=" << movie.input_bytes
                      << " frames=" << movie.frames_received << " converted=" << movie.frames_converted
                      << " staged-total=" << host_movie_frames_staged << " buffers-total=" << host_movie_buffers_staged
                      << " bytes-total=" << host_movie_bytes_staged << " loops-total=" << host_movie_loop_restarts
                      << " rate=" << movie.source_frame_rate_numerator << '/' << movie.source_frame_rate_denominator
                      << " media-ms=" << movie.timeline_elapsed_nanoseconds / 1000000U
                      << " due=" << movie.presentation_frames_due << " repeats=" << movie.presentation_frame_repeats
                      << " dropped=" << movie.late_frames_dropped << " unknown-timing=" << movie.unknown_timing_frames
                      << " rate-changes=" << movie.source_rate_changes
                      << " zero-frame-loop-stops=" << movie.zero_frame_loop_terminations;
            if (movie.media_duration_nanoseconds.has_value()) {
                std::cout << " duration-ms=" << *movie.media_duration_nanoseconds / 1000000U;
            } else {
                std::cout << " duration-ms=pending";
            }
            const auto geometry = host_movie_player.geometry();
            if (geometry.has_value()) {
                std::cout << " geometry=" << geometry->width << 'x' << geometry->height
                          << " gs-base=" << hex32(movie.staged_gs_base);
            }
            std::cout << '\n';
        }
    }
    if (host_controller_bridge) {
        std::cout << "  post-title frontend: ";
        if (virtual_ds2_trace.has_value() && virtual_ds2_trace->title_next_flow_step.has_value()) {
            std::cout << "reached step=" << *virtual_ds2_trace->title_next_flow_step << " flow=" << hex32(0x01EA2A10U)
                      << " via-retail-event=yes";
        } else {
            std::cout << "pending";
        }
        std::cout << '\n';
    }
    if (gs_capture_directory.has_value()) {
        const auto& capture = memory.gs_capture();
        std::cout << "  GS capture:          images=" << exported_gs_images << " register-writes=" << capture.register_writes().size()
                  << " diagnostics=" << capture.diagnostics().size() << " directory=\"" << gs_capture_directory->string()
                  << "\"\n";
        if (first_gs_capture.has_value()) {
            std::cout << "  GS first image:      " << first_gs_capture->string() << '\n';
        }
        const std::size_t shown_notes = std::min<std::size_t>(capture.diagnostics().size(), 8U);
        for (std::size_t index = 0U; index < shown_notes; ++index) {
            const auto& diagnostic = capture.diagnostics()[index];
            std::cout << "  GS capture note:     " << guest::to_string(diagnostic.code) << " detail=" << hex32(diagnostic.detail)
                      << " address=" << hex32(diagnostic.address) << '\n';
        }
    }
    if (parallel_gs_stream_path.has_value()) {
        std::cout << "  paraLLEl-GS stream:  VBlanks=" << raised_vblank_starts << " file=\"" << parallel_gs_stream_path->string()
                  << "\"\n";
    } else if (parallel_gs_live_fifo_path.has_value()) {
        std::cout << "  paraLLEl-GS live:    VBlanks=" << raised_vblank_starts << " FIFO=\"" << parallel_gs_live_fifo_path->string()
                  << "\" software=" << (parallel_gs_only ? "disabled" : "enabled");
        if (parallel_gs_live_capture_path.has_value()) {
            std::cout << " capture=\"" << parallel_gs_live_capture_path->string() << '"';
        }
        std::cout << '\n';
    }
    if (parallel_gs_stream_writer != nullptr) {
        const auto& stream = parallel_gs_stream_writer->statistics();
        if (native_model_live != nullptr) {
            app::print_native_model_statistics(*native_model_live);
        }
        std::cout << "  paraLLEl-GS host:    guest-qw=" << stream.guest_quadwords_submitted << " transfer=" << stream.transfer_packets_encoded
                  << '/' << stream.transfer_quadwords_encoded << " vblank=" << stream.vblank_boundaries_encoded
                  << " encoded-bytes=" << stream.encoded_bytes << " payload-copy-bytes=" << stream.payload_copy_bytes
                  << " encode-ns=" << stream.encode_nanoseconds << " output=" << stream.output_write_operations << '/'
                  << stream.output_bytes_written << " output-write-ns=" << stream.output_write_nanoseconds
                  << " fifo-wait=" << stream.fifo_backpressure_waits << '/' << stream.fifo_backpressure_nanoseconds
                  << " output-flush=" << stream.output_flush_operations << '/' << stream.output_flush_nanoseconds
                  << " mirror=" << stream.mirror_write_operations << '/' << stream.mirror_bytes_written
                  << " mirror-write-ns=" << stream.mirror_write_nanoseconds << " mirror-flush=" << stream.mirror_flush_operations
                  << '/' << stream.mirror_flush_nanoseconds << '\n';
        std::cout << "  paraLLEl-GS transport: segments=" << stream.segments_published << " oversize=" << stream.oversize_segments
                  << " producer-wait=" << stream.producer_wait_operations << '/' << stream.producer_wait_nanoseconds << '\n';
    }
    if (gs_semantic_trace_writer != nullptr) {
        const auto& stats = gs_semantic_trace_writer->statistics();
        std::cout << "  GS semantic trace:   events=" << stats.events_written << " omitted=" << stats.events_omitted
                  << " filtered=" << stats.events_filtered << " tags=" << stats.tags_observed
                  << " images=" << stats.image_transfers_observed << " kicks=" << stats.draw_kicks_observed << " file=\""
                  << gs_semantic_trace_path->string() << "\"\n";
        std::cout << "  GS checkpoints:      count=" << gs_semantic_checkpoints_written;
        if (gs_semantic_last_checkpoint_path.has_value()) {
            std::cout << " last=\"" << gs_semantic_last_checkpoint_path->string() << '"';
        }
        std::cout << '\n';
    }
    if (femain_terminal_diagnostic_path.has_value()) {
        std::cout << "  FEMain producer diagnostic: file=\"" << femain_terminal_diagnostic_path->string()
                  << "\" terminal-authority=none\n";
        if (femain_retail_consume_invocation_barrier != nullptr && femain_retail_consume_observation_adapter != nullptr) {
            std::cout << "  FEMain retail consume: barrier=" << runtime::to_string(femain_retail_consume_invocation_barrier->phase())
                      << " barrier-failure=" << runtime::to_string(femain_retail_consume_invocation_barrier->failure())
                      << " adapter=" << runtime::to_string(femain_retail_consume_observation_adapter->attachment())
                      << " adapter-failure=" << runtime::to_string(femain_retail_consume_observation_adapter->failure())
                      << " records=" << femain_retail_consume_report_records
                      << " terminal-authority=none file=\"" << femain_retail_consume_report_path->string() << "\"\n";
        }
    }
    if (femain_dynamic_callback_report_path.has_value()) {
        std::cout << "  FEMain callback candidate probe: file=\"" << femain_dynamic_callback_report_path->string()
                  << "\" terminal-authority=none\n";
    }
    if (gs_capture_directory.has_value() || gs_semantic_trace_path.has_value() || render_gs) {
        const auto& activity = memory.gs_capture().activity();
        std::cout << "  GS register trace:   gif-tag=" << activity.gif_tag_prim_writes << " packed=" << activity.packed_register_writes
                  << " reglist=" << activity.reglist_register_writes << " ad=" << activity.ad_register_writes
                  << " omitted=" << activity.omitted_register_writes << " image-stream=" << activity.queued_image_transfers
                  << " image-dropped=" << activity.omitted_queued_image_transfers << '\n';
        const auto& display_state = memory.gs_privileged_display().state();
        if (display_state.any_seen()) {
            std::cout << "  GS CRTC observer:    revision=" << display_state.revision;
            const auto print_register = [](const std::string_view label, const guest::GsObservedPrivilegedRegister& value) {
                if (value.seen) {
                    std::cout << ' ' << label << '=' << hex64(value.raw);
                }
            };
            print_register("PMODE", display_state.pmode);
            print_register("SMODE2", display_state.smode2);
            print_register("DISPFB1", display_state.dispfb[0]);
            print_register("DISPLAY1", display_state.display[0]);
            print_register("DISPFB2", display_state.dispfb[1]);
            print_register("DISPLAY2", display_state.display[1]);
            print_register("BGCOLOR", display_state.bgcolor);
            std::cout << '\n';
        }
        const auto display_policy = evaluate_display_policy();
        std::cout << "  GS output policy:     host=" << presentation::to_string(display_policy.host_frame_topology)
                  << " source=" << presentation::to_string(display_policy.source_topology)
                  << " conversion=" << presentation::to_string(display_policy.progressive_conversion)
                  << " fields-forbidden=" << display_policy.field_submission_forbidden << '\n';
    }
    if (render_gs) {
        const auto& capture = memory.gs_capture();
        const auto& renderer = gs_renderer_statistics;
        std::cout << (window_requested ? "  Host window:         SDL GS local-memory scanout; "
                                       : "  GS renderer:         offscreen local-memory scanout; ")
                  << "PPM-history=" << capture.image_transfers().size() << " placed=" << renderer.image_transfers_applied
                  << " rejected=" << renderer.image_transfers_rejected
                  << " local-moves=" << renderer.local_to_local_transfers_applied << '/'
                  << renderer.local_to_local_transfers_rejected << " kicks=" << renderer.drawable_kicks
                  << " ADC-suppressed=" << renderer.adc_suppressed_positions
                  << " XYZ3=" << renderer.nonkicking_positions
                  << " solid-points=" << renderer.solid_points_drawn
                  << " solid-lines=" << renderer.solid_lines_drawn
                  << " solid-sprites=" << renderer.solid_sprites_drawn
                  << " solid-triangles=" << renderer.solid_triangles_drawn
                  << " present-sprites=" << renderer.presentation_sprites_drawn
                  << " textured-sprites=" << renderer.textured_sprites_drawn
                  << " textured-triangles=" << renderer.textured_triangles_drawn
                  << " deferred-textures=" << renderer.textured_primitives_deferred
                  << " empty-primitives=" << renderer.empty_primitives
                  << " pixels=" << renderer.written_pixels
                  << " plan=" << presentation::to_string(host_window_scanout_status);
        if (host_window_scanout_plan.progressive_frame_extraction) {
            std::cout << " conversion=progressive-framebuffer";
        }
        if (!host_window_rgba.empty()) {
            std::cout << " circuit=" << host_window_scanout_plan.circuit
                      << " PSM=" << hex32(host_window_scanout_plan.source_surface.pixel_storage_mode)
                      << " source=" << host_window_scanout_plan.source_width << 'x' << host_window_scanout_plan.source_height
                      << " output=" << host_window_surface_width << 'x' << host_window_surface_height;
        }
        std::cout << " frame=" << presentation::to_string(host_window_render_status);
        std::cout << '\n';
        if (window_requested) {
            std::cout << "  Host presentation:  vblank-fifo-requests=" << host_window_vblank_snapshot_requests
                      << " saturated-vblanks=" << host_window_saturated_vblanks
                      << " final-fifo-requests=" << host_window_final_snapshot_requests
                      << " completed=" << host_window_completed_snapshots << " visual-frames=" << host_window_presented_scanouts
                      << " SDL-presents=" << host_window_present_calls << '\n';
        }
        {
            const auto now = host_steady_now_nanoseconds();
            std::ostringstream rate_report;
            rate_report << std::fixed << std::setprecision(1);
            rate_report << "  Host FPS:           VIS rolling=" << host_window_visual_fps.fps(now)
                        << " avg=" << host_window_visual_fps.average_fps(now) << "; GUEST rolling=" << host_window_guest_fps.fps(now)
                        << " avg=" << host_window_guest_fps.average_fps(now) << "; PRESENT rolling=" << host_window_present_fps.fps(now)
                        << " avg=" << host_window_present_fps.average_fps(now) << '\n';
            std::cout << rate_report.str();
        }
        std::cout << "  GS worker queue:     submitted=" << gs_renderer_queue_statistics.submitted_commands
                  << " completed=" << gs_renderer_queue_statistics.completed_commands
                  << " producer-waits=" << gs_renderer_queue_statistics.producer_waits
                  << " peak-commands=" << gs_renderer_queue_statistics.peak_queued_commands
                  << " peak-image-bytes=" << gs_renderer_queue_statistics.peak_queued_image_bytes
                  << " peak-native-ui-bytes=" << gs_renderer_queue_statistics.peak_queued_native_ui_bytes << '\n';
        std::cout << "  GS raster workers:  threads=" << renderer.raster_worker_threads
                  << " parallel-primitives=" << renderer.raster_parallel_primitives
                  << " tiles=" << renderer.raster_parallel_tiles
                  << " serial-primitives=" << renderer.raster_serial_primitives
                  << " small-fallbacks=" << renderer.raster_small_primitive_fallbacks
                  << " hazard-fallbacks=" << renderer.raster_hazard_fallbacks << '\n';
        std::cout << "  GS CLUT state:      loads=" << renderer.clut_loads_applied
                  << " skipped=" << renderer.clut_loads_skipped
                  << " deferred=" << renderer.clut_loads_deferred << " version=" << renderer.clut_version
                  << " CLD=";
        bool printed_cld = false;
        for (std::size_t condition = 0U; condition < renderer.clut_loads_by_condition.size(); ++condition) {
            const auto loads = renderer.clut_loads_by_condition[condition];
            const auto skips = renderer.clut_skips_by_condition[condition];
            const auto deferred = renderer.clut_deferred_by_condition[condition];
            if (loads + skips + deferred == 0U) {
                continue;
            }
            std::cout << (printed_cld ? "," : "") << condition << "=l" << loads << "/s" << skips << "/d" << deferred;
            printed_cld = true;
        }
        std::cout << (printed_cld ? "\n" : "none\n");
        std::cout << "  GS reject reasons:  ";
        bool printed_rejection_reason = false;
        for (std::size_t index = 0U; index < renderer.rejected_primitives_by_reason.size(); ++index) {
            const auto count = renderer.rejected_primitives_by_reason[index];
            if (count == 0U) {
                continue;
            }
            if (printed_rejection_reason) {
                std::cout << "; ";
            }
            std::cout << guest::to_string(static_cast<guest::GsDrawRejectionReason>(index)) << '=' << count;
            printed_rejection_reason = true;
        }
        std::cout << (printed_rejection_reason ? "\n" : "none\n");
        std::cout << "  GS reject topology: ";
        bool printed_rejection_topology = false;
        for (std::size_t index = 0U; index < renderer.deferred_primitives_by_topology.size(); ++index) {
            const auto deferred = renderer.deferred_primitives_by_topology[index];
            const auto empty = renderer.empty_primitives_by_topology[index];
            const auto incomplete = renderer.incomplete_state_primitives_by_topology[index];
            const auto unsupported = renderer.unsupported_primitives_by_topology[index];
            if (deferred + empty + incomplete + unsupported == 0U) {
                continue;
            }
            if (printed_rejection_topology) {
                std::cout << "; ";
            }
            std::cout << index << "=d" << deferred << "/e" << empty << "/i" << incomplete << "/u" << unsupported;
            printed_rejection_topology = true;
        }
        std::cout << (printed_rejection_topology ? "\n" : "none\n");
        const auto print_psm_rejections = [](const std::string_view label, const auto& histogram) {
            std::cout << label;
            bool printed = false;
            for (std::size_t psm = 0U; psm < histogram.size(); ++psm) {
                if (histogram[psm] == 0U) {
                    continue;
                }
                std::cout << (printed ? " " : "") << hex32(static_cast<std::uint32_t>(psm)) << '=' << histogram[psm];
                printed = true;
            }
            std::cout << (printed ? "\n" : "none\n");
        };
        print_psm_rejections("  GS reject FB PSM:   ", renderer.rejected_framebuffer_psm);
        print_psm_rejections("  GS reject TEX PSM:  ", renderer.rejected_texture_psm);
        print_psm_rejections("  GS reject CLUT PSM: ", renderer.rejected_clut_psm);
        if (scanout_ppm_path.has_value()) {
            std::cout << "  GS scanout PPM:      " << scanout_ppm_path->string() << '\n';
        }
    }
    if (host_window != nullptr) {
        if (host_window_timing_enabled) {
            const auto host_timing = host_window->timing();
            std::cout << "  Host window timing:  enabled=" << (host_timing.enabled ? 1 : 0) << " pump-calls=" << host_timing.pump_calls
                      << " pump-ns=" << host_timing.pump_nanoseconds << " present-calls=" << host_timing.present_calls
                      << " present-ns=" << host_timing.present_nanoseconds << " title-calls=" << host_timing.title_calls
                      << " title-ns=" << host_timing.title_nanoseconds << '\n';
            std::cout << "  Host window SDL:     renderer=\""
                      << (host_timing.renderer_name.empty() ? std::string("unknown") : host_timing.renderer_name)
                      << "\" driver=\""
                      << (host_timing.video_driver.empty() ? std::string("unknown") : host_timing.video_driver)
                      << "\" vsync-requested=" << (host_timing.vsync_requested ? 1 : 0)
                      << " vsync-applied=" << (host_timing.vsync_applied ? 1 : 0)
                      << " vsync-observed=" << (host_timing.vsync_observed ? 1 : 0)
                      << " vsync-interval=" << host_timing.vsync_interval << '\n';
        }
    }
    if (host_window != nullptr) {
        if (skip_compat_present) {
            std::cout << "  Host present policy: compat-present=skipped (default for --parallel-gs-only; "
                         "TAKEDOWN_SKIP_COMPAT_PRESENT=0 restores) ";
        }
        std::cout << "SDL-presents=" << host_window_present_calls << " visual-frames=" << host_window_presented_scanouts << '\n';
    }
    if (startup.main_thread_initialized) {
        std::cout << "  SetupThread:         handled\n";
        std::cout << "  initial SP:          " << hex32(startup.initial_stack_pointer) << '\n'
                  << "  argument block:      " << hex32(startup.argument_block) << '\n';
    }
    if (startup.heap_initialized) {
        std::cout << "  SetupHeap:           handled\n";
        std::cout << "  heap range:          [" << hex32(startup.heap_start) << ", " << hex32(startup.heap_end) << ")\n";
    }
    const auto& gs_crt = kernel.gs_crt_state();
    if (gs_crt.configured) {
        std::cout << "  GS CRT:              interlace=" << gs_crt.interlace << " mode=" << gs_crt.mode
                  << " field=" << gs_crt.field_mode << '\n';
    }
    if (retail_trace) {
        for (const auto& milestone : retail_pc_milestones) {
            std::cout << "  Retail milestone:    " << milestone.label << " hits=" << milestone.hits;
            if (milestone.hits != 0U) {
                std::cout << " first-step=" << milestone.first_step << " last-step=" << milestone.last_step;
            }
            std::cout << '\n';
        }
        for (const auto& event : retail_event_trace) {
            std::cout << "  Retail event:        step=" << event.step << " handler=" << hex32(event.handler)
                      << " object=" << hex32(event.object) << " event=" << hex32(event.event)
                      << " a2=" << hex32(event.argument2) << " a3=" << hex32(event.argument3)
                      << " ra=" << hex32(event.return_address) << '\n';
        }
        std::vector<RetailCallSiteTrace> hottest_retail_calls;
        hottest_retail_calls.reserve(retail_call_sites.size());
        for (const auto& [call_site, trace] : retail_call_sites) {
            hottest_retail_calls.push_back(trace);
        }
        std::ranges::sort(hottest_retail_calls, [](const RetailCallSiteTrace& left, const RetailCallSiteTrace& right) {
            if (left.hits != right.hits) {
                return left.hits > right.hits;
            }
            return left.call_site < right.call_site;
        });
        if (hottest_retail_calls.size() > 24U) {
            hottest_retail_calls.resize(24U);
        }
        for (const auto& call : hottest_retail_calls) {
            std::cout << "  Retail hot call:     " << hex32(call.call_site) << " -> " << hex32(call.target)
                      << " hits=" << call.hits << " first-step=" << call.first_step << " last-step=" << call.last_step << '\n';
        }
    }
    if (memory.contains(0x0051A99CU, 4U) && memory.contains(0x0051A9A0U, 4U) && memory.contains(0x0051A9A4U, 4U) &&
        memory.contains(0x0051A9A8U, 8U) && memory.contains(0x0051A9B4U, 4U)) {
        const auto next_flow = static_cast<std::uint64_t>(memory.read_u32_le(0x0051A9A8U)) |
                               (static_cast<std::uint64_t>(memory.read_u32_le(0x0051A9ACU)) << 32U);
        std::cout << "  Retail movie state:  init-phase=" << memory.read_u32_le(0x0051A99CU)
                  << " playback=" << memory.read_u32_le(0x0051A9A0U) << " index=" << memory.read_u32_le(0x0051A9A4U)
                  << " next-flow=" << hex64(next_flow) << " stream=" << hex32(memory.read_u32_le(0x0051A9B4U)) << '\n';
    }
    if (memory.contains(0x0051A9C0U, 128U) && memory.read_u8(0x0051A9C0U) != 0U) {
        print_bounded_ascii_runs(memory, 0x0051A9C0U, 128U);
    }
    if (memory.contains(0x01E65144U, 4U) && memory.contains(0x01E654D0U, 4U)) {
        std::cout << "  Retail intro state:  outer=" << memory.read_u32_le(0x01E65144U)
                  << " inner=" << memory.read_u32_le(0x01E654C8U) << " terminal=" << memory.read_u32_le(0x01E654CCU)
                  << " ready=" << static_cast<unsigned int>(memory.read_u8(0x01E654C0U))
                  << " resource=" << hex32(memory.read_u32_le(0x01E654B0U)) << " file=" << hex32(memory.read_u32_le(0x01E654C4U))
                  << " async=" << hex32(memory.read_u32_le(0x01E654D0U)) << " dnas-module=" << hex32(memory.read_u32_le(0x01E654B4U))
                  << " vid-us=" << hex32(memory.read_u32_le(0x01E654B8U)) << '\n';
    }
    if (memory.contains(0x01E90424U, 4U)) {
        std::cout << "  Retail frontend phase: " << memory.read_u32_le(0x01E90424U)
                  << " timer=" << std::bit_cast<float>(memory.read_u32_le(0x01E9041CU))
                  << " intro-selector=" << static_cast<unsigned int>(memory.read_u8(0x01E898F2U)) << '\n';
        const auto updater = memory.read_u32_le(0x01E90430U);
        std::cout << "  Retail frontend updater: " << hex32(updater);
        if (updater != 0U && memory.contains(updater, 12U)) {
            const auto vtable = memory.read_u32_le(updater);
            std::cout << " vtable=" << hex32(vtable);
            if (vtable != 0U && memory.contains(vtable, 16U)) {
                std::cout << " update=" << hex32(memory.read_u32_le(vtable + 8U))
                          << " close=" << hex32(memory.read_u32_le(vtable + 12U));
            }
            std::cout << " state=" << memory.read_u32_le(updater + 4U)
                      << " done=" << static_cast<unsigned int>(memory.read_u8(updater + 8U))
                      << " option=" << static_cast<unsigned int>(memory.read_u8(updater + 9U))
                      << " active=" << static_cast<unsigned int>(memory.read_u8(updater + 10U));
        }
        std::cout << '\n';
    }
    if (memory.contains(0x01EA2970U, 24U) && memory.contains(0x01EA2C20U, 184U)) {
        const auto current = memory.read_u32_le(0x01EA2974U);
        std::cout << "  Retail frontend flow: vtable=" << hex32(memory.read_u32_le(0x01EA2970U)) << " current=" << hex32(current)
                  << " context=" << hex32(memory.read_u32_le(0x01EA2978U)) << " table=" << hex32(memory.read_u32_le(0x01EA297CU))
                  << " auxiliary=" << hex32(memory.read_u32_le(0x01EA2980U)) << " new-profile=" << hex32(0x01EA2C20U)
                  << " new-profile-callback=" << hex32(memory.read_u32_le(0x01EA2C2CU));
        if (current != 0U && memory.contains(current, 16U)) {
            std::cout << " current-callback=" << hex32(memory.read_u32_le(current + 12U));
        }
        std::cout << '\n';
        const auto table = memory.read_u32_le(0x01EA297CU);
        if (table != 0U && memory.contains(table, 16U)) {
            for (std::size_t index = 0U; index < 64U; ++index) {
                const auto descriptor = static_cast<std::uint32_t>(index) * 16U + table;
                if (!memory.contains(descriptor, 12U)) {
                    break;
                }
                const auto low = memory.read_u32_le(descriptor);
                const auto high = memory.read_u32_le(descriptor + 4U);
                if ((low | high) == 0U) {
                    break;
                }
                const auto object = memory.read_u32_le(descriptor + 8U) + 0x01EA2970U;
                if (current != object && object != 0x01EA2C20U) {
                    continue;
                }
                const auto type = (static_cast<std::uint64_t>(high) << 32U) | low;
                std::cout << "  Retail flow type:    object=" << hex32(object) << " type=" << hex64(type)
                          << " offset=" << hex32(object - 0x01EA2970U) << '\n';
            }
        }
    }
    if (memory.contains(0x01E91C3CU, 2U) && memory.contains(0x01E900B0U, 4U) && memory.contains(0x01E8E8A0U, 12U) &&
        memory.contains(0x01E65499U, 1U)) {
        std::cout << "  Retail special updater: mode=" << memory.read_u32_le(0x01E900B0U)
                  << " wait=" << static_cast<unsigned int>(memory.read_u8(0x01E91C3CU)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x01E91C3DU))
                  << " overlay=" << static_cast<unsigned int>(memory.read_u8(0x01E8E8AAU))
                  << " trigger=" << static_cast<unsigned int>(memory.read_u8(0x01E65499U))
                  << " overlay-vtable=" << hex32(memory.read_u32_le(0x01E8E8A0U)) << '\n';
    }
    if (memory.contains(0x004EB1E0U, 4U)) {
        std::cout << "  Retail frontend gate:  " << static_cast<unsigned int>(memory.read_u8(0x004EB1E0U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x004EB1E1U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x004EB1E2U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x004EB1E3U)) << '\n';
    }
    constexpr std::array<std::uint32_t, 15> kRetailControllerActionOffsets{20U,  32U,  44U,  56U,  68U,  92U,  140U, 152U,
                                                                           164U, 176U, 212U, 224U, 248U, 260U, 272U};
    if (memory.contains(0x004F6060U, 5U) && memory.contains(0x004F5E08U, 574U)) {
        std::cout << "  Retail controllers:  metadata=";
        for (std::size_t index = 0U; index < 5U; ++index) {
            std::cout << (index == 0U ? "" : "/")
                      << static_cast<unsigned int>(memory.read_u8(0x004F6060U + static_cast<std::uint32_t>(index)));
        }
        for (std::size_t slot = 0U; slot < 2U; ++slot) {
            const auto base = static_cast<std::uint32_t>(slot) * 300U + 0x004F5E08U;
            std::cout << " slot" << slot << "=[";
            for (std::size_t action = 0U; action < kRetailControllerActionOffsets.size(); ++action) {
                std::cout << (action == 0U ? "" : ",")
                          << static_cast<unsigned long>(memory.read_u16_le(base + kRetailControllerActionOffsets[action]));
            }
            std::cout << ']';
        }
        std::cout << '\n';
    }
    if (memory.contains(0x004F52C0U, 104U) && memory.contains(0x004F5340U, 236U) && memory.contains(0x004F52D8U, 64U) &&
        memory.contains(0x004F5E0CU, 4U) && memory.contains(0x004F6060U, 7U)) {
        std::cout << "  Retail DS2 manager:  devices=" << hex32(memory.read_u32_le(0x004F52C0U)) << '/'
                  << hex32(memory.read_u32_le(0x004F52C4U)) << " states=" << memory.read_u32_le(0x004F52D0U) << '/'
                  << memory.read_u32_le(0x004F52D4U) << " handles=" << static_cast<int>(memory.read_u32_le(0x004F5318U)) << '/'
                  << static_cast<int>(memory.read_u32_le(0x004F531CU)) << " active=" << memory.read_u32_le(0x004F5324U)
                  << " device0=" << hex32(memory.read_u32_le(0x004F5340U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x004F5348U)) << '/' << hex32(memory.read_u32_le(0x004F5424U)) << '/'
                  << memory.read_u32_le(0x004F5428U) << " action-device=" << hex32(memory.read_u32_le(0x004F5E0CU)) << " map=";
        for (std::size_t index = 0U; index < 7U; ++index) {
            std::cout << (index == 0U ? "" : "/")
                      << static_cast<unsigned int>(memory.read_u8(0x004F6060U + static_cast<std::uint32_t>(index)));
        }
        std::cout << " decoded0=";
        for (std::size_t index = 0U; index < 32U; ++index) {
            std::cout << (index == 0U ? "" : ".") << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned int>(memory.read_u8(0x004F52D8U + static_cast<std::uint32_t>(index)));
        }
        std::cout << std::dec << std::setfill(' ');
        std::cout << " decoded1=";
        for (std::size_t index = 0U; index < 32U; ++index) {
            std::cout << (index == 0U ? "" : ".") << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned int>(memory.read_u8(0x004F52F8U + static_cast<std::uint32_t>(index)));
        }
        std::cout << std::dec << std::setfill(' ');
        std::cout << '\n';
    }
    if (memory.contains(0x0051A9A0U, 4U) && memory.contains(0x0051BA88U, 4U) && memory.contains(0x004F6060U, 2U) &&
        memory.contains(0x0051BA94U, 8U)) {
        const auto application = memory.read_u32_le(0x0051BA88U);
        std::cout << "  Retail application:  object=" << hex32(application) << " movie-state=" << memory.read_u32_le(0x0051A9A0U)
                  << " selected-controllers=" << static_cast<int>(static_cast<std::int8_t>(memory.read_u8(0x004F6060U))) << '/'
                  << static_cast<int>(static_cast<std::int8_t>(memory.read_u8(0x004F6061U)))
                  << " route=" << memory.read_u32_le(0x0051BA94U) << " route-ready=" << memory.read_u32_le(0x0051BA98U);
        if (application != 0U && memory.contains(application, 444U)) {
            std::cout << " active=" << hex32(memory.read_u32_le(application + 436U))
                      << " pending=" << hex32(memory.read_u32_le(application + 440U));
        }
        std::cout << '\n';
    }
    if (memory.contains(0x0051BAA0U, 4U)) {
        std::cout << "  Retail bootstrap state: " << memory.read_u32_le(0x0051BAA0U) << '\n';
    }
    if (memory.contains(0x01E7751CU, 4U) && memory.contains(0x01E7C844U, 4U)) {
        std::cout << "  Retail resource loader: state=" << hex32(memory.read_u32_le(0x01E7751CU))
                  << " handle=" << hex32(memory.read_u32_le(0x01E7C844U)) << '\n';
        const auto pool_capacity = memory.read_u32_le(0x01E75660U);
        const auto pool_storage = memory.read_u32_le(0x01E75668U);
        const auto pool_free = memory.read_u32_le(0x01E75678U);
        const auto pool_active_tail = memory.read_u32_le(0x01E75680U);
        std::cout << "  Retail resource pool: capacity=" << pool_capacity << " storage=" << hex32(pool_storage)
                  << " free=" << hex32(pool_free) << " active-tail=" << hex32(pool_active_tail) << '\n';
    }
    if (memory.contains(0x01E7A888U, 432U)) {
        const std::uint32_t audio_handle = memory.read_u32_le(0x01E7A948U);
        std::cout << "  Retail movie bootstrap renderer: state=" << memory.read_u32_le(0x01E7A9F8U)
                  << " result=" << memory.read_u32_le(0x01E7A9FCU) << " mode=" << memory.read_u32_le(0x01E7AA00U)
                  << " handles=" << hex32(memory.read_u32_le(0x01E7A9E8U)) << '/' << hex32(memory.read_u32_le(0x01E7A9ECU))
                  << '/' << hex32(memory.read_u32_le(0x01E7A9F0U)) << '/' << hex32(memory.read_u32_le(0x01E7A9F4U))
                  << " flags=" << hex32(memory.read_u32_le(0x01E7AA20U))
                  << " retry=" << static_cast<int>(static_cast<std::int8_t>(memory.read_u8(0x01E7AA1CU)))
                  << " active=" << static_cast<unsigned int>(memory.read_u8(0x01E7AA36U))
                  << " audio=" << static_cast<unsigned int>(memory.read_u8(0x01E7A944U)) << ':'
                  << memory.read_u32_le(0x01E7A950U) << " audio-handle=" << hex32(audio_handle)
                  << " audio-entry=" << hex32(memory.read_u32_le(0x01E7A94CU))
                  << " channel-mode=" << memory.read_u32_le(0x01E7A954U) << " status=" << memory.read_u32_le(0x01E7A958U)
                  << " cursor=" << memory.read_u32_le(0x01E7A95CU) << '/' << memory.read_u32_le(0x01E7A960U)
                  << " bank=" << memory.read_u32_le(0x01E7A968U) << " deadline=" << memory.read_u32_le(0x01E7A96CU)
                  << " audio-flags=" << static_cast<unsigned int>(memory.read_u8(0x01E7A970U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x01E7A971U)) << '/'
                  << static_cast<unsigned int>(memory.read_u8(0x01E7A972U)) << '\n';
        if (audio_handle != 0U && memory.contains(audio_handle, 504U)) {
            const std::uint32_t middleware = memory.read_u32_le(audio_handle + 460U);
            std::cout << "  Retail EATrax channel: source=" << hex32(memory.read_u32_le(audio_handle + 304U))
                      << " format=" << memory.read_u32_le(audio_handle + 308U) << '/'
                      << memory.read_u32_le(audio_handle + 312U) << '/' << memory.read_u32_le(audio_handle + 316U)
                      << " callback=" << hex32(memory.read_u32_le(audio_handle + 452U))
                      << " time=" << hex32(memory.read_u32_le(audio_handle + 456U)) << " middleware=" << hex32(middleware)
                      << " state=" << memory.read_u32_le(audio_handle + 464U)
                      << " voices=" << memory.read_u32_le(audio_handle + 496U)
                      << " initialized=" << static_cast<unsigned int>(memory.read_u8(audio_handle + 500U)) << '/'
                      << static_cast<unsigned int>(memory.read_u8(audio_handle + 501U)) << '/'
                      << static_cast<unsigned int>(memory.read_u8(audio_handle + 502U)) << '\n';
            if (middleware != 0U && memory.contains(middleware, 592U)) {
                std::cout << "  Retail EATrax middleware: state=" << memory.read_u32_le(middleware + 588U)
                          << " vtable=" << hex32(memory.read_u32_le(middleware))
                          << " field70=" << hex32(memory.read_u32_le(middleware + 0x70U))
                          << " field118=" << hex32(memory.read_u32_le(middleware + 0x118U))
                          << " major=" << memory.read_u32_le(middleware + 268U)
                          << " substate=" << memory.read_u32_le(middleware + 316U)
                          << " list=" << hex32(memory.read_u32_le(middleware + 292U))
                          << " transfer-busy=" << memory.read_u32_le(0x004E2CF4U) << '\n';
                if (memory.contains(0x01E83650U, 20U)) {
                    std::cout << "  Retail RWA SPU descriptor:";
                    for (std::uint32_t word = 0U; word < 5U; ++word) {
                        std::cout << ' ' << hex32(memory.read_u32_le((0x007A0D94U + word) * 4U));
                    }
                    std::cout << '\n';
                }
            }
        }
        print_bounded_ascii_runs(memory, 0x01E7A8D0U, 96U);
    }
    for (const auto& query : eatrax_child_query_trace) {
        std::cout << "  Retail EATrax child: step=" << query.step << " node=" << hex32(query.node)
                  << " child=" << hex32(query.child) << " interface=" << hex32(query.interface)
                  << " result=" << query.result << '\n';
    }
    for (const auto& transition : retail_phase_transitions) {
        std::cout << "  Retail phase change: " << transition.from << " -> " << transition.to
                  << " at " << hex32(transition.instruction) << " step=" << transition.step << '\n';
    }
    for (const auto& transition : retail_flow_transitions) {
        std::cout << "  Retail flow change:  " << hex32(transition.from) << " -> " << hex32(transition.to)
                  << " at " << hex32(transition.instruction) << " step=" << transition.step << '\n';
    }
    for (const auto& transition : retail_movie_phase_transitions) {
        std::cout << "  Retail movie init change: " << transition.from << " -> " << transition.to
                  << " at " << hex32(transition.instruction) << " step=" << transition.step << '\n';
    }
    for (const auto& transition : retail_movie_state_transitions) {
        std::cout << "  Retail movie state change: " << transition.from << " -> " << transition.to
                  << " at " << hex32(transition.instruction) << " step=" << transition.step << '\n';
    }
    const auto& ipu_activity = memory.ipu_activity();
    std::cout << "  IPU activity:        control=" << ipu_activity.control_writes << " reset=" << ipu_activity.reset_writes
              << " commands=" << ipu_activity.command_writes << " last=" << hex32(ipu_activity.last_command)
              << " fifo-qw=" << ipu_activity.input_fifo_quadwords << '\n'
              << "  IPU DMA from/to:     starts=" << ipu_activity.from_ipu_dma_starts << '/'
              << ipu_activity.to_ipu_dma_starts << " completed=" << ipu_activity.from_ipu_dma_completions << '/'
              << ipu_activity.to_ipu_dma_completions << " requested-qw=" << ipu_activity.from_ipu_quadwords_requested << '/'
              << ipu_activity.to_ipu_quadwords_requested << '\n';
    if (ipu_activity.command_writes != 0U) {
        std::cout << "  IPU opcode counts:  ";
        for (std::size_t opcode = 0U; opcode < ipu_activity.command_opcode_counts.size(); ++opcode) {
            if (ipu_activity.command_opcode_counts[opcode] != 0U) {
                std::cout << " op" << std::hex << opcode << std::dec << '=' << ipu_activity.command_opcode_counts[opcode];
            }
        }
        std::cout << '\n';
    }
    std::cout << "  IPU macroblock rows: decoded=" << ipu_activity.macroblock_rows_decoded
              << " placed=" << ipu_activity.macroblock_rows_placed << '\n';
    const auto& dmac_completion_totals = memory.dmac_completion_counts();
    std::cout << "  SPR DMA completions: from=" << dmac_completion_totals[8] << " to=" << dmac_completion_totals[9] << '\n';
    const auto& vu1_activity = memory.vu1_activity();
    std::cout << "  VU1 activity:        mpg-uploads=" << vu1_activity.vif1_mpg_uploads
              << " micro-words=" << vu1_activity.vif1_micro_words_written
              << " unpack-cmds=" << vu1_activity.vif1_unpack_commands
              << " unpack-vecs=" << vu1_activity.vif1_unpack_vectors_written
              << " vumem-words=" << vu1_activity.vif1_vumem_words_written << " runs=" << vu1_activity.micro_runs_started
              << " completed=" << vu1_activity.micro_runs_completed << " pairs=" << vu1_activity.micro_pairs_committed
              << " cycles=" << vu1_activity.micro_cycles_elapsed << " stalls=" << vu1_activity.micro_pipeline_stall_cycles
              << " chcr-starts=" << vu1_activity.vif1_chcr_start_writes
              << " non-chain=" << vu1_activity.vif1_chcr_non_chain_entries << " tags=" << vu1_activity.vif1_chain_tags_processed
              << " words=" << vu1_activity.vif1_chain_words_submitted << " chains-ok=" << vu1_activity.vif1_chain_completions
              << " chain-faults=" << vu1_activity.vif1_chain_faults << " fifo-words=" << vu1_activity.vif1_fifo_words_submitted
              << " unsup-cmd=" << vu1_activity.vif1_unsupported_commands
              << " first-unsup=" << hex32(vu1_activity.vif1_first_unsupported_word) << '\n';
    if (vu1_activity.native_strip_submission_jobs != 0U) {
        std::cout << "  Native strip bridge: jobs=" << vu1_activity.native_strip_submission_jobs
                  << " packets=" << vu1_activity.native_strip_submission_packets
                  << " quadwords=" << vu1_activity.native_strip_submission_quadwords
                  << " (native execution jobs=" << vu1_activity.native_strip_execution_jobs
                  << "; remaining jobs retain reference state execution; diagnostic only)\n";
        const auto families = runtime::slus_21050::native_strip_prepared_jobs();
        std::cout << "  Native strip families: specular=" << families[0] << " supplied-alpha=" << families[1]
                  << " two-sided=" << families[2] << " two-sided-specular=" << families[3]
                  << " environment=" << families[4] << '\n';
    }
    if (vu1_activity.native_strip_execution_jobs != 0U) {
        std::cout << "  Native strip execution: jobs=" << vu1_activity.native_strip_execution_jobs
                  << " reference-pairs=0 (experimental completion; unqualified output)\n";
    }
    if (vu1_activity.native_strip_scratch_reset_jobs != 0U) {
        std::cout << "  Native strip scratch reset: jobs=" << vu1_activity.native_strip_scratch_reset_jobs
                  << " (cross-family consumer diagnostic)\n";
    }
    std::cout << "  VU1 XGKICK/PATH1:   attempts=" << vu1_activity.xgkick_packets
              << " completed=" << vu1_activity.xgkick_packets_completed << " faults=" << vu1_activity.xgkick_faults
              << " qwords=" << vu1_activity.xgkick_quadwords << " oversized=" << vu1_activity.xgkick_oversized_tags
              << " oversized-at-submit=" << vu1_activity.xgkick_oversized_tags_present_at_submit
              << " start-tag-changed=" << vu1_activity.xgkick_start_tags_changed_before_transfer;
    if (vu1_activity.xgkick_packets != 0U) {
        std::cout << " first-vector=" << hex32(vu1_activity.xgkick_first_vector);
    }
    std::cout << '\n';
    std::cout << "  VIF1 PATH2 IMAGE:   qwords=" << vu1_activity.vif1_path2_image_quadwords
              << " zero=" << vu1_activity.vif1_path2_zero_image_quadwords
              << " unknown-source=" << vu1_activity.vif1_path2_unknown_image_sources
              << " spans=" << vu1_activity.vif1_zero_image_source_span_count
              << " dropped=" << vu1_activity.vif1_zero_image_source_spans_dropped << '\n';
    for (std::uint32_t index = 0U; index < vu1_activity.vif1_zero_image_source_span_count; ++index) {
        const auto& span = vu1_activity.vif1_zero_image_source_spans[index];
        std::cout << "    zero-span[#" << index << "] source=" << hex32(span.first_source) << ".."
                  << hex32(span.last_source) << " qwords=" << span.quadwords << " chain=" << hex32(span.chain_tadr)
                  << '\n';
    }
    std::cout << "  VIF1 tag histogram:  ";
    for (std::size_t id = 0U; id < vu1_activity.vif1_chain_tag_id_counts.size(); ++id) {
        if (vu1_activity.vif1_chain_tag_id_counts[id] != 0U) {
            std::cout << "id" << id << '=' << vu1_activity.vif1_chain_tag_id_counts[id] << ' ';
        }
    }
    std::cout << '\n';
    std::cout << "  VIF1 walk stops:     ";
    for (std::size_t bucket = 0U; bucket < vu1_activity.vif1_chain_stop_counts.size(); ++bucket) {
        if (vu1_activity.vif1_chain_stop_counts[bucket] != 0U) {

            std::cout << kVif1ChainStopNames[bucket] << '=' << vu1_activity.vif1_chain_stop_counts[bucket] << ' ';
        }
    }
    std::cout << '\n';
    for (std::size_t chain = 0U; chain < vu1_activity.vif1_chain_dump_count; ++chain) {
        const auto& dump = vu1_activity.vif1_chain_dumps[chain];
        std::cout << "  VIF1 chain dump " << chain << ": tadr=" << hex32(dump.start_tadr) << " tags=" << dump.tag_count
                  << " stop=" << kVif1ChainStopNames[static_cast<int>(dump.break_stop)] << '\n';
        for (std::size_t i = 0U; i < dump.tag_count; ++i) {
            const auto& tag = vu1_activity.vif1_chain_dump_tags[chain][i];
            const std::uint32_t qwc = tag.w0 & 0xFFFFU;
            const std::uint32_t tag_id = (tag.w0 >> 28U) & 0x7U;
            std::cout << "    [" << i << "] @" << hex32(tag.tadr) << " id=" << tag_id << " qwc=" << qwc
                      << " irq=" << (tag.w0 >> 31U) << " addr=" << hex32(tag.w1) << " w2=" << hex32(tag.w2)
                      << " w3=" << hex32(tag.w3) << '\n';
        }
    }
    std::cout << "  VU1 cmd histogram:   ";
    for (std::size_t bit = 0U; bit < 128U; ++bit) {
        if (((vu1_activity.vif1_command_bits[bit >> 6U] >> (bit & 63U)) & 1U) != 0U) {
            std::cout << std::hex << bit << " ";
        }
    }
    std::cout << std::dec << '\n';
    if (vu1_activity.vif1_tadr_history_count != 0U) {
        std::cout << "  VU1 first TADRs:     ";
        for (std::size_t i = 0U; i < vu1_activity.vif1_tadr_history_count; ++i) {
            std::cout << hex32(vu1_activity.vif1_first_tadrs[i]) << ' ';
        }
        std::cout << '\n';
    }
    if (vu1_activity.vif1_tadr_write_count != 0U) {
        std::cout << "  VIF1 TADR writes:    count=" << vu1_activity.vif1_tadr_write_count << '\n';
        for (std::uint32_t i = 0U, shown = std::min<std::uint32_t>(vu1_activity.vif1_tadr_write_count, 16U); i < shown; ++i) {
            std::cout << "    [#" << i << "] value=" << hex32(vu1_activity.vif1_tadr_write_log[i].value)
                      << " pc=" << hex32(vu1_activity.vif1_tadr_write_log[i].pc) << '\n';
        }
    }
    if (vu1_activity.vif1_madr_write_count != 0U) {
        std::cout << "  VIF1 MADR writes:    count=" << vu1_activity.vif1_madr_write_count << '\n';
        for (std::uint32_t i = 0U, shown = std::min<std::uint32_t>(vu1_activity.vif1_madr_write_count, 16U); i < shown; ++i) {
            std::cout << "    [#" << i << "] value=" << hex32(vu1_activity.vif1_madr_write_log[i].value)
                      << " pc=" << hex32(vu1_activity.vif1_madr_write_log[i].pc) << '\n';
        }
    }
    if (vu1_activity.vif1_fifo_write_log_count != 0U) {
        std::cout << "  VIF1 FIFO writes:    count=" << vu1_activity.vif1_fifo_words_submitted << '\n';
        const std::uint32_t shown = std::min<std::uint32_t>(vu1_activity.vif1_fifo_write_log_count, 16U);
        for (std::uint32_t i = 0U; i < shown; ++i) {
            std::cout << "    [#" << i << "] value=" << hex32(vu1_activity.vif1_fifo_write_log[i].value)
                      << " pc=" << hex32(vu1_activity.vif1_fifo_write_log[i].pc) << '\n';
        }
    }
    if (vu1_activity.vif1_chain_faults != 0U) {
        std::cout << "  VIF1 chain fault:    reason=" << vu1_activity.vif1_first_chain_fault_reason
                  << " tadr=" << hex32(vu1_activity.vif1_first_chain_fault_tadr) << '\n';
    }
    if (vu1_activity.saw_mscal) {
        std::cout << "  VU1 first MSCAL:     entry=" << hex32(vu1_activity.first_mscal_entry) << '\n';
    }
    if (vu1_activity.ee_vu1_micro_writes != 0U) {
        std::cout << "  VU1 EE micro writes: count=" << vu1_activity.ee_vu1_micro_writes
                  << " first=" << hex32(vu1_activity.ee_vu1_micro_first_addr) << '\n';
    }
    if (vu1_activity.micro_run_faults != 0U) {
        std::cout << "  VU1 first fault:     entry=" << hex32(vu1_activity.first_fault_entry)
                  << " pc=" << hex32(vu1_activity.first_fault_pc)
                  << " slot=" << guest::to_string(vu1_activity.first_fault_slot)
                  << " status=" << guest::to_string(vu1_activity.first_fault_status)
                  << " upper=" << hex32(vu1_activity.first_fault_upper_word)
                  << " lower=" << hex32(vu1_activity.first_fault_lower_word) << '\n'
                  << "  VU1 last fault:      pc=" << hex32(vu1_activity.last_fault_pc)
                  << " upper=" << hex32(vu1_activity.last_fault_upper_word)
                  << " lower=" << hex32(vu1_activity.last_fault_lower_word) << '\n';
        std::cout << "  VU1 fault signatures:";
        for (std::size_t index = 0U; index < vu1_activity.fault_signature_count; ++index) {
            const auto& signature = vu1_activity.fault_signatures[index];
            std::cout << "\n    [#" << index << "] count=" << signature.count << " entry=" << hex32(signature.entry)
                      << " pc=" << hex32(signature.pc) << " slot=" << guest::to_string(signature.slot)
                      << " status=" << guest::to_string(signature.status) << " upper=" << hex32(signature.upper_word)
                      << " lower=" << hex32(signature.lower_word);
        }
        if (vu1_activity.fault_signatures_dropped != 0U) {
            std::cout << "\n    dropped=" << vu1_activity.fault_signatures_dropped;
        }
        std::cout << '\n';
    }
    const auto vu1_cache_stats = memory.vu1_program_cache().statistics();
    std::cout << "  VU1 program cache:   hashes=" << vu1_cache_stats.generations_hashed
              << " image-hit/miss=" << vu1_cache_stats.image_hits << '/' << vu1_cache_stats.image_misses
              << " evicted=" << vu1_cache_stats.image_evictions << " decoded=" << vu1_cache_stats.pairs_decoded
              << " predecoded=" << vu1_cache_stats.pairs_predecoded << " cfg-flow=" << vu1_cache_stats.control_flow_pairs
              << " hot=" << vu1_cache_stats.hot_promotions
              << " descriptor-dispatches=" << vu1_cache_stats.compiled_dispatches
              << " native-executor-dispatches=" << vu1_cache_stats.compiled_function_dispatches
              << " direct-predecoded=" << vu1_cache_stats.direct_predecoded_dispatches
              << " static-block=" << vu1_cache_stats.static_block_dispatches
              << " static-lookup=" << vu1_cache_stats.static_block_resolver_hits << '/'
              << vu1_cache_stats.static_block_resolver_calls << " static-pairs=" << vu1_cache_stats.static_block_pairs
              << " static-fallback=" << vu1_cache_stats.static_block_fallback_pairs
              << " static-range=" << vu1_cache_stats.static_block_range_lookups << '/'
              << vu1_cache_stats.static_block_range_activations << '/' << vu1_cache_stats.static_block_range_pairs
              << " static-range-fallback=" << vu1_cache_stats.static_block_range_interior_fallback_pairs << '/'
              << vu1_cache_stats.static_block_range_short_fallback_pairs << '/'
              << vu1_cache_stats.static_block_range_budget_fallback_pairs << '/'
              << vu1_cache_stats.static_block_range_flow_fallback_pairs
              << " upper-stage=" << vu1_cache_stats.static_upper_stage_hits << '/'
              << vu1_cache_stats.static_upper_stage_fallbacks
              << " linear-scoreboard-pairs=" << vu1_cache_stats.linear_scoreboard_pairs
              << " minimal-execution-dispatches=" << vu1_cache_stats.minimal_execution_dispatches
              << " local-inline-block-hits=" << vu1_cache_stats.local_inline_block_hits
              << " local-inline-block-pairs=" << vu1_cache_stats.local_inline_block_pairs
              << " native-scheduled-blocks=" << vu1_cache_stats.native_scheduled_blocks
              << " native-scheduled-pairs=" << vu1_cache_stats.native_scheduled_pairs
              << " native-transfer-blocks=" << vu1_cache_stats.native_transfer_blocks
              << " native-flag-liveness-blocks=" << vu1_cache_stats.native_flag_liveness_blocks
              << " native-control-pairs=" << vu1_cache_stats.native_control_pairs
              << " native-control-fast-pairs=" << vu1_cache_stats.native_control_fast_pairs
              << " auto-upper=" << (disable_vu1_upper_stages ? "disabled" : "enabled")
              << " auto-natural-blocks=" << (disable_vu1_natural_blocks ? "disabled" : "enabled")
              << " lower-plans=" << vu1_cache_stats.opcode_threaded_lower_stage_hits << '/'
              << vu1_cache_stats.opcode_threaded_lower_stage_fallbacks
              << " auto-lower-plans=" << (disable_vu1_lower_plans ? "disabled" : "enabled")
              << " simple-alu-lower-plans="
              << (memory.vu1_program_cache().simple_alu_lower_plans_enabled() ? "enabled" : "disabled")
              << " ilw-lower-plans=" << (memory.vu1_program_cache().ilw_lower_plans_enabled() ? "enabled" : "disabled")
              << " isw-lower-plans=" << (memory.vu1_program_cache().isw_lower_plans_enabled() ? "enabled" : "disabled")
              << " branch-lower-plans=" << vu1_cache_stats.branch_lower_plan_hits << '/'
              << vu1_cache_stats.branch_lower_plan_fallbacks << " branch-lower="
              << (memory.vu1_program_cache().branch_lower_plans_enabled() && !disable_vu1_lower_plans ? "enabled"
                                                                                                       : "disabled")
              << " vi-history=" << "sparse" << " flag-read-lower-plans=" << vu1_cache_stats.flag_read_lower_plan_hits << '/'
              << vu1_cache_stats.flag_read_lower_plan_fallbacks << " flag-read-lower="
              << (memory.vu1_program_cache().flag_read_lower_plans_enabled() && !disable_vu1_lower_plans ? "enabled"
                                                                                                          : "disabled")
              << " fmand=" << vu1_cache_stats.fmand_lower_plan_hits << '/' << vu1_cache_stats.fmand_lower_plan_fallbacks
              << " fcget=" << vu1_cache_stats.fcget_lower_plan_hits << '/' << vu1_cache_stats.fcget_lower_plan_fallbacks
              << " clip-predicate-lower-plans=" << vu1_cache_stats.clip_predicate_lower_plan_hits << '/'
              << vu1_cache_stats.clip_predicate_lower_plan_fallbacks << " clip-predicate-lower="
              << (memory.vu1_program_cache().clip_predicate_lower_plans_enabled() && !disable_vu1_lower_plans
                      ? "enabled"
                      : "disabled")
              << " fcand=" << vu1_cache_stats.fcand_lower_plan_hits << '/' << vu1_cache_stats.fcand_lower_plan_fallbacks
              << " fcor=" << vu1_cache_stats.fcor_lower_plan_hits << '/' << vu1_cache_stats.fcor_lower_plan_fallbacks
              << " natural-block=" << vu1_cache_stats.runtime_hot_block_range_lookups << '/'
              << vu1_cache_stats.runtime_hot_block_activations << '/' << vu1_cache_stats.runtime_hot_block_pairs
              << " natural-fallback=" << vu1_cache_stats.runtime_hot_block_interior_fallback_pairs << '/'
              << vu1_cache_stats.runtime_hot_block_short_fallback_pairs << '/'
              << vu1_cache_stats.runtime_hot_block_budget_fallback_pairs << '/'
              << vu1_cache_stats.runtime_hot_block_pending_flow_fallback_pairs << '/'
              << vu1_cache_stats.runtime_hot_block_flow_mismatch_fallback_pairs
              << " async=" << vu1_cache_stats.background_predecode_requests << '/'
              << vu1_cache_stats.background_predecode_completions << '/' << vu1_cache_stats.background_predecode_activations
              << " stale=" << vu1_cache_stats.background_predecode_stale_results
              << " retry=" << vu1_cache_stats.background_predecode_queue_retries
              << " profile-dropped=" << vu1_cache_stats.profiles_dropped << '\n';
    const auto& deopts = vu1_cache_stats.aot_deopts;
    const auto& declines = vu1_cache_stats.aot_declines;
    std::uint64_t aot_deopts = 0U;
    for (const std::uint64_t count : deopts) {
        aot_deopts += count;
    }
    std::uint64_t aot_declines = 0U;
    for (const std::uint64_t count : declines) {
        aot_declines += count;
    }
    std::cout << "VU1 AOT: module=" << (guest::vu1_aot_module_linked() ? "linked" : "absent")
              << " enabled=" << (memory.vu1_program_cache().aot_enabled() ? 1 : 0)
              << " identity-checks=" << vu1_cache_stats.aot_identity_checks
              << " identity-misses=" << vu1_cache_stats.aot_identity_misses << " launches=" << vu1_cache_stats.aot_launches
              << " completed=" << vu1_cache_stats.aot_completed << " pairs=" << vu1_cache_stats.aot_pairs
              << " deopts=" << aot_deopts << " deopt-pair-limit=" << deopts[static_cast<std::size_t>(guest::Vu1AotDeoptReason::PairLimit)]
              << " deopt-dynamic-target=" << deopts[static_cast<std::size_t>(guest::Vu1AotDeoptReason::DynamicTarget)]
              << " deopt-untranslated-pair=" << deopts[static_cast<std::size_t>(guest::Vu1AotDeoptReason::UntranslatedPair)]
              << " deopt-variant-limit=" << deopts[static_cast<std::size_t>(guest::Vu1AotDeoptReason::VariantLimit)] << " declines=" << aot_declines
              << " decline-reference-tier=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::ReferenceTier)]
              << " decline-profiling=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::Profiling)]
              << " decline-float-environment=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::FloatEnvironment)]
              << " decline-xgkick-mode=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::XgkickMode)]
              << " decline-pair-limit=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::PairLimit)]
              << " decline-status-sticky=" << declines[static_cast<std::size_t>(guest::Vu1AotDeclineReason::StatusSticky)] << '\n';
    if (vu1_lower_plan_window_telemetry.has_value()) {
        const auto& window = *vu1_lower_plan_window_telemetry;
        if (window.start_captured && window.end_captured) {
            std::cout << "  VU1 native schedule window: total-pairs=" << (window.native_end[0] - window.native_start[0])
                      << " native-pairs=" << (window.native_end[1] - window.native_start[1])
                      << " transfer-blocks=" << (window.native_end[2] - window.native_start[2])
                      << " control-pairs=" << (window.native_end[3] - window.native_start[3])
                      << " control-fast-pairs=" << (window.native_end[4] - window.native_start[4]) << '\n';
        }
        std::cout << "  VU1 simple-ALU lower-plan window: status=" << (window.complete ? "complete" : "incomplete")
                  << " candidate=" << (window.candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.start_hits << " start-fallbacks=" << window.start_fallbacks
                  << " end-hits=" << window.end_hits << " end-fallbacks=" << window.end_fallbacks
                  << " delta-hits=" << window.delta_hits << " delta-fallbacks=" << window.delta_fallbacks
                  << " delta-total=" << window.delta_total << '\n';
        std::cout << "  VU1 ILW lower-plan window: status=" << (window.complete ? "complete" : "incomplete")
                  << " candidate=" << (window.ilw_candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.start_hits << " start-fallbacks=" << window.start_fallbacks
                  << " end-hits=" << window.end_hits << " end-fallbacks=" << window.end_fallbacks
                  << " delta-hits=" << window.delta_hits << " delta-fallbacks=" << window.delta_fallbacks
                  << " delta-total=" << window.delta_total << '\n';
        std::cout << "  VU1 ISW lower-plan window: status=" << (window.complete ? "complete" : "incomplete")
                  << " candidate=" << (window.isw_candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.start_hits << " start-fallbacks=" << window.start_fallbacks
                  << " end-hits=" << window.end_hits << " end-fallbacks=" << window.end_fallbacks
                  << " delta-hits=" << window.delta_hits << " delta-fallbacks=" << window.delta_fallbacks
                  << " delta-total=" << window.delta_total << '\n';
        std::cout << "  VU1 IBEQ/IBNE lower-plan window: status=" << (window.branch_complete ? "complete" : "incomplete")
                  << " candidate=" << (window.branch_candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.branch_start_hits << " start-fallbacks=" << window.branch_start_fallbacks
                  << " end-hits=" << window.branch_end_hits << " end-fallbacks=" << window.branch_end_fallbacks
                  << " delta-hits=" << window.branch_delta_hits << " delta-fallbacks=" << window.branch_delta_fallbacks
                  << " delta-total=" << window.branch_delta_total << '\n';
        std::cout << "  VU1 FMAND/FCGET lower-plan window: status="
                  << (window.flag_read_complete ? "complete" : "incomplete")
                  << " candidate=" << (window.flag_read_candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.flag_read_start_hits << " start-fallbacks=" << window.flag_read_start_fallbacks
                  << " end-hits=" << window.flag_read_end_hits << " end-fallbacks=" << window.flag_read_end_fallbacks
                  << " delta-hits=" << window.flag_read_delta_hits
                  << " delta-fallbacks=" << window.flag_read_delta_fallbacks
                  << " delta-total=" << window.flag_read_delta_total << '\n';
        std::cout << "    FMAND: status=" << (window.fmand_complete ? "complete" : "incomplete")
                  << " delta-hits=" << window.fmand_delta_hits << " delta-fallbacks=" << window.fmand_delta_fallbacks
                  << " delta-total=" << window.fmand_delta_total << '\n';
        std::cout << "    FCGET: status=" << (window.fcget_complete ? "complete" : "incomplete")
                  << " delta-hits=" << window.fcget_delta_hits << " delta-fallbacks=" << window.fcget_delta_fallbacks
                  << " delta-total=" << window.fcget_delta_total << '\n';
        std::cout << "  VU1 FCAND/FCOR lower-plan window: status="
                  << (window.clip_predicate_complete ? "complete" : "incomplete")
                  << " candidate=" << (window.clip_predicate_candidate_enabled ? "enabled" : "disabled")
                  << " start-hits=" << window.clip_predicate_start_hits
                  << " start-fallbacks=" << window.clip_predicate_start_fallbacks
                  << " end-hits=" << window.clip_predicate_end_hits << " end-fallbacks=" << window.clip_predicate_end_fallbacks
                  << " delta-hits=" << window.clip_predicate_delta_hits
                  << " delta-fallbacks=" << window.clip_predicate_delta_fallbacks
                  << " delta-total=" << window.clip_predicate_delta_total << '\n';
        std::cout << "    FCAND: status=" << (window.fcand_complete ? "complete" : "incomplete")
                  << " delta-hits=" << window.fcand_delta_hits << " delta-fallbacks=" << window.fcand_delta_fallbacks
                  << " delta-total=" << window.fcand_delta_total << '\n';
        std::cout << "    FCOR: status=" << (window.fcor_complete ? "complete" : "incomplete")
                  << " delta-hits=" << window.fcor_delta_hits << " delta-fallbacks=" << window.fcor_delta_fallbacks
                  << " delta-total=" << window.fcor_delta_total << '\n';
    }
    if (vu1_worker_differential || vu1_worker_consume) {
        memory.vu1_worker_consume_fence(runtime::Vu1WorkerConsumeReason::Sync);
        vu1_worker_differential_controller.join_deferred_worker();
        vu1_worker_differential_controller.report(std::cout);
    }
    if (vu1_profile_directory.has_value()) {
        std::cout << "  VU1 host timing:     scope=" << (profile_guest_work_window ? "B3IR-A-to-C" : "guest-loop")
                  << " hash=" << vu1_cache_stats.hash_nanoseconds << "ns decode=" << vu1_cache_stats.decode_nanoseconds
                  << "ns execute=" << vu1_cache_stats.execute_nanoseconds << "ns\n";
        std::vector<guest::Vu1ProgramProfile> ranked_profiles(memory.vu1_program_cache().profiles().begin(),
                                                               memory.vu1_program_cache().profiles().end());
        std::sort(ranked_profiles.begin(), ranked_profiles.end(),
                  [](const auto& left, const auto& right) { return left.host_nanoseconds > right.host_nanoseconds; });
        const std::size_t shown = std::min<std::size_t>(ranked_profiles.size(), 8U);
        for (std::size_t index = 0U; index < shown; ++index) {
            const auto& profile = ranked_profiles[index];
            std::cout << "    hot-vu1[#" << index << "] hash=" << hex64(profile.hash.high) << ':' << hex64(profile.hash.low)
                      << " entry=" << hex32(profile.entry) << " resolved=" << hex32(profile.resolved_entry)
                      << " runs=" << profile.runs << " descriptor=" << profile.compiled_runs
                      << " predecoded=" << profile.direct_predecoded_runs << " static-block=" << profile.static_block_runs
                      << " native=" << profile.native_executor_runs << " pairs=" << profile.committed_pairs
                      << " host=" << profile.host_nanoseconds << "ns\n";
        }
        write_vu1_profile_artifacts(*vu1_profile_directory, memory.vu1_program_cache());
        std::cout << "  VU1 profile output:  scope=" << (profile_guest_work_window ? "B3IR-A-to-C" : "guest-loop") << ' '
                  << (*vu1_profile_directory / "profile.tsv").string() << " and "
                  << (*vu1_profile_directory / "pairs.tsv").string() << " and "
                  << (*vu1_profile_directory / "hot-vu1-programs.cpp").string() << '\n';
    }
    if (guest::IpuMpeg2Decoder* const ipu_decoder = memory.ipu_mpeg2_decoder(); ipu_decoder != nullptr) {
        ipu_decoder->flush();
        std::optional<guest::IpuDecodedFrame> latest_content;
        std::size_t concealed_count = 0U;
        while (std::optional<guest::IpuDecodedFrame> frame = ipu_decoder->receive_frame()) {
            if (frame->concealed) {
                ++concealed_count;
            } else {
                latest_content = std::move(frame);
            }
        }
        std::cout << "  IPU decoder:         backend=" << ipu_decoder->backend_name()
                  << " bytes=" << ipu_decoder->bytes_received() << " frames=" << ipu_decoder->frames_decoded()
                  << " concealed=" << concealed_count << '\n';
        if (ipu_frame_dump_path.has_value() && latest_content.has_value()) {
            write_luma_ppm(*ipu_frame_dump_path, latest_content->width, latest_content->height,
                           std::span(latest_content->y_plane));
            std::cout << "  IPU frame dump:     wrote " << ipu_frame_dump_path->string() << " (" << latest_content->width
                      << 'x' << latest_content->height << " luma, checksum=0x" << std::hex << latest_content->checksum
                      << std::dec << ")\n";
        } else if (ipu_frame_dump_path.has_value()) {
            std::cout << "  IPU frame dump:     no complete picture decoded (" << concealed_count
                      << " concealed placeholder frame(s)); skipping\n";
        }
    }
    const auto& rwa_activity = kernel.rwa_activity();
    {
        std::ostringstream rwa_summary;
        rwa_summary << "  RWA commands:        total=" << rwa_activity.command_packets
                    << " last=" << hex32(rwa_activity.last_opcode);
        for (std::size_t opcode = 0U; opcode < 256U; ++opcode) {
            if (rwa_activity.opcode_counts[opcode] != 0U) {
                rwa_summary << " op" << std::hex << std::setfill('0') << std::setw(2) << opcode << std::dec << '='
                            << rwa_activity.opcode_counts[opcode];
            }
        }
        std::cout << rwa_summary.str() << '\n';
    }
    for (const std::uint32_t opcode : {0x11U, 0x13U, 0x15U}) {
        bool any = false;
        for (const std::uint32_t word : rwa_activity.last_packet_words[opcode]) {
            any |= word != 0U;
        }
        if (!any) {
            continue;
        }
        std::cout << "  RWA pkt 0x" << std::hex << opcode << std::dec << ":";
        for (std::size_t w = 0U; w < 12U; ++w) {
            if (rwa_activity.last_packet_words[opcode][w] != 0U) {
                std::cout << " +" << std::hex << (w * 4U) << '=' << rwa_activity.last_packet_words[opcode][w];
            }
        }
        std::cout << std::dec << '\n';
    }
    if (memory.contains(0x004E2CE8U, 8U)) {
        std::cout << "  RWA staging:         ptr=" << hex32(memory.read_u32_le(0x004E2CE8U))
                  << " size=" << hex32(memory.read_u32_le(0x004E2CECU)) << " (retail 0x130100/0x4000) first-op05="
                  << (rwa_first_op05_step_valid ? std::to_string(rwa_first_op05_step) : std::string("never")) << '\n';
        for (std::uint32_t address = 0x004E2C00U; address < 0x004E2D80U; address += 4U) {
            if (!memory.contains(address, 4U)) {
                continue;
            }
            const std::uint32_t& value = memory.read_u32_le(address);
            if (value == 0U) {
                continue;
            }
            std::cout << "    gp-win 0x" << std::hex << address << " = 0x" << value << std::dec << '\n';
        }
        for (const auto& sample : rwa_staging_samples) {
            std::cout << "    staging-change step=" << sample.step << " ptr=" << hex32(sample.pointer)
                      << " size=" << hex32(sample.size)
                      << (rwa_first_op05_step_valid && sample.step < rwa_first_op05_step ? " (pre-send)" : "") << '\n';
        }
    }
    if (memory.contains(0x004E2838U, 8U)) {
        std::cout << "  Movie join:          cb=" << hex32(memory.read_u32_le(0x004E17D8U))
                  << " read=" << hex32(memory.read_u32_le(0x004E2838U))
                  << " commit=" << hex32(memory.read_u32_le(0x004E283CU))
                  << " busy=" << hex32(memory.read_u32_le(0x004E2830U) & 0xFFU)
                  << " bufIdx=" << (memory.read_u32_le(0x004E2804U) & 0xFFU)
                  << " build=" << hex32(memory.read_u32_le(0x004E2808U))
                  << " limit=" << hex32(memory.read_u32_le(0x004E2810U))
                  << " pend=" << hex32(memory.read_u32_le(0x004E281CU))
                  << " slot30/34=" << memory.read_u32_le(0x0051A8A0U) << '/' << memory.read_u32_le(0x0051A8A4U)
                  << " minstat=" << hex32(memory.read_u32_le(0x006662E0U)) << '\n';
    }
    if (rwa_activity.opcode_counts[0x14U] != 0U) {
        std::cout << "  RWA voice snapshot:  returned=" << rwa_activity.voice_snapshots_returned << '/'
                  << rwa_activity.opcode_counts[0x14U] << " bytes=" << rwa_activity.voice_snapshot_return_bytes
                  << " last-dst=" << hex32(rwa_activity.last_voice_snapshot_destination)
                  << " deferred-applied=" << rwa_activity.voice_snapshots_deferred_applied
                  << " deferred-pending=" << rwa_activity.voice_snapshots_deferred_pending << '\n';
    }
    if (rwa_activity.opcode_counts[5U] != 0U) {
        std::cout << "  RWA op05 VoiceTrans: completed=" << rwa_activity.op05_voice_transfer_completed << '/'
                  << rwa_activity.opcode_counts[5U] << " rejected=" << rwa_activity.op05_voice_transfer_rejected
                  << " bytes=" << rwa_activity.op05_voice_transfer_bytes
                  << " last-iop=" << hex32(rwa_activity.last_op05_iop_source)
                  << " last-spu=" << hex32(rwa_activity.last_op05_spu_destination)
                  << " last-size=" << rwa_activity.last_op05_size
                  << " last-fnv=" << hex64(rwa_activity.last_op05_fnv1a64) << '\n';
        const auto& op05_records = rwa_activity.recent_op05_voice_transfers;
        const std::size_t first_op05_record = op05_records.size() > 8U ? op05_records.size() - 8U : 0U;
        if (op05_records.size() > 12U) {
            for (std::size_t index = 0U; index < 12U; ++index) {
                const auto& record = op05_records[index];
                std::cout << "    op05[" << record.sequence << "] iop=" << hex32(record.packet_words[6])
                          << " spu=" << hex32(record.packet_words[7]) << " size=" << record.packet_words[8] << '\n';
            }
            std::cout << "    op05 ... " << (op05_records.size() - 20U) << " more ...\n";
            const auto& first_record = op05_records.front();
            std::cout << "    op05[" << first_record.sequence << "] full packet:";
            for (std::size_t w = 0U; w < 12U; ++w) {
                std::cout << " +" << std::hex << (w * 4U) << '=' << first_record.packet_words[w];
            }
            std::cout << std::dec << '\n';
        }
        for (std::size_t index = first_op05_record; index < op05_records.size(); ++index) {
            const auto& record = op05_records[index];
            std::cout << "    op05[" << record.sequence << "] id=" << record.sif_dma_id
                      << " packet=" << hex32(record.packet_address) << " syscall-pc=" << hex32(record.syscall_pc)
                      << " syscall-ra=" << hex32(record.syscall_ra) << " sender-call=" << hex32(record.sender_callsite)
                      << " iop=" << hex32(record.packet_words[6]) << " spu=" << hex32(record.packet_words[7])
                      << " size=" << record.packet_words[8] << '\n';
        }
    }
    if (rwa_activity.opcode_counts[7U] != 0U || rwa_activity.op07_staging_transfer_completed != 0U) {
        std::cout << "  RWA op07 VoiceTrans: completed=" << rwa_activity.op07_staging_transfer_completed << '/'
                  << rwa_activity.opcode_counts[7U] << " rejected=" << rwa_activity.op07_staging_transfer_rejected
                  << " bytes=" << rwa_activity.op07_staging_transfer_bytes
                  << " cid1-enqueued=" << rwa_activity.op07_staging_responses_enqueued
                  << " cid1-dropped=" << rwa_activity.op07_staging_responses_dropped
                  << " last-stage=" << hex32(rwa_activity.last_op07_staging_address)
                  << " last-spu=" << hex32(rwa_activity.last_op07_spu_destination)
                  << " last-chunk=" << rwa_activity.last_op07_chunk_size
                  << " last-remaining=" << rwa_activity.last_op07_remaining_size
                  << " last-fnv=" << hex64(rwa_activity.last_op07_fnv1a64) << '\n';
        const auto& op07_records = rwa_activity.recent_op07_staging_transfers;
        const std::size_t first_op07_record = op07_records.size() > 8U ? op07_records.size() - 8U : 0U;
        for (std::size_t index = first_op07_record; index < op07_records.size(); ++index) {
            const auto& record = op07_records[index];
            std::cout << "    op07[" << record.sequence << "] id=" << record.sif_dma_id
                      << " packet=" << hex32(record.packet_address) << " stage=" << hex32(record.packet_words[1])
                      << " ee=" << hex32(record.packet_words[6]) << " spu=" << hex32(record.packet_words[7])
                      << " remaining=" << record.packet_words[8] << '\n';
        }
    }
    std::cout << "  EE threads:           count=" << kernel.threads().size() << " current=" << kernel.current_thread_id()
              << '\n';
    {
        std::size_t index = 0U;
        for (const auto& thread : kernel.threads()) {
            if (index < 16U) {
                std::cout << "    thread[" << index << "] status=" << static_cast<std::uint32_t>(thread.status)
                          << " prio=" << thread.current_priority << " entry=0x" << std::hex << thread.entry_point
                          << std::dec << '\n';
            }
            ++index;
        }
    }
    if (rwa_activity.command_packets != 0U) {
        std::cout << "  Voice arming probe:  commits-with-pending=" << rwa_activity.voice_pending_commits << '/'
                  << rwa_activity.command_packets << " max-pending=" << rwa_activity.voice_pending_max << " states=[";
        for (std::size_t i = 0U; i < rwa_activity.voice_state_histogram.size(); ++i) {
            std::cout << i << ':' << rwa_activity.voice_state_histogram[i]
                      << (i + 1U < rwa_activity.voice_state_histogram.size() ? " " : "");
        }
        std::cout << "]\n";
        std::string table;
        std::uint32_t allocated = 0U;
        for (std::uint32_t i = 0U; i < 48U; ++i) {
            const std::uint32_t word = memory.read_u32_le((7998752U + i) * 4U);
            if (word != 0U) {
                ++allocated;
                if (table.size() < 96U) {
                    table += " [" + std::to_string(i) + "]=0x" + hex32(word);
                }
            }
        }
        std::cout << "  Voice alloc table:   " << allocated << "/48 busy" << table << '\n';
    }
    if (rwa_activity.spu_transfer_count != 0U) {
        std::cout << "  RWA SPU transfer:    count=" << rwa_activity.spu_transfer_count << " desc=";
        for (const std::uint32_t word : rwa_activity.last_spu_descriptor) {
            std::cout << ' ' << hex32(word);
        }
        std::cout << " copied=" << rwa_activity.spu_transfer_materialized
                  << " missing-src=" << rwa_activity.spu_transfer_source_missing
                  << " rejected=" << rwa_activity.spu_transfer_bounds_rejected
                  << " bytes=" << rwa_activity.spu_transfer_bytes << " last-iop=" << hex32(rwa_activity.last_spu_iop_source)
                  << " last-spu=" << hex32(rwa_activity.last_spu_destination)
                  << " last-size=" << rwa_activity.last_spu_size << " last-fnv=" << hex64(rwa_activity.last_spu_fnv1a64)
                  << '\n';
        const std::size_t descriptor_count =
            std::min<std::size_t>(rwa_activity.spu_descriptor_history_count, guest::EeRwaActivity::kSpuDescriptorHistory);
        for (std::size_t index = 0U; index < descriptor_count; ++index) {
            const auto& entry = rwa_activity.spu_descriptor_history[index];
            std::cout << "    spu46[" << entry[0] << "] offset=" << hex32(entry[1]) << " spu=" << hex32(entry[2])
                      << " iop=" << hex32(entry[3]) << " size=" << hex32(entry[4]) << " window=" << hex32(entry[5])
                      << '\n';
        }
    }
    if (rwa_activity.bank_probe_history_count != 0U) {
        const std::size_t count = rwa_activity.bank_probe_history_count;
        const std::size_t shown = std::min<std::size_t>(count, guest::EeRwaActivity::kBankProbeHistory);
        const std::size_t first = count > shown ? count - shown : 0U;
        std::cout << "  Bank loader probe:   scans=" << rwa_activity.bank_probe_scans
                  << " snapshots=" << rwa_activity.bank_probe_history_count << '\n';
        for (std::size_t index = 0U; index < shown; ++index) {
            const auto& probe = rwa_activity.bank_probe_history[(first + index) % guest::EeRwaActivity::kBankProbeHistory];
            std::cout << "    bank[cmd " << probe.at_command << "] node=0x" << std::hex << probe.node_address
                      << " state=" << std::dec << probe.bank_state << " loader=0x" << std::hex << probe.loader_ptr
                      << " flags=0x" << probe.flags << " records=0x" << probe.records << " count=" << std::dec
                      << probe.chunk_count << " size=0x" << std::hex << probe.chunk_size << " spu_base=0x"
                      << probe.spu_base << " win{ptr=0x" << probe.window_ptr << " log2=" << std::dec
                      << probe.window_log2 << " base=0x" << std::hex << probe.window_base << " stride=0x"
                      << probe.window_stride << "}" << " inflight=" << std::dec << probe.in_flight << '\n';
        }
    }
    for (const auto& bind : rpc_bind_trace) {
        std::cout << "  SIF RPC bind:        step=" << bind.step << " server=" << hex32(bind.server_id)
                  << " client=" << hex32(bind.client) << " packet=" << hex32(bind.source) << '\n';
    }
    if (rwa_activity.opcode_counts[1U] != 0U) {
        std::cout << "  RWA opcode 01:      ";
        for (const std::uint32_t word : rwa_activity.last_packet_words[1U]) {
            std::cout << ' ' << hex32(word);
        }
        std::cout << '\n';
    }
    for (const auto& submission : rwa_dma_submission_trace) {
        std::cout << "  RWA DMA submit:      id=" << submission.id << " step=" << submission.step
                  << " src=" << hex32(submission.source) << " size-word=" << hex32(submission.words[0])
                  << " cid=" << hex32(submission.words[2]) << " opcode=" << hex32(submission.words[4]);
        if (memory.contains(submission.source + 16U, 4U)) {
            const std::uint32_t final_opcode = memory.read_u32_le(submission.source + 16U);
            std::cout << " final-opcode=" << hex32(final_opcode);
            if (submission.words[4] != final_opcode) {
                std::cout << " (source reused)";
            }
        }
        std::cout << '\n';
        if (submission.words[0] != 48U || submission.words[2] != 0U) {
            std::cout << "    candidate words:  ";
            for (const std::uint32_t word : submission.words) {
                std::cout << ' ' << hex32(word);
            }
            std::cout << '\n';
            for (const auto& dma : submission.descriptors) {
                if (dma.destination == 0xF0000U && dma.size == 48U) {
                    continue;
                }
                std::cout << "    companion DMA:    src=" << hex32(dma.source) << " dst=" << hex32(dma.destination)
                          << " size=" << dma.size << " attr=" << hex32(dma.attributes) << '\n';
            }
            if (submission.extra_words.has_value()) {
                const auto& object = *submission.extra_words;
                std::cout << "    RWA object:       slot=" << object[0] << " dst=" << hex32(object[1])
                          << " bytes=" << object[2] << " offset=" << object[3] << " state=" << object[4]
                          << " callback=" << hex32(object[5]) << " flags=" << hex32(object[7]) << '\n';
            }
        }
    }
    const auto& gtfs_activity = kernel.gtfs_activity();
    std::cout << "  GTFS host I/O:       open=" << gtfs_activity.successful_opens << '/' << gtfs_activity.open_requests
              << " read=" << gtfs_activity.successful_reads << '/' << gtfs_activity.read_requests
              << " sectors=" << gtfs_activity.sectors_requested << " host-bytes=" << gtfs_activity.host_bytes_read
              << " padding=" << gtfs_activity.zero_padding_bytes << " seek=" << gtfs_activity.successful_seeks << '/'
              << gtfs_activity.seek_requests << " close=" << gtfs_activity.successful_closes << '/'
              << gtfs_activity.close_requests << '\n';
    const auto gtfs_records = kernel.gtfs_open_records();
    const std::size_t first_gtfs_record = gtfs_records.size() > 12U ? gtfs_records.size() - 12U : 0U;
    for (std::size_t index = first_gtfs_record; index < gtfs_records.size(); ++index) {
        const auto& record = gtfs_records[index];
        std::cout << "  GTFS file:           " << (record.opened ? "opened" : "rejected") << " guest=\""
                  << record.guest_path << '"';
        if (record.opened) {
            std::cout << " host=\"" << record.host_path.string() << "\" handle=" << record.handle
                      << " size=" << record.file_size << " reads=" << record.successful_reads << '/'
                      << record.read_requests << " sectors=" << record.sectors_requested
                      << " host-bytes=" << record.host_bytes_read << " padding=" << record.zero_padding_bytes
                      << " seeks=" << record.successful_seeks << '/' << record.seek_requests
                      << " closed=" << (record.closed ? "yes" : "no");
        }
        std::cout << '\n';
    }
    const std::string rwa_shadow_failure = [&]() {
        std::ostringstream report;
        if (!rwa_iop_shadow.has_value() || !rwa_iop_shadow->first_failure().has_value()) {
            report << "none";
        } else {
            const auto& failure = *rwa_iop_shadow->first_failure();
            report << audio::RwaIopShadowRuntime::failure_stage_name(failure.stage) << ":op=" << hex32(failure.opcode)
                   << ":delivery=" << static_cast<unsigned>(failure.delivery)
                   << ":reason=" << static_cast<unsigned>(failure.stop.reason) << ":pc=" << hex32(failure.stop.pc)
                   << ":word=" << hex32(failure.stop.instruction) << ":steps=" << failure.steps;
        }
        return report.str();
    }();
    const char* const rwa_shadow_mode_name =
        rwa_shadow_mode == 0 ? "native" : rwa_shadow_mode == 1 ? "boot-oracle" : "full-oracle";
    std::cout << "  RWA native init:     loaded=" << (rwa_native_boot.loaded() ? "yes" : "no")
              << " calls=" << rwa_native_boot_calls << " failures=" << rwa_native_boot_failures
              << " module-start=" << rwa_native_boot_last.module_start_address_writes << '/'
              << rwa_native_boot_last.module_start_core_attribute_writes
              << " op11-addr=" << rwa_native_boot_last.address_writes
              << " param=" << rwa_native_boot_last.parameter_writes
              << " attr=" << rwa_native_boot_last.core_attribute_writes << " dma=" << rwa_native_boot_last.dma_transfers
              << " dma-bytes=" << rwa_native_boot_last.dma_bytes << '\n';
    std::cout << "  RWA IOP shadow:      loaded="
              << (rwa_iop_shadow.has_value() && rwa_iop_shadow->loaded() ? "yes" : "no")
              << " failed=" << (rwa_iop_shadow.has_value() && rwa_iop_shadow->failed() ? "yes" : "no")
              << " mode=" << rwa_shadow_mode_name << " boot-done=" << (rwa_shadow_boot_done ? "yes" : "no")
              << " boot-uploads=" << rwa_shadow_spu2_uploads << " bank-mask=0x" << std::hex
              << static_cast<unsigned>(rwa_shadow_system_bank_mask) << std::dec
              << " snapshots=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->executor().voice_snapshot_count() : 0U)
              << " stream-hle=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->stream_commands_skipped() : 0U)
              << " queue-create=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->queue_creates() : 0U)
              << "/fail=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->queue_create_failures() : 0U)
              << " quiescent=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->quiescent_commands() : 0U)
              << " clock-windows=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->clock_windows() : 0U)
              << " iop-ticks=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->executor().clock_ticks() : 0U)
              << " last-steps=" << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->last_progress().steps : 0U)
              << " boundary="
              << (rwa_iop_shadow.has_value() && rwa_iop_shadow->last_progress().outbound_boundary
                      ? (rwa_iop_shadow->last_progress().quiescent_boundary ? "outbound+quiescent" : "outbound")
                      : (rwa_iop_shadow.has_value() && rwa_iop_shadow->last_progress().quiescent_boundary ? "quiescent"
                                                                                                           : "none"))
              << " module-upload-skips="
              << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->executor().module_upload_skips() : 0U) << std::hex
              << " first-skip=0x"
              << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->executor().first_module_upload_skip_address() : 0U) << '/'
              << (rwa_iop_shadow.has_value() ? rwa_iop_shadow->executor().first_module_upload_skip_size() : 0U)
              << std::dec << " first-fail=" << rwa_shadow_failure << '\n';
    if (rwa_iop_shadow.has_value() && rwa_iop_shadow->loaded() && rwa_iop_shadow->extension_loaded()) {
        const std::uint32_t module_base = rwa_iop_shadow->executor().module_base();
        const std::uint32_t extension_base = rwa_iop_shadow->executor().extension_base();
        const auto read_iop_word = [&](const std::uint32_t address) {
            std::uint32_t value = 0U;
            return rwa_iop_shadow->executor().inspect_word(address, value) ? value : 0U;
        };
        const std::uint32_t handle_table = read_iop_word(extension_base + 37492U);
        int handles_open = 0;
        for (std::uint32_t slot = 0U; slot != 8U; ++slot) {
            if (handle_table != 0U && read_iop_word(slot * 68U + handle_table) != 0U) {
                ++handles_open;
            }
        }
        const std::uint32_t iso_files = read_iop_word(extension_base + 8608U);
        const std::uint32_t iso_dirs = read_iop_word(extension_base + 8612U);
        std::cout << "  GTFS iso table:      files=" << iso_files << " dirs=" << iso_dirs << " names:";
        for (std::uint32_t rec = 0U; rec != 4U && rec < iso_files; ++rec) {
            const std::uint32_t rec_base = rec * 64U + extension_base + 30216U;
            std::string name;
            for (std::uint32_t c = 0U; c != 16U; ++c) {
                std::uint32_t value = 0U;
                if (!rwa_iop_shadow->executor().inspect_word(c + rec_base, value) || value == 0U) {
                    break;
                }
                name.push_back(static_cast<char>(value));
            }
            std::cout << " \"" << name << "\"@0x" << std::hex << read_iop_word(rec_base + 16U) << std::dec;
        }
        std::cout << '\n';
        std::cout << "  RWA retail stream:   vtable=0x" << std::hex << read_iop_word(module_base + 57584U)
                  << " ring-w/r=" << read_iop_word(module_base + 55980U) << '/' << read_iop_word(module_base + 55976U)
                  << " pool-q=" << std::dec << read_iop_word(module_base + 45880U) << " handles-open=" << handles_open
                  << std::hex << " obj0-op=0x" << read_iop_word(module_base + 45516U)
                  << " cmd1-built=0x" << read_iop_word(extension_base + 37632U) << std::dec << '\n';
        for (std::uint32_t slot = 0U; slot != 4U; ++slot) {
            const std::uint32_t object_base = slot * 112U + 459472U;
            std::cout << "    stream-obj[" << slot << "] @0x" << std::hex << object_base
                      << " id=0x" << read_iop_word(object_base) << " buf(+4)=0x" << read_iop_word(object_base + 4U)
                      << " size(+8)=0x" << read_iop_word(object_base + 8U)
                      << " cursor(+c)=0x" << read_iop_word(object_base + 12U)
                      << " state(+10)=0x" << read_iop_word(object_base + 16U)
                      << " +14=0x" << read_iop_word(object_base + 20U)
                      << " +18=0x" << read_iop_word(object_base + 24U) << std::dec << '\n';
        }
        bool any = false;
        for (const std::uint32_t word : rwa_activity.last_packet_words[0x32U]) {
            any |= word != 0U;
        }
        if (any) {
            std::cout << "  RWA pkt 0x32:";
            for (std::size_t w = 0U; w != 12U; ++w) {
                if (rwa_activity.last_packet_words[0x32U][w] != 0U) {
                    std::cout << " +" << std::hex << w * 4U << '=' << rwa_activity.last_packet_words[0x32U][w];
                }
            }
            std::cout << std::dec << '\n';
        }
        if (rwa_iop_shadow.has_value()) {
            const auto text_write = rwa_iop_shadow->executor().first_text_write();
            std::cout << "  RWA text writes:     count=" << rwa_iop_shadow->executor().first_text_write_count();
            if (text_write.pc != 0U) {
                std::cout << " first: pc=0x" << std::hex << text_write.pc << " addr=0x" << text_write.address
                          << " value=0x" << text_write.value << std::dec;
            }
            std::cout << '\n';
            std::cout << "  RWA ring/queue watched writes: " << rwa_iop_shadow->executor().watched_writes().size()
                      << '\n';
            const std::size_t first_watch = rwa_iop_shadow->executor().watched_writes().size() > 24U
                                                ? rwa_iop_shadow->executor().watched_writes().size() - 24U
                                                : 0U;
            for (std::size_t index = first_watch; index < rwa_iop_shadow->executor().watched_writes().size(); ++index) {
                const auto& write = rwa_iop_shadow->executor().watched_writes()[index];
                std::cout << "    [" << index << "] pc=0x" << std::hex << write.pc << " value=0x" << write.value
                          << " off=0x" << write.offset << std::dec << '\n';
            }
        }
    }
    {
        const auto& voice_stats = rwa_spu2_voices.stats();
        std::cout << "  RWA libsd writes:    addr=" << voice_stats.set_addr << " param=" << voice_stats.set_param
                  << " switch=" << voice_stats.set_switch << " kon=" << voice_stats.kon << " koff=" << voice_stats.koff
                  << " rejected=" << voice_stats.rejected << " key-events=" << rwa_key_on_count
                  << " guest-spu2={dma=" << guest_spu2_link.dma_transfers_written()
                  << " dma-bytes=" << guest_spu2_link.dma_bytes_written()
                  << " keyons=" << guest_spu2_link.key_ons_programmed() << " koffs=" << guest_spu2_link.key_offs()
                  << " params=" << guest_spu2_link.param_writes() << " active-voices=" << guest_spu2_link.active_voices()
                  << " endx=0x" << std::hex << guest_spu2->cores[0].endx << '/' << guest_spu2->cores[1].endx << std::dec
                  << "}\n";
    }
    const char* spu2_output_status = "disabled";
    if (spu2_renderer.sink().status() == audio::HostAudioSink::Status::Playing) {
        spu2_output_status = "playing";
    } else if (spu2_renderer.sink().status() == audio::HostAudioSink::Status::Partial) {
        spu2_output_status = "device-unavailable";
    }
    if (spu2_renderer.sink().stretch_fills() != 0U) {
        std::cout << "    audio-time-stretch={fills=" << spu2_renderer.sink().stretch_fills()
                  << " frames=" << spu2_renderer.sink().stretched_frames() << "}\n";
    }
    if (audio_master_clock) {
        std::cout << "    audio-master-clock={blocks=" << audio_master_blocks
                  << " blocked-ns=" << audio_master_blocked_nanoseconds
                  << " final-queued=" << spu2_renderer.sink().queued_frames() << "}\n";
    }
    std::cout << "    spu2-render={frames=" << spu2_renderer.samples_rendered()
              << " non-silent=" << spu2_renderer.non_silent_frames() << " output=" << spu2_output_status
              << " queued=" << spu2_renderer.sink().queued_frames()
              << " no-device-discard=" << spu2_renderer.sink().no_device_frames_discarded()
              << " underruns=" << spu2_renderer.sink().underruns_recovered()
              << " overflow-resyncs=" << spu2_renderer.sink().overflow_resyncs()
              << " overflow-dropped=" << spu2_renderer.sink().overflow_frames_dropped()
              << " fnv=" << hex64(spu2_renderer.fnv1a64()) << "}\n";
    {
        std::string rejects;
        for (unsigned sel = 0U; sel != 256U; ++sel) {
            const std::uint64_t count = rwa_spu2_voices.stats().rejected_by_selector[sel];
            if (count != 0U) {
                char buf[48];
                std::snprintf(buf, sizeof buf, " 0x%02x:%llu", sel, static_cast<unsigned long long>(count));
                rejects += buf;
            }
        }
        if (!rejects.empty()) {
            std::cout << "    rwa-reject-selectors:" << rejects << '\n';
            std::cout << "    rwa-reject-recent:";
            static const char* kinds[] = {"addr", "param", "switch"};
            const std::uint32_t shown = std::min(rwa_spu2_voices.stats().last_rejected_count, 8U);
            for (std::uint32_t i = 0U; i < shown; ++i) {
                char buf[80];
                std::snprintf(buf, sizeof buf, " [%s] entry=0x%04x value=0x%x",
                              kinds[rwa_spu2_voices.stats().last_rejected_kinds[i] % 3],
                              rwa_spu2_voices.stats().last_rejected_entries[i],
                              rwa_spu2_voices.stats().last_rejected_values[i]);
                std::cout << buf;
            }
            std::cout << '\n';
        }
    }
    for (const auto& key_on : recent_rwa_key_ons) {
        std::cout << "    key core=" << static_cast<unsigned>(key_on.core)
                  << " voice=" << static_cast<unsigned>(key_on.voice) << " ssa=0x" << std::hex << key_on.start_address
                  << " pitch=0x" << key_on.pitch << " voll=0x" << key_on.volume_left << " volr=0x"
                  << key_on.volume_right << " adsr1=0x" << key_on.adsr1 << " adsr2=0x" << key_on.adsr2 << std::dec
                  << '\n';
    }
    const guest::IpuMpeg2Decoder* const final_ipu_decoder = memory.ipu_mpeg2_decoder();
    runtime::MediaParityEvidence media_evidence{};
    media_evidence.movie_path_observed = !headless_movie_eos_traces.empty() || ipu_activity.command_writes != 0U ||
                                         ipu_activity.to_ipu_dma_starts != 0U;
    media_evidence.movie_lifecycle_bypass_active = headless_movie_eos;
    media_evidence.movie_decoder_frames = final_ipu_decoder != nullptr ? final_ipu_decoder->frames_decoded() : 0U;
    media_evidence.movie_macroblock_rows_decoded = ipu_activity.macroblock_rows_decoded;
    media_evidence.movie_macroblock_rows_placed = ipu_activity.macroblock_rows_placed;
    media_evidence.movie_guest_frames_staged = host_movie_frames_staged;
    media_evidence.spu_transfer_commands = rwa_activity.spu_transfer_count;
    media_evidence.spu_transfers_materialized = guest_spu2_link.dma_transfers_written();
    media_evidence.spu_transfer_bytes = guest_spu2_link.dma_bytes_written();
    media_evidence.retail_spu2_key_ons = guest_spu2_link.key_ons_programmed();
    media_evidence.retail_spu2_key_offs = guest_spu2_link.key_offs();
    media_evidence.spu2_rendered_frames = spu2_renderer.samples_rendered();
    media_evidence.spu2_non_silent_frames = spu2_renderer.non_silent_frames();
    media_evidence.retail_music_stream_reads = retail_music_stream_reads;
    const runtime::MediaParityReport media_parity = runtime::audit_media_parity(media_evidence);
    std::cout << "  Media parity audit:  overall=" << (media_parity.verified() ? "verified" : "NOT-VERIFIED") << '\n';
    const auto print_media_check = [](const std::string_view name, const runtime::MediaParityCheck& check) {
        std::cout << "    " << name << '=' << runtime::to_string(check.state) << ": " << check.detail << '\n';
    };
    print_media_check("fmv", media_parity.fmv);
    print_media_check("sfx", media_parity.sfx);
    print_media_check("music", media_parity.music);
    if (require_media_parity && !media_parity.verified()) {
        std::cout << "  Media parity gate:   FAIL (--require-media-parity, exit 5 on an otherwise normal stop)\n";
    }
    if (native_simulation_shadow_summary.enabled && !native_simulation_shadow_summary.passed) {
        std::cout << "  Native simulation shadow gate: FAIL; expected complete owner return comparisons with no cycle, trace, or timer failure\n"
                  << "  stop reason:         native simulation shadow failed ("
                  << runtime::to_string(native_simulation_shadow_summary.failure) << ")\n"
                  << "  exit status:         failure\n";
        return 10;
    }
    const auto print_memory_fault = [&]() {
        if (stop.status == guest::EeStepStatus::MemoryAccessFault) {
            const std::uint32_t base = state.read_gpr_u32(stop.instruction.rs);
            const unsigned int address = base + static_cast<unsigned int>(stop.instruction.signed_immediate());
            std::cout << "  memory base:         " << hex32(base) << '\n'
                      << "  effective address:   " << hex32(address) << '\n'
                      << "  caller / stack:      " << hex32(state.read_gpr_u32(31U)) << " / "
                      << hex32(state.read_gpr_u32(29U)) << '\n';
            for (std::size_t first = 4U; first != 24U; first += 4U) {
                std::cout << "  fault registers:    ";
                for (std::size_t index = first; index < first + 4U; ++index) {
                    std::cout << " r" << index << '=' << hex32(state.read_gpr_u32(index));
                }
                std::cout << '\n';
            }
        }
    };
    if (guest_input_timeline_enabled) {
        const bool guest_input_timeline_complete =
            guest_work_measurement_completed_normally &&
            ((guest_input_recorder != nullptr && guest_input_recorder->finished()) ||
             (guest_input_replay != nullptr && guest_input_replay->finish_validated()));
        if (!guest_input_timeline_complete) {
            const char* const underlying = stopped_by_host_window ? "host window closed"
                                           : stopped_without_runnable_thread ? "no runnable EE guest thread"
                                           : outside_guest_memory ? "outside mapped guest memory"
                                           : stopped_at_kernel_call ? "kernel call stop"
                                           : committed_steps == maximum_steps ? "step limit"
                                                                               : "guest stop";
            std::cout << "  Guest input timeline: FAIL; exact A/C measurement completion was not validated\n"
                      << "  stop reason:         incomplete guest input record/replay (" << underlying << ")\n"
                      << "  stop PC:             " << hex32(state.pc()) << " ra=" << hex32(state.read_gpr_u32(31U))
                      << " guest steps=" << committed_steps << '\n'
                      << "  guest stop status:   " << guest::to_string(stop.status)
                      << " word=" << hex32(stop.instruction.word) << '\n'
                      << "  exit status:         failure\n";
            print_memory_fault();
            return 6;
        }
    }
    if (retail_timer_differential_enabled && !retail_timer_differential_passed) {
        std::cout << "  Retail timer gate:   FAIL; expected at least one complete exact projection and no mismatch\n"
                  << "  stop reason:         retail timer differential failed\n"
                  << "  exit status:         failure\n";
        return 7;
    }
    if (retail_timer_consumer_trace_summary.gate_applicable && !retail_timer_consumer_trace_summary.passed) {
        std::cout << "  Retail timer consumer trace gate: FAIL; expected at least one complete entry/return trace and atomic publication\n"
                  << "  stop reason:         retail timer consumer trace failed ("
                  << runtime::to_string(retail_timer_consumer_trace_summary.failure) << ")\n"
                  << "  exit status:         failure\n";
        return 8;
    }
    if (retail_timer_owner_abi_trace_summary.enabled && !retail_timer_owner_abi_trace_summary.passed) {
        std::cout << "  Retail timer owner ABI trace gate: FAIL; expected at least one complete exact owner entry/return trace and atomic publication\n"
                  << "  stop reason:         retail timer owner ABI trace failed ("
                  << runtime::to_string(retail_timer_owner_abi_trace_summary.failure) << ")\n"
                  << "  exit status:         failure\n";
        return 9;
    }
    if (stopped_by_host_window) {
        std::cout << "  stop reason:         host window closed\n"
                  << "  next PC:             " << hex32(state.pc()) << '\n'
                  << "  guest steps:         " << committed_steps << '\n';
        if (require_media_parity &&
            !media_parity.verified()) {
            return 5;
        }
        return 0;
    }
    if (committed_steps == maximum_steps) {
        std::cout << "  stop reason:         "
                  << (guest_work_measurement_completed_normally
                          ? "guest-work measurement exact step budget reached (normal window completion)\n"
                          : "configured step limit reached (normal completion)\n")
                  << "  exit status:         success; no guest fault was observed\n"
                  << "  next PC:             " << hex32(state.pc()) << '\n'
                  << "  ra:                  " << hex32(state.read_gpr_u32(31U)) << '\n'
                  << "  sp:                  " << hex32(state.read_gpr_u32(29U)) << '\n'
                  << "  v0:                  " << hex32(state.read_gpr_u32(2U)) << '\n'
                  << "  a0:                  " << hex32(state.read_gpr_u32(4U)) << '\n'
                  << "  a1:                  " << hex32(state.read_gpr_u32(5U)) << '\n'
                  << "  spin-probe:          entries=" << freeze_spin_entries
                  << " last-target=" << hex32(freeze_last_spin_target) << '\n'
                  << "  GIF CTRL/MODE/STAT:  " << hex32(memory.read_u32_le(0x10003000U)) << ' '
                  << hex32(memory.read_u32_le(0x10003010U)) << ' ' << hex32(memory.read_u32_le(0x10003020U)) << '\n'
                  << "  VIF1 STAT/CODE:      " << hex32(memory.read_u32_le(0x10003C00U)) << ' '
                  << hex32(memory.read_u32_le(0x10003C80U)) << '\n'
                  << "  IPU CMD/CTRL:        " << hex32(memory.read_u32_le(0x10002000U)) << ' '
                  << hex32(memory.read_u32_le(0x10002010U)) << '\n'
                  << "  VIF1 FIFO tail:      " << hex32(memory.read_u32_le(0x10005000U)) << ' '
                  << hex32(memory.read_u32_le(0x10005004U)) << ' ' << hex32(memory.read_u32_le(0x10005008U)) << ' '
                  << hex32(memory.read_u32_le(0x1000500CU)) << '\n'
                  << "  enabled DMAC mask:   " << hex32(kernel.enabled_dmac_channels()) << '\n'
                  << "  DMA queue read/end:  " << hex32(memory.read_u32_le(0x4E2838U)) << ' '
                  << hex32(memory.read_u32_le(0x4E283CU))
                  << " active=" << static_cast<unsigned int>(memory.read_u8(0x4E2830U)) << '\n'
                  << "  VIF0/VIF1/GIF CHCR:  " << hex32(memory.read_u32_le(0x10008000U)) << ' '
                  << hex32(memory.read_u32_le(0x10009000U)) << ' ' << hex32(memory.read_u32_le(0x1000A000U)) << '\n'
                  << "  VIF0 MADR/QWC/TADR:  " << hex32(memory.read_u32_le(0x10008010U)) << ' '
                  << hex32(memory.read_u32_le(0x10008020U)) << ' ' << hex32(memory.read_u32_le(0x10008030U)) << '\n'
                  << "  VIF1 MADR/QWC/TADR:  " << hex32(memory.read_u32_le(0x10009010U)) << ' '
                  << hex32(memory.read_u32_le(0x10009020U)) << ' ' << hex32(memory.read_u32_le(0x10009030U)) << '\n'
                  << "  GIF MADR/QWC/TADR:   " << hex32(memory.read_u32_le(0x1000A010U)) << ' '
                  << hex32(memory.read_u32_le(0x1000A020U)) << ' ' << hex32(memory.read_u32_le(0x1000A030U)) << '\n'
                  << "  FROM_IPU registers:  " << hex32(memory.read_u32_le(0x1000B000U)) << ' '
                  << hex32(memory.read_u32_le(0x1000B010U)) << ' ' << hex32(memory.read_u32_le(0x1000B020U)) << ' '
                  << hex32(memory.read_u32_le(0x1000B030U)) << '\n'
                  << "  TO_IPU registers:    " << hex32(memory.read_u32_le(0x1000B400U)) << ' '
                  << hex32(memory.read_u32_le(0x1000B410U)) << ' ' << hex32(memory.read_u32_le(0x1000B420U)) << ' '
                  << hex32(memory.read_u32_le(0x1000B430U)) << '\n'
                  << "  FROM_SPR registers:  " << hex32(memory.read_u32_le(0x1000D000U)) << ' '
                  << hex32(memory.read_u32_le(0x1000D010U)) << ' ' << hex32(memory.read_u32_le(0x1000D020U)) << ' '
                  << hex32(memory.read_u32_le(0x1000D080U)) << '\n'
                  << "  TO_SPR registers:    " << hex32(memory.read_u32_le(0x1000D400U)) << ' '
                  << hex32(memory.read_u32_le(0x1000D410U)) << ' ' << hex32(memory.read_u32_le(0x1000D420U)) << ' '
                  << hex32(memory.read_u32_le(0x1000D480U)) << '\n'
                  << "  DMAC STAT/PCR:       " << hex32(memory.read_u32_le(0x1000E010U)) << ' '
                  << hex32(memory.read_u32_le(0x1000E020U)) << '\n'
                  << "  INTC STAT/MASK:      " << hex32(memory.intc_status()) << ' ' << hex32(memory.intc_mask()) << '\n'
                  << "  COP0 STATUS/callback:" << ' ' << hex32(state.read_cop0(12U))
                  << " active=" << (kernel.guest_callback_active() ? "yes" : "no") << '\n';
        const std::uint32_t queue_read = memory.read_u32_le(0x4E2838U);
        if (memory.contains(queue_read, 16U)) {
            std::cout << "  DMA queue words:     " << hex32(memory.read_u32_le(queue_read)) << ' '
                      << hex32(memory.read_u32_le(queue_read + 4U)) << ' '
                      << hex32(memory.read_u32_le(queue_read + 8U)) << ' '
                      << hex32(memory.read_u32_le(queue_read + 12U)) << '\n';
        }
        const auto& completion_counts = memory.dmac_completion_counts();
        const auto& callback_counts = kernel.dmac_callback_counts();
        std::cout << "  DMAC events/callbacks: VIF1=" << completion_counts[1] << '/' << callback_counts[1]
                  << " GIF=" << completion_counts[2] << '/' << callback_counts[2]
                  << " FROM_IPU=" << completion_counts[3] << '/' << callback_counts[3]
                  << " TO_IPU=" << completion_counts[4] << '/' << callback_counts[4]
                  << " FROM_SPR=" << completion_counts[8] << '/' << callback_counts[8]
                  << " queued=" << memory.queued_dmac_completion_count()
                  << " pending=" << kernel.pending_dmac_callback_count() << '\n';
        const std::size_t first_callback =
            dmac_callback_trace.size() > 12U ? dmac_callback_trace.size() - 12U : 0U;
        for (std::size_t index = first_callback; index < dmac_callback_trace.size(); ++index) {
            const DmacCallbackTrace& trace = dmac_callback_trace[index];
            std::cout << "    callback ch" << trace.channel << " step=" << trace.dispatch_step
                      << " queue=" << hex32(trace.read_before) << '/' << hex32(trace.end_before)
                      << " active=" << static_cast<unsigned int>(trace.active_before)
                      << " -> step=" << trace.return_step << " queue=" << hex32(trace.read_after) << '/'
                      << hex32(trace.end_after) << " active=" << static_cast<unsigned int>(trace.active_after)
                      << '\n';
        }
        for (const auto& handler : kernel.dmac_handlers()) {
            std::cout << "  DMAC handler:        channel=" << handler.channel << " id=" << handler.id
                      << " fn=" << hex32(handler.handler) << " arg=" << hex32(handler.argument)
                      << " gp=" << hex32(handler.global_pointer) << '\n';
        }
        for (const auto& handler : kernel.intc_handlers()) {
            std::cout << "  INTC handler:        cause=" << handler.cause << " id=" << handler.id
                      << " fn=" << hex32(handler.handler) << " arg=" << hex32(handler.argument)
                      << " gp=" << hex32(handler.global_pointer) << '\n';
        }
        const auto runtime_snapshots = kernel.thread_runtime_snapshots(state);
        for (std::size_t index = 0; index < kernel.threads().size(); ++index) {
            const auto& thread = kernel.threads()[index];
            const auto& snapshot = runtime_snapshots[index];
            std::cout << "  EE thread:           id=" << thread.id << " status=" << hex32(static_cast<std::uint32_t>(thread.status))
                      << " fn=" << hex32(thread.entry_point) << " stack=" << hex32(thread.stack) << '+'
                      << thread.stack_size << " priority=" << thread.current_priority
                      << " arg=" << hex32(thread.argument) << " pc=" << hex32(snapshot.program_counter)
                      << " sp=" << hex32(snapshot.stack_pointer) << " ra=" << hex32(snapshot.return_address)
                      << " wait=" << snapshot.wait_type << ':' << snapshot.wait_id
                      << " wakeups=" << snapshot.wakeup_count << '\n';
        }
        for (const auto& sif_register : kernel.sif_registers()) {
            std::cout << "  SIF register:        " << hex32(sif_register.number) << "=" << hex32(sif_register.value)
                      << '\n';
        }
        const auto transfers = kernel.sif_dma_transfers();
        const std::size_t first_transfer = transfers.size() > 4U ? transfers.size() - 4U : 0U;
        for (std::size_t index = first_transfer; index < transfers.size(); ++index) {
            const auto& transfer = transfers[index];
            std::cout << "  SIF DMA:             id=" << transfer.id << " src=" << hex32(transfer.source)
                      << " dst=" << hex32(transfer.destination) << " size=" << transfer.size
                      << " attr=" << hex32(transfer.attributes) << '\n';
            const std::uint32_t preview_words = std::min<std::uint32_t>(transfer.size >> 2U, 16U);
            if (preview_words != 0U && memory.contains(transfer.source, std::size_t{preview_words} * 4U)) {
                std::cout << "    packet words:     ";
                for (std::uint32_t word = 0; word < preview_words; ++word) {
                    std::cout << ' ' << hex32(memory.read_u32_le(transfer.source + word * 4U));
                }
                std::cout << '\n';
            }
        }
        if (gs_renderer_worker.has_value()) {
            const auto& statistics = gs_renderer_statistics;
            std::cout << "  GS renderer:         points-solid=" << statistics.solid_points_drawn
                      << " lines-solid=" << statistics.solid_lines_drawn
                      << " sprites-solid=" << statistics.solid_sprites_drawn
                      << " tex=" << statistics.textured_sprites_drawn
                      << " pres=" << statistics.presentation_sprites_drawn
                      << " triangles-solid=" << statistics.solid_triangles_drawn
                      << " tex=" << statistics.textured_triangles_drawn
                      << " deferred-tex=" << statistics.textured_primitives_deferred
                      << " incomplete=" << statistics.incomplete_state_primitives
                      << " empty=" << statistics.empty_primitives
                      << " unsupported=" << statistics.unsupported_primitives
                      << " image-tx=" << statistics.image_transfers_applied
                      << " pixels=" << statistics.written_pixels
                      << " raster-threads=" << statistics.raster_worker_threads
                      << " raster-parallel=" << statistics.raster_parallel_primitives << '/'
                      << statistics.raster_parallel_tiles
                      << " raster-serial=" << statistics.raster_serial_primitives << '\n';
            for (std::size_t psm = 0; psm != statistics.deferred_texture_psm.size(); ++psm) {
                if (statistics.deferred_texture_psm[psm] != 0U) {
                    std::cout << "    deferred PSM 0x" << std::hex << psm << std::dec << " ("
                              << statistics.deferred_texture_psm[psm] << ")\n";
                }
            }
            for (std::size_t tfx = 0; tfx != statistics.deferred_texture_tfx.size(); ++tfx) {
                if (statistics.deferred_texture_tfx[tfx] != 0U) {
                    std::cout << "    deferred TFX " << tfx << " (" << statistics.deferred_texture_tfx[tfx]
                              << ")\n";
                }
            }
        }
        return require_media_parity && !media_parity.verified() ? 5 : 0;
    }
    if (stopped_without_runnable_thread) {
        std::cout << "  stop reason:         no runnable EE guest thread\n"
                  << "  stop PC:             " << hex32(state.pc()) << '\n';
        for (const auto& thread : kernel.threads()) {
            std::cout << "  EE thread:           id=" << thread.id << " status=" << hex32(static_cast<std::uint32_t>(thread.status))
                      << " fn=" << hex32(thread.entry_point) << " priority=" << thread.current_priority << '\n';
        }
        return 4;
    }
    if (outside_guest_memory) {
        std::cout << "  stop reason:         outside mapped guest memory\n"
                  << "  stop PC:             " << hex32(state.pc()) << '\n';
        return 4;
    }
    if (stopped_at_kernel_call) {
        std::cout << "  stop reason:         " << guest::to_string(kernel_stop.status) << '\n'
                  << "  stop PC:             " << hex32(stop.instruction.pc) << '\n'
                  << "  syscall:             " << kernel_stop.number << " (" << kernel_stop.name << ")\n"
                  << "  a0:                  " << hex32(state.read_gpr_u32(4U)) << '\n'
                  << "  a1:                  " << hex32(state.read_gpr_u32(5U)) << '\n'
                  << "  a2:                  " << hex32(state.read_gpr_u32(6U)) << '\n'
                  << "  a3:                  " << hex32(state.read_gpr_u32(7U)) << '\n'
                  << "  ra:                  " << hex32(state.read_gpr_u32(31U)) << '\n';
        if (kernel_stop.status == guest::EeKernelCallStatus::Blocked && kernel_stop.number == 68U) {
            const auto semaphore_id = static_cast<int>(state.read_gpr_u32(4U));
            for (const auto& semaphore : kernel.semaphores()) {
                if (semaphore.id == semaphore_id) {
                    std::cout << "  semaphore:           id=" << semaphore.id << " count=" << semaphore.count
                              << " max=" << semaphore.max_count << " waiters=" << semaphore.wait_threads << '\n';
                    break;
                }
            }
            const auto transfers = kernel.sif_dma_transfers();
            const std::size_t first_transfer = transfers.size() > 4U ? transfers.size() - 4U : 0U;
            for (std::size_t index = first_transfer; index < transfers.size(); ++index) {
                const auto& transfer = transfers[index];
                std::cout << "  recent SIF DMA [" << index << "]: src=" << hex32(transfer.source)
                          << " dst=" << hex32(transfer.destination) << " size=" << hex32(transfer.size)
                          << " attr=" << hex32(transfer.attributes) << '\n';
                if (memory.contains(transfer.source, transfer.size) && transfer.size <= 128U) {
                    std::cout << "    words:";
                    for (std::uint32_t offset = 0; offset + 4U <= transfer.size; offset += 4U) {
                        std::cout << ' ' << hex32(memory.read_u32_le(transfer.source + offset));
                    }
                    std::cout << '\n';
                } else if ((transfer.destination == 0xF1900U || transfer.destination == 0xF1B00U ||
                            transfer.destination == 0xF1D00U || transfer.destination == 0xF1F00U ||
                            transfer.destination == 0xF2100U || transfer.destination == 0xF2300U ||
                            transfer.destination == 0xF2500U) &&
                           transfer.size <= 4096U && memory.contains(transfer.source, transfer.size)) {
                    std::cout << "    first words:";
                    const std::uint32_t first_word_count = std::min<std::uint32_t>(transfer.size >> 2U, 8U);
                    for (std::uint32_t word_index = 0; word_index < first_word_count; ++word_index) {
                        std::cout << ' ' << hex32(memory.read_u32_le(transfer.source + word_index * 4U));
                    }
                    std::cout << '\n';
                    print_bounded_ascii_runs(memory, transfer.source, transfer.size);
                }
            }
        }
        if (startup.main_thread_initialized && startup.heap_initialized) {
            std::cout << "  milestone:           retail CRT thread/heap setup completed; linked kernel initializer entered\n"
                      << "  next service:        " << kernel_stop.name << " kernel model required\n";
        }
        if (kernel_stop.status == guest::EeKernelCallStatus::InvalidArguments) {
            return 3;
        }
        if (kernel_stop.number == 64U && startup.main_thread_initialized && startup.heap_initialized) {
            return 0;
        }
        return 4;
    }
    std::cout << "  stop reason:         " << guest::to_string(stop.status) << '\n'
              << "  stop PC:             " << hex32(stop.instruction.pc) << '\n'
              << "  raw word:            " << hex32(stop.instruction.word) << '\n'
              << "  decoded as:          " << guest::format_r5900_instruction(stop.instruction) << '\n';
    if (stop.vu0_micro.has_value()) {
        const auto& vu0_micro = *stop.vu0_micro;
        std::cout << "  VU0 micro status:    " << guest::to_string(vu0_micro.status) << '\n'
                  << "  VU0 micro pair:      " << hex32(vu0_micro.pc) << '\n'
                  << "  VU0 micro slot:      " << guest::to_string(vu0_micro.slot) << '\n'
                  << "  VU0 lower word:      " << hex32(vu0_micro.lower_word) << '\n'
                  << "  VU0 upper word:      " << hex32(vu0_micro.upper_word) << '\n'
                  << "  VU0 pairs committed:" << vu0_micro.committed_pairs << '\n';
        std::cout << "  VU0 micro window:    (pair:lower/upper)";
        for (std::uint16_t offset = 0; offset != 24U; ++offset) {
            const auto pair = static_cast<std::uint16_t>((vu0_micro.pc + offset) & 511U);
            const unsigned int address = 0x11000000U + static_cast<unsigned int>(pair) * 8U;
            std::cout << '\n'
                      << "    " << hex32(pair) << ':' << hex32(memory.read_u32_le(address)) << '/'
                      << hex32(memory.read_u32_le(address + 4U));
        }
        std::cout << '\n'
                  << "  VIF0 MADR/QWC/TADR:  " << hex32(memory.read_u32_le(0x10008010U)) << ' '
                  << hex32(memory.read_u32_le(0x10008020U)) << ' ' << hex32(memory.read_u32_le(0x10008030U)) << '\n';
        std::cout << "  VU0 VI0..VI15:      ";
        for (std::size_t index = 0; index != 16U; ++index) {
            std::cout << (index == 0U ? " " : "/") << hex32(state.read_vu0_integer(index));
        }
        std::cout << '\n';
    }
    print_memory_fault();
    if ((stop.instruction.word >> 26U) == 17U && stop.instruction.rs == 16U) {
        std::cout << "  COP1 FS:             " << hex32(state.read_fpr_bits(stop.instruction.rd)) << '\n'
                  << "  COP1 FT:             " << hex32(state.read_fpr_bits(stop.instruction.rt)) << '\n'
                  << "  COP1 FD (before):    " << hex32(state.read_fpr_bits(stop.instruction.shift)) << '\n';
    }
    if (stop.vu0_micro.has_value()) {
        return 4;
    }
    if ((stop.instruction.word >> 26U) == 18U && (stop.instruction.rs & 16U) != 0U) {
        const auto print_vu0_vector = [&](const std::string_view label, const std::uint8_t index) {
            const guest::Gpr128 value = state.read_vu0_vector(index);
            std::cout << "  " << label << " $vf" << static_cast<unsigned int>(index) << ":      ";
            for (const unsigned int word : value.words) {
                std::cout << ' ' << hex32(word);
            }
            std::cout << '\n';
        };
        print_vu0_vector("VU0 FS", stop.instruction.rd);
        print_vu0_vector("VU0 FT", stop.instruction.rt);
        print_vu0_vector("VU0 FD (before)", stop.instruction.shift);
        std::cout << "  VU0 destination mask:" << hex32(stop.instruction.rs & 15U) << '\n';
    }
    return 4;
}

struct RgbContentSummary {
    std::uint64_t fnv1a64{14695981039346656037ULL};
    std::uint64_t nonblack_pixels{};
};

[[nodiscard]] RgbContentSummary summarize_rgb_content(const std::span<const std::uint8_t> rgba) {
    RgbContentSummary summary;
    for (std::size_t offset = 0; offset + 3U < rgba.size(); offset += 4U) {
        summary.nonblack_pixels +=
            (rgba[offset] != 0U || rgba[offset + 1U] != 0U || rgba[offset + 2U] != 0U) ? 1U : 0U;
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            summary.fnv1a64 ^= rgba[offset + channel];
            summary.fnv1a64 *= 1099511628211ULL;
        }
    }
    return summary;
}

// A frozen savestate carries the CRTC registers but no write history; seed them
// in the order the retail boot programs them so the plan sees a complete state.
[[nodiscard]] takedown::guest::GsPrivilegedDisplayState
display_state_from_crtc(const takedown::platform::Pcsx2GsDumpCrtcRegisters& crtc) {
    takedown::guest::GsPrivilegedDisplayState state{};
    const auto seed = [&state](takedown::guest::GsObservedPrivilegedRegister& slot,
                               const std::uint64_t raw) {
        slot.raw = raw;
        slot.seen = true;
        slot.last_write_sequence = ++state.revision;
    };
    seed(state.pmode, crtc.pmode);
    seed(state.smode2, crtc.smode2);
    seed(state.dispfb[0], crtc.dispfb1);
    seed(state.display[0], crtc.display1);
    seed(state.dispfb[1], crtc.dispfb2);
    seed(state.display[1], crtc.display2);
    seed(state.bgcolor, crtc.bgcolor);
    return state;
}

// The performance paths the release enables (the launcher's host program used to set these). A variable that is
// already set keeps its value, so "0" still selects a comparison path.
void apply_release_defaults() {
    constexpr std::array names{
        "TAKEDOWN_SCALAR_BURST",
        "TAKEDOWN_SCALAR_COLD_OBSERVERS",
        "TAKEDOWN_GRAPHICS_RUN_AHEAD",
        "TAKEDOWN_JOIN_PUMP_CALLBACK_REGION",
        "TAKEDOWN_JOIN_PUMP_CALLBACK_TAIL_REGION",
        "TAKEDOWN_JOIN_PUMP_LOOP_BATCH",
        "TAKEDOWN_JOIN_PUMP_IDLE_SUMMARY",
        "TAKEDOWN_VU1_SIMD_ARITHMETIC",
        "TAKEDOWN_VU1_AVX512_ARITHMETIC",
        "TAKEDOWN_VU1_MINIMAL_COUNTERS",
        "TAKEDOWN_VU1_INLINE_BLOCKS",
        "TAKEDOWN_VU1_NATIVE_SCHEDULE",
        "TAKEDOWN_VU1_NATIVE_TRANSFER_SCHEDULE",
        "TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC",
        "TAKEDOWN_VU1_NATIVE_FLAG_LIVENESS",
        "TAKEDOWN_VU1_AOT"};
    for (const char* const name : names) {
        if (std::getenv(name) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(name, "1");
#else
        setenv(name, "1", 0);
#endif
    }
}

} // namespace

int main(int argc, char** argv) {
try {
    apply_release_defaults();
    // RECOVERY: main.cpp is compiled with TAKEDOWN_DISTRIBUTION_BUILD=1, so only the release branch is
    // verified. The developer build most plausibly had no `#else` at all (the same commands, with the
    // capture/profiling environment and --capture/--profile/--trace/--dump arguments left enabled).
    // TAKEDOWN_NATIVE_ONLY_EXECUTABLE is not defined for this object: every subcommand (including the
    // interpreter-based decode-entry/probe-entry/boot-entry) is present in the release main.
#if TAKEDOWN_DISTRIBUTION_BUILD
    if (!takedown::platform::disable_release_diagnostics()) {
        throw std::runtime_error("Could not disable developer diagnostics");
    }
    for (int index = 1; index < argc; ++index) {
        if (takedown::platform::diagnostic_argument(argv[index])) {
            std::cerr << "Developer captures and profiling are disabled in this release\n";
            return 2;
        }
    }
#endif
    if (argc < 2) {
        print_usage(std::cerr);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "--version" || command == "version") {
        std::cout << takedown::kGameVersionText << '\n';
        return 0;
    }
    if (command == "doctor") {
        const bool bsdtar = exists_on_path("bsdtar");
        const bool chdman = exists_on_path("chdman");
        std::cout << "Milestone 0 host dependencies\n"
                  << "  bsdtar  " << (bsdtar ? "found" : "missing")
                  << "  (required for ISO extraction)\n"
                  << "  chdman  " << (chdman ? "found" : "missing")
                  << "  (required only for CHD import)\n";
        return bsdtar ? 0 : 2;
    }
    if (command == "import") {
        return run_import(argc, argv);
    }
    if (command == "save-data") {
        return run_save_data(argc, argv);
    }
    if (command == "inspect-elf") {
        return run_inspect_elf(argc, argv);
    }
    if (command == "analyze-elf") {
        if (argc != 3) {
            throw std::invalid_argument("analyze-elf requires one boot ELF path");
        }
        const auto analysis = takedown::content::analyze_ps2_elf_static(argv[2]);
        std::cout << takedown::content::elf_static_analysis_json(analysis);
        return 0;
    }
    if (command == "analyze-function") {
        return run_analyze_function(argc, argv);
    }
    if (command == "lift-function-cpp") {
        return run_lift_function_cpp(argc, argv);
    }
    if (command == "lift-instruction-block-cpp") {
        return run_lift_instruction_block_cpp(argc, argv);
    }
    if (command == "generate-block-module-cpp") {
        return run_generate_block_module_cpp(argc, argv);
    }
    if (command == "generate-sparse-block-module-cpp") {
        return run_generate_sparse_block_module_cpp(argc, argv);
    }
    if (command == "generate-leaf-module-cpp") {
        return run_generate_leaf_module_cpp(argc, argv);
    }
    if (command == "decode-entry") {
        return run_decode_entry(argc, argv);
    }
    if (command == "probe-entry") {
        return run_probe_entry(argc, argv);
    }
    if (command == "boot-entry") {
        return run_boot_entry(argc, argv);
    }
    if (command == "pcsx2-probe") {
        return run_pcsx2_probe(argc, argv);
    }
    if (command == "pcsx2-snapshot") {
        return run_pcsx2_snapshot(argc, argv);
    }
    if (command == "pcsx2-inspect-gs") {
        if (argc != 3) {
            throw std::invalid_argument("pcsx2-inspect-gs requires exactly one .gs or .gs.zst path");
        }
        const auto inspection = takedown::platform::inspect_pcsx2_gs_dump(argv[2]);
        std::cout << "PCSX2 GS dump display metadata\n"
                  << "  compression: " << takedown::platform::to_string(inspection.compression)
                  << '\n'
                  << "  input bytes: " << inspection.input_bytes << '\n'
                  << "  decoded bytes: " << inspection.decoded_bytes << '\n'
                  << "  state version: " << inspection.state_version << '\n'
                  << "  state bytes: " << inspection.state_bytes << '\n'
                  << "  packets: transfer=" << inspection.transfer_packets << " ("
                  << inspection.skipped_transfer_bytes
                  << " payload bytes inspected only as aggregate framing)"
                  << " vsync=" << inspection.vsync_packets
                  << " fifo-read=" << inspection.fifo_read_packets
                  << " register=" << inspection.privileged_register_packets << '\n'
                  << "  CRTC source: "
                  << (inspection.final_crtc_came_from_packet ? "last register packet"
                                                             : "initial header")
                  << '\n';
        print_gs_dump_display(inspection.final_crtc);
        print_gs_dump_gif_statistics(inspection.gif);
        std::cout << "  note: no pixels, GIF payloads, serialized state, or files were exported\n";
        return 0;
    }
    if (command == "pcsx2-compare-gs") {
        using takedown::platform::GsSemanticComparisonStatus;
        if (argc != 4) {
            throw std::invalid_argument(
                "pcsx2-compare-gs requires one live .gssem trace and one .gs or .gs.zst dump");
        }
        const auto comparison = takedown::platform::compare_gs_semantic_trace_with_pcsx2_dump(
            std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]});
        std::cout << "GS semantic comparison\n";
        std::cout << "  result:             " << takedown::platform::to_string(comparison.status) << '\n'
                  << "  live VBlank:        " << comparison.live_vblank << " (best of "
                  << comparison.live_intervals_considered << ")\n"
                  << "  PCSX2 interval:     " << comparison.pcsx2_interval << " (best of "
                  << comparison.pcsx2_intervals_considered << ")\n"
                  << "  comparable events: live=" << comparison.live_interval_events
                  << " pcsx2=" << comparison.pcsx2_interval_events << '\n'
                  << "  matching prefix:    " << comparison.matched_prefix_events << '\n'
                  << "  decoder diagnostics: " << comparison.pcsx2_decoder_diagnostics << '\n';
        if (comparison.live_trace_event_limit_reached) {
            std::cout << "  live trace bound:   reached\n";
        }
        if (!comparison.matching_context.empty()) {
            std::cout << "  last matching context:\n";
            for (const auto& line : comparison.matching_context) {
                std::cout << "    " << line << '\n';
            }
        }
        if (comparison.status == GsSemanticComparisonStatus::Diverged) {
            std::cout << "  first live event:   "
                      << comparison.live_mismatch.value_or("<end of interval>") << '\n'
                      << "  first PCSX2 event:  "
                      << comparison.pcsx2_mismatch.value_or("<end of interval>") << '\n';
        }
        if (comparison.image_comparison_available) {
            std::cout << "  IMAGE-only result:  " << takedown::platform::to_string(comparison.image_status)
                      << '\n'
                      << "    intervals:        live VBlank " << comparison.image_live_vblank
                      << ", PCSX2 " << comparison.image_pcsx2_interval << '\n'
                      << "    upload counts:    live=" << comparison.live_image_transfers
                      << " pcsx2=" << comparison.pcsx2_image_transfers << '\n'
                      << "    matching prefix:  " << comparison.matched_image_prefix << '\n';
            for (const auto& line : comparison.image_matching_context) {
                std::cout << "      " << line << '\n';
            }
            if (comparison.image_status == GsSemanticComparisonStatus::Diverged) {
                std::cout << "    first live upload:  "
                          << comparison.live_image_mismatch.value_or("<end of interval>") << '\n'
                          << "    first PCSX2 upload: "
                          << comparison.pcsx2_image_mismatch.value_or("<end of interval>") << '\n';
            }
        } else {
            std::cout << "  IMAGE-only result:  unavailable (one side has no completed upload)\n";
        }
        std::cout << "  normalization:      path/cmdseq/KICK ignored; IMAGE payloads compared by FNV-1a hash\n";
        std::cout << "  note: the PCSX2 command stream was decoded only in memory; no payload was exported\n";
        return comparison.status == GsSemanticComparisonStatus::Diverged ? 1 : 0;
    }
    if (command == "pcsx2-render-savestate-scanout") {
        if (argc != 5) {
            throw std::invalid_argument(
                "pcsx2-render-savestate-scanout requires extracted GS.bin, extracted PCSX2 "
                "Internal Structures.dat, and an output PPM path");
        }
        const auto seed = takedown::platform::load_pcsx2_gs_scanout_seed(
            std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]});
        takedown::guest::GsLocalMemory local_memory;
        if (!local_memory.load_raw_backing(seed.local_memory)) {
            throw std::runtime_error("validated PCSX2 GS seed did not contain exactly 4 MiB of VRAM");
        }
        const auto display_state = display_state_from_crtc(seed.crtc);
        const auto plan = takedown::presentation::plan_gs_scanout(display_state);
        if (!plan.ready()) {
            throw std::runtime_error("saved GS CRTC is not renderable: " +
                                     std::string(takedown::presentation::to_string(plan.status)));
        }
        const auto frame = takedown::presentation::render_gs_scanout(local_memory, plan);
        if (!frame.rendered()) {
            throw std::runtime_error("saved GS framebuffer could not be rendered: " +
                                     std::string(takedown::presentation::to_string(frame.status)));
        }
        const std::filesystem::path output_path{argv[4]};
        write_rgba_ppm(output_path, frame.width, frame.height, frame.rgba);
        const auto content = summarize_rgb_content(frame.rgba);
        const auto pixel_count =
            static_cast<std::uint64_t>(frame.width) * static_cast<std::uint64_t>(frame.height);
        std::cout << "PCSX2 savestate frozen GS framebuffer\n"
                  << "  state version:       " << seed.state_version << '\n'
                  << "  GS video mode:       " << seed.video_mode << '\n'
                  << "  local memory:        " << seed.local_memory.size() << " bytes\n"
                  << "  display circuit:     " << plan.circuit << '\n'
                  << "  source:              " << plan.source_width << 'x' << plan.source_height
                  << " PSM " << plan.source_surface.pixel_storage_mode << '\n'
                  << "  CRTC geometry:       " << plan.crtc_width << 'x' << plan.crtc_height
                  << " repeat " << plan.horizontal_repeat << 'x' << plan.vertical_repeat << '\n'
                  << "  host frame:          " << frame.width << 'x' << frame.height
                  << (plan.progressive_frame_extraction ? " progressive extraction"
                                                        : " progressive")
                  << '\n'
                  << "  non-black RGB:       " << content.nonblack_pixels << '/' << pixel_count
                  << '\n'
                  << "  RGB fnv1a64:         " << hex64(content.fnv1a64) << '\n'
                  << "  output:              " << output_path.string() << '\n';
        if (content.nonblack_pixels == 0U) {
            std::cout << "  note: the CRTC-selected surface is byte-black in frozen GS local "
                         "memory; Screenshot.png is a separate host presentation and need not "
                         "match it\n";
        }
        return 0;
    }
    if (command == "replay-vif1") {
        return run_replay_vif1(argc, argv);
    }
    if (command == "replay-gif") {
        if (argc != 4) {
            throw std::invalid_argument("replay-gif requires a raw 32 MiB EE-RAM image and TADR");
        }
        const auto tadr = parse_u32(argv[3], "GIF TADR");
        const auto replay = takedown::runtime::replay_gif_from_raw_ee_ram(argv[2], tadr);
        const auto& fingerprint = replay.gs_fingerprint;
        std::cout << "GIF raw EE-RAM source-chain replay\n";
        std::cout << "  start:              tadr=" << hex32(tadr) << " path=PATH3\n"
                  << "  final DMAC:         chcr=" << hex32(replay.final_chcr)
                  << " events=" << replay.completion_events << '/'
                  << replay.queued_completion_events << '\n'
                  << "  GS decoded stream:  fnv1a64=" << hex64(fingerprint.fnv1a64)
                  << " tags=" << fingerprint.gif_tag_begins
                  << " writes=" << fingerprint.register_writes
                  << " images=" << fingerprint.image_transfers
                  << " image-bytes=" << fingerprint.image_payload_bytes
                  << " diagnostics=" << fingerprint.diagnostics
                  << " bounded-writes-omitted=" << fingerprint.capture_activity.omitted_register_writes
                  << '\n'
                  << "  GS signature scope: independent PATH3 decoded commands only; this is not a "
                     "chronological merge with PATH1/PATH2 and not a scanout\n";
        return replay.completion_events == 1U && replay.queued_completion_events == 1U &&
                       fingerprint.diagnostics == 0U
                   ? 0
                   : 3;
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(std::cout);
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    print_usage(std::cerr);
    return 2;
} catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
}
}


