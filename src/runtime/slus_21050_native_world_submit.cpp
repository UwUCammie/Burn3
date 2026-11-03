// P2 WP3: native SubmitWorldObject (original 0x0026af50) -- dispatcher glue. The decisions are in slus_21050_native_world_submit_core.cpp.
//
// The call is resolved in the limit resolver (read-only: the plan, including the mesh import into the host registry) and executed by the
// handler, which finds the plan in the state record. A declined call costs nothing: the guest runs the original code.
//
// Replace mode: culled objects return 0 (the guest writes nothing for them either); visible ones get the staging output described in the
// core header, a WorldSubmit in the ring, v0 = 1 and pc := ra. Shadow mode: the record goes to the shadow ring, the native executes the
// original's first instruction (`addiu sp, sp, -16`) and sets pc to the second, so the original code runs on unchanged; the registers
// and memory it sees are exactly the call's.

#include "takedown/runtime/slus_21050_native_world_submit.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/platform/native_world_submit.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"
#include "takedown/runtime/slus_21050_native_world_submit_core.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

namespace takedown::runtime::slus_21050 {

namespace {

namespace wc = world_submit_core;
namespace ws = platform::world_submit;
namespace nc = takedown::native_cmd;

constexpr std::uint64_t kCodeFnv = 0x37a8918c069be35dULL;   // FNV-1a 64 over the 612 bytes of the retail routine
constexpr std::uint32_t kCeiling = 1024U;

// ---- EeMemory behind the core's memory interface ----------------------------------------------------------------------------------------
[[nodiscard]] bool ee_mapped(const guest::EeMemory& m, const std::uint32_t address, const std::uint32_t bytes) noexcept {
    const std::uint64_t end = static_cast<std::uint64_t>(address) + bytes;
    return end <= m.size() || (address >= wc::kScratchpadBase && end <= wc::kScratchpadEnd);
}

class EeReadView : public wc::ReadView {
  public:
    explicit EeReadView(const guest::EeMemory& memory) noexcept
        : memory_(memory),
          ram_(reinterpret_cast<const std::uint8_t*>(memory.bytes().data()), memory.bytes().size()) {}
    [[nodiscard]] bool mapped(const std::uint32_t address, const std::uint32_t bytes) const noexcept override { return ee_mapped(memory_, address, bytes); }
    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t address) const override { return ee_mapped(memory_, address, 1U) ? memory_.read_u8(address) : std::uint8_t{0}; }
    [[nodiscard]] std::uint16_t read_u16(const std::uint32_t address) const override { return ee_mapped(memory_, address, 2U) ? memory_.read_u16_le(address) : std::uint16_t{0}; }
    [[nodiscard]] std::uint32_t read_u32(const std::uint32_t address) const override { return ee_mapped(memory_, address, 4U) ? memory_.read_u32_le(address) : 0U; }
    using wc::ReadView::read_u32;
    [[nodiscard]] bool read_qw(const std::uint32_t address, std::uint32_t out[4]) const noexcept override { return ram_.read_qw(address, out); }

  private:
    const guest::EeMemory& memory_;
    world_mesh::MainRamView ram_;
};

class EeGuestMemory final : public wc::GuestMemory {
  public:
    explicit EeGuestMemory(guest::EeMemory& memory) noexcept
        : memory_(memory),
          ram_(reinterpret_cast<const std::uint8_t*>(std::as_const(memory).bytes().data()), std::as_const(memory).bytes().size()) {}
    [[nodiscard]] bool mapped(const std::uint32_t address, const std::uint32_t bytes) const noexcept override { return ee_mapped(memory_, address, bytes); }
    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t address) const override { return ee_mapped(memory_, address, 1U) ? memory_.read_u8(address) : std::uint8_t{0}; }
    [[nodiscard]] std::uint16_t read_u16(const std::uint32_t address) const override { return ee_mapped(memory_, address, 2U) ? memory_.read_u16_le(address) : std::uint16_t{0}; }
    [[nodiscard]] std::uint32_t read_u32(const std::uint32_t address) const override { return ee_mapped(memory_, address, 4U) ? memory_.read_u32_le(address) : 0U; }
    using wc::GuestMemory::read_u32;
    [[nodiscard]] bool read_qw(const std::uint32_t address, std::uint32_t out[4]) const noexcept override { return ram_.read_qw(address, out); }
    void write_u8(const std::uint32_t address, const std::uint8_t value) override { memory_.write_u8(address, value); }
    void write_u32(const std::uint32_t address, const std::uint32_t value) override { memory_.write_u32_le(address, value); }
    void write_quad(const std::uint32_t address, const std::uint32_t (&words)[4]) override {
        memory_.write_u128_le(address, std::array<std::uint32_t, 4>{words[0], words[1], words[2], words[3]});
    }

  private:
    guest::EeMemory& memory_;
    world_mesh::MainRamView ram_;
};

// ---- state ----------------------------------------------------------------------------------------------------------------------------
struct Key {
    std::uint32_t sp{}, ra{}, ctx{}, record{};
    std::uint64_t clip{}, pass{};
    bool operator==(const Key&) const = default;
};

struct State {
    ws::Ring* ring{};
    bool shadow{};
    std::uint32_t decline_every{};
    std::uint64_t every_count{};
    wc::Producer producer;
    wc::Plan plan;
    Key key;
    bool have_plan{};
    std::uint64_t ticks{};
    std::chrono::steady_clock::time_point last_frame{std::chrono::steady_clock::now()};
    std::uint64_t taken{}, pass_through{}, replans{}, ring_declines{};
};

State* g_state = nullptr;

[[nodiscard]] bool call_of(const guest::R5900State& cpu, wc::Call& call, Key& key) {
    const std::uint64_t a0 = cpu.read_gpr_u64(4U), a1 = cpu.read_gpr_u64(5U);
    if (a0 != static_cast<std::uint32_t>(a0) || a1 != static_cast<std::uint32_t>(a1)) {
        return false;
    }
    call.ctx = static_cast<std::uint32_t>(a0);
    call.record = static_cast<std::uint32_t>(a1);
    call.clip = cpu.read_gpr_u64(6U);
    call.pass = cpu.read_gpr_u64(7U);
    call.gp = cpu.read_gpr_u64(28U);
    key = Key{cpu.read_gpr_u32(29U), cpu.read_gpr_u32(31U), call.ctx, call.record, call.clip, call.pass};
    return true;
}

// The shared decision; nullopt = decline.
std::optional<wc::Outcome> decide(State& st, const guest::R5900State& cpu, const guest::EeMemory& memory) {
    wc::Call call;
    Key key;
    // The dispatcher resolves an invocation twice back to back (prepare, then dispatch): the second pass of an accepted call is a repeat and
    // does not count again (nor advance the decline-every counter).
    const bool repeat = st.have_plan && call_of(cpu, call, key) && key == st.key;
    st.have_plan = false;
    if (!call_of(cpu, call, key)) {
        return std::nullopt;
    }
    if (!repeat && st.decline_every != 0U && ++st.every_count % st.decline_every == 0U) {
        ++st.producer.stats().declined;
        return std::nullopt;
    }
    if (st.ring->free_slots() < 1U) {
        ++st.ring_declines;
        st.ring->note_declined();
        ++st.producer.stats().declined;
        ++st.producer.stats().reasons[static_cast<std::size_t>(wc::Reason::kRing)];
        return std::nullopt;
    }
    const EeReadView view{memory};
    const wc::Stats saved = st.producer.stats();
    const wc::Outcome outcome = st.producer.plan(view, call, st.shadow, st.plan);
    if (repeat) {
        st.producer.stats() = saved;
    }
    if (outcome == wc::Outcome::kDecline) {
        if (st.plan.reason != wc::Reason::kNone) {
            st.ring->note_declined();
        }
        return std::nullopt;
    }
    if (outcome == wc::Outcome::kAccept && st.ring->free_slots() < st.plan.slots) {
        ++st.ring_declines;
        st.ring->note_declined();
        ++st.producer.stats().declined;
        ++st.producer.stats().reasons[static_cast<std::size_t>(wc::Reason::kRing)];
        return std::nullopt;
    }
    st.key = key;
    st.have_plan = true;
    return outcome;
}

std::optional<NativeFunctionLimits> resolve(State& st, const NativeFunctionPrepareContext& context) {
    if ((++st.ticks & 63U) == 0U) {
        // The registry ages its entries per frame; the resolver has no frame signal, a wall-clock tick does it.
        const auto now = std::chrono::steady_clock::now();
        if (now - st.last_frame >= std::chrono::milliseconds(16)) {
            st.last_frame = now;
            st.producer.registry().begin_frame();
        }
    }
    const std::optional<wc::Outcome> outcome = decide(st, context.cpu, context.memory);
    if (!outcome) {
        return std::nullopt;
    }
    const std::uint32_t cost = st.shadow ? 1U : st.plan.retired;
    return NativeFunctionLimits{cost, cost};
}

// Continue in the original: its first instruction, then the second.
NativeFunctionExecution pass_through(State& st, guest::R5900State& cpu) {
    ++st.pass_through;
    cpu.write_gpr_sign_extended_32(29U, cpu.read_gpr_u32(29U) - 16U);
    cpu.set_pc(wc::kEntryNext);
    return {.retired_instructions = 1U, .ee_cycles = 1U};
}

NativeFunctionExecution run(State& st, NativeFunctionContext& context) {
    guest::R5900State& cpu = context.cpu;
    wc::Call call;
    Key key;
    if (!call_of(cpu, call, key) || !st.have_plan || !(key == st.key)) {
        // Not the call the resolver planned (cannot happen with the dispatcher's order of calls): plan again, or let the guest have it.
        ++st.replans;
        const std::optional<wc::Outcome> outcome = decide(st, cpu, context.memory);
        if (!outcome) {
            return pass_through(st, cpu);
        }
    }
    st.have_plan = false;
    const wc::Plan& plan = st.plan;
    ++st.taken;

    if (plan.outcome == wc::Outcome::kOutside) {
        cpu.write_gpr_u64(2U, 0U);
        cpu.set_pc(cpu.read_gpr_u32(31U));
        return {.retired_instructions = plan.retired, .ee_cycles = plan.retired};
    }

    // One ring slot per 16 groups, committed as they are filled (one slot is open at a time); the resolver saw room for all of them.
    nc::Quad markers[wc::kMaxSlots];
    for (std::uint32_t k = 0; k != plan.slots; ++k) {
        ws::Submit* const slot = st.ring->begin_push(ws::kKindWorld);
        if (slot == nullptr) {
            return pass_through(st, cpu);   // (records already pushed stay in the ring; their markers never come: the consumer drops them as lost)
        }
        wc::Producer::fill(plan, k, *slot);
        {
            static const bool log_views = std::getenv("TAKEDOWN_NATIVE_WORLD_VIEWLOG") != nullptr;
            if (log_views) {
                static std::unordered_set<std::uint64_t> seen;
                const std::uint64_t h = nc::hash64(slot->view, 160U, 0x7669657755ULL);
                if (seen.insert(h).second && seen.size() < 200000U) {
                    std::fprintf(stderr, "[WVIEW2] %016llx stamp=%u rec=%08x\n", static_cast<unsigned long long>(h), slot->stamp, plan.record);
                }
            }
        }
        markers[k] = ws::Ring::marker_of(*slot);
        st.ring->commit_push();
    }
    if (st.shadow) {
        return pass_through(st, cpu);
    }
    EeGuestMemory memory{context.memory};
    wc::Producer::apply(memory, plan, markers);
    cpu.write_gpr_u64(2U, 1U);
    cpu.set_pc(cpu.read_gpr_u32(31U));
    return {.retired_instructions = plan.retired, .ee_cycles = plan.retired};
}

} // namespace

bool register_native_world_submit(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    if (g_state != nullptr) {
        return false;   // once per process
    }
    ws::Ring* const shadow_ring = platform::native_world_shadow_ring();
    ws::Ring* const replace_ring = platform::native_world_submit_ring();
    ws::Ring* const ring = shadow_ring != nullptr ? shadow_ring : replace_ring;
    if (ring == nullptr || native_function_disabled_by_environment("SubmitWorldObject")) {
        std::fprintf(stderr, "[NATIVEWORLD] not registered: ring=%p disabled=%d\n", static_cast<void*>(ring), native_function_disabled_by_environment("SubmitWorldObject") ? 1 : 0);
        return false;
    }
    if (original_code_fnv1a64(memory, wc::kEntry, wc::kCodeBytes) != kCodeFnv) {
        std::fprintf(stderr, "[NATIVEWORLD] not registered: code hash %016llx != %016llx\n",
                     static_cast<unsigned long long>(original_code_fnv1a64(memory, wc::kEntry, wc::kCodeBytes)), static_cast<unsigned long long>(kCodeFnv));
        return false;   // another revision of the game
    }
    static State state;
    state.ring = ring;
    state.shadow = shadow_ring != nullptr;
    if (const char* every = std::getenv("TAKEDOWN_NATIVE_WORLD_DECLINE_EVERY"); every != nullptr && *every != '\0') {
        state.decline_every = static_cast<std::uint32_t>(std::strtoul(every, nullptr, 10));
    }
    if (const char* verify = std::getenv("TAKEDOWN_NATIVE_WORLD_VERIFY"); verify != nullptr && *verify == '1') {
        world_mesh::WorldMeshRegistry::Config config;
        config.verify_every_use = true;   // debug: re-decode every use and compare the keys
        state.producer.registry() = world_mesh::WorldMeshRegistry(config);
    }
    const NativeFunctionTraits traits{
        .scheduler_state_transparent = true,
        .single_step_host_call = false,
        .defers_dmac_completions = true,
    };
    State* const shared = &state;
    const NativeFunctionRegistrationResult registration = dispatcher.register_function(
        kNativeWorldSubmitEntry, "SubmitWorldObject", NativeFunctionLimits{kCeiling, kCeiling}, traits,
        [shared](const NativeFunctionPrepareContext& context) { return resolve(*shared, context); },
        [shared](NativeFunctionContext& context) { return run(*shared, context); });
    if (!registration.registered()) {
        std::fprintf(stderr, "[NATIVEWORLD] not registered: dispatcher refused (address taken?)\n");
        return false;
    }
    g_state = shared;
    return true;
}

std::string native_world_submit_statistics() {
    if (g_state == nullptr) {
        return "[NATIVEWORLD] off";
    }
    const wc::Stats& s = g_state->producer.stats();
    const auto& registry = g_state->producer.registry().stats();
    char buffer[768];
    int n = std::snprintf(buffer, sizeof buffer,
                          "[NATIVEWORLD] %s resolves=%llu taken=%llu planned=%llu outside=%llu declined=%llu groups=%llu pass_through=%llu replans=%llu | "
                          "kinds 0:%llu 1:%llu 6:%llu 7:%llu | declined:",
                          g_state->shadow ? "shadow" : "replace", static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(g_state->taken),
                          static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.outside), static_cast<unsigned long long>(s.declined),
                          static_cast<unsigned long long>(s.groups), static_cast<unsigned long long>(g_state->pass_through),
                          static_cast<unsigned long long>(g_state->replans), static_cast<unsigned long long>(s.kinds[0]), static_cast<unsigned long long>(s.kinds[1]),
                          static_cast<unsigned long long>(s.kinds[6]), static_cast<unsigned long long>(s.kinds[7]));
    for (std::size_t i = 1U; i < static_cast<std::size_t>(wc::Reason::kCount) && n > 0 && static_cast<std::size_t>(n) < sizeof buffer; ++i) {
        n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " %s=%llu", wc::reason_name(static_cast<wc::Reason>(i)),
                           static_cast<unsigned long long>(s.reasons[i]));
    }
    std::string line = buffer;
    char tail[256];
    std::snprintf(tail, sizeof tail, " | meshes: hits=%llu imports=%llu failures=%llu invalidated=%llu owner_refreshes=%llu entries=%zu",
                  static_cast<unsigned long long>(registry.hits), static_cast<unsigned long long>(registry.imports),
                  static_cast<unsigned long long>(registry.failures), static_cast<unsigned long long>(registry.invalidated),
                  static_cast<unsigned long long>(s.owner_refreshes), g_state->producer.registry().size());
    return line + tail;
}

} // namespace takedown::runtime::slus_21050
