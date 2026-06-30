#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uTex;
void main() {
    vec4 c = texture(uTex, vUV);
    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    FragColor = vec4(vec3(gray), c.a);
}
