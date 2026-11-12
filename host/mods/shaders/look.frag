#version 450
layout(set=0,binding=0) uniform sampler2D tex;
layout(push_constant) uniform P { vec4 gain; vec4 p; } pc;   // gain.rgb; p = saturation, contrast, intensity, srgb flag
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  vec3 c = texture(tex, uv).rgb;
  vec3 g = (pc.p.w > 0.5) ? pow(c, vec3(1.0/2.2)) : c;
  g *= pc.gain.rgb;
  float l = dot(g, vec3(0.2126, 0.7152, 0.0722));
  g = mix(vec3(l), g, pc.p.x);
  g = (g - 0.5) * pc.p.y + 0.5;
  g = clamp(g, 0.0, 1.0);
  if (pc.p.w > 0.5) g = pow(g, vec3(2.2));
  o = vec4(mix(c, g, pc.p.z), 1.0);
}
