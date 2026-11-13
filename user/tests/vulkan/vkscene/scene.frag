// SPDX-License-Identifier: MIT
// vkscene: a bindless texture array (descriptor indexing: runtime array, variable count, partially bound, update
// after bind) indexed per instance with nonuniformEXT
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 0, binding = 1) uniform texture2D tex[];
layout(push_constant) uniform PC { mat4 viewproj; vec4 light; } pc;
layout(location = 0) in vec3 vN;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTex;
layout(location = 3) in vec4 vTint;
layout(location = 0) out vec4 o;
void main() {
    float d = max(dot(normalize(vN), normalize(pc.light.xyz)), 0.0) * 0.75 + 0.25;
    vec4 c = texture(sampler2D(tex[nonuniformEXT(vTex)], smp), vUV);
    o = vec4(c.rgb * vTint.rgb * d, 1.0);
}
