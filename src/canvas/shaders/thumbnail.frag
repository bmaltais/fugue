#version 440
// A thumbnail with rounded corners (docs/canvas-look.md 4.6): the image, its alpha cut by a
// rounded rectangle the size of the item. Used only when the corner is above 0.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 p = qt_TexCoord0 * size - size * 0.5;
    vec2 q = abs(p) - (size * 0.5 - vec2(radius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    fragColor = texture(source, qt_TexCoord0) * clamp(0.5 - d, 0.0, 1.0) * qt_Opacity;
}
