// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// StaggeredGridMark.qml: the 3 x 3 staggered grid loader drawn on one quad. A tile grows and
// lights up as the wave passes its diagonal, top left to bottom right.
//
// The compiled shader staggered_grid_mark.frag.qsb sits next to this file and is committed, so
// QML loaded from the module and from the source tree (QML tests) finds it. After a change,
// regenerate it with the qsb tool of the Qt kit:
//   qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o staggered_grid_mark.frag.qsb staggered_grid_mark.frag

#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    // Wave clock, 0 to 1 over one pass.
    float phase;
    // 1 runs the wave; 0 draws every tile at restLevel.
    float animated;
    // Wave level of a tile that the wave does not reach.
    float restLevel;
    // A tile at rest scales to 1 - scaleDepth.
    float scaleDepth;
    // Opacity of waveColor over a tile at the crest of the wave.
    float waveStrength;
    // Mark size, gap between tiles, and tile corner radius, in item pixels.
    float markSize;
    float gap;
    float tileRadius;
    vec4  tileColor;
    vec4  waveColor;
};

// Signed distance from p to a rounded box centred at the origin.
float roundedBox(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

void main() {
    const float travel = 0.45;
    const float glow   = 0.4;

    float tile  = max((markSize - 2.0 * gap) / 3.0, 1.0);
    float pitch = tile + gap;
    vec2  p     = qt_TexCoord0 * markSize;
    vec2  cell  = clamp(floor(p / pitch), vec2(0.0), vec2(2.0));
    vec2  local = p - cell * pitch - vec2(tile * 0.5);

    float level = restLevel;
    if (animated > 0.5) {
        float start = (cell.x + cell.y) / 4.0 * travel;
        float t     = fract(phase - start + 1.0);
        if (t < glow) {
            float crest = sin(3.14159265 * t / glow);
            level       = restLevel + (1.0 - restLevel) * crest * crest;
        }
    }

    float scale    = 1.0 - scaleDepth * (1.0 - level);
    float halfTile = tile * 0.5 * scale;
    float radius   = min(tileRadius * scale, halfTile);
    float coverage = clamp(0.5 - roundedBox(local, vec2(halfTile), radius), 0.0, 1.0);

    vec3  rgb   = mix(tileColor.rgb, waveColor.rgb, waveStrength * level);
    float alpha = coverage * tileColor.a * qt_Opacity;
    fragColor   = vec4(rgb * alpha, alpha);
}
