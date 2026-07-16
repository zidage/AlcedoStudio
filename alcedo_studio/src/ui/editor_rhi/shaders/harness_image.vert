#version 440

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 vUv;

void main() {
  gl_Position = vec4(inPosition, 0.0, 1.0);
  vUv = inUv;
}
