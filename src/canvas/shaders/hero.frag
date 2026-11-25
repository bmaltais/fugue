#version 440
// The header of a Drift card (docs/canvas-look.md 20.5): the picture or blend with its top corners rounded and its lower part faded to
// transparent, so it seems to flow behind the panel. Opaque down to `fadeFrom` (px from the top), clear at `fadeTo`.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
    float fadeFrom;
    float fadeTo;
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 pos = qt_TexCoord0 * size;
    vec2 p = pos - size * 0.5;
    vec2 q = abs(p) - (size * 0.5 - vec2(radius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float fade = 1.0 - clamp((pos.y - fadeFrom) / max(fadeTo - fadeFrom, 1.0), 0.0, 1.0);
    fragColor = texture(source, qt_TexCoord0) * clamp(0.5 - d, 0.0, 1.0) * fade * qt_Opacity;
}
