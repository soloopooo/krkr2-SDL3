#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
    float gamma;
    float brightness;
    float contrast;
    float pad2;
};
void main() {
    vec2 uv = vUV * uvScale + uvOffset;
    vec4 c = texture(uTex, uv);
    float g = 1.0 / max(gamma, 0.001);
    vec3 adjusted = pow(c.rgb, vec3(g));
    adjusted = (adjusted - 0.5) * contrast + 0.5 + brightness;
    FragColor = vec4(adjusted, c.a * opacity);
}
