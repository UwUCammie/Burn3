#version 450
// Temporal AA, object motion (NativeScene::render): the screen motion of a moving native draw (a car, a prop that
// moved), from this frame's position to the frame before's, in UV units (FSR 3.1's convention: previous = current +
// motion). Drawn over the camera motion (takedown_taa_motion.comp) where the draw is the visible surface.
layout(location = 3) in vec4 vCurrent;
layout(location = 4) in vec4 vPrevious;
layout(location = 0) out vec2 Motion;
void main() {
    if (vPrevious.w <= 0.0 || vCurrent.w <= 0.0)
        discard;
    Motion = (vPrevious.xy / vPrevious.w - vCurrent.xy / vCurrent.w) * 0.5;
}
