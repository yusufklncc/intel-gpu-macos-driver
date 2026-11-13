// SPDX-License-Identifier: MIT
// vkscene check pass: mode 0/1 = an 8x8 grid of cells (instance = cell = texture index), mode 2 = one triangle in
// cell 0 whose long edge cuts pixels (the MSAA resolve must blend there). Depth from the push constants.
#version 450
layout(push_constant) uniform PC { layout(offset = 80) int mode; layout(offset = 84) float depth; } pc;
layout(location = 0) flat out uint vCell;
void main() {
    uint c = gl_InstanceIndex;
    vCell = c;
    if (pc.mode == 2) {
        // pixels of a 256x256 target: (0,0), (31.3,0), (0,31.3); NDC = px / 128 - 1
        vec2 p[3] = vec2[](vec2(0.0, 0.0), vec2(31.3, 0.0), vec2(0.0, 31.3));
        gl_Position = vec4(p[gl_VertexIndex] / 128.0 - 1.0, pc.depth, 1.0);
        return;
    }
    vec2 q[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 cell = vec2(float(c % 8u), float(c / 8u));
    gl_Position = vec4((cell + q[gl_VertexIndex]) / 4.0 - 1.0, pc.depth, 1.0);
}
