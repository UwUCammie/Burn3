#include "ui_layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace TakedownRenderer
{
namespace
{
// Picture (or texture) pixels of slack when testing containment.
constexpr float kSlack = 1.0f;

const char kBuiltinRules[] =
#include "ui_placements.inc"
	;

bool inside(float x0, float y0, float x1, float y1, const float (&r)[4])
{
	return x0 >= r[0] - kSlack && y0 >= r[1] - kSlack && x1 <= r[2] + kSlack && y1 <= r[3] + kSlack;
}

bool parse_rect(const std::string &value, float (&out)[4])
{
	return std::sscanf(value.c_str(), "%f,%f,%f,%f", &out[0], &out[1], &out[2], &out[3]) == 4;
}

bool parse_key(const std::string &value, uint64_t &out)
{
	char *end = nullptr;
	out = std::strtoull(value.c_str(), &end, 16);
	return end && *end == '\0' && out != 0;
}

bool matches(const UiRule &rule, const UiElement &e)
{
	if (rule.texture == kUiUntextured ? e.texture != 0 : rule.texture != 0 && e.texture != rule.texture)
		return false;
	if (rule.has_texels && !inside(e.u0, e.v0, e.u1, e.v1, rule.texels))
		return false;
	if (rule.has_area && !inside(e.x0, e.y0, e.x1, e.y1, rule.area))
		return false;
	if (e.x1 - e.x0 < rule.min_size[0] || e.y1 - e.y0 < rule.min_size[1])
		return false;
	if ((rule.max_size[0] > 0.0f && e.x1 - e.x0 > rule.max_size[0]) ||
	    (rule.max_size[1] > 0.0f && e.y1 - e.y0 > rule.max_size[1]))
		return false;
	if (rule.has_origin && !inside(e.x0, e.y0, e.x0, e.y0, rule.origin))
		return false;
	if (rule.text >= 0 && e.text != (rule.text != 0))
		return false;
	return true;
}
}

std::vector<UiRule> parse_ui_rules(const std::string &text, std::vector<std::string> *errors)
{
	std::vector<UiRule> rules;
	std::istringstream lines(text);
	std::string line;
	int number = 0;
	while (std::getline(lines, line))
	{
		number++;
		const auto hash = line.find('#');
		if (hash != std::string::npos)
			line.resize(hash);
		std::istringstream words(line);
		std::string word;
		if (!(words >> word))
			continue;
		UiRule rule;
		bool ok = true;
		if (word == "centre")
			rule.anchor = UiAnchor::Centre;
		else if (word == "left")
			rule.anchor = UiAnchor::Left;
		else if (word == "right")
			rule.anchor = UiAnchor::Right;
		else if (word == "stretch")
			rule.anchor = UiAnchor::Stretch;
		else if (word == "pin")
			rule.anchor = UiAnchor::Pin;
		else if (word == "span")
			rule.anchor = UiAnchor::Span;
		else
			ok = false;
		while (ok && words >> word)
		{
			const auto eq = word.find('=');
			if (eq == std::string::npos)
			{
				ok = false;
				break;
			}
			const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
			if (key == "name")
				rule.name = value;
			else if (key == "tex" && value == "none")
				rule.texture = kUiUntextured;
			else if (key == "tex")
				ok = parse_key(value, rule.texture);
			else if (key == "texels")
				ok = rule.has_texels = parse_rect(value, rule.texels);
			else if (key == "area")
				ok = rule.has_area = parse_rect(value, rule.area);
			else if (key == "origin")
				ok = rule.has_origin = parse_rect(value, rule.origin);
			else if (key == "max_size")
				ok = std::sscanf(value.c_str(), "%f,%f", &rule.max_size[0], &rule.max_size[1]) == 2;
			else if (key == "min_size")
				ok = std::sscanf(value.c_str(), "%f,%f", &rule.min_size[0], &rule.min_size[1]) == 2;
			else if (key == "text" && (value == "0" || value == "1"))
				rule.text = value == "1";
			else if (key == "when")
				ok = parse_key(value, rule.when);
			else if (key == "when_texels")
				ok = rule.has_when_texels = parse_rect(value, rule.when_texels);
			else
				ok = false;
		}
		if (ok)
			rules.push_back(std::move(rule));
		else if (errors)
			errors->push_back("line " + std::to_string(number) + ": " + line);
	}
	return rules;
}

const std::vector<UiRule> &builtin_ui_rules()
{
	static const std::vector<UiRule> rules = []() {
		std::string text = kBuiltinRules;
		if (const char *path = std::getenv("TAKEDOWN_UI_PLACEMENTS"); path && *path)
		{
			std::ifstream file(path);
			if (file)
			{
				std::ostringstream contents;
				contents << file.rdbuf();
				text = contents.str();
			}
			else
				std::fprintf(stderr, "UI placements: cannot read %s, using the built-in table.\n", path);
		}
		std::vector<std::string> errors;
		auto parsed = parse_ui_rules(text, &errors);
		for (const auto &error : errors)
			std::fprintf(stderr, "UI placements: skipped %s\n", error.c_str());
		return parsed;
	}();
	return rules;
}

std::vector<UiPlacement> place_ui(const std::vector<UiElement> &elements, float picture_aspect,
                                  const std::vector<UiRule> &rules, std::vector<int> *matched)
{
	std::vector<UiPlacement> placements(elements.size());
	if (matched)
		matched->assign(elements.size(), -1);
	const float scale = (4.0f / 3.0f) / picture_aspect;
	if (!(scale < 1.0f) || elements.empty())
		return placements;
	const float free_space = (1.0f - scale) * kUiPictureWidth;

	// Which rules apply on this screen.
	std::vector<bool> active(rules.size(), true);
	for (std::size_t r = 0; r < rules.size(); r++)
	{
		const UiRule &rule = rules[r];
		if (rule.when == 0)
			continue;
		bool present = false;
		for (const UiElement &e : elements)
			present = present || (e.texture == rule.when &&
			                       (!rule.has_when_texels || inside(e.u0, e.v0, e.u1, e.v1, rule.when_texels)));
		active[r] = present;
	}

	for (std::size_t i = 0; i < elements.size(); i++)
	{
		const UiElement &e = elements[i];
		UiAnchor anchor = UiAnchor::Centre;
		// Backgrounds and fades over the whole width fill the picture.
		if (e.x0 <= kSlack && e.x1 >= kUiPictureWidth - kSlack)
			anchor = UiAnchor::Stretch;
		for (std::size_t r = 0; r < rules.size(); r++)
		{
			if (active[r] && matches(rules[r], e))
			{
				anchor = rules[r].anchor;
				if (matched)
					(*matched)[i] = int(r);
				break;
			}
		}
		switch (anchor)
		{
		case UiAnchor::Centre:
			placements[i] = { scale, free_space * 0.5f, false };
			break;
		case UiAnchor::Left:
			placements[i] = { scale, 0.0f, false };
			break;
		case UiAnchor::Right:
			placements[i] = { scale, free_space, false };
			break;
		case UiAnchor::Stretch:
			placements[i] = {};
			break;
		case UiAnchor::Pin:
			placements[i] = { scale, free_space * 0.5f, true };
			break;
		case UiAnchor::Span:
			// x0 -> x0 * scale, x1 -> x1 * scale + free_space.
			if (e.x1 - e.x0 > kSlack)
			{
				const float s = scale + free_space / (e.x1 - e.x0);
				placements[i] = { s, -e.x0 * (s - scale), false };
			}
			else
				placements[i] = { scale, free_space * 0.5f, false };
			break;
		}
	}
	return placements;
}
}
