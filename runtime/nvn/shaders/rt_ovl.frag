#version 460
#extension GL_EXT_ray_query : require
// SNAP_RT_DEBUG overlay: one primary ray per pixel into the TLAS built from the captured geometry, compared with the scene depth.
//   mode 1: green = ray hit matches the depth, red = hit closer than the depth, blue = hit farther, magenta = depth but no hit, orange = hit on a sky pixel
//   mode 2: hit-distance heat map; mode 3: statistics only (nothing drawn)
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2, std430) buffer Stats { uint c[8]; } stats;
layout(push_constant) uniform PC { mat4 invVP; vec4 eye; vec4 p0; vec4 size; } pc;   // p0: mode, flipY, tolRel, tolAbs
layout(location = 0) out vec4 outColor;
void main() {
  vec2 uv = gl_FragCoord.xy / pc.size.xy;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, pc.p0.y > 0.5 ? 1.0 - uv.y * 2.0 : uv.y * 2.0 - 1.0);
  float d = texture(depthTex, uv).r;
  vec4 pn = pc.invVP * vec4(ndc, 1.0, 1.0);
  vec3 dir = normalize(pn.xyz / pn.w - pc.eye.xyz);
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, pc.eye.xyz, 0.005, dir, 100000.0);
  while (rayQueryProceedEXT(rq)) {}
  bool hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
  float t = hit ? rayQueryGetIntersectionTEXT(rq, true) : 0.0;
  bool haveD = d > 1e-7;
  float td = 0.0;
  if (haveD) { vec4 pd = pc.invVP * vec4(ndc, d, 1.0); td = dot(pd.xyz / pd.w - pc.eye.xyz, dir); }
  int cls;   // 1 match, 2 rt in front, 3 rt behind, 4 depth but no hit, 5 both none, 6 hit on a sky pixel
  if (haveD && hit) { float tol = max(pc.p0.w, pc.p0.z * td); cls = abs(t - td) <= tol ? 1 : (t < td ? 2 : 3); }
  else if (haveD) cls = 4; else if (hit) cls = 6; else cls = 5;
  ivec2 ip = ivec2(gl_FragCoord.xy);
  if (((ip.x | ip.y) & 3) == 0) { atomicAdd(stats.c[0], 1u); atomicAdd(stats.c[cls], 1u); }
  int mode = int(pc.p0.x + 0.5);
  vec4 col = vec4(0.0);
  if (mode == 1) {
    if (cls == 1) col = vec4(0.1, 0.9, 0.2, 0.45);
    else if (cls == 2) col = vec4(1.0, 0.1, 0.1, 0.7);
    else if (cls == 3) col = vec4(0.1, 0.3, 1.0, 0.7);
    else if (cls == 4) col = vec4(1.0, 0.1, 1.0, 0.7);
    else if (cls == 6) col = vec4(1.0, 0.6, 0.0, 0.7);
  } else if (mode == 2 && hit) {
    col = vec4(0.5 + 0.5 * cos(6.2831853 * (log2(1.0 + t) * 0.15 + vec3(0.0, 0.33, 0.67))), 0.65);
  }
  outColor = col;
}
