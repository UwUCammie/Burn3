#pragma once
// Where the game picture sits in the window. The picture is 4:3, or 16:9
// with widescreen, letterboxed/pillarboxed and centred in the viewport.
namespace TakedownRenderer
{
struct PictureRect
{
	float x, y, width, height;
};
PictureRect fit_picture(float viewport_width, float viewport_height, bool widescreen);
}
