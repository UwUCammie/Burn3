#include "takedown/guest/gs_live_renderer.hpp"

namespace takedown::guest {

// RECOVERY: surface temporary is non-const in this build; body otherwise matches.
void GsLiveRenderer::on_gs_image_transfer(const GsImageTransfer& transfer) {
    const auto result = local_memory_.apply_host_to_local(transfer);
    if (result.applied()) {
        rasterizer_.invalidate_texture_cache_region(
            GsLocalMemorySurface{
                .base_pointer = transfer.destination_base_pointer,
                .buffer_width = transfer.destination_buffer_width,
                .pixel_storage_mode = transfer.destination_psm,
            },
            transfer.destination_x, transfer.destination_y, transfer.width, transfer.height);
        ++statistics_.image_transfers_applied;
        ++statistics_.framebuffer_revision;
    } else {
        ++statistics_.image_transfers_rejected;
    }
}

namespace {

[[nodiscard]] const std::shared_ptr<const GsRasterClut>& empty_clut() {
    static const std::shared_ptr<const GsRasterClut> snapshot =
        std::make_shared<const GsRasterClut>();
    return snapshot;
}

// RECOVERY: the original keeps this helper out of line (it has two callers there); here it is inlined into load_csm1_clut.
// Expands a CSM1 CLUT block from the raw GS local-memory backing. Addresses wrap
// within the backing span, matching the GS memory image.
void populate_csm1_clut(GsRasterClut& destination, std::span<const std::uint8_t> backing,
                        std::uint64_t tex0) {
    const auto texture_psm = static_cast<std::uint32_t>(tex0 >> 20U) & 63U;
    const auto cbp = static_cast<std::uint32_t>(tex0 >> 37U) & 16383U;
    const auto csa = static_cast<std::uint32_t>(tex0 >> 56U) & 15U;

    const auto copy_entry = [&](std::uint32_t destination_index, std::uint32_t index) {
        const std::uint32_t swizzled = (index & 0x81U) | ((index << 1U) & 0x0cU) |
                                       ((index >> 2U) & 0x02U) | ((index << 2U) & 0x40U) |
                                       ((index >> 1U) & 0x30U);
        const std::uint64_t byte =
            ((static_cast<std::uint64_t>(cbp) * 64U + swizzled) * 4U) % backing.size();
        auto& entry = destination.entries[destination_index];
        entry.red = backing[byte];
        entry.green = backing[(byte + 1U) % backing.size()];
        entry.blue = backing[(byte + 2U) % backing.size()];
        entry.alpha = backing[(byte + 3U) % backing.size()];
    };

    if (texture_psm == kGsPsmt4) {
        const auto first = csa << 4U;
        for (std::uint32_t index = 0; index != 16U; ++index) {
            copy_entry(first + index, index);
        }
    } else {
        for (std::uint32_t index = csa << 4U; index != 256U; ++index) {
            copy_entry(index, index);
        }
    }
}

} // namespace

const char* to_string(const GsDrawRejectionReason reason) noexcept {
    switch (reason) {
    case GsDrawRejectionReason::UnsupportedTopology:
        return "unsupported topology";
    case GsDrawRejectionReason::MissingFrame:
        return "missing FRAME";
    case GsDrawRejectionReason::MissingXyoffset:
        return "missing XYOFFSET";
    case GsDrawRejectionReason::MissingScissor:
        return "missing SCISSOR";
    case GsDrawRejectionReason::MissingTex0:
        return "missing TEX0";
    case GsDrawRejectionReason::MissingRgbaq:
        return "missing RGBAQ";
    case GsDrawRejectionReason::MissingUv:
        return "missing UV";
    case GsDrawRejectionReason::MissingStq:
        return "missing ST/Q";
    case GsDrawRejectionReason::MissingAlpha:
        return "missing ALPHA";
    case GsDrawRejectionReason::InvalidFramebufferWidth:
        return "zero framebuffer width";
    case GsDrawRejectionReason::InvalidScissor:
        return "invalid scissor";
    case GsDrawRejectionReason::UnsupportedFixedUvTriangle:
        return "fixed-UV triangle unsupported";
    case GsDrawRejectionReason::InvalidStq:
        return "invalid ST/Q";
    case GsDrawRejectionReason::UnsupportedFramebufferTexturePair:
        return "unsupported framebuffer/texture PSM pair";
    case GsDrawRejectionReason::OversizedTexture:
        return "unsupported texture dimensions";
    case GsDrawRejectionReason::UnsupportedTextureFunction:
        return "unsupported texture function";
    case GsDrawRejectionReason::MissingTexa:
        return "missing TEXA";
    case GsDrawRejectionReason::UnsupportedClutPixelStorageMode:
        return "unsupported CLUT PSM";
    case GsDrawRejectionReason::UnsupportedClutStorageMode:
        return "unsupported CLUT storage mode";
    case GsDrawRejectionReason::EmptyRasterization:
        return "empty rasterization";
    case GsDrawRejectionReason::UnsupportedFramebufferFormat:
        return "unsupported framebuffer format";
    case GsDrawRejectionReason::UnsupportedAlphaBlend:
        return "unsupported alpha blend";
    case GsDrawRejectionReason::InvalidRasterTarget:
        return "invalid raster target";
    case GsDrawRejectionReason::Count:
        break;
    }
    return "unknown draw rejection";
}

const char* to_string(const GsReplayStateRestoreStatus status) noexcept {
    switch (status) {
    case GsReplayStateRestoreStatus::Applied:
        return "applied";
    case GsReplayStateRestoreStatus::InvalidLocalMemorySize:
        return "invalid local-memory size";
    case GsReplayStateRestoreStatus::ActiveGifPath:
        return "serialized GIF path is active";
    case GsReplayStateRestoreStatus::IncompleteTransfer:
        return "serialized image transfer is incomplete";
    }
    return "unknown GS replay-state status";
}

GsLiveRenderer::GsLiveRenderer() : rasterizer_(local_memory_), clut_(empty_clut()) {
    statistics_.raster_worker_threads = rasterizer_.threading_statistics().worker_threads;
}

void GsLiveRenderer::on_gs_gif_tag_begin(const GifPath path) {
    static_cast<void>(path);
    packed_q_bits_ = 0x3f800000U;
}

void GsLiveRenderer::load_csm1_clut(const std::uint64_t tex0) {
    auto next = std::make_shared<GsRasterClut>(*clut_);
    populate_csm1_clut(*next, local_memory_.raw_backing(), tex0);
    const auto version = ++next_clut_version_;
    next->version = version;
    statistics_.clut_version = version;
    clut_ = std::move(next);
}

// RECOVERY: dead comparisons and PSM range test layout differ.
void GsLiveRenderer::process_clut_write(const std::uint8_t address) {
    std::size_t context{};
    switch (address) {
    case 0x06:
    case 0x16:
        break;
    case 0x07:
    case 0x17:
        context = 1U;
        break;
    default:
        return;
    }

    const auto& context_state = assembler_.state().contexts[context];
    if (!context_state.tex0.known) {
        return;
    }
    const auto tex0 = context_state.tex0.raw;
    const auto texture_psm = static_cast<std::uint32_t>(tex0 >> 20U) & 63U;
    const bool clut_indexed_psm = texture_psm == kGsPsmt8h || (texture_psm - kGsPsmt8) <= 1U;
    if (!clut_indexed_psm) {
        return;
    }

    const auto cbp = static_cast<std::uint32_t>(tex0 >> 37U) & 16383U;
    const auto condition = static_cast<std::uint32_t>(tex0 >> 61U);
    bool load = false;
    switch (condition) {
    case 1:
        load = true;
        break;
    case 2:
        clut_cbp_slots_[0] = cbp;
        load = true;
        break;
    case 3:
        clut_cbp_slots_[1] = cbp;
        load = true;
        break;
    case 4:
        if (clut_cbp_slots_[0] != cbp) {
            clut_cbp_slots_[0] = cbp;
            load = true;
        }
        break;
    case 5:
        if (clut_cbp_slots_[1] != cbp) {
            clut_cbp_slots_[1] = cbp;
            load = true;
        }
        break;
    default:
        break;
    }

    if (!load) {
        ++statistics_.clut_loads_skipped;
        ++statistics_.clut_skips_by_condition[condition];
        return;
    }

    const bool csm2 = (tex0 & (std::uint64_t{1} << 55U)) != 0U;
    if ((static_cast<std::uint32_t>(tex0 >> 51U) & 14U) != 0U || csm2) {
        ++statistics_.clut_loads_deferred;
        ++statistics_.clut_deferred_by_condition[condition];
        return;
    }

    load_csm1_clut(tex0);
    ++statistics_.clut_loads_applied;
    ++statistics_.clut_loads_by_condition[condition];
}

void GsLiveRenderer::reset_drawing_state() noexcept {
    assembler_.reset();
    pending_vertex_count_ = 0;
    pending_topology_.reset();
    texa_ = 0;
    texa_known_ = false;
    clut_ = empty_clut();
    clut_cbp_slots_ = {};
    next_clut_version_ = 0;
    statistics_.clut_version = 0;
    bitbltbuf_ = 0;
    trxpos_ = 0;
    trxreg_ = 0;
    packed_q_bits_ = 0x3f800000U;
    colclamp_ = 0;
    colclamp_known_ = false;
    fogcol_ = 0;
}

void GsLiveRenderer::on_gs_reset() {
    rasterizer_.invalidate_texture_cache();
    reset_drawing_state();
    ++statistics_.gs_soft_resets;
}

void GsLiveRenderer::reset() noexcept {
    local_memory_.clear();
    rasterizer_.invalidate_texture_cache();
    statistics_ = {};
    rasterizer_.reset_threading_statistics();
    statistics_.raster_worker_threads = rasterizer_.threading_statistics().worker_threads;
    reset_drawing_state();
}

// RECOVERY: packed-address range tests and branch layout differ.
void GsLiveRenderer::on_gs_register_write(const GsRegisterWrite& write) {
    ++statistics_.register_writes;
    process_transfer_write(write);

    // TEXFLUSH invalidates the cached texture view of local memory.
    if (write.address == 0x3fU) {
        rasterizer_.invalidate_texture_cache();
    }
    if (write.address == 0x3bU) {
        texa_ = write.value;
        texa_known_ = true;
    }
    if (write.address == 0x46U) {
        colclamp_ = write.value;
        colclamp_known_ = true;
    }
    if (write.address == 0x3dU) {
        fogcol_ = write.value & 0xffffffU;
    }
    if (write.address == 0x00U) {
        pending_vertex_count_ = 0;
        pending_topology_.reset();
    }

    // A PACKED position with the ADC bit set is written but does not kick.
    const bool packed_adc = write.source == GsRegisterWriteSource::Packed &&
                            static_cast<std::uint8_t>(write.address - 4U) <= 1U &&
                            (static_cast<std::uint32_t>(write.packed_high >> 32U) & 0x8000U) != 0U;
    const bool position3 = static_cast<std::uint8_t>(write.address - 12U) <= 1U;
    const auto kick = forward_register_write(write);
    process_clut_write(write.address);
    if (packed_adc || position3) {
        if (packed_adc) {
            ++statistics_.adc_suppressed_positions;
        } else {
            ++statistics_.nonkicking_positions;
        }
    }

    if (const auto vertex = assembler_.current_vertex_snapshot(); vertex) {
        handle_kick(*vertex, true);
    }
    if (kick) {
        ++statistics_.drawable_kicks;
        handle_kick(*kick, false);
    }
}

// RECOVERY: statement layout of the packed-register switch differs.
std::optional<GsPrimitiveKickSnapshot>
GsLiveRenderer::forward_register_write(const GsRegisterWrite& write) noexcept {
    if (write.source != GsRegisterWriteSource::Packed) {
        return assembler_.write(GsAssemblerRegisterWrite{
            .address = write.address,
            .value = write.value,
        });
    }

    switch (write.address) {
    case 1U:
        return assembler_.write(GsRegisterAddress::Rgbaq,
                                canonical_packed_rgba(write.value, write.packed_high, packed_q_bits_));
    case 2U: {
        packed_q_bits_ = static_cast<std::uint32_t>(write.packed_high);
        const auto current_rgba = assembler_.state().vertex.rgbaq;
        const auto rgba = current_rgba.known ? static_cast<std::uint32_t>(current_rgba.raw) : 0U;
        static_cast<void>(assembler_.write(GsRegisterAddress::Rgbaq,
                                           static_cast<std::uint64_t>(rgba) | (write.packed_high << 32U)));
        return assembler_.write(GsRegisterAddress::St, write.value);
    }
    case 3U:
        return assembler_.write(GsRegisterAddress::Uv, canonical_packed_uv(write.value));
    case 4U:
    case 5U:
        return assembler_.write_packed_position(GsPackedPositionWrite{
            .address = static_cast<GsRegisterAddress>(write.address),
            .payload = {.low = write.value, .high = write.packed_high},
        });
    case 12U:
    case 13U:
        return assembler_.write(GsAssemblerRegisterWrite{
            .address = write.address,
            .value = canonical_packed_position(write.address, write.value, write.packed_high),
        });
    default:
        return assembler_.write(GsAssemblerRegisterWrite{
            .address = write.address,
            .value = write.value,
        });
    }
}

std::uint64_t GsLiveRenderer::canonical_packed_rgba(const std::uint64_t low, const std::uint64_t high,
                                                    const std::uint32_t q_bits) noexcept {
    const auto red = static_cast<std::uint32_t>(low) & 0xffU;
    const auto green = static_cast<std::uint32_t>(low >> 32U) & 0xffU;
    const auto blue = static_cast<std::uint32_t>(high) & 0xffU;
    const auto alpha = static_cast<std::uint32_t>(high >> 32U) & 0xffU;
    return static_cast<std::uint64_t>(red) | (static_cast<std::uint64_t>(green) << 8U) |
           (static_cast<std::uint64_t>(blue) << 16U) | (static_cast<std::uint64_t>(alpha) << 24U) |
           (static_cast<std::uint64_t>(q_bits) << 32U);
}

std::uint64_t GsLiveRenderer::canonical_packed_uv(const std::uint64_t low) noexcept {
    const auto u = static_cast<std::uint32_t>(low) & 16383U;
    const auto v = static_cast<std::uint32_t>(low >> 32U) & 16383U;
    return static_cast<std::uint64_t>(u) | (static_cast<std::uint64_t>(v) << 16U);
}

// RECOVERY: local-to-local transfer padding init and branch layout differ.
void GsLiveRenderer::process_transfer_write(const GsRegisterWrite& write) noexcept {
    switch (write.address) {
    case 0x50U:
        bitbltbuf_ = write.value;
        break;
    case 0x51U:
        trxpos_ = write.value;
        break;
    case 0x52U:
        trxreg_ = write.value;
        break;
    case 0x53U: {
        if ((write.value & 3U) != 2U) {
            break;
        }
        GsLocalToLocalTransfer transfer;
        transfer.source.base_pointer = static_cast<std::uint32_t>(bitbltbuf_) & 16383U;
        transfer.source.buffer_width = static_cast<std::uint32_t>(bitbltbuf_ >> 16U) & 63U;
        transfer.source.pixel_storage_mode = static_cast<std::uint32_t>(bitbltbuf_ >> 24U) & 63U;
        transfer.destination.base_pointer = static_cast<std::uint32_t>(bitbltbuf_ >> 32U) & 16383U;
        transfer.destination.buffer_width = static_cast<std::uint32_t>(bitbltbuf_ >> 48U) & 63U;
        transfer.destination.pixel_storage_mode =
            static_cast<std::uint32_t>(bitbltbuf_ >> 56U) & 63U;
        transfer.source_x = static_cast<std::uint32_t>(trxpos_) & 2047U;
        transfer.source_y = static_cast<std::uint32_t>(trxpos_ >> 16U) & 2047U;
        transfer.destination_x = static_cast<std::uint32_t>(trxpos_ >> 32U) & 2047U;
        transfer.destination_y = static_cast<std::uint32_t>(trxpos_ >> 48U) & 2047U;
        transfer.width = static_cast<std::uint32_t>(trxreg_) & 4095U;
        transfer.height = static_cast<std::uint32_t>(trxreg_ >> 32U) & 4095U;
        transfer.reverse_x = (trxpos_ & (std::uint64_t{1} << 60U)) != 0U;
        transfer.reverse_y = (trxpos_ & (std::uint64_t{1} << 59U)) != 0U;

        const auto result = local_memory_.apply_local_to_local(transfer);
        if (result.applied()) {
            rasterizer_.invalidate_texture_cache_region(transfer.destination, transfer.destination_x,
                                                        transfer.destination_y, transfer.width,
                                                        transfer.height);
            ++statistics_.local_to_local_transfers_applied;
            ++statistics_.framebuffer_revision;
        } else {
            ++statistics_.local_to_local_transfers_rejected;
        }
        break;
    }
    default:
        break;
    }
}

// RECOVERY: statement order of the position field decode differs.
std::uint64_t GsLiveRenderer::canonical_packed_position(const std::uint8_t address,
                                                        const std::uint64_t low,
                                                        const std::uint64_t high) noexcept {
    const auto y_bits = low >> 32U;
    const auto word2 = static_cast<std::uint32_t>(high);
    const auto word3 = static_cast<std::uint32_t>(high >> 32U);
    const auto x = low & 0xffffU;
    const auto y = (y_bits << 16U) & 0xffffffffU;
    if (address == 12U) {
        const auto z = (static_cast<std::uint64_t>(word2 >> 4U) << 32U) & 0x00ffffff00000000U;
        const auto fog = static_cast<std::uint64_t>(word3 >> 4U) << 56U;
        return x | y | z | fog;
    }
    return x | y | (high << 32U);
}

const GsLocalMemory& GsLiveRenderer::local_memory() const noexcept {
    return local_memory_;
}

GsLocalMemory& GsLiveRenderer::local_memory() noexcept {
    return local_memory_;
}

const GsLiveRendererStatistics& GsLiveRenderer::statistics() const noexcept {
    return statistics_;
}

const GsDrawingContextState& GsLiveRenderer::current_context1_state() const noexcept {
    return assembler_.state().contexts[0];
}

const GsPrimitiveAssemblerState& GsLiveRenderer::current_drawing_state() const noexcept {
    return assembler_.state();
}

// RECOVERY: optional-state branch order differs from the original.
std::optional<std::uint64_t>
GsLiveRenderer::current_tex0(const GsDrawingContext context) const noexcept {
    std::optional<std::size_t> index;
    if (context == GsDrawingContext::Context1) {
        index = 0U;
    } else if (context == GsDrawingContext::Context2) {
        index = 1U;
    }
    if (!index) {
        return {};
    }
    const auto& tex0 = assembler_.state().contexts[*index].tex0;
    if (tex0.known) {
        return tex0.raw;
    }
    return {};
}

std::optional<std::uint64_t> GsLiveRenderer::current_colclamp() const noexcept {
    if (colclamp_known_) {
        return colclamp_;
    }
    return {};
}

void GsLiveRenderer::reset_pending_vertices() noexcept {
    pending_vertex_count_ = 0;
    pending_topology_.reset();
}

void GsLiveRenderer::record_rejection_details(const GsDrawRejectionReason reason,
                                              const std::optional<std::uint32_t> framebuffer_psm,
                                              const std::optional<std::uint32_t> texture_psm,
                                              const std::optional<std::uint32_t> clut_psm) noexcept {
    const auto reason_index = static_cast<std::size_t>(reason);
    if (reason_index < kGsDrawRejectionReasonCount) {
        ++statistics_.rejected_primitives_by_reason[reason_index];
    }
    if (framebuffer_psm && *framebuffer_psm < kGsPixelStorageModeCount) {
        ++statistics_.rejected_framebuffer_psm[*framebuffer_psm];
    }
    if (texture_psm && *texture_psm < kGsPixelStorageModeCount) {
        ++statistics_.rejected_texture_psm[*texture_psm];
    }
    if (clut_psm && *clut_psm < kGsClutPixelStorageModeCount) {
        ++statistics_.rejected_clut_psm[*clut_psm];
    }
}

void GsLiveRenderer::census_texture(const std::uint32_t texture_psm,
                                    const std::uint32_t texture_function,
                                    const std::uint32_t clut_format) noexcept {
    if (texture_psm < kGsPixelStorageModeCount) {
        ++statistics_.drawn_texture_psm[texture_psm];
    }
    if (texture_function < 4U) {
        ++statistics_.drawn_texture_tfx[texture_function];
    }
    if (clut_format < kGsClutPixelStorageModeCount) {
        ++statistics_.drawn_texture_clut[clut_format];
    }
}

void GsLiveRenderer::record_deferred(const GsPrimitiveTopology topology,
                                     const GsDrawRejectionReason reason,
                                     const std::optional<std::uint32_t> framebuffer_psm,
                                     const std::optional<std::uint32_t> texture_psm,
                                     const std::optional<std::uint32_t> clut_psm) noexcept {
    ++statistics_.textured_primitives_deferred;
    ++statistics_.deferred_primitives_by_topology[static_cast<std::size_t>(topology)];
    record_rejection_details(reason, framebuffer_psm, texture_psm, clut_psm);
}

void GsLiveRenderer::record_incomplete(const GsPrimitiveTopology topology,
                                       const GsDrawRejectionReason reason,
                                       const std::optional<std::uint32_t> framebuffer_psm,
                                       const std::optional<std::uint32_t> texture_psm,
                                       const std::optional<std::uint32_t> clut_psm) noexcept {
    ++statistics_.incomplete_state_primitives;
    ++statistics_.incomplete_state_primitives_by_topology[static_cast<std::size_t>(topology)];
    record_rejection_details(reason, framebuffer_psm, texture_psm, clut_psm);
}

// RECOVERY: field stores are reordered relative to the original.
GsRasterVertex GsLiveRenderer::raster_vertex(const GsVertexState& vertex) noexcept {
    const auto position = vertex.position.raw;
    const bool fogged = vertex.position_kind == GsVertexPositionKind::XyzWithFog;
    const auto rgbaq = vertex.rgbaq.raw;
    const auto uv = vertex.uv.raw;
    const auto st = vertex.st.raw;
    return GsRasterVertex{
        .x_12_4 = static_cast<std::uint16_t>(position),
        .y_12_4 = static_cast<std::uint16_t>(position >> 16U),
        .z = fogged ? static_cast<std::uint32_t>(position >> 32U) & 0xffffffU
                    : static_cast<std::uint32_t>(position >> 32U),
        .fog = fogged ? static_cast<std::uint8_t>(position >> 56U) : std::uint8_t{},
        .color = GsRgba8{
            .red = static_cast<std::uint8_t>(rgbaq),
            .green = static_cast<std::uint8_t>(rgbaq >> 8U),
            .blue = static_cast<std::uint8_t>(rgbaq >> 16U),
            .alpha = static_cast<std::uint8_t>(rgbaq >> 24U),
        },
        .u_10_4 = static_cast<std::int32_t>(uv) & 16383,
        .v_10_4 = static_cast<std::int32_t>(uv >> 16U) & 16383,
        .s_bits = static_cast<std::uint32_t>(st),
        .t_bits = static_cast<std::uint32_t>(st >> 32U),
        .q_bits = static_cast<std::uint32_t>(rgbaq >> 32U),
    };
}

// RECOVERY: unsupported-primitive counter index is a runtime value in this build.
void GsLiveRenderer::handle_kick(const GsPrimitiveKickSnapshot& kick,
                                 const bool suppress_draw) noexcept {
    const auto topology = kick.controls.topology;
    if (!pending_topology_ || *pending_topology_ != topology) {
        pending_vertex_count_ = 0;
        pending_topology_ = topology;
    }

    switch (topology) {
    case GsPrimitiveTopology::Point:
        if (!suppress_draw) {
            if (kick.controls.texture_mapping_enabled) {
                ++statistics_.unsupported_primitives;
                ++statistics_.unsupported_primitives_by_topology[static_cast<std::size_t>(GsPrimitiveTopology::Point)];
                record_rejection_details(GsDrawRejectionReason::UnsupportedTopology,
                                         kick.context.frame.known
                                             ? std::optional<std::uint32_t>(
                                                   static_cast<std::uint32_t>(kick.context.frame.raw >> 24U) & 63U)
                                             : std::nullopt,
                                         kick.context.tex0.known
                                             ? std::optional<std::uint32_t>(
                                                   static_cast<std::uint32_t>(kick.context.tex0.raw >> 20U) & 63U)
                                             : std::nullopt,
                                         kick.context.tex0.known
                                             ? std::optional<std::uint32_t>(
                                                   static_cast<std::uint32_t>(kick.context.tex0.raw >> 51U) & 15U)
                                             : std::nullopt);
            } else {
                draw_point(kick);
            }
        }
        reset_pending_vertices();
        break;
    case GsPrimitiveTopology::Line:
        pending_vertices_[pending_vertex_count_++] = kick;
        if (pending_vertex_count_ == 2) {
            if (!suppress_draw) {
                const auto& second = pending_vertices_[1];
                if (second.controls.texture_mapping_enabled) {
                    ++statistics_.unsupported_primitives;
                    ++statistics_.unsupported_primitives_by_topology[static_cast<std::size_t>(GsPrimitiveTopology::Line)];
                    record_rejection_details(GsDrawRejectionReason::UnsupportedTopology,
                                             second.context.frame.known
                                                 ? std::optional<std::uint32_t>(
                                                       static_cast<std::uint32_t>(second.context.frame.raw >> 24U) & 63U)
                                                 : std::nullopt,
                                             second.context.tex0.known
                                                 ? std::optional<std::uint32_t>(
                                                       static_cast<std::uint32_t>(second.context.tex0.raw >> 20U) & 63U)
                                                 : std::nullopt,
                                             second.context.tex0.known
                                                 ? std::optional<std::uint32_t>(
                                                       static_cast<std::uint32_t>(second.context.tex0.raw >> 51U) & 15U)
                                                 : std::nullopt);
                } else {
                    draw_line(pending_vertices_[0], pending_vertices_[1]);
                }
            }
            reset_pending_vertices();
        }
        break;
    case GsPrimitiveTopology::Sprite:
        pending_vertices_[pending_vertex_count_++] = kick;
        if (pending_vertex_count_ == 2) {
            if (!suppress_draw) {
                draw_sprite(pending_vertices_[0], pending_vertices_[1]);
            }
            reset_pending_vertices();
        }
        break;
    case GsPrimitiveTopology::TriangleStrip:
        pending_vertices_[pending_vertex_count_++] = kick;
        if (pending_vertex_count_ == 3) {
            if (!suppress_draw) {
                draw_triangle(pending_vertices_[0], pending_vertices_[1], pending_vertices_[2]);
            }
            pending_vertices_[0] = pending_vertices_[1];
            pending_vertices_[1] = pending_vertices_[2];
            pending_vertex_count_ = 2;
            pending_topology_ = GsPrimitiveTopology::TriangleStrip;
        }
        break;
    default:
        ++statistics_.unsupported_primitives;
        ++statistics_.unsupported_primitives_by_topology[static_cast<std::size_t>(topology)];
        record_rejection_details(GsDrawRejectionReason::UnsupportedTopology, std::nullopt,
                                 std::nullopt, std::nullopt);
        reset_pending_vertices();
        break;
    }
}

// RECOVERY: reconstructed from the GIMPLE control flow; optional-temporary layout differs.
void GsLiveRenderer::draw_point(const GsPrimitiveKickSnapshot& point) noexcept {
    const auto known_framebuffer_psm =
        point.context.frame.known
            ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(point.context.frame.raw >> 24U) & 63U)
            : std::nullopt;
    if (!point.context.frame.known) {
        record_incomplete(GsPrimitiveTopology::Point, GsDrawRejectionReason::MissingFrame);
        return;
    }
    if (!point.context.xyoffset.known) {
        record_incomplete(GsPrimitiveTopology::Point, GsDrawRejectionReason::MissingXyoffset,
                          known_framebuffer_psm);
        return;
    }
    if (!point.context.scissor.known) {
        record_incomplete(GsPrimitiveTopology::Point, GsDrawRejectionReason::MissingScissor,
                          known_framebuffer_psm);
        return;
    }
    if (!point.vertex.rgbaq.known) {
        record_incomplete(GsPrimitiveTopology::Point, GsDrawRejectionReason::MissingRgbaq,
                          known_framebuffer_psm);
        return;
    }
    if (point.controls.alpha_blending_enabled && !point.context.alpha.known) {
        record_incomplete(GsPrimitiveTopology::Point, GsDrawRejectionReason::MissingAlpha,
                          known_framebuffer_psm);
        return;
    }

    const auto test = point.context.test.known ? std::optional<std::uint64_t>(point.context.test.raw)
                                               : std::nullopt;
    const auto zbuf = point.context.zbuf.known ? std::optional<std::uint64_t>(point.context.zbuf.raw)
                                               : std::nullopt;
    const auto target = decode_gs_raster_target(point.context.frame.raw, point.context.xyoffset.raw,
                                                point.context.scissor.raw, zbuf, test, colclamp_);
    if (!target) {
        const auto& scissor = point.context.scissor.raw;
        const auto& frame = point.context.frame.raw;
        GsDrawRejectionReason reason = GsDrawRejectionReason::InvalidRasterTarget;
        if ((frame & 0x3f0000U) == 0U) {
            reason = GsDrawRejectionReason::InvalidFramebufferWidth;
        } else {
            const auto left = scissor & 2047U;
            const auto right = (scissor >> 16U) & 2047U;
            const auto top = (scissor >> 32U) & 2047U;
            const auto bottom = (scissor >> 48U) & 2047U;
            if (left > right || top > bottom) {
                reason = GsDrawRejectionReason::InvalidScissor;
            }
        }
        record_incomplete(GsPrimitiveTopology::Point, reason, known_framebuffer_psm);
        return;
    }

    const auto alpha = point.context.alpha.known ? point.context.alpha.raw : std::uint64_t{};
    const auto alpha_blend = decode_gs_alpha_blend(alpha, point.controls.alpha_blending_enabled);
    const auto result =
        rasterizer_.draw_solid_point(*target, raster_vertex(point.vertex), alpha_blend);
    apply_raster_result(result, GsPrimitiveTopology::Point, target->surface.pixel_storage_mode);
    if (result.status == GsRasterStatus::Drawn) {
        ++statistics_.solid_points_drawn;
    }
}

// RECOVERY: threading-statistics copy order differs.
void GsLiveRenderer::apply_raster_result(const GsRasterResult& result,
                                         const GsPrimitiveTopology topology,
                                         const std::uint32_t framebuffer_psm,
                                         const std::optional<std::uint32_t> texture_psm,
                                         const std::optional<std::uint32_t> clut_psm) noexcept {
    statistics_.covered_pixels += result.covered_pixels;
    statistics_.written_pixels += result.written_pixels;
    if (result.written_pixels != 0U) {
        ++statistics_.framebuffer_revision;
    }

    const auto& threading = rasterizer_.threading_statistics();
    statistics_.raster_worker_threads = threading.worker_threads;
    statistics_.raster_parallel_primitives = threading.parallel_primitives;
    statistics_.raster_parallel_tiles = threading.parallel_tiles;
    statistics_.raster_serial_primitives = threading.serial_primitives;
    statistics_.raster_small_primitive_fallbacks = threading.small_primitive_fallbacks;
    statistics_.raster_hazard_fallbacks = threading.hazard_fallbacks;

    const auto topology_index = static_cast<std::size_t>(topology);
    if (result.status == GsRasterStatus::Empty) {
        ++statistics_.empty_primitives;
        ++statistics_.empty_primitives_by_topology[topology_index];
        record_rejection_details(GsDrawRejectionReason::EmptyRasterization, framebuffer_psm,
                                 texture_psm, clut_psm);
    } else if (result.status == GsRasterStatus::UnsupportedFramebufferFormat) {
        ++statistics_.unsupported_primitives;
        ++statistics_.unsupported_primitives_by_topology[topology_index];
        record_rejection_details(GsDrawRejectionReason::UnsupportedFramebufferFormat, framebuffer_psm,
                                 texture_psm, clut_psm);
    } else if (result.status == GsRasterStatus::UnsupportedAlphaBlend) {
        ++statistics_.unsupported_primitives;
        ++statistics_.unsupported_primitives_by_topology[topology_index];
        record_rejection_details(GsDrawRejectionReason::UnsupportedAlphaBlend, framebuffer_psm,
                                 texture_psm, clut_psm);
    } else if (result.status == GsRasterStatus::InvalidTarget) {
        ++statistics_.incomplete_state_primitives;
        ++statistics_.incomplete_state_primitives_by_topology[topology_index];
        record_rejection_details(GsDrawRejectionReason::InvalidRasterTarget, framebuffer_psm,
                                 texture_psm, clut_psm);
    }
}

// RECOVERY: reconstructed from the GIMPLE control flow; optional-temporary layout differs.
void GsLiveRenderer::draw_line(const GsPrimitiveKickSnapshot& first,
                               const GsPrimitiveKickSnapshot& second) noexcept {
    const auto topology = GsPrimitiveTopology::Line;
    const auto known_framebuffer_psm =
        second.context.frame.known
            ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(second.context.frame.raw >> 24U) & 63U)
            : std::nullopt;
    if (!second.context.frame.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingFrame);
        return;
    }
    if (!second.context.xyoffset.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingXyoffset, known_framebuffer_psm);
        return;
    }
    if (!second.context.scissor.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingScissor, known_framebuffer_psm);
        return;
    }
    if (!second.vertex.rgbaq.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingRgbaq, known_framebuffer_psm);
        return;
    }
    if (second.controls.alpha_blending_enabled && !second.context.alpha.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingAlpha, known_framebuffer_psm);
        return;
    }

    const auto test = second.context.test.known ? std::optional<std::uint64_t>(second.context.test.raw)
                                                : std::nullopt;
    const auto zbuf = second.context.zbuf.known ? std::optional<std::uint64_t>(second.context.zbuf.raw)
                                                : std::nullopt;
    const auto target = decode_gs_raster_target(second.context.frame.raw, second.context.xyoffset.raw,
                                                second.context.scissor.raw, zbuf, test, colclamp_);
    if (!target) {
        const auto& scissor = second.context.scissor.raw;
        const auto& frame = second.context.frame.raw;
        GsDrawRejectionReason reason = GsDrawRejectionReason::InvalidRasterTarget;
        if ((frame & 0x3f0000U) == 0U) {
            reason = GsDrawRejectionReason::InvalidFramebufferWidth;
        } else {
            const auto left = scissor & 2047U;
            const auto right = (scissor >> 16U) & 2047U;
            const auto top = (scissor >> 32U) & 2047U;
            const auto bottom = (scissor >> 48U) & 2047U;
            if (left > right || top > bottom) {
                reason = GsDrawRejectionReason::InvalidScissor;
            }
        }
        record_incomplete(topology, reason, known_framebuffer_psm);
        return;
    }

    const auto attributes = second.controls.effective_attributes_raw;
    const bool gouraud = (attributes & 8U) != 0U;
    const bool fog_enabled = (attributes & 32U) != 0U;
    const bool antialias = (attributes & 128U) != 0U;
    const auto alpha_blend =
        decode_gs_alpha_blend(second.context.alpha.raw, second.controls.alpha_blending_enabled);
    GsRasterFog fog{};
    fog.enabled = fog_enabled;
    fog.color.red = static_cast<std::uint8_t>(fogcol_);
    fog.color.green = static_cast<std::uint8_t>(fogcol_ >> 8U);
    fog.color.blue = static_cast<std::uint8_t>(fogcol_ >> 16U);

    const auto result = rasterizer_.draw_solid_line(*target, raster_vertex(first.vertex), raster_vertex(second.vertex),
                                                    gouraud, antialias, alpha_blend, fog);
    apply_raster_result(result, topology, target->surface.pixel_storage_mode);
    if (result.status == GsRasterStatus::Drawn) {
        ++statistics_.solid_lines_drawn;
    }
}

// RECOVERY: clut copy and TEXA optional temporaries are laid out differently.
std::optional<GsTextureImageSnapshot>
GsLiveRenderer::snapshot_texture_image(const std::uint64_t tex0) const {
    const std::optional<std::uint64_t> texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    return snapshot_gs_texture_image(local_memory_, tex0, texa, clut_);
}

// RECOVERY: TEXA optional temporary is laid out differently.
bool GsLiveRenderer::texture_image_matches(const std::uint64_t tex0,
                                           const GsTextureImageSnapshot& expected) const noexcept {
    const std::optional<std::uint64_t> texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    return gs_texture_image_matches(local_memory_, tex0, texa, clut_.get(), expected);
}

// RECOVERY: surface temporary and status PHI layout differ.
GsTextureSnapshotSourceStatus
GsLiveRenderer::classify_texture_snapshot_source(const std::uint64_t tex0) const noexcept {
    if (!can_snapshot_gs_texture_image(tex0, texa_known_, clut_ != nullptr)) {
        return GsTextureSnapshotSourceStatus::UnsupportedTexture;
    }
    const auto width_shift = static_cast<std::uint32_t>(tex0 >> 26U) & 15U;
    const auto height_shift = static_cast<std::uint32_t>(tex0 >> 30U) & 15U;
    const GsLocalMemorySurface surface{
        .base_pointer = static_cast<std::uint32_t>(tex0) & 16383U,
        .buffer_width = static_cast<std::uint32_t>(tex0 >> 14U) & 63U,
        .pixel_storage_mode = static_cast<std::uint32_t>(tex0 >> 20U) & 63U,
    };
    if (rasterizer_.local_memory_is_authoritative_for_texture(surface, 1U << width_shift,
                                                              1U << height_shift)) {
        return GsTextureSnapshotSourceStatus::CurrentLocalMemory;
    }
    return GsTextureSnapshotSourceStatus::FeedbackCacheConflict;
}

GsTextureImageValidationResult GsLiveRenderer::validate_texture_image_for_residency(
    const std::uint64_t tex0, GsTextureImageResidencyCapture& capture) const noexcept {
    // RECOVERY: the original splits this body into a .part.0 clone; the logic is
    // the same, the code layout is not.
    if (capture.source_tex0_ && *capture.source_tex0_ != tex0) {
        return {};
    }

    const std::optional<std::uint64_t> current_texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    const auto psm = static_cast<std::uint32_t>(tex0 >> 20U) & 63U;
    std::optional<std::uint64_t> relevant_texa{};
    if ((tex0 & (std::uint64_t{1} << 34U)) != 0U &&
        (psm == kGsPsmct24 || psm == kGsPsmz16 || psm == kGsPsmz16s)) {
        relevant_texa = current_texa;
    }

    if (relevant_texa.has_value() != capture.direct_texa_.has_value()) {
        return {};
    }
    if (relevant_texa && *relevant_texa != *capture.direct_texa_) {
        return {};
    }
    if (!gs_texture_image_non_memory_state_matches(tex0, current_texa, clut_.get(), capture.image_)) {
        return {};
    }

    GsTextureImageValidationResult result{};
    bool current = false;
    switch (local_memory_.classify_any_mutations_since(capture.local_memory_checkpoint_)) {
    case GsLocalMemoryMutationStatus::Unchanged:
        current = true;
        result.texture_content_scan_skipped = true;
        break;
    case GsLocalMemoryMutationStatus::Changed: {
        const auto width_shift = static_cast<std::uint32_t>(tex0 >> 26U) & 15U;
        const auto height_shift = static_cast<std::uint32_t>(tex0 >> 30U) & 15U;
        const GsLocalMemorySurface surface{
            .base_pointer = static_cast<std::uint32_t>(tex0) & 16383U,
            .buffer_width = static_cast<std::uint32_t>(tex0 >> 14U) & 63U,
            .pixel_storage_mode = psm,
        };
        const auto footprint = rasterizer_.texture_snapshot_block_footprint(
            surface, 1U << width_shift, 1U << height_shift);
        result.mutation_block_scan_performed = footprint.has_value();
        if (footprint && local_memory_.classify_mutations_since(capture.local_memory_checkpoint_,
                                                                *footprint) ==
                             GsLocalMemoryMutationStatus::Unchanged) {
            current = true;
            result.texture_content_scan_skipped = true;
        }
        break;
    }
    default:
        current = gs_texture_image_matches(local_memory_, tex0, current_texa, clut_.get(),
                                           capture.image_);
        break;
    }

    if (!current) {
        result.current = false;
        return result;
    }
    capture.local_memory_checkpoint_ = local_memory_.capture_mutation_checkpoint();
    capture.direct_texa_ = relevant_texa;
    result.current = true;
    return result;
}

std::optional<GsTextureImageResidencyCapture>
GsLiveRenderer::capture_texture_image_for_residency(const std::uint64_t tex0) {
    // RECOVERY: the original folds the proof refresh into a longer sequence;
    // this version snapshots the image, then binds the checkpoint and TEXA.
    auto image = snapshot_texture_image(tex0);
    if (!image) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    return GsTextureImageResidencyCapture(std::move(*image), tex0,
                                          local_memory_.capture_mutation_checkpoint(), texa);
}

GsReplayStateRestoreStatus GsLiveRenderer::restore_idle_state(const GsReplayStateSeed& seed) {
    // RECOVERY: field order follows the decoded GIMPLE; not verified byte-for-byte.
    const auto status = validate_gs_replay_state_seed(seed);
    if (status != GsReplayStateRestoreStatus::Applied) {
        return status;
    }
    if (!local_memory_.load_raw_backing(seed.local_memory)) {
        return GsReplayStateRestoreStatus::InvalidLocalMemorySize;
    }
    rasterizer_.invalidate_texture_cache();
    rasterizer_.reset_threading_statistics();
    assembler_.restore_state(seed.drawing);
    pending_vertex_count_ = 0;
    pending_topology_.reset();
    texa_ = seed.texa;
    texa_known_ = true;

    const auto tex0 = seed.drawing.contexts[0].tex0;
    clut_ = empty_clut();
    if (tex0.known) {
        const auto psm = static_cast<std::uint32_t>(tex0.raw >> 20U) & 63U;
        if (psm == kGsPsmt4 || psm == kGsPsmt8) {
            auto restored = std::make_shared<GsRasterClut>();
            populate_csm1_clut(*restored, local_memory_.raw_backing(), tex0.raw);
            restored->version = 1U;
            clut_ = std::move(restored);
        }
    }
    clut_cbp_slots_ = {};
    next_clut_version_ = 1U;
    statistics_.clut_version = 1U;
    bitbltbuf_ = seed.bitbltbuf;
    trxpos_ = seed.trxpos;
    trxreg_ = seed.trxreg;
    packed_q_bits_ = seed.packed_q_bits;
    colclamp_ = seed.colclamp;
    colclamp_known_ = true;
    fogcol_ = 0;
    statistics_.raster_worker_threads = rasterizer_.threading_statistics().worker_threads;
    return GsReplayStateRestoreStatus::Applied;
}

GsReplayStateRestoreStatus restore_gs_idle_replay_state(GsCapture& capture, GsLiveRenderer& renderer,
                                                        const GsReplayStateSeed& seed) {
    // RECOVERY: approximate; the capture-side step and the renderer step are
    // sequenced as in the decoded GIMPLE, but the exact guard order is unverified.
    const auto status = validate_gs_replay_state_seed(seed);
    if (status != GsReplayStateRestoreStatus::Applied) {
        return status;
    }
    const auto capture_status = capture.restore_idle_state(seed);
    if (capture_status != GsReplayStateRestoreStatus::Applied) {
        return capture_status;
    }
    return renderer.restore_idle_state(seed);
}

namespace {

// Builds the raster texture state for one textured sprite or triangle from the
// ordered TEX0, TEX1, MIPTBP, TEXA and CLAMP latches of its drawing context.
// RECOVERY: field-level decode is reconstructed from the GS register layout;
// it is not a byte-for-byte match of the original's inlined expansion.
[[nodiscard, gnu::always_inline]] inline GsRasterTexture make_raster_texture(const GsDrawingContextState& context,
                                                  std::shared_ptr<const GsRasterClut> clut,
                                                  std::optional<std::uint64_t> texa,
                                                  GsRasterAlphaBlend alpha_blend) {
    GsRasterTexture texture{};
    const auto tex0 = context.tex0.raw;
    const auto psm = static_cast<std::uint32_t>(tex0 >> 20U) & 63U;
    texture.surface.base_pointer = static_cast<std::uint32_t>(tex0) & 16383U;
    texture.surface.buffer_width = static_cast<std::uint32_t>(tex0 >> 14U) & 63U;
    texture.surface.pixel_storage_mode = psm;
    texture.width = 1U << (static_cast<std::uint32_t>(tex0 >> 26U) & 15U);
    texture.height = 1U << (static_cast<std::uint32_t>(tex0 >> 30U) & 15U);
    texture.clut_base_pointer = static_cast<std::uint32_t>(tex0 >> 37U) & 16383U;
    texture.clut_pixel_storage_mode = static_cast<std::uint32_t>(tex0 >> 51U) & 15U;
    texture.clut_storage_mode2 = ((tex0 >> 55U) & 1U) != 0U;
    texture.clut_start = static_cast<std::uint8_t>((tex0 >> 56U) & 31U);
    texture.clut = std::move(clut);
    texture.texture_alpha_enabled = ((tex0 >> 34U) & 1U) != 0U;
    texture.modulate = ((tex0 >> 35U) & 3U) == 0U;
    texture.alpha_blend = alpha_blend;
    if (texa) {
        texture.alpha = static_cast<std::uint8_t>(*texa);
        texture.alpha_high = static_cast<std::uint8_t>(*texa >> 32U);
        texture.alpha_is_zero_for_black = (*texa & 0x8000U) != 0U;
    }
    if (context.clamp.known) {
        texture.clamp = context.clamp.raw;
    }
    if (context.tex1.known) {
        const auto tex1 = context.tex1.raw;
        const auto minification = static_cast<std::uint8_t>((tex1 >> 6U) & 7U);
        texture.fixed_lod = (tex1 & 1U) != 0U;
        texture.maximum_lod_level = static_cast<std::uint8_t>((tex1 >> 2U) & 7U);
        texture.magnification_linear = (tex1 & 32U) != 0U;
        texture.minification_linear = minification == 1U || minification == 4U || minification == 5U;
        texture.mipmap_linear = minification == 3U || minification == 5U;
        texture.lod_scale = static_cast<std::uint8_t>((tex1 >> 19U) & 3U);
        const auto bias = static_cast<std::uint32_t>(tex1 >> 32U) & 4095U;
        texture.lod_bias_4 = static_cast<std::int16_t>(static_cast<std::int32_t>(bias ^ 0x800U) - 0x800);
    }
    texture.maximum_mip_level = texture.maximum_lod_level;
    if (context.miptbp1.known && context.miptbp2.known) {
        const auto mip1 = context.miptbp1.raw;
        const auto mip2 = context.miptbp2.raw;
        for (std::size_t level = 0; level < 6U; ++level) {
            const auto raw = level < 3U ? mip1 : mip2;
            const auto shift = static_cast<std::uint32_t>(level % 3U) * 20U;
            texture.mipmap_surfaces[level] = GsLocalMemorySurface{
                .base_pointer = static_cast<std::uint32_t>(raw >> shift) & 16383U,
                .buffer_width = static_cast<std::uint32_t>(raw >> (shift + 14U)) & 63U,
                .pixel_storage_mode = psm,
            };
        }
    }
    return texture;
}

} // namespace

// RECOVERY: best-effort textured path; solid path matches the GIMPLE control flow.
void GsLiveRenderer::draw_sprite(const GsPrimitiveKickSnapshot& first,
                                 const GsPrimitiveKickSnapshot& second) noexcept {
    // RECOVERY: validation order follows the decoded GIMPLE; the textured path
    // uses make_raster_texture() rather than the original's inlined expansion.
    const auto topology = GsPrimitiveTopology::Sprite;
    const bool textured = second.controls.texture_mapping_enabled;
    const auto known_framebuffer_psm =
        second.context.frame.known
            ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(second.context.frame.raw >> 24U) & 63U)
            : std::nullopt;
    if (!second.context.frame.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingFrame);
        return;
    }
    if (!second.context.xyoffset.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingXyoffset, known_framebuffer_psm);
        return;
    }
    if (!second.context.scissor.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingScissor, known_framebuffer_psm);
        return;
    }
    if (textured && !second.context.tex0.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingTex0, known_framebuffer_psm);
        return;
    }
    if (!second.vertex.rgbaq.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingRgbaq, known_framebuffer_psm);
        return;
    }
    if (textured && !second.vertex.uv.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingUv, known_framebuffer_psm);
        return;
    }
    if (second.controls.alpha_blending_enabled && !second.context.alpha.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingAlpha, known_framebuffer_psm);
        return;
    }

    const auto test = second.context.test.known ? std::optional<std::uint64_t>(second.context.test.raw)
                                                : std::nullopt;
    const auto zbuf = second.context.zbuf.known ? std::optional<std::uint64_t>(second.context.zbuf.raw)
                                                : std::nullopt;
    const auto target = decode_gs_raster_target(second.context.frame.raw, second.context.xyoffset.raw,
                                                second.context.scissor.raw, zbuf, test, colclamp_);
    if (!target) {
        const auto& scissor = second.context.scissor.raw;
        const auto& frame = second.context.frame.raw;
        GsDrawRejectionReason reason = GsDrawRejectionReason::InvalidRasterTarget;
        if ((frame & 0x3f0000U) == 0U) {
            reason = GsDrawRejectionReason::InvalidFramebufferWidth;
        } else {
            const auto left = scissor & 2047U;
            const auto right = (scissor >> 16U) & 2047U;
            const auto top = (scissor >> 32U) & 2047U;
            const auto bottom = (scissor >> 48U) & 2047U;
            if (left > right || top > bottom) {
                reason = GsDrawRejectionReason::InvalidScissor;
            }
        }
        record_incomplete(topology, reason, known_framebuffer_psm);
        return;
    }

    const auto alpha = second.context.alpha.known ? second.context.alpha.raw : std::uint64_t{};
    const auto alpha_blend = decode_gs_alpha_blend(alpha, second.controls.alpha_blending_enabled);
    if (!textured) {
        const auto result = rasterizer_.draw_solid_sprite(*target, raster_vertex(first.vertex),
                                                          raster_vertex(second.vertex), alpha_blend);
        apply_raster_result(result, topology, target->surface.pixel_storage_mode);
        if (result.status == GsRasterStatus::Drawn) {
            ++statistics_.solid_sprites_drawn;
        }
        return;
    }

    const std::optional<std::uint64_t> texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    const auto texture = make_raster_texture(second.context, clut_, texa, alpha_blend);
    const auto texture_psm = std::optional<std::uint32_t>(
        static_cast<std::uint32_t>(second.context.tex0.raw >> 20U) & 63U);
    const auto clut_psm = std::optional<std::uint32_t>(
        static_cast<std::uint32_t>(second.context.tex0.raw >> 51U) & 15U);
    const auto result = rasterizer_.draw_textured_sprite(*target, texture, raster_vertex(first.vertex),
                                                         raster_vertex(second.vertex));
    apply_raster_result(result, topology, target->surface.pixel_storage_mode, texture_psm, clut_psm);
    if (result.status == GsRasterStatus::Drawn) {
        ++statistics_.textured_sprites_drawn;
        census_texture(*texture_psm,
                       static_cast<std::uint32_t>(second.context.tex0.raw >> 35U) & 3U,
                       *clut_psm);
    }
}

// RECOVERY: best-effort textured path; solid path matches the GIMPLE control flow.
void GsLiveRenderer::draw_triangle(const GsPrimitiveKickSnapshot& first,
                                   const GsPrimitiveKickSnapshot& second,
                                   const GsPrimitiveKickSnapshot& third) noexcept {
    // RECOVERY: validation order follows the decoded GIMPLE; the textured path
    // uses make_raster_texture() rather than the original's inlined expansion.
    const auto topology = GsPrimitiveTopology::TriangleStrip;
    const bool textured = third.controls.texture_mapping_enabled;
    const auto known_framebuffer_psm =
        third.context.frame.known
            ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(third.context.frame.raw >> 24U) & 63U)
            : std::nullopt;
    if (!third.context.frame.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingFrame);
        return;
    }
    if (!third.context.xyoffset.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingXyoffset, known_framebuffer_psm);
        return;
    }
    if (!third.context.scissor.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingScissor, known_framebuffer_psm);
        return;
    }
    if (textured && !third.context.tex0.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingTex0, known_framebuffer_psm);
        return;
    }
    if (!third.vertex.rgbaq.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingRgbaq, known_framebuffer_psm);
        return;
    }
    if (textured && third.controls.fixed_uv) {
        record_deferred(topology, GsDrawRejectionReason::UnsupportedFixedUvTriangle,
                        known_framebuffer_psm,
                        std::optional<std::uint32_t>(static_cast<std::uint32_t>(third.context.tex0.raw >> 20U) & 63U),
                        std::optional<std::uint32_t>(static_cast<std::uint32_t>(third.context.tex0.raw >> 51U) & 15U));
        return;
    }
    if (third.controls.alpha_blending_enabled && !third.context.alpha.known) {
        record_incomplete(topology, GsDrawRejectionReason::MissingAlpha, known_framebuffer_psm);
        return;
    }

    const auto test = third.context.test.known ? std::optional<std::uint64_t>(third.context.test.raw)
                                               : std::nullopt;
    const auto zbuf = third.context.zbuf.known ? std::optional<std::uint64_t>(third.context.zbuf.raw)
                                               : std::nullopt;
    const auto target = decode_gs_raster_target(third.context.frame.raw, third.context.xyoffset.raw,
                                                third.context.scissor.raw, zbuf, test, colclamp_);
    if (!target) {
        const auto& scissor = third.context.scissor.raw;
        const auto& frame = third.context.frame.raw;
        GsDrawRejectionReason reason = GsDrawRejectionReason::InvalidRasterTarget;
        if ((frame & 0x3f0000U) == 0U) {
            reason = GsDrawRejectionReason::InvalidFramebufferWidth;
        } else {
            const auto left = scissor & 2047U;
            const auto right = (scissor >> 16U) & 2047U;
            const auto top = (scissor >> 32U) & 2047U;
            const auto bottom = (scissor >> 48U) & 2047U;
            if (left > right || top > bottom) {
                reason = GsDrawRejectionReason::InvalidScissor;
            }
        }
        record_incomplete(topology, reason, known_framebuffer_psm);
        return;
    }

    const auto attributes = third.controls.effective_attributes_raw;
    const bool gouraud = (attributes & 8U) != 0U;
    const auto alpha = third.context.alpha.known ? third.context.alpha.raw : std::uint64_t{};
    const auto alpha_blend = decode_gs_alpha_blend(alpha, third.controls.alpha_blending_enabled);
    if (!textured) {
        const auto result =
            rasterizer_.draw_solid_triangle(*target, raster_vertex(first.vertex), raster_vertex(second.vertex),
                                            raster_vertex(third.vertex), gouraud, alpha_blend);
        apply_raster_result(result, topology, target->surface.pixel_storage_mode);
        if (result.status == GsRasterStatus::Drawn) {
            ++statistics_.solid_triangles_drawn;
        }
        return;
    }

    const std::optional<std::uint64_t> texa =
        texa_known_ ? std::optional<std::uint64_t>(texa_) : std::nullopt;
    const auto texture = make_raster_texture(third.context, clut_, texa, alpha_blend);
    const auto texture_psm = std::optional<std::uint32_t>(
        static_cast<std::uint32_t>(third.context.tex0.raw >> 20U) & 63U);
    const auto clut_psm = std::optional<std::uint32_t>(
        static_cast<std::uint32_t>(third.context.tex0.raw >> 51U) & 15U);
    const auto result = rasterizer_.draw_textured_triangle(
        *target, texture, raster_vertex(first.vertex), raster_vertex(second.vertex),
        raster_vertex(third.vertex), gouraud);
    apply_raster_result(result, topology, target->surface.pixel_storage_mode, texture_psm, clut_psm);
    if (result.status == GsRasterStatus::Drawn) {
        ++statistics_.textured_triangles_drawn;
        census_texture(*texture_psm, static_cast<std::uint32_t>(third.context.tex0.raw >> 35U) & 3U,
                       *clut_psm);
    }
}

} // namespace takedown::guest
