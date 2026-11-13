//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

typedef struct {
  uint width;
  uint height;
  uint tile_width;
  uint tile_height;
  uint passes;
  uint green_radius;
  uint rb_radius;
  uint rgb_fc[36];
} XTransParams;

static inline int XTransClampCoord(int value, int limit) {
  return clamp(value, 0, limit - 1);
}

static inline uint XTransPatternColorAt(XTransParams params, int y, int x) {
  const int tile_h = max(1, (int)params.tile_height);
  const int tile_w = max(1, (int)params.tile_width);
  const int wrapped_y = (y % tile_h + tile_h) % tile_h;
  const int wrapped_x = (x % tile_w + tile_w) % tile_w;
  return params.rgb_fc[wrapped_y * tile_w + wrapped_x];
}

static inline float XTransSafeRead(global const float* raw, uint offset, XTransParams params, int y,
                                   int x) {
  const int clamped_x = XTransClampCoord(x, (int)params.width);
  const int clamped_y = XTransClampCoord(y, (int)params.height);
  return raw[offset + (size_t)clamped_y * (size_t)params.width + (size_t)clamped_x];
}

static inline float XTransFindDirectionalGreen(global const float* raw, uint raw_off,
                                               XTransParams params, int y, int x) {
  float left = XTransSafeRead(raw, raw_off, params, y, x);
  float right = left;
  float up = left;
  float down = left;

  int has_left = 0;
  int has_right = 0;
  int has_up = 0;
  int has_down = 0;

  for (int radius = 1; radius <= (int)params.green_radius && (!has_left || !has_right);
       ++radius) {
    if (!has_left && XTransPatternColorAt(params, y, x - radius) == 1u) {
      left = XTransSafeRead(raw, raw_off, params, y, x - radius);
      has_left = 1;
    }
    if (!has_right && XTransPatternColorAt(params, y, x + radius) == 1u) {
      right = XTransSafeRead(raw, raw_off, params, y, x + radius);
      has_right = 1;
    }
  }

  for (int radius = 1; radius <= (int)params.green_radius && (!has_up || !has_down); ++radius) {
    if (!has_up && XTransPatternColorAt(params, y - radius, x) == 1u) {
      up = XTransSafeRead(raw, raw_off, params, y - radius, x);
      has_up = 1;
    }
    if (!has_down && XTransPatternColorAt(params, y + radius, x) == 1u) {
      down = XTransSafeRead(raw, raw_off, params, y + radius, x);
      has_down = 1;
    }
  }

  if (has_left && has_right && has_up && has_down) {
    const float horizontal_grad = fabs(left - right);
    const float vertical_grad = fabs(up - down);
    return horizontal_grad <= vertical_grad ? 0.5f * (left + right) : 0.5f * (up + down);
  }
  if (has_left && has_right) {
    return 0.5f * (left + right);
  }
  if (has_up && has_down) {
    return 0.5f * (up + down);
  }

  float sum = 0.0f;
  uint count = 0u;
  for (int radius = 1; radius <= (int)params.green_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (XTransPatternColorAt(params, y + dy, x + dx) != 1u) {
          continue;
        }
        sum += XTransSafeRead(raw, raw_off, params, y + dy, x + dx);
        ++count;
      }
    }
    if (count > 0u) {
      break;
    }
  }

  return count > 0u ? sum / (float)count : XTransSafeRead(raw, raw_off, params, y, x);
}

static inline float XTransEstimateMissingChannel(global const float* raw, uint raw_off,
                                                 global const float* green, uint green_off,
                                                 XTransParams params, int y, int x,
                                                 uint target_color, float current_green) {
  float sum = 0.0f;
  float wsum = 0.0f;

  for (int radius = 1; radius <= (int)params.rb_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (XTransPatternColorAt(params, y + dy, x + dx) != target_color) {
          continue;
        }

        const float neigh_raw = XTransSafeRead(raw, raw_off, params, y + dy, x + dx);
        const float neigh_green = XTransSafeRead(green, green_off, params, y + dy, x + dx);
        const float weight = 1.0f / (float)(abs(dx) + abs(dy));
        sum += (neigh_raw - neigh_green) * weight;
        wsum += weight;
      }
    }
    if (wsum > 0.0f) {
      break;
    }
  }

  return wsum == 0.0f ? current_green : fmax(0.0f, current_green + sum / wsum);
}

__kernel void xtrans_green(global const float* raw, global float* green, XTransParams params,
                           uint raw_off, uint green_off) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  const int ix = (int)x;
  const int iy = (int)y;
  const uint color = XTransPatternColorAt(params, iy, ix);
  const size_t index = (size_t)y * (size_t)params.width + (size_t)x;
  green[green_off + index] =
      color == 1u ? raw[raw_off + index]
                  : XTransFindDirectionalGreen(raw, raw_off, params, iy, ix);
}

__kernel void xtrans_rgba(global const float* raw, global const float* green, global float4* rgba,
                          XTransParams params, uint raw_off, uint green_off, uint rgba_off) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  const int ix = (int)x;
  const int iy = (int)y;
  const size_t index = (size_t)y * (size_t)params.width + (size_t)x;
  const uint color = XTransPatternColorAt(params, iy, ix);
  const float raw_value = raw[raw_off + index];
  const float green_value = green[green_off + index];

  const float r = color == 0u ? raw_value
                              : XTransEstimateMissingChannel(raw, raw_off, green, green_off, params,
                                                             iy, ix, 0u, green_value);
  float g = green_value;
  const float b = color == 2u ? raw_value
                              : XTransEstimateMissingChannel(raw, raw_off, green, green_off, params,
                                                             iy, ix, 2u, green_value);

  if (color == 1u) {
    g = raw_value;
  }

  rgba[rgba_off + index] = (float4)(r, g, b, 1.0f);
}
