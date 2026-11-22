//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: a read and write pass over one memory block (the speed of the block shows where
// Windows keeps it), and a read of an address with no memory (a device loss of the pipeline
// device). Experiment code: it does not merge.

struct TouchPush {
  uint64_t base;   // device address of the block
  uint64_t count;  // float4 elements
};

[[vk::push_constant]] TouchPush touch;

// A grid-stride loop: a fixed grid of 1024 groups of 256 threads covers a block of any size.
[shader("compute")]
[numthreads(256, 1, 1)]
void vx5_touch(uint3 id: SV_DispatchThreadID) {
  const uint64_t stride = 1024ULL * 256ULL;
  for (uint64_t i = id.x; i < touch.count; i += stride) {
    float4* p = (float4*)(touch.base + i * 16ULL);
    *p        = *p * 0.999f + 1.0e-6f;
  }
}

struct FaultPush {
  uint64_t bad;  // an address with no memory
  uint64_t out;  // a valid float buffer
};

[[vk::push_constant]] FaultPush fault;

[shader("compute")]
[numthreads(64, 1, 1)]
void vx5_fault(uint3 id: SV_DispatchThreadID) {
  float* src = (float*)(fault.bad + uint64_t(id.x) * 4096ULL);
  float* dst = (float*)(fault.out + uint64_t(id.x) * 4ULL);
  *dst       = *src + 1.0f;
}
