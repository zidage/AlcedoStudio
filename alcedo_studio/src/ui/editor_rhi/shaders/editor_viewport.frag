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
  // xyz: photo UV -> detail coverage UV, u row; w: 1 when a detail coverage frame is composited.
  vec4 detailCoverageU;
  // xyz: photo UV -> detail coverage UV, v row.
  vec4 detailCoverageV;
} ubo;

layout(binding = 1) uniform sampler2D baseTexture;
layout(binding = 2) uniform sampler2D detailTexture;
// Effective Mask coverage in red (SM8.5 coverage channel).
layout(binding = 3) uniform sampler2D coverageTexture;
// Coverage of the detail ROI render (Mask ranges D10), in red.
layout(binding = 4) uniform sampler2D detailCoverageTexture;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

bool insideFrame(vec2 uv) {
  return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

void main() {
  vec4 color = texture(baseTexture, vUv);
  bool inDetail = false;
  if (ubo.detailFlags.x > 0.5) {
    vec2 roiMin = ubo.detailRoi.xy;
    vec2 roiSize = ubo.detailRoi.zw;
    vec2 roiMax = roiMin + roiSize;
    if (roiSize.x > 0.0001 && roiSize.y > 0.0001 &&
        vUv.x >= roiMin.x && vUv.x <= roiMax.x &&
        vUv.y >= roiMin.y && vUv.y <= roiMax.y) {
      color = texture(detailTexture, (vUv - roiMin) / roiSize);
      inDetail = true;
    }
  }
  // The one coverage composite (SM8 section 5.3), after the base/detail photo selection. Inside
  // the detail ROI the detail coverage frame is sampled where it has texels; elsewhere the base
  // coverage frame. A coverage frame has no texels outside its photo frame: there it is 0.
  if (ubo.coverageU.w > 0.5 || ubo.detailCoverageU.w > 0.5) {
    vec3 photoUv = vec3(vUv, 1.0);
    float coverage = 0.0;
    bool sampled = false;
    if (inDetail && ubo.detailCoverageU.w > 0.5) {
      vec2 detailUv = vec2(dot(ubo.detailCoverageU.xyz, photoUv),
                           dot(ubo.detailCoverageV.xyz, photoUv));
      if (insideFrame(detailUv)) {
        coverage = texture(detailCoverageTexture, detailUv).r;
        sampled = true;
      }
    }
    if (!sampled && ubo.coverageU.w > 0.5) {
      vec2 coverageUv = vec2(dot(ubo.coverageU.xyz, photoUv), dot(ubo.coverageV.xyz, photoUv));
      if (insideFrame(coverageUv)) {
        coverage = texture(coverageTexture, coverageUv).r;
      }
    }
    float a = clamp(coverage, 0.0, 1.0) * ubo.coverageTint.a;
    // An uncovered pixel keeps its photo value, including negative and above-one values.
    if (a > 0.0) {
      color.rgb = color.rgb * (1.0 - a) + ubo.coverageTint.rgb * a;
    }
  }
  fragColor = color;
}
