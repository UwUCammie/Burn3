#version 450
layout(location = 0) in vec2 Position;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec4 Color;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
void main() {
    gl_Position = vec4(Position / vec2(640.0, 448.0) * 2.0 - 1.0, 0.0, 1.0);
    vUV = UV; vColor = Color;
}
