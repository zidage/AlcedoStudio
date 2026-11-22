//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: the separable Gaussian passes of the film print (film_print_blur_horizontal and
// film_print_blur_vertical of edit/runtime/opencl/shader/film_print.cl) on planes of 3 floats per
// pixel. The taps are a device address: weights[layer * (kMaxRadius + 1) + tap]. Experiment code:
// it does not merge.

static const int kMaxRadius = 128;

struct BlurPush {
  float* src;
  float* dst;
  float* weights;
  int    width;
  int    height;
  int    radius;
  float  weight;
  int    accumulate;
};

[[vk::push_constant]] BlurPush push;

static int Reflect(int position, int size) {
  if (position >= 0 && position < size) return position;
  const int period = 2 * size;
  position %= period;
  if (position < 0) position += period;
  return position < size ? position : period - 1 - position;
}

static float3 Load(float* plane, uint index) {
  float* t = plane + 3U * index;
  return float3(t[0], t[1], t[2]);
}

static void Store(float* plane, uint index, float3 v) {
  float* t = plane + 3U * index;
  t[0]     = v.x;
  t[1]     = v.y;
  t[2]     = v.z;
}

static float3 Taps(int tap) {
  return float3(push.weights[tap], push.weights[(kMaxRadius + 1) + tap],
                push.weights[2 * (kMaxRadius + 1) + tap]);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void film_blur_horizontal(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint row = uint(y) * uint(push.width);
  float3     sum = Taps(0) * Load(push.src, row + uint(x));
  for (int tap = 1; tap <= push.radius; ++tap) {
    const float3 left  = Load(push.src, row + uint(Reflect(x - tap, push.width)));
    const float3 right = Load(push.src, row + uint(Reflect(x + tap, push.width)));
    sum += Taps(tap) * (left + right);
  }
  Store(push.dst, row + uint(x), sum);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void film_blur_vertical(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint index = uint(y) * uint(push.width) + uint(x);
  float3     sum   = Taps(0) * Load(push.src, index);
  for (int tap = 1; tap <= push.radius; ++tap) {
    const float3 top    = Load(push.src, uint(Reflect(y - tap, push.height)) * uint(push.width) + uint(x));
    const float3 bottom = Load(push.src, uint(Reflect(y + tap, push.height)) * uint(push.width) + uint(x));
    sum += Taps(tap) * (top + bottom);
  }
  if (push.accumulate != 0) {
    Store(push.dst, index, Load(push.dst, index) + push.weight * sum);
  } else {
    Store(push.dst, index, push.weight * sum);
  }
}
