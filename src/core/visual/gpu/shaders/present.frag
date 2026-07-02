#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uTex;
void main() {
    FragColor = vec4(texture(uTex, vUV).rgb, 1.0);
}
