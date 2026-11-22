//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: shared helpers. Experiment code: it does not merge.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace vx2 {

inline auto NowNs() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void Log(const char* fmt, ...) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  va_list           args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

/// The test photo. Bands: grey, red, green, blue ramps; a grid of 4096 colours (16 levels per
/// channel); ramps from -0.25 to 1.5 in each channel; a white square at the top left and a red
/// square at the top right that show the orientation.
inline auto MakeFixture(int width, int height) -> std::vector<float> {
  std::vector<float> rgba(static_cast<std::size_t>(width) * height * 4);
  const int          band = height / 8;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float      r = 0, g = 0, b = 0;
      const float u = static_cast<float>(x) / static_cast<float>(width - 1);
      if (y < band) {
        r = g = b = u;
      } else if (y < 2 * band) {
        r = u;
      } else if (y < 3 * band) {
        g = u;
      } else if (y < 4 * band) {
        b = u;
      } else if (y < 7 * band) {
        const int cx = x * 64 / width;
        const int cy = (y - 4 * band) * 64 / (3 * band);
        const int n  = cy * 64 + cx;
        r            = static_cast<float>(n % 16) / 15.0f;
        g            = static_cast<float>((n / 16) % 16) / 15.0f;
        b            = static_cast<float>(n / 256) / 15.0f;
      } else {
        const float v = -0.25f + 1.75f * u;
        const int   k = ((y - 7 * band) * 3) / std::max(1, height - 7 * band);
        r             = k == 0 ? v : 0.5f;
        g             = k == 1 ? v : 0.5f;
        b             = k == 2 ? v : 0.5f;
      }
      if (y < 32 && x < 32) r = g = b = 1.0f;
      if (y < 32 && x >= width - 32) {
        r = 1.0f;
        g = b = 0.0f;
      }
      float* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
      p[0]     = r;
      p[1]     = g;
      p[2]     = b;
      p[3]     = 1.0f;
    }
  }
  return rgba;
}

struct Summary {
  std::size_t count = 0;
  double      p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};

inline auto Summarize(std::vector<double> values) -> Summary {
  Summary s;
  s.count = values.size();
  if (values.empty()) return s;
  std::sort(values.begin(), values.end());
  auto at = [&](double q) {
    const auto i = static_cast<std::size_t>(std::floor(q * static_cast<double>(values.size() - 1)));
    return values[i];
  };
  s.p50 = at(0.50);
  s.p95 = at(0.95);
  s.p99 = at(0.99);
  s.max = values.back();
  double sum = 0;
  for (double v : values) sum += v;
  s.mean = sum / static_cast<double>(values.size());
  return s;
}

inline auto Format(const char* name, const Summary& s) -> std::string {
  char text[256];
  std::snprintf(text, sizeof(text), "%s_n=%zu %s_mean=%.3f %s_p50=%.3f %s_p95=%.3f %s_p99=%.3f %s_max=%.3f",
                name, s.count, name, s.mean, name, s.p50, name, s.p95, name, s.p99, name, s.max);
  return text;
}

}  // namespace vx2
