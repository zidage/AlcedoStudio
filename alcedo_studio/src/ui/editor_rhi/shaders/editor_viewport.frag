#version 440

// ViewportCompositeUniforms (viewport_composite_pass.hpp).
layout(std140, binding = 0) uniform ViewerUniforms {
  vec4 scaleZoom;
  vec4 panMode;
  vec4 detailRoi;
  vec4 detailFlags;
  // xyz: photo UV -> coverage UV, u row; w: 1 when coverage is composited.
  vec4 coverageU;
  // xyz: photo UV -> coverage UV, v row.
  vec4 coverageV;
  // rgb: display-encoded tint; a: overlay opacity.
  vec4 coverageTint;
} ubo;

layout(binding = 1) uniform sampler2D baseTexture;
layout(binding = 2) uniform sampler2D detailTexture;
// Effective Mask coverage in red (SM8.5 coverage channel).
layout(binding = 3) uniform sampler2D coverageTexture;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
  vec4 color = texture(baseTexture, vUv);
  if (ubo.detailFlags.x > 0.5) {
    vec2 roiMin = ubo.detailRoi.xy;
    vec2 roiSize = ubo.detailRoi.zw;
    vec2 roiMax = roiMin + roiSize;
    if (roiSize.x > 0.0001 && roiSize.y > 0.0001 &&
        vUv.x >= roiMin.x && vUv.x <= roiMax.x &&
        vUv.y >= roiMin.y && vUv.y <= roiMax.y) {
      color = texture(detailTexture, (vUv - roiMin) / roiSize);
    }
  }
  // The one coverage composite (SM8 section 5.3), after the base/detail photo selection. The
  // coverage frame has no texels outside its photo frame: there the coverage is 0.
  if (ubo.coverageU.w > 0.5) {
    vec3 photoUv = vec3(vUv, 1.0);
    vec2 coverageUv = vec2(dot(ubo.coverageU.xyz, photoUv), dot(ubo.coverageV.xyz, photoUv));
    float coverage = 0.0;
    if (coverageUv.x >= 0.0 && coverageUv.x <= 1.0 && coverageUv.y >= 0.0 &&
        coverageUv.y <= 1.0) {
      coverage = texture(coverageTexture, coverageUv).r;
    }
    float a = clamp(coverage, 0.0, 1.0) * ubo.coverageTint.a;
    // An uncovered pixel keeps its photo value, including negative and above-one values.
    if (a > 0.0) {
      color.rgb = color.rgb * (1.0 - a) + ubo.coverageTint.rgb * a;
    }
  }
  fragColor = color;
}
