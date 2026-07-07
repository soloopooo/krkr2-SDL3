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
layout(set = 3, binding = 1) uniform ColorParams {
    vec4 text_color;
};
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    vec4 src = texture(uSrc, uv);
    vec4 dst = texture(uDst, uv);
    // ConstColorAlphaBlend_d: constant-opacity dest-alpha compositing with color
    // OGL: d.a = opacity + d.a - opacity*d.a; d.rgb = mix(d.rgb, color.rgb, opacity/(d.a+eps))
    float opa = uP.opacity;
    float newAlpha = opa + dst.a - opa * dst.a;
    vec3 result = mix(dst.rgb, text_color.rgb, opa / max(newAlpha, 0.0001));
    FragColor = vec4(result, newAlpha);
}
