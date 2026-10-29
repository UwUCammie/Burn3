"""Draw the host UI layer (images, text, panels) over the game picture.

Apply after the Panini port. The edits are idempotent.

The runtime sends a HostUi record (record type 7,
include/takedown/platform/parallel_gs_host_ui.hpp) when the layer changes: the
MODS page's mod icons and details panel. The replayer keeps the latest layer,
loads each image file once (PNG, JPEG, ...) and draws the items in game
picture coordinates (640x448) mapped onto the picture's place in the window,
so the layer follows the game's menus at any window size and aspect.
"""
from pathlib import Path
import sys


def replace_once(root, name, before, after, present=None):
    path = root / name
    source = path.read_text(encoding='utf-8')
    if after in source or (present is not None and present in source):
        return
    if source.count(before) != 1:
        raise RuntimeError(f'Unsupported renderer source at {name}: {before[:90]!r}')
    path.write_text(source.replace(before, after, 1), encoding='utf-8', newline='\n')


def port(root):
    header = 'dump/gs_dump_parser.hpp'
    replace_once(root, header, '    PaniniWarp = 6,\n', '    PaniniWarp = 6,\n    HostUi = 7,\n',
                 present='    HostUi = 7,\n')
    replace_once(root, header, '''    uint64_t get_host_presentation_serial() const { return host_presentation_serial; }
''', '''    uint64_t get_host_presentation_serial() const { return host_presentation_serial; }
    // Latest host UI layer record (parallel_gs_host_ui.hpp) and how many have
    // been read; decoded by the application.
    const std::vector<uint8_t> &get_host_ui() const { return host_ui; }
    uint64_t get_host_ui_serial() const { return host_ui_serial; }
''', present='get_host_ui_serial()')
    replace_once(root, header, '''    uint64_t host_presentation_serial = 0;
    uint32_t output_resolution_scale''', '''    uint64_t host_presentation_serial = 0;
    std::vector<uint8_t> host_ui;
    uint64_t host_ui_serial = 0;
    uint32_t output_resolution_scale''', present='    std::vector<uint8_t> host_ui;\n')

    replace_once(root, 'dump/gs_dump_parser.cpp', '''        case GSDumpPacketType::HostPresentation:
''', '''        case GSDumpPacketType::HostUi:
        {
            const auto size = read_u32();
            if (eof) break;
            if (size > 512 * 1024)
                throw std::runtime_error("host UI record exceeds budget");
            std::vector<uint8_t> bytes(size);
            read_data(bytes.data(), bytes.size());
            if (eof) break;
            host_ui = std::move(bytes);
            host_ui_serial++;
            break;
        }

        case GSDumpPacketType::HostPresentation:
''', present='case GSDumpPacketType::HostUi:')

    app = 'tools/gs_stream_replayer.cpp'
    replace_once(root, app, '#include "takedown/platform/parallel_gs_host_presentation.hpp"\n',
                 '#include "takedown/platform/parallel_gs_host_presentation.hpp"\n'
                 '#include "takedown/platform/parallel_gs_host_ui.hpp"\n'
                 '#include "texture/texture_files.hpp"\n'
                 '#include <fstream>\n'
                 '#include <unordered_map>\n',
                 present='#include "takedown/platform/parallel_gs_host_ui.hpp"\n')
    replace_once(root, app, '''\t// The options menu as a centered panel of text lines.
''', '''\t// Host UI layer: the latest record and the images it names, loaded once
\t// per path (a failed load is remembered as an empty handle).
\tuint64_t host_ui_serial = 0;
\ttakedown::platform::ParallelGsHostUi host_ui;
\tstd::unordered_map<std::string, ImageHandle> host_ui_images;

\tvoid apply_host_ui()
\t{
\t\tif (parser.get_host_ui_serial() == host_ui_serial)
\t\t\treturn;
\t\thost_ui_serial = parser.get_host_ui_serial();
\t\tconst auto &bytes = parser.get_host_ui();
\t\ttakedown::platform::ParallelGsHostUi next;
\t\tif (!takedown::platform::decode_parallel_gs_host_ui(bytes.data(), bytes.size(), next))
\t\t{
\t\t\tLOGE("Ignoring a malformed host UI record.\\n");
\t\t\treturn;
\t\t}
\t\thost_ui = std::move(next);
\t}

\tconst ImageView *host_ui_image(const std::string &path)
\t{
\t\tauto found = host_ui_images.find(path);
\t\tif (found == host_ui_images.end())
\t\t{
\t\t\tImageHandle image;
\t\t\tstd::ifstream file(path, std::ios::binary);
\t\t\tstd::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
\t\t\tauto texture = data.empty() ? MemoryMappedTexture{} :
\t\t\t               load_texture_from_memory(data.data(), data.size(), ColorSpace::sRGB);
\t\t\tif (!texture.empty())
\t\t\t{
\t\t\t\tconst auto &layout = texture.get_layout();
\t\t\t\tauto info = ImageCreateInfo::immutable_2d_image(layout.get_width(), layout.get_height(),
\t\t\t\t                                                 layout.get_format());
\t\t\t\tImageInitialData initial = {};
\t\t\t\tinitial.data = layout.data();
\t\t\t\timage = get_wsi().get_device().create_image(info, &initial);
\t\t\t}
\t\t\telse
\t\t\t\tLOGE("Host UI: cannot load image %s.\\n", path.c_str());
\t\t\tfound = host_ui_images.emplace(path, image).first;
\t\t}
\t\treturn found->second ? &found->second->get_view() : nullptr;
\t}

\tvoid render_host_ui(float viewport_width, float viewport_height)
\t{
\t\tif (host_ui.items.empty())
\t\t\treturn;
\t\t// The picture's place in the window, as the scanout draw fits it.
\t\tconst float aspect = host_presentation.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
\t\tvec2 origin(0.0f), size(viewport_width, viewport_height);
\t\tif (viewport_width > viewport_height * aspect)
\t\t{
\t\t\tsize.x = viewport_height * aspect;
\t\t\torigin.x = (viewport_width - size.x) * 0.5f;
\t\t}
\t\telse
\t\t{
\t\t\tsize.y = viewport_width / aspect;
\t\t\torigin.y = (viewport_height - size.y) * 0.5f;
\t\t}
\t\tconst vec2 scale = size / vec2(takedown::platform::kParallelGsHostUiPictureWidth,
\t\t                               takedown::platform::kParallelGsHostUiPictureHeight);
\t\t// Later items are drawn on top (smaller depth).
\t\tfloat depth = 0.29f;
\t\tfor (const auto &item : host_ui.items)
\t\t{
\t\t\tconst vec4 color = vec4(float(item.colour & 0xffu), float((item.colour >> 8) & 0xffu),
\t\t\t                        float((item.colour >> 16) & 0xffu), float(item.colour >> 24)) / 255.0f;
\t\t\t// The game's UI is authored for a 4:3 picture. On widescreen, centre each
\t\t\t// item that does not span the picture in a 4:3 area instead of stretching
\t\t\t// it across the full 16:9 width. Full-width backgrounds still cover the
\t\t\t// picture. One branch per item, no extra passes.
\t\t\tvec2 item_origin = origin;
\t\t\tvec2 item_scale = scale;
\t\t\tif (host_presentation.widescreen &&
\t\t\t    float(item.width) < takedown::platform::kParallelGsHostUiPictureWidth)
\t\t\t{
\t\t\t\tconst float ui_width = size.y * (4.0f / 3.0f);
\t\t\t\titem_scale.x = ui_width / takedown::platform::kParallelGsHostUiPictureWidth;
\t\t\t\titem_origin.x = origin.x + (size.x - ui_width) * 0.5f;
\t\t\t}
\t\t\tvec2 at = item_origin + vec2(float(item.x), float(item.y)) * item_scale;
\t\t\tvec2 extent = vec2(float(item.width), float(item.height)) * item_scale;
\t\t\tdepth -= 0.0005f;
\t\t\tswitch (item.kind)
\t\t\t{
\t\t\tcase takedown::platform::ParallelGsUiKind::Rectangle:
\t\t\t\tflat_renderer.render_quad(vec3(at, depth), extent, color);
\t\t\t\tbreak;
\t\t\tcase takedown::platform::ParallelGsUiKind::Image:
\t\t\t{
\t\t\t\tconst ImageView *view = host_ui_image(item.text);
\t\t\t\tif (!view)
\t\t\t\t\tbreak;
\t\t\t\tconst float w = float(view->get_image().get_width());
\t\t\t\tconst float h = float(view->get_image().get_height());
\t\t\t\tif ((item.flags & takedown::platform::kParallelGsUiKeepAspect) != 0)
\t\t\t\t\textent.x = extent.y * w / h;
\t\t\t\tflat_renderer.render_textured_quad(*view, vec3(at, depth), extent, vec2(0.0f), vec2(w, h),
\t\t\t\t                                   DrawPipeline::AlphaBlend, color,
\t\t\t\t                                   StockSampler::TrilinearClamp);
\t\t\t\tbreak;
\t\t\t}
\t\t\tcase takedown::platform::ParallelGsUiKind::Text:
\t\t\t{
\t\t\t\tconst auto size_bits = item.flags & takedown::platform::kParallelGsUiTextSizeMask;
\t\t\t\tauto &font = GRANITE_UI_MANAGER()->get_font(
\t\t\t\t\tsize_bits == takedown::platform::kParallelGsUiTextSmall ? UI::FontSize::Small :
\t\t\t\t\tsize_bits == takedown::platform::kParallelGsUiTextLarge ? UI::FontSize::Large :
\t\t\t\t\tUI::FontSize::Normal);
\t\t\t\tconst auto align = (item.flags & takedown::platform::kParallelGsUiAlignRight) != 0 ?
\t\t\t\t                   Font::Alignment::CenterRight : Font::Alignment::CenterLeft;
\t\t\t\tflat_renderer.render_text(font, item.text.c_str(), vec3(at, depth), extent, color, align);
\t\t\t\tbreak;
\t\t\t}
\t\t\t}
\t\t}
\t}

\t// The options menu as a centered panel of text lines.
''', present='void render_host_ui(')
    replace_once(root, app, '''\t\trender_host_overlay(cmd->get_viewport().width, cmd->get_viewport().height);
\t\tflat_renderer.flush(''', '''\t\tapply_host_ui();
\t\trender_host_ui(cmd->get_viewport().width, cmd->get_viewport().height);
\t\trender_host_overlay(cmd->get_viewport().width, cmd->get_viewport().height);
\t\tflat_renderer.flush(''', present='\t\trender_host_ui(')


    # Native menu text uses imported Burnout atlases and owned glyph metrics.
    replace_once(root, app, '#include "takedown/platform/parallel_gs_host_ui.hpp"\n',
                 '#include "takedown/platform/parallel_gs_host_ui.hpp"\n'
                 '#include "takedown/presentation/bitmap_font.hpp"\n',
                 present='#include "takedown/presentation/bitmap_font.hpp"')
    helper = Path(__file__).with_name('parallel_gs_burnout_ui.inc').read_text(encoding='utf-8')
    replace_once(root, app, '\tvoid render_host_ui(float viewport_width, float viewport_height)\n',
                 helper + '\n\tvoid render_host_ui(float viewport_width, float viewport_height)\n',
                 present='void render_burnout_text(')
    path = root / app
    source = path.read_text(encoding='utf-8')
    start = source.index('\tstd::string burnout_font_directory = ')
    canvas = source.rfind('\tImageHandle burnout_ui_canvas;', 0, start)
    if canvas >= 0: start = canvas
    end = source.index('\tvoid render_host_ui(', start)
    updated = source[:start] + helper.rstrip() + '\n\n' + source[end:]
    if updated != source:
        path.write_text(updated, encoding='utf-8', newline='\n')
    replace_once(root, app, '\t\t\tcase takedown::platform::ParallelGsUiKind::Rectangle:\n',
                 '\t\t\tcase takedown::platform::ParallelGsUiKind::FontDirectory:\n'
                 '\t\t\t\tif (burnout_font_directory != item.text) burnout_fonts.clear();\n'
                 '\t\t\t\tburnout_font_directory = item.text;\n'
                 '\t\t\t\tbreak;\n'
                 '\t\t\tcase takedown::platform::ParallelGsUiKind::Rectangle:\n',
                 present='case takedown::platform::ParallelGsUiKind::FontDirectory:')
    replace_once(root, app, '\t\t\t\tconst auto size_bits = item.flags & takedown::platform::kParallelGsUiTextSizeMask;\n',
                 '\t\t\t\tif (item.flags & takedown::platform::kParallelGsUiBurnoutFont)\n'
                 '\t\t\t\t{\n'
                 '\t\t\t\t\trender_burnout_text(item, at, scale, depth, color);\n'
                 '\t\t\t\t\tbreak;\n'
                 '\t\t\t\t}\n'
                 '\t\t\t\tconst auto size_bits = item.flags & takedown::platform::kParallelGsUiTextSizeMask;\n',
                 present='render_burnout_text(item, at, scale, depth, color);')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: port_parallel_gs_host_ui.py <renderer-source>')
    port(Path(sys.argv[1]).absolute())
