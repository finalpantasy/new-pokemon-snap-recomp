#version 450
// P8 (lighting plan): GTAO replacement for the game's SSAOHorizonBasedAOPS fragment program (SNAP_SSAO_GTAO=1). Same inputs and output as the original:
// HBAO constant buffers (projection inverse, resolution, numDirections, numSamples, radius, strength, farClip, fadeLength), the depth texture (reversed
// depth, 1920x1080 target), output = AO factor in rgb (1 = open), alpha 1. The game's own blur and composite follow unchanged.
// Method: ground truth AO (Jimenez et al. 2016) in view space: per pixel a few slices, two horizon searches per slice, the analytic cosine-weighted
// visibility integral, a falloff window so far occluders fade out, interleaved-gradient noise per pixel (the game's bilateral blur removes it).
// Bindings follow the translator's convention (cbuffer slot s -> set 0 binding s + 17, bound texture index i -> set 2 binding 32 + i); the real
// numbers for the replaced program are patched in at load (nvn_gtao.cpp), these are the defaults.
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 20, std140) uniform ConstantBuffer {
  mat4 projection;
  mat4 projectionInverse;
  vec2 resolution;
  vec2 resolutionInverse;
  float nearZ;
  float farZ;
};
layout(set = 0, binding = 21, std140) uniform HBAOConstantBuffer {
  int numDirections;
  int numSamples;
  float focalLength;
  float radius;
  float radius2;
  float radius2NegativeInverse;
  float maxRadiusPixels;
  float angleBias;
  float tanAngleBias;
  float strength;
  float farClip;
  float fadeLength;
};
layout(set = 2, binding = 32) uniform sampler2D depthTex;

const float PI = 3.14159265;
const float HALF_PI = 1.57079633;

vec3 viewPos(vec2 uv, out float d) {
  d = textureLod(depthTex, uv, 0.0).x;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
  vec4 p = projectionInverse * vec4(ndc, d, 1.0);
  return p.xyz / p.w;
}
vec3 viewPos(vec2 uv) { float d; return viewPos(uv, d); }

float fastAcos(float x) {
  float ax = abs(x);
  float r = -0.0187293 * ax + 0.0742610;
  r = r * ax - 0.2121144;
  r = r * ax + 1.5707288;
  r *= sqrt(1.0 - ax);
  return x >= 0.0 ? r : PI - r;
}

void main() {
  float d0;
  vec3 pc = viewPos(vUv, d0);
  if (!(d0 > 1.0e-7) || any(isnan(pc)) || any(isinf(pc))) { outColor = vec4(1.0, 1.0, 1.0, 1.0); return; }   // sky / invalid
  const vec2 px = resolutionInverse;
  // view-space normal from the depth neighbourhood (the side with the smaller depth change on each axis)
  vec3 pl = viewPos(vUv - vec2(px.x, 0.0)), pr = viewPos(vUv + vec2(px.x, 0.0));
  vec3 pu = viewPos(vUv - vec2(0.0, px.y)), pd = viewPos(vUv + vec2(0.0, px.y));
  vec3 dx = abs(pc.z - pl.z) < abs(pr.z - pc.z) ? pc - pl : pr - pc;
  vec3 dy = abs(pc.z - pu.z) < abs(pd.z - pc.z) ? pc - pu : pd - pc;   // dy goes with uv.y (view y decreases)
  vec3 n = cross(dx, dy);
  float nl = length(n);
  if (!(nl > 1.0e-12)) { outColor = vec4(1.0, 1.0, 1.0, 1.0); return; }
  n /= nl;
  vec3 viewVec = normalize(-pc);
  if (dot(n, viewVec) < 0.0) n = -n;

  const float zAbs = abs(pc.z);
  float fade = clamp((farClip - zAbs) / max(fadeLength, 1.0e-4), 0.0, 1.0);
  if (fade <= 0.0) { outColor = vec4(1.0, 1.0, 1.0, 1.0); return; }

  // screen-space radius in pixels (half the height times cot(fov/2) over depth), bounded like the original
  float rpx = radius * projection[1][1] * 0.5 * resolution.y / zAbs;
  rpx = min(rpx, max(maxRadiusPixels, 4.0));
  if (rpx < 1.5) { outColor = vec4(1.0, 1.0, 1.0, 1.0); return; }

  const int slices = clamp(numDirections / 2, 2, 8);
  const int steps = clamp(numSamples / 2, 6, 24);
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float noise2 = fract(noise * 7.0 + 0.37);
  const float falloffFrom = radius * 0.6;
  const float falloffMul = -1.0 / max(radius - falloffFrom, 1.0e-4);
  const float falloffAdd = radius / max(radius - falloffFrom, 1.0e-4);

  float vis = 0.0;
  for (int s = 0; s < slices; s++) {
    float phi = (float(s) + noise) / float(slices) * PI;
    vec2 dir2 = vec2(cos(phi), sin(phi));
    vec3 dirV = vec3(dir2, 0.0);
    vec2 uvStep = vec2(dir2.x, -dir2.y) * px;
    vec3 orthoDir = dirV - dot(dirV, viewVec) * viewVec;
    vec3 axis = cross(dirV, viewVec);
    vec3 projN = n - axis * dot(n, axis);
    float projLen = length(projN);
    if (!(projLen > 1.0e-5)) { vis += 1.0; continue; }
    float sgn = sign(dot(orthoDir, projN));
    float cosN = clamp(dot(projN, viewVec) / projLen, -1.0, 1.0);
    float nAng = sgn * fastAcos(cosN);
    float lowCos0 = cos(nAng + HALF_PI), lowCos1 = cos(nAng - HALF_PI);
    float hCos0 = lowCos0, hCos1 = lowCos1;
    for (int t = 0; t < steps; t++) {
      float st = (float(t) + noise2) / float(steps);
      st = st * st;                                   // denser near the pixel
      float off = max(rpx * st, 1.0 + float(t));
      vec3 sp0 = viewPos(vUv + uvStep * off);
      vec3 sp1 = viewPos(vUv - uvStep * off);
      vec3 dl0 = sp0 - pc, dl1 = sp1 - pc;
      float dist0 = length(dl0), dist1 = length(dl1);
      float c0 = dot(dl0, viewVec) / max(dist0, 1.0e-5), c1 = dot(dl1, viewVec) / max(dist1, 1.0e-5);
      float w0 = clamp(dist0 * falloffMul + falloffAdd, 0.0, 1.0), w1 = clamp(dist1 * falloffMul + falloffAdd, 0.0, 1.0);
      c0 = mix(lowCos0, c0, w0); c1 = mix(lowCos1, c1, w1);
      hCos0 = max(hCos0, c0); hCos1 = max(hCos1, c1);
    }
    float h0 = -fastAcos(hCos1), h1 = fastAcos(hCos0);
    h0 = nAng + clamp(h0 - nAng, -HALF_PI, HALF_PI);
    h1 = nAng + clamp(h1 - nAng, -HALF_PI, HALF_PI);
    float sinN = sin(nAng);
    float iarc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - nAng)) * 0.25;
    float iarc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - nAng)) * 0.25;
    vis += projLen * (iarc0 + iarc1);
  }
  vis = clamp(vis / float(slices), 0.0, 1.0);
  // multi-bounce approximation for a mid-grey albedo (Jimenez 2016): keeps corners from going black
  const float alb = 0.5;
  float a = 2.0404 * alb - 0.3324, b = -4.7951 * alb + 0.6417, c = 2.7552 * alb + 0.6903;
  vis = max(vis, ((vis * a + b) * vis + c) * vis);
  float ao = clamp(1.0 - (1.0 - vis) * strength * fade, 0.0, 1.0);
  outColor = vec4(ao, ao, ao, 1.0);
}
