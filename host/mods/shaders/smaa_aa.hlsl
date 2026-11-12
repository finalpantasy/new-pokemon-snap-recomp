// SMAA 1x passes for runtime/nvn/nvn_aa.cpp. Wrapper around the reference SMAA.hlsl (Jimenez et al., MIT; see THIRD_PARTY_NOTICES.md),
// which this build reads from build/smaa_ref/SMAA.hlsl (fetched by host/mods/shaders/build_aa_shaders.ps1; not stored in the repo).
// Descriptor set 0: 0 colorTex, 1 second input (edges or blend weights), 2 areaTex, 3 searchTex, 4 linear sampler, 5 point sampler.
// Push constants: rt = (1/w, 1/h, w, h).
#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
[[vk::push_constant]] struct { float4 rt; } g_pc;
#define SMAA_RT_METRICS g_pc.rt
[[vk::binding(4, 0)]] SamplerState LinearSampler;
[[vk::binding(5, 0)]] SamplerState PointSampler;
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#include "SMAA.hlsl"

[[vk::binding(0, 0)]] Texture2D colorTex;
[[vk::binding(1, 0)]] Texture2D secondTex;
[[vk::binding(2, 0)]] Texture2D areaTex;
[[vk::binding(3, 0)]] Texture2D searchTex;

struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; float2 pix : TEXCOORD4; };

VOut vs_common(uint id) {
  VOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
  o.o0 = o.o1 = o.o2 = 0; o.pix = 0;
  return o;
}
VOut vs_edge(uint id : SV_VertexID) { VOut o = vs_common(id); float4 off[3]; SMAAEdgeDetectionVS(o.uv, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o; }
VOut vs_weights(uint id : SV_VertexID) { VOut o = vs_common(id); float4 off[3]; SMAABlendingWeightCalculationVS(o.uv, o.pix, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o; }
VOut vs_blend(uint id : SV_VertexID) { VOut o = vs_common(id); float4 off; SMAANeighborhoodBlendingVS(o.uv, off); o.o0 = off; return o; }

float2 ps_edge(VOut i) : SV_Target { float4 off[3] = { i.o0, i.o1, i.o2 }; return SMAALumaEdgeDetectionPS(i.uv, off, colorTex); }
float4 ps_weights(VOut i) : SV_Target { float4 off[3] = { i.o0, i.o1, i.o2 }; return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, secondTex, areaTex, searchTex, float4(0, 0, 0, 0)); }
float4 ps_blend(VOut i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o0, colorTex, secondTex); }
