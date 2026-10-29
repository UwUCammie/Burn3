#version 450
// Native render-target passes (takedown/native_scene.cpp, NativeScene::run_pass_op): one triangle over
// the target; the scissor selects the pixels written.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
