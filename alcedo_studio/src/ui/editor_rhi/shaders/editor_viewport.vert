#version 440

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;

layout(std140, binding = 0) uniform ViewerUniforms {
  vec4 scaleZoom;
  vec4 panMode;
  vec4 detailRoi;
  vec4 detailFlags;
} ubo;

layout(location = 0) out vec2 vUv;

void main() {
  vec2 position = inPosition * ubo.scaleZoom.xy * ubo.scaleZoom.z + ubo.panMode.xy;
  gl_Position = vec4(position, 0.0, 1.0);
  vUv = inUv;
}
