#version 450
layout(location = 0) out vec4 FragColor;
layout(set = 3, binding = 0) uniform FillColor {
    vec4 color;
} uC;
void main() {
    FragColor = vec4(0.0, 0.0, 0.0, uC.color.a);
}
