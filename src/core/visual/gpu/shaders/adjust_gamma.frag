#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
    vec4 u_gamma;
    vec4 u_floor;
    vec4 u_amp;
};
void main() {
    vec2 uv = vUV * uvScale + uvOffset;
    vec4 s = texture(uTex, uv);
    s.rgb = clamp(pow(s.rgb, u_gamma.xyz) * u_amp.xyz + u_floor.xyz, 0.0, 1.0);
    s.a *= opacity;
    FragColor = s;
}
