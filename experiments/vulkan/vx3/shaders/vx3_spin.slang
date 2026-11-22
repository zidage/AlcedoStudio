//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: a kernel of a chosen length for the GPU timeout test. Each invocation runs a
// chain of dependent multiply-adds; the result goes to memory so that the compiler keeps the loop.
// Experiment code: it does not merge.

struct SpinPush {
  float* out;
  uint   iterations;
  uint   width;
  uint   height;
};

[[vk::push_constant]] SpinPush push;

[shader("compute")]
[numthreads(16, 16, 1)]
void vx3_spin(uint3 id: SV_DispatchThreadID) {
  if (id.x >= push.width || id.y >= push.height) return;
  float a = float(id.x & 255U) * 1.0e-3f + 0.5f;
  float b = float(id.y & 255U) * 1.0e-3f + 0.25f;
  for (uint i = 0; i < push.iterations; ++i) {
    a = a * 0.9999931f + b * 1.3e-6f;
    b = b * 0.9999917f + a * 1.1e-6f;
  }
  push.out[(id.y & 1023U) * 1024U + (id.x & 1023U)] = a + b;
}
