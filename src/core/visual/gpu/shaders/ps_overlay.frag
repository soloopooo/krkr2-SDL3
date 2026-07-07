#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 2, binding = 1) uniform sampler2D uDst;
layout(set = 3, binding = 0) uniform FragParams {
    vec2 uvOffset;
    vec2 uvScale;
    float opacity;
    float pad;
} uP;
void main() {
    vec2 uv = vUV * uP.uvScale + uP.uvOffset;
    vec4 src = texture(uSrc, uv);
    vec4 dst = texture(uDst, uv);
    float lum = dot(dst.rgb, vec3(0.299, 0.587, 0.114));
    vec3 result = dst.rgb;
    bvec3 dark = lessThan(dst.rgb, vec3(0.5));
    result = mix(result, 2.0 * src.rgb * dst.rgb, vec3(dark));
    result = mix(result, 1.0 - 2.0 * (1.0 - src.rgb) * (1.0 - dst.rgb), 1.0 - vec3(dark));
    FragColor = vec4(mix(dst.rgb, result, src.a * uP.opacity), dst.a);
}
