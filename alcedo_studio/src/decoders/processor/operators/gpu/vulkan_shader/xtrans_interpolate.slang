//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group xtrans_interpolate: the Slang form of xtrans_green and xtrans_rgba of
// opencl_shader/xtrans_interpolate.cl (plan phase VK4), on planes at device addresses (plan
// section 4.2). The names and the push constant layouts are in
// include/vulkan/vulkan_xtrans_kernels.hpp.

#include "raw_utils.slangh"

// XTransParams: 172 bytes, the struct of xtrans_interpolate.cl. Above the 128 push constant bytes
// of every Vulkan device, so the record is in a buffer.
struct XTransParams {
  uint width;
  uint height;
  uint tile_width;
  uint tile_height;
  uint passes;
  uint green_radius;
  uint rb_radius;
  uint rgb_fc[36];
};

// XTransPushConstants: 32 bytes, one layout for both kernels.
struct XTransPushConstants {
  float*   raw;
  float*   green;
  float*   rgba;        // xtrans_rgba only
  uint64_t parameters;  // device address of one XTransParams record
};

static int XTransClampCoord(int value, int limit) {
  return clamp(value, 0, limit - 1);
}

static uint XTransPatternColorAt(XTransParams* params, int y, int x) {
  const int tile_h    = max(1, int(params->tile_height));
  const int tile_w    = max(1, int(params->tile_width));
  const int wrapped_y = WrapCoord(y, tile_h);
  const int wrapped_x = WrapCoord(x, tile_w);
  return params->rgb_fc[wrapped_y * tile_w + wrapped_x];
}

static float XTransSafeRead(float* plane, XTransParams* params, int y, int x) {
  const int clamped_x = XTransClampCoord(x, int(params->width));
  const int clamped_y = XTransClampCoord(y, int(params->height));
  return plane[uint(clamped_y) * params->width + uint(clamped_x)];
}

static float XTransFindDirectionalGreen(float* raw, XTransParams* params, int y, int x) {
  float left  = XTransSafeRead(raw, params, y, x);
  float right = left;
  float up    = left;
  float down  = left;

  bool  has_left  = false;
  bool  has_right = false;
  bool  has_up    = false;
  bool  has_down  = false;

  const int green_radius = int(params->green_radius);
  for (int radius = 1; radius <= green_radius && (!has_left || !has_right); ++radius) {
    if (!has_left && XTransPatternColorAt(params, y, x - radius) == 1u) {
      left     = XTransSafeRead(raw, params, y, x - radius);
      has_left = true;
    }
    if (!has_right && XTransPatternColorAt(params, y, x + radius) == 1u) {
      right     = XTransSafeRead(raw, params, y, x + radius);
      has_right = true;
    }
  }

  for (int radius = 1; radius <= green_radius && (!has_up || !has_down); ++radius) {
    if (!has_up && XTransPatternColorAt(params, y - radius, x) == 1u) {
      up     = XTransSafeRead(raw, params, y - radius, x);
      has_up = true;
    }
    if (!has_down && XTransPatternColorAt(params, y + radius, x) == 1u) {
      down     = XTransSafeRead(raw, params, y + radius, x);
      has_down = true;
    }
  }

  if (has_left && has_right && has_up && has_down) {
    const float horizontal_grad = abs(left - right);
    const float vertical_grad   = abs(up - down);
    return horizontal_grad <= vertical_grad ? 0.5f * (left + right) : 0.5f * (up + down);
  }
  if (has_left && has_right) {
    return 0.5f * (left + right);
  }
  if (has_up && has_down) {
    return 0.5f * (up + down);
  }

  float sum   = 0.0f;
  uint  count = 0u;
  for (int radius = 1; radius <= green_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (XTransPatternColorAt(params, y + dy, x + dx) != 1u) {
          continue;
        }
        sum += XTransSafeRead(raw, params, y + dy, x + dx);
        ++count;
      }
    }
    if (count > 0u) {
      break;
    }
  }

  return count > 0u ? sum / float(count) : XTransSafeRead(raw, params, y, x);
}

static float XTransEstimateMissingChannel(float* raw, float* green, XTransParams* params, int y,
                                          int x, uint target_color, float current_green) {
  float     sum       = 0.0f;
  float     wsum      = 0.0f;
  const int rb_radius = int(params->rb_radius);

  for (int radius = 1; radius <= rb_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (XTransPatternColorAt(params, y + dy, x + dx) != target_color) {
          continue;
        }
        const float neigh_raw   = XTransSafeRead(raw, params, y + dy, x + dx);
        const float neigh_green = XTransSafeRead(green, params, y + dy, x + dx);
        const float weight      = 1.0f / float(abs(dx) + abs(dy));
        sum += (neigh_raw - neigh_green) * weight;
        wsum += weight;
      }
    }
    if (wsum > 0.0f) {
      break;
    }
  }

  return wsum == 0.0f ? current_green : max(0.0f, current_green + sum / wsum);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void xtrans_green(uint3 id: SV_DispatchThreadID, uniform XTransPushConstants push) {
  XTransParams* params = (XTransParams*)(push.parameters);
  if (id.x >= params->width || id.y >= params->height) {
    return;
  }
  const int  ix    = int(id.x);
  const int  iy    = int(id.y);
  const uint color = XTransPatternColorAt(params, iy, ix);
  const uint index = id.y * params->width + id.x;
  push.green[index] =
      color == 1u ? push.raw[index] : XTransFindDirectionalGreen(push.raw, params, iy, ix);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void xtrans_rgba(uint3 id: SV_DispatchThreadID, uniform XTransPushConstants push) {
  XTransParams* params = (XTransParams*)(push.parameters);
  if (id.x >= params->width || id.y >= params->height) {
    return;
  }
  const int   ix          = int(id.x);
  const int   iy          = int(id.y);
  const uint  index       = id.y * params->width + id.x;
  const uint  color       = XTransPatternColorAt(params, iy, ix);
  const float raw_value   = push.raw[index];
  const float green_value = push.green[index];

  const float r = color == 0u ? raw_value
                              : XTransEstimateMissingChannel(push.raw, push.green, params, iy, ix,
                                                             0u, green_value);
  float       g = green_value;
  const float b = color == 2u ? raw_value
                              : XTransEstimateMissingChannel(push.raw, push.green, params, iy, ix,
                                                             2u, green_value);
  if (color == 1u) {
    g = raw_value;
  }

  const uint base     = index * 4;
  push.rgba[base]     = r;
  push.rgba[base + 1] = g;
  push.rgba[base + 2] = b;
  push.rgba[base + 3] = 1.0f;
}
