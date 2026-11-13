// SPDX-License-Identifier: MIT
// The shaders of user/tests/plugin/link_host.m (kSource; the ones of user/tests/metal/mtl4_linking.m that link_host
// links). apply.ll, fs_tab.ll, op_add.ll, op_mul.ll, op_neg.ll, col_red.ll and col_green.ll are their AIR as this Mac's
// Metal compiler made it, read out of the library container by the plugin's own metallib reader
// (link_host --make <NVMTLDriver.bundle> <dir>, on an M1 Pro); stages.json is the stage the reader gave each.
#include <metal_stdlib>
using namespace metal;
[[visible]] float op_add(float x) { return x + 1.0f; }
[[visible]] float op_mul(float x) { return x * 3.0f; }
[[visible]] float op_neg(float x) { return -x; }
kernel void apply(device float *o [[buffer(0)]], visible_function_table<float(float)> t [[buffer(1)]], constant uint &n [[buffer(2)]],
                  uint i [[thread_position_in_grid]]) { o[i] = t[i % n](o[i]); }
[[visible]] float4 col_red(float2 p) { return float4(1, 0, 0, 1); }
[[visible]] float4 col_green(float2 p) { return float4(0, 1, 0, 1); }
struct V { float4 p [[position]]; };
fragment float4 fs_tab(V in [[stage_in]], visible_function_table<float4(float2)> t [[buffer(0)]], constant uint &slot [[buffer(1)]]) {
    return t[slot](in.p.xy); }
