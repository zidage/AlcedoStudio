#version 440

layout(std140, binding = 0) uniform ViewerUniforms {
  vec4 scaleZoom;
  vec4 panMode;
  vec4 detailRoi;
  vec4 detailFlags;
} ubo;

layout(binding = 1) uniform sampler2D baseTexture;
layout(binding = 2) uniform sampler2D detailTexture;

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
  fragColor = color;
}
