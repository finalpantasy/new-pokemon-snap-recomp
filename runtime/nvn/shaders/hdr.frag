#version 450
// HDR output pass (SNAP_HDR=hdr10|scrgb): the game's finished SDR frame -> PQ (ST 2084, Rec.2020, A2B10G10R10) or scRGB (linear Rec.709, 1.0 = 80 nits, RGBA16F).
layout(set=0, binding=0) uniform sampler2D tex;
layout(set=0, binding=1) uniform sampler2D lay;   // overlay layer (RGBA8 UNORM, sRGB-encoded colors premultiplied by alpha): the F1 menu, toasts, shader screen
layout(push_constant) uniform P {
  vec4 rect;   // source uv offset (xy) and scale (zw)
  vec4 p;      // paper white nits, peak nits, source already linear (sRGB view) 1/0, highlight expansion 0..1
  vec4 q;      // x: output 0 = PQ, 1 = scRGB
} pc;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;

vec3 srgb_eotf(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec3 pq_oetf(vec3 nits) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
  return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}
// highlight roll-off: linear up to the knee, then an exponential shoulder that approaches (never exceeds) the display peak
float rolloff(float n, float peak) {
  float knee = 0.75 * peak;
  if (n <= knee) return n;
  return knee + (peak - knee) * (1.0 - exp(-(n - knee) / (peak - knee)));
}
void main() {
  vec3 c = texture(tex, pc.rect.xy + uv * pc.rect.zw).rgb;
  vec3 lin = pc.p.z > 0.5 ? c : srgb_eotf(clamp(c, 0.0, 1.0));
  // inverse tone map (SNAP_HDR_EXPAND): lift only the bright end, keep hue (one gain for all channels)
  float m = max(lin.r, max(lin.g, lin.b));
  float w = clamp((m - 0.75) / 0.25, 0.0, 1.0);
  float ratio = max(pc.p.y / pc.p.x, 1.0);
  float gain = 1.0 + (ratio - 1.0) * 0.25 * pc.p.w * w * w;
  vec3 nits = lin * gain * pc.p.x;
  float mn = max(nits.r, max(nits.g, nits.b));
  if (mn > 0.0) nits *= rolloff(mn, pc.p.y) / mn;   // roll off on the brightest channel so hue is kept
  // overlay: composed at paper white in linear light (never inverse tone mapped or rolled off)
  vec4 L = texture(lay, uv);
  if (L.a > 0.0) nits = srgb_eotf(clamp(L.rgb / L.a, 0.0, 1.0)) * (L.a * pc.p.x) + nits * (1.0 - L.a);
  if (pc.q.x > 0.5) { o = vec4(nits / 80.0, 1.0); return; }
  const mat3 to2020 = mat3(0.6274039, 0.0690973, 0.0163914,
                           0.3292830, 0.9195404, 0.0880133,
                           0.0433131, 0.0113623, 0.8955953);
  o = vec4(pq_oetf(max(to2020 * nits, vec3(0.0))), 1.0);
}
