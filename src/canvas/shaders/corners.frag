#version 440
// The corners a rounded picture frame cuts away (docs/canvas-look.md 20.14): `cornerColor` outside a rounded rectangle the size of the
// item, clear inside, so a viewer that cannot be clipped to a radius (a Qt item clip is a rectangle) still shows the card's corner.
// It has no texture; the colour is the card's own fill behind it.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
    vec4 cornerColor;
};

void main() {
    vec2 p = qt_TexCoord0 * size - size * 0.5;
    vec2 q = abs(p) - (size * 0.5 - vec2(radius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    fragColor = vec4(cornerColor.rgb, 1.0) * cornerColor.a * clamp(0.5 + d, 0.0, 1.0) * qt_Opacity;
}
