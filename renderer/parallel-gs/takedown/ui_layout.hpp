#pragma once
// Places the game's 4:3 UI on a wider picture.
//
// The game authors its HUD and menus in a 640x448 picture shown at 4:3. On a
// wider picture the UI keeps its shape: by default it is narrowed about the
// centre as one 4:3 block, so every layout stays exactly as the game drew it,
// and items spanning the whole width (backgrounds, fades) stretch to fill the
// picture. Anything else is an explicit exception in a placement table
// (ui_placements.inc), matched by the texture an item is drawn with, the part
// of that texture, where it is, and which other items are on screen; it can
// move to the left or right edge, stretch, or keep its ends on the edges.
//
// Plain C++17 (no Granite), so the tests build anywhere.
#include <cstdint>
#include <string>
#include <vector>

namespace TakedownRenderer
{
constexpr float kUiPictureWidth = 640.0f;
constexpr float kUiPictureHeight = 448.0f;

struct UiElement
{
	// Bounds in picture pixels (the 640x448 picture).
	float x0, y0, x1, y1;
	// Glyphs.
	bool text;
	// The GS texture it is drawn with (GSInterface::TakedownUiBatch::texture_key),
	// 0 when untextured or drawn by the host.
	uint64_t texture = 0;
	// The texels it uses: bounds in texture pixels.
	float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

// x' = x * scale + offset, in picture pixels of the 640-wide picture that is
// shown across the whole (wider) picture rectangle.
struct UiPlacement
{
	float scale = 1.0f;
	float offset = 0.0f;
	// Points on or beyond the picture's left and right edges stay there.
	bool pin = false;
	float apply(float x) const
	{
		if (pin && (x <= 0.5f || x >= kUiPictureWidth - 0.5f))
			return x;
		return x * scale + offset;
	}
};

enum class UiAnchor
{
	Centre,  // narrowed about the centre with everything else
	Left,    // narrowed, on the left edge
	Right,   // narrowed, on the right edge
	Stretch, // across the whole picture
	Pin,     // centred, but ends on the picture's edges stay there
	Span,    // left end placed as Left, right end as Right (a bar between two edge pieces)
};

// UiRule::texture for items drawn without a texture.
constexpr uint64_t kUiUntextured = ~uint64_t(0);

// One exception. An element matches when every condition given matches;
// the first matching rule wins.
struct UiRule
{
	UiAnchor anchor = UiAnchor::Centre;
	// texture: the element's texture key; 0 matches any, kUiUntextured only
	// untextured items.
	uint64_t texture = 0;
	// The element's texels lie inside this rectangle (texture pixels).
	bool has_texels = false;
	float texels[4] = {};
	// The element lies inside this rectangle (picture pixels).
	bool has_area = false;
	float area[4] = {};
	// The element is at least this wide and tall (picture pixels).
	float min_size[2] = {};
	// The element starts (its top-left corner) inside this rectangle: for
	// effects that grow out of an area, like flames streaming off a bar.
	bool has_origin = false;
	float origin[4] = {};
	// The element is at most this wide and tall (0: no limit).
	float max_size[2] = {};
	// Glyphs only (1), never glyphs (0), either (-1).
	int text = -1;
	// Only while an element drawn with this texture is on screen (0: always).
	uint64_t when = 0;
	// When `when` is set, the texels that element uses lie inside this rectangle.
	bool has_when_texels = false;
	float when_texels[4] = {};
	// For the layout log.
	std::string name;
};

// Rules, one per line:
//
//   <anchor> [name=<word>] [tex=<hex key>|none] [texels=u0,v0,u1,v1]
//            [area=x0,y0,x1,y1] [origin=x0,y0,x1,y1] [min_size=w,h] [max_size=w,h] [text=0|1] [when=<hex key>] [when_texels=u0,v0,u1,v1]
//
// anchor is centre, left, right, stretch, pin or span. '#' starts a comment.
// Malformed lines are reported in errors and skipped.
std::vector<UiRule> parse_ui_rules(const std::string &text, std::vector<std::string> *errors = nullptr);

// The built-in table (ui_placements.inc), or the file named by
// TAKEDOWN_UI_PLACEMENTS when that is set (for editing rules live).
const std::vector<UiRule> &builtin_ui_rules();

// picture_aspect is the shown picture's width over its height. At 4:3 (or
// narrower) every placement is the identity. matched (optional) receives the
// index of each element's rule, or -1.
std::vector<UiPlacement> place_ui(const std::vector<UiElement> &elements, float picture_aspect,
                                  const std::vector<UiRule> &rules, std::vector<int> *matched = nullptr);
}
