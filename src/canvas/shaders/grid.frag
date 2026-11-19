#version 440
// The canvas's dot grid (docs/canvas-look.md 11): a dot every `spacing` world units, a major one
// every fifth, drawn for the whole view in one pass. Dots keep their size on screen; the minor
// ones fade out as the view zooms out (minorAlpha).
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;       // the item, in px
    vec2 origin;     // where world (0, 0) is, in px
    float zoom;
    float spacing;   // world units between dots
    float minorAlpha;
    float minorRadius;
    float majorRadius;
    vec4 dotColor;
    vec4 majorColor;
};

void main() {
    vec2 p = qt_TexCoord0 * size;
    vec2 cell = (p - origin) / (spacing * zoom);
    vec2 nearest = floor(cell + 0.5);
    float d = length(p - (nearest * spacing * zoom + origin));
    bool major = mod(nearest.x, 5.0) == 0.0 && mod(nearest.y, 5.0) == 0.0;
    float radius = major ? majorRadius : minorRadius;
    float a = clamp(radius + 0.5 - d, 0.0, 1.0) * (major ? 1.0 : minorAlpha);
    vec4 c = major ? majorColor : dotColor;
    fragColor = vec4(c.rgb, 1.0) * c.a * a * qt_Opacity;
}
