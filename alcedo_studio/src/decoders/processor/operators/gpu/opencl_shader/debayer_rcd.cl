//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL kernels for Bayer RCD demosaicing.
// Depends on definitions from raw_utils_opencl.cl (concatenated at build time).

#pragma OPENCL FP_CONTRACT OFF

typedef struct {
  uint width;
  uint height;
  uint stride;
  uint rgb_fc[4];
} SinglePlaneParams;

typedef struct {
  uint width;
  uint height;
  uint plane_stride;
  uint rgba_stride;
} MergeParams;

constant float kEps   = 1e-5f;
constant float kEpsSq = 1e-10f;

static inline uint FC(SinglePlaneParams params, uint y, uint x) {
  return params.rgb_fc[((y & 1u) << 1u) | (x & 1u)];
}

static inline float LoadPlane(global const float* plane, uint base, SinglePlaneParams params, int y,
                              int x) {
  return plane[base + (uint)y * params.stride + (uint)x];
}

static inline float LoadDir(global const float* dir, uint base, SinglePlaneParams params, int y,
                            int x) {
  return dir[base + (uint)y * params.stride + (uint)x];
}

static inline float ClampRcdOutput(float value) {
  return fmax(value, 0.0f);
}

static inline float RcdDirectionalStat(float c, float m1, float p1, float m2, float p2,
                                       float m3, float p3, float m4, float p4) {
  float stat = 0.0f;
  stat += -18.f * c * m1;
  stat += -18.f * c * p1;
  stat += -36.f * c * m2;
  stat += -36.f * c * p2;
  stat += 18.f * c * m3;
  stat += 18.f * c * p3;
  stat += -2.f * c * m4;
  stat += -2.f * c * p4;
  stat += 38.f * c * c;
  stat += -70.f * m1 * p1;
  stat += -12.f * m1 * m2;
  stat += 24.f * m1 * p2;
  stat += -38.f * m1 * m3;
  stat += 16.f * m1 * p3;
  stat += 12.f * m1 * m4;
  stat += -6.f * m1 * p4;
  stat += 46.f * m1 * m1;
  stat += 24.f * p1 * m2;
  stat += -12.f * p1 * p2;
  stat += 16.f * p1 * m3;
  stat += -38.f * p1 * p3;
  stat += -6.f * p1 * m4;
  stat += 12.f * p1 * p4;
  stat += 46.f * p1 * p1;
  stat += 14.f * m2 * p2;
  stat += -12.f * m2 * p3;
  stat += -2.f * m2 * m4;
  stat += 2.f * m2 * p4;
  stat += 11.f * m2 * m2;
  stat += -12.f * p2 * m3;
  stat += 2.f * p2 * m4;
  stat += -2.f * p2 * p4;
  stat += 11.f * p2 * p2;
  stat += 2.f * m3 * p3;
  stat += -6.f * m3 * m4;
  stat += 10.f * m3 * m3;
  stat += -6.f * p3 * p4;
  stat += 10.f * p3 * p3;
  stat += m4 * m4;
  stat += p4 * p4;
  return max(stat, kEpsSq);
}

static inline float LowPassAt(global const float* raw, uint raw_off, SinglePlaneParams params, int y,
                              int x) {
  const float c  = LoadPlane(raw, raw_off, params, y, x);
  const float n  = LoadPlane(raw, raw_off, params, y - 1, x);
  const float s  = LoadPlane(raw, raw_off, params, y + 1, x);
  const float w  = LoadPlane(raw, raw_off, params, y, x - 1);
  const float e  = LoadPlane(raw, raw_off, params, y, x + 1);
  const float nw = LoadPlane(raw, raw_off, params, y - 1, x - 1);
  const float ne = LoadPlane(raw, raw_off, params, y - 1, x + 1);
  const float sw = LoadPlane(raw, raw_off, params, y + 1, x - 1);
  const float se = LoadPlane(raw, raw_off, params, y + 1, x + 1);

  return 0.25f * c + 0.125f * (n + s + w + e) + 0.0625f * (nw + ne + sw + se);
}

static inline float ReconstructRbAtGreen(global const float* channel, uint channel_off,
                                         SinglePlaneParams params, int y, int x, float g_c,
                                         float g_m2, float g_p2, float g_l2, float g_r2, float g_m1,
                                         float g_p1, float g_l1, float g_r1, float vh_disc) {
  const float ch_m1 = LoadPlane(channel, channel_off, params, y - 1, x);
  const float ch_p1 = LoadPlane(channel, channel_off, params, y + 1, x);
  const float ch_m3 = LoadPlane(channel, channel_off, params, y - 3, x);
  const float ch_p3 = LoadPlane(channel, channel_off, params, y + 3, x);
  const float ch_l1 = LoadPlane(channel, channel_off, params, y, x - 1);
  const float ch_r1 = LoadPlane(channel, channel_off, params, y, x + 1);
  const float ch_l3 = LoadPlane(channel, channel_off, params, y, x - 3);
  const float ch_r3 = LoadPlane(channel, channel_off, params, y, x + 3);

  const float N_grad = kEps + fabs(g_c - g_m2) + fabs(ch_m1 - ch_p1) + fabs(ch_m1 - ch_m3);
  const float S_grad = kEps + fabs(g_c - g_p2) + fabs(ch_p1 - ch_m1) + fabs(ch_p1 - ch_p3);
  const float W_grad = kEps + fabs(g_c - g_l2) + fabs(ch_l1 - ch_r1) + fabs(ch_l1 - ch_l3);
  const float E_grad = kEps + fabs(g_c - g_r2) + fabs(ch_r1 - ch_l1) + fabs(ch_r1 - ch_r3);

  const float N_est = ch_m1 - g_m1;
  const float S_est = ch_p1 - g_p1;
  const float W_est = ch_l1 - g_l1;
  const float E_est = ch_r1 - g_r1;

  const float V_est = (N_grad * S_est + S_grad * N_est) / (N_grad + S_grad);
  const float H_est = (E_grad * W_est + W_grad * E_est) / (E_grad + W_grad);
  return ClampRcdOutput(g_c + (1.f - vh_disc) * V_est + vh_disc * H_est);
}

__kernel void rcd_init_and_vh(global const float* raw,
                              global float*       r,
                              global float*       g,
                              global float*       b,
                              global float*       vh_dir,
                              SinglePlaneParams   params,
                              uint raw_off, uint r_off, uint g_off, uint b_off, uint vh_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  uint index = y * params.stride + x;
  float val  = raw[raw_off + index];
  uint color = FC(params, y, x);

  r[r_off + index] = (color == 0u) ? val : 0.0f;
  g[g_off + index] = (color == 1u) ? val : 0.0f;
  b[b_off + index] = (color == 2u) ? val : 0.0f;

  float vh   = 0.0f;
  if (x >= 4u && y >= 4u && x + 4u < params.width && y + 4u < params.height) {
    int ix = (int)x;
    int iy = (int)y;
    float c   = val;

    float vm1 = LoadPlane(raw, raw_off, params, iy - 1, ix);
    float vp1 = LoadPlane(raw, raw_off, params, iy + 1, ix);
    float vm2 = LoadPlane(raw, raw_off, params, iy - 2, ix);
    float vp2 = LoadPlane(raw, raw_off, params, iy + 2, ix);
    float vm3 = LoadPlane(raw, raw_off, params, iy - 3, ix);
    float vp3 = LoadPlane(raw, raw_off, params, iy + 3, ix);
    float vm4 = LoadPlane(raw, raw_off, params, iy - 4, ix);
    float vp4 = LoadPlane(raw, raw_off, params, iy + 4, ix);

    float hm1 = LoadPlane(raw, raw_off, params, iy, ix - 1);
    float hp1 = LoadPlane(raw, raw_off, params, iy, ix + 1);
    float hm2 = LoadPlane(raw, raw_off, params, iy, ix - 2);
    float hp2 = LoadPlane(raw, raw_off, params, iy, ix + 2);
    float hm3 = LoadPlane(raw, raw_off, params, iy, ix - 3);
    float hp3 = LoadPlane(raw, raw_off, params, iy, ix + 3);
    float hm4 = LoadPlane(raw, raw_off, params, iy, ix - 4);
    float hp4 = LoadPlane(raw, raw_off, params, iy, ix + 4);

    float V_stat = RcdDirectionalStat(c, vm1, vp1, vm2, vp2, vm3, vp3, vm4, vp4);
    float H_stat = RcdDirectionalStat(c, hm1, hp1, hm2, hp2, hm3, hp3, hm4, hp4);

    vh = V_stat / (V_stat + H_stat);
  }

  vh_dir[vh_off + index] = vh;
}

__kernel void rcd_green_at_rb(global const float* raw,
                              global const float* vh_dir,
                              global float*       g,
                              SinglePlaneParams   params,
                              uint raw_off, uint vh_off, uint g_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height || x < 4u || y < 4u ||
      x + 4u >= params.width || y + 4u >= params.height) {
    return;
  }

  if (FC(params, y, x) == 1u) {
    return;
  }

  int ix = (int)x;
  int iy = (int)y;

  float VH_central = LoadDir(vh_dir, vh_off, params, iy, ix);
  float VH_neigh   = 0.25f * (LoadDir(vh_dir, vh_off, params, iy - 1, ix - 1) +
                              LoadDir(vh_dir, vh_off, params, iy - 1, ix + 1) +
                              LoadDir(vh_dir, vh_off, params, iy + 1, ix - 1) +
                              LoadDir(vh_dir, vh_off, params, iy + 1, ix + 1));
  float VH_disc =
      (fabs(0.5f - VH_central) < fabs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  float c   = LoadPlane(raw, raw_off, params, iy, ix);
  float vm1 = LoadPlane(raw, raw_off, params, iy - 1, ix);
  float vp1 = LoadPlane(raw, raw_off, params, iy + 1, ix);
  float vm2 = LoadPlane(raw, raw_off, params, iy - 2, ix);
  float vp2 = LoadPlane(raw, raw_off, params, iy + 2, ix);
  float vm3 = LoadPlane(raw, raw_off, params, iy - 3, ix);
  float vp3 = LoadPlane(raw, raw_off, params, iy + 3, ix);
  float vm4 = LoadPlane(raw, raw_off, params, iy - 4, ix);
  float vp4 = LoadPlane(raw, raw_off, params, iy + 4, ix);

  float hm1 = LoadPlane(raw, raw_off, params, iy, ix - 1);
  float hp1 = LoadPlane(raw, raw_off, params, iy, ix + 1);
  float hm2 = LoadPlane(raw, raw_off, params, iy, ix - 2);
  float hp2 = LoadPlane(raw, raw_off, params, iy, ix + 2);
  float hm3 = LoadPlane(raw, raw_off, params, iy, ix - 3);
  float hp3 = LoadPlane(raw, raw_off, params, iy, ix + 3);
  float hm4 = LoadPlane(raw, raw_off, params, iy, ix - 4);
  float hp4 = LoadPlane(raw, raw_off, params, iy, ix + 4);

  float lpf_c  = LowPassAt(raw, raw_off, params, iy, ix);
  float lpf_n2 = LowPassAt(raw, raw_off, params, iy - 2, ix);
  float lpf_s2 = LowPassAt(raw, raw_off, params, iy + 2, ix);
  float lpf_w2 = LowPassAt(raw, raw_off, params, iy, ix - 2);
  float lpf_e2 = LowPassAt(raw, raw_off, params, iy, ix + 2);

  float N_grad = kEps + fabs(vm1 - vp1) + fabs(c - vm2) + fabs(vm1 - vm3) + fabs(vm2 - vm4);
  float S_grad = kEps + fabs(vp1 - vm1) + fabs(c - vp2) + fabs(vp1 - vp3) + fabs(vp2 - vp4);
  float W_grad = kEps + fabs(hm1 - hp1) + fabs(c - hm2) + fabs(hm1 - hm3) + fabs(hm2 - hm4);
  float E_grad = kEps + fabs(hp1 - hm1) + fabs(c - hp2) + fabs(hp1 - hp3) + fabs(hp2 - hp4);

  float N_est  = vm1 * (1.f + (lpf_c - lpf_n2) / (kEps + lpf_c + lpf_n2));
  float S_est  = vp1 * (1.f + (lpf_c - lpf_s2) / (kEps + lpf_c + lpf_s2));
  float W_est  = hm1 * (1.f + (lpf_c - lpf_w2) / (kEps + lpf_c + lpf_w2));
  float E_est  = hp1 * (1.f + (lpf_c - lpf_e2) / (kEps + lpf_c + lpf_e2));

  float V_est  = (S_grad * N_est + N_grad * S_est) / (N_grad + S_grad);
  float H_est  = (W_grad * E_est + E_grad * W_est) / (E_grad + W_grad);

  g[g_off + y * params.stride + x] = ClampRcdOutput(VH_disc * H_est + (1.f - VH_disc) * V_est);
}

__kernel void rcd_pq_dir(global const float* raw,
                         global float*       pq_dir,
                         SinglePlaneParams   params,
                         uint raw_off, uint pq_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  float pq = 0.0f;
  if (x >= 4u && y >= 4u && x + 4u < params.width && y + 4u < params.height &&
      FC(params, y, x) != 1u) {
    int ix = (int)x;
    int iy = (int)y;
    float c   = LoadPlane(raw, raw_off, params, iy, ix);

    float nw1 = LoadPlane(raw, raw_off, params, iy - 1, ix - 1);
    float se1 = LoadPlane(raw, raw_off, params, iy + 1, ix + 1);
    float nw2 = LoadPlane(raw, raw_off, params, iy - 2, ix - 2);
    float se2 = LoadPlane(raw, raw_off, params, iy + 2, ix + 2);
    float nw3 = LoadPlane(raw, raw_off, params, iy - 3, ix - 3);
    float se3 = LoadPlane(raw, raw_off, params, iy + 3, ix + 3);
    float nw4 = LoadPlane(raw, raw_off, params, iy - 4, ix - 4);
    float se4 = LoadPlane(raw, raw_off, params, iy + 4, ix + 4);

    float sw1 = LoadPlane(raw, raw_off, params, iy + 1, ix - 1);
    float ne1 = LoadPlane(raw, raw_off, params, iy - 1, ix + 1);
    float sw2 = LoadPlane(raw, raw_off, params, iy + 2, ix - 2);
    float ne2 = LoadPlane(raw, raw_off, params, iy - 2, ix + 2);
    float sw3 = LoadPlane(raw, raw_off, params, iy + 3, ix - 3);
    float ne3 = LoadPlane(raw, raw_off, params, iy - 3, ix + 3);
    float sw4 = LoadPlane(raw, raw_off, params, iy + 4, ix - 4);
    float ne4 = LoadPlane(raw, raw_off, params, iy - 4, ix + 4);

    float P_stat = RcdDirectionalStat(c, nw1, se1, nw2, se2, nw3, se3, nw4, se4);
    float Q_stat = RcdDirectionalStat(c, sw1, ne1, sw2, ne2, sw3, ne3, sw4, ne4);

    pq = P_stat / (P_stat + Q_stat);
  }

  pq_dir[pq_off + y * params.stride + x] = pq;
}

__kernel void rcd_rb_at_rb(global const float* pq_dir,
                           global const float* g,
                           global float*       r,
                           global float*       b,
                           SinglePlaneParams   params,
                           uint pq_off, uint g_off, uint r_off, uint b_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height || x < 4u || y < 4u ||
      x + 4u >= params.width || y + 4u >= params.height) {
    return;
  }

  uint color = FC(params, y, x);
  if (color == 1u) {
    return;
  }

  int ix = (int)x;
  int iy = (int)y;
  uint c = 2u - color;

  float PQ_c    = LoadDir(pq_dir, pq_off, params, iy, ix);
  float PQ_n    = 0.25f * (LoadDir(pq_dir, pq_off, params, iy - 1, ix - 1) +
                           LoadDir(pq_dir, pq_off, params, iy - 1, ix + 1) +
                           LoadDir(pq_dir, pq_off, params, iy + 1, ix - 1) +
                           LoadDir(pq_dir, pq_off, params, iy + 1, ix + 1));
  float PQ_disc = (fabs(0.5f - PQ_c) < fabs(0.5f - PQ_n)) ? PQ_n : PQ_c;

  float g_c     = LoadPlane(g, g_off, params, iy, ix);

  global const float* channel = (c == 0u) ? r : b;
  uint channel_off            = (c == 0u) ? r_off : b_off;

  float ch_nw1 = LoadPlane(channel, channel_off, params, iy - 1, ix - 1);
  float ch_ne1 = LoadPlane(channel, channel_off, params, iy - 1, ix + 1);
  float ch_sw1 = LoadPlane(channel, channel_off, params, iy + 1, ix - 1);
  float ch_se1 = LoadPlane(channel, channel_off, params, iy + 1, ix + 1);

  float ch_nw3 = LoadPlane(channel, channel_off, params, iy - 3, ix - 3);
  float ch_ne3 = LoadPlane(channel, channel_off, params, iy - 3, ix + 3);
  float ch_sw3 = LoadPlane(channel, channel_off, params, iy + 3, ix - 3);
  float ch_se3 = LoadPlane(channel, channel_off, params, iy + 3, ix + 3);

  float g_nw2  = LoadPlane(g, g_off, params, iy - 2, ix - 2);
  float g_ne2  = LoadPlane(g, g_off, params, iy - 2, ix + 2);
  float g_sw2  = LoadPlane(g, g_off, params, iy + 2, ix - 2);
  float g_se2  = LoadPlane(g, g_off, params, iy + 2, ix + 2);

  float NW_grad =
      kEps + fabs(ch_nw1 - ch_se1) + fabs(ch_nw1 - ch_nw3) + fabs(g_c - g_nw2);
  float NE_grad =
      kEps + fabs(ch_ne1 - ch_sw1) + fabs(ch_ne1 - ch_ne3) + fabs(g_c - g_ne2);
  float SW_grad =
      kEps + fabs(ch_sw1 - ch_ne1) + fabs(ch_sw1 - ch_sw3) + fabs(g_c - g_sw2);
  float SE_grad =
      kEps + fabs(ch_se1 - ch_nw1) + fabs(ch_se1 - ch_se3) + fabs(g_c - g_se2);

  float g_nw1 = LoadPlane(g, g_off, params, iy - 1, ix - 1);
  float g_ne1 = LoadPlane(g, g_off, params, iy - 1, ix + 1);
  float g_sw1 = LoadPlane(g, g_off, params, iy + 1, ix - 1);
  float g_se1 = LoadPlane(g, g_off, params, iy + 1, ix + 1);

  float NW_est = ch_nw1 - g_nw1;
  float NE_est = ch_ne1 - g_ne1;
  float SW_est = ch_sw1 - g_sw1;
  float SE_est = ch_se1 - g_se1;

  float P_est   = (NW_grad * SE_est + SE_grad * NW_est) / (NW_grad + SE_grad);
  float Q_est   = (NE_grad * SW_est + SW_grad * NE_est) / (NE_grad + SW_grad);
  float out_val = ClampRcdOutput(g_c + (1.f - PQ_disc) * P_est + PQ_disc * Q_est);

  if (c == 0u) {
    r[r_off + y * params.stride + x] = out_val;
  } else {
    b[b_off + y * params.stride + x] = out_val;
  }
}

__kernel void rcd_rb_at_g(global const float* vh_dir,
                          global const float* g,
                          global float*       r,
                          global float*       b,
                          SinglePlaneParams   params,
                          uint vh_off, uint g_off, uint r_off, uint b_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height || x < 4u || y < 4u ||
      x + 4u >= params.width || y + 4u >= params.height ||
      FC(params, y, x) != 1u) {
    return;
  }

  int ix = (int)x;
  int iy = (int)y;

  float VH_central = LoadDir(vh_dir, vh_off, params, iy, ix);
  float VH_neigh   = 0.25f * (LoadDir(vh_dir, vh_off, params, iy - 1, ix - 1) +
                              LoadDir(vh_dir, vh_off, params, iy - 1, ix + 1) +
                              LoadDir(vh_dir, vh_off, params, iy + 1, ix - 1) +
                              LoadDir(vh_dir, vh_off, params, iy + 1, ix + 1));
  float VH_disc =
      (fabs(0.5f - VH_central) < fabs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  float g_c  = LoadPlane(g, g_off, params, iy, ix);
  float g_m2 = LoadPlane(g, g_off, params, iy - 2, ix);
  float g_p2 = LoadPlane(g, g_off, params, iy + 2, ix);
  float g_l2 = LoadPlane(g, g_off, params, iy, ix - 2);
  float g_r2 = LoadPlane(g, g_off, params, iy, ix + 2);
  float g_m1 = LoadPlane(g, g_off, params, iy - 1, ix);
  float g_p1 = LoadPlane(g, g_off, params, iy + 1, ix);
  float g_l1 = LoadPlane(g, g_off, params, iy, ix - 1);
  float g_r1 = LoadPlane(g, g_off, params, iy, ix + 1);

  uint index = y * params.stride + x;

  r[r_off + index] = ReconstructRbAtGreen(r, r_off, params, iy, ix, g_c, g_m2, g_p2, g_l2, g_r2,
                                          g_m1, g_p1, g_l1, g_r1, VH_disc);
  b[b_off + index] = ReconstructRbAtGreen(b, b_off, params, iy, ix, g_c, g_m2, g_p2, g_l2, g_r2,
                                          g_m1, g_p1, g_l1, g_r1, VH_disc);
}

__kernel void rcd_merge_rgba(global const float* r,
                             global const float* g,
                             global const float* b,
                             global float4*      out_rgba,
                             SinglePlaneParams   in_params,
                             MergeParams         out_params,
                             uint r_off, uint g_off, uint b_off, uint out_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= in_params.width || y >= in_params.height) {
    return;
  }

  // Skip the 4-pixel invalid border band produced by RCD.
  if (x < 4u || y < 4u || x + 4u >= in_params.width || y + 4u >= in_params.height) {
    return;
  }

  uint in_idx  = y * in_params.stride + x;
  uint out_idx = (y - 4u) * out_params.rgba_stride + (x - 4u);
  out_rgba[out_off + out_idx] =
      (float4)(r[r_off + in_idx], g[g_off + in_idx], b[b_off + in_idx], 1.0f);
}
