//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

static inline ulong metal_prng_mix64(ulong value) {
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9UL;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebUL;
  return value ^ (value >> 31u);
}

static inline ulong metal_prng_counter_hash(ulong seed, ulong stream, ulong counter) {
  ulong value = seed + 0x9e3779b97f4a7c15UL;
  value ^= metal_prng_mix64(stream + 0xd1b54a32d192ed03UL);
  value += counter * 0x9e3779b97f4a7c15UL;
  return metal_prng_mix64(value);
}

static inline ulong metal_prng_pixel_stream_2d(int x, int y, uint channel) {
  ulong value = static_cast<ulong>(static_cast<uint>(x));
  value |= static_cast<ulong>(static_cast<uint>(y)) << 32u;
  value ^= static_cast<ulong>(channel) * 0x9e3779b97f4a7c15UL;
  return metal_prng_mix64(value);
}

static inline float metal_prng_uniform_float01_from_bits(ulong bits) {
  const uint mantissa = static_cast<uint>((bits >> 40u) & 0x00ffffffUL);
  return static_cast<float>(mantissa) * (1.0f / 16777216.0f);
}

static inline float metal_prng_uniform_float01(ulong seed, ulong stream, ulong counter) {
  return metal_prng_uniform_float01_from_bits(metal_prng_counter_hash(seed, stream, counter));
}
