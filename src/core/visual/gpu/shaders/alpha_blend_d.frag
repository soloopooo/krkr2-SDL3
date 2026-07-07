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
    float Sa = src.a * uP.opacity;
    float Da = dst.a;
    // TVPOpacityOnOpacityTable approximation:
    // eff = Sa / (Sa + Da * (1 - Sa))
    float eff = Sa / max(Sa + Da * (1.0 - Sa), 0.001);
    // Porter-Duff source-over alpha
    float newAlpha = Sa + Da * (1.0 - Sa);
    vec3 result_rgb = mix(dst.rgb, src.rgb, eff);
    FragColor = vec4(result_rgb, newAlpha);
}
