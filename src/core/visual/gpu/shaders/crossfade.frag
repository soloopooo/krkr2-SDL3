#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;   // for tex0
    vec2 uvScale;    // for tex0
    float opacity;
    float pad;
    vec2 uvOffset1;  // for tex1 (Bug #23 fix: separate UV for second texture)
    vec2 uvScale1;   // for tex1
} uP;
void main() {
    vec2 uv0 = vUV * uP.uvScale + uP.uvOffset;
    vec2 uv1 = vUV * uP.uvScale1 + uP.uvOffset1;
    vec4 c1 = texture(tex0, uv0);
    vec4 c2 = texture(tex1, uv1);
    FragColor = mix(c1, c2, uP.opacity);
}
