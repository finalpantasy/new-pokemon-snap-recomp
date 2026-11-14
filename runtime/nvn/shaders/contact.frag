#version 450
// SPDX-License-Identifier: MIT
// Screen-space contact shadows (nvn_contact.cpp), run at the Fx pre-tonemap slot on the linear HDR scene colour.
// One full-screen triangle per frame, blended as colour = colour * out (RGB only). Per pixel: world position and a normal from the scene depth
// (reversed infinite Z, viewport jitter undone like upscale_mv.comp), then a short march toward the sun in CLIP space (the world ray is linear in
// clip space, so only one clip-space direction is needed), comparing the ray point's view distance with the depth buffer's.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 dclip;   // VP * (L, 0): clip-space change per world unit toward the light
    vec4 light;   // xyz: unit world direction toward the light, w: strength
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: ray length (world units), y: thickness, z: fade-out distance (view units), w: frame counter
} pc;
layout(location = 0) out vec4 outMul;

vec3 ndcOf(vec2 pix, float d) {
    vec2 win = pix + 0.5 - pc.size.zw;
    return vec3(win / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d);
}
vec3 posAt(ivec2 p, out float w) {
    p = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) { w = 1e9; return vec3(0.0); }
    vec4 h = pc.ivp * vec4(ndcOf(vec2(p), d), 1.0);
    w = 1.0 / h.w;
    return h.xyz / h.w;
}

void main() {
    outMul = vec4(1.0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) return;   // sky / infinity (reversed Z)
    float wC, wR, wL, wU, wD;
    vec3 pC = posAt(p, wC);
    int dm = int(pc.light.w * 0.1);   // debug view = tens digit of light.w (SNAP_CONTACT_DEBUG): 2 tints, 3 view distance, 4 normal, 5 sun facing, 6 depth, 7 the shadow factor itself (replace blend)
    float strength = pc.light.w - 10.0 * float(dm);
    if (dm == 3) { outMul = vec4(vec3(fract(wC * 0.1)), 1.0); return; }
    if (dm == 6) { outMul = vec4(vec3(sqrt(d)), 1.0); return; }
    if (wC > pc.q.z) return;
    vec3 pR = posAt(p + ivec2(1, 0), wR), pL = posAt(p - ivec2(1, 0), wL);
    vec3 pD = posAt(p + ivec2(0, 1), wD), pU = posAt(p - ivec2(0, 1), wU);
    vec3 dx = abs(wR - wC) < abs(wC - wL) ? pR - pC : pC - pL;
    vec3 dy = abs(wD - wC) < abs(wC - wU) ? pD - pC : pC - pU;
    vec3 n = cross(dx, dy);
    float nl = length(n);
    if (!(nl > 1e-12)) return;
    n /= nl;
    vec4 hf = pc.ivp * vec4(ndcOf(vec2(p), d * 0.5), 1.0);   // farther along the pixel's ray: its direction faces away from the camera
    if (dot(n, hf.xyz / hf.w - pC) > 0.0) n = -n;
    float ndl = dot(n, pc.light.xyz);
    float facing = smoothstep(0.02, 0.3, ndl);   // surfaces turned from the sun are already dark
    bool dbg = dm == 2;
    if (dm == 4) { outMul = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (dm == 5) { outMul = vec4(vec3(facing), 1.0); return; }
    if (facing <= 0.0) { if (dbg) outMul = vec4(0.8, 1.0, 0.8, 1.0); return; }

    const int N = 16;
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy + pc.q.w * vec2(5.588238, 2.173), vec2(0.06711056, 0.00583715))));
    float bias = 0.012 + 0.004 * wC, thick = pc.q.y + 0.01 * wC;
    vec4 C0 = vec4(ndcOf(vec2(p), d) * wC, wC);
    float occ = 0.0, firstT = 0.0; int hits = 0;   // two hits needed: a single-step hit is a thin blade or a depth edge
    for (int i = 0; i < N; i++) {
        float t = (float(i) + jit) / float(N);
        vec4 C = C0 + pc.dclip * (pc.q.x * t);
        if (C.w < 1e-3) break;
        vec2 win = (C.xy / C.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
        ivec2 tp = ivec2(floor(win + pc.size.zw));
        if (any(lessThan(tp, ivec2(0))) || any(greaterThanEqual(tp, ivec2(pc.size.xy)))) break;
        float sd = texelFetch(depthTex, tp, 0).r;
        if (sd <= 0.0) continue;
        float sw = 1.0 / (pc.ivp * vec4(ndcOf(vec2(tp), sd), 1.0)).w;
        float diff = C.w - sw;   // > 0: the ray point is behind the surface the depth buffer shows there
        if (diff > bias && diff < thick) { if (hits++ == 0) firstT = t; if (hits >= 2) { occ = 1.0 - 0.75 * firstT; break; } }
    }
    float fade = 1.0 - smoothstep(0.5 * pc.q.z, pc.q.z, wC);
    if (dbg) { outMul = vec4(1.0, 1.0 - occ * fade, 1.0 - occ * fade, 1.0); return; }
    float s = strength * occ * facing * fade;
    outMul = vec4(vec3(1.0 - s), 1.0);
}
