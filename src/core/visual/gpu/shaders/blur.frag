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
    vec2 ts = 1.0 / textureSize(uTex, 0);
    vec4 r = vec4(0.0);
    float w = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            r += texture(uTex, uv + vec2(x, y) * ts);
            w += 1.0;
        }
    }
    FragColor = (r / w);
    FragColor.a *= uP.opacity;
}
