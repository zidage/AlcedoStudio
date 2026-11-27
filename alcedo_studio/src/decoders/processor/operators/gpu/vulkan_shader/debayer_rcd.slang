//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group debayer_rcd: the Slang form of the five RCD kernels of
// opencl_shader/debayer_rcd.cl (plan phase VK4), on planes at device addresses (plan section 4.2).
// The names and the push constant layout are in include/vulkan/vulkan_debayer_rcd_kernels.hpp.
//
// The OpenCL program has FP_CONTRACT OFF and evaluates each sum from left to right. The direction
// weights select between two estimates where they cross 0.5, so one ulp of a weight can change a
// texel by a large amount (VK4 record: up to 0.1). So these kernels keep the OpenCL rounding of
// each operation: each float multiply is Mul (AlcedoExactMul: NoContraction, no FMA), each sum of
// three or more terms is a chain of Add (AlcedoExactAdd: NoContraction, no reassociation; the
// NVIDIA driver combines the adds of a plain sum). An add or a subtract of two values that are
// not products needs no decoration. A division stays OpFDiv: the OpenCL division of the NVIDIA
// driver is not correctly rounded either, and a corrected Vulkan division did not reduce the
// difference (VK4 record).

#include "vulkan/shader/math.slangh"

// SinglePlaneParams: 28 bytes, the struct of debayer_rcd.cl.
struct SinglePlaneParams {
  uint width;
  uint height;
  uint stride;
  uint rgb_fc[4];
};

// RcdPushConstants: 80 bytes, one layout for the five kernels. Each kernel reads the planes of
// its OpenCL arguments.
struct RcdPushConstants {
  float*            raw;
  float*            r;
  float*            g;
  float*            b;
  float*            vh;
  float*            pq;
  SinglePlaneParams params;
  uint              reserved;  // pads the block to the 8-byte struct alignment
};

static const float kEps   = 1e-5f;
static const float kEpsSq = 1e-10f;

static float Mul(float x, float y) {
  return AlcedoExactMul(x, y);
}

static float Add(float x, float y) {
  return AlcedoExactAdd(x, y);
}

static uint FC(SinglePlaneParams params, uint y, uint x) {
  return params.rgb_fc[((y & 1u) << 1u) | (x & 1u)];
}

static float LoadPlane(float* plane, SinglePlaneParams params, int y, int x) {
  return plane[uint(y) * params.stride + uint(x)];
}

static float ClampRcdOutput(float value) {
  return max(value, 0.0f);
}

static float RcdDirectionalStat(float c, float m1, float p1, float m2, float p2, float m3,
                                float p3, float m4, float p4) {
  float stat = 0.0f;
  stat = Add(stat, Mul(Mul(-18.f, c), m1));
  stat = Add(stat, Mul(Mul(-18.f, c), p1));
  stat = Add(stat, Mul(Mul(-36.f, c), m2));
  stat = Add(stat, Mul(Mul(-36.f, c), p2));
  stat = Add(stat, Mul(Mul(18.f, c), m3));
  stat = Add(stat, Mul(Mul(18.f, c), p3));
  stat = Add(stat, Mul(Mul(-2.f, c), m4));
  stat = Add(stat, Mul(Mul(-2.f, c), p4));
  stat = Add(stat, Mul(Mul(38.f, c), c));
  stat = Add(stat, Mul(Mul(-70.f, m1), p1));
  stat = Add(stat, Mul(Mul(-12.f, m1), m2));
  stat = Add(stat, Mul(Mul(24.f, m1), p2));
  stat = Add(stat, Mul(Mul(-38.f, m1), m3));
  stat = Add(stat, Mul(Mul(16.f, m1), p3));
  stat = Add(stat, Mul(Mul(12.f, m1), m4));
  stat = Add(stat, Mul(Mul(-6.f, m1), p4));
  stat = Add(stat, Mul(Mul(46.f, m1), m1));
  stat = Add(stat, Mul(Mul(24.f, p1), m2));
  stat = Add(stat, Mul(Mul(-12.f, p1), p2));
  stat = Add(stat, Mul(Mul(16.f, p1), m3));
  stat = Add(stat, Mul(Mul(-38.f, p1), p3));
  stat = Add(stat, Mul(Mul(-6.f, p1), m4));
  stat = Add(stat, Mul(Mul(12.f, p1), p4));
  stat = Add(stat, Mul(Mul(46.f, p1), p1));
  stat = Add(stat, Mul(Mul(14.f, m2), p2));
  stat = Add(stat, Mul(Mul(-12.f, m2), p3));
  stat = Add(stat, Mul(Mul(-2.f, m2), m4));
  stat = Add(stat, Mul(Mul(2.f, m2), p4));
  stat = Add(stat, Mul(Mul(11.f, m2), m2));
  stat = Add(stat, Mul(Mul(-12.f, p2), m3));
  stat = Add(stat, Mul(Mul(2.f, p2), m4));
  stat = Add(stat, Mul(Mul(-2.f, p2), p4));
  stat = Add(stat, Mul(Mul(11.f, p2), p2));
  stat = Add(stat, Mul(Mul(2.f, m3), p3));
  stat = Add(stat, Mul(Mul(-6.f, m3), m4));
  stat = Add(stat, Mul(Mul(10.f, m3), m3));
  stat = Add(stat, Mul(Mul(-6.f, p3), p4));
  stat = Add(stat, Mul(Mul(10.f, p3), p3));
  stat = Add(stat, Mul(m4, m4));
  stat = Add(stat, Mul(p4, p4));
  return max(stat, kEpsSq);
}

static float LowPassAt(float* raw, SinglePlaneParams params, int y, int x) {
  const float c  = LoadPlane(raw, params, y, x);
  const float n  = LoadPlane(raw, params, y - 1, x);
  const float s  = LoadPlane(raw, params, y + 1, x);
  const float w  = LoadPlane(raw, params, y, x - 1);
  const float e  = LoadPlane(raw, params, y, x + 1);
  const float nw = LoadPlane(raw, params, y - 1, x - 1);
  const float ne = LoadPlane(raw, params, y - 1, x + 1);
  const float sw = LoadPlane(raw, params, y + 1, x - 1);
  const float se = LoadPlane(raw, params, y + 1, x + 1);
  const float edges   = Add(Add(Add(n, s), w), e);
  const float corners = Add(Add(Add(nw, ne), sw), se);
  return Add(Add(Mul(0.25f, c), Mul(0.125f, edges)), Mul(0.0625f, corners));
}

static float ReconstructRbAtGreen(float* channel, SinglePlaneParams params, int y, int x,
                                  float g_c, float g_m2, float g_p2, float g_l2, float g_r2,
                                  float g_m1, float g_p1, float g_l1, float g_r1, float vh_disc) {
  const float ch_m1 = LoadPlane(channel, params, y - 1, x);
  const float ch_p1 = LoadPlane(channel, params, y + 1, x);
  const float ch_m3 = LoadPlane(channel, params, y - 3, x);
  const float ch_p3 = LoadPlane(channel, params, y + 3, x);
  const float ch_l1 = LoadPlane(channel, params, y, x - 1);
  const float ch_r1 = LoadPlane(channel, params, y, x + 1);
  const float ch_l3 = LoadPlane(channel, params, y, x - 3);
  const float ch_r3 = LoadPlane(channel, params, y, x + 3);

  const float N_grad =
      Add(Add(Add(kEps, abs(g_c - g_m2)), abs(ch_m1 - ch_p1)), abs(ch_m1 - ch_m3));
  const float S_grad =
      Add(Add(Add(kEps, abs(g_c - g_p2)), abs(ch_p1 - ch_m1)), abs(ch_p1 - ch_p3));
  const float W_grad =
      Add(Add(Add(kEps, abs(g_c - g_l2)), abs(ch_l1 - ch_r1)), abs(ch_l1 - ch_l3));
  const float E_grad =
      Add(Add(Add(kEps, abs(g_c - g_r2)), abs(ch_r1 - ch_l1)), abs(ch_r1 - ch_r3));

  const float N_est = ch_m1 - g_m1;
  const float S_est = ch_p1 - g_p1;
  const float W_est = ch_l1 - g_l1;
  const float E_est = ch_r1 - g_r1;

  const float V_est = (Mul(N_grad, S_est) + Mul(S_grad, N_est)) / (N_grad + S_grad);
  const float H_est = (Mul(E_grad, W_est) + Mul(W_grad, E_est)) / (E_grad + W_grad);
  return ClampRcdOutput(Add(Add(g_c, Mul(1.f - vh_disc, V_est)), Mul(vh_disc, H_est)));
}

static bool InsideBorder(SinglePlaneParams params, uint x, uint y) {
  return x >= 4u && y >= 4u && x + 4u < params.width && y + 4u < params.height;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void rcd_init_and_vh(uint3 id: SV_DispatchThreadID, uniform RcdPushConstants push) {
  const SinglePlaneParams params = push.params;
  const uint              x      = id.x;
  const uint              y      = id.y;
  if (x >= params.width || y >= params.height) {
    return;
  }

  const uint  index = y * params.stride + x;
  const float val   = push.raw[index];
  const uint  color = FC(params, y, x);

  push.r[index] = (color == 0u) ? val : 0.0f;
  push.g[index] = (color == 1u) ? val : 0.0f;
  push.b[index] = (color == 2u) ? val : 0.0f;

  float vh = 0.0f;
  if (InsideBorder(params, x, y)) {
    const int   ix  = int(x);
    const int   iy  = int(y);
    const float c   = val;

    const float vm1 = LoadPlane(push.raw, params, iy - 1, ix);
    const float vp1 = LoadPlane(push.raw, params, iy + 1, ix);
    const float vm2 = LoadPlane(push.raw, params, iy - 2, ix);
    const float vp2 = LoadPlane(push.raw, params, iy + 2, ix);
    const float vm3 = LoadPlane(push.raw, params, iy - 3, ix);
    const float vp3 = LoadPlane(push.raw, params, iy + 3, ix);
    const float vm4 = LoadPlane(push.raw, params, iy - 4, ix);
    const float vp4 = LoadPlane(push.raw, params, iy + 4, ix);

    const float hm1 = LoadPlane(push.raw, params, iy, ix - 1);
    const float hp1 = LoadPlane(push.raw, params, iy, ix + 1);
    const float hm2 = LoadPlane(push.raw, params, iy, ix - 2);
    const float hp2 = LoadPlane(push.raw, params, iy, ix + 2);
    const float hm3 = LoadPlane(push.raw, params, iy, ix - 3);
    const float hp3 = LoadPlane(push.raw, params, iy, ix + 3);
    const float hm4 = LoadPlane(push.raw, params, iy, ix - 4);
    const float hp4 = LoadPlane(push.raw, params, iy, ix + 4);

    const float V_stat = RcdDirectionalStat(c, vm1, vp1, vm2, vp2, vm3, vp3, vm4, vp4);
    const float H_stat = RcdDirectionalStat(c, hm1, hp1, hm2, hp2, hm3, hp3, hm4, hp4);

    vh = V_stat / (V_stat + H_stat);
  }

  push.vh[index] = vh;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void rcd_green_at_rb(uint3 id: SV_DispatchThreadID, uniform RcdPushConstants push) {
  const SinglePlaneParams params = push.params;
  const uint              x      = id.x;
  const uint              y      = id.y;
  if (x >= params.width || y >= params.height || !InsideBorder(params, x, y)) {
    return;
  }
  if (FC(params, y, x) == 1u) {
    return;
  }

  const int   ix         = int(x);
  const int   iy         = int(y);
  float*      raw        = push.raw;
  float*      vh_dir     = push.vh;

  const float VH_central = LoadPlane(vh_dir, params, iy, ix);
  const float VH_neigh = Mul(
      0.25f, Add(Add(Add(LoadPlane(vh_dir, params, iy - 1, ix - 1),
                         LoadPlane(vh_dir, params, iy - 1, ix + 1)),
                     LoadPlane(vh_dir, params, iy + 1, ix - 1)),
                 LoadPlane(vh_dir, params, iy + 1, ix + 1)));
  const float VH_disc =
      (abs(0.5f - VH_central) < abs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const float c   = LoadPlane(raw, params, iy, ix);
  const float vm1 = LoadPlane(raw, params, iy - 1, ix);
  const float vp1 = LoadPlane(raw, params, iy + 1, ix);
  const float vm2 = LoadPlane(raw, params, iy - 2, ix);
  const float vp2 = LoadPlane(raw, params, iy + 2, ix);
  const float vm3 = LoadPlane(raw, params, iy - 3, ix);
  const float vp3 = LoadPlane(raw, params, iy + 3, ix);
  const float vm4 = LoadPlane(raw, params, iy - 4, ix);
  const float vp4 = LoadPlane(raw, params, iy + 4, ix);

  const float hm1 = LoadPlane(raw, params, iy, ix - 1);
  const float hp1 = LoadPlane(raw, params, iy, ix + 1);
  const float hm2 = LoadPlane(raw, params, iy, ix - 2);
  const float hp2 = LoadPlane(raw, params, iy, ix + 2);
  const float hm3 = LoadPlane(raw, params, iy, ix - 3);
  const float hp3 = LoadPlane(raw, params, iy, ix + 3);
  const float hm4 = LoadPlane(raw, params, iy, ix - 4);
  const float hp4 = LoadPlane(raw, params, iy, ix + 4);

  const float lpf_c  = LowPassAt(raw, params, iy, ix);
  const float lpf_n2 = LowPassAt(raw, params, iy - 2, ix);
  const float lpf_s2 = LowPassAt(raw, params, iy + 2, ix);
  const float lpf_w2 = LowPassAt(raw, params, iy, ix - 2);
  const float lpf_e2 = LowPassAt(raw, params, iy, ix + 2);

  const float N_grad =
      Add(Add(Add(Add(kEps, abs(vm1 - vp1)), abs(c - vm2)), abs(vm1 - vm3)), abs(vm2 - vm4));
  const float S_grad =
      Add(Add(Add(Add(kEps, abs(vp1 - vm1)), abs(c - vp2)), abs(vp1 - vp3)), abs(vp2 - vp4));
  const float W_grad =
      Add(Add(Add(Add(kEps, abs(hm1 - hp1)), abs(c - hm2)), abs(hm1 - hm3)), abs(hm2 - hm4));
  const float E_grad =
      Add(Add(Add(Add(kEps, abs(hp1 - hm1)), abs(c - hp2)), abs(hp1 - hp3)), abs(hp2 - hp4));

  const float N_est = Mul(vm1, 1.f + (lpf_c - lpf_n2) / Add(kEps + lpf_c, lpf_n2));
  const float S_est = Mul(vp1, 1.f + (lpf_c - lpf_s2) / Add(kEps + lpf_c, lpf_s2));
  const float W_est = Mul(hm1, 1.f + (lpf_c - lpf_w2) / Add(kEps + lpf_c, lpf_w2));
  const float E_est = Mul(hp1, 1.f + (lpf_c - lpf_e2) / Add(kEps + lpf_c, lpf_e2));

  const float V_est = (Mul(S_grad, N_est) + Mul(N_grad, S_est)) / (N_grad + S_grad);
  const float H_est = (Mul(W_grad, E_est) + Mul(E_grad, W_est)) / (E_grad + W_grad);

  push.g[y * params.stride + x] = ClampRcdOutput(Mul(VH_disc, H_est) + Mul(1.f - VH_disc, V_est));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void rcd_pq_dir(uint3 id: SV_DispatchThreadID, uniform RcdPushConstants push) {
  const SinglePlaneParams params = push.params;
  const uint              x      = id.x;
  const uint              y      = id.y;
  if (x >= params.width || y >= params.height) {
    return;
  }

  float pq = 0.0f;
  if (InsideBorder(params, x, y) && FC(params, y, x) != 1u) {
    const int   ix  = int(x);
    const int   iy  = int(y);
    float*      raw = push.raw;
    const float c   = LoadPlane(raw, params, iy, ix);

    const float nw1 = LoadPlane(raw, params, iy - 1, ix - 1);
    const float se1 = LoadPlane(raw, params, iy + 1, ix + 1);
    const float nw2 = LoadPlane(raw, params, iy - 2, ix - 2);
    const float se2 = LoadPlane(raw, params, iy + 2, ix + 2);
    const float nw3 = LoadPlane(raw, params, iy - 3, ix - 3);
    const float se3 = LoadPlane(raw, params, iy + 3, ix + 3);
    const float nw4 = LoadPlane(raw, params, iy - 4, ix - 4);
    const float se4 = LoadPlane(raw, params, iy + 4, ix + 4);

    const float sw1 = LoadPlane(raw, params, iy + 1, ix - 1);
    const float ne1 = LoadPlane(raw, params, iy - 1, ix + 1);
    const float sw2 = LoadPlane(raw, params, iy + 2, ix - 2);
    const float ne2 = LoadPlane(raw, params, iy - 2, ix + 2);
    const float sw3 = LoadPlane(raw, params, iy + 3, ix - 3);
    const float ne3 = LoadPlane(raw, params, iy - 3, ix + 3);
    const float sw4 = LoadPlane(raw, params, iy + 4, ix - 4);
    const float ne4 = LoadPlane(raw, params, iy - 4, ix + 4);

    const float P_stat = RcdDirectionalStat(c, nw1, se1, nw2, se2, nw3, se3, nw4, se4);
    const float Q_stat = RcdDirectionalStat(c, sw1, ne1, sw2, ne2, sw3, ne3, sw4, ne4);

    pq = P_stat / (P_stat + Q_stat);
  }

  push.pq[y * params.stride + x] = pq;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void rcd_rb_at_rb(uint3 id: SV_DispatchThreadID, uniform RcdPushConstants push) {
  const SinglePlaneParams params = push.params;
  const uint              x      = id.x;
  const uint              y      = id.y;
  if (x >= params.width || y >= params.height || !InsideBorder(params, x, y)) {
    return;
  }

  const uint color = FC(params, y, x);
  if (color == 1u) {
    return;
  }

  const int   ix      = int(x);
  const int   iy      = int(y);
  const uint  c       = 2u - color;
  float*      pq_dir  = push.pq;
  float*      g       = push.g;

  const float PQ_c    = LoadPlane(pq_dir, params, iy, ix);
  const float PQ_n = Mul(
      0.25f, Add(Add(Add(LoadPlane(pq_dir, params, iy - 1, ix - 1),
                         LoadPlane(pq_dir, params, iy - 1, ix + 1)),
                     LoadPlane(pq_dir, params, iy + 1, ix - 1)),
                 LoadPlane(pq_dir, params, iy + 1, ix + 1)));
  const float PQ_disc = (abs(0.5f - PQ_c) < abs(0.5f - PQ_n)) ? PQ_n : PQ_c;

  const float g_c     = LoadPlane(g, params, iy, ix);

  float*      channel = (c == 0u) ? push.r : push.b;

  const float ch_nw1  = LoadPlane(channel, params, iy - 1, ix - 1);
  const float ch_ne1  = LoadPlane(channel, params, iy - 1, ix + 1);
  const float ch_sw1  = LoadPlane(channel, params, iy + 1, ix - 1);
  const float ch_se1  = LoadPlane(channel, params, iy + 1, ix + 1);

  const float ch_nw3  = LoadPlane(channel, params, iy - 3, ix - 3);
  const float ch_ne3  = LoadPlane(channel, params, iy - 3, ix + 3);
  const float ch_sw3  = LoadPlane(channel, params, iy + 3, ix - 3);
  const float ch_se3  = LoadPlane(channel, params, iy + 3, ix + 3);

  const float g_nw2   = LoadPlane(g, params, iy - 2, ix - 2);
  const float g_ne2   = LoadPlane(g, params, iy - 2, ix + 2);
  const float g_sw2   = LoadPlane(g, params, iy + 2, ix - 2);
  const float g_se2   = LoadPlane(g, params, iy + 2, ix + 2);

  const float NW_grad =
      Add(Add(Add(kEps, abs(ch_nw1 - ch_se1)), abs(ch_nw1 - ch_nw3)), abs(g_c - g_nw2));
  const float NE_grad =
      Add(Add(Add(kEps, abs(ch_ne1 - ch_sw1)), abs(ch_ne1 - ch_ne3)), abs(g_c - g_ne2));
  const float SW_grad =
      Add(Add(Add(kEps, abs(ch_sw1 - ch_ne1)), abs(ch_sw1 - ch_sw3)), abs(g_c - g_sw2));
  const float SE_grad =
      Add(Add(Add(kEps, abs(ch_se1 - ch_nw1)), abs(ch_se1 - ch_se3)), abs(g_c - g_se2));

  const float g_nw1   = LoadPlane(g, params, iy - 1, ix - 1);
  const float g_ne1   = LoadPlane(g, params, iy - 1, ix + 1);
  const float g_sw1   = LoadPlane(g, params, iy + 1, ix - 1);
  const float g_se1   = LoadPlane(g, params, iy + 1, ix + 1);

  const float NW_est  = ch_nw1 - g_nw1;
  const float NE_est  = ch_ne1 - g_ne1;
  const float SW_est  = ch_sw1 - g_sw1;
  const float SE_est  = ch_se1 - g_se1;

  const float P_est   = (Mul(NW_grad, SE_est) + Mul(SE_grad, NW_est)) / (NW_grad + SE_grad);
  const float Q_est   = (Mul(NE_grad, SW_est) + Mul(SW_grad, NE_est)) / (NE_grad + SW_grad);
  const float out_val =
      ClampRcdOutput(Add(Add(g_c, Mul(1.f - PQ_disc, P_est)), Mul(PQ_disc, Q_est)));

  if (c == 0u) {
    push.r[y * params.stride + x] = out_val;
  } else {
    push.b[y * params.stride + x] = out_val;
  }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void rcd_rb_at_g(uint3 id: SV_DispatchThreadID, uniform RcdPushConstants push) {
  const SinglePlaneParams params = push.params;
  const uint              x      = id.x;
  const uint              y      = id.y;
  if (x >= params.width || y >= params.height || !InsideBorder(params, x, y) ||
      FC(params, y, x) != 1u) {
    return;
  }

  const int   ix         = int(x);
  const int   iy         = int(y);
  float*      vh_dir     = push.vh;
  float*      g          = push.g;

  const float VH_central = LoadPlane(vh_dir, params, iy, ix);
  const float VH_neigh = Mul(
      0.25f, Add(Add(Add(LoadPlane(vh_dir, params, iy - 1, ix - 1),
                         LoadPlane(vh_dir, params, iy - 1, ix + 1)),
                     LoadPlane(vh_dir, params, iy + 1, ix - 1)),
                 LoadPlane(vh_dir, params, iy + 1, ix + 1)));
  const float VH_disc =
      (abs(0.5f - VH_central) < abs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const float g_c  = LoadPlane(g, params, iy, ix);
  const float g_m2 = LoadPlane(g, params, iy - 2, ix);
  const float g_p2 = LoadPlane(g, params, iy + 2, ix);
  const float g_l2 = LoadPlane(g, params, iy, ix - 2);
  const float g_r2 = LoadPlane(g, params, iy, ix + 2);
  const float g_m1 = LoadPlane(g, params, iy - 1, ix);
  const float g_p1 = LoadPlane(g, params, iy + 1, ix);
  const float g_l1 = LoadPlane(g, params, iy, ix - 1);
  const float g_r1 = LoadPlane(g, params, iy, ix + 1);

  const uint  index = y * params.stride + x;
  // Both reads use the non-green neighbors of the plane; this kernel writes only green texels.
  const float r_out = ReconstructRbAtGreen(push.r, params, iy, ix, g_c, g_m2, g_p2, g_l2, g_r2,
                                           g_m1, g_p1, g_l1, g_r1, VH_disc);
  const float b_out = ReconstructRbAtGreen(push.b, params, iy, ix, g_c, g_m2, g_p2, g_l2, g_r2,
                                           g_m1, g_p1, g_l1, g_r1, VH_disc);
  push.r[index]     = r_out;
  push.b[index]     = b_out;
}
