// Tests of place_ui (ui_layout.hpp): how the game's 4:3 UI is placed on a 16:9 picture.
#include "ui_layout.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace TakedownRenderer;

namespace
{
int failures = 0;

void check(bool condition, const char *what)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

bool near(float a, float b)
{
	return std::fabs(a - b) < 1e-3f;
}

constexpr float kWide = 16.0f / 9.0f;

UiElement item(float x0, float y0, float x1, float y1, uint64_t texture = 0, float u0 = 0, float v0 = 0,
               float u1 = 0, float v1 = 0, bool text = false)
{
	UiElement e{ x0, y0, x1, y1, text };
	e.texture = texture;
	e.u0 = u0;
	e.v0 = v0;
	e.u1 = u1;
	e.v1 = v1;
	return e;
}
}

int main()
{
	const std::vector<UiRule> none;
	// At 4:3 nothing moves.
	{
		const auto p = place_ui({ item(10, 10, 100, 40) }, 4.0f / 3.0f, none);
		check(near(p[0].scale, 1.0f) && near(p[0].offset, 0.0f), "4:3 is the identity");
	}
	// By default the UI is one centred 4:3 block; full-width items stretch.
	{
		const auto p = place_ui({ item(0, 10, 180, 60), item(500, 380, 640, 440), item(0, 0, 640, 448) }, kWide, none);
		check(near(p[0].scale, 0.75f) && near(p[0].offset, 80.0f), "an edge item moves with the block");
		check(near(p[1].offset, p[0].offset), "everything moves together");
		check(near(p[2].scale, 1.0f) && near(p[2].offset, 0.0f), "full-width background stretches");
	}
	// Rules: by texture and texels, by area, and only while another texture is on screen.
	{
		std::vector<std::string> errors;
		const auto rules = parse_ui_rules(
			"# race HUD\n"
			"left name=pos tex=1a2b texels=0,0,64,32\n"
			"right area=500,0,640,80 when=c0de\n"
			"pin tex=99 text=0\n"
			"stretch tex=77 area=0,300,640,448\n"
			"sideways tex=1\n",
			&errors);
		check(rules.size() == 4 && errors.size() == 1, "rules parse, bad lines are reported");
		check(rules[0].name == "pos" && rules[0].texture == 0x1a2b, "rule fields parse");

		const auto p = place_ui({ item(0, 10, 60, 40, 0x1a2b, 0, 0, 64, 32), item(0, 50, 60, 80, 0x1a2b, 64, 0, 128, 32),
		                          item(520, 10, 630, 40, 0x5), item(0, 142, 84, 155, 0x99) },
		                        kWide, rules);
		check(near(p[0].offset, 0.0f) && near(p[0].scale, 0.75f), "texture and texels match: left edge");
		check(near(p[1].offset, 80.0f), "other texels of the same texture stay centred");
		check(near(p[2].offset, 80.0f), "a rule waits for its screen");
		check(p[3].pin && near(p[3].apply(0.0f), 0.0f) && near(p[3].apply(84.0f), 84.0f * 0.75f + 80.0f),
		      "pinned items keep their edge ends");

		std::vector<int> matched;
		const auto q = place_ui({ item(520, 10, 630, 40, 0x5), item(200, 200, 210, 210, 0xc0de) }, kWide, rules,
		                        &matched);
		check(near(q[0].offset, 160.0f) && matched[0] == 1 && matched[1] == -1, "a rule applies on its screen");
	}
	// A span keeps its ends on the edges; untextured matches only untextured items.
	{
		const auto rules = parse_ui_rules("span tex=none area=0,380,610,392\nright area=600,350,640,392\n");
		const auto p = place_ui({ item(0, 387, 608, 389), item(608, 359, 640, 389, 0x6), item(0, 387, 600, 389, 0x6) },
		                        kWide, rules);
		check(near(p[0].apply(0.0f), 0.0f) && near(p[0].apply(608.0f), p[1].apply(608.0f)) &&
		      near(p[1].apply(640.0f), 640.0f), "a span meets the edge piece");
		check(near(p[2].offset, 80.0f) && near(p[2].scale, 0.75f), "tex=none skips textured items");
	}
	// min_size keeps small pieces out.
	{
		const auto rules = parse_ui_rules("stretch texels=0,0,64,64 min_size=60,28\n");
		const auto p = place_ui({ item(64, 60, 128, 119, 0x9, 0, 0, 64, 64), item(84, 85, 108, 96, 0x8, 0, 0, 20, 11) },
		                        kWide, rules);
		check(near(p[0].scale, 1.0f) && near(p[1].scale, 0.75f), "min_size matches only large pieces");
	}
	// Pieces anchored by where they start: flames streaming past the area move with it.
	{
		const auto rules = parse_ui_rules("left origin=0,300,420,448 max_size=480,148\n");
		const auto p = place_ui({ item(380, 370, 520, 420, 0x7), item(0, 300, 640, 448), item(450, 380, 600, 430) },
		                        kWide, rules);
		check(near(p[0].offset, 0.0f) && near(p[1].scale, 1.0f) && near(p[2].offset, 80.0f),
		      "origin matches flames past the area, not wide flashes or pieces starting outside");
	}
	// The first matching rule wins, and rules override the full-width default.
	{
		const auto rules = parse_ui_rules("centre tex=5\nstretch tex=5\n");
		const auto p = place_ui({ item(0, 0, 640, 448, 0x5) }, kWide, rules);
		check(near(p[0].scale, 0.75f), "first rule wins over later rules and the default");
	}
	if (failures == 0)
		std::printf("ui layout tests passed\n");
	return failures == 0 ? 0 : 1;
}
