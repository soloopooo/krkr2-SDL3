#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 2, binding = 1) uniform sampler2D uDst;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
} uP;
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    vec4 src = texture(uSrc, uv);
    vec4 dst = texture(uDst, uv);
    // PsHardLight: same as Overlay but step() tests src instead of dst
    vec3 sd = src.rgb * dst.rgb;
    vec3 result = step(0.5, src.rgb) * ((src.rgb + dst.rgb) * 2.0 - sd * 4.0 - 1.0) + sd * 2.0;
    FragColor = vec4(mix(dst.rgb, result, src.a * uP.opacity), dst.a);
}
