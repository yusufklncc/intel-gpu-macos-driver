// SPDX-License-Identifier: MIT
// The shaders of user/tests/metal/mtl_reflection.m (kSource). kref.ll, vs.ll and fs.ll are their AIR as this Mac's Metal
// compiler made it (newLibraryWithSource, serializeToURL, nvmtl-air-dis); kref.json, vs.json and fs.json the
// translator's reflection of that AIR (metal2vulkan --emit-meta). refl_host feeds both to the plugin.
#include <metal_stdlib>
using namespace metal;
struct Params { float4 scale; uint count; float pad[3]; };
kernel void kref(device float4 *out [[buffer(0)]], constant Params &p [[buffer(1)]], const device uint *unused [[buffer(5)]],
                 texture2d<float, access::read> src [[texture(0)]], texture2d<uint, access::write> dst [[texture(3)]],
                 depth2d<float> dep [[texture(4)]], sampler s [[sampler(2)]], threadgroup float4 *tg [[threadgroup(0)]],
                 threadgroup uint *tg2 [[threadgroup(1)]], uint i [[thread_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {
    tg[l] = src.read(uint2(i, 0)) * p.scale + dep.sample(s, float2(0.5));
    tg2[l] = i;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    out[i] = tg[(l + 1) % p.count] + float(tg2[0]);
    dst.write(uint4(i), uint2(i, 0));
}
struct VO { float4 pos [[position]]; float2 uv; };
struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
vertex VO vs(VIn in [[stage_in]], constant float4x4 &mvp [[buffer(1)]]) {
    VO o; o.pos = mvp * float4(in.pos, 0, 1); o.uv = in.uv; return o;
}
fragment float4 fs(VO in [[stage_in]], texture2d<half> t [[texture(1)]], sampler s [[sampler(0)]], constant float4 &tint [[buffer(2)]]) {
    return float4(t.sample(s, in.uv)) * tint;
}
