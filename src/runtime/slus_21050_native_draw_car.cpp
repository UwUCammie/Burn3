// P1 / WP5: native DrawCar (original 0x0021e6f0) -- see include/takedown/runtime/slus_21050_native_draw_car.hpp.
//
// The hook sits at 0x0021e8cc, behind DrawCar's call of the texture bind. What the guest has done by then, and keeps doing:
//   prologue (frame, saves), S+248 / S+252, the LOD block, StartEffect (W scratch), the texture bind 0x0021ff60 (bind packet in the
//   display list, texture manager residency and upload, S+32 TEX0 slot, gp-24080).
// What the hook does instead of the rest of DrawCar (everything is a pure function of guest memory, done in the resolver):
//   plan_draw_car_ee           which part draws, in which order, with which matrices, the glass patch bytes
//   start_shading/part_entries the 52-quadword constants of every draw-list entry (bit for bit the guest's)
//   mesh cache                 the entry's static packets as CarBatch images, with the trusted path's mesh keys
// and at execution: the glass patch bytes (+11/+13 of the BGV glass entries, as DrawCar writes them), one stub per submission in the
// display list (DMA CNT + VIF DIRECT [+ 4-quadword A+D prefix] + marker, through the guest's own ReservePacketBuffer) and the ring push.
// Then pc := 0x0021f754, the function's epilogue (the guest restores s0..s7/fp/ra and returns).
//
// Declining is free: the resolver returns nothing and the guest continues at 0x0021e8cc with a complete DrawCar state. When the display
// list segment has no room for the stubs the hook first sends the guest's ReservePacketBuffer (0x001f3080: it flushes the segment
// through the DMA channel, device work a native cannot do) and returns to 0x0021e8cc; the next resolve then finds room.
//
// Not written: W scratch beyond what StartEffect wrote, VU0 registers and data memory, the lighting matrix reuse state W+144.

#include "takedown/runtime/slus_21050_native_draw_car.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/platform/native_car_submit.hpp"
#include "takedown/runtime/game/n3_support.hpp"
#include "takedown/runtime/native_car_constants.hpp"
#include "takedown/runtime/native_car_draw.hpp"
#include "takedown/runtime/native_car_submit_build.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"

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
#include <vector>

N3_CALLEE(001f3080)

namespace takedown::runtime::slus_21050 {

namespace {

namespace nc = native_car;
namespace cs = platform::car_submit;

constexpr std::uint32_t kDrawCarCodeBytes = 4244U;
constexpr std::uint64_t kDrawCarCodeFnv = 0x7be03e7a1f78f407ULL;

constexpr std::uint32_t kGlobalPointer = 0x004e8670U;
constexpr std::uint32_t kCursorSlot = kGlobalPointer - 24240U;     // display list cursor
constexpr guest::GuestAddress kReservePacketBuffer = 0x001f3080U;

// Registers of DrawCar at 0x0021e8cc.
constexpr unsigned kRegSp = 29U, kRegRa = 31U, kRegA0 = 4U, kRegA1 = 5U;
constexpr unsigned kRegBlk = 16U;     // s0
constexpr unsigned kRegModel = 17U;   // s1
constexpr unsigned kRegDraw = 18U;    // s2  (B)
constexpr unsigned kRegFrame = 19U;   // s3  (F)
constexpr unsigned kRegCtx = 20U;     // s4  (S)
constexpr unsigned kRegLight = 21U;   // s5

// Optional (off): the call ends with one UNPACK of the last submission's 52 quadwords into VU1 962.. (STCYCL 4,4 as the car packets leave
// it), so VU1 data memory holds the last entry's constants as it does after the guest's car packets. Tried for the strips drawn after the
// cars pass in the garage; they do not read these quadwords (see slus_21050_native_draw_car.hpp, "State not reproduced").
constexpr std::uint32_t kTailQuadwords = 2U + 44U + 1U + 8U;   // at most: DMA tag, VIF codes + 962..1005, VIF codes + 1006..1013

constexpr std::uint32_t kMinFreeSlots = 64U;
constexpr std::uint32_t kMaxStubQuadwords = 480U;                  // a segment holds 512 quadwords including the reserve's fixed part
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kCostBase = 900U;                          // nominal: the guest path retires about 9000
constexpr std::uint32_t kCostPerStub = 40U;
constexpr std::uint32_t kCostFlush = 60U;

class EeReader {
  public:
    explicit EeReader(const guest::EeMemory& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint8_t u8(const std::uint32_t a) const { return memory_.read_u8(a); }
    [[nodiscard]] std::int8_t s8(const std::uint32_t a) const { return static_cast<std::int8_t>(memory_.read_u8(a)); }
    [[nodiscard]] std::uint16_t u16(const std::uint32_t a) const { return memory_.read_u16_le(a); }
    [[nodiscard]] std::uint32_t u32(const std::uint32_t a) const { return memory_.read_u32_le(a); }
    [[nodiscard]] float f32(const std::uint32_t a) const { return std::bit_cast<float>(memory_.read_u32_le(a)); }

  private:
    const guest::EeMemory& memory_;
};

struct Counters {
    std::uint64_t resolves{}, taken{}, flushes{};
    std::uint64_t declined_gate{}, declined_every{}, declined_ring{}, declined_state{}, declined_plan{}, declined_unlit{},
        declined_build{}, declined_space{}, declined_flush_again{};
    std::uint64_t submits{}, stubs_qw{}, batches{}, entries{}, tails{};
    nc::BuildStats build{};
};

struct State {
    platform::CarSubmitRing* ring{};
    std::uint32_t decline_every{};
    bool vu1_tail{true};                // TAKEDOWN_NATIVE_CAR_VU1_TAIL=0 leaves out the UNPACK of the call's constants into VU1 (see below)
    bool vu0_memory{false};             // TAKEDOWN_NATIVE_CAR_VU0_MEM=1: leave microprogram 0xfb's VU0 data memory image (not needed, see below)
    std::uint64_t calls{};

    std::unique_ptr<nc::CarDrawPlan> plan = std::make_unique<nc::CarDrawPlan>();
    std::unique_ptr<std::array<nc::EntryDraw, nc::kMaxEntries>> entries = std::make_unique<std::array<nc::EntryDraw, nc::kMaxEntries>>();
    nc::CarShading shading;
    nc::MeshCache cache;
    std::vector<nc::PendingSubmit> submits;
    std::uint32_t total_qw{};

    // What `submits` was built from; a resolve on the same call state reuses it (the dispatcher resolves twice, back to back).
    struct Key {
        std::uint32_t sp{}, s{}, f{}, b{}, blk{};
        std::uint64_t generation{};
        bool operator==(const Key&) const = default;
    } key;
    bool built{};

    bool flush_pending{};      // the previous execution sent ReservePacketBuffer; the next resolve must find room
    bool need_flush{};         // set by the resolver for the execution that follows it
    Counters counters;
};

State* g_state = nullptr;

[[nodiscard]] bool reserve_dry(const guest::R5900State& cpu, const guest::EeMemory& memory, const std::uint32_t quadwords) {
    guest::R5900State copy = cpu;
    n3::DryMemory dry{memory};
    n3::DryMachine machine{copy, dry, 0U};
    n3::Call<n3::DryMemory> call{machine};
    call.set_arg64(0U, 0U);
    call.set_arg(1U, quadwords);
    const n3::Run run = n3::fn_001f3080(machine);
    return !run.declined && call.result() != 0U;
}

[[nodiscard]] bool reserve_live(guest::R5900State& cpu, guest::EeMemory& memory, const std::uint32_t quadwords) {
    n3::LiveMachine machine{cpu, memory, 0U};
    n3::Call<guest::EeMemory> call{machine};
    call.set_arg64(0U, 0U);
    call.set_arg(1U, quadwords);
    const n3::Run run = n3::fn_001f3080(machine);
    return !run.declined && call.result() != 0U;
}

std::optional<NativeFunctionLimits> resolve(State& st, const NativeFunctionPrepareContext& context) {
    Counters& c = st.counters;
    ++c.resolves;
    const bool second_try = st.flush_pending;
    st.flush_pending = false;
    st.need_flush = false;

    if (st.decline_every != 0U && ++st.calls % st.decline_every == 0U) {
        ++c.declined_every;
        st.ring->note_declined();
        return std::nullopt;
    }
    if (st.ring->free_slots() < kMinFreeSlots) {
        ++c.declined_ring;
        st.ring->note_declined();
        return std::nullopt;
    }

    const guest::R5900State& cpu = context.cpu;
    const guest::EeMemory& memory = context.memory;
    const EeReader reader{memory};
    const std::uint32_t sp = cpu.read_gpr_u32(kRegSp);
    const std::uint32_t blk = cpu.read_gpr_u32(kRegBlk);
    const std::uint32_t model = cpu.read_gpr_u32(kRegModel);
    const std::uint32_t b = cpu.read_gpr_u32(kRegDraw);
    const std::uint32_t f = cpu.read_gpr_u32(kRegFrame);
    const std::uint32_t s = cpu.read_gpr_u32(kRegCtx);

    // The registers must describe a DrawCar frame as the prologue leaves it: the model is the car frame's BGV file and the LOD block
    // one of its five blocks.
    if (model == 0U || reader.u32(f + 64U) != model) {
        ++c.declined_state;
        return std::nullopt;
    }
    std::int32_t lod = -1;
    for (std::int32_t k = 0; k <= 4; ++k) {
        if (reader.u32(model + 76U + 4U * static_cast<std::uint32_t>(k)) == blk) {
            lod = k;
            break;
        }
    }
    if (lod < 0 || blk == 0U) {
        ++c.declined_state;
        return std::nullopt;
    }

    nc::CarDrawPlan& plan = *st.plan;
    plan = nc::CarDrawPlan{};
    if (!nc::plan_draw_car_ee(memory, s, f, b, lod, plan)) {
        ++c.declined_plan;
        st.ring->note_declined();
        return std::nullopt;
    }
    if (!plan.drawn || plan.blk != blk || plan.s != s || plan.f != f || plan.b != b || plan.light_bits != cpu.read_gpr_u32(kRegLight) ||
        plan.part_count == 0U) {
        ++c.declined_state;
        return std::nullopt;
    }
    // part_entries reuses the lighting matrix of the last lit part; the call has to start with a lit one.
    if (!plan.parts[0].lighting) {
        ++c.declined_unlit;
        st.ring->note_declined();
        return std::nullopt;
    }

    // The submissions: pure functions of memory, so a resolve on the same state (and unchanged RAM) keeps the previous build.
    auto& mutable_memory = const_cast<guest::EeMemory&>(memory);
    const std::uint64_t generation = n3::main_ram_generation(mutable_memory);
    const State::Key key{sp, s, f, b, blk, generation};
    if (!(st.built && generation != 0U && generation != ~std::uint64_t{0U} && key == st.key)) {
        st.submits.clear();
        nc::BuildStats stats;
        const bool ok = nc::build_call_submits(reader, plan, st.shading, *st.entries, st.cache, st.submits, stats);
        c.build.entries += stats.entries;
        c.build.bad_meshes += stats.bad_meshes;
        c.build.vertex_mismatch += stats.vertex_mismatch;
        c.build.truncated += stats.truncated;
        st.built = ok && !st.submits.empty();
        st.key = key;
        st.total_qw = nc::stub_quadwords_total(st.submits) + (st.vu1_tail ? kTailQuadwords : 0U);
        if (!st.built) {
            ++c.declined_build;
            st.ring->note_declined();
            return std::nullopt;
        }
    }
    if (st.total_qw > kMaxStubQuadwords || st.ring->free_slots() < st.submits.size() + kMinFreeSlots) {
        ++c.declined_space;
        st.ring->note_declined();
        return std::nullopt;
    }

    // Room in the display list for the stubs.
    if (!reserve_dry(cpu, memory, st.total_qw)) {
        if (second_try) {
            ++c.declined_flush_again;
            st.ring->note_declined();
            return std::nullopt;
        }
        st.need_flush = true;
    }
    const std::uint32_t cost = std::min(kCeiling, kCostBase + kCostPerStub * static_cast<std::uint32_t>(st.submits.size()));
    return NativeFunctionLimits{st.need_flush ? kCostFlush : cost, st.need_flush ? kCostFlush : cost};
}

NativeFunctionExecution run(State& st, NativeFunctionContext& context) {
    guest::R5900State& cpu = context.cpu;
    guest::EeMemory& memory = context.memory;
    Counters& c = st.counters;
    const nc::CarDrawPlan& plan = *st.plan;

    if (st.need_flush) {
        // ReservePacketBuffer as the guest's DrawEntryList asks for it: it may send the finished segment to the DMA channel. Return to
        // 0x0021e8cc afterwards; DrawCar's own registers (s0..s7, sp) are untouched, a0/a1/ra are free again at this point.
        st.need_flush = false;
        st.flush_pending = true;
        ++c.flushes;
        cpu.write_gpr_u64(kRegA0, 0U);
        cpu.write_gpr_sign_extended_32(kRegA1, st.total_qw);
        cpu.write_gpr_sign_extended_32(kRegRa, kDrawCarAfterBind);
        cpu.set_pc(kReservePacketBuffer);
        return {.retired_instructions = kCostFlush, .ee_cycles = kCostFlush};
    }

    // The glass patch of the BGV entries (DrawCar writes +11 and +13 of the glass entry of the body list and of each drawn panel list).
    for (std::size_t i = 0U; i < plan.glass_patch_count; ++i) {
        memory.write_u8(plan.glass_patches[i].entry + 11U, plan.glass_patches[i].b11);
        memory.write_u8(plan.glass_patches[i].entry + 13U, plan.glass_patches[i].b13);
    }

    if (!reserve_live(cpu, memory, st.total_qw)) {
        // The resolver saw room a moment ago and nothing ran since; if it is gone anyway, ask the guest to make it (once).
        st.flush_pending = true;
        ++c.flushes;
        cpu.write_gpr_u64(kRegA0, 0U);
        cpu.write_gpr_sign_extended_32(kRegA1, st.total_qw);
        cpu.write_gpr_sign_extended_32(kRegRa, kDrawCarAfterBind);
        cpu.set_pc(kReservePacketBuffer);
        return {.retired_instructions = kCostFlush, .ee_cycles = kCostFlush};
    }

    if (static const long debug_calls = [] { const char* v = std::getenv("TAKEDOWN_NATIVE_CAR_DEBUG"); return v != nullptr ? std::strtol(v, nullptr, 10) : 0L; }();
        debug_calls > 0 && static_cast<long>(c.taken) < debug_calls) {
        // Diagnostic: the first calls' plan and first submission (TAKEDOWN_NATIVE_CAR_DEBUG=<count>).
        const nc::PartDraw& p0 = plan.parts[0];
        std::fprintf(stderr, "[NATIVECAR-DBG] call %llu s=%08x f=%08x b=%08x blk=%08x parts=%zu submits=%zu kind0=%d\n", static_cast<unsigned long long>(c.taken), plan.s,
                     plan.f, plan.b, plan.blk, plan.part_count, st.submits.size(), static_cast<int>(p0.kind));
        for (std::size_t i = 0U; i < 4U; ++i) {
            std::fprintf(stderr, "[NATIVECAR-DBG]   matrix[%zu] %08x %08x %08x %08x\n", i, p0.matrix[i][0], p0.matrix[i][1], p0.matrix[i][2], p0.matrix[i][3]);
        }
        for (std::uint32_t i = 0U; i < 16U; i += 4U) {
            std::fprintf(stderr, "[NATIVECAR-DBG]   F+%u: %08x %08x %08x %08x\n", i * 4U, memory.read_u32_le(plan.f + i * 4U), memory.read_u32_le(plan.f + i * 4U + 4U),
                         memory.read_u32_le(plan.f + i * 4U + 8U), memory.read_u32_le(plan.f + i * 4U + 12U));
        }
        for (std::uint32_t k = 0U; k < 4U; ++k) {   // StartEffect's results the guest already wrote into the scratchpad record W (0x70002000) vs the native ones
            std::fprintf(stderr, "[NATIVECAR-DBG]   W+%u guest %08x %08x %08x %08x | native %08x %08x %08x %08x\n", 784U + 16U * k,
                         memory.read_u32_le(0x70002000U + 784U + 16U * k), memory.read_u32_le(0x70002000U + 788U + 16U * k),
                         memory.read_u32_le(0x70002000U + 792U + 16U * k), memory.read_u32_le(0x70002000U + 796U + 16U * k), k < 3U ? st.shading.axes[k][0] : st.shading.delta[0],
                         k < 3U ? st.shading.axes[k][1] : st.shading.delta[1], k < 3U ? st.shading.axes[k][2] : st.shading.delta[2], k < 3U ? st.shading.axes[k][3] : st.shading.delta[3]);
        }
        std::fprintf(stderr, "[NATIVECAR-DBG]   S+192 %08x %08x %08x %08x  S+224 %08x %08x %08x %08x  W+832 guest %08x %08x %08x %08x\n", memory.read_u32_le(plan.s + 192U),
                     memory.read_u32_le(plan.s + 196U), memory.read_u32_le(plan.s + 200U), memory.read_u32_le(plan.s + 204U), memory.read_u32_le(plan.s + 224U),
                     memory.read_u32_le(plan.s + 228U), memory.read_u32_le(plan.s + 232U), memory.read_u32_le(plan.s + 236U), memory.read_u32_le(0x70002000U + 832U),
                     memory.read_u32_le(0x70002000U + 836U), memory.read_u32_le(0x70002000U + 840U), memory.read_u32_le(0x70002000U + 844U));
        for (std::size_t q = 0U; q < 12U && !st.submits.empty(); ++q) {
            const auto& w = st.submits[0].submit.qw[q];
            std::fprintf(stderr, "[NATIVECAR-DBG]   qw%zu: %08x %08x %08x %08x\n", q, w[0], w[1], w[2], w[3]);
        }
    }

    const std::uint32_t cursor = memory.read_u32_le(kCursorSlot);
    std::size_t pushed = 0U;
    std::uint32_t written = nc::emit_submits(
        *st.ring, st.submits,
        [&memory, cursor](const std::uint32_t index, const std::array<std::uint32_t, 4>& words) { memory.write_u128_le(cursor + 16U * index, words); }, pushed);
    for (std::size_t i = 0U; i < pushed; ++i) {
        c.batches += st.submits[i].submit.batch_count;
    }
    if (st.vu0_memory && pushed == st.submits.size() && !st.submits.empty()) {
        // VU0 data memory as microprogram 0xfb leaves it after the call's last part: the part matrix (the translation's w forced to 0), the
        // lighting matrix, the view vectors (970 at qw 16, 971..973 at qw 8..10). A later microprogram of the cars pass (the ground blob
        // drawn after the cars) reads them as its inputs; the native path has no VCALLMS, so the memory would otherwise stay stale.
        const auto& qw = st.submits.back().submit.qw;
        const auto put = [&memory](const std::uint32_t index, const std::uint32_t (&q)[4]) {
            memory.write_u128_le(0x11004000U + 16U * index, std::array<std::uint32_t, 4>{q[0], q[1], q[2], q[3]});
        };
        // qw 4..7: microprogram 0xfb's lit path (a part with VI3 != 0) builds the SH basis matrix there from fixed constants (0.429043,
        // 0.511664, 0.743125, 0.886227) before it rotates it; the unlit path (VI3 == 0) passes the previous lighting matrix through.
        static constexpr std::uint32_t kShBasis[4][4] = {
            {0x3edbab86U, 0x3edbab86U, 0x3edbab86U, 0x3f02fc69U},
            {0x3edbab86U, 0xbedbab86U, 0x3edbab86U, 0x3f02fc69U},
            {0x3edbab86U, 0x3edbab86U, 0x3f3e3d71U, 0x3f02fc69U},
            {0x3f02fc69U, 0x3f02fc69U, 0x3f02fc69U, 0x3f62dfc6U},
        };
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            put(i, qw[i]);
            put(4U + i, plan.parts[plan.part_count - 1U].lighting ? kShBasis[i] : qw[4U + i]);
        }
        for (std::uint32_t i = 0U; i < 3U; ++i) {
            put(8U + i, qw[9U + i]);
        }
        put(16U, qw[8]);
    }
    if (st.vu1_tail && pushed == st.submits.size() && !st.submits.empty()) {
        // VU1 962..1013 as the original leaves them after the call: every quadword the call's car program packets sent, with the value
        // of the last packet that sent it. The guest code that runs after a native call reads that memory as it would after the guest
        // DrawCar: a later guest DrawCar's part with VI3 == 0 passes the previous lighting matrix 966..969 through, and a part sends
        // only its first 4*T damage zones, the program reading the others (a vertex influence can address all 8) from earlier parts.
        // Zones are sent as a prefix of 974..1005, so what the call sent is 962..973 + 4*maxT zone quadwords and 1006..1013.
        std::uint32_t vu1[52][4]{};
        std::uint32_t max_t = 0U;
        for (const nc::PendingSubmit& p : st.submits) {
            const auto& q = p.submit.qw;
            const std::uint32_t t = std::min<std::uint32_t>(q[51][2] & 0xFFFFU, 8U);
            for (std::uint32_t i = 0U; i < 52U; ++i) {
                if (i < 12U || i >= 44U || i < 12U + 4U * t) {
                    std::memcpy(vu1[i], q[i], sizeof vu1[i]);
                }
            }
            max_t = std::max(max_t, t);
        }
        const std::uint32_t n1 = 12U + 4U * max_t;                    // 962.. (contiguous with 1006 when max_t == 8)
        const bool one = n1 == 44U;
        const std::uint32_t at = written;
        std::uint32_t k = at;
        const auto put = [&](const std::array<std::uint32_t, 4>& w) { memory.write_u128_le(cursor + 16U * k++, w); };
        const std::uint32_t total = 2U + (one ? 52U : n1 + 1U + 8U);
        put({(total - 1U) | (1U << 28U), 0U, 0U, 0U});                  // DMA CNT, VIF NOP NOP
        put({0U, 0U, 0x01000404U, 0x6c000000U | ((one ? 52U : n1) << 16U) | 962U});   // NOP NOP STCYCL 4,4 UNPACK V4-32
        for (std::uint32_t i = 0U; i < (one ? 52U : n1); ++i) {
            put({vu1[i][0], vu1[i][1], vu1[i][2], vu1[i][3]});
        }
        if (!one) {
            put({0U, 0U, 0U, 0x6c000000U | (8U << 16U) | 1006U});     // NOP NOP NOP UNPACK V4-32 8 at 1006
            for (std::uint32_t i = 44U; i < 52U; ++i) {
                put({vu1[i][0], vu1[i][1], vu1[i][2], vu1[i][3]});
            }
        }
        written += total;
        ++c.tails;
    }
    if (pushed == st.submits.size() && st.shading.lighting_valid) {
        // W+144..207: the lighting matrix of the call's last lit part, where the guest DrawEntryList leaves it. A later call whose first
        // part is unlit (VI3 == 0; declined here, drawn by the guest) passes it through as 966..969.
        const std::uint32_t work = memory.read_u32_le(plan.s + 256U);
        if (work >= 0x70000000U && work + 208U <= 0x70004000U) {
            for (std::uint32_t i = 0U; i < 4U; ++i) {
                const auto& l = st.shading.lighting[i];
                memory.write_u128_le(work + 144U + 16U * i, std::array<std::uint32_t, 4>{l[0], l[1], l[2], l[3]});
            }
        }
    }
    c.submits += pushed;
    c.stubs_qw += written;
    memory.write_u32_le(kCursorSlot, cursor + 16U * written);
    ++c.taken;
    if (static const std::uint64_t stats_every = [] { const char* v = std::getenv("TAKEDOWN_NATIVE_CAR_STATS_EVERY"); return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ULL; }();
        stats_every != 0U && c.taken % stats_every == 0U) {
        std::fprintf(stderr, "%s\n", native_draw_car_statistics().c_str());   // the harness leaves through _Exit: no summary at the end
    }

    cpu.set_pc(kDrawCarEpilogue);
    const std::uint32_t cost = std::min(kCeiling, kCostBase + kCostPerStub * static_cast<std::uint32_t>(st.submits.size()));
    return {.retired_instructions = cost, .ee_cycles = cost};
}

} // namespace

bool register_native_draw_car(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    if (g_state != nullptr) {
        return false;                                              // once per process
    }
    platform::CarSubmitRing* const ring = platform::native_car_submit_ring();
    if (ring == nullptr || native_function_disabled_by_environment("DrawCar")) {
        return false;
    }
    if (original_code_fnv1a64(memory, kDrawCarEntry, kDrawCarCodeBytes) != kDrawCarCodeFnv) {
        return false;                                              // another revision of the game
    }
    static State state;
    state.ring = ring;
    if (const char* tail = std::getenv("TAKEDOWN_NATIVE_CAR_VU1_TAIL"); tail != nullptr && *tail == '0') {
        state.vu1_tail = false;
    }
    if (const char* vu0 = std::getenv("TAKEDOWN_NATIVE_CAR_VU0_MEM"); vu0 != nullptr && *vu0 == '1') {
        state.vu0_memory = true;
    }
    if (const char* every = std::getenv("TAKEDOWN_NATIVE_CAR_DECLINE_EVERY"); every != nullptr && *every != '\0') {
        state.decline_every = static_cast<std::uint32_t>(std::strtoul(every, nullptr, 10));
    }
    const NativeFunctionTraits traits{
        .scheduler_state_transparent = true,
        .single_step_host_call = false,
        .defers_dmac_completions = true,
    };
    State* const shared = &state;
    if (const char* probe = std::getenv("TAKEDOWN_NATIVE_CAR_PROBE_EPILOGUE"); probe != nullptr && *probe == '1') {
        // Diagnostic: log the VU0 registers and the scratchpad record at DrawCar's epilogue (guest run and native run both pass it).
        static std::uint32_t probe_calls = 0U;
        const NativeFunctionTraits probe_traits{.scheduler_state_transparent = true, .single_step_host_call = false, .defers_dmac_completions = true};
        static_cast<void>(dispatcher.register_function(
            kDrawCarEpilogue, "DrawCarEpilogueProbe", NativeFunctionLimits{1U, 1U}, probe_traits,
            [](const NativeFunctionPrepareContext& context) -> std::optional<NativeFunctionLimits> {
                if (probe_calls >= 12U) {
                    return std::nullopt;
                }
                const std::uint32_t n = probe_calls++;
                for (std::size_t i = 1U; i < 32U; ++i) {
                    const guest::Gpr128 v = context.cpu.read_vu0_vector(i);
                    std::fprintf(stderr, "[EPILOGUE %u] vf%zu %08x %08x %08x %08x\n", n, i, v.words[0], v.words[1], v.words[2], v.words[3]);
                }
                const guest::Gpr128 acc = context.cpu.vu0_accumulator();
                std::fprintf(stderr, "[EPILOGUE %u] acc %08x %08x %08x %08x\n", n, acc.words[0], acc.words[1], acc.words[2], acc.words[3]);
                for (std::size_t i = 0U; i < 16U; ++i) {
                    std::fprintf(stderr, "[EPILOGUE %u] vi%zu %04x\n", n, i, context.cpu.read_vu0_integer(i));
                }
                for (std::uint32_t w = 0U; w < 0x500U; w += 16U) {
                    std::fprintf(stderr, "[EPILOGUE %u] W+%03x %08x %08x %08x %08x\n", n, w, context.memory.read_u32_le(0x70002000U + w),
                                 context.memory.read_u32_le(0x70002000U + w + 4U), context.memory.read_u32_le(0x70002000U + w + 8U),
                                 context.memory.read_u32_le(0x70002000U + w + 12U));
                }
                return std::nullopt;
            },
            [](NativeFunctionContext&) { return NativeFunctionExecution{}; }));
    }
    if (const char* dump_call = std::getenv("TAKEDOWN_NATIVE_CAR_DUMP_CALL"); dump_call != nullptr && *dump_call != '\0') {
        // Diagnostic: main RAM and the scratchpad to <TAKEDOWN_NATIVE_CAR_DUMP_FILE> when DrawCar reaches its epilogue the <n>th time.
        static std::uint64_t dump_target = std::strtoull(dump_call, nullptr, 10);
        static std::uint64_t dump_seen = 0U;
        const NativeFunctionTraits dump_traits{.scheduler_state_transparent = true, .single_step_host_call = false, .defers_dmac_completions = true};
        static_cast<void>(dispatcher.register_function(
            kDrawCarEpilogue, "DrawCarRamDump", NativeFunctionLimits{1U, 1U}, dump_traits,
            [](const NativeFunctionPrepareContext& context) -> std::optional<NativeFunctionLimits> {
                // The dispatcher may resolve one visit twice in a row; a visit is counted once per display list cursor value.
                static std::uint32_t last_cursor = 0xffffffffU;
                const std::uint32_t cursor = context.memory.read_u32_le(kCursorSlot);
                if (cursor == last_cursor) {
                    return std::nullopt;
                }
                last_cursor = cursor;
                if (dump_seen++ != dump_target) {
                    return std::nullopt;
                }
                const char* path = std::getenv("TAKEDOWN_NATIVE_CAR_DUMP_FILE");
                if (std::FILE* out = path != nullptr ? std::fopen(path, "wb") : nullptr) {
                    std::vector<std::uint32_t> buffer(0x4000U);
                    for (std::uint32_t base = 0U; base < 0x02000000U; base += 0x10000U) {
                        for (std::uint32_t i = 0U; i < 0x4000U; ++i) {
                            buffer[i] = context.memory.read_u32_le(base + 4U * i);
                        }
                        std::fwrite(buffer.data(), 4U, buffer.size(), out);
                    }
                    for (std::uint32_t i = 0U; i < 0x1000U; ++i) {
                        buffer[i] = context.memory.read_u32_le(0x70000000U + 4U * i);
                    }
                    std::fwrite(buffer.data(), 4U, 0x1000U, out);
                    for (std::uint32_t i = 0U; i < 0x400U; ++i) {       // VU0 data memory (0x11004000), 4 KiB
                        buffer[i] = context.memory.read_u32_le(0x11004000U + 4U * i);
                    }
                    std::fwrite(buffer.data(), 4U, 0x400U, out);
                    std::fclose(out);
                }
                return std::nullopt;
            },
            [](NativeFunctionContext&) { return NativeFunctionExecution{}; }));
    }
    if (const char* clobber = std::getenv("TAKEDOWN_NATIVE_CAR_CLOBBER"); clobber != nullptr && *clobber != '\0') {
        // Diagnostic (which state do the callers of DrawCar read after it?): zero the named groups when DrawCar reaches its epilogue.
        // Groups (comma separated): vf1 = VF1..14, vf2 = VF15..31, acc, vi = VI1..15, w = scratchpad record 0x70002000..+0x500.
        static const std::string groups = std::string(",") + clobber + ",";
        static bool pending_pass = false;
        const NativeFunctionTraits clobber_traits{.scheduler_state_transparent = true, .single_step_host_call = false, .defers_dmac_completions = true};
        static_cast<void>(dispatcher.register_function(
            kDrawCarEpilogue, "DrawCarClobber", NativeFunctionLimits{1U, 1U}, clobber_traits,
            [](const NativeFunctionPrepareContext&) -> std::optional<NativeFunctionLimits> {
                if (pending_pass) {
                    pending_pass = false;
                    return std::nullopt;
                }
                return NativeFunctionLimits{1U, 1U};
            },
            [](NativeFunctionContext& context) {
                const auto has = [](const char* name) { return groups.find(std::string(",") + name + ",") != std::string::npos; };
                if (has("vf1")) { for (std::size_t i = 1U; i <= 14U; ++i) { context.cpu.write_vu0_vector(i, guest::Gpr128{}); } }
                if (has("vf2")) { for (std::size_t i = 15U; i < 32U; ++i) { context.cpu.write_vu0_vector(i, guest::Gpr128{}); } }
                if (has("acc")) { context.cpu.set_vu0_accumulator(guest::Gpr128{}); }
                if (has("vi")) { for (std::size_t i = 1U; i < 16U; ++i) { context.cpu.write_vu0_integer(i, 0U); } }
                if (has("w")) { for (std::uint32_t w = 0U; w < 0x500U; w += 4U) { context.memory.write_u32_le(0x70002000U + w, 0U); } }
                pending_pass = true;
                return NativeFunctionExecution{1U, 1U};
            }));
    }
    const NativeFunctionRegistrationResult registration = dispatcher.register_function(
        kDrawCarAfterBind, "DrawCar", NativeFunctionLimits{kCeiling, kCeiling}, traits,
        [shared](const NativeFunctionPrepareContext& context) { return resolve(*shared, context); },
        [shared](NativeFunctionContext& context) { return run(*shared, context); });
    if (!registration.registered()) {
        return false;
    }
    g_state = shared;
    return true;
}

std::string native_draw_car_statistics() {
    if (g_state == nullptr) {
        return "[NATIVECAR] off";
    }
    const Counters& c = g_state->counters;
    char buffer[768];
    std::snprintf(buffer, sizeof buffer,
                  "[NATIVECAR] taken=%llu tails=%llu resolves=%llu flushes=%llu submits=%llu batches=%llu stub_qw=%llu | declined: plan=%llu unlit=%llu build=%llu "
                  "state=%llu ring=%llu space=%llu flush_again=%llu every=%llu | meshes: bad=%llu vertex_mismatch=%llu truncated=%llu cache=%zu/%zu bytes",
                  static_cast<unsigned long long>(c.taken), static_cast<unsigned long long>(c.tails), static_cast<unsigned long long>(c.resolves), static_cast<unsigned long long>(c.flushes),
                  static_cast<unsigned long long>(c.submits), static_cast<unsigned long long>(c.batches), static_cast<unsigned long long>(c.stubs_qw),
                  static_cast<unsigned long long>(c.declined_plan), static_cast<unsigned long long>(c.declined_unlit), static_cast<unsigned long long>(c.declined_build),
                  static_cast<unsigned long long>(c.declined_state), static_cast<unsigned long long>(c.declined_ring),
                  static_cast<unsigned long long>(c.declined_space), static_cast<unsigned long long>(c.declined_flush_again),
                  static_cast<unsigned long long>(c.declined_every), static_cast<unsigned long long>(c.build.bad_meshes),
                  static_cast<unsigned long long>(c.build.vertex_mismatch), static_cast<unsigned long long>(c.build.truncated),
                  g_state->cache.entries(), g_state->cache.bytes());
    return buffer;
}

} // namespace takedown::runtime::slus_21050
