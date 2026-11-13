// SPDX-License-Identifier: MIT
// vkscene: instanced cubes; per-instance data from a storage buffer (set 1, dynamic offset per frame in flight)
#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uv;
struct Inst { mat4 model; vec4 tint; uint tex; uint pad0, pad1, pad2; };
layout(std430, set = 1, binding = 0) readonly buffer Insts { Inst inst[]; };
layout(push_constant) uniform PC { mat4 viewproj; vec4 light; } pc;
layout(location = 0) out vec3 vN;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTex;
layout(location = 3) out vec4 vTint;
void main() {
    Inst i = inst[gl_InstanceIndex];
    gl_Position = pc.viewproj * (i.model * vec4(pos, 1.0));
    vN = mat3(i.model) * nrm;
    vUV = uv;
    vTex = i.tex;
    vTint = i.tint;
}
