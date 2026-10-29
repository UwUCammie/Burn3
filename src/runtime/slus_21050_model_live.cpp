#include "takedown/runtime/slus_21050_model_live.hpp"

#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/slus_21050_model_texture_resource.hpp"
#include "takedown/runtime/slus_21050_native_functions.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <xmmintrin.h>

namespace takedown::runtime::slus_21050 {
namespace {

// The retail parent routine this adapter replaces, and the 620 bytes of its
// code that must still be the shipped instructions before it is promoted.
constexpr guest::GuestAddress kParentEntry = 0x0021d2e0U;
constexpr std::size_t kParentBytes = 620U;
constexpr std::uint64_t kParentCodeHash = 8206309373423853396ULL;

// Console state behind the retail model parent: the batch globals, the VIF
// packet allocator and the 208-byte model state block it leaves behind.
constexpr guest::GuestAddress kGlobalPointer = 0x004e8670U;
constexpr guest::GuestAddress kModelState = 0x01e32a10U;
constexpr std::size_t kModelStateBytes = 208U;
constexpr std::size_t kQueueLimit = 64U;
// Tokens handed to the DMAC resolver: ours carry the high bit, a forwarded
// observer's are its own 1-based position.
constexpr std::uint64_t kClaimToken = 0x8000000000000000ULL;

struct GeometryArithmeticContext {
    int rounding{};
    int exceptions{};
    unsigned mxcsr{};
    unsigned short x87_control{};
    unsigned short x87_exceptions{};
    bool supported{};
    bool operator==(const GeometryArithmeticContext&) const = default;

    static GeometryArithmeticContext capture() {
        GeometryArithmeticContext context;
        context.rounding = std::fegetround();
        context.exceptions = std::fetestexcept(FE_ALL_EXCEPT);
        context.mxcsr = _mm_getcsr();
        unsigned short status = 0U;
        asm volatile("fnstcw %0" : "=m"(context.x87_control));
        asm volatile("fnstsw %0" : "=am"(status));
        context.x87_exceptions = static_cast<unsigned short>(status & 0x3fU);
        context.supported = context.rounding != -1;
        return context;
    }
};

}  // namespace

struct ModelLive::Impl {
    // One published queue item whose marker the VIF1 path has not yet reached.
    struct Claim {
        ModelProducerTicket ticket;
        std::uint32_t source{};
        std::array<std::uint32_t, 4> marker{};
        std::optional<guest::Vif1DmacPreSnapshotResolvedMarker> resolved;
    };
    // The packet allocator state the parent would have consumed.
    struct Reservation {
        std::uint32_t scratch{};
        std::uint32_t backing{};
        std::uint32_t reserved_end{};
        std::uint32_t system_header{};
        std::uint32_t system_qwords{};
        std::optional<ColdVifPacketReservation> cold;
    };
    struct Asset {
        std::uint64_t id{};
        std::shared_ptr<const ModelTextureAsset> asset;
    };
    struct Geometry {
        std::unique_ptr<ModelSource> source;
        PreparedModelRender model;
        GeometryArithmeticContext arithmetic;
    };
    struct Prepared {
        guest::EeMainRamMutationToken token;
        GeometryArithmeticContext arithmetic;
        std::array<std::uint32_t, 6> args;
        Reservation reservation;
        PreparedModelBatchFrame frame;
        ModelProducer::Transaction transaction;
        std::map<std::uint32_t, Asset> assets;
        std::list<Claim> claims;
        std::uint64_t next_resource{};
        std::uint64_t uploads{};
        std::uint64_t reuses{};
        std::array<std::array<std::uint32_t, 4>, 11> packet;
    };

    static unsigned geometry_workers_from_environment() {
        const char* value = std::getenv("TAKEDOWN_NATIVE_MODEL_GEOMETRY_WORKERS");
        if (value == nullptr || *value == '\0') {
            return 0U;
        }
        const std::string_view text{value};
        unsigned count = 0U;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || count > ModelGeometryExecutor::max_worker_threads) {
            throw std::invalid_argument("TAKEDOWN_NATIVE_MODEL_GEOMETRY_WORKERS must be 0 through 8");
        }
        return count;
    }

    static bool geometry_reuse_from_environment() {
        const char* value = std::getenv("TAKEDOWN_NATIVE_MODEL_GEOMETRY_REUSE");
        if (value == nullptr || *value == '\0') {
            return true;
        }
        if (std::string_view{value} == "1") {
            return true;
        }
        if (std::string_view{value} == "0") {
            return false;
        }
        throw std::invalid_argument("TAKEDOWN_NATIVE_MODEL_GEOMETRY_REUSE must be 0 or 1");
    }

    Impl(guest::Vif1Path2SourceMarkerObserver& f, Sink s) : forward(f), sink(std::move(s)) {
        const unsigned count = geometry_workers_from_environment();
        if (count != 0U) {
            geometry_executor = std::make_unique<ModelGeometryExecutor>(count);
        }
    }

    guest::Vif1Path2SourceMarkerObserver& forward;
    Sink sink;
    ModelProducer producer{ModelProducerLimits{.queued_models = 64U}};
    ModelTextureResources texture_resources{16U * 1024U * 1024U};
    ModelAssetResources model_assets{ModelAssetResourceLimits{}};
    std::unique_ptr<ModelGeometryExecutor> geometry_executor;
    ModelLiveGeometryReuseStatistics geometry_reuse{.enabled = geometry_reuse_from_environment()};
    std::unique_ptr<Geometry> geometry;
    ModelLiveStatistics stats;
    std::list<Claim> claims;
    std::map<std::uint32_t, Asset> assets;
    std::uint64_t next_resource{1};
    std::unique_ptr<Prepared> prepared;
    std::array<guest::Vif1DmacPreSnapshotCandidate, guest::kVif1DmacPreSnapshotCandidateCapacity> forwarded{};
    std::size_t forwarded_count{};
    std::exception_ptr failure;
    const char* failure_reason{};


    static std::optional<Reservation> plan_reservation(const guest::EeMemory& memory) {
        const std::uint32_t state = memory.read_u32_le(0x004e282cU);
        if (state == 0U) {
            const auto cold = plan_cold_vif_packet_reservation(memory, 11U);
            if (!cold) {
                return std::nullopt;
            }
            return Reservation{.scratch = cold->output.begin,
                               .backing = cold->backing_begin,
                               .reserved_end = cold->backing_end,
                               .system_header = 0U,
                               .system_qwords = 0U,
                               .cold = cold};
        }
        if ((state & 0x7fffffffU) != 0x1101U) {
            return std::nullopt;
        }
        const std::uint32_t backing = memory.read_u32_le(0x004e27c0U);
        const std::uint32_t scratch = memory.read_u32_le(0x004e27dcU);
        const std::uint32_t offset = memory.read_u32_le(0x004e27e0U);
        const std::uint32_t limit = memory.read_u32_le(0x004e2810U);
        if (scratch + 0x90000000U > 0x2000U || backing < scratch || (backing & 15U) != 0U ||
            (scratch & 15U) != 0U ||
            static_cast<std::uint64_t>(backing - scratch) + 240U > 0x2000U ||
            (offset & 15U) != 0U) {
            return std::nullopt;
        }
        const auto begin = static_cast<unsigned long long>(backing) + offset - scratch;
        const auto end = begin + 240U;
        if (end >= limit || end > memory.size()) {
            return std::nullopt;
        }
        std::uint32_t header = 0U;
        std::uint32_t count = 0U;
        if ((state & 0x80000000U) != 0U) {
            header = memory.read_u32_le(0x004e2828U);
            if (header == 0U || (header & 15U) != 0U || begin < header || begin - header <= 31U ||
                begin - header > 0x10000fU || !memory.is_main_ram_range(header, 16U)) {
                return std::nullopt;
            }
            count = static_cast<std::uint32_t>((begin - header) >> 4U) - 1U;
        }
        return Reservation{.scratch = backing,
                           .backing = static_cast<std::uint32_t>(begin),
                           .reserved_end = static_cast<std::uint32_t>(end),
                           .system_header = header,
                           .system_qwords = count};
    }

    static bool same_asset(const ModelTextureAsset& a, const ModelTextureAsset& b) {
        std::size_t i = 0U;
        if (a.palette != b.palette || a.mips.size() != b.mips.size()) {
            return false;
        }
        for (; i < a.mips.size(); ++i) {
            if (a.mips[i].width != b.mips[i].width || a.mips[i].height != b.mips[i].height ||
                a.mips[i].indices != b.mips[i].indices) {
                return false;
            }
        }
        return true;
    }

    // RECOVERY: GIMPLE differs from the original in extra local decls (same_asset/claim structure) and bb0 count; behaviour equivalent.
    bool prepare(const NativeFunctionPrepareContext& c) {
        if (failure || failure_reason != nullptr) {
            return false;
        }
        if (claims.size() >= kQueueLimit) {
            return false;
        }
        if (c.cpu.pc() != kParentEntry) {
            return false;
        }
        if (c.cpu.is_executing_delay_slot()) {
            return false;
        }
        if (c.cpu.read_gpr_u32(28U) != kGlobalPointer) {
            return false;
        }
        if (!c.memory.is_main_ram_range(kModelState, kModelStateBytes)) {
            return false;
        }
        if (c.memory.read_u32_le(0x0049b040U) != 0U) {
            return false;
        }
        std::array<std::uint32_t, 6> args{
            c.cpu.read_gpr_u32(4U),  c.cpu.read_gpr_u32(5U),  c.cpu.read_gpr_u32(6U),
            c.cpu.read_gpr_u32(7U),  c.cpu.read_gpr_u32(28U), c.cpu.read_gpr_u32(31U)};
        // An admitted parent stays valid until guest memory or the FP environment changes.
        const auto current_arithmetic = GeometryArithmeticContext::capture();
        if (prepared && current_arithmetic.supported && prepared->arithmetic == current_arithmetic &&
            prepared->args == args && producer.transaction_current(prepared->transaction) &&
            c.memory.validate_main_ram_mutation(prepared->token, 0U, c.memory.size())) {
            return true;
        }
        prepared.reset();

        const auto reservation = plan_reservation(c.memory);
        if (!reservation) {
            ++stats.declined_allocator;
            return false;
        }
        for (const auto& claim : claims) {
            if (claim.source >= reservation->backing && claim.source < reservation->backing + 176U) {
                failure_reason = "native model queue storage reused before its marker was consumed";
                return false;
            }
        }

        auto ram = c.memory.view(0U, c.memory.size());
        const auto frame = import_model_batch_frame(ram, kGlobalPointer);
        if (!frame) {
            geometry.reset();
            ++stats.declined_source;
            return false;
        }
        auto source = import_model_source(
            ram,
            {
                .root = args[0],
                .selection = {.group_mode = static_cast<std::int32_t>(args[1]),
                              .first_pass = args[2] != 0U,
                              .additional_passes = args[3] != 0U,
                              .pass_count = static_cast<std::int32_t>(c.memory.read_u32_le(0x00667054U))},
                .frame = *frame},
            ModelSourceAmbient{}, &texture_resources, &model_assets);
        if (!source) {
            geometry.reset();
            ++stats.declined_source;
            return false;
        }

        // Reuse the previous geometry only for the exact same inputs and FP mode.
        const auto arithmetic = GeometryArithmeticContext::capture();
        const bool reused = geometry_reuse.enabled && arithmetic.supported && geometry != nullptr &&
                            geometry->arithmetic == arithmetic &&
                            source->same_render_inputs(*geometry->source);
        std::optional<PreparedModelRender> fresh_model;
        const PreparedModelRender* model = nullptr;
        bool retain_fresh = false;
        if (reused) {
            model = &geometry->model;
            ++geometry_reuse.reuses;
        } else {
            ++geometry_reuse.preparations;
            fresh_model = geometry_executor != nullptr
                              ? prepare_model_render(source->input(), *geometry_executor)
                              : prepare_model_render(source->input());
            if (!fresh_model) {
                ++stats.declined_geometry;
                return false;
            }
            model = &*fresh_model;
            retain_fresh = geometry_reuse.enabled && arithmetic.supported &&
                           arithmetic == GeometryArithmeticContext::capture();
        }

        std::vector<std::uint64_t> ids;
        auto next = next_resource;
        for (const auto& texture : source->textures()) {
            const auto found = assets.find(texture.source_address);
            ids.push_back(found == assets.end() ? next++ : found->second.id);
        }
        auto textures = source->texture_bindings(*model, ids);
        if (!textures) {
            ++stats.declined_geometry;
            return false;
        }

        // Stage asset identities; only changed images become producer updates.
        auto staged_assets = assets;
        std::vector<ModelProducerTextureUpdate> updates;
        std::uint64_t uploads = 0U;
        std::uint64_t reuses = 0U;
        for (std::size_t i = 0U; i < ids.size(); ++i) {
            const auto& texture = source->textures()[i];
            const auto found = assets.find(texture.source_address);
            if (found != assets.end() &&
                (found->second.asset == texture.asset_owner() ||
                 same_asset(*found->second.asset, texture.asset))) {
                staged_assets.at(texture.source_address).asset = texture.asset_owner();
                ++reuses;
                continue;
            }
            updates.push_back({ids[i], &texture.asset});
            staged_assets.insert_or_assign(texture.source_address, Asset{ids[i], texture.asset_owner()});
            ++uploads;
        }

        auto transaction = [&]() -> std::optional<ModelProducer::Transaction> {
            if (reused || retain_fresh) {
                return producer.prepare_transaction(*model, source->raster_passes(), *textures,
                                                    updates, true);
            }
            return producer.prepare_transaction(std::move(*fresh_model), source->raster_passes(),
                                                *textures, updates, true);
        }();
        if (!transaction || (transaction->completion().ticket.value & kClaimToken) != 0U) {
            ++stats.declined_geometry;
            return false;
        }
        if (geometry_reuse.enabled && !reused) {
            if (retain_fresh) {
                geometry = std::make_unique<Geometry>(
                    Geometry{std::move(source), std::move(*fresh_model), arithmetic});
            } else {
                geometry.reset();
            }
        }

        // One GIF/VIF quadword: the low half of a 64-bit read, then a third word.
        const auto qword = [](std::uint64_t low, std::uint32_t word2) -> std::array<std::uint32_t, 4> {
            return {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U), word2, 0U};
        };
        // The compatibility packet: marker/outer-state restore plus the retail
        // register residue it must still leave in the console's batch registers.
        std::array<std::array<std::uint32_t, 4>, 11> packet{};
        packet[0] = {0x10000000U, 0U, 0x13000000U, 0U};
        packet[1] = {0x10000001U, 0U, 0U, 0x50000001U};
        packet[3] = {0x10000007U, 0U, 0U, 0x50000007U};
        packet[4] = {0x8006U, 0x10000000U, 14U, 0U};
        for (unsigned i = 0U; i < 4U; ++i) {
            const unsigned a = (419407U + i) * 16U;
            packet[5U + i] = qword(c.memory.read_u64_le(a), c.memory.read_u32_le(a + 8U));
            const std::array<std::uint32_t, 4> registers{76U, 78U, 24U, 64U};
            if (packet[5U + i][2] != registers[i] || c.memory.read_u32_le(a + 12U) != 0U) {
                return false;
            }
        }
        packet[9] = qword(c.memory.read_u64_le(0x004e1868U), 66U);
        packet[10] = qword(c.memory.read_u64_le(0x004e1840U), 71U);
        const ModelProducerTicket ticket = transaction->completion().ticket;
        packet[2] = {0x8000U, 0U, static_cast<std::uint32_t>(ticket.value),
                     static_cast<std::uint32_t>(ticket.value >> 32U) | 0xb3000000U};
        std::list<Claim> staged_claims;
        staged_claims.push_back(Claim{.ticket = ticket, .source = reservation->backing + 32U, .marker = packet[2]});
        prepared = std::make_unique<Prepared>(Prepared{
            .token = c.memory.capture_main_ram_mutation(0U, c.memory.size()).token,
            .arithmetic = GeometryArithmeticContext::capture(),
            .args = args,
            .reservation = *reservation,
            .frame = *frame,
            .transaction = std::move(*transaction),
            .assets = std::move(staged_assets),
            .claims = std::move(staged_claims),
            .next_resource = next,
            .uploads = uploads,
            .reuses = reuses,
            .packet = packet});
        return true;
    }

    // Leave the VIF packet allocator exactly as the retail parent would have.
    static void commit_allocator(guest::EeMemory& memory, const Reservation& reservation,
                                 const std::array<std::array<std::uint32_t, 4>, 11>& packet) {
        if (reservation.cold) {
            // The cold path commits the five allocator globals itself.
            commit_cold_vif_packet_reservation(memory, *reservation.cold);
        } else {
            memory.write_u32_le(0x004e2814U, reservation.reserved_end);
            if (reservation.system_header != 0U) {
                memory.write_u128_le(reservation.system_header,
                                     {reservation.system_qwords | 0x10000000U, 0U, 0U,
                                      reservation.system_qwords | 0x50000000U});
                memory.write_u32_le(0x004e2828U, 0U);
                memory.write_u32_le(0x004e282cU, 0x1101U);
            }
        }
        for (std::size_t i = 0U; i < packet.size(); ++i) {
            memory.write_u128_le(reservation.scratch + static_cast<std::uint32_t>(i) * 16U, packet[i]);
        }
        memory.write_u32_le(0x004e27c0U, reservation.scratch + 176U);
    }

    static std::uint64_t low_half(const std::array<std::uint32_t, 4>& quadword) {
        return quadword[0] | static_cast<std::uint64_t>(quadword[1]) << 32U;
    }

    NativeFunctionExecution execute(NativeFunctionContext& c) {
        if (!prepare({c.cpu, c.memory, c.entry_address})) {
            throw std::runtime_error("native model parent preparation changed");
        }
        auto call = std::move(prepared);
        const auto work = producer.commit_transaction(std::move(call->transaction));
        if (!work) {
            throw std::runtime_error("native model preparation became stale before commit");
        }
        assets.swap(call->assets);
        claims.splice(claims.end(), call->claims);
        next_resource = call->next_resource;
        stats.source_uploads += call->uploads;
        stats.source_reuses += call->reuses;
        commit_allocator(c.memory, call->reservation, call->packet);
        const auto write_vector = [&c](std::uint32_t offset, const std::array<float, 4>& value) {
            for (unsigned i = 0U; i < 4U; ++i) {
                c.memory.write_u32_le(kModelState + offset + i * 4U,
                                      std::bit_cast<std::uint32_t>(value[i]));
            }
        };
        write_vector(48U, work->completed_frame.eye);
        write_vector(64U, work->completed_frame.light);
        write_vector(80U, work->completed_frame.environment);
        write_vector(96U, work->completed_frame.reflection);
        c.memory.write_u32_le(kModelState + 204U,
                              std::bit_cast<std::uint32_t>(work->completed_frame.extra));
        c.memory.write_u32_le(kModelState + 200U,
                              std::bit_cast<std::uint32_t>(call->frame.texture_lod));
        const auto fog = c.memory.read_u64_le(0x004e1868U);
        c.memory.write_u32_le(kModelState + 192U, (static_cast<std::uint32_t>(fog) & 0xffU) |
                                                      (static_cast<std::uint32_t>(fog >> 8U) & 0xff000000U));
        c.memory.write_u32_le(kModelState + 196U, c.memory.read_u32_le(0x004e1840U));
        c.memory.write_u32_le(kModelState + 148U, 0U);
        c.memory.write_u8(kModelState + 152U, 0U);
        c.memory.write_u8(kModelState + 153U, 255U);
        c.memory.write_u8(kModelState + 154U, 0U);
        for (unsigned offset : {160U, 168U, 176U, 184U}) {
            c.memory.write_u64_le(kModelState + offset, 0U);
        }
        c.memory.write_u32_le(0x004e2a10U, 0U);
        c.memory.write_u32_le(0x00667050U, 0U);
        for (const auto [address, index] :
             std::array<std::pair<std::uint32_t, std::uint32_t>, 3>{
                 {{28216U, 5U}, {28224U, 6U}, {28200U, 7U}}}) {
            const auto& words = call->packet[index];
            c.memory.write_u64_le(kGlobalPointer - address, words[0] | static_cast<std::uint64_t>(words[1]) << 32U);
        }
        c.memory.write_u32_le(0x004e2860U, 0U);
        c.memory.write_u32_le(0x0049b064U, 0U);
        c.cpu.set_pc(call->args[5]);
        ++stats.admitted;
        return {1U, 1U};
    }
};

ModelLive::ModelLive(guest::Vif1Path2SourceMarkerObserver& f, Sink s)
    : impl_(std::make_unique<Impl>(f, std::move(s))) {}

ModelLive::~ModelLive() = default;

bool ModelLive::register_parent(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    // Promote only the shipped parent routine: its 620 code bytes are hashed (FNV-1a).
    const auto matches = [](const guest::EeMemory& ee) {
        if (!ee.is_main_ram_range(kParentEntry, kParentBytes)) {
            return false;
        }
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::byte byte : ee.view(kParentEntry, kParentBytes)) {
            hash = (hash ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
        }
        return hash == kParentCodeHash;
    };
    if (!matches(memory)) {
        return false;
    }
    auto page = memory.capture_main_ram_code_page(kParentEntry);
    return dispatcher
        .register_function(
            kParentEntry, "NativeModelParent", NativeFunctionLimits{1U, 1U},
            NativeFunctionTraits{.scheduler_state_transparent = true},
            [this, page, matches](const NativeFunctionPrepareContext& c) mutable
                -> std::optional<NativeFunctionLimits> {
                if (!c.memory.validate_main_ram_code_page(page.token)) {
                    if (!matches(c.memory)) {
                        return std::nullopt;
                    }
                    page = c.memory.capture_main_ram_code_page(kParentEntry);
                }
                if (impl_->prepare(c)) {
                    return NativeFunctionLimits{1U, 1U};
                }
                return std::nullopt;
            },
            [this](NativeFunctionContext& c) -> NativeFunctionExecution { return impl_->execute(c); })
        .registered();
}

bool ModelLive::failed() const noexcept {
    return static_cast<bool>(impl_->failure) || impl_->failure_reason != nullptr;
}

void ModelLive::throw_if_failed() const {
    if (impl_->failure) {
        std::rethrow_exception(impl_->failure);
    }
    if (impl_->failure_reason != nullptr) {
        throw std::runtime_error(impl_->failure_reason);
    }
}

ModelLiveStatistics ModelLive::statistics() const noexcept {
    auto result = impl_->stats;
    result.pending = impl_->claims.size();
    return result;
}

ModelProducerStatistics ModelLive::producer_statistics() const noexcept {
    return impl_->producer.statistics();
}

ModelGeometryExecutorStatistics ModelLive::geometry_statistics() const noexcept {
    if (impl_->geometry_executor != nullptr) {
        return impl_->geometry_executor->statistics();
    }
    return {};
}

ModelLiveGeometryReuseStatistics ModelLive::geometry_reuse_statistics() const noexcept {
    return impl_->geometry_reuse;
}

ModelAssetResourceStatistics ModelLive::asset_statistics() const noexcept {
    return impl_->model_assets.statistics();
}

bool ModelLive::has_vif1_dmac_pre_snapshot_candidates() const noexcept {
    return !impl_->claims.empty() || impl_->forward.has_vif1_dmac_pre_snapshot_candidates();
}


std::size_t ModelLive::on_vif1_dmac_pre_snapshot_candidates(
    const guest::Vif1DmacPreSnapshotObservation& o,
    std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept {
    auto& impl = *impl_;
    impl.forwarded_count = 0U;
    std::size_t count = 0U;
    // Our own unresolved claims come first, tagged with the high token bit.
    for (const auto& claim : impl.claims) {
        if (!claim.resolved && count < candidates.size()) {
            candidates[count++] = guest::Vif1DmacPreSnapshotCandidate{
                claim.ticket.value | kClaimToken, claim.source, claim.marker};
        }
    }
    if (impl.forward.has_vif1_dmac_pre_snapshot_candidates()) {
        const auto room = std::min(candidates.size() - count, impl.forwarded.size());
        impl.forwarded_count = std::min(
            impl.forward.on_vif1_dmac_pre_snapshot_candidates(
                o, std::span{impl.forwarded}.first(room)),
            room);
        for (std::size_t i = 0; i < impl.forwarded_count; ++i) {
            candidates[count] = impl.forwarded[i];
            candidates[count++].token = i + 1U;
        }
    }
    return count;
}

// RECOVERY: GIMPLE differs only in compare operand order of the loop condition (SSA version numbering); logic identical.
void ModelLive::on_vif1_dmac_pre_snapshot_markers_resolved(
    std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
    std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept {
    auto& impl = *impl_;
    std::array<guest::Vif1DmacPreSnapshotResolvedMarker, guest::kVif1DmacPreSnapshotCandidateCapacity> forwarded{};
    std::array<guest::Vif1DmacPreSnapshotMarkerDecision, guest::kVif1DmacPreSnapshotCandidateCapacity> answers{};
    std::array<std::size_t, guest::kVif1DmacPreSnapshotCandidateCapacity> positions{};
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < std::min(markers.size(), decisions.size()); ++i) {
        const auto& marker = markers[i];
        decisions[i] = guest::Vif1DmacPreSnapshotMarkerDecision::Reject;
        if ((marker.token & kClaimToken) != 0U) {
            const auto ticket = marker.token & ~kClaimToken;
            const auto claim = std::find_if(impl.claims.begin(), impl.claims.end(), [&](const auto& c) {
                return c.ticket.value == ticket;
            });
            if (claim != impl.claims.end() && !claim->resolved &&
                marker.candidate_source == claim->source &&
                marker.request.expected_quadword == claim->marker && !impl.failure &&
                impl.failure_reason == nullptr) {
                claim->resolved = marker;
                decisions[i] = guest::Vif1DmacPreSnapshotMarkerDecision::Accept;
                ++impl.stats.resolved;
            }
        } else if (marker.token != 0U && marker.token <= impl.forwarded_count &&
                   count < forwarded.size()) {
            forwarded[count] = marker;
            forwarded[count].token = impl.forwarded[marker.token - 1U].token;
            positions[count++] = i;
        }
    }
    if (count != 0U) {
        impl.forward.on_vif1_dmac_pre_snapshot_markers_resolved(
            std::span{forwarded}.first(count),
            std::span{answers}.first(count));
    }
    for (std::size_t i = 0U; i < count; ++i) {
        decisions[positions[i]] = answers[i];
    }
}

void ModelLive::on_vif1_path2_source_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    auto& impl = *impl_;
    const auto owner = std::find_if(impl.claims.begin(), impl.claims.end(), [&](const auto& claim) {
        return claim.resolved && claim.resolved->generation == marker.generation;
    });
    if (owner == impl.claims.end()) {
        impl.forward.on_vif1_path2_source_marker_reached(marker);
        return;
    }
    if (impl.failure || impl.failure_reason != nullptr) {
        return;
    }
    // Queue items are consumed in order, and only for the exact marker armed.
    const auto same_marker = [](const guest::Vif1DmacPreSnapshotResolvedMarker& armed,
                                const guest::Vif1Path2SourceMarkerReached& reached) {
        return armed.generation == reached.generation && armed.request.source == reached.source &&
               armed.request.chain_root == reached.chain_root &&
               armed.request.source_tag_location == reached.source_tag_location &&
               armed.request.source_tag_ordinal == reached.source_tag_ordinal &&
               armed.request.source_payload_word_ordinal == reached.source_payload_word_ordinal &&
               armed.request.expected_quadword == reached.quadword;
    };
    if (owner != impl.claims.begin() || !same_marker(*owner->resolved, marker)) {
        impl.failure_reason = "native model marker order or source identity mismatch";
        return;
    }
    if (impl.producer.publish(owner->ticket, impl.sink) != ModelProducerPublish::Published) {
        impl.failure = impl.producer.failure();
        impl.failure_reason = "native model sink could not consume its ordered queue item";
        return;
    }
    impl.claims.pop_front();
    ++impl.stats.published;
}

void ModelLive::on_vif1_path2_source_marker_cancelled(
    const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    auto& impl = *impl_;
    for (const auto& claim : impl.claims) {
        if (claim.resolved && claim.resolved->generation == marker.generation) {
            ++impl.stats.cancelled;
            impl.failure_reason = "native model source marker cancelled after parent retirement";
            return;
        }
    }
    impl.forward.on_vif1_path2_source_marker_cancelled(marker);
}

void ModelLive::on_vif1_path2_source_marker_memory_cleared() noexcept {
    auto& impl = *impl_;
    if (!impl.claims.empty()) {
        impl.failure_reason = "native model memory reset with outstanding queue items";
    }
    impl.forward.on_vif1_path2_source_marker_memory_cleared();
}

}  // namespace takedown::runtime::slus_21050
