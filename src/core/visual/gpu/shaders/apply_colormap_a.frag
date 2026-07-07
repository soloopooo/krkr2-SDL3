#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
} uP;
layout(set = 3, binding = 1) uniform ColorParams {
    vec4 text_color;
} uC;
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    float mask = texture(uTex, uv).r;
    float a = mask * uP.opacity;
    FragColor = vec4(uC.text_color.rgb * a, a);
}
