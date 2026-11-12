// Sparse-texture residency probes: each function reads sparse_color::resident() (AIR: member 1 of the { T, i8 } the
// texture call returns, nonzero when the texels were not resident), except value_only, which reads only value().
#include <metal_stdlib>
using namespace metal;
struct VO { float4 p [[position]]; float2 uv; float3 d; };
static float4 mark(bool r) { return r ? float4(0) : float4(1, 0, 1, 1); }
fragment float4 sp_sample_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_sample(s, in.uv); return c.value() + mark(c.resident()); }
fragment float4 sp_sample_level_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_sample(s, in.uv, level(2.0)); return c.value() + mark(c.resident()); }
fragment float4 sp_sample_grad_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_sample(s, in.uv, gradient2d(dfdx(in.uv), dfdy(in.uv))); return c.value() + mark(c.resident()); }
fragment float4 sp_sample_minlod_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_sample(s, in.uv, min_lod_clamp(1.0)); return c.value() + mark(c.resident()); }
fragment float4 sp_read_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]]) {
  sparse_color<float4> c = t.sparse_read(uint2(in.p.xy), 1); return c.value() + mark(c.resident()); }
fragment float4 sp_gather_2d(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_gather(s, in.uv); return c.value() + mark(c.resident()); }
fragment half4 sp_sample_2da_half(VO in [[stage_in]], texture2d_array<half> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<half4> c = t.sparse_sample(s, in.uv, 1); return c.value() + half4(mark(c.resident())); }
fragment uint4 sp_read_3d_uint(VO in [[stage_in]], texture3d<uint> t [[texture(0)]]) {
  sparse_color<uint4> c = t.sparse_read(uint3(uint2(in.p.xy), 3), 0); return c.value() + uint4(c.resident() ? 0 : 7); }
fragment float4 sp_sample_cube(VO in [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_sample(s, in.d); return c.value() + mark(c.resident()); }
fragment float4 sp_depth_compare(VO in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float> c = t.sparse_sample_compare(s, in.uv, 0.5); return float4(c.value()) + mark(c.resident()); }
fragment float4 sp_depth_gather_compare(VO in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  sparse_color<float4> c = t.sparse_gather_compare(s, in.uv, 0.5); return c.value() + mark(c.resident()); }
fragment float4 sp_depth_read(VO in [[stage_in]], depth2d<float> t [[texture(0)]]) {
  sparse_color<float> c = t.sparse_read(uint2(in.p.xy), 0); return float4(c.value()) + mark(c.resident()); }
fragment float4 sp_rw_read(VO in [[stage_in]], texture2d<float, access::read_write> t [[texture(0)]]) {
  sparse_color<float4> c = t.sparse_read(uint2(in.p.xy)); return c.value() + mark(c.resident()); }
fragment float4 sp_value_only(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sparse_sample(s, in.uv).value(); }
// refused: a linear pixel-coordinate sampler on an array texture is emulated by filtering four fetches, which have no
// single image instruction whose residency is the call's
fragment float4 sp_pixel_array_refused(VO in [[stage_in]], texture2d_array<float> t [[texture(0)]]) {
  constexpr sampler s(coord::pixel, filter::linear);
  sparse_color<float4> c = t.sparse_sample(s, in.p.xy, 0); return c.value() + mark(c.resident()); }
