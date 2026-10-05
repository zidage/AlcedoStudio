//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/hash/sha256.hpp"

#include <algorithm>
#include <vector>

namespace alcedo {
namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr auto RotateRight(uint32_t value, int bits) -> uint32_t {
  return (value >> bits) | (value << (32 - bits));
}

void CompressBlock(std::array<uint32_t, 8>& state, const uint8_t* block) {
  std::array<uint32_t, 64> w{};
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1    = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t ch    = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0    = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    h                    = g;
    g                    = f;
    f                    = e;
    e                    = d + temp1;
    d                    = c;
    c                    = b;
    b                    = a;
    a                    = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

auto ComputeSha256(std::span<const std::byte> bytes) -> std::array<uint8_t, 32> {
  std::array<uint32_t, 8> state      = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto*             data       = reinterpret_cast<const uint8_t*>(bytes.data());
  const auto              full_count = bytes.size() / 64;
  for (std::size_t i = 0; i < full_count; ++i) {
    CompressBlock(state, data + i * 64);
  }
  // Final blocks: the remaining bytes, 0x80, zero padding and the 64-bit bit length.
  const auto           remaining = bytes.size() - full_count * 64;
  std::vector<uint8_t> tail(remaining < 56 ? 64 : 128, 0);
  std::copy(data + full_count * 64, data + bytes.size(), tail.begin());
  tail[remaining]          = 0x80;
  const uint64_t bit_count = static_cast<uint64_t>(bytes.size()) * 8u;
  for (int i = 0; i < 8; ++i) {
    tail[tail.size() - 1 - i] = static_cast<uint8_t>(bit_count >> (i * 8));
  }
  for (std::size_t offset = 0; offset < tail.size(); offset += 64) {
    CompressBlock(state, tail.data() + offset);
  }
  std::array<uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4]     = static_cast<uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
  }
  return digest;
}

auto ComputeSha256Hex(std::span<const std::byte> bytes) -> std::string {
  constexpr char kHex[] = "0123456789abcdef";
  const auto     digest = ComputeSha256(bytes);
  std::string    text;
  text.reserve(64);
  for (const auto value : digest) {
    text.push_back(kHex[value >> 4]);
    text.push_back(kHex[value & 0x0F]);
  }
  return text;
}

}  // namespace alcedo
