#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
    float phase;
    float vague;
    float pad1;
    float pad2;
};
void main() {
    vec2 uv = vUV * uvScale + uvOffset;
    vec4 s1 = texture(tex0, uv);
    vec4 s2 = texture(tex1, uv);
    vec4 rule = texture(tex2, uv);
    float opa = clamp((rule.r - phase) / (vague + 0.0001), 0.0, 1.0);
    s1.rgb = mix(s2.rgb, s1.rgb, opa);
    s1.a = mix(s2.a, s1.a, opa) * opacity;
    FragColor = s1;
}
