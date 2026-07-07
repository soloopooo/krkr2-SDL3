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
    // PsColorDodge: dodge = dst / max(1 - src, dst)
    vec3 s = src.rgb - 0.001;  // avoid div-by-zero
    vec3 result = dst.rgb / max(1.0 - s, dst.rgb);
    FragColor = vec4(mix(dst.rgb, result, src.a * uP.opacity), dst.a);
}
