#include "takedown/presentation/gs_scanout.hpp"

#include <cstdint>
#include <vector>

namespace takedown::presentation {

const char* to_string(const GsScanoutPlanStatus status) noexcept {
    switch (status) {
    case GsScanoutPlanStatus::Ready:
        return "ready";
    case GsScanoutPlanStatus::MissingPrivilegedRegisters:
        return "missing privileged GS registers";
    case GsScanoutPlanStatus::NoEnabledCircuit:
        return "no enabled display circuit";
    case GsScanoutPlanStatus::DualCircuitCompositionUnsupported:
        return "dual display-circuit composition is unsupported";
    case GsScanoutPlanStatus::InterlacedSourceRequiresConversion:
        return "interlaced source requires progressive conversion";
    case GsScanoutPlanStatus::UnsupportedPixelStorageMode:
        return "unsupported display pixel storage mode";
    case GsScanoutPlanStatus::ZeroFramebufferWidth:
        return "zero display framebuffer width";
    case GsScanoutPlanStatus::InvalidOutputGeometry:
        return "invalid or oversized display geometry";
    default:
        return "unknown GS scanout plan status";
    }
}

const char* to_string(const GsScanoutRenderStatus status) noexcept {
    switch (status) {
    case GsScanoutRenderStatus::Rendered:
        return "rendered";
    case GsScanoutRenderStatus::InvalidPlan:
        return "invalid scanout plan";
    case GsScanoutRenderStatus::FrameTooLarge:
        return "scanout frame exceeds host limit";
    default:
        return "unknown GS scanout render status";
    }
}

GsScanoutPlan plan_gs_scanout(const guest::GsPrivilegedDisplayState& display_state) noexcept {
    if (!display_state.pmode.seen || !display_state.smode2.seen) {
        return GsScanoutPlan{.status = GsScanoutPlanStatus::MissingPrivilegedRegisters};
    }
    const guest::GsPmodeFields pmode = guest::decode_gs_pmode(display_state.pmode.raw);
    if (pmode.circuit1_enabled && pmode.circuit2_enabled) {
        return GsScanoutPlan{.status = GsScanoutPlanStatus::DualCircuitCompositionUnsupported};
    }
    if (!pmode.circuit1_enabled && !pmode.circuit2_enabled) {
        return GsScanoutPlan{.status = GsScanoutPlanStatus::NoEnabledCircuit};
    }

    // Circuit 1 is preferred; circuit 2 is used only when circuit 1 is off.
    const unsigned int circuit_index = pmode.circuit1_enabled ? 0U : 1U;
    if (display_state.dispfb[circuit_index].seen && display_state.display[circuit_index].seen) {
        const guest::GsSmode2Fields smode2 = guest::decode_gs_smode2(display_state.smode2.raw);
        const guest::GsDispfbFields dispfb =
            guest::decode_gs_dispfb(display_state.dispfb[circuit_index].raw);
        const guest::GsDisplayFields display =
            guest::decode_gs_display(display_state.display[circuit_index].raw);
        switch (dispfb.pixel_storage_mode) {
        case guest::kGsPsmct32:
        case guest::kGsPsmct16:
        case guest::kGsPsmct16s:
            break;
        default:
            return GsScanoutPlan{.status = GsScanoutPlanStatus::UnsupportedPixelStorageMode};
        }
        if (dispfb.framebuffer_width == 0U) {
            return GsScanoutPlan{.status = GsScanoutPlanStatus::ZeroFramebufferWidth};
        }

        const unsigned int output_width = static_cast<unsigned int>(display.display_width_raw) + 1U;
        const unsigned int output_height = static_cast<unsigned int>(display.display_height_raw) + 1U;
        const unsigned int horizontal_repeat = static_cast<unsigned int>(display.horizontal_magnification) + 1U;
        const unsigned int vertical_repeat = static_cast<unsigned int>(display.vertical_magnification) + 1U;
        if (output_width > 4096U || output_height > 4096U) {
            return GsScanoutPlan{.status = GsScanoutPlanStatus::InvalidOutputGeometry};
        }
        // Visible source pixels: ceil(output / repeat) in each direction.
        const auto ceil_div = [](const unsigned int numerator, const unsigned int denominator) {
            return numerator / denominator + (numerator % denominator != 0U ? 1U : 0U);
        };
        if (ceil_div(output_width, horizontal_repeat) == 0U ||
            ceil_div(output_height, vertical_repeat) == 0U) {
            return GsScanoutPlan{.status = GsScanoutPlanStatus::InvalidOutputGeometry};
        }

        return GsScanoutPlan{
            .status = GsScanoutPlanStatus::Ready,
            .circuit = circuit_index + 1U,
            .source_surface =
                guest::GsLocalMemorySurface{
                    .base_pointer = static_cast<std::uint32_t>(dispfb.framebuffer_base_pointer) << 5U,
                    .buffer_width = dispfb.framebuffer_width,
                    .pixel_storage_mode = dispfb.pixel_storage_mode,
                },
            .source_x = dispfb.display_x,
            .source_y = dispfb.display_y,
            .source_width = ceil_div(output_width, horizontal_repeat),
            .source_height = ceil_div(output_height, vertical_repeat),
            .crtc_width = output_width,
            .crtc_height = output_height,
            .output_width = ceil_div(output_width, horizontal_repeat),
            .output_height = ceil_div(output_height, vertical_repeat),
            .horizontal_repeat = horizontal_repeat,
            .vertical_repeat = vertical_repeat,
            .progressive_frame_extraction = smode2.interlaced,
        };
    }
    return GsScanoutPlan{.status = GsScanoutPlanStatus::MissingPrivilegedRegisters};
}

GsScanoutFrame render_gs_scanout(const guest::GsLocalMemory& memory, const GsScanoutPlan& plan) {
    if (!plan.ready() || plan.output_width == 0U || plan.output_height == 0U ||
        plan.horizontal_repeat == 0U || plan.vertical_repeat == 0U) {
        return GsScanoutFrame{.status = GsScanoutRenderStatus::InvalidPlan};
    }
    const unsigned long long pixel_count = static_cast<unsigned long long>(plan.output_width) *
                                           static_cast<unsigned long long>(plan.output_height);
    if (pixel_count > 16777216U) {
        return GsScanoutFrame{.status = GsScanoutRenderStatus::FrameTooLarge};
    }

    GsScanoutFrame frame{
        .status = GsScanoutRenderStatus::Rendered,
        .width = plan.output_width,
        .height = plan.output_height,
        .rgba = std::vector<std::uint8_t>(pixel_count * 4U, 0U),
    };
    for (std::uint32_t output_y = 0U; output_y < plan.output_height; ++output_y) {
        const unsigned int source_y = plan.source_y + output_y;
        for (std::uint32_t output_x = 0U; output_x < plan.output_width; ++output_x) {
            const unsigned int source_x = plan.source_x + output_x;
            const auto source = memory.read_rgba8(plan.source_surface, source_x, source_y);
            if (!source) {
                return GsScanoutFrame{.status = GsScanoutRenderStatus::InvalidPlan};
            }
            const unsigned long long destination =
                (static_cast<unsigned long long>(output_y) * plan.output_width + output_x) * 4U;
            frame.rgba[destination] = source->red;
            frame.rgba[destination + 1U] = source->green;
            frame.rgba[destination + 2U] = source->blue;
            frame.rgba[destination + 3U] = source->alpha;
        }
    }
    return frame;
}

} // namespace takedown::presentation
