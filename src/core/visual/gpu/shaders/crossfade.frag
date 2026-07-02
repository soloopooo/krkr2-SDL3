#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
} uP;
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    vec4 c1 = texture(tex0, uv);
    vec4 c2 = texture(tex1, uv);
    FragColor = mix(c1, c2, uP.opacity);
    FragColor.a = 1.0;
}
