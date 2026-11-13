//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL

#define ALCEDO_OPENCL_NEIGHBOR_MAX_TAP_COUNT 64
#define ALCEDO_OPENCL_NEIGHBOR_OP_SHARPEN    1u
#define ALCEDO_OPENCL_NEIGHBOR_OP_CLARITY    2u
#define ALCEDO_OPENCL_NEIGHBOR_OP_HALATION   3u
#define ALCEDO_OPENCL_NEIGHBOR_OP_FILM_GRAIN 4u

typedef struct {
  uint  kind_;
  uint  radius_;
  uint  tap_count_;
  float amount_;
  float threshold_;
  float weights_[ALCEDO_OPENCL_NEIGHBOR_MAX_TAP_COUNT];
  uint  enabled_;
  int   eotf_;
  uint  seed_lo_;
  uint  seed_hi_;
  float sigma_x_;
  float sigma_y_;
  float redshift_[3];
  float reserved_;
  uint  roi_enabled_;
  int   roi_x_;
  int   roi_y_;
  float roi_scale_x_;
  float roi_scale_y_;
  int   roi_reference_width_;
  int   roi_reference_height_;
  uint  reserved_tail_;
} OpenClNeighborStageParams;

// === Detail helpers =============================================================

static inline float opencl_detail_luminance(float4 c) {
  // Match the CUDA implementation's COLOR_BGR2GRAY coefficients.
  return c.x * 0.114f + c.y * 0.587f + c.z * 0.299f;
}

static inline float4 opencl_detail_read_clamped(__global const float4* src, int x, int y,
                                                int width, int height) {
  const int cx = clamp(x, 0, width - 1);
  const int cy = clamp(y, 0, height - 1);
  return src[(size_t)cy * (size_t)width + (size_t)cx];
}

static inline float opencl_detail_read_log_clamped(__global const float* src, int x, int y,
                                                   int width, int height) {
  const int cx = clamp(x, 0, width - 1);
  const int cy = clamp(y, 0, height - 1);
  return src[(size_t)cy * (size_t)width + (size_t)cx];
}

static inline float opencl_detail_smoothstep(float edge0, float edge1, float x) {
  const float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL
