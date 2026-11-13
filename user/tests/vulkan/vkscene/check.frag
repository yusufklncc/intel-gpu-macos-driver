// SPDX-License-Identifier: MIT
// vkscene check pass: mode 0 = texel (0,0) of texture[cell] (nonuniform index), 1 = magenta (must lose the depth
// test), 2 = white (must win it)
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 0, binding = 1) uniform texture2D tex[];
layout(push_constant) uniform PC { layout(offset = 80) int mode; layout(offset = 84) float depth; } pc;
layout(location = 0) flat in uint vCell;
layout(location = 0) out vec4 o;
void main() {
    if (pc.mode == 1) o = vec4(1.0, 0.0, 1.0, 1.0);
    else if (pc.mode == 2) o = vec4(1.0);
    else o = texelFetch(sampler2D(tex[nonuniformEXT(vCell)], smp), ivec2(0, 0), 0);
}
