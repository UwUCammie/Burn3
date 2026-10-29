#pragma once

#include "takedown/audio/rwa_spu2_voice.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <cstdio>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takedown::audio {

// Strict, deliberately small R3000A execution substrate. Unsupported
// instructions stop with their exact PC/word; nothing is treated as a NOP.
class IopR3000Executor {
  public:
    enum class StopReason { None, Halted, UnsupportedInstruction, MemoryFault };
    enum class SifDeliveryResult { Queued, NotInitialized, InvalidPacket, MissingHandler, Busy };
    struct Stop { StopReason reason{}; std::uint32_t pc{}; std::uint32_t instruction{};
                  // First kernel-call argument when the stop is a stub
                  // rejection; otherwise 0. Diagnostic aid for modeling gaps.
                  std::uint32_t argument{}; };
    struct LoadError { std::uint32_t offset{}; std::uint32_t type{}; };
    struct Semaphore { std::uint32_t attributes{}; std::uint32_t option{};
                       std::uint32_t count{}; std::uint32_t maximum{};
                       // False once DeleteSema destroyed the kernel object;
                       // the slot is retained so ids stay stable.
                       bool live{true}; };
    struct Thread { std::uint32_t attributes{}; std::uint32_t option{};
                    std::uint32_t entry{}; std::uint32_t stack_base{};
                    std::uint32_t stack_size{}; std::uint32_t priority{};
                    std::uint32_t argument{}; bool runnable{};
                    std::array<std::uint32_t, 32> registers{}; std::uint32_t pc{};
                    bool sleeping{}; std::uint64_t wake_tick{};
                    std::uint32_t waiting_semaphore{}; bool waiting_wakeup{};
                    std::uint32_t waiting_event_flag{}; std::uint32_t waiting_event_pattern{};
                    std::uint32_t waiting_event_mode{}; std::uint32_t waiting_event_out{};
                    // The thread is parked inside the kernel's RpcMain
                    // service loop on this sifcmd RPC queue (0 = not
                    // waiting) — the retail blocking server loop.
                    std::uint32_t waiting_rpc_queue{};
                    // IOP thread states this model tracks: DORMANT after
                    // ExitThread/TerminateThread, DEAD after DeleteThread
                    // (the TCB slot is retained so live thread ids stay
                    // stable, exactly like the retail kernel's id table).
                    bool dormant{}; bool dead{}; };
    struct SifCommandHandler { std::uint32_t command{}; std::uint32_t function{};
                               std::uint32_t argument{}; };
    struct Allocation { std::uint32_t address{}; std::uint32_t size{}; bool live{}; };
    struct EventFlag { std::uint32_t attributes{}; std::uint32_t option{};
                       std::uint32_t bits{}; bool live{}; };
    struct CoreAttribute { std::uint16_t entry{}; std::uint16_t value{}; };
    struct AddressRegister { std::uint16_t entry{}; std::uint32_t value{}; };
    struct SwitchRegister { std::uint16_t entry{}; std::uint32_t value{}; };
    struct VoiceTransfer { std::uint32_t channel{}, mode{}, iop_address{},
                           spu_address{}, size{}; };
    struct OutboundSifCommand { std::int32_t id{}; std::uint32_t command{}, packet_address{},
                                packet_size{}, extra_source{}, extra_destination{}, extra_size{};
                                std::vector<std::byte> packet{}, extra_data{}; };
    struct VoiceSnapshotRecord { std::uint8_t voice{}; std::array<std::uint32_t, 8> words{}; };

    bool load_irx(std::span<const std::byte> elf, std::uint32_t load_base = 0U) {
        load_error_ = {};
        module_base_ = 0U;
        module_image_end_ = 0U;
        module_file_end_ = 0U;
        module_file_end_candidate_ = 0U;
        module_upload_skips_ = 0U;
        first_module_upload_skip_ = {};
        memory_.assign(2U * 1024U * 1024U, 0U);
        module_base_ = load_base;
        next_sysmem_address_ = 0U;
        IrxImageResult image{};
        if (!load_irx_image(elf, load_base, image)) return false;
        module_image_end_ = image.image_end;
        module_file_end_ = image.file_end;
        module_text_end_ = image.text_end;
        const auto entry64 = static_cast<std::uint64_t>(read_elf_u32(elf, 24U)) + load_base;
        if (entry64 >= memory_.size()) return false;
        pc_ = static_cast<std::uint32_t>(entry64);
        gpr_.fill(0U); hi_ = 0U; lo_ = 0U; pending_target_ = 0U; in_delay_slot_ = false;
        enabled_irqs_ = 0U; cpu_interrupts_enabled_ = true; clock_ticks_ = 0U;
        clock_ticks_ceiling_ = ~std::uint64_t{0U};
        halted_ = false;
        sifcmd_initialized_ = false; spu2_initialized_ = false;
        semaphores_.clear(); threads_.clear();
        sif_command_buffer_ = 0U; sif_command_buffer_count_ = 0U; sif_handlers_.clear();
        event_flags_.clear();
        transfer_handlers_.fill(0U); transfer_handler_arguments_.fill(0U);
        spu2_interrupt_handler_ = 0U; spu2_interrupt_argument_ = 0U;
        core_attributes_.clear();
        parameter_registers_.clear(); address_registers_.clear(); switch_registers_.clear();
        spu2_ram_.fill(0U); voice_transfers_.clear();
        outbound_sif_commands_.clear(); next_outbound_sif_id_ = 1;
        voice_snapshot_records_.clear(); voice_snapshot_count_ = 0U;
        sif_receive_address_ = 0U; interrupt_active_ = false;
        next_thread_stack_top_ = 0x1f0000U;
        next_sysmem_address_ = (next_sysmem_address_ + 15U) & ~15U; allocations_.clear();
        current_thread_id_ = 1U; current_thread_priority_ = 0U;
        extension_base_ = 0U; extension_file_end_ = 0U; extension_image_end_ = 0U;
        disc_sector_source_ = {};
        return true;
    }

    // Loads a second retail module into the SAME live IOP image without
    // disturbing any state (docs/archive/AUDIO_HISTORY.md section 8, increment 8): the
    // retail GTFSCDVD.IRX at its PCSX2-observed base 0x78c30 alongside the
    // already-booted RWA.IRX at 0x65130. Segments and relocations are
    // applied exactly like load_irx, but threads/semaphores/kernel state
    // and the CPU context are untouched, the sysmem watermark advances past
    // the image (retail loadmodule marks the range used), and the
    // file-backed range is protected from EE/IOP uploads like the primary
    // module's. The module's entry is NOT executed; callers bootstrap it
    // explicitly (run_bootstrap_call).
    bool load_extension_irx(std::span<const std::byte> elf, std::uint32_t load_base) {
        if (module_base_ == 0U || extension_base_ != 0U) return false;
        load_error_ = {};
        // Overlap check first (the image loader writes memory directly):
        // the extension must not overlap the primary image and must stay
        // in bounds.
        const auto phoff = read_elf_u32(elf, 28U);
        const auto phentsize = read_elf_u16(elf, 42U);
        const auto phnum = read_elf_u16(elf, 44U);
        std::uint32_t min_vaddr = ~std::uint32_t{0U};
        std::uint32_t max_end = 0U;
        for (std::uint32_t i = 0; i < phnum; ++i) {
            const auto at = static_cast<std::size_t>(phoff) +
                            static_cast<std::size_t>(i) * phentsize;
            if (at + 32U > elf.size() || read_elf_u32(elf, at) != 1U) continue;
            const auto vaddr = read_elf_u32(elf, at + 8U) + load_base;
            const auto memsz = read_elf_u32(elf, at + 20U);
            min_vaddr = std::min(min_vaddr, vaddr);
            max_end = std::max(max_end, vaddr + memsz);
        }
        if (min_vaddr >= max_end) return false;
        const bool overlaps_primary =
            min_vaddr < module_image_end_ && module_base_ < max_end;
        if (overlaps_primary || max_end > memory_.size()) return false;
        IrxImageResult image{};
        if (!load_irx_image(elf, load_base, image)) return false;
        extension_base_ = load_base;
        extension_file_end_ = image.file_end;
        extension_image_end_ = image.image_end;
        if (image.image_end > next_sysmem_address_) next_sysmem_address_ = image.image_end;
        return true;
    }

    // The medium behind cdvdman sceCdRead once the extension module is
    // loaded: (lbn, sectors, host_out) -> success, filling exactly
    // sectors*2048 bytes of user data. The owner wires the validated raw
    // sector cache (docs/archive/AUDIO_HISTORY.md section 8, increment 8 tail: 2448-byte
    // units, user data at offset 0); the executor copies into IOP RAM.
    // Without a source, sceCdRead honestly fails.
    using DiscSectorSource =
        std::function<bool(std::uint32_t lbn, std::uint32_t sectors,
                           std::byte* host_out)>;
    void set_disc_sector_source(DiscSectorSource source) {
        disc_sector_source_ = std::move(source);
    }
    [[nodiscard]] bool has_disc_sector_source() const {
        return static_cast<bool>(disc_sector_source_);
    }
    // Diagnostic write-watch: records (pc, value, index) for every word
    // write into any registered range. Multiple ranges may be registered
    // (each watch_writes call appends); bounded 256-entry record list.
    void watch_writes(std::uint32_t address, std::uint32_t words) {
        watch_ranges_.push_back({address, words});
    }
    void clear_write_watch() { watch_ranges_.clear(); }
    struct WatchedWrite {
        std::uint32_t pc{};
        std::uint32_t value{};
        std::uint32_t offset{};
    };
    [[nodiscard]] const std::vector<WatchedWrite>& watched_writes() const {
        return watched_writes_;
    }
    // First executed-CPU store into the primary module's text (see
    // write_word). Zero pc = never happened.
    struct TextWrite {
        std::uint32_t pc{};
        std::uint32_t address{};
        std::uint32_t value{};
    };
    [[nodiscard]] TextWrite first_text_write() const { return first_text_write_; }
    [[nodiscard]] std::uint64_t first_text_write_count() const {
        return first_text_write_count_;
    }
    [[nodiscard]] std::uint64_t unmapped_data_stores() const {
        return unmapped_data_stores_;
    }
    [[nodiscard]] std::uint64_t unmapped_data_loads() const {
        return unmapped_data_loads_;
    }
    // Retail-crash semantics for an uncommitted MemoryFault (increment 8):
    // the faulting CONTEXT dies — a thread is retired (dormant, like
    // ExitThread; it can never resume, preserving the no-resume policy) and
    // an interrupt context is dropped — while the rest of the machine keeps
    // running. The exact fault evidence stays in the Stop the caller
    // recorded. A faulted whole-machine freeze (the earlier policy)
    // killed every later command behind one dead handler, which is both
    // worse fidelity and worse evidence.
    void retire_faulted_context() {
        if (interrupt_active_) {
            interrupt_active_ = false;
            interrupt_blocked_ = false;
            // The interrupted thread's saved state is orphaned with the
            // interrupt; retire it too (its continuation can never resume).
            if (interrupted_thread_id_ >= 2U &&
                interrupted_thread_id_ - 2U < threads_.size()) {
                auto& victim = threads_[interrupted_thread_id_ - 2U];
                victim.runnable = false;
                victim.dormant = true;
            }
        } else if (current_thread_id_ >= 2U &&
                   current_thread_id_ - 2U < threads_.size()) {
            auto& victim = threads_[current_thread_id_ - 2U];
            victim.runnable = false;
            victim.sleeping = false;
            victim.waiting_semaphore = 0U;
            victim.waiting_wakeup = false;
            victim.waiting_event_flag = 0U;
            victim.waiting_rpc_queue = 0U;
            victim.dormant = true;
        }
        dispatch_next_runnable();
    }
    // Direct IOP write for MODELED kernel-service deliveries only (the RPC
    // request buffer behind deliver_rpc_call, exactly like the sifcmd
    // packet copy inside deliver_sif_command): these are the kernel's own
    // writes, not guest uploads, and must bypass the module-image upload
    // protection.
    bool write_iop_bytes_for_delivery(std::uint32_t address,
                                      std::span<const std::byte> bytes) {
        if (address > memory_.size() || memory_.size() - address < bytes.size())
            return false;
        for (std::size_t index = 0; index < bytes.size(); ++index)
            memory_[address + index] = std::to_integer<std::uint8_t>(bytes[index]);
        return true;
    }
    // Raises the sysmem allocation watermark to the retail partition base:
    // retail's IOP sysmem hands out user allocations from ~0x174100 (the
    // live savestate: GTFSCDVD's first allocation = its stream array at
    // 0x174100, handle array at 0x184200 = 0x174100 + 3*(0x40+0x4000)), and
    // the low region belongs to the game's own buffers. Our modeled
    // allocator started near zero, colliding with them.
    void raise_sysmem_allocation_base(std::uint32_t base) {
        if (base > next_sysmem_address_) next_sysmem_address_ = base;
    }
    [[nodiscard]] std::uint32_t extension_base() const { return extension_base_; }
    [[nodiscard]] std::uint32_t extension_file_end() const { return extension_file_end_; }

    // Modeled sifcmd RPC delivery to a registered retail server (the kernel
    // service behind sceSifRpcMain): wakes the parked server thread and runs
    // the retail dispatch it registered — (cmd, data, size) — as ITS thread
    // context, with the sentinel-return convention so the model re-parks
    // the thread in the RpcMain loop when the dispatch returns. The request
    // bytes must already be resident at `request_iop_address` (the caller
    // mirrors them, exactly like the opcode-0x14 extra-DMA mirror). The
    // dispatch runs on guest time across clock windows; a mid-dispatch
    // DelayThread (the retail disk-ready retry loop) parks and resumes
    // honestly. Returns false when no retail server is parked on a queue.
    [[nodiscard]] bool deliver_rpc_call(std::uint32_t command,
                                        std::uint32_t request_iop_address,
                                        std::uint32_t request_size) {
        if (rpc_dispatch_active_ || rpc_queue_address_ == 0U ||
            rpc_dispatch_function_ == 0U || rpc_queue_thread_ < 2U ||
            rpc_queue_thread_ - 2U >= threads_.size())
            return false;
        auto& server = threads_[rpc_queue_thread_ - 2U];
        if (server.waiting_rpc_queue != rpc_queue_address_ || server.runnable)
            return false;
        server.registers = std::array<std::uint32_t, 32>{};
        server.registers[4U] = command;
        server.registers[5U] = request_iop_address;
        server.registers[6U] = request_size;
        server.registers[29U] = server.stack_base + server.stack_size;
        server.registers[31U] = sentinel_return_address();
        server.pc = rpc_dispatch_function_;
        server.waiting_rpc_queue = 0U;
        server.runnable = true;
        rpc_dispatch_active_ = true;
        dispatch_next_runnable();
        return true;
    }
    [[nodiscard]] bool rpc_dispatch_active() const { return rpc_dispatch_active_; }
    [[nodiscard]] std::uint32_t rpc_request_buffer() const {
        return rpc_request_buffer_;
    }
    [[nodiscard]] std::uint32_t rpc_dispatch_function() const {
        return rpc_dispatch_function_;
    }
    [[nodiscard]] std::uint32_t rpc_queue_address() const { return rpc_queue_address_; }

    bool load_text(std::span<const std::byte> text, std::uint32_t entry,
                   std::uint32_t memory_size = 2U * 1024U * 1024U) {
        if (text.size() > memory_size || entry >= text.size()) return false;
        memory_.assign(memory_size, 0U);
        module_base_ = 0U;
        for (std::size_t i = 0; i < text.size(); ++i) memory_[i] = std::to_integer<std::uint8_t>(text[i]);
        pc_ = entry;
        gpr_.fill(0U);
        hi_ = 0U;
        lo_ = 0U;
        pending_target_ = 0U;
        in_delay_slot_ = false;
        enabled_irqs_ = 0U;
        cpu_interrupts_enabled_ = true;
        clock_ticks_ = 0U;
        clock_ticks_ceiling_ = ~std::uint64_t{0U};
        halted_ = false;
        semaphores_.clear();
        threads_.clear();
        next_thread_stack_top_ = 0x1f0000U;
        next_sysmem_address_ = (static_cast<std::uint32_t>(text.size()) + 15U) & ~15U;
        allocations_.clear();
        current_thread_id_ = 1U; current_thread_priority_ = 0U;
        sifcmd_initialized_ = false;
        spu2_initialized_ = false;
        sif_command_buffer_ = 0U; sif_command_buffer_count_ = 0U; sif_handlers_.clear();
        event_flags_.clear();
        transfer_handlers_.fill(0U); transfer_handler_arguments_.fill(0U);
        spu2_interrupt_handler_ = 0U; spu2_interrupt_argument_ = 0U;
        core_attributes_.clear();
        parameter_registers_.clear(); address_registers_.clear(); switch_registers_.clear();
        spu2_ram_.fill(0U); voice_transfers_.clear();
        outbound_sif_commands_.clear(); next_outbound_sif_id_ = 1;
        voice_snapshot_records_.clear(); voice_snapshot_count_ = 0U;
        sif_receive_address_ = 0U; interrupt_active_ = false;
        return true;
    }

    void attach_voice_state(RwaSpu2VoiceState* voices) { voices_ = voices; }

    // Mirror only bytes which the EE/IOP bridge has already proven were
    // physically uploaded. This does not execute or acknowledge a command.
    // The retail module's executable text is immutable once loaded, so an
    // observed transfer overlapping it is skipped and counted. File-backed
    // data/BSS and ordinary IOP RAM remain writable: state-3 reads and real
    // SIF uploads can legally target those regions. Opcode 0x05 is not such
    // an upload; it consumes already-resident IOP bytes via VoiceTrans.
    // Guest-visible IOP RAM (owned by EeKernelHle) is unchanged here.
    [[nodiscard]] bool mirror_iop_upload(std::uint32_t address,
                                         std::span<const std::byte> bytes) {
        if (address > memory_.size() || memory_.size() - address < bytes.size()) return false;
        const std::uint32_t begin = address;
        const std::uint32_t end = address + static_cast<std::uint32_t>(bytes.size());
        // Both retail images' file-backed ranges are protected: executed
        // module code must not be replaced by guest uploads (BSS work areas
        // stay writable).
        const auto protected_byte = [&](std::uint32_t a) {
            const auto primary_end = module_text_end_ != 0U ? module_text_end_
                                                             : module_file_end_;
            if (primary_end != 0U && a >= module_base_ && a < primary_end)
                return true;
            // The extension image is protected through its BSS: retail's
            // game never writes GTFSCDVD memory at all, so a guest upload
            // crossing it is fabricated-address evidence.
            if (extension_base_ != 0U && extension_image_end_ != 0U &&
                a >= extension_base_ && a < extension_image_end_)
                return true;
            return false;
        };
        bool skipped = false;
        for (std::uint32_t a = begin; a < end; ++a) {
            if (protected_byte(a)) {
                skipped = true;
                continue;
            }
            memory_[a] = std::to_integer<std::uint8_t>(bytes[a - begin]);
        }
        if (skipped) {
            if (module_upload_skips_ == 0U) {
                first_module_upload_skip_ = {begin, end - begin};
            }
            ++module_upload_skips_;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t module_upload_skips() const { return module_upload_skips_; }
    [[nodiscard]] std::uint32_t first_module_upload_skip_address() const {
        return first_module_upload_skip_.first;
    }
    [[nodiscard]] std::uint32_t first_module_upload_skip_size() const {
        return first_module_upload_skip_.second;
    }
    [[nodiscard]] std::uint32_t module_image_end() const { return module_image_end_; }
    // TEMPORARY DIAGNOSTIC: scheduler/thread state for stop-reason triage.
    struct ThreadDebug { std::uint32_t id; bool runnable; std::uint32_t pc; };
    [[nodiscard]] std::uint32_t debug_current_thread() const { return current_thread_id_; }
    [[nodiscard]] std::uint32_t debug_current_ra() const { return gpr_[31U]; }
    [[nodiscard]] std::uint32_t debug_current_a0() const { return gpr_[4U]; }
    [[nodiscard]] std::uint32_t debug_current_sp() const { return gpr_[29U]; }
    [[nodiscard]] std::uint32_t debug_current_gpr(std::size_t index) const { return gpr_[index]; }
    // TEMPORARY DIAGNOSTIC: writers of module BSS 0xdff4 (sema-id global).
    struct DiagWordWrite { std::uint32_t pc; std::uint32_t value; };
    [[nodiscard]] const std::vector<DiagWordWrite>& diag_word_writes() const {
        return diag_word_writes_;
    }
    [[nodiscard]] std::uint32_t debug_current_pc() const { return pc_; }
    // Oldest-to-newest snapshot of the rolling PC ring.
    [[nodiscard]] std::array<std::uint32_t, 32> debug_pc_history() const {
        std::array<std::uint32_t, 32> ordered{};
        for (std::size_t index = 0U; index < pc_history_.size(); ++index) {
            ordered[index] = pc_history_[(pc_history_index_ + index) %
                                         pc_history_.size()];
        }
        return ordered;
    }
    [[nodiscard]] std::uint8_t debug_memory_byte(std::uint32_t address) const {
        return memory_[address];
    }
    [[nodiscard]] std::vector<ThreadDebug> debug_threads() const {
        std::vector<ThreadDebug> out;
        out.reserve(threads_.size());
        for (std::size_t i = 0; i < threads_.size(); ++i)
            out.push_back({static_cast<std::uint32_t>(i) + 2U, threads_[i].runnable,
                           threads_[i].pc});
        return out;
    }
    [[nodiscard]] const std::vector<VoiceTransfer>& voice_transfers() const {
        return voice_transfers_;
    }
    [[nodiscard]] const std::vector<OutboundSifCommand>& outbound_sif_commands() const {
        return outbound_sif_commands_;
    }
    void discard_outbound_sif_commands(std::size_t count) {
        count = std::min(count, outbound_sif_commands_.size());
        outbound_sif_commands_.erase(
            outbound_sif_commands_.begin(),
            outbound_sif_commands_.begin() + static_cast<std::ptrdiff_t>(count));
    }
    [[nodiscard]] const std::vector<VoiceSnapshotRecord>& voice_snapshot_records() const {
        return voice_snapshot_records_;
    }
    [[nodiscard]] std::uint64_t voice_snapshot_count() const { return voice_snapshot_count_; }
    // Upload-time SPU2 residency notification: fired synchronously after each
    // successful sceSdVoiceTrans copy so Spu2GuestLink can mirror the exact
    // resident bytes into the modelled machine. The sink observes executed
    // retail code; it never initiates a transfer or keys a voice.
    using Spu2UploadSink =
        std::function<void(std::uint32_t spu_address, std::uint32_t iop_address,
                           std::uint32_t size)>;
    void set_spu2_upload_sink(Spu2UploadSink sink) {
        spu2_upload_sink_ = std::move(sink);
    }
    // Bounded read-only view of resident SPU2 ADPCM bytes.
    [[nodiscard]] std::span<const std::byte> spu2_range(std::uint32_t address,
                                                        std::uint32_t size) const {
        if (address > spu2_ram_.size() || spu2_ram_.size() - address < size) return {};
        return {reinterpret_cast<const std::byte*>(spu2_ram_.data() + address), size};
    }
    [[nodiscard]] bool inspect_spu2_byte(std::uint32_t address, std::uint8_t& value) const {
        if (address >= spu2_ram_.size()) return false;
        value = spu2_ram_[address];
        return true;
    }

    SifDeliveryResult deliver_sif_command(std::span<const std::byte> packet) {
        if (!sifcmd_initialized_ || sif_command_buffer_count_ == 0U)
            return SifDeliveryResult::NotInitialized;
        if (interrupt_active_) return SifDeliveryResult::Busy;
        if (packet.size() != 0x30U || (read_elf_u32(packet, 0U) & 0xffU) != 0x30U)
            return SifDeliveryResult::InvalidPacket;
        const auto command = read_elf_u32(packet, 8U);
        const SifCommandHandler* handler = nullptr;
        for (const auto& candidate : sif_handlers_)
            if (candidate.command == command) { handler = &candidate; break; }
        if (handler == nullptr || handler->function >= memory_.size())
            return SifDeliveryResult::MissingHandler;
        constexpr std::uint32_t packet_size = 0x30U;
        if (sif_receive_address_ == 0U) {
            if (next_sysmem_address_ > next_thread_stack_top_ - packet_size)
                return SifDeliveryResult::InvalidPacket;
            sif_receive_address_ = next_sysmem_address_;
            next_sysmem_address_ += packet_size;
        }
        for (std::size_t index = 0; index < packet.size(); ++index)
            memory_[sif_receive_address_ + index] = std::to_integer<std::uint8_t>(packet[index]);
        if (read_elf_u32(packet, 0x10U) == 0x14U) {
            constexpr std::uint32_t snapshot = 0xe100U;
            constexpr std::uint32_t record_size = 0x20U;
            constexpr std::uint32_t voice_count = 48U;
            voice_snapshot_records_.clear();
            voice_snapshot_records_.reserve(voice_count);
            for (std::uint32_t voice = 0U; voice < voice_count; ++voice) {
                VoiceSnapshotRecord record{.voice = static_cast<std::uint8_t>(voice)};
                for (std::uint32_t word = 0U; word < record.words.size(); ++word)
                    (void)read_word(snapshot + voice * record_size + word * 4U, record.words[word]);
                voice_snapshot_records_.push_back(record);
            }
            ++voice_snapshot_count_;
        }
        interrupted_gpr_ = gpr_; interrupted_pc_ = pc_; interrupted_thread_id_ = current_thread_id_;
        interrupt_active_ = true;
        halted_ = false; // The interrupt handler context is runnable.
        gpr_.fill(0U); gpr_[4U] = sif_receive_address_; gpr_[5U] = handler->argument;
        gpr_[29U] = 0x1ff000U; gpr_[31U] = static_cast<std::uint32_t>(memory_.size());
        pc_ = handler->function; pending_target_ = 0U; in_delay_slot_ = false;
        return SifDeliveryResult::Queued;
    }

    [[nodiscard]] bool interrupt_active() const { return interrupt_active_; }
    // Sentinel return address used by the modeled sifcmd/bootstrap call
    // convention (one past the last valid IOP RAM word).
    [[nodiscard]] std::uint32_t sentinel_return_address() const {
        return static_cast<std::uint32_t>(memory_.size());
    }

    // Retail kernel-style bootstrap invocation of a module entry point.
    //
    // The retail opcode-0x32 stream-open path re-creates the module's global
    // request queue through its dispatcher ops table (module 0xa308, reached
    // via the ops pointer at 0xe0f0, slot +8) inside the op-0x32 sifcmd
    // handler. That handler delegates the open itself through the GTFSCDVD
    // vtable owned by another IOP module this executor deliberately does not
    // load, so the runtime models the open as HLE-owned (docs/AUDIO.md
    // 7.6/7.8) and invokes the retail queue-creation entry directly here.
    // Every executed instruction is retail code; only the call trigger is
    // modeled. The call convention is the same sentinel-return interruption
    // used by deliver_sif_command: the caller steps the executor until
    // interrupt_active() clears (the return through the sentinel address
    // restores the interrupted context in step()).
    [[nodiscard]] SifDeliveryResult run_bootstrap_call(std::uint32_t function) {
        if (interrupt_active_) return SifDeliveryResult::Busy;
        if (function >= memory_.size()) return SifDeliveryResult::InvalidPacket;
        interrupted_gpr_ = gpr_; interrupted_pc_ = pc_; interrupted_thread_id_ = current_thread_id_;
        interrupt_active_ = true;
        halted_ = false; // The bootstrap call context is runnable.
        gpr_.fill(0U);
        gpr_[29U] = 0x1ff000U;
        gpr_[31U] = static_cast<std::uint32_t>(memory_.size());
        pc_ = function; pending_target_ = 0U; in_delay_slot_ = false;
        return SifDeliveryResult::Queued;
    }

    bool schedule_next_runnable() {
        if (current_thread_id_ >= 2U && current_thread_id_ - 2U < threads_.size()) {
            auto& current = threads_[current_thread_id_ - 2U];
            current.registers = gpr_;
            current.pc = pc_;
        }
        return dispatch_next_runnable();
    }

    // Earliest pending DelayThread deadline, or kNoWakeTick when nothing
    // sleeps. Read-only view for master-clock windowing.
    [[nodiscard]] std::uint64_t earliest_sleep_wake_tick() const {
        std::uint64_t earliest = kNoWakeTick;
        for (const auto& thread : threads_)
            if (thread.sleeping && thread.wake_tick < earliest) earliest = thread.wake_tick;
        return earliest;
    }

    // Master-clock ceiling for the scheduler's idle fast-forward. The retail
    // IOP runs concurrently with the EE; in this model its windows run at
    // deterministic guest boundaries, so the executor's system clock must
    // never advance beyond the guest master clock's current IOP-SysClock
    // target. The default is unlimited (boot trampoline and unit execution
    // keep their original semantics); the shadow runtime sets the ceiling
    // before every window.
    void set_clock_ticks_ceiling(std::uint64_t ceiling) { clock_ticks_ceiling_ = ceiling; }

    // Pass modeled idle IOP time: move the system clock forward to `tick`
    // (never backwards) and wake sleeping threads whose retail deadline has
    // passed. The retail CPU HALTs while idle while its timers keep counting,
    // which is exactly this jump. Returns true when at least one sleeper
    // woke. Does not dispatch; callers re-evaluate the schedule themselves.
    bool advance_system_clock_to(std::uint64_t tick) {
        if (tick <= clock_ticks_) return false;
        clock_ticks_ = tick;
        bool woke = false;
        for (auto& thread : threads_) {
            if (thread.sleeping && thread.wake_tick <= clock_ticks_) {
                thread.sleeping = false;
                thread.runnable = true;
                woke = true;
            }
        }
        return woke;
    }

    // Dispatch the next runnable thread without saving the current parked
    // context. Used when the parked frame is a completed handler return that
    // belongs to no schedulable thread (a stale sentinel frame); saving it
    // would overwrite the blocked owner's real saved state.
    bool dispatch_next_runnable() {
        for (auto& thread : threads_) {
            if (thread.sleeping && thread.wake_tick <= clock_ticks_) {
                thread.sleeping = false;
                thread.runnable = true;
            }
        }
        bool any_runnable = false;
        for (const auto& thread : threads_) any_runnable = any_runnable || thread.runnable;
        if (!any_runnable) {
            const std::uint64_t earliest = earliest_sleep_wake_tick();
            // The idle fast-forward may never outrun guest time. With a
            // master-clock ceiling set (see set_clock_ticks_ceiling), a
            // wakeup strictly beyond it stays pending: the runtime advances
            // the system clock to it only when the guest master clock has
            // reached that tick.
            if (earliest != kNoWakeTick && earliest <= clock_ticks_ceiling_) {
                clock_ticks_ = earliest;
                for (auto& thread : threads_) {
                    if (thread.sleeping && thread.wake_tick <= clock_ticks_) {
                        thread.sleeping = false; thread.runnable = true;
                    }
                }
            }
        }
        // Retail IOP scheduler semantics: the runnable thread with the
        // highest priority (lowest number) runs; equal priorities run in
        // creation order. The first module alone never contended (its
        // workers were created in priority order), but the second retail
        // module's threads (GTFSCDVD's RPC server + stream workers) share
        // the scheduler and make real dispatch choices.
        std::optional<std::size_t> chosen{};
        for (std::size_t index = 0U; index < threads_.size(); ++index) {
            const auto& thread = threads_[index];
            if (!thread.runnable) continue;
            if (!chosen || thread.priority < threads_[*chosen].priority) chosen = index;
        }
        if (!chosen) {
            // Nothing may execute: every worker is blocked, asleep, or
            // waiting, and no pending wakeup is due within the clock
            // ceiling. Latch the halt — the live pc_/gpr_ still hold a
            // BLOCKED context's continuation, and executing it would run a
            // blocked thread as if its wait had returned (observed
            // corruption: one master-clock window between two commands
            // changed the second command's retail protocol outcome). Only a
            // successful dispatch, a SIF delivery, or a bootstrap call may
            // resume execution.
            halted_ = true;
            return false;
        }
        auto& thread = threads_[*chosen];
        current_thread_id_ = static_cast<std::uint32_t>(*chosen) + 2U;
        current_thread_priority_ = thread.priority;
        gpr_ = thread.registers;
        pc_ = thread.pc;
        pending_target_ = 0U;
        in_delay_slot_ = false;
        halted_ = false;
        return true;
    }

    // Re-evaluate every thread blocked in WaitEventFlag against the current
    // flag bit patterns (called after SetEventFlag ORs new bits in). A
    // satisfied waiter writes its out-bits, applies WEF_CLEAR to the flag,
    // and becomes runnable again.
    void wake_event_waiters() {
        for (auto& thread : threads_) {
            if (thread.waiting_event_flag == 0U) continue;
            if (thread.waiting_event_flag > event_flags_.size()) continue;
            auto& event = event_flags_[thread.waiting_event_flag - 1U];
            if (!event.live) continue;
            const auto satisfied = (thread.waiting_event_mode & 1U) == 0U
                                       ? (event.bits & thread.waiting_event_pattern) != 0U
                                       : (event.bits & thread.waiting_event_pattern) ==
                                             thread.waiting_event_pattern;
            if (!satisfied) continue;
            static_cast<void>(write_word(thread.waiting_event_out, event.bits));
            if (thread.waiting_event_mode & 2U)
                event.bits &= ~thread.waiting_event_pattern;
            thread.waiting_event_flag = 0U;
            thread.waiting_event_pattern = 0U;
            thread.waiting_event_mode = 0U;
            thread.waiting_event_out = 0U;
            thread.runnable = true;
        }
    }

    Stop step() {
        // A latched halt means every schedulable context is blocked; the
        // live pc_/gpr_ belong to one of them and must not execute (see
        // dispatch_next_runnable).
        if (halted_) return {StopReason::Halted, pc_, 0U};
        pc_history_[pc_history_index_++ % pc_history_.size()] = pc_;
        // Second-module import handling (increment 8). GTFSCDVD shares RWA's
        // kernel-service imports (same module versions, same ordinals): its
        // stub PCs translate to RWA's stub addresses so the existing
        // implementations service both modules. Extension-only services
        // (cdvdman, sifman, sifcmd RPC setup, extra sysclib) dispatch below
        // on the extension's own addresses. Anything else left inside the
        // import-stub region is an unmodeled import: strict stop with
        // evidence — a bare stub's `jr ra` would silently return its
        // ordinal as a result.
        if (extension_base_ != 0U && pc_ >= extension_base_ &&
            pc_ < extension_base_ + kExtensionStubRegionEnd) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto offset = pc_ - extension_base_;
            const auto translated = extension_shared_stub_translation(offset);
            if (translated.has_value()) {
                pc_ = module_address(*translated);
            } else if (first != 0x03e00008U || (second & 0xffff0000U) != 0x24000000U) {
                return {StopReason::UnsupportedInstruction, pc_, first};
            } else {
                return step_extension_import(offset, second & 0xffffU);
            }
        }
        // The sifcmd interrupt handler's sentinel return restores the
        // interrupted context FIRST: a retail RPC dispatch (below) may be
        // the interrupted context, and its own sentinel must only fire when
        // the dispatch actually returns, never on a handler's return.
        if (interrupt_active_ && pc_ == memory_.size()) {
            gpr_ = interrupted_gpr_; pc_ = interrupted_pc_;
            current_thread_id_ = interrupted_thread_id_; interrupt_active_ = false;
            ++clock_ticks_;
            return {};
        }
        // The retail RPC dispatch executed on the server thread returned
        // through the sentinel (deliver_rpc_call): the kernel's RpcMain
        // service re-parks the thread on its queue and dispatches the next
        // runnable thread.
        if (rpc_dispatch_active_ && pc_ == memory_.size()) {
            if (current_thread_id_ >= 2U &&
                current_thread_id_ - 2U < threads_.size()) {
                auto& server = threads_[current_thread_id_ - 2U];
                server.registers = gpr_;
                server.runnable = false;
                server.waiting_rpc_queue = rpc_queue_address_;
            }
            rpc_dispatch_active_ = false;
            ++clock_ticks_;
            schedule_next_runnable();
            return {};
        }
        // RWA.IRX intrman 1.2 import, ordinal 6 (EnableIntr). Validate the
        // actual stub before changing state, exactly like the libsd hooks.
        if (pc_ == module_address(0x8920U)) {
            std::uint32_t first{};
            std::uint32_t second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000006U || gpr_[4U] >= 64U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            enabled_irqs_ |= std::uint64_t{1U} << gpr_[4U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // intrman 1.2 ordinal 17, CpuSuspendIntr. Preserve the previous CPU
        // interrupt state through the caller-provided word for CpuResumeIntr.
        if (pc_ == module_address(0x8928U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000011U ||
                !write_word(gpr_[4U], cpu_interrupts_enabled_ ? 1U : 0U))
                return {StopReason::UnsupportedInstruction, pc_, second};
            cpu_interrupts_enabled_ = false;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // intrman ordinal 18, CpuResumeIntr(previous_state).
        if (pc_ == module_address(0x8930U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000012U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            cpu_interrupts_enabled_ = gpr_[4U] != 0U;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // RWA.IRX thbase 1.2 ordinal 39, USec2SysClock.  The IOP clock is
        // 36.864 MHz; the ABI writes a 64-bit clock value to a1.
        if (pc_ == module_address(0x8b30U)) {
            std::uint32_t first{};
            std::uint32_t second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000027U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            const auto ticks = static_cast<std::uint64_t>(gpr_[4U]) * 36864U / 1000U;
            if (!write_word(gpr_[5U], static_cast<std::uint32_t>(ticks)) ||
                !write_word(gpr_[5U] + 4U, static_cast<std::uint32_t>(ticks >> 32U)))
                return {StopReason::MemoryFault, pc_, second};
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // RWA.IRX thbase 1.2 ordinal 34, GetSystemTime. The scheduler clock is
        // maintained in native IOP ticks and reset with each loaded image.
        if (pc_ == module_address(0x8b28U)) {
            std::uint32_t first{};
            std::uint32_t second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000022U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            if (!write_word(gpr_[4U], static_cast<std::uint32_t>(clock_ticks_)) ||
                !write_word(gpr_[4U] + 4U, static_cast<std::uint32_t>(clock_ticks_ >> 32U)))
                return {StopReason::MemoryFault, pc_, second};
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thbase ordinal 43, GetSystemTimeLow: low 32 bits of the same
        // monotonic IOP tick counter used by GetSystemTime.
        if (pc_ == module_address(0x8b40U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400002bU)
                return {StopReason::UnsupportedInstruction, pc_, second};
            gpr_[2U] = static_cast<std::uint32_t>(clock_ticks_);
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // RWA.IRX thsemap 1.1 ordinal 4, CreateSema. Retain the exact kernel
        // descriptor so later Wait/Signal imports can operate on real state.
        if (pc_ == module_address(0x8bb0U)) {
            std::uint32_t first{}, second{}, attributes{}, option{}, initial{}, maximum{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                !read_word(gpr_[4U], attributes) || !read_word(gpr_[4U] + 4U, option) ||
                !read_word(gpr_[4U] + 8U, initial) || !read_word(gpr_[4U] + 12U, maximum))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000004U || maximum == 0U || initial > maximum)
                return {StopReason::UnsupportedInstruction, pc_, second};
            semaphores_.push_back({attributes, option, initial, maximum});
            gpr_[2U] = static_cast<std::uint32_t>(semaphores_.size());
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thsemap ordinal 9, PollSema: non-blocking acquisition. The retail
        // kernel never stops on this import: an id the model does not know
        // (including a0=0, seen live in the first opcode-0x05 command) is a
        // modeling gap answered with KE_UNKNOWN_SEMID (-408) exactly like
        // DeleteSema's invalid-id path, an empty semaphore returns
        // KE_SEMA_ZERO (-419, ps2sdk iop kerr.h), and success decrements the
        // count and returns the semaphore id.
        if (pc_ == module_address(0x8bd8U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000009U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (id == 0U || id > semaphores_.size() || !semaphores_[id - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-408); // KE_UNKNOWN_SEMID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            auto& semaphore = semaphores_[id - 1U];
            if (semaphore.count != 0U) {
                --semaphore.count;
                gpr_[2U] = id;
            } else {
                gpr_[2U] = static_cast<std::uint32_t>(-419);
            }
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thsemap ordinal 5, DeleteSema. The retail kernel destroys the
        // semaphore and wakes every thread blocked on it with KE_WAIT_DELETE
        // (-425) as the WaitSema result. The live opcode-0x46 path executes
        // this stub (pc 0x6dce8 = module 0x8bb8, word 0x24000005).
        if (pc_ == module_address(0x8bb8U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000005U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (id == 0U || id > semaphores_.size() || !semaphores_[id - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-408); // KE_UNKNOWN_SEMID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            semaphores_[id - 1U].live = false;
            for (auto& thread : threads_) {
                if (thread.waiting_semaphore != id) continue;
                thread.waiting_semaphore = 0U;
                thread.registers[2U] = static_cast<std::uint32_t>(-425); // KE_WAIT_DELETE
                thread.runnable = true;
            }
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // Interrupt-continuation wake shared by SignalSema/iSignalSema: the
        // signaling thread keeps its own stub result (saved to its TCB),
        // and the parked sifcmd handler becomes the live context again.
        auto resume_parked_interrupt = [&](bool signaled_ok) {
            if (!interrupt_blocked_ || interrupt_blocked_semaphore_ != gpr_[4U])
                return false;
            interrupt_blocked_ = false;
            if (current_thread_id_ >= 2U &&
                current_thread_id_ - 2U < threads_.size()) {
                auto& signaler = threads_[current_thread_id_ - 2U];
                signaler.registers = gpr_;
                signaler.registers[2U] = signaled_ok ? 0U : static_cast<std::uint32_t>(-419);
                signaler.pc = gpr_[31U];
                signaler.runnable = true;
            }
            gpr_ = interrupt_blocked_gpr_;
            pc_ = interrupt_blocked_pc_;
            return true;
        };
        if (pc_ == module_address(0x8bc0U)) { // thsemap ordinal 6, SignalSema
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000006U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            // Unknown-id / overflow are kernel errors the caller survives, not
            // shadow-fatal stops (see iSignalSema below).
            if (id == 0U || id > semaphores_.size()) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            if (!semaphores_[id - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-408); // KE_UNKNOWN_SEMID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            bool woke = false;
            for (auto& thread : threads_) {
                if (thread.waiting_semaphore == id) {
                    thread.waiting_semaphore = 0U; thread.runnable = true; woke = true; break;
                }
            }
            if (!woke) {
                if (semaphores_[id - 1U].count >= semaphores_[id - 1U].maximum) {
                    if (resume_parked_interrupt(false)) return {};
                    gpr_[2U] = static_cast<std::uint32_t>(-1);
                    pc_ = gpr_[31U];
                    ++clock_ticks_;
                    return {};
                }
                ++semaphores_[id - 1U].count;
            }
            if (resume_parked_interrupt(true)) return {};
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8bc8U)) { // thsemap ordinal 7, iSignalSema
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000007U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            // An id the model never allocated is a modeling gap, not a retail
            // fault: the kernel returns an error and the caller proceeds.
            // Stopping the whole shadow for it starves retail streaming.
            if (id == 0U || id > semaphores_.size()) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            if (!semaphores_[id - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-408); // KE_UNKNOWN_SEMID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            bool woke = false;
            for (auto& thread : threads_) {
                if (thread.waiting_semaphore == id) {
                    thread.waiting_semaphore = 0U; thread.runnable = true; woke = true; break;
                }
            }
            if (!woke) {
                if (semaphores_[id - 1U].count >= semaphores_[id - 1U].maximum) {
                    // Kernel overflow error; retail callers branch on <0.
                    if (resume_parked_interrupt(false)) return {};
                    gpr_[2U] = static_cast<std::uint32_t>(-1);
                    pc_ = gpr_[31U];
                    ++clock_ticks_;
                    return {};
                }
                ++semaphores_[id - 1U].count;
            }
            if (resume_parked_interrupt(true)) return {};
            gpr_[2U] = 0U; pc_ = gpr_[31U]; ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8bd0U)) { // thsemap ordinal 8, WaitSema
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000008U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (id == 0U || id > semaphores_.size()) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            if (!semaphores_[id - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-408); // KE_UNKNOWN_SEMID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            if (current_thread_id_ < 2U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (semaphores_[id - 1U].count != 0U) {
                --semaphores_[id - 1U].count;
                gpr_[2U] = 0U; pc_ = gpr_[31U]; ++clock_ticks_;
                return {};
            }
            gpr_[2U] = 0U; pc_ = gpr_[31U]; ++clock_ticks_;
            // The sifcmd interrupt context is not a thread: when a command
            // handler blocks, its continuation parks here (retail DPC
            // semantics) and is restored by the matching SignalSema — never
            // into the interrupted thread's TCB (whose state lives in
            // interrupted_*).
            if (interrupt_active_) {
                interrupt_blocked_ = true;
                interrupt_blocked_semaphore_ = id;
                interrupt_blocked_gpr_ = gpr_;
                interrupt_blocked_pc_ = pc_;
                if (!dispatch_next_runnable())
                    return {StopReason::Halted, pc_, second};
                return {};
            }
            auto& current = threads_[current_thread_id_ - 2U];
            current.registers = gpr_; current.pc = pc_; current.runnable = false;
            current.waiting_semaphore = id;
            if (!schedule_next_runnable()) return {StopReason::Halted, pc_, second};
            return {};
        }
        // RWA.IRX thbase 1.2 ordinals 4/6, CreateThread/StartThread. Creation
        // retains the retail descriptor and allocates a bounded IOP stack;
        // start only makes it runnable, leaving context switching to the
        // scheduler rather than executing the mixer re-entrantly.
        if (pc_ == module_address(0x8ad0U)) {
            std::uint32_t first{}, second{}, attributes{}, option{}, entry{}, stack_size{}, priority{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                !read_word(gpr_[4U], attributes) || !read_word(gpr_[4U] + 4U, option) ||
                !read_word(gpr_[4U] + 8U, entry) || !read_word(gpr_[4U] + 12U, stack_size) ||
                !read_word(gpr_[4U] + 16U, priority))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto aligned_size = (stack_size + 15U) & ~15U;
            if (first != 0x03e00008U || second != 0x24000004U || aligned_size == 0U ||
                aligned_size > next_thread_stack_top_ || entry >= memory_.size())
                return {StopReason::UnsupportedInstruction, pc_, second};
            next_thread_stack_top_ -= aligned_size;
            Thread thread{};
            thread.attributes = attributes; thread.option = option; thread.entry = entry;
            thread.stack_base = next_thread_stack_top_; thread.stack_size = aligned_size;
            thread.priority = priority; thread.pc = entry;
            threads_.push_back(thread);
            gpr_[2U] = static_cast<std::uint32_t>(threads_.size()) + 1U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8ae0U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000006U || id < 2U ||
                id - 1U > threads_.size() || threads_[id - 2U].runnable)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (threads_[id - 2U].dead) {
                // A deleted thread's id no longer exists in the kernel.
                gpr_[2U] = static_cast<std::uint32_t>(-407); // KE_UNKNOWN_THID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            threads_[id - 2U].argument = gpr_[5U];
            threads_[id - 2U].runnable = true;
            threads_[id - 2U].registers.fill(0U);
            threads_[id - 2U].registers[4U] = gpr_[5U];
            threads_[id - 2U].registers[29U] = threads_[id - 2U].stack_base + threads_[id - 2U].stack_size;
            threads_[id - 2U].registers[31U] = static_cast<std::uint32_t>(memory_.size());
            threads_[id - 2U].pc = threads_[id - 2U].entry;
            threads_[id - 2U].dormant = false;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8ad8U)) { // thbase ordinal 5, DeleteThread
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000005U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (id == 0U || id - 2U >= threads_.size() || threads_[id - 2U].dead) {
                gpr_[2U] = static_cast<std::uint32_t>(-407); // KE_UNKNOWN_THID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            auto& thread = threads_[id - 2U];
            // The retail kernel deletes only DORMANT threads. RWA.IRX's
            // teardown helper (module 0x9c94) terminates the worker first
            // and then deletes the same id.
            if (thread.runnable || thread.sleeping || thread.waiting_semaphore != 0U ||
                thread.waiting_wakeup || thread.waiting_event_flag != 0U) {
                gpr_[2U] = static_cast<std::uint32_t>(-414); // KE_NOT_DORMANT
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            thread.dead = true;
            thread.dormant = false;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8ae8U)) { // thbase ordinal 8, ExitThread
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000008U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (current_thread_id_ < 2U || current_thread_id_ - 2U >= threads_.size())
                return {StopReason::UnsupportedInstruction, pc_, second};
            auto& current = threads_[current_thread_id_ - 2U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            // The calling thread never returns from ExitThread: it moves to
            // DORMANT and the kernel dispatches the next ready thread
            // (retail worker main loop at module 0x9bcc).
            current.registers = gpr_;
            current.pc = pc_;
            current.runnable = false;
            current.sleeping = false;
            current.dormant = true;
            current.waiting_wakeup = false;
            current.waiting_semaphore = 0U;
            current.waiting_event_flag = 0U;
            if (!schedule_next_runnable()) return {StopReason::Halted, pc_, second};
            return {};
        }
        // thbase ordinal 10, TerminateThread(thid). Forcibly moves the thread
        // out of READY/WAIT into DORMANT without satisfying any pending wait.
        // Retail RWA.IRX teardown helper (module 0x9c94) runs
        // TerminateThread(tid); DeleteThread(tid); — the live op-0x05 path
        // reached this stub (pc 0x6dc20, stub word 0x2400000a) and stopped
        // the whole shadow. Interrupt-safe variant iTerminateThread is
        // ordinal 11 and is not imported by RWA.IRX.
        if (pc_ == module_address(0x8af0U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x2400000aU)
                return {StopReason::UnsupportedInstruction, pc_, second};
            if (id == 0U || id - 2U >= threads_.size() || threads_[id - 2U].dead) {
                gpr_[2U] = static_cast<std::uint32_t>(-407); // KE_UNKNOWN_THID
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            auto& thread = threads_[id - 2U];
            thread.runnable = false;
            thread.sleeping = false;
            thread.dormant = true;
            thread.waiting_wakeup = false;
            thread.waiting_semaphore = 0U;
            thread.waiting_event_flag = 0U;
            thread.waiting_event_pattern = 0U;
            thread.waiting_event_mode = 0U;
            thread.waiting_event_out = 0U;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // RWA.IRX sifcmd 1.1 ordinal 4, sceSifInitCmd. Its externally visible
        // effect at this boundary is initializing the command subsystem; DMA
        // handlers and RPC registration remain separate imports.
        if (pc_ == module_address(0x8998U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000004U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            sifcmd_initialized_ = true;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // sysmem ordinal 14, Kprintf. Formatting is not emulated, but the
        // retail NUL-terminated format pointer is validated and the ignored
        // return value is deterministic.
        if (pc_ == module_address(0x88fcU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            bool terminated = false;
            for (std::size_t i = gpr_[4U]; i < memory_.size() && i - gpr_[4U] < 4096U; ++i) {
                if (memory_[i] == 0U) { terminated = true; break; }
            }
            if (first != 0x03e00008U || second != 0x2400000eU || !terminated)
                return {StopReason::UnsupportedInstruction, pc_, second};
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8a44U)) { // sysclib ordinal 12, memcpy
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto destination = gpr_[4U]; const auto source = gpr_[5U]; const auto size = gpr_[6U];
            if (first != 0x03e00008U || second != 0x2400000cU ||
                destination > memory_.size() || size > memory_.size() - destination ||
                source > memory_.size() || size > memory_.size() - source)
                return {StopReason::UnsupportedInstruction, pc_, second};
            std::vector<std::uint8_t> copy(memory_.begin() + source, memory_.begin() + source + size);
            std::copy(copy.begin(), copy.end(), memory_.begin() + destination);
            if (destination <= 0x73124U && 0x73128U <= destination + size)
                diag_word_writes_.push_back({pc_, *reinterpret_cast<const std::uint32_t*>(&copy[0x73124U - destination])});
            gpr_[2U] = destination; pc_ = gpr_[31U]; ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8a4cU)) { // sysclib ordinal 14, memset
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto destination = gpr_[4U]; const auto size = gpr_[6U];
            if (first != 0x03e00008U || second != 0x2400000eU ||
                destination > memory_.size() || size > memory_.size() - destination)
                return {StopReason::UnsupportedInstruction, pc_, second};
            std::fill_n(memory_.begin() + destination, size, static_cast<std::uint8_t>(gpr_[5U]));
            gpr_[2U] = destination; pc_ = gpr_[31U]; ++clock_ticks_;
            return {};
        }
        // sysmem ordinals 4/5, AllocSysMemory/FreeSysMemory. Retail RWA uses
        // first-fit type zero here; allocations grow above the loaded image
        // and remain disjoint from descending thread stacks.
        if (pc_ == module_address(0x88e4U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto size = (gpr_[5U] + 15U) & ~15U;
            if (first != 0x03e00008U || second != 0x24000004U || gpr_[4U] != 0U ||
                gpr_[6U] != 0U || size == 0U || size > next_thread_stack_top_ ||
                next_sysmem_address_ > next_thread_stack_top_ - size)
                return {StopReason::UnsupportedInstruction, pc_, second};
            const auto address = next_sysmem_address_;
            next_sysmem_address_ += size;
            allocations_.push_back({address, size, true});
            gpr_[2U] = address;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x88ecU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            auto found = allocations_.end();
            for (auto it = allocations_.begin(); it != allocations_.end(); ++it)
                if (it->address == gpr_[4U] && it->live) { found = it; break; }
            if (first != 0x03e00008U || second != 0x24000005U || found == allocations_.end())
                return {StopReason::UnsupportedInstruction, pc_, second};
            found->live = false;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // sysmem ordinal 7, QueryMaxFreeMemSize. RWA's opcode-0x13 handler
        // publishes this value to the EE before its work-area allocations.
        // This executor's allocator is deliberately monotonic, so the exact
        // largest allocatable extent is the aligned gap between its heap
        // cursor and the descending thread-stack boundary.
        if (pc_ == module_address(0x88f4U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000007U ||
                next_sysmem_address_ > next_thread_stack_top_)
                return {StopReason::UnsupportedInstruction, pc_, second};
            gpr_[2U] = next_thread_stack_top_ - next_sysmem_address_;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thbase ordinal 20, GetThreadId, and ordinal 14,
        // ChangeThreadPriority. ID 1 is the loader/module-start context.
        if (pc_ == module_address(0x8b00U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000014U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            gpr_[2U] = current_thread_id_;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8b20U)) { // thbase ordinal 33, DelayThread
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) || current_thread_id_ < 2U ||
                current_thread_id_ - 2U >= threads_.size())
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000021U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            auto& current = threads_[current_thread_id_ - 2U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            current.registers = gpr_; current.pc = pc_; current.runnable = false;
            current.sleeping = true;
            current.wake_tick = clock_ticks_ + static_cast<std::uint64_t>(gpr_[4U]) * 36864U / 1000U;
            if (!schedule_next_runnable())
                return {StopReason::Halted, pc_, second};
            return {};
        }
        if (pc_ == module_address(0x8b08U)) { // thbase ordinal 24, SleepThread
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) || current_thread_id_ < 2U ||
                current_thread_id_ - 2U >= threads_.size())
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000018U)
                return {StopReason::UnsupportedInstruction, pc_, first};
            auto& current = threads_[current_thread_id_ - 2U];
            gpr_[2U] = 0U; pc_ = gpr_[31U]; ++clock_ticks_;
            current.registers = gpr_; current.pc = pc_; current.runnable = false;
            current.waiting_wakeup = true;
            if (!schedule_next_runnable()) return {StopReason::Halted, pc_, second};
            return {};
        }
        if (pc_ == module_address(0x8b10U)) { // thbase ordinal 25, WakeupThread
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto id = gpr_[4U];
            if (first != 0x03e00008U || second != 0x24000019U || id < 2U ||
                id - 2U >= threads_.size() || !threads_[id - 2U].waiting_wakeup)
                return {StopReason::UnsupportedInstruction, pc_, second};
            threads_[id - 2U].waiting_wakeup = false; threads_[id - 2U].runnable = true;
            gpr_[2U] = 0U; pc_ = gpr_[31U]; ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8af8U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                first != 0x03e00008U || second != 0x2400000eU || gpr_[4U] != current_thread_id_)
                return {StopReason::UnsupportedInstruction, pc_, second};
            current_thread_priority_ = gpr_[5U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // sifcmd ordinals 8/10, sceSifSetCmdBuffer/sceSifAddCmdHandler.
        if (pc_ == module_address(0x89a0U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto bytes = static_cast<std::uint64_t>(gpr_[5U]) * 16U;
            if (first != 0x03e00008U || second != 0x24000008U || !sifcmd_initialized_ ||
                bytes > memory_.size() || gpr_[4U] > memory_.size() - bytes)
                return {StopReason::UnsupportedInstruction, pc_, second};
            sif_command_buffer_ = gpr_[4U];
            sif_command_buffer_count_ = gpr_[5U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x89a8U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400000aU || !sifcmd_initialized_ ||
                gpr_[5U] >= memory_.size())
                return {StopReason::UnsupportedInstruction, pc_, second};
            sif_handlers_.push_back({gpr_[4U], gpr_[5U], gpr_[6U]});
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // libsd ordinal 4, sceSdInit. The retail caller handles -301 as the
        // already-initialized result and otherwise continues on success.
        if (pc_ == module_address(0x8bfcU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000004U || gpr_[4U] != 0U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            gpr_[2U] = spu2_initialized_ ? static_cast<std::uint32_t>(-301) : 0U;
            spu2_initialized_ = true;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thevent ordinal 4, CreateEventFlag. The descriptor is three words:
        // attributes, opaque option, and initial bit pattern.
        if (pc_ == module_address(0x8b64U)) {
            std::uint32_t first{}, second{}, attributes{}, option{}, bits{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                !read_word(gpr_[4U], attributes) || !read_word(gpr_[4U] + 4U, option) ||
                !read_word(gpr_[4U] + 8U, bits))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000004U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            event_flags_.push_back({attributes, option, bits, true});
            gpr_[2U] = static_cast<std::uint32_t>(event_flags_.size());
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thevent ordinal 5, DeleteEventFlag. Marks the flag dead so later
        // references are rejected instead of silently operating on stale
        // state.
        if (pc_ == module_address(0x8b6cU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000005U || gpr_[4U] == 0U ||
                gpr_[4U] > event_flags_.size() || !event_flags_[gpr_[4U] - 1U].live)
                return {StopReason::UnsupportedInstruction, pc_, second};
            event_flags_[gpr_[4U] - 1U].live = false;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thevent ordinal 6, SetEventFlag(evid, bits): OR the bits in and wake
        // every blocked waiter whose WaitEventFlag condition is now satisfied
        // (the waiter itself writes its out-bits and applies WEF_CLEAR).
        if (pc_ == module_address(0x8b74U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000006U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            // Unknown/dead flag id: kernel error, caller survives (see
            // WaitEventFlag). A fatal stop here starves retail streaming.
            if (gpr_[4U] == 0U || gpr_[4U] > event_flags_.size() ||
                !event_flags_[gpr_[4U] - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            auto& event = event_flags_[gpr_[4U] - 1U];
            event.bits |= gpr_[5U];
            wake_event_waiters();
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thevent ordinal 8, ClearEventFlag(evid, clear_bits).
        if (pc_ == module_address(0x8b7cU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x24000008U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            // Unknown/dead flag id: kernel error, caller survives (see
            // WaitEventFlag).
            if (gpr_[4U] == 0U || gpr_[4U] > event_flags_.size() ||
                !event_flags_[gpr_[4U] - 1U].live) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            event_flags_[gpr_[4U] - 1U].bits &= ~gpr_[5U];
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // thevent ordinals 10/11, WaitEventFlag / iWaitEventFlag
        // (evid, pattern, mode, out_bits). IOP thevent modes: 0 = WEF_OR,
        // 1 = WEF_AND, bit 1 (0x2) = WEF_CLEAR (drop the waited-for bits once
        // satisfied). An immediately satisfied wait completes inline;
        // otherwise the calling thread blocks exactly like WaitSema until a
        // SetEventFlag wakes it. Retail evidence: the worker loop at module
        // 0x66c4 waits with mode 0, the opcode-0x05 DMA path waits with
        // mode 1.
        if (pc_ == module_address(0x8b84U) || pc_ == module_address(0x8b8cU)) {
            std::uint32_t first{}, second{}, expected{};
            if (pc_ == module_address(0x8b84U)) {
                expected = 0x2400000aU;
            } else {
                expected = 0x2400000bU;
            }
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto pattern = gpr_[5U];
            const auto mode = gpr_[6U];
            if (first != 0x03e00008U || second != expected)
                return {StopReason::UnsupportedInstruction, pc_, second};
            // Unknown/dead flag id or bad mode is a kernel error the retail
            // caller survives; stopping the shadow here starves retail
            // streaming. Flag ids the model never allocated mean an earlier
            // modeling gap (e.g. a skipped descriptor mirror), not a fault.
            if (gpr_[4U] == 0U || gpr_[4U] > event_flags_.size() ||
                !event_flags_[gpr_[4U] - 1U].live || mode > 3U ||
                current_thread_id_ < 2U || current_thread_id_ - 2U >= threads_.size()) {
                gpr_[2U] = static_cast<std::uint32_t>(-1); // KE_ERROR
                pc_ = gpr_[31U];
                ++clock_ticks_;
                return {};
            }
            auto& event = event_flags_[gpr_[4U] - 1U];
            const auto satisfied = (mode & 1U) == 0U
                                       ? (event.bits & pattern) != 0U
                                       : (event.bits & pattern) == pattern;
            if (!satisfied) {
                auto& current = threads_[current_thread_id_ - 2U];
                gpr_[2U] = 0U;
                pc_ = gpr_[31U];
                ++clock_ticks_;
                current.registers = gpr_;
                current.pc = pc_;
                current.runnable = false;
                current.waiting_event_flag = gpr_[4U];
                current.waiting_event_pattern = pattern;
                current.waiting_event_mode = mode;
                current.waiting_event_out = gpr_[7U];
                if (!schedule_next_runnable()) return {StopReason::Halted, pc_, second};
                return {};
            }
            if (gpr_[7U] != 0U && !write_word(gpr_[7U], event.bits))
                return {StopReason::MemoryFault, pc_, second};
            if (mode & 2U) event.bits &= ~pattern;
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // libsd ordinals 26/28: sceSdSetTransIntrHandler and
        // sceSdGetTransIntrHandlerArgument. RWA saves the old argument before
        // installing its per-core DMA completion callback.
        if (pc_ == module_address(0x8c6cU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400001cU || gpr_[4U] >= 2U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            gpr_[2U] = transfer_handler_arguments_[gpr_[4U]];
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8c5cU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400001aU || gpr_[4U] >= 2U ||
                (gpr_[5U] != 0U && gpr_[5U] >= memory_.size()))
                return {StopReason::UnsupportedInstruction, pc_, second};
            const auto old = transfer_handlers_[gpr_[4U]];
            transfer_handlers_[gpr_[4U]] = gpr_[5U];
            transfer_handler_arguments_[gpr_[4U]] = gpr_[6U];
            gpr_[2U] = old;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8c74U)) { // sceSdGetSpu2IntrHandlerArgument
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400001dU)
                return {StopReason::UnsupportedInstruction, pc_, first};
            gpr_[2U] = spu2_interrupt_argument_;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8c64U)) { // sceSdSetSpu2IntrHandler
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400001bU ||
                (gpr_[4U] != 0U && gpr_[4U] >= memory_.size()))
                return {StopReason::UnsupportedInstruction, pc_, second};
            const auto old = spu2_interrupt_handler_;
            spu2_interrupt_handler_ = gpr_[4U];
            spu2_interrupt_argument_ = gpr_[5U];
            gpr_[2U] = old;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        if (pc_ == module_address(0x8c34U)) { // sceSdSetCoreAttr
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            if (first != 0x03e00008U || second != 0x2400000bU)
                return {StopReason::UnsupportedInstruction, pc_, first};
            const auto entry = static_cast<std::uint16_t>(gpr_[4U]);
            const auto value = static_cast<std::uint16_t>(gpr_[5U]);
            bool replaced = false;
            for (auto& attribute : core_attributes_) {
                if (attribute.entry == entry) { attribute.value = value; replaced = true; break; }
            }
            if (!replaced) core_attributes_.push_back({entry, value});
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // Exact retail libsd voice-register imports. These calls are routed to
        // the shared SPU2 voice state only when explicitly attached.
        if (pc_ == module_address(0x8c04U) ||
            pc_ == module_address(0x8c14U) ||
            pc_ == module_address(0x8c24U)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto ordinal = pc_ == module_address(0x8c04U)
                                     ? 5U
                                     : pc_ == module_address(0x8c14U) ? 7U : 9U;
            if (first != 0x03e00008U || second != 0x24000000U + ordinal)
                return {StopReason::UnsupportedInstruction, pc_, second};
            const auto entry = static_cast<std::uint16_t>(gpr_[4U]);
            if (ordinal == 5U) {
                parameter_registers_.push_back({entry, static_cast<std::uint16_t>(gpr_[5U])});
                if (voices_ != nullptr) (void)voices_->set_param(entry, static_cast<std::uint16_t>(gpr_[5U]));
            } else if (ordinal == 7U) {
                switch_registers_.push_back({entry, gpr_[5U]});
                if (voices_ != nullptr) (void)voices_->set_switch(entry, gpr_[5U]);
            } else {
                address_registers_.push_back({entry, gpr_[5U]});
                if (voices_ != nullptr) (void)voices_->set_addr(entry, gpr_[5U]);
            }
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // libsd ordinals 6/8/10, sceSdGetParam/GetSwitch/GetAddr: the read
        // side of the same tracked SPU2 state the write imports feed. The
        // streaming path polls these; an unmodeled read stopped the whole
        // command window and starved the ring consumer.
        if (pc_ == module_address(0x8c0cU) || pc_ == module_address(0x8c1cU) ||
            pc_ == module_address(0x8c2cU)) {
            std::uint32_t first{}, second{};
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto ordinal = pc_ == module_address(0x8c0cU)
                                     ? 6U
                                     : pc_ == module_address(0x8c1cU) ? 8U : 10U;
            if (first != 0x03e00008U || second != 0x24000000U + ordinal)
                return {StopReason::UnsupportedInstruction, pc_, second};
            const auto entry = static_cast<std::uint16_t>(gpr_[4U]);
            if (ordinal == 6U) {
                gpr_[2U] = voices_ != nullptr
                               ? static_cast<std::uint32_t>(voices_->get_param(entry))
                               : 0U;
            } else if (ordinal == 8U) {
                gpr_[2U] = voices_ != nullptr ? voices_->get_switch(entry) : 0U;
            } else {
                gpr_[2U] = voices_ != nullptr ? voices_->get_addr(entry) : 0U;
            }
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // libsd ordinal 17, sceSdVoiceTrans(channel, mode, iop, spu, size).
        // Retail RWA reaches both write encodings: mode 0 from opcode 0x07's
        // bank-wave path and mode 8 from its explicit transfer path. Other
        // direction/async variants remain strict boundaries.
        if (pc_ == module_address(0x8c44U)) {
            std::uint32_t first{}, second{}, size{};
            const auto stack_argument64 = static_cast<std::uint64_t>(gpr_[29U]) + 16U;
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                stack_argument64 > std::numeric_limits<std::uint32_t>::max() ||
                !read_word(static_cast<std::uint32_t>(stack_argument64), size))
                return {StopReason::MemoryFault, pc_, 0U};
            const auto iop = gpr_[6U];
            const auto spu = gpr_[7U];
            if (first != 0x03e00008U || second != 0x24000011U ||
                gpr_[4U] >= 2U || (gpr_[5U] != 0U && gpr_[5U] != 8U) || size == 0U ||
                iop > memory_.size() || memory_.size() - iop < size ||
                spu > spu2_ram_.size() || spu2_ram_.size() - spu < size)
                return {StopReason::UnsupportedInstruction, pc_, second};
            for (std::uint32_t index = 0; index < size; ++index)
                spu2_ram_[spu + index] = memory_[iop + index];
            voice_transfers_.push_back({gpr_[4U], gpr_[5U], iop, spu, size});
            if (spu2_upload_sink_) spu2_upload_sink_(spu, iop, size);
            gpr_[2U] = 0U;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // sifcmd ordinal 12, sceSifSendCmd(cid, packet, packet_size,
        // extra_source, extra_destination, extra_size). This executor records
        // the exact outbound transfer but never delivers or acknowledges it
        // to an EE peer; the host HLE remains the sole completion authority.
        if (pc_ == module_address(0x89b8U)) {
            std::uint32_t first{}, second{}, extra_destination{}, extra_size{};
            const auto stack64 = static_cast<std::uint64_t>(gpr_[29U]) + 16U;
            if (!read_word(pc_, first) || !read_word(pc_ + 4U, second) ||
                stack64 > std::numeric_limits<std::uint32_t>::max() - 4U ||
                !read_word(static_cast<std::uint32_t>(stack64), extra_destination) ||
                !read_word(static_cast<std::uint32_t>(stack64 + 4U), extra_size))
                return {StopReason::MemoryFault, pc_, 0U};
            constexpr std::uint32_t maximum_packet = 0x1000U;
            constexpr std::uint32_t maximum_extra = 0x100000U;
            const auto packet_address = gpr_[5U];
            const auto packet_size = gpr_[6U];
            const auto extra_source = gpr_[7U];
            if (first != 0x03e00008U || second != 0x2400000cU ||
                packet_size < 16U || packet_size > maximum_packet ||
                packet_address > memory_.size() || memory_.size() - packet_address < packet_size ||
                extra_size > maximum_extra ||
                (extra_size != 0U && (extra_source > memory_.size() ||
                 memory_.size() - extra_source < extra_size)) ||
                outbound_sif_commands_.size() >= 256U)
                return {StopReason::UnsupportedInstruction, pc_, second};
            OutboundSifCommand command{
                .id = next_outbound_sif_id_++, .command = gpr_[4U],
                .packet_address = packet_address, .packet_size = packet_size,
                .extra_source = extra_source, .extra_destination = extra_destination,
                .extra_size = extra_size};
            command.packet.resize(packet_size);
            for (std::uint32_t index = 0; index < packet_size; ++index)
                command.packet[index] = static_cast<std::byte>(memory_[packet_address + index]);
            command.extra_data.resize(extra_size);
            for (std::uint32_t index = 0; index < extra_size; ++index)
                command.extra_data[index] = static_cast<std::byte>(memory_[extra_source + index]);
            gpr_[2U] = static_cast<std::uint32_t>(command.id);
            outbound_sif_commands_.push_back(std::move(command));
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return {};
        }
        // An unresolved IRX import stub is an execution boundary, not normal
        // code.  Never accidentally "execute" its ordinal load as though the
        // imported kernel function had returned that ordinal.
        std::uint32_t import_first{};
        std::uint32_t import_second{};
        if (read_word(pc_, import_first) && read_word(pc_ + 4U, import_second) &&
            import_first == 0x03e00008U && (import_second & 0xffff0000U) == 0x24000000U)
            return {StopReason::UnsupportedInstruction, pc_, import_second};
        const auto instruction_pc = pc_;
        std::uint32_t word{};
        if (!read_word(pc_, word)) return {StopReason::MemoryFault, pc_, 0U};
        const bool apply_pending = in_delay_slot_;
        const auto target_after_delay = pending_target_;
        in_delay_slot_ = false;
        pc_ += 4U;
        const auto op = word >> 26U;
        const auto rs = (word >> 21U) & 31U;
        const auto rt = (word >> 16U) & 31U;
        const auto immediate = static_cast<std::int16_t>(word & 0xffffU);
        bool supported = true;
        if (word == 0U) {
        } else if (op == 0U && (word & 0x3fU) == 0x21U) { // addu
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] + gpr_[rt];
        } else if (op == 0U && (word & 0x3fU) == 0x23U) { // subu
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] - gpr_[rt];
        } else if (op == 0U && (word & 0x3fU) == 0x2bU) { // sltu
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] < gpr_[rt] ? 1U : 0U;
        } else if (op == 0U && (word & 0x3fU) == 0x2aU) { // slt
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = static_cast<std::int32_t>(gpr_[rs]) < static_cast<std::int32_t>(gpr_[rt]) ? 1U : 0U;
        } else if (op == 0U && (word & 0x3fU) == 0x27U) { // nor
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = ~(gpr_[rs] | gpr_[rt]);
        } else if (op == 0U && (word & 0x3fU) == 0x24U) { // and
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] & gpr_[rt];
        } else if (op == 0U && (word & 0x3fU) == 0x25U) { // or
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] | gpr_[rt];
        } else if (op == 0U && (word & 0x3fU) == 0x26U) { // xor
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rs] ^ gpr_[rt];
        } else if (op == 0U && (word & 0x3fU) == 0x18U) { // mult
            const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(gpr_[rs])) *
                                 static_cast<std::int64_t>(static_cast<std::int32_t>(gpr_[rt]));
            lo_ = static_cast<std::uint32_t>(product);
            hi_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32U);
        } else if (op == 0U && (word & 0x3fU) == 0x1aU) { // div
            const auto dividend = static_cast<std::int32_t>(gpr_[rs]);
            const auto divisor = static_cast<std::int32_t>(gpr_[rt]);
            if (divisor == 0) {
                lo_ = dividend >= 0 ? 0xffffffffU : 1U;
                hi_ = static_cast<std::uint32_t>(dividend);
            } else if (dividend == std::numeric_limits<std::int32_t>::min() && divisor == -1) {
                lo_ = static_cast<std::uint32_t>(dividend);
                hi_ = 0U;
            } else {
                lo_ = static_cast<std::uint32_t>(dividend / divisor);
                hi_ = static_cast<std::uint32_t>(dividend % divisor);
            }
        } else if (op == 0U && (word & 0x3fU) == 0x1bU) { // divu
            const auto dividend = gpr_[rs];
            const auto divisor = gpr_[rt];
            if (divisor == 0U) {
                lo_ = 0xffffffffU;
                hi_ = dividend;
            } else {
                lo_ = dividend / divisor;
                hi_ = dividend % divisor;
            }
        } else if (op == 0U && (word & 0x3fU) == 0x12U) { // mflo
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = lo_;
        } else if (op == 0U && (word & 0x3fU) == 0x10U) { // mfhi
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = hi_;
        } else if (op == 0U && (word & 0x3fU) == 0x00U) { // sll
            const auto rd = (word >> 11U) & 31U;
            const auto shift = (word >> 6U) & 31U;
            gpr_[rd] = gpr_[rt] << shift;
        } else if (op == 0U && (word & 0x3fU) == 0x02U) { // srl
            const auto rd = (word >> 11U) & 31U;
            const auto shift = (word >> 6U) & 31U;
            gpr_[rd] = gpr_[rt] >> shift;
        } else if (op == 0U && (word & 0x3fU) == 0x03U) { // sra
            const auto rd = (word >> 11U) & 31U;
            const auto shift = (word >> 6U) & 31U;
            gpr_[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(gpr_[rt]) >> shift);
        } else if (op == 0U && (word & 0x3fU) == 0x04U) { // sllv
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rt] << (gpr_[rs] & 31U);
        } else if (op == 0U && (word & 0x3fU) == 0x06U) { // srlv
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = gpr_[rt] >> (gpr_[rs] & 31U);
        } else if (op == 0U && (word & 0x3fU) == 0x07U) { // srav
            const auto rd = (word >> 11U) & 31U;
            gpr_[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(gpr_[rt]) >>
                                                  (gpr_[rs] & 31U));
        } else if (op == 0x09U) { // addiu
            gpr_[rt] = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
        } else if (op == 0x0fU) { // lui
            gpr_[rt] = static_cast<std::uint32_t>(word & 0xffffU) << 16U;
        } else if (op == 0x0dU) { // ori
            gpr_[rt] = gpr_[rs] | (word & 0xffffU);
        } else if (op == 0x0eU) { // xori
            gpr_[rt] = gpr_[rs] ^ (word & 0xffffU);
        } else if (op == 0x0cU) { // andi
            gpr_[rt] = gpr_[rs] & (word & 0xffffU);
        } else if (op == 0x0bU) { // sltiu (sign-extended immediate)
            gpr_[rt] = gpr_[rs] < static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
        } else if (op == 0x0aU) { // slti
            gpr_[rt] = static_cast<std::int32_t>(gpr_[rs]) < static_cast<std::int32_t>(immediate)
                           ? 1U : 0U;
        } else if (op == 0x2bU) { // sw
            // Retail IOP data-access semantics (HARD RULE 6): the IOP has no
            // MMU — a store whose address leaves the 2 MiB RAM window goes
            // to the expansion/hardware space and is silently dropped, never
            // faulting. Unmapped-store/load counters keep the evidence.
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (!write_word(address, gpr_[rt])) ++unmapped_data_stores_;
            supported = true;
            if (address == 0x73124U) diag_word_writes_.push_back({instruction_pc, gpr_[rt]});
        } else if (op == 0x29U) { // sh
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address > memory_.size() || memory_.size() - address < 2U) {
                ++unmapped_data_stores_;
            } else {
                memory_[address] = static_cast<std::uint8_t>(gpr_[rt]);
                memory_[address + 1U] = static_cast<std::uint8_t>(gpr_[rt] >> 8U);
            }
        } else if (op == 0x28U) { // sb
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address >= memory_.size()) {
                ++unmapped_data_stores_;
            } else {
                memory_[address] = static_cast<std::uint8_t>(gpr_[rt]);
            }
        } else if (op == 0x23U) { // lw
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (!read_word(address, gpr_[rt])) {
                ++unmapped_data_loads_;
                gpr_[rt] = 0U; // open bus
            }
            supported = true;
        } else if (op == 0x25U) { // lhu
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address > memory_.size() || memory_.size() - address < 2U) {
                ++unmapped_data_loads_;
                gpr_[rt] = 0U;
            } else {
                gpr_[rt] = static_cast<std::uint32_t>(memory_[address]) |
                           (static_cast<std::uint32_t>(memory_[address + 1U]) << 8U);
            }
        } else if (op == 0x21U) { // lh
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address > memory_.size() || memory_.size() - address < 2U) {
                ++unmapped_data_loads_;
                gpr_[rt] = 0U;
            } else {
                const auto value = static_cast<std::uint16_t>(memory_[address]) |
                                   static_cast<std::uint16_t>(memory_[address + 1U] << 8U);
                gpr_[rt] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int16_t>(value)));
            }
        } else if (op == 0x20U) { // lb
            const auto address = gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address >= memory_.size()) {
                ++unmapped_data_loads_;
                gpr_[rt] = 0U;
            } else {
                gpr_[rt] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(memory_[address])));
            }
        } else if (op == 0x24U) { // lbu
            const auto address =
                gpr_[rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate));
            if (address >= memory_.size()) {
                ++unmapped_data_loads_;
                gpr_[rt] = 0U;
            } else {
                gpr_[rt] = memory_[address];
            }
        } else if (op == 0x03U) { // jal
            gpr_[31U] = pc_ + 4U;
            pending_target_ = (pc_ & 0xf0000000U) | ((word & 0x03ffffffU) << 2U);
            in_delay_slot_ = true;
        } else if (op == 0x02U) { // j
            pending_target_ = (pc_ & 0xf0000000U) | ((word & 0x03ffffffU) << 2U);
            in_delay_slot_ = true;
        } else if (op == 0x01U && rt == 0U) { // bltz
            if (static_cast<std::int32_t>(gpr_[rs]) < 0) {
                pending_target_ = pc_ + (static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate)) << 2U);
                in_delay_slot_ = true;
            }
        } else if (op == 0x01U && rt == 1U) { // bgez
            if (static_cast<std::int32_t>(gpr_[rs]) >= 0) {
                pending_target_ = pc_ + (static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate)) << 2U);
                in_delay_slot_ = true;
            }
        } else if (op == 0x07U) { // bgtz
            if (static_cast<std::int32_t>(gpr_[rs]) > 0) {
                pending_target_ = pc_ + (static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate)) << 2U);
                in_delay_slot_ = true;
            }
        } else if (op == 0x06U) { // blez
            if (static_cast<std::int32_t>(gpr_[rs]) <= 0) {
                pending_target_ = pc_ + (static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate)) << 2U);
                in_delay_slot_ = true;
            }
        } else if (op == 0x04U || op == 0x05U) { // beq / bne
            const bool taken = op == 0x04U ? gpr_[rs] == gpr_[rt] : gpr_[rs] != gpr_[rt];
            if (taken) {
                pending_target_ = pc_ +
                                  (static_cast<std::uint32_t>(static_cast<std::int32_t>(immediate)) << 2U);
                in_delay_slot_ = true;
            }
        } else if (op == 0U && (word & 0x3fU) == 0x08U) { // jr
            pending_target_ = gpr_[rs];
            in_delay_slot_ = true;
        } else if (op == 0U && (word & 0x3fU) == 0x09U) { // jalr
            const auto rd = (word >> 11U) & 31U;
            const auto target = gpr_[rs];
            gpr_[rd] = pc_ + 4U;
            pending_target_ = target;
            in_delay_slot_ = true;
        } else {
            supported = false;
        }
        gpr_[0U] = 0U;
        if (!supported) return {StopReason::UnsupportedInstruction, instruction_pc, word};
        if (apply_pending) pc_ = target_after_delay;
        ++clock_ticks_;
        return {};
    }

    std::array<std::uint32_t, 32>& gpr() { return gpr_; }
    [[nodiscard]] std::uint32_t pc() const { return pc_; }
    // Test seam: the retail-driven paths never need external PC injection,
    // but stub-level unit tests drive individual imports directly.
    void set_pc(std::uint32_t pc) { pc_ = pc; }
    [[nodiscard]] std::uint32_t module_base() const { return module_base_; }
    [[nodiscard]] std::uint32_t hi() const { return hi_; }
    [[nodiscard]] std::uint32_t lo() const { return lo_; }
    [[nodiscard]] std::uint64_t enabled_irqs() const { return enabled_irqs_; }
    [[nodiscard]] std::uint64_t clock_ticks() const { return clock_ticks_; }
    [[nodiscard]] std::uint32_t current_thread_id() const { return current_thread_id_; }
    [[nodiscard]] const std::vector<Semaphore>& semaphores() const { return semaphores_; }
    [[nodiscard]] const std::vector<EventFlag>& event_flags() const { return event_flags_; }
    [[nodiscard]] const std::vector<Thread>& threads() const { return threads_; }
    [[nodiscard]] bool sifcmd_initialized() const { return sifcmd_initialized_; }
    [[nodiscard]] std::uint32_t sif_command_buffer() const { return sif_command_buffer_; }
    [[nodiscard]] const std::vector<SifCommandHandler>& sif_handlers() const { return sif_handlers_; }
    [[nodiscard]] LoadError load_error() const { return load_error_; }
    [[nodiscard]] bool inspect_word(std::uint32_t address, std::uint32_t& value) const {
        return read_word(address, value);
    }

  private:
    static constexpr std::uint64_t kNoWakeTick = ~std::uint64_t{0U};

    // Segment + relocation core shared by load_irx (fresh image) and
    // load_extension_irx (second retail module in the live image). Applies
    // the same validation and the same four relocation types; advances
    // next_sysmem_address_ past the image and reports the file-backed end.
    struct IrxImageResult {
        std::uint32_t image_end{};
        std::uint32_t file_end{};
        // End of the executable section (SHF_EXECINSTR PROGBITS): live code
        // only. Retail games DMA over a module's .data work areas (RWA's
        // stream objects land at module 0xb1a0, inside .data), so upload
        // protection must scope to code, never the whole file image.
        std::uint32_t text_end{};
    };
    bool load_irx_image(std::span<const std::byte> elf, std::uint32_t load_base,
                        IrxImageResult& out) {
        if (elf.size() < 52U || read_elf_u32(elf, 0U) != 0x464c457fU ||
            std::to_integer<std::uint8_t>(elf[4U]) != 1U ||
            std::to_integer<std::uint8_t>(elf[5U]) != 1U || read_elf_u16(elf, 18U) != 8U) return false;
        const auto phoff = read_elf_u32(elf, 28U);
        const auto shoff = read_elf_u32(elf, 32U);
        const auto phentsize = read_elf_u16(elf, 42U);
        const auto phnum = read_elf_u16(elf, 44U);
        const auto shentsize = read_elf_u16(elf, 46U);
        const auto shnum = read_elf_u16(elf, 48U);
        bool loaded = false;
        std::uint32_t image_end = next_sysmem_address_;
        std::uint32_t file_end = 0U;
        for (std::uint32_t i = 0; i < phnum; ++i) {
            const auto at64 = static_cast<std::uint64_t>(phoff) +
                              static_cast<std::uint64_t>(i) * phentsize;
            if (at64 > elf.size() || elf.size() - static_cast<std::size_t>(at64) < 32U) return false;
            const auto at = static_cast<std::size_t>(at64);
            if (read_elf_u32(elf, at) != 1U) continue;
            const auto offset = read_elf_u32(elf, at + 4U);
            const auto vaddr64 = static_cast<std::uint64_t>(read_elf_u32(elf, at + 8U)) + load_base;
            if (vaddr64 > std::numeric_limits<std::uint32_t>::max()) return false;
            const auto vaddr = static_cast<std::uint32_t>(vaddr64);
            const auto filesz = read_elf_u32(elf, at + 16U);
            const auto memsz = read_elf_u32(elf, at + 20U);
            if (filesz > memsz || offset > elf.size() || elf.size() - offset < filesz ||
                vaddr > memory_.size() || memory_.size() - vaddr < memsz) return false;
            for (std::uint32_t byte = 0; byte < filesz; ++byte)
                memory_[vaddr + byte] = std::to_integer<std::uint8_t>(elf[offset + byte]);
            const auto segment_end = vaddr + memsz;
            if (segment_end > image_end) image_end = segment_end;
            const auto segment_file_end = vaddr + filesz;
            if (segment_file_end > file_end) file_end = segment_file_end;
            loaded = true;
        }
        if (!loaded) return false;
        std::vector<std::uint32_t> pending_hi;
        for (std::uint32_t i = 0; i < shnum; ++i) {
            const auto section64 = static_cast<std::uint64_t>(shoff) +
                                   static_cast<std::uint64_t>(i) * shentsize;
            if (section64 > elf.size() || elf.size() - static_cast<std::size_t>(section64) < 40U) return false;
            const auto section = static_cast<std::size_t>(section64);
            if (read_elf_u32(elf, section + 4U) != 9U) continue;
            const auto offset = read_elf_u32(elf, section + 16U);
            const auto size = read_elf_u32(elf, section + 20U);
            const auto entsize = read_elf_u32(elf, section + 36U);
            if (entsize != 8U || offset > elf.size() || elf.size() - offset < size) return false;
            for (std::uint32_t rel = 0; rel < size; rel += 8U) {
                const auto target64 = static_cast<std::uint64_t>(load_base) +
                                      read_elf_u32(elf, static_cast<std::size_t>(offset) + rel);
                if (target64 > std::numeric_limits<std::uint32_t>::max()) return false;
                const auto target = static_cast<std::uint32_t>(target64);
                const auto type = read_elf_u32(elf, offset + rel + 4U) & 0xffU;
                std::uint32_t word{};
                if (!read_word(target, word)) return false;
                if (type == 2U) {
                    const auto value = static_cast<std::uint64_t>(word) + load_base;
                    if (value > std::numeric_limits<std::uint32_t>::max() ||
                        !write_word(target, static_cast<std::uint32_t>(value))) return false;
                } else if (type == 4U) {
                    const auto address = (static_cast<std::uint64_t>(word & 0x03ffffffU) << 2U) + load_base;
                    if ((address & 3U) != 0U || (address >> 2U) > 0x03ffffffU) return false;
                    if (!write_word(target, (word & 0xfc000000U) |
                                              static_cast<std::uint32_t>(address >> 2U))) return false;
                } else if (type == 5U) {
                    pending_hi.push_back(target);
                } else if (type == 6U) {
                    const auto low = static_cast<std::int16_t>(word & 0xffffU);
                    const auto relocated_low = static_cast<std::int64_t>(low) + load_base;
                    if (!write_word(target, (word & 0xffff0000U) |
                                              static_cast<std::uint32_t>(
                                                  static_cast<std::uint64_t>(relocated_low) & 0xffffU))) return false;
                    for (const auto hi_target : pending_hi) {
                        std::uint32_t hi_word{};
                        if (!read_word(hi_target, hi_word)) return false;
                        const auto value = static_cast<std::int64_t>(hi_word & 0xffffU) * 0x10000LL +
                                           static_cast<std::int64_t>(low) + load_base;
                        if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) return false;
                        const auto rounded = static_cast<std::uint64_t>(value) + 0x8000U;
                        if ((rounded >> 16U) > 0xffffU ||
                            !write_word(hi_target, (hi_word & 0xffff0000U) |
                                                  static_cast<std::uint32_t>(rounded >> 16U))) return false;
                    }
                    pending_hi.clear();
                } else {
                    load_error_ = {target, type};
                    return false;
                }
            }
        }
        if (!pending_hi.empty()) { load_error_ = {pending_hi.front(), 5U}; return false; }
        std::uint32_t text_end = 0U;
        for (std::uint32_t i = 0; i < shnum; ++i) {
            const auto section = static_cast<std::size_t>(shoff) +
                                 static_cast<std::size_t>(i) * shentsize;
            if (shoff > elf.size() || elf.size() - shoff < static_cast<std::size_t>(shnum) * shentsize)
                break;
            if (read_elf_u32(elf, section + 4U) != 1U) continue; // PROGBITS
            if ((read_elf_u32(elf, section + 4U + 4U) & 0x4U) == 0U)
                continue; // not SHF_EXECINSTR
            const auto sec_size = read_elf_u32(elf, section + 16U + 4U);
            const auto sec_addr = read_elf_u32(elf, section + 8U + 4U);
            const auto end64 = static_cast<std::uint64_t>(load_base) + sec_addr + sec_size;
            if (end64 <= std::numeric_limits<std::uint32_t>::max())
                text_end = std::max<std::uint32_t>(text_end, static_cast<std::uint32_t>(end64));
        }
        next_sysmem_address_ = image_end;
        out.image_end = image_end;
        out.file_end = file_end;
        out.text_end = text_end;
        return true;
    }

    [[nodiscard]] std::uint32_t module_address(std::uint32_t offset) const {
        return module_base_ + offset;
    }
    [[nodiscard]] std::uint32_t extension_address(std::uint32_t offset) const {
        return extension_base_ + offset;
    }

    // The import-stub region of the extension image: the SONY inline import
    // tables occupy the image start; the first real code (module entry) is
    // at offset 0x224 (GTFSCDVD, entry decoded at retail base 0x78c30).
    static constexpr std::uint32_t kExtensionStubRegionEnd = 0x224U;

    // Shared kernel-service imports: GTFSCDVD stub offset -> RWA stub
    // offset (same module version, same ordinal, therefore the same service;
    // the existing handlers validate the stub words at the translated
    // address, whose ordinal halfword is identical). Decoded from the retail
    // import tables (docs/archive/AUDIO_HISTORY.md section 8, increment 8).
    struct ExtensionSharedStub {
        std::uint32_t extension_offset;
        std::uint32_t primary_offset;
    };
    static constexpr std::array<ExtensionSharedStub, 19> kExtensionSharedStubs{{
        // sysmem 1.1: AllocSysMemory(4), Kprintf(14)
        {0x014U, 0x88e4U}, {0x01cU, 0x88fcU},
        // intrman 1.2: CpuSuspendIntr(17), CpuResumeIntr(18)
        {0x040U, 0x8928U}, {0x048U, 0x8930U},
        // sifcmd 1.1: AddCmdHandler(10), sceSifSendCmd(12)
        {0x06cU, 0x89a8U}, {0x074U, 0x89b8U},
        // sysclib 1.4: memcpy(12)
        {0x0f4U, 0x8a44U},
        // thbase 1.2: CreateThread(4), StartThread(6), GetThreadId(20),
        // SleepThread(24), DelayThread(33)
        {0x138U, 0x8ad0U}, {0x140U, 0x8ae0U}, {0x148U, 0x8b00U},
        {0x150U, 0x8b08U}, {0x160U, 0x8b20U},
        // thevent 1.1: CreateEventFlag(4), SetEventFlag(6),
        // ClearEventFlag(8), WaitEventFlag(10)
        {0x184U, 0x8b64U}, {0x18cU, 0x8b74U}, {0x194U, 0x8b7cU},
        {0x19cU, 0x8b84U},
        // thsemap 1.1: CreateSema(4), SignalSema(6), WaitSema(8)
        {0x1c0U, 0x8bb0U}, {0x1c8U, 0x8bc0U}, {0x1d0U, 0x8bd0U},
    }};

    [[nodiscard]] static std::optional<std::uint32_t> extension_shared_stub_translation(
        std::uint32_t extension_offset) {
        for (const auto& stub : kExtensionSharedStubs) {
            if (stub.extension_offset == extension_offset) return stub.primary_offset;
        }
        return std::nullopt;
    }

    // Extension-only import services (GTFSCDVD), dispatched by stub offset
    // with the actual stub ordinal validated. Semantics derived from the
    // retail disassembly (docs/archive/AUDIO_HISTORY.md section 8, increment 8):
    // - cdvdman 1.1: sceCdRead(6) is served synchronously by the wired disc
    //   sector source; Sync(11) then always reports complete, GetError(8)
    //   reports no error, GetDiskType(12) reports 2 (the polled >=2 gate),
    //   DiskReady(13) reports 6.
    // - sifcmd 1.1 RPC setup: InitRpc(14)/RegisterRpc(17)/SetRpcQueue(19)
    //   succeed with no extra modeled state (the runtime synthesizes the
    //   RPC traffic itself); RpcMain(22) parks the calling server thread
    //   forever, exactly like the retail blocking loop with no RPC traffic.
    // - sysclib 1.1 strings: strlen(6)/strcmp(20)/strncmp(27).
    // - sifman (7/8/32) and thbase ordinal 26 are on paths the RWA audio
    //   path never exercises (verified by disassembly): strict stop with
    //   the stub word as evidence if they ever execute.
    Stop step_extension_import(std::uint32_t offset, std::uint32_t ordinal) {
        const auto complete = [&](std::uint32_t value) {
            gpr_[2U] = value;
            pc_ = gpr_[31U];
            ++clock_ticks_;
            return Stop{};
        };
        const auto unsupported = [&] {
            return Stop{StopReason::UnsupportedInstruction, pc_,
                        0x24000000U | ordinal};
        };
        // Bounded copy helpers for the sysclib string stubs (RE-confirmed
        // ABIs: 20 = strcat, 23 = strcpy — docs/archive/AUDIO_HISTORY.md section 8,
        // increment 8). Copies stop at the NUL or at the end of IOP RAM.
        auto str_copy = [&](std::uint32_t dst, std::uint32_t src) {
            std::uint32_t i = 0U;
            for (; src + i < memory_.size() && dst + i < memory_.size(); ++i) {
                const auto byte = memory_[src + i];
                memory_[dst + i] = byte;
                if (byte == 0U) break;
            }
            return dst + (i == 0U && src < memory_.size() &&
                                  memory_[src] == 0U
                              ? 0U
                              : i);
        };
        auto complete_str_copy = [&](std::uint32_t dst, std::uint32_t src) {
            return complete(str_copy(dst, src));
        };
        auto complete_str_cat = [&](std::uint32_t dst, std::uint32_t src) {
            std::uint32_t len = 0U;
            while (dst + len < memory_.size() && memory_[dst + len] != 0U) ++len;
            return complete(str_copy(dst + len, src));
        };
        switch (offset) {
        case 0x07cU: // sifcmd 14, sceSifInitRpc
            return ordinal == 14U ? complete(0U) : unsupported();
        case 0x084U: { // sifcmd 17, sceSifRegisterRpc(sd, sid, func, buf, ...)
            if (ordinal != 17U) return unsupported();
            rpc_dispatch_function_ = gpr_[6U];
            rpc_request_buffer_ = gpr_[7U];
            return complete(0U);
        }
        case 0x08cU: { // sifcmd 19, sceSifSetRpcQueue(queue, thread_id)
            if (ordinal != 19U) return unsupported();
            rpc_queue_address_ = gpr_[4U];
            rpc_queue_thread_ = gpr_[5U];
            return complete(0U);
        }
        case 0x094U: { // sifcmd 22, sceSifRpcMain(queue): kernel blocking
            // server loop. The thread parks on the queue; the modeled RPC
            // delivery (deliver_rpc_call) resumes it AT the retail dispatch
            // with the request ABI, exactly the kernel-service behavior
            // sceSifRpcMain would perform.
            if (ordinal != 22U) return unsupported();
            if (current_thread_id_ < 2U ||
                current_thread_id_ - 2U >= threads_.size())
                return unsupported();
            auto& current = threads_[current_thread_id_ - 2U];
            gpr_[2U] = 0U;
            ++clock_ticks_;
            current.registers = gpr_;
            current.pc = pc_;
            current.runnable = false;
            current.waiting_rpc_queue = gpr_[4U];
            if (!schedule_next_runnable()) return {StopReason::Halted, pc_, 0U};
            return {};
        }
        case 0x0ecU: { // sysclib 6, toupper(a0): the ISO9660 name matcher
            // folds the requested path char-by-char through this stub
            // (call sites 0x1198-0x11c0 store each result byte back).
            if (ordinal != 6U) return unsupported();
            const auto c = gpr_[4U] & 0xffU;
            return complete(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        case 0x0fcU: // sysclib 20, strcat(dst, src): call site 0x113c appends
            // the requested name, 0x1180 appends the ";1" rodata constant
            return ordinal == 20U ? complete_str_cat(gpr_[4U], gpr_[5U])
                                  : unsupported();
        case 0x104U: { // sysclib 22, strcmp(a0, a1): call site 0x18ec tests
            // the result with bnez (strings differ)
            if (ordinal != 22U) return unsupported();
            for (std::uint32_t i = 0;; ++i) {
                const auto a =
                    gpr_[4U] + i < memory_.size() ? memory_[gpr_[4U] + i] : std::uint8_t{0U};
                const auto b =
                    gpr_[5U] + i < memory_.size() ? memory_[gpr_[5U] + i] : std::uint8_t{0U};
                if (a != b) return complete(a < b ? ~std::uint32_t{0U} : 1U);
                if (a == 0U) return complete(0U);
            }
        }
        case 0x10cU: // sysclib 23, strcpy(dst, src): call sites 0x1128/
            // 0x15a4/0x169c/0x18d4 copy short name constants into buffers
            // and ignore the result (strncpy-with-length would be
            // indistinguishable at these sites; the constants are
            // NUL-terminated shorts)
            return ordinal == 23U ? complete_str_copy(gpr_[4U], gpr_[5U])
                                  : unsupported();
        case 0x114U: // sysclib 27, strlen(a0): call sites 0x1148/0x1164 use
            // result+string as the end pointer for the ";1" suffix check
            if (ordinal != 27U) return unsupported();
            {
                std::uint32_t length = 0U;
                while (gpr_[4U] + length < memory_.size() &&
                       memory_[gpr_[4U] + length] != 0U)
                    ++length;
                return complete(length);
            }
        case 0x0b8U: { // sifman 7, sceSifSetDma(list, count): the outbound
            // SIF DMA convention (same as sifcmd 12 SendCmd): the host HLE
            // is the completion authority, so the id is recorded and
            // immediately complete; the live open/read path polls
            // sceSifDmaStat on this id (GTFS BSS 0x9294).
            if (ordinal != 7U) return unsupported();
            ++sif_dma_sequence_;
            return complete(0xb0c50000U | (sif_dma_sequence_ & 0xffffU));
        }
        case 0x0c0U: // sifman 8, sceSifDmaStat: <0 = transfer complete
            return ordinal == 8U ? complete(~std::uint32_t{0U}) : unsupported();
        case 0x0c8U: { // sifman 32, sceSifSetDmaWithCompletion(sdt, 1, cb,
            // arg): retail cb = iWakeupThread(arg) — the DMA completes and
            // wakes the sleeping flush worker.
            if (ordinal != 32U) return unsupported();
            ++sif_dma_sequence_;
            const auto id = 0xb0c50000U | (sif_dma_sequence_ & 0xffffU);
            const auto wake_id = gpr_[7U];
            if (wake_id >= 2U && wake_id - 2U < threads_.size() &&
                threads_[wake_id - 2U].waiting_wakeup) {
                threads_[wake_id - 2U].waiting_wakeup = false;
                threads_[wake_id - 2U].runnable = true;
            }
            return complete(id);
        }
        case 0x158U: { // thbase 26, iWakeupThread(a0): the sifman-32
            // completion callback target (retail stub 0xc84 jumps here).
            if (ordinal != 26U) return unsupported();
            const auto wake_id = gpr_[4U];
            if (wake_id < 2U || wake_id - 2U >= threads_.size() ||
                !threads_[wake_id - 2U].waiting_wakeup)
                return complete(static_cast<std::uint32_t>(
                    -426)); // KE_UNKNOWN_THID
            threads_[wake_id - 2U].waiting_wakeup = false;
            threads_[wake_id - 2U].runnable = true;
            return complete(wake_id);
        }
        case 0x1f4U: { // cdvdman 6, sceCdRead(lbn, sectors, buf, cb)
            if (ordinal != 6U) return unsupported();
            const auto lbn = gpr_[4U];
            const auto sectors = gpr_[5U];
            const auto destination = gpr_[6U];
            bool ok = disc_sector_source_ && sectors != 0U &&
                      sectors <= memory_.size() / 2048U &&
                      destination <= memory_.size() &&
                      static_cast<std::uint64_t>(sectors) * 2048U <=
                          memory_.size() - destination;
            if (ok) {
                scratch_sectors_.resize(static_cast<std::size_t>(sectors) * 2048U);
                ok = disc_sector_source_(lbn, sectors, scratch_sectors_.data());
                for (std::size_t byte = 0; ok && byte < scratch_sectors_.size(); ++byte)
                    memory_[destination + byte] = std::to_integer<std::uint8_t>(
                        scratch_sectors_[byte]);
            }
            return complete(ok ? 1U : 0U);
        }
        case 0x1fcU: // cdvdman 8, GetError: no error after synchronous reads
            return ordinal == 8U ? complete(0U) : unsupported();
        case 0x204U: // cdvdman 11, sceCdSync: the read model is synchronous
            return ordinal == 11U ? complete(0U) : unsupported();
        case 0x20cU: // cdvdman 12, GetDiskType: 4 = DVD (the retail disc;
            // cmd-1 polls until type >= 2)
            return ordinal == 12U ? complete(4U) : unsupported();
        case 0x214U: // cdvdman 13, DiskReady: 6 == complete
            return ordinal == 13U ? complete(6U) : unsupported();
        default:
            (void)ordinal;
            return unsupported();
        }
    }

    bool read_word(std::uint32_t address, std::uint32_t& value) const {
        if (address > memory_.size() || memory_.size() - address < 4U) return false;
        value = memory_[address] | (static_cast<std::uint32_t>(memory_[address + 1U]) << 8U) |
                (static_cast<std::uint32_t>(memory_[address + 2U]) << 16U) |
                (static_cast<std::uint32_t>(memory_[address + 3U]) << 24U);
        return true;
    }
    void record_watch(std::uint32_t address, std::uint32_t value) {
        if (watch_ranges_.empty()) return;
        for (const auto& range : watch_ranges_) {
            if (address < range.address ||
                address >= range.address + range.words * 4U)
                continue;
            if (watched_writes_.size() < 4096U)
                watched_writes_.push_back(
                    {pc_, value, address - range.address});
            return;
        }
    }
    bool write_word(std::uint32_t address, std::uint32_t value) {
        record_watch(address, value);
        // First CPU store into the primary module's executable text: retail
        // never patches live code, so a store here names a model divergence
        // exactly (pc of the store, target, value).
        if (module_text_end_ != 0U && address >= module_base_ &&
            address < module_text_end_) {
            if (first_text_write_count_ == 0U) {
                first_text_write_ = {pc_, address, value};
            }
            ++first_text_write_count_;
        }
        if (address > memory_.size() || memory_.size() - address < 4U) return false;
        for (std::uint32_t i = 0; i < 4U; ++i) memory_[address + i] = static_cast<std::uint8_t>(value >> (i * 8U));
        return true;
    }
    static std::uint16_t read_elf_u16(std::span<const std::byte> bytes, std::size_t at) {
        if (at > bytes.size() || bytes.size() - at < 2U) return 0U;
        return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[at]) |
               (std::to_integer<std::uint16_t>(bytes[at + 1U]) << 8U));
    }
    static std::uint32_t read_elf_u32(std::span<const std::byte> bytes, std::size_t at) {
        if (at > bytes.size() || bytes.size() - at < 4U) return 0U;
        return std::to_integer<std::uint32_t>(bytes[at]) |
               (std::to_integer<std::uint32_t>(bytes[at + 1U]) << 8U) |
               (std::to_integer<std::uint32_t>(bytes[at + 2U]) << 16U) |
               (std::to_integer<std::uint32_t>(bytes[at + 3U]) << 24U);
    }
    std::vector<std::uint8_t> memory_{};
    std::array<std::uint32_t, 32> gpr_{};
    // Rolling executed-PC ring behind TAKEDOWN_RWA_SHADOW_DIAG for
    // unsupported-stop triage (which jalr chain reached the boundary).
    std::array<std::uint32_t, 32> pc_history_{};
    std::size_t pc_history_index_{};
    std::uint32_t hi_{};
    std::uint32_t lo_{};
    std::uint32_t pc_{};
    std::uint32_t module_base_{};
    std::uint32_t module_image_end_{};
    std::uint32_t module_file_end_{};
    // End of the executable section (see IrxImageResult::text_end): the
    // upload-protection scope for the primary module.
    std::uint32_t module_text_end_{};
    std::uint32_t module_file_end_candidate_{};
    // Second retail module living in the same IOP image (GTFSCDVD.IRX at
    // 0x78c30 next to RWA.IRX at 0x65130 — increment 8). Zero until
    // load_extension_irx succeeds.
    std::uint32_t extension_base_{};
    std::uint32_t extension_file_end_{};
    std::uint32_t extension_image_end_{};
    DiscSectorSource disc_sector_source_{};
    // The registered retail sifcmd RPC server (captured by the
    // sceSifRegisterRpc / sceSifSetRpcQueue extension handlers; see
    // deliver_rpc_call).
    std::uint32_t rpc_dispatch_function_{};
    std::uint32_t rpc_request_buffer_{};
    std::uint32_t rpc_queue_address_{};
    std::uint32_t rpc_queue_thread_{};
    bool rpc_dispatch_active_{};
    std::uint32_t sif_dma_sequence_{};
    struct WatchRange {
        std::uint32_t address;
        std::uint32_t words;
    };
    std::vector<WatchRange> watch_ranges_{};
    std::vector<WatchedWrite> watched_writes_{};
    TextWrite first_text_write_{};
    std::uint64_t first_text_write_count_{};
    // Retail no-MMU data-access telemetry (see the generic decoder's
    // load/store handlers).
    std::uint64_t unmapped_data_stores_{};
    std::uint64_t unmapped_data_loads_{};
    // Reusable byte buffer for synchronous sceCdRead copies (increment 8).
    std::vector<std::byte> scratch_sectors_{};
    // Parked sifcmd interrupt continuation (increment 8): a command handler
    // that blocks in WaitSema parks HERE, not in any thread's TCB — the
    // interrupted thread's state lives in interrupted_*. The matching
    // SignalSema/iSignalSema restores it (resume_parked_interrupt).
    bool interrupt_blocked_{};
    std::uint32_t interrupt_blocked_semaphore_{};
    std::array<std::uint32_t, 32> interrupt_blocked_gpr_{};
    std::uint32_t interrupt_blocked_pc_{};
    std::uint64_t module_upload_skips_{};
    std::pair<std::uint32_t, std::uint32_t> first_module_upload_skip_{};
    std::uint32_t pending_target_{};
    bool in_delay_slot_{};
    std::uint64_t enabled_irqs_{};
    bool cpu_interrupts_enabled_{true};
    std::uint64_t clock_ticks_{};
    // Master-clock bound for dispatch-time idle fast-forward (see
    // set_clock_ticks_ceiling); unlimited until a runtime window sets it.
    std::uint64_t clock_ticks_ceiling_{~std::uint64_t{0U}};
    // Latched when every schedulable context is blocked (see
    // dispatch_next_runnable): step() refuses to execute a blocked context's
    // saved continuation until a dispatch, delivery, or bootstrap call
    // makes something runnable again.
    bool halted_{};
    std::vector<Semaphore> semaphores_{};
    std::vector<Thread> threads_{};
    std::uint32_t next_thread_stack_top_{0x1f0000U};
    std::uint32_t current_thread_id_{1U};
    std::uint32_t current_thread_priority_{};
    bool sifcmd_initialized_{};
    bool spu2_initialized_{};
    std::uint32_t sif_command_buffer_{};
    std::uint32_t sif_command_buffer_count_{};
    std::vector<SifCommandHandler> sif_handlers_{};
    std::uint32_t sif_receive_address_{};
    bool interrupt_active_{};
    std::array<std::uint32_t, 32> interrupted_gpr_{};
    std::uint32_t interrupted_pc_{};
    std::uint32_t interrupted_thread_id_{};
    std::uint32_t next_sysmem_address_{};
    std::vector<Allocation> allocations_{};
    std::vector<EventFlag> event_flags_{};
    std::array<std::uint32_t, 2> transfer_handlers_{};
    std::array<std::uint32_t, 2> transfer_handler_arguments_{};
    std::uint32_t spu2_interrupt_handler_{};
    std::uint32_t spu2_interrupt_argument_{};
    std::vector<CoreAttribute> core_attributes_{};
    std::vector<CoreAttribute> parameter_registers_{};
    std::vector<AddressRegister> address_registers_{};
    std::vector<SwitchRegister> switch_registers_{};
    std::array<std::uint8_t, 2U * 1024U * 1024U> spu2_ram_{};
    Spu2UploadSink spu2_upload_sink_{};
    std::vector<VoiceTransfer> voice_transfers_{};
    std::vector<OutboundSifCommand> outbound_sif_commands_{};
    std::int32_t next_outbound_sif_id_{1};
    std::vector<VoiceSnapshotRecord> voice_snapshot_records_{};
    std::uint64_t voice_snapshot_count_{};
    RwaSpu2VoiceState* voices_{};
    LoadError load_error_{};
    std::vector<DiagWordWrite> diag_word_writes_{};
};

} // namespace takedown::audio
