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
    // PsExclusion: dst + (src - src*dst*2) * src.a * opacity
    vec3 result = dst.rgb + (src.rgb - (src.rgb * dst.rgb * 2.0)) * src.a * uP.opacity;
    FragColor = vec4(result, dst.a);
}
