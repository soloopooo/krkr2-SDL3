#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 3, binding = 0) uniform UnivTransParams {
	float phase;
	float vague;
	float pad0;
	float pad1;
};
void main() {
	vec4 s1 = texture(tex0, vUV);
	vec4 s2 = texture(tex1, vUV);
	vec4 rule = texture(tex2, vUV);
	float opa = clamp((rule.r - phase) / (vague + 0.0001), 0.0, 1.0);
	s1.rgb = mix(s2.rgb, s1.rgb, opa);
	FragColor = s1;
}
