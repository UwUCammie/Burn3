// P2 / WP5: native props -- see include/takedown/runtime/slus_21050_native_props.hpp for the design.
//
// Layers: gather() reads everything a call depends on from the guest (ctx, staging, material, VU0 data memory, view blocks, instances);
// native_props::plan_call() (slus_21050_native_props_core.cpp, tested offline against the original's microprograms) decides and computes;
// build_submits() turns the plan into PropSubmits with the section's imported mesh; run() / resolve() carry it out (replace) or only
// publish it (shadow).

#include "takedown/runtime/slus_21050_native_props.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/platform/native_world_submit.hpp"
#include "takedown/runtime/native_world_mesh.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"
#include "takedown/runtime/slus_21050_native_props_core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

namespace np = native_props;
namespace ws = platform::world_submit;
namespace wm = runtime::world_mesh;
namespace nc = takedown::native_cmd;

constexpr std::uint32_t kGlobalPointer = 0x004e8670U;
constexpr std::uint32_t kStagingSlot = kGlobalPointer - 23648U;    // gp-23648: pointer to the draw list (scratchpad 0x70002c00)
constexpr std::uint32_t kCtx = 0x01e32a10U;                        // the one world / prop context the callers pass
constexpr std::uint32_t kTopology = 0x0064c994U;                   // *(0x64c994): the track topology (mesh epoch)
constexpr std::uint32_t kViewTable = 0x00666330U;                  // view struct v = +560 v; the packet block at +288
constexpr std::uint32_t kViewStride = 560U;
constexpr std::uint32_t kViewBlockAt = 288U;
constexpr std::uint32_t kCurrentTarget = 0x00667050U;
constexpr std::uint32_t kRamBytes = 32U * 1024U * 1024U;
constexpr std::uint32_t kScratchpad = 0x70000000U;
constexpr std::uint32_t kVu0Data = 0x11004000U;
constexpr std::uint32_t kPropProgram = 14U;
constexpr std::uint32_t kMinFreeSlots = 24U;

// Retail code fingerprints (tools/function_fingerprint.py style FNV-1a 64 of the function bytes).
constexpr std::uint32_t kCellBytes = 256U;
constexpr std::uint64_t kCellFnv = 0x91d7ae51589f2b6eULL;
constexpr std::uint32_t kDrainBytes = 128U;
constexpr std::uint64_t kDrainFnv = 0xa26a4d7502e31672ULL;
constexpr std::uint32_t kFlushBytes = 976U;                        // 0x26a240, whose semantics both calls reproduce
constexpr std::uint64_t kFlushFnv = 0x0392165a73fc4f61ULL;
constexpr std::uint32_t kSelectBytes = 448U;                       // 0x26baa0, ported below
constexpr std::uint64_t kSelectFnv = 0x5bea5ed6bce28130ULL;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kCostBase = 90U;
constexpr std::uint32_t kCostPerInstance = 40U;
constexpr std::uint32_t kCostPerFlush = 160U;

struct Counters {
    std::uint64_t calls{}, cells{}, drains{}, noops{}, taken{}, flushes{}, instances{}, records{}, tails{};
    std::uint64_t declined_every{}, declined_gather{}, declined_ring{}, declined_mesh{}, declined_replan{};
    std::uint64_t decline_reason[8]{};
    std::uint64_t shadow_pushed{}, shadow_dropped{};
    std::uint64_t reused{};               // run() took the resolver's plan
};

struct State {
    ws::Ring* ring{};
    bool shadow{};
    std::uint32_t decline_every{};
    std::uint64_t seen{};
    std::uint64_t drains_seen{};
    wm::WorldMeshRegistry registry;
    Counters counters;
};

State* g_state = nullptr;

[[nodiscard]] bool ram_range(const std::uint32_t address, const std::uint32_t bytes) noexcept {
    const std::uint32_t physical = address & 0x1fffffffU;
    return (address & 3U) == 0U && physical >= 0x1000U && physical < kRamBytes && bytes <= kRamBytes - physical;
}
[[nodiscard]] bool scratch_range(const std::uint32_t address, const std::uint32_t bytes) noexcept {
    return (address & 15U) == 0U && address >= kScratchpad && bytes <= 0x4000U && address - kScratchpad <= 0x4000U - bytes;
}
[[nodiscard]] std::int32_t sext8(const std::uint8_t v) noexcept { return static_cast<std::int32_t>(static_cast<std::int8_t>(v)); }

// ---- the guest, read ------------------------------------------------------------------------------------------------------------------
struct Gathered {
    np::CallInputs in;
    std::uint32_t ctx{}, staging{}, pair{};
    std::int32_t count{};                 // cell: instances (signed byte)
    bool noop{};                          // a cell call with nothing to do
};

[[nodiscard]] bool gather(const guest::R5900State& cpu, const guest::EeMemory& memory, const bool drain, const bool shadow, Gathered& g) {
    const std::uint32_t ctx = cpu.read_gpr_u32(4U);
    if (ctx != kCtx) {
        return false;
    }
    g.ctx = ctx;
    g.in.drain = drain;
    g.in.check_stage = !shadow;
    const std::uint32_t staging = memory.read_u32_le(kStagingSlot);
    if (!scratch_range(staging, 336U)) {
        return false;
    }
    g.staging = staging;
    const std::uint32_t material = memory.read_u32_le(ctx + 148U);
    if (!ram_range(material, 40U)) {
        return false;
    }
    g.in.material_kind = memory.read_u32_le(material);
    g.in.material_flags = memory.read_u16_le(material + 36U);
    const std::uint32_t table = memory.read_u32_le(material + 12U);
    if (table != 0U) {
        const std::uint32_t at = table + 4U * static_cast<std::uint32_t>(sext8(memory.read_u8(material + 16U)));
        if (!ram_range(at, 4U)) {
            return false;
        }
        g.in.texture = memory.read_u32_le(at) != 0U;
    }
    g.in.slots_byte = sext8(memory.read_u8(ctx + 132U));
    for (std::uint32_t k = 0; k != np::kMaxSlots; ++k) {
        g.in.view_of_slot[k] = sext8(memory.read_u8(ctx + 135U + k));
    }
    g.in.current_view = sext8(memory.read_u8(ctx + 153U));
    g.in.masks_unchanged = memory.read_u64_le(ctx + 160U) == memory.read_u64_le(ctx + 168U) && memory.read_u64_le(ctx + 176U) == memory.read_u64_le(ctx + 184U);
    g.pair = memory.read_u32_le(ctx + 128U);
    const std::uint32_t cursor = memory.read_u32_le(staging + 4U), base = memory.read_u32_le(staging + 8U);
    if (cursor < base || (cursor - base) % 16U != 0U) {
        return false;
    }
    g.in.queue_qw = memory.read_u32_le(staging);
    g.in.stage_used_qw = (cursor - base) / 16U;
    if (g.in.queue_qw > 64U) {
        return false;
    }
    g.in.regs.slots = cpu.read_vu0_integer(1U);
    g.in.regs.mode = cpu.read_vu0_integer(15U);
    g.in.regs.lighting = cpu.read_vu0_integer(8U);
    g.in.regs.retained = static_cast<std::uint32_t>(sext8(memory.read_u8(ctx + 138U)) != 0);
    for (std::uint32_t i = 0; i != np::kImageWords; ++i) {
        g.in.image.words[i] = memory.read_vu0_local_u32(kVu0Data + 4U * i);
    }
    for (std::uint32_t k = 0; k != np::kMaxSlots && k < g.in.regs.slots; ++k) {
        const std::int32_t v = g.in.view_of_slot[k];
        if (v >= 0 && v < static_cast<std::int32_t>(np::kMaxViews) && !g.in.blocks[static_cast<std::size_t>(v)].valid) {
            np::ViewBlock& block = g.in.blocks[static_cast<std::size_t>(v)];
            const std::uint32_t at = kViewTable + kViewStride * static_cast<std::uint32_t>(v) + kViewBlockAt;
            for (std::uint32_t w = 0; w != np::kViewBlockWords; ++w) {
                block.words[w] = memory.read_u32_le(at + 4U * w);
            }
            block.valid = true;
        }
    }
    if (!drain) {
        g.count = sext8(static_cast<std::uint8_t>(cpu.read_gpr_u32(5U) & 0xFFU));
        const std::uint32_t ptr = cpu.read_gpr_u32(6U);
        if (g.count <= 0) {
            g.noop = true;                   // the original loop does not run
            return true;
        }
        if (!ram_range(ptr, 64U * static_cast<std::uint32_t>(g.count)) || (ptr & 15U) != 0U) {
            return false;
        }
        g.in.instances.resize(static_cast<std::size_t>(g.count));
        for (std::int32_t i = 0; i != g.count; ++i) {
            for (std::uint32_t w = 0; w != 16U; ++w) {
                g.in.instances[static_cast<std::size_t>(i)][w] = memory.read_u32_le(ptr + 64U * static_cast<std::uint32_t>(i) + 4U * w);
            }
        }
    }
    return true;
}

// ---- submissions ----------------------------------------------------------------------------------------------------------------------
struct MeshUse {
    std::shared_ptr<const wm::ImportedMesh> mesh;
};

// The mesh of the section (ctx word 128 points at the pair {mesh, continuation}); nullptr when the importer cannot take it.
[[nodiscard]] std::shared_ptr<const wm::ImportedMesh> section_mesh(State& st, const guest::EeMemory& memory, const std::uint32_t pair) {
    if (!ram_range(pair, 8U)) {
        return nullptr;
    }
    const std::uint32_t packet = memory.read_u32_le(pair), cont = memory.read_u32_le(pair + 4U);
    if (packet == 0U) {
        return nullptr;
    }
    const auto bytes = std::as_const(memory).bytes();
    const wm::MainRamView ram(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    st.registry.set_epoch(0x100000000ULL | memory.read_u32_le(kTopology));
    std::shared_ptr<const wm::ImportedMesh> mesh = st.registry.acquire(ram, packet, cont, static_cast<std::uint16_t>(kPropProgram));
    if (!mesh || mesh->status != wm::ImportStatus::kOk || mesh->groups.empty() || mesh->groups.size() > ws::kMaxGroups) {
        return nullptr;
    }
    for (const wm::ImportedGroup& g : mesh->groups) {
        if (g.n < 3U || g.n > nc::kMaxGroupVertices || g.mesh.n != g.n) {      // the emitter's group_valid
            return nullptr;
        }
    }
    return mesh;
}

void fill_submit(ws::Submit& s, const Gathered& g, const np::CallPlan& plan, const std::size_t k, const std::shared_ptr<const wm::ImportedMesh>& mesh) {
    const np::Flush& flush = plan.flushes[k];
    s.program = static_cast<std::uint16_t>(kPropProgram);
    s.prim = plan.prim;
    s.record_addr = g.pair;
    const np::ViewBlock& block = g.in.blocks[plan.planned[k].view];
    for (std::uint32_t i = 0; i != np::kViewBlockWords; ++i) {
        s.view[i] = std::bit_cast<float>(block.words[i]);
    }
    s.group_count = static_cast<std::uint8_t>(mesh->groups.size());
    for (std::size_t i = 0; i != mesh->groups.size(); ++i) {
        s.groups[i].mesh = std::shared_ptr<const nc::StripMesh>(mesh, &mesh->groups[i].mesh);
        s.groups[i].mesh_key = mesh->groups[i].key;
        s.groups[i].missing = mesh->groups[i].missing;
    }
    s.inst.resize(flush.records.size());
    for (std::size_t i = 0; i != flush.records.size(); ++i) {
        const np::InstanceView v = np::instance_view(flush.records[i]);
        std::memcpy(s.inst[i].xform, v.xform, sizeof v.xform);
        std::memcpy(s.inst[i].tint, v.tint, sizeof v.tint);
    }
    s.inst_count = static_cast<std::uint16_t>(s.inst.size());
}

// ---- 0x26baa0 (SelectRenderTarget), ported: queues the 17 quadwords that make the view's packet block current -----------------------------
using Quad = std::array<std::uint32_t, 4>;
[[nodiscard]] Quad load_quad(const guest::EeMemory& m, const std::uint32_t at) {
    return Quad{m.read_u32_le(at), m.read_u32_le(at + 4U), m.read_u32_le(at + 8U), m.read_u32_le(at + 12U)};
}
[[nodiscard]] Quad make_quad(const std::uint64_t upper, const std::uint64_t lower) noexcept {
    return Quad{static_cast<std::uint32_t>(lower), static_cast<std::uint32_t>(lower >> 32U), static_cast<std::uint32_t>(upper),
                static_cast<std::uint32_t>(upper >> 32U)};
}
void select_render_target(guest::EeMemory& m, const std::uint32_t object) {
    const std::uint32_t buffer = m.read_u32_le(kStagingSlot);
    const std::int32_t index = sext8(m.read_u8(object + 153U));
    m.write_u32_le(kCurrentTarget, static_cast<std::uint32_t>(index));
    m.write_u32_le(buffer, 0U);
    const std::uint32_t target = kViewTable + kViewStride * static_cast<std::uint32_t>(index);
    const std::uint32_t constants = target + kViewBlockAt;
    const auto append = [&m, buffer](const Quad& q) {
        const std::uint32_t count = m.read_u32_le(buffer);
        m.write_u32_le(buffer, count + 1U);
        m.write_u128_le(buffer + 16U * count + 16U, q);
    };
    append(make_quad(0x6c0a03f601000404ULL, 0x000000001000000aULL));
    for (std::uint32_t pass = 0; pass != 10U; ++pass) {
        append(load_quad(m, constants + 16U * pass));
    }
    append(make_quad(0x5000000500000000ULL, 0x0000000010000005ULL));
    append(make_quad(0x0EULL, 0x1000000000008004ULL));
    const Quad row448 = load_quad(m, target + 448U), row464 = load_quad(m, target + 464U);
    const std::uint64_t frame_lo = (static_cast<std::uint64_t>(row448[1]) << 32U | row448[0]) | m.read_u64_le(object + 160U);
    const std::uint64_t zbuf_lo = (static_cast<std::uint64_t>(row464[1]) << 32U | row464[0]) | m.read_u64_le(object + 176U);
    append(make_quad(76U, frame_lo));
    append(make_quad(78U, zbuf_lo));
    append(load_quad(m, target + 480U));
    append(load_quad(m, target + 496U));
}

// ---- one call, both modes ---------------------------------------------------------------------------------------------------------------
struct Prepared {
    Gathered g;
    np::CallPlan plan;
    std::shared_ptr<const wm::ImportedMesh> mesh;
};

// The dispatcher resolves a call and runs it back to back on the same state: run() takes the resolver's Prepared instead of gathering
// and planning the cell a second time when the CPU state and main RAM are unchanged (the check n3's dry-run reuse makes).
struct PreparedCache {
    bool valid{};
    bool drain{};
    std::uint64_t generation{};
    std::array<std::byte, sizeof(guest::R5900State)> cpu{};
    Prepared p;
};
PreparedCache g_cache;

[[nodiscard]] std::uint64_t ram_generation(const guest::EeMemory& memory) noexcept {
    const std::uint64_t* const g = const_cast<guest::EeMemory&>(memory).ee_aot_memory_view().mutation_generation;
    return g != nullptr ? *g : 0U;
}

// Reads, plans, imports. False: the call is not ours (State::counters says why).
[[nodiscard]] bool prepare(State& st, const guest::R5900State& cpu, const guest::EeMemory& memory, const bool drain, Prepared& p) {
    Counters& c = st.counters;
    if (!gather(cpu, memory, drain, st.shadow, p.g)) {
        ++c.declined_gather;
        return false;
    }
    if (p.g.noop) {
        return true;
    }
    p.plan = np::plan_call(p.g.in);
    if (p.plan.decline != np::Decline::kNone) {
        ++c.decline_reason[static_cast<std::size_t>(p.plan.decline)];
        return false;
    }
    if (!p.plan.flushes.empty()) {
        p.mesh = section_mesh(st, memory, p.g.pair);
        if (!p.mesh) {
            ++c.declined_mesh;
            return false;
        }
    }
    return true;
}

[[nodiscard]] NativeFunctionLimits cost_of(const Prepared& p) noexcept {
    const std::uint32_t cost = std::min(kCeiling, kCostBase + kCostPerInstance * static_cast<std::uint32_t>(p.g.in.instances.size()) +
                                                       kCostPerFlush * static_cast<std::uint32_t>(p.plan.flushes.size()));
    return NativeFunctionLimits{cost, cost};
}

// Shadow mode: publish what the guest is about to flush.
void publish_shadow(State& st, const Prepared& p) {
    for (std::size_t k = 0; k != p.plan.flushes.size(); ++k) {
        ws::Submit* const s = st.ring->begin_push(ws::kKindProp);
        if (s == nullptr) {
            ++st.counters.shadow_dropped;
            continue;
        }
        fill_submit(*s, p.g, p.plan, k, p.mesh);
        st.ring->commit_push();
        ++st.counters.shadow_pushed;
    }
}

std::optional<NativeFunctionLimits> resolve(State& st, const NativeFunctionPrepareContext& context, const bool drain) {
    Counters& c = st.counters;
    ++c.calls;
    if (st.decline_every != 0U && ++st.seen % st.decline_every == 0U) {
        ++c.declined_every;
        return std::nullopt;
    }
    if (!st.shadow && st.ring->free_slots() < kMinFreeSlots + 14U) {
        ++c.declined_ring;
        st.ring->note_declined();
        return std::nullopt;
    }
    Prepared p;
    if (!prepare(st, context.cpu, context.memory, drain, p)) {
        return std::nullopt;
    }
    if (st.shadow) {
        publish_shadow(st, p);
        return std::nullopt;                    // the guest runs the call
    }
    if (st.ring->free_slots() < p.plan.flushes.size() + kMinFreeSlots) {
        ++c.declined_ring;
        st.ring->note_declined();
        return std::nullopt;
    }
    const NativeFunctionLimits limits = cost_of(p);
    g_cache.generation = ram_generation(context.memory);
    g_cache.valid = g_cache.generation != 0U && g_cache.generation != ~std::uint64_t{0U};
    g_cache.drain = drain;
    std::memcpy(g_cache.cpu.data(), &context.cpu, sizeof(guest::R5900State));
    g_cache.p = std::move(p);
    return limits;
}

NativeFunctionExecution run(State& st, NativeFunctionContext& context, const bool drain) {
    guest::R5900State& cpu = context.cpu;
    guest::EeMemory& memory = context.memory;
    Counters& c = st.counters;
    Prepared p;
    const bool reuse = g_cache.valid && g_cache.drain == drain && g_cache.generation == ram_generation(memory) &&
                       std::memcmp(g_cache.cpu.data(), &cpu, sizeof(guest::R5900State)) == 0;
    g_cache.valid = false;
    if (reuse) {
        p = std::move(g_cache.p);
        ++c.reused;
    }
    // The resolver ran on this very state; if the plan cannot be made again (it can not happen), the call is carried out as a no-op
    // rather than half done.
    if (!reuse && !prepare(st, cpu, memory, drain, p)) {
        ++c.declined_replan;
        p = Prepared{};
        p.g.noop = true;
    }
    ++(drain ? c.drains : c.cells);
    const std::uint32_t ctx = kCtx;
    std::uint32_t cursor = 0;
    bool wrote = false;
    if (!p.g.noop && !p.plan.flushes.empty()) {
        const std::uint32_t staging = p.g.staging;
        cursor = memory.read_u32_le(staging + 4U);
        for (std::size_t k = 0; k != p.plan.flushes.size(); ++k) {
            const np::PlannedFlush& f = p.plan.planned[k];
            if (f.select_target) {
                // 0x26a240: ctx[153] = view, the masks are saved, then SelectRenderTarget
                memory.write_u8(ctx + 153U, static_cast<std::uint8_t>(f.view));
                memory.write_u64_le(ctx + 168U, memory.read_u64_le(ctx + 160U));
                memory.write_u64_le(ctx + 184U, memory.read_u64_le(ctx + 176U));
                select_render_target(memory, ctx);
            }
            if (f.queue_qw != 0U) {
                // FLUSHE first (the queued view block is written to absolute VU1 addresses), then the queue, as 0x26af50 does
                memory.write_u128_le(cursor, Quad{0x10000000U, 0U, 0x10000000U, 0U});
                cursor += 16U;
                for (std::uint32_t i = 0; i != f.queue_qw; ++i) {
                    memory.write_u128_le(cursor, load_quad(memory, staging + 16U + 16U * i));
                    cursor += 16U;
                }
                memory.write_u32_le(staging, 0U);
            }
            ws::Submit* const s = st.ring->begin_push(ws::kKindProp);
            if (s == nullptr) {                  // the resolver checked the room; a full ring here only loses this batch
                ++c.declined_ring;
                continue;
            }
            fill_submit(*s, p.g, p.plan, k, p.mesh);
            std::uint32_t stub[12];
            const std::size_t qw = ws::write_stub(stub, *s);
            st.ring->commit_push();
            for (std::size_t i = 0; i != qw; ++i) {
                memory.write_u128_le(cursor, Quad{stub[4U * i], stub[4U * i + 1U], stub[4U * i + 2U], stub[4U * i + 3U]});
                cursor += 16U;
            }
            memory.write_u8(ctx + 133U, static_cast<std::uint8_t>(f.slot));
            ++c.flushes;
            c.records += p.plan.flushes[k].records.size();
            wrote = true;
        }
        memory.write_u32_le(staging + 4U, cursor);
        if (wrote) {
            memory.write_u8(ctx + 155U, 1U);
            memory.write_u8(ctx + 134U, 0U);
        }
    }
    if (!p.g.noop) {
        c.instances += p.g.in.instances.size();
        for (std::uint32_t i = 0; i != np::kImageWords; ++i) {      // the VU0 data memory the original would have left
            if (p.plan.image.words[i] != p.g.in.image.words[i]) {
                memory.write_vu0_local_u32(kVu0Data + 4U * i, p.plan.image.words[i]);
            }
        }
    } else {
        ++c.noops;
    }
    ++c.taken;
    cpu.write_gpr_sign_extended_32(2U, 1U);                           // both functions return 1
    const std::uint32_t cost = std::min(kCeiling, kCostBase + kCostPerInstance * static_cast<std::uint32_t>(p.g.in.instances.size()) +
                                                       kCostPerFlush * static_cast<std::uint32_t>(p.plan.flushes.size()));
    if (wrote) {
        // Tail call: the guest's group prologue (binds program 14 and the texture once per group) and the copy of the staging output
        // to the ring, then back to our caller (ra and sp are untouched).
        cpu.write_gpr_sign_extended_32(4U, ctx);
        cpu.set_pc(kPropStageCopyAddress);
        ++c.tails;
    } else {
        cpu.set_pc(cpu.read_gpr_u32(31U));
    }
    return {.retired_instructions = cost, .ee_cycles = cost};
}

} // namespace

bool register_native_props(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    if (g_state != nullptr) {
        return false;
    }
    ws::Ring* const replace = platform::native_world_submit_ring();
    ws::Ring* const shadow = platform::native_world_shadow_ring();
    ws::Ring* const ring = shadow != nullptr ? shadow : replace;
    if (ring == nullptr || native_function_disabled_by_environment("PropCell") || native_function_disabled_by_environment("PropDrain")) {
        return false;
    }
    if (original_code_fnv1a64(memory, kPropCellAddress, kCellBytes) != kCellFnv || original_code_fnv1a64(memory, kPropDrainAddress, kDrainBytes) != kDrainFnv ||
        original_code_fnv1a64(memory, 0x0026a240U, kFlushBytes) != kFlushFnv || original_code_fnv1a64(memory, 0x0026baa0U, kSelectBytes) != kSelectFnv) {
        return false;                                                 // another revision of the game
    }
    static State state;
    state.ring = ring;
    state.shadow = shadow != nullptr;
    if (const char* every = std::getenv("TAKEDOWN_NATIVE_PROPS_DECLINE_EVERY"); every != nullptr && *every != '\0') {
        state.decline_every = static_cast<std::uint32_t>(std::strtoul(every, nullptr, 10));
    }
    const NativeFunctionTraits traits{
        .scheduler_state_transparent = true,
        .single_step_host_call = false,
        .defers_dmac_completions = true,
    };
    State* const s = &state;
    const NativeFunctionRegistrationResult cell = dispatcher.register_function(
        kPropCellAddress, "PropCell", NativeFunctionLimits{kCeiling, kCeiling}, traits,
        [s](const NativeFunctionPrepareContext& context) { return resolve(*s, context, false); },
        [s](NativeFunctionContext& context) { return run(*s, context, false); });
    if (!cell.registered()) {
        return false;
    }
    const NativeFunctionRegistrationResult drain = dispatcher.register_function(
        kPropDrainAddress, "PropDrain", NativeFunctionLimits{kCeiling, kCeiling}, traits,
        [s](const NativeFunctionPrepareContext& context) { return resolve(*s, context, true); },
        [s](NativeFunctionContext& context) { return run(*s, context, true); });
    if (!drain.registered()) {
        return false;                                                 // (the cell hook stays; it is correct on its own)
    }
    g_state = s;
    return true;
}

std::string native_props_statistics() {
    if (g_state == nullptr) {
        return "[NATIVEPROPS] off";
    }
    const Counters& c = g_state->counters;
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    char buffer[768];
    std::snprintf(buffer, sizeof buffer,
                  "[NATIVEPROPS] %s calls=%llu taken=%llu (cells %llu drains %llu noops %llu) flushes=%llu records=%llu instances=%llu tails=%llu reused=%llu | declined: gather=%llu ring=%llu mesh=%llu "
                  "every=%llu replan=%llu regs=%llu material=%llu buffer=%llu view=%llu lit=%llu stage=%llu | shadow pushed=%llu dropped=%llu | mesh cache=%zu/%llu bytes",
                  g_state->shadow ? "shadow" : "replace", u(c.calls), u(c.taken), u(c.cells), u(c.drains), u(c.noops), u(c.flushes), u(c.records), u(c.instances), u(c.tails), u(c.reused),
                  u(c.declined_gather), u(c.declined_ring), u(c.declined_mesh), u(c.declined_every), u(c.declined_replan),
                  u(c.decline_reason[static_cast<std::size_t>(np::Decline::kRegs)]), u(c.decline_reason[static_cast<std::size_t>(np::Decline::kMaterial)]),
                  u(c.decline_reason[static_cast<std::size_t>(np::Decline::kBuffer)]), u(c.decline_reason[static_cast<std::size_t>(np::Decline::kView)]),
                  u(c.decline_reason[static_cast<std::size_t>(np::Decline::kLit)]), u(c.decline_reason[static_cast<std::size_t>(np::Decline::kStage)]), u(c.shadow_pushed),
                  u(c.shadow_dropped), g_state->registry.size(), u(g_state->registry.bytes()));
    return buffer;
}

} // namespace takedown::runtime::slus_21050
