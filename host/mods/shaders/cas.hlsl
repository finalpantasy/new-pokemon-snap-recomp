// Contrast adaptive sharpening after the AA pass. Follows the published AMD FidelityFX CAS algorithm (MIT, Copyright (c) 2019 Advanced Micro
// Devices, Inc.; see THIRD_PARTY_NOTICES.md): per-pixel sharpening weight from the local min/max, 5-tap cross filter.
// Set 0 binding 0 = input color, binding 4 = point sampler. Push constants: x = 1/w, y = 1/h, z = sharpness 0..1.
[[vk::push_constant]] struct { float4 p; } g_pc;
[[vk::binding(0, 0)]] Texture2D colorTex;
[[vk::binding(4, 0)]] SamplerState PointSampler;
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vs_cas(uint id : SV_VertexID) { VOut o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0); return o; }
float3 ld(float2 uv, float2 d) { return colorTex.SampleLevel(PointSampler, uv + d * g_pc.p.xy, 0).rgb; }
float4 ps_cas(VOut i) : SV_Target {
  float3 a = ld(i.uv, float2(-1, -1)), b = ld(i.uv, float2(0, -1)), c = ld(i.uv, float2(1, -1));
  float3 d = ld(i.uv, float2(-1, 0)), e = ld(i.uv, float2(0, 0)), f = ld(i.uv, float2(1, 0));
  float3 g = ld(i.uv, float2(-1, 1)), h = ld(i.uv, float2(0, 1)), k = ld(i.uv, float2(1, 1));
  float3 mn = min(min(min(d, e), min(f, b)), h);
  float3 mn2 = min(min(min(mn, a), min(c, g)), k);
  mn += mn2;
  float3 mx = max(max(max(d, e), max(f, b)), h);
  float3 mx2 = max(max(max(mx, a), max(c, g)), k);
  mx += mx2;
  float3 amp = saturate(min(mn, 2.0 - mx) * rcp(max(mx, 1e-5)));
  amp = sqrt(amp);
  float peak = -rcp(lerp(8.0, 5.0, saturate(g_pc.p.z)));
  float3 w = amp * peak;
  float3 rcpW = rcp(1.0 + 4.0 * w);
  float3 outc = saturate((b * w + d * w + f * w + h * w + e) * rcpW);
  return float4(outc, 1.0);
}
