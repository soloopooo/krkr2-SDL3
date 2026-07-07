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
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    vec4 c = texture(uTex, uv);
    c.rgb = vec3(0.0); // RGB will come from dst via blend state
    FragColor = c;
}
