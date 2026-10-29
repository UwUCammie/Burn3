#include "picture_layout.hpp"
namespace TakedownRenderer
{
PictureRect fit_picture(float viewport_width, float viewport_height, bool widescreen)
{
	const float aspect = widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
	PictureRect r = { 0.0f, 0.0f, viewport_width, viewport_height };
	if (viewport_width > viewport_height * aspect)
	{
		r.width = viewport_height * aspect;
		r.x = (viewport_width - r.width) * 0.5f;
	}
	else
	{
		r.height = viewport_width / aspect;
		r.y = (viewport_height - r.height) * 0.5f;
	}
	return r;
}
}
