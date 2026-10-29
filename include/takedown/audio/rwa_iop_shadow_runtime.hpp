#pragma once

#include "takedown/audio/guest_clocks.hpp"
#include "takedown/audio/iop_r3000_executor.hpp"
#include "takedown/guest/ee_kernel.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace takedown::audio {

// Non-authoritative retail RWA.IRX execution shadow. EeKernelHle remains the
// only guest-visible command completer; this class consumes its read-only
// observers and never sends the executor's outbound SIF records back to EE.
class RwaIopShadowRuntime {
  public:
    // PCSX2 race-start IOP RAM places the retail module here. Keeping the
    // shadow at the same address is significant: RWA bulk-stream destinations
    // legitimately occupy low IOP memory and must not overwrite module code.
    static constexpr std::uint32_t kRetailRwaLoadBase = 0x00065130U;

    struct Progress {
        IopR3000Executor::Stop stop{};
        std::size_t steps{};
        bool outbound_boundary{};
        // Every runnable RWA worker is asleep or blocked in a kernel wait.
        // A later SIF delivery can interrupt this idle state, so it is a
        // normal command boundary rather than an execution failure.
        bool quiescent_boundary{};
    };
    enum class FailureStage { NotLoaded, Upload, Delivery, Execution };
    struct Failure {
        FailureStage stage{};
        std::uint32_t opcode{};
        IopR3000Executor::SifDeliveryResult delivery{
            IopR3000Executor::SifDeliveryResult::Queued};
        IopR3000Executor::Stop stop{};
        std::size_t steps{};
    };

    [[nodiscard]] static constexpr const char* failure_stage_name(FailureStage stage) {
        switch (stage) {
        case FailureStage::NotLoaded: return "not-loaded";
        case FailureStage::Upload: return "upload";
        case FailureStage::Delivery: return "delivery";
        case FailureStage::Execution: return "execution";
        }
        return "unknown";
    }

    explicit RwaIopShadowRuntime(RwaSpu2VoiceState& voices) : voices_(voices) {}

    [[nodiscard]] bool load(const std::filesystem::path& irx_path) {
        std::ifstream input(irx_path, std::ios::binary);
        if (!input) return false;
        std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index)
            bytes[index] = static_cast<std::byte>(raw[index]);
        if (!executor_.load_irx(bytes, kRetailRwaLoadBase)) return false;
        executor_.attach_voice_state(&voices_);
        executor_.gpr()[4U] = 0U;
        executor_.gpr()[29U] = 0x1ff000U;
        executor_.gpr()[31U] = 0x200000U;
        auto progress = advance(1000000U);
        if (progress.stop.reason != IopR3000Executor::StopReason::MemoryFault ||
            progress.stop.pc != 0x200000U || !executor_.schedule_next_runnable()) return false;
        progress = advance(100000U);
        if (progress.stop.reason != IopR3000Executor::StopReason::None) return false;
        loaded_ = true;
        // Anchor the shared master clock: the boot above is a host-side
        // bring-up artifact (zero EE cycles elapse while it runs), and on
        // retail the IOP has merely been up since power-on when the game
        // starts. The post-boot tick count therefore becomes the IOP-side
        // zero of guest time, and the scheduler's idle fast-forward is
        // ceiling-clamped to it from here on so no window — command or
        // clock — can run IOP time ahead of the guest master clock (a run without
        // the ceiling measured a runaway 14.2e9-tick counter from
        // unclamped DelayThread fast-forwards).
        clock_base_ticks_ = executor_.clock_ticks();
        installed_ceiling_ = clock_base_ticks_;
        executor_.set_clock_ticks_ceiling(installed_ceiling_);
        return true;
    }

    // Retail storage module (docs/archive/AUDIO_HISTORY.md section 8, increment 8): loads
    // GTFSCDVD.IRX into the SAME live IOP image at its PCSX2-observed
    // retail base and runs its module start through the same
    // sentinel-return bootstrap as the primary module's boot. Retail
    // module-start (entry 0x224) creates the RPC server thread (priority 11)
    // and returns resident; the RPC server's retail code then creates the
    // stream/handle worker threads and semaphores during its init when the
    // modeled RPC handshake runs. Every executed instruction is retail; the
    // only modeled pieces are the load trigger and the later RPC delivery —
    // the established honesty framework.
    static constexpr std::uint32_t kRetailGtfsLoadBase = 0x00078c30U;
    static constexpr std::uint32_t kGtfsModuleEntryOffset = 0x224U;
    // GTFSCDVD module-start's created RPC server thread (entry, retail
    // disassembly at 0x298).
    static constexpr std::uint32_t kGtfsRpcServerEntryOffset = 0x298U;
    // The storage vtable GTFSCDVD's RPC cmd-1 init returns (its BSS 0x9300),
    // and the same pointer retail publishes into RWA's BSS 0xe0f0 through
    // RWA command 0x30's packet word +0x24 (verified against the live
    // savestate: RWA [base+0xe0f0] == 0x78c30+0x9300).
    static constexpr std::uint32_t kGtfsStorageVtable =
        kRetailGtfsLoadBase + 0x9300U;

    [[nodiscard]] bool load_extension(const std::filesystem::path& irx_path) {
        if (!loaded_ || extension_loaded_) return false;
        std::ifstream input(irx_path, std::ios::binary);
        if (!input) return false;
        std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index)
            bytes[index] = static_cast<std::byte>(raw[index]);
        if (!executor_.load_extension_irx(bytes, kRetailGtfsLoadBase)) return false;
        if (executor_.run_bootstrap_call(
                executor_.extension_base() + kGtfsModuleEntryOffset) !=
            IopR3000Executor::SifDeliveryResult::Queued)
            return false;
        // Bootstrap convention (run_bootstrap_call): step until the
        // interrupt context clears — the sentinel return restores the
        // interrupted post-boot context in step().
        for (std::size_t steps = 0; steps < 1000000U; ++steps) {
            const auto stop = executor_.step();
            if (stop.reason != IopR3000Executor::StopReason::None) return false;
            if (!executor_.interrupt_active()) break;
        }
        if (executor_.interrupt_active()) return false;
        // The bootstrap returns only after module start finished (the
        // sentinel fault restores the interrupted context); the retail
        // CreateThread for the RPC server must have executed.
        bool rpc_thread_created = false;
        for (const auto& thread : executor_.threads()) {
            if (thread.entry ==
                executor_.extension_base() + kGtfsRpcServerEntryOffset)
                rpc_thread_created = true;
        }
        if (!rpc_thread_created) return false;
        // Retail sysmem partition base (see the executor comment): every
        // later allocation — GTFSCDVD cmd-1's arrays, RWA's pool nodes, the
        // game's bank buffers routed through executed retail — lands in
        // retail's high region instead of colliding with the game's own
        // low-memory buffers.
        executor_.raise_sysmem_allocation_base(0x00174100U);
        extension_loaded_ = true;
        // Increment-10 boundary watch: the 0xb32c request-queue block
        // (absolute 0x7045c, 9 words) held instruction words at fault time.
        executor_.watch_writes(
            executor_.module_base() + 0xb32cU, 9U);
        // Increment-13 ring watch: the RWA request-ring indices
        // (module 0xdaa8/0xdaac) hold bank bytes at the wild fetch; the
        // watched writes name every IOP-CPU store into them (DMA-mirror
        // writes bypass the watch by construction, so a watched write with
        // a module PC proves code, not the upload mirror, moved the ring).
        executor_.watch_writes(
            executor_.module_base() + 0xdaa8U, 2U);
        return true;
    }

    [[nodiscard]] bool extension_loaded() const { return extension_loaded_; }


    void bind(takedown::guest::EeKernelHle& kernel, std::size_t command_budget = 500000U) {
        command_budget_ = command_budget;
        // TAKEDOWN_RWA_SHADOW_DIAG is passive per-command logging only; every
        // accuracy-affecting scheduling knob (the former
        // TAKEDOWN_RWA_MIXER_STEPS / TAKEDOWN_RWA_IOP_CLOCK_STEPS) was
        // removed with the master-clock windowing above.
        if (const char* diag = std::getenv("TAKEDOWN_RWA_SHADOW_DIAG"))
            diag_enabled_.store(std::string_view{diag} == "1", std::memory_order_relaxed);
        kernel.set_rwa_execution_observers(
            [this](std::span<const std::byte> packet) { observe_packet(packet); },
            [this](std::uint32_t destination, std::span<const std::byte> bytes) {
                observe_upload(destination, bytes);
            },
            [this](std::uint32_t opcode) {
                return take_response(expected_response_opcode(opcode).value_or(opcode));
            });
    }

    // Guest master-clock anchor for command windows. The owner (the app,
    // which owns emulated_ee_cycles) supplies the current guest cycle; every
    // inbound command/upload window first advances the shadow to exactly
    // that guest time, so the window's execution budget and scheduler
    // ceiling are the real ones for the packet's guest cycle — commands
    // start when the guest sent them, not at a stale boundary. Deterministic
    // because the observer fires at a fixed guest cycle for a fixed event
    // stream.
    void set_guest_cycle_provider(std::function<std::uint64_t()> provider) {
        guest_cycle_provider_ = std::move(provider);
    }

    void observe_upload(std::uint32_t destination, std::span<const std::byte> bytes) {
        if (diag_enabled_.load(std::memory_order_relaxed)) {
            fprintf(stderr, "[RWA-SHADOW] upload dst=0x%06x size=%zu\n",
                    destination, bytes.size());
        }
        if (loaded_) {
            sync_master_clock_to_guest();
            retry_held_delivery();
            service_pending_queue_create();
        }
        // Small packet extra-DMA (the 0x14 snapshot, 0x32 stream objects,
        // 0x46 descriptor pairs): retail delivers these synchronously with
        // the packet, so arrival-time mirroring is the retail ordering.
        if (!loaded_ || !executor_.mirror_iop_upload(destination, bytes)) {
            record_failure({.stage = loaded_ ? FailureStage::Upload
                                             : FailureStage::NotLoaded,
                            .opcode = 0U});
        }
    }

    void observe_packet(std::span<const std::byte> packet) {
        const auto outbound_before = executor_.outbound_sif_commands().size();
        const auto opcode = packet.size() >= 0x14U
                                ? std::to_integer<std::uint8_t>(packet[0x10U]) : 0U;
        // TEMPORARY DIAGNOSTIC (remove before commit)
        if (diag_enabled_.load(std::memory_order_relaxed)) {
            const auto w = [&](std::uint32_t a) {
                std::uint32_t v{}; static_cast<void>(executor_.inspect_word(a, v)); return v;
            };
            std::uint32_t server_pc = 0U, server_flags = 0U;
            for (const auto& t : executor_.threads()) {
                if (t.entry ==
                    executor_.extension_base() + kGtfsRpcServerEntryOffset) {
                    server_pc = t.pc;
                    server_flags = (t.runnable ? 1U : 0U) |
                                   (t.waiting_rpc_queue != 0U ? 2U : 0U) |
                                   (t.sleeping ? 4U : 0U);
                }
            }
            fprintf(stderr, "[RWA-SHADOW] packet op=0x%02x size=%zu loaded=%d outbound_before=%zu "
                            "ext=%d cmd1=%d rpc_active=%d vt=0x%x vtbl0=0x%x ring=%u/%u poolq=%u "
                            "srv_pc=0x%x srv_f=%u disc=%d\n",
                    opcode, packet.size(), loaded_ ? 1 : 0, outbound_before,
                    extension_loaded_ ? 1 : 0, cmd1_delivered_ ? 1 : 0,
                    executor_.rpc_dispatch_active() ? 1 : 0,
                    w(executor_.module_base() + 0xe0f0U),
                    executor_.extension_base() != 0U
                        ? w(executor_.extension_base() + 0x9300U)
                        : 0U,
                    w(executor_.module_base() + 0xdaacU),
                    w(executor_.module_base() + 0xdaa8U),
                    w(executor_.module_base() + 0xb338U),
                    server_pc, server_flags,
                    executor_.has_disc_sector_source() ? 1 : 0);
        }
        if (!loaded_) {
            record_failure({.stage = FailureStage::NotLoaded, .opcode = opcode});
            return;
        }
        // Bring the shadow to this packet's exact guest time before the
        // command window runs (see set_guest_cycle_provider).
        sync_master_clock_to_guest();
        // Retail sifcmd queues commands while a previous command's interrupt
        // handler is still running; a Busy delivery is exactly that state,
        // not a lost command. Retry the held packet here (one bounded
        // attempt per inbound command) so the EE->IOP RWA command stream
        // cannot silently drop on scheduler overlap.
        retry_held_delivery();
        // Without the executed retail GTFSCDVD extension, opcode 0x32
        // delegates through the storage vtable owned by another IOP module
        // this executor has not loaded; EeKernelHle is the authoritative
        // byte-exact implementation of that request there, and the open's
        // remaining retail effect — the dispatcher ops-table call that
        // (re)creates the module's global request queue (module 0xa308) —
        // runs here as retail code via the bootstrap call. docs/AUDIO.md
        // 7.8. With the extension loaded, the full 0x32 path executes as
        // retail code (deliver_sif_command below; the retail ring +
        // dispatcher + pool worker + storage vtable chain) and only the
        // per-open queue-creation trigger stays modeled.
        if (opcode == 0x32U && packet.size() == 0x30U &&
            std::to_integer<std::uint8_t>(packet[0]) == 0x30U) {
            // State-4 retail open independently calls module 0xa308 for each
            // stream. Preserve that proven operation whether the storage
            // extension or the byte-exact native storage HLE owns the rest of
            // the open; this is not repair for an opcode-0x05 memory write.
            run_queue_create();
            if (!extension_loaded_) {
                ++stream_commands_skipped_;
                last_progress_ = {};
                return;
            }
        }
        // Retail "STG" RPC cmd-1 init: the HLE completes the game's RPC
        // guest-visibly, but the executed GTFSCDVD server must run its own
        // retail cmd-1 (disk poll, ISO9660 table walk over the real disc,
        // storage vtable build) before any storage call can work. The game's
        // op-0x30 publish (carrying the exact vtable pointer the HLE
        // replied) is the modeled delivery trigger — the same point retail's
        // server would already have served the request. The 0x30-byte
        // request is the exact retail payload the HLE validates ({1, 3,
        // 0x20, 0x10} + 28 zero bytes = empty stored path; the live
        // savestate shows 0x9240 all zeros). Delivered once; the dispatch
        // completes across later master-clock windows.
        if (extension_loaded_ && opcode == 0x30U && !cmd1_delivered_) {
            cmd1_delivered_ = true;
            std::array<std::byte, 0x30U> request{};
            const auto put = [&](std::size_t at, std::uint32_t value) {
                for (std::size_t byte = 0; byte < 4U; ++byte)
                    request[at + byte] =
                        static_cast<std::byte>(value >> (byte * 8U));
            };
            put(0x00U, 1U);
            put(0x04U, 3U);
            put(0x08U, 0x20U);
            put(0x0cU, 0x10U);
            static_cast<void>(executor_.write_iop_bytes_for_delivery(
                executor_.rpc_request_buffer(), request));
            if (!executor_.deliver_rpc_call(1U, executor_.rpc_request_buffer(),
                                            0x30U)) {
                record_failure({.stage = FailureStage::Execution,
                                .opcode = 0x30U});
            }
        }
        const auto delivery = executor_.deliver_sif_command(packet);
        if (delivery == IopR3000Executor::SifDeliveryResult::Busy) {
            // Retail sifcmd queues the packet behind the still-running
            // interrupt handler. Hold it for a retry at the next command
            // boundary rather than dropping it (which starves the retail
            // streaming pool and cuts off guest music streaming).
            held_packet_.emplace(packet.begin(), packet.end());
            return;
        }
        if (delivery != IopR3000Executor::SifDeliveryResult::Queued) {
            record_failure({.stage = FailureStage::Delivery,
                            .opcode = opcode,
                            .delivery = delivery});
            return;
        }
        last_progress_ = advance_command(command_budget_, outbound_before,
                                         expected_response_opcode(opcode));
        if (last_progress_.quiescent_boundary) {
            ++quiescent_commands_;
        } else if (last_progress_.stop.reason != IopR3000Executor::StopReason::None) {
            record_execution_failure(opcode, last_progress_);
        }
        if (diag_enabled_.load(std::memory_order_relaxed)) {
            fprintf(stderr, "[RWA-SHADOW] op=0x%02x cmd-steps=%zu boundary=%d reason=%u "
                            "spc=%u sarg=%u thread=%u ra=%u a0=%u\n",
                    opcode, last_progress_.steps,
                    last_progress_.outbound_boundary ? 1 : 0,
                    static_cast<unsigned>(last_progress_.stop.reason),
                    last_progress_.stop.pc, last_progress_.stop.argument,
                    executor_.debug_current_thread(), executor_.debug_current_ra(),
                    executor_.debug_current_a0());
                        // Unsupported-stop triage (TAKEDOWN_RWA_SHADOW_DIAG=1): dump
            // sp, the first stack words, the executed-PC ring, and the loaded
            // import region, so the jalr chain that reached a strict import
            // boundary is visible without a rebuild.
            if (last_progress_.stop.reason ==
                IopR3000Executor::StopReason::UnsupportedInstruction) {
                const auto sp = executor_.debug_current_sp();
                fprintf(stderr, "[RWA-SHADOW]   stack sp=%u:", sp);
                for (std::uint32_t at = 0U; at < 16U; ++at) {
                    std::uint32_t word{};
                    if (!executor_.inspect_word(sp + at * 4U, word)) break;
                    fprintf(stderr, " %u:%u", at, word);
                }
                fprintf(stderr, "\n[RWA-SHADOW]   pcs:");
                for (const auto pc : executor_.debug_pc_history())
                    fprintf(stderr, " %u", pc);
                {
                    std::uint32_t faulting{};
                    if (executor_.inspect_word(last_progress_.stop.pc, faulting))
                        fprintf(stderr, "\n[RWA-SHADOW]   fault word @%u = %08x",
                                last_progress_.stop.pc, faulting);
                    std::uint32_t base{};
                    if (executor_.inspect_word(executor_.module_base(), base))
                        fprintf(stderr, "  module-base-word=%08x", base);
                }
                fprintf(stderr, "\n[RWA-SHADOW]   loaded import region:");
                for (std::uint32_t off = 0x8b90U; off < 0x8d00U; off += 4U) {
                    std::uint32_t word{};
                    if (!executor_.inspect_word(executor_.module_base() + off, word)) break;
                    fprintf(stderr, " %x=%u", off, word);
                }
                fprintf(stderr, "\n[RWA-SHADOW]   regs:");
                for (std::size_t index = 2U; index < 32U; ++index) {
                    std::uint32_t word{};
                    if (!executor_.inspect_word(
                            0xDEADBEEFU, word)) { /* placeholder */ }
                    fprintf(stderr, " %zu=%u", index,
                            executor_.debug_current_gpr(index));
                }
                fprintf(stderr, "\n");
            }
        }
    }

    [[nodiscard]] std::uint64_t voice_stats_kon() const { return voices_.stats().kon; }

    [[nodiscard]] Progress advance(std::size_t budget) {
        Progress progress{};
        for (; progress.steps < budget; ++progress.steps) {
            progress.stop = executor_.step();
            if (progress.stop.reason != IopR3000Executor::StopReason::None) break;
        }
        return progress;
    }

    // Runtime-window stepping (post-boot command/mixer/clock windows). A
    // parked executor at the sentinel return address with no owning
    // interrupt context is a completed sifcmd handler whose return address
    // the scheduler persisted into a worker's saved frame; retail's
    // scheduler simply dispatches the next runnable thread here, and an
    // empty runnable set is the normal quiescent boundary, not a fault.
    // load()'s boot trampoline keeps the raw step() semantics (its stop IS
    // the sentinel fault), so the resolution lives in this runtime wrapper.
    [[nodiscard]] IopR3000Executor::Stop step_resolved() {
        auto stop = executor_.step();
        if (stop.reason == IopR3000Executor::StopReason::MemoryFault &&
            stop.pc == executor_.sentinel_return_address() &&
            !executor_.interrupt_active()) {
            if (!executor_.dispatch_next_runnable())
                return {IopR3000Executor::StopReason::Halted, stop.pc, 0U, 0U};
            stop = executor_.step();
        }
        // Retail-crash semantics: an uncommitted memory fault retires the
        // faulting context (thread dormant / interrupt dropped) while the
        // machine continues. The evidence lives in `stop`, which the caller
        // records; the faulted context never re-executes.
        if (stop.reason == IopR3000Executor::StopReason::MemoryFault) {
            if (diag_enabled_.load(std::memory_order_relaxed)) {
                const auto& g = executor_.gpr();
                std::fprintf(stderr,
                             "[FAULT-DIAG] pc=0x%x word=0x%x a0=%x a1=%x a2=%x "
                             "a3=%x s0=%x s1=%x sp=%x ra=%x\n",
                             stop.pc, stop.instruction, g[4], g[5], g[6], g[7],
                             g[16], g[17], g[29], g[31]);
                for (std::size_t ti = 0; ti < executor_.threads().size();
                     ++ti) {
                    const auto& t = executor_.threads()[ti];
                    std::fprintf(stderr,
                                 "[FAULT-DIAG]   thread id=%zu entry=0x%x "
                                 "runnable=%d sleep=%d wsema=%u wkup=%d wqueue=%u\n",
                                 ti + 2U, t.entry, t.runnable ? 1 : 0,
                                 t.sleeping ? 1 : 0, t.waiting_semaphore,
                                 t.waiting_wakeup ? 1 : 0,
                                 t.waiting_rpc_queue);
                }
                for (std::size_t sema = 0; sema < executor_.semaphores().size();
                     ++sema) {
                    std::fprintf(stderr, "[FAULT-DIAG]   sema[%zu] count=%u\n",
                                 sema, executor_.semaphores()[sema].count);
                }
                for (const auto& w : executor_.watched_writes())
                    std::fprintf(stderr, "[WATCH] pc=0x%x value=%08x off=%u\n",
                                 w.pc, w.value, w.offset);
            }
            executor_.retire_faulted_context();
        }
        return stop;
    }

    [[nodiscard]] Progress advance_runtime(std::size_t budget) {
        Progress progress{};
        for (; progress.steps < budget; ++progress.steps) {
            progress.stop = step_resolved();
            if (progress.stop.reason != IopR3000Executor::StopReason::None) break;
        }
        return progress;
    }

    // Retail second-CPU modeling on the shared guest master clock
    // (docs/archive/AUDIO_HISTORY.md section 8, increment 7). The EE cycle counter is the
    // master; the executor's tick domain is the 36.864 MHz IOP SysClock,
    // which is exactly EE/8 (the retail thbase 1.2 USec2SysClock constant),
    // so the window target is `clock_base_ticks_ + ee_cycle / 8` with no
    // fractional drift (the base anchors guest-time zero after the
    // host-side boot). Command windows run under the same ceiling: every
    // inbound command/upload first syncs the shadow to the packet's exact
    // guest cycle, then executes within the remaining budget. A retail
    // handler whose protocol waits on field-paced workers (measured: a
    // 16,700 us DelayThread poll loop) therefore PARKS instead of
    // fabricating fields of IOP time; its remaining work runs in later
    // master-clock windows as guest time advances, and its outbound
    // response appears then (asynchronous completion — the honest retail
    // pacing). A window at guest boundary `ee_cycle`:
    //   1. bounds the scheduler's idle fast-forward to that target (worker
    //      wakeups beyond it stay pending — IOP time may not outrun guest
    //      time),
    //   2. executes runnable workers for at most the remaining
    //      target - clock_ticks() instructions (one tick per instruction),
    //   3. on quiescence (all workers blocked/asleep — the retail CPU HALTs)
    //      passes idle time to the next sleeper deadline or the target, so
    //      no budget is ever discarded and DelayThread pacing tracks guest
    //      time exactly, and
    //   4. leaves clock_ticks() at exactly the target when it returns
    //      without a fault.
    // A modeled execution fault is recorded like a command-window failure
    // and disables further clock windows.
    void advance_master_clock(std::uint64_t ee_cycle) {
        if (!loaded_ || !master_clock_enabled_) return;
        // Guest-time anchor: the IOP-side zero of the master clock is the
        // post-boot tick count (see load()); guest EE cycles advance the
        // target at the exact EE/8 SysClock ratio.
        const auto target =
            clock_base_ticks_ + ee_cycle / guest_clocks::kEeCyclesPerIopSysTick;
        if (target <= executor_.clock_ticks()) return;
        installed_ceiling_ = target;
        executor_.set_clock_ticks_ceiling(target);
        ++clock_windows_;
        while (executor_.clock_ticks() < target) {
            const auto remaining =
                target - executor_.clock_ticks();
            const auto progress =
                advance_runtime(static_cast<std::size_t>(remaining));
            if (progress.stop.reason == IopR3000Executor::StopReason::None) break;
            if (progress.stop.reason == IopR3000Executor::StopReason::Halted) {
                // Idle: advance modeled time to the next pending wakeup that
                // is due within this window, dispatch whoever woke, and keep
                // going; when nothing is due before the target, idle through
                // to the target and finish.
                const auto next_wake = executor_.earliest_sleep_wake_tick();
                const auto jump_to = std::min(next_wake, target);
                if (jump_to > executor_.clock_ticks() &&
                    executor_.advance_system_clock_to(jump_to) &&
                    executor_.schedule_next_runnable()) {
                    continue;
                }
                static_cast<void>(executor_.advance_system_clock_to(target));
                break;
            }
            if (diag_enabled_.load(std::memory_order_relaxed)) {
                fprintf(stderr, "[RWA-SHADOW] clock-fault steps=%zu reason=%u pc=%u\n",
                        progress.steps, static_cast<unsigned>(progress.stop.reason),
                        progress.stop.pc);
            }
            if (progress.stop.reason == IopR3000Executor::StopReason::MemoryFault) {
                // The modeled state has diverged from retail (retail never
                // faults). Keep the executor's exact fault evidence for the
                // report, but stop driving clock windows through corrupt
                // state: resuming past an uncommitted memory fault runs wild
                // (observed: a second fault with pc=0x8c430000). Command
                // windows remain authoritative and re-record nothing
                // (first_failure wins).
                master_clock_enabled_ = false;
            }
            record_execution_failure(0U, progress);
            break;
        }
    }

    // Retry a packet that was held because the retail sifcmd interrupt
    // handler was still running (Busy delivery). One bounded attempt per
    // inbound command keeps the EE->IOP RWA command stream loss-free without
    // unbounded work per call.
    void retry_held_delivery() {
        if (!held_packet_ || !loaded_) return;
        std::vector<std::byte> held = std::move(*held_packet_);
        held_packet_.reset();
        const auto opcode = held.size() >= 0x14U
                                ? std::to_integer<std::uint8_t>(held[0x10U]) : 0U;
        if (executor_.deliver_sif_command(held) !=
            IopR3000Executor::SifDeliveryResult::Queued) {
            held_packet_ = std::move(held);
            return;
        }
        const auto outbound_before = executor_.outbound_sif_commands().size();
        last_progress_ = advance_command(command_budget_, outbound_before,
                                         expected_response_opcode(opcode));
        if (last_progress_.quiescent_boundary) {
            ++quiescent_commands_;
        } else if (last_progress_.stop.reason != IopR3000Executor::StopReason::None) {
            record_execution_failure(opcode, last_progress_);
        }
    }

    // A retail command is complete at this shadow boundary once its worker has
    // submitted an outbound SIF packet. Continuing to interpret until the
    // arbitrary safety budget after that point only runs sleeping/background
    // RWA threads (the scheduler deliberately fast-forwards their delays) and
    // charged up to 500,000 IOP instructions to every EE audio packet. Keep a
    // short retirement tail so the sending worker can leave sceSifSendCmd and
    // save a coherent continuation, then return control to the EE runtime.
    [[nodiscard]] Progress advance_command(
        std::size_t budget, std::size_t outbound_before,
        std::optional<std::uint32_t> expected_response = std::nullopt) {
        constexpr std::size_t kPostOutboundTail = 256U;
        Progress progress{};
        std::size_t tail_remaining{};
        std::size_t outbound_checked = outbound_before;
        for (; progress.steps < budget; ++progress.steps) {
            progress.stop = step_resolved();
            if (progress.stop.reason != IopR3000Executor::StopReason::None) break;
            bool matched_outbound = false;
            const auto& outbound = executor_.outbound_sif_commands();
            for (; !progress.outbound_boundary && outbound_checked < outbound.size();
                 ++outbound_checked) {
                const auto opcode = packet_opcode(outbound[outbound_checked].packet);
                matched_outbound = !expected_response || opcode == expected_response;
                if (matched_outbound) {
                    ++outbound_checked;
                    break;
                }
            }
            if (!progress.outbound_boundary && matched_outbound) {
                progress.outbound_boundary = true;
                tail_remaining = kPostOutboundTail;
            } else if (progress.outbound_boundary) {
                if (tail_remaining == 0U) break;
                --tail_remaining;
            }
        }
        progress.quiescent_boundary =
            progress.stop.reason == IopR3000Executor::StopReason::Halted;
        return progress;
    }

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] const Progress& last_progress() const { return last_progress_; }
    [[nodiscard]] const std::optional<Failure>& first_failure() const {
        return first_failure_;
    }
    [[nodiscard]] IopR3000Executor& executor() { return executor_; }
    [[nodiscard]] const IopR3000Executor& executor() const { return executor_; }
    [[nodiscard]] std::uint64_t stream_commands_skipped() const {
        return stream_commands_skipped_;
    }
    [[nodiscard]] std::uint64_t queue_creates() const { return queue_creates_; }
    [[nodiscard]] std::uint64_t queue_create_failures() const {
        return queue_create_failures_;
    }
    [[nodiscard]] std::uint64_t quiescent_commands() const {
        return quiescent_commands_;
    }
    [[nodiscard]] std::uint64_t clock_windows() const {
        return clock_windows_;
    }
    // The IOP-side zero of guest master time (post-boot tick count).
    [[nodiscard]] std::uint64_t master_clock_base_ticks() const {
        return clock_base_ticks_;
    }

    [[nodiscard]] std::optional<takedown::guest::EeRwaShadowResponse>
    take_response(std::uint32_t expected_opcode) {
        const auto& commands = executor_.outbound_sif_commands();
        for (; drained_outbound_ < commands.size(); ++drained_outbound_) {
            const auto& command = commands[drained_outbound_];
            const auto opcode = packet_opcode(command.packet);
            // TEMPORARY DIAGNOSTIC: reveal the shadow's IOP->EE traffic.
            if (diag_enabled_.load(std::memory_order_relaxed)) {
                fprintf(stderr, "[RWA-SHADOW] outbound op=0x%02x extra=%zu\n",
                        opcode, command.extra_data.size());
            }
            if (command.extra_size == command.extra_data.size()) {
                pending_responses_[opcode].push_back(takedown::guest::EeRwaShadowResponse{
                    .opcode = opcode, .extra_destination = command.extra_destination,
                    .extra_data = command.extra_data});
            }
        }
        // The executor's outbound vector is a transfer queue, not an unbounded
        // command history. Retire its scanned records only after copying every
        // response into the opcode-keyed queues above. A later response can be
        // produced while the HLE happens to ask for another opcode; retaining
        // it here prevents asynchronous op-0x14 completions from being lost.
        if (drained_outbound_ != 0U) {
            executor_.discard_outbound_sif_commands(drained_outbound_);
            drained_outbound_ = 0U;
        }
        const auto pending = pending_responses_.find(expected_opcode);
        if (pending == pending_responses_.end() || pending->second.empty()) {
            return std::nullopt;
        }
        auto response = std::move(pending->second.front());
        pending->second.pop_front();
        if (pending->second.empty()) pending_responses_.erase(pending);
        return response;
    }

  private:
    // Bring the shadow to the current guest time (command-window entry).
    void sync_master_clock_to_guest() {
        if (guest_cycle_provider_) advance_master_clock(guest_cycle_provider_());
    }

    // Retail opcode-0x32 open path, queue-creation step (module 0xa308,
    // invoked through the dispatcher ops pointer at 0xe0f0 slot +8). Retail
    // executes this on every stream open. The module 0xa308 entry ignores its
    // arguments
    // (verified by disassembly: no read of a0..a3 before the internal
    // allocator call), so the modeled zero-argument invocation is
    // behavior-identical to the retail call site.
    void run_queue_create() {
        if (queue_create_pending_) return;
        const auto delivery = executor_.run_bootstrap_call(
            executor_.module_base() + 0xa308U);
        if (delivery == IopR3000Executor::SifDeliveryResult::Busy) {
            // A previous command's interrupt handler is still active; retry
            // at the next command boundary, like a held sifcmd packet.
            queue_create_pending_ = true;
            return;
        }
        if (delivery != IopR3000Executor::SifDeliveryResult::Queued) {
            ++queue_create_failures_;
            record_failure({.stage = FailureStage::Execution, .opcode = 0x32U});
            return;
        }
        ++queue_creates_;
        for (std::size_t steps = 0; steps < 100000U; ++steps) {
            const auto stop = executor_.step();
            if (stop.reason != IopR3000Executor::StopReason::None) {
                ++queue_create_failures_;
                record_failure({.stage = FailureStage::Execution,
                                .opcode = 0x32U, .stop = stop, .steps = steps});
                return;
            }
            if (!executor_.interrupt_active()) {
                if (diag_enabled_.load(std::memory_order_relaxed))
                    fprintf(stderr, "[RWA-SHADOW] queue-create steps=%zu\n",
                            steps + 1U);
                return;
            }
        }
        ++queue_create_failures_;
        record_failure({.stage = FailureStage::Execution, .opcode = 0x32U});
    }

    void service_pending_queue_create() {
        if (!queue_create_pending_ || !loaded_) return;
        queue_create_pending_ = false;
        run_queue_create();
    }

    void record_failure(Failure failure) {
        failed_ = true;
        if (!first_failure_) first_failure_ = failure;
    }

    void record_execution_failure(std::uint32_t opcode, const Progress& progress) {
        record_failure({.stage = FailureStage::Execution,
                        .opcode = opcode,
                        .stop = progress.stop,
                        .steps = progress.steps});
    }

    static std::uint32_t packet_opcode(std::span<const std::byte> packet) {
        if (packet.size() < 0x14U) return 0U;
        return std::to_integer<std::uint32_t>(packet[0x10U]) |
               (std::to_integer<std::uint32_t>(packet[0x11U]) << 8U) |
               (std::to_integer<std::uint32_t>(packet[0x12U]) << 16U) |
               (std::to_integer<std::uint32_t>(packet[0x13U]) << 24U);
    }

    static std::optional<std::uint32_t> expected_response_opcode(
        std::uint32_t inbound) {
        switch (inbound) {
        case 0x05U: return 0x0dU;
        case 0x11U: return 0x1aU;
        case 0x13U: return 0x1bU;
        case 0x14U: return 0x14U;
        case 0x15U: return 0x1cU;
        case 0x17U: return 0x1dU;
        case 0x30U: return 0x31U;
        case 0x32U: return 0x33U;
        case 0x46U: return 0x47U;
        case 0x4eU: return 0x4fU;
        default: return std::nullopt;
        }
    }

    RwaSpu2VoiceState& voices_;
    IopR3000Executor executor_{};
    Progress last_progress_{};
    std::size_t command_budget_{500000U};
    bool loaded_{};
    bool failed_{};
    std::optional<Failure> first_failure_{};
    std::optional<std::vector<std::byte>> held_packet_{};
    std::size_t drained_outbound_{};
    std::unordered_map<
        std::uint32_t,
        std::deque<takedown::guest::EeRwaShadowResponse>> pending_responses_{};
    std::uint64_t stream_commands_skipped_{};
    std::uint64_t quiescent_commands_{};
    std::uint64_t queue_creates_{};
    std::uint64_t queue_create_failures_{};
    bool queue_create_pending_{};
    // Passive per-command logging switch (TAKEDOWN_RWA_SHADOW_DIAG=1).
    std::atomic<bool> diag_enabled_{false};
    // Master-clock windowing state: enabled until a clock-window execution
    // fault disables further windows (command windows stay authoritative).
    bool master_clock_enabled_{true};
    std::uint64_t clock_windows_{};
    std::uint64_t clock_base_ticks_{};
    // The master clock's currently installed scheduler ceiling.
    std::uint64_t installed_ceiling_{~std::uint64_t{0U}};
    std::function<std::uint64_t()> guest_cycle_provider_{};
    bool extension_loaded_{};
    bool cmd1_delivered_{};
};

} // namespace takedown::audio
