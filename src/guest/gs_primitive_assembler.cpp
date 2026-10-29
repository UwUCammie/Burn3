#include "takedown/guest/gs_primitive_assembler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takedown::guest {

namespace {

// Bits per texel for the PSMs that have a direct entry; everything else is
// treated as 32-bit when deriving the mip tail.
std::uint32_t texel_bits(const std::uint32_t psm) noexcept {
    switch (psm) {
    case 0x02U:
        return 16U;
    case 0x0aU:
        return 16U;
    case 0x13U:
        return 8U;
    case 0x14U:
        return 4U;
    default:
        return 32U;
    }
}

// TEX1.LCM set means the lower mip levels are not supplied by the game, so the
// MIPTBP1 base/width fields are derived from the TEX0 write itself. Depth
// formats and widths the GS cannot mip return nothing.
inline std::optional<std::uint64_t> derive_miptbp1(const GsDrawingContextState& context,
                                            const std::uint64_t tex0) noexcept {
    if (!context.tex1.known || (context.tex1.raw & 512U) == 0U) {
        return std::nullopt;
    }

    const auto psm = static_cast<std::uint32_t>(tex0 >> 20U) & 63U;
    const auto width_shift = static_cast<std::uint32_t>(tex0 >> 26U) & 15U;
    const std::uint32_t bits_per_texel = texel_bits(psm);
    const std::uint32_t maximum_width_shift = bits_per_texel != 32U ? 10U : 9U;
    if ((psm & 48U) == 48U || width_shift <= 4U || width_shift > maximum_width_shift) {
        return std::nullopt;
    }

    const std::uint32_t width = 1U << width_shift;
    std::uint32_t base_pointer = static_cast<std::uint32_t>(tex0) & 16383U;
    std::uint32_t buffer_width = std::max(width >> 6U, 1U);
    std::uint32_t texture_blocks = ((width << width_shift) * (bits_per_texel >> 2U)) >> 9U;

    std::uint64_t result = context.miptbp1.known ? (context.miptbp1.raw & 0xF000000000000000ULL)
                                                 : 0ULL;
    for (std::uint32_t field = 0U; field != 3U; ++field) {
        base_pointer += texture_blocks;
        buffer_width = std::max(buffer_width >> 1U, 1U);
        texture_blocks = std::max(texture_blocks >> 2U, 1U);
        const std::uint32_t shift = field * 20U;
        result |= static_cast<std::uint64_t>(base_pointer & 16383U) << shift;
        result |= static_cast<std::uint64_t>(buffer_width & 63U) << (shift + 14U);
    }
    return result;
}

} // namespace

GsPrimitiveTopology decode_gs_primitive_topology(const std::uint64_t prim) noexcept {
    return static_cast<GsPrimitiveTopology>(static_cast<std::uint8_t>(prim) & 7U);
}

bool is_drawable_gs_primitive(const GsPrimitiveTopology topology) noexcept {
    return topology != GsPrimitiveTopology::Reserved;
}

GsPrimitiveAssembler::GsPrimitiveAssembler() noexcept {
    reset();
}

void GsPrimitiveAssembler::track(GsTrackedRegister& destination,
                                 const std::uint64_t value) noexcept {
    destination.raw = value;
    destination.known = true;
}

// RECOVERY: the original folds the context cast with the & 1 into an enum-typed
// expression; this rebuild keeps the underlying type. Same behaviour, different GIMPLE type.
GsPrimitiveControlState
decode_gs_primitive_controls(const GsPrimitiveAssemblerState& state) noexcept {
    GsPrimitiveControlState control_state{};
    control_state.prim_raw = state.prim.raw;
    control_state.topology = decode_gs_primitive_topology(state.prim.raw);

    control_state.attributes_from_prim =
        state.prmodecont.known && (state.prmodecont.raw & 1U) != 0U;
    const GsTrackedRegister& attribute_source =
        control_state.attributes_from_prim ? state.prim : state.prmode;
    control_state.effective_attributes_raw = attribute_source.raw;
    control_state.texture_mapping_enabled = (attribute_source.raw & 16U) != 0U;
    control_state.alpha_blending_enabled = (attribute_source.raw & 64U) != 0U;
    control_state.fixed_uv = (attribute_source.raw & 256U) != 0U;
    control_state.context = static_cast<GsDrawingContext>(
        static_cast<std::uint8_t>(attribute_source.raw >> 9U) & 1U);
    control_state.known = state.prim.known && state.prmodecont.known && attribute_source.known;
    return control_state;
}

GsPrimitiveControlState GsPrimitiveAssembler::controls() const noexcept {
    return decode_gs_primitive_controls(state_);
}

// RECOVERY: the original outlines the snapshot path as write_position.part.0
// (partial inlining of current_vertex_snapshot); not reproduced from source.
std::optional<GsPrimitiveKickSnapshot>
GsPrimitiveAssembler::write_position(std::uint8_t address, std::uint64_t canonical_value,
                                     GsVertexPositionKind kind, bool drawing_kick, bool packed_adc,
                                     std::optional<GsPackedRegisterValue> packed_value) noexcept {
    state_.vertex.position = {canonical_value, true};
    state_.vertex.position_kind = kind;
    state_.vertex.position_register_address = address;
    state_.vertex.packed_position = packed_value;
    state_.vertex.packed_adc = packed_adc;
    if (!drawing_kick || packed_adc) {
        return {};
    }
    return current_vertex_snapshot();
}

std::optional<GsPrimitiveKickSnapshot>
GsPrimitiveAssembler::write(const GsAssemblerRegisterWrite& write) noexcept {
    ++state_.write_sequence;
    switch (write.address) {
    case 0x00U:
        track(state_.prim, write.value);
        break;
    case 0x01U:
        track(state_.vertex.rgbaq, write.value);
        break;
    case 0x02U:
        track(state_.vertex.st, write.value);
        break;
    case 0x03U:
        track(state_.vertex.uv, write.value);
        break;
    case 0x04U:
        return write_position(0x04U, write.value, GsVertexPositionKind::XyzWithFog, true, false,
                              std::nullopt);
    case 0x05U:
        return write_position(0x05U, write.value, GsVertexPositionKind::Xyz, true, false,
                              std::nullopt);
    case 0x06U:
        track(state_.contexts[0].tex0, write.value);
        if (const auto miptbp1 = derive_miptbp1(state_.contexts[0], write.value)) {
            state_.contexts[0].miptbp1 = {*miptbp1, true};
        }
        break;
    case 0x07U:
        track(state_.contexts[1].tex0, write.value);
        if (const auto miptbp1 = derive_miptbp1(state_.contexts[1], write.value)) {
            state_.contexts[1].miptbp1 = {*miptbp1, true};
        }
        break;
    case 0x08U:
        track(state_.contexts[0].clamp, write.value);
        break;
    case 0x09U:
        track(state_.contexts[1].clamp, write.value);
        break;
    case 0x0cU:
        return write_position(0x0cU, write.value, GsVertexPositionKind::XyzWithFog, false, false,
                              std::nullopt);
    case 0x0dU:
        return write_position(0x0dU, write.value, GsVertexPositionKind::Xyz, false, false,
                              std::nullopt);
    case 0x14U:
        track(state_.contexts[0].tex1, write.value);
        break;
    case 0x15U:
        track(state_.contexts[1].tex1, write.value);
        break;
    case 0x16U:
        if (state_.contexts[0].tex0.known) {
            track(state_.contexts[0].tex0,
                              (state_.contexts[0].tex0.raw & 0x1FFC0FFFFFULL) |
                                  (write.value & 0xFFFFFFE003F00000ULL));
        }
        break;
    case 0x17U:
        if (state_.contexts[1].tex0.known) {
            track(state_.contexts[1].tex0,
                              (state_.contexts[1].tex0.raw & 0x1FFC0FFFFFULL) |
                                  (write.value & 0xFFFFFFE003F00000ULL));
        }
        break;
    case 0x18U:
        track(state_.contexts[0].xyoffset, write.value);
        break;
    case 0x19U:
        track(state_.contexts[1].xyoffset, write.value);
        break;
    case 0x1aU:
        track(state_.prmodecont, write.value);
        break;
    case 0x1bU:
        if (state_.prmodecont.known && (state_.prmodecont.raw & 1U) == 0U) {
            track(state_.prmode, write.value);
        }
        break;
    case 0x22U:
        track(state_.scanmsk, write.value);
        break;
    case 0x34U:
        track(state_.contexts[0].miptbp1, write.value);
        break;
    case 0x35U:
        track(state_.contexts[1].miptbp1, write.value);
        break;
    case 0x36U:
        track(state_.contexts[0].miptbp2, write.value);
        break;
    case 0x37U:
        track(state_.contexts[1].miptbp2, write.value);
        break;
    case 0x40U:
        track(state_.contexts[0].scissor, write.value);
        break;
    case 0x41U:
        track(state_.contexts[1].scissor, write.value);
        break;
    case 0x42U:
        track(state_.contexts[0].alpha, write.value);
        break;
    case 0x43U:
        track(state_.contexts[1].alpha, write.value);
        break;
    case 0x45U:
        track(state_.dthe, write.value);
        break;
    case 0x47U:
        track(state_.contexts[0].test, write.value);
        break;
    case 0x48U:
        track(state_.contexts[1].test, write.value);
        break;
    case 0x49U:
        track(state_.pabe, write.value);
        break;
    case 0x4aU:
        track(state_.fba[0], write.value);
        break;
    case 0x4bU:
        track(state_.fba[1], write.value);
        break;
    case 0x4cU:
        track(state_.contexts[0].frame, write.value);
        break;
    case 0x4dU:
        track(state_.contexts[1].frame, write.value);
        break;
    case 0x4eU:
        track(state_.contexts[0].zbuf, write.value);
        break;
    case 0x4fU:
        track(state_.contexts[1].zbuf, write.value);
        break;
    default:
        break;
    }
    return {};
}

std::optional<GsPrimitiveKickSnapshot> GsPrimitiveAssembler::write(const GsRegisterAddress address,
                                                                   const std::uint64_t value) noexcept {
    return write(GsAssemblerRegisterWrite{static_cast<std::uint8_t>(address), value});
}

// RECOVERY: the original interleaves the payload loads and shifts differently
// (y-part shift before the high-word load); statement order differs.
std::optional<GsPrimitiveKickSnapshot>
GsPrimitiveAssembler::write_packed_position(const GsPackedPositionWrite& write) noexcept {
    ++state_.write_sequence;
    const std::uint64_t y = ((write.payload.low >> 32U) << 16U) & 4294967295U;
    const auto word2 = static_cast<std::uint32_t>(write.payload.high);
    const auto word3 = static_cast<std::uint32_t>(write.payload.high >> 32U);
    const std::uint64_t x = write.payload.low & 65535U;
    const bool adc = (word3 & 32768U) != 0U;

    switch (write.address) {
    case GsRegisterAddress::Xyzf2: {
        const std::uint64_t z =
            (static_cast<std::uint64_t>(word2 >> 4U) << 32U) & 72057589742960640ULL;
        const std::uint64_t fog = static_cast<std::uint64_t>(word3 >> 4U) << 56U;
        return write_position(0x04U, x | y | z | fog, GsVertexPositionKind::XyzWithFog, true, adc,
                              write.payload);
    }
    case GsRegisterAddress::Xyz2:
        return write_position(0x05U, x | y | (write.payload.high << 32U), GsVertexPositionKind::Xyz,
                             true, adc, write.payload);
    default:
        return {};
    }
}

// RECOVERY: the original builds the snapshot in an unnamed temporary with
// write_sequence stored last; the field order here differs.
std::optional<GsPrimitiveKickSnapshot> GsPrimitiveAssembler::current_vertex_snapshot() const noexcept {
    const GsPrimitiveControlState control_state = controls();
    if (!state_.vertex.position.known || !control_state.known ||
        !is_drawable_gs_primitive(control_state.topology)) {
        return {};
    }
    GsPrimitiveKickSnapshot snapshot;
    snapshot.controls = control_state;
    snapshot.context = state_.contexts[static_cast<std::size_t>(
        control_state.context != GsDrawingContext::Context1)];
    snapshot.vertex = state_.vertex;
    snapshot.write_sequence = state_.write_sequence;
    return snapshot;
}

void GsPrimitiveAssembler::reset() noexcept {
    state_ = {};
    state_.prmodecont = {1U, true};
    for (auto& context : state_.contexts) {
        context.alpha = {0U, true};
    }
}

void GsPrimitiveAssembler::restore_state(const GsPrimitiveAssemblerState& state) noexcept {
    state_ = state;
}

const GsPrimitiveAssemblerState& GsPrimitiveAssembler::state() const noexcept {
    return state_;
}

} // namespace takedown::guest
