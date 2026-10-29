#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace takedown::guest {

// GS general-purpose register addresses accepted by GsPrimitiveAssembler.
// Unknown addresses are intentionally harmless: the assembler simply ignores
// them so a complete GIF stream can be forwarded without a filtering layer.
enum class GsRegisterAddress : std::uint8_t {
    Prim = 0x00,
    Rgbaq = 0x01,
    St = 0x02,
    Uv = 0x03,
    Xyzf2 = 0x04,
    Xyz2 = 0x05,
    Tex0Context1 = 0x06,
    Tex0Context2 = 0x07,
    ClampContext1 = 0x08,
    ClampContext2 = 0x09,
    Xyzf3 = 0x0c,
    Xyz3 = 0x0d,
    Tex1Context1 = 0x14,
    Tex1Context2 = 0x15,
    Tex2Context1 = 0x16,
    Tex2Context2 = 0x17,
    XyoffsetContext1 = 0x18,
    XyoffsetContext2 = 0x19,
    Prmodecont = 0x1a,
    Prmode = 0x1b,
    Scanmsk = 0x22,
    Miptbp1Context1 = 0x34,
    Miptbp1Context2 = 0x35,
    Miptbp2Context1 = 0x36,
    Miptbp2Context2 = 0x37,
    ScissorContext1 = 0x40,
    ScissorContext2 = 0x41,
    AlphaContext1 = 0x42,
    AlphaContext2 = 0x43,
    Dthe = 0x45,
    TestContext1 = 0x47,
    TestContext2 = 0x48,
    Pabe = 0x49,
    FbaContext1 = 0x4a,
    FbaContext2 = 0x4b,
    FrameContext1 = 0x4c,
    FrameContext2 = 0x4d,
    ZbufContext1 = 0x4e,
    ZbufContext2 = 0x4f,
};

enum class GsPrimitiveTopology : std::uint8_t {
    Point = 0,
    Line = 1,
    LineStrip = 2,
    Triangle = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprite = 6,
    Reserved = 7,
};

enum class GsDrawingContext : std::uint8_t {
    Context1 = 0,
    Context2 = 1,
};

enum class GsVertexPositionKind : std::uint8_t {
    Xyz,
    XyzWithFog,
};

struct GsTrackedRegister {
    std::uint64_t raw{};
    bool known{};
};

struct GsDrawingContextState {
    GsTrackedRegister frame{};
    GsTrackedRegister tex0{};
    GsTrackedRegister tex1{};
    GsTrackedRegister clamp{};
    GsTrackedRegister miptbp1{};
    GsTrackedRegister miptbp2{};
    GsTrackedRegister xyoffset{};
    GsTrackedRegister scissor{};
    GsTrackedRegister alpha{};
    GsTrackedRegister test{};
    GsTrackedRegister zbuf{};
};

struct GsPackedRegisterValue {
    // Exact little-endian halves from one PACKED GIF payload quadword.
    std::uint64_t low{};
    std::uint64_t high{};
};

// Attribute registers are retained in their canonical 64-bit GS register
// form. A packed position additionally retains its exact 128-bit payload;
// this is required because Z and ADC do not fit in the packed low 64 bits.
struct GsVertexState {
    GsTrackedRegister rgbaq{};
    GsTrackedRegister st{};
    GsTrackedRegister uv{};
    GsTrackedRegister position{};
    GsVertexPositionKind position_kind{GsVertexPositionKind::Xyz};
    std::uint8_t position_register_address{};
    std::optional<GsPackedRegisterValue> packed_position{};
    bool packed_adc{};
};

struct GsPrimitiveControlState {
    GsPrimitiveTopology topology{GsPrimitiveTopology::Point};
    GsDrawingContext context{GsDrawingContext::Context1};
    bool texture_mapping_enabled{};
    bool alpha_blending_enabled{};
    bool fixed_uv{};
    bool attributes_from_prim{};
    bool known{};
    std::uint64_t prim_raw{};
    std::uint64_t effective_attributes_raw{};
};

struct GsPrimitiveKickSnapshot {
    std::uint64_t write_sequence{};
    GsPrimitiveControlState controls{};
    GsDrawingContextState context{};
    GsVertexState vertex{};
};

struct GsPrimitiveAssemblerState {
    GsTrackedRegister prim{};
    GsTrackedRegister prmodecont{};
    GsTrackedRegister prmode{};
    // Admission latches live outside kick snapshots to avoid growing the hot
    // per-vertex context copy. Unknown is distinct from an observed zero.
    std::array<GsTrackedRegister, 2> fba{};
    GsTrackedRegister pabe{};
    GsTrackedRegister scanmsk{};
    GsTrackedRegister dthe{};
    std::array<GsDrawingContextState, 2> contexts{};
    GsVertexState vertex{};
    std::uint64_t write_sequence{};
};

struct GsAssemblerRegisterWrite {
    std::uint8_t address{};
    std::uint64_t value{};
};

struct GsPackedPositionWrite {
    // PACKED GIF supports the kick-bearing XYZF2 and XYZ2 descriptors. ADC is
    // word 3 bit 15 and is deliberately derived here rather than supplied as
    // a second, potentially inconsistent fact by the caller.
    GsRegisterAddress address{GsRegisterAddress::Xyz2};
    GsPackedRegisterValue payload{};
};

[[nodiscard]] GsPrimitiveTopology decode_gs_primitive_topology(std::uint64_t prim) noexcept;
[[nodiscard]] GsPrimitiveControlState
decode_gs_primitive_controls(const GsPrimitiveAssemblerState& state) noexcept;
[[nodiscard]] bool is_drawable_gs_primitive(GsPrimitiveTopology topology) noexcept;

class GsPrimitiveAssembler {
  public:
    GsPrimitiveAssembler() noexcept;

    // Returns a snapshot only for an unsuppressed XYZ2/XYZF2 drawing kick with
    // enough known control state to select a valid primitive and context.
    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot>
    write(const GsAssemblerRegisterWrite& write) noexcept;

    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot> write(GsRegisterAddress address,
                                                               std::uint64_t value) noexcept;

    // Converts all four PACKED XYZ words to the canonical GS position value,
    // latches the full payload, and applies PACKED ADC suppression.
    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot>
    write_packed_position(const GsPackedPositionWrite& write) noexcept;

    // ADC suppresses rasterization, not vertex submission. The ordinary
    // write result remains empty for an ADC position, while the live
    // renderer can use this snapshot to advance its primitive queue exactly
    // as the GS does.
    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot> current_vertex_snapshot() const noexcept;

    void reset() noexcept;
    // Replaces already-decoded register latches without replaying drawing
    // kicks. The caller is responsible for supplying one coherent GS state.
    void restore_state(const GsPrimitiveAssemblerState& state) noexcept;
    [[nodiscard]] const GsPrimitiveAssemblerState& state() const noexcept;

  private:
    void track(GsTrackedRegister& destination, std::uint64_t value) noexcept;
    [[nodiscard]] GsPrimitiveControlState controls() const noexcept;
    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot>
    write_position(std::uint8_t address, std::uint64_t canonical_value, GsVertexPositionKind kind,
                   bool drawing_kick, bool packed_adc,
                   std::optional<GsPackedRegisterValue> packed_value) noexcept;

    GsPrimitiveAssemblerState state_{};
};

} // namespace takedown::guest
