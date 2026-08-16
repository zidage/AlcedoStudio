//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct SinglePlaneParams {
  uint width;
  uint height;
  uint rgb_fc[4];
};

constant float kEps   = 1e-5f;
constant float kEpsSq = 1e-10f;

static inline uint FC(constant SinglePlaneParams& params, uint y, uint x) {
  return params.rgb_fc[((y & 1u) << 1u) | (x & 1u)];
}

static inline float LoadTex(texture2d<float, access::read> plane, int y, int x) {
  return plane.read(uint2(static_cast<uint>(x), static_cast<uint>(y))).r;
}

static inline float LoadTexRW(texture2d<float, access::read_write> plane, int y, int x) {
  return plane.read(uint2(static_cast<uint>(x), static_cast<uint>(y))).r;
}

static inline float LowPassAt(texture2d<float, access::read> raw, int y, int x) {
  const float c  = LoadTex(raw, y, x);
  const float n  = LoadTex(raw, y - 1, x);
  const float s  = LoadTex(raw, y + 1, x);
  const float w  = LoadTex(raw, y, x - 1);
  const float e  = LoadTex(raw, y, x + 1);
  const float nw = LoadTex(raw, y - 1, x - 1);
  const float ne = LoadTex(raw, y - 1, x + 1);
  const float sw = LoadTex(raw, y + 1, x - 1);
  const float se = LoadTex(raw, y + 1, x + 1);

  return 0.25f * c + 0.125f * (n + s + w + e) + 0.0625f * (nw + ne + sw + se);
}

static inline float ReconstructRbAtGreen(texture2d<float, access::read_write> channel, int y, int x,
                                         float g_c, float g_m2, float g_p2, float g_l2, float g_r2,
                                         float g_m1, float g_p1, float g_l1, float g_r1,
                                         float vh_disc) {
  const float ch_m1 = LoadTexRW(channel, y - 1, x);
  const float ch_p1 = LoadTexRW(channel, y + 1, x);
  const float ch_m3 = LoadTexRW(channel, y - 3, x);
  const float ch_p3 = LoadTexRW(channel, y + 3, x);
  const float ch_l1 = LoadTexRW(channel, y, x - 1);
  const float ch_r1 = LoadTexRW(channel, y, x + 1);
  const float ch_l3 = LoadTexRW(channel, y, x - 3);
  const float ch_r3 = LoadTexRW(channel, y, x + 3);

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
  return max(0.f, g_c + (1.f - vh_disc) * V_est + vh_disc * H_est);
}

kernel void rcd_init_and_vh(texture2d<float, access::read>  raw [[texture(0)]],
                            texture2d<float, access::write> r [[texture(1)]],
                            texture2d<float, access::write> g [[texture(2)]],
                            texture2d<float, access::write> b [[texture(3)]],
                            texture2d<float, access::write> vh_dir [[texture(4)]],
                            constant SinglePlaneParams&     params [[buffer(0)]],
                            uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float val  = raw.read(gid).r;
  const uint  color = FC(params, gid.y, gid.x);

  r.write((color == 0u) ? val : 0.0f, gid);
  g.write((color == 1u) ? val : 0.0f, gid);
  b.write((color == 2u) ? val : 0.0f, gid);

  float vh = 0.0f;
  if (gid.x >= 4u && gid.y >= 4u && gid.x + 4u < params.width && gid.y + 4u < params.height) {
    const int   x   = static_cast<int>(gid.x);
    const int   y   = static_cast<int>(gid.y);
    const float c   = val;

    const float vm1 = LoadTex(raw, y - 1, x);
    const float vp1 = LoadTex(raw, y + 1, x);
    const float vm2 = LoadTex(raw, y - 2, x);
    const float vp2 = LoadTex(raw, y + 2, x);
    const float vm3 = LoadTex(raw, y - 3, x);
    const float vp3 = LoadTex(raw, y + 3, x);
    const float vm4 = LoadTex(raw, y - 4, x);
    const float vp4 = LoadTex(raw, y + 4, x);

    const float hm1 = LoadTex(raw, y, x - 1);
    const float hp1 = LoadTex(raw, y, x + 1);
    const float hm2 = LoadTex(raw, y, x - 2);
    const float hp2 = LoadTex(raw, y, x + 2);
    const float hm3 = LoadTex(raw, y, x - 3);
    const float hp3 = LoadTex(raw, y, x + 3);
    const float hm4 = LoadTex(raw, y, x - 4);
    const float hp4 = LoadTex(raw, y, x + 4);

    const float V_stat = max(
        -18.f * c * vm1 - 18.f * c * vp1 - 36.f * c * vm2 - 36.f * c * vp2 + 18.f * c * vm3 +
            18.f * c * vp3 - 2.f * c * vm4 - 2.f * c * vp4 + 38.f * c * c - 70.f * vm1 * vp1 -
            12.f * vm1 * vm2 + 24.f * vm1 * vp2 - 38.f * vm1 * vm3 + 16.f * vm1 * vp3 +
            12.f * vm1 * vm4 - 6.f * vm1 * vp4 + 46.f * vm1 * vm1 + 24.f * vp1 * vm2 -
            12.f * vp1 * vp2 + 16.f * vp1 * vm3 - 38.f * vp1 * vp3 - 6.f * vp1 * vm4 +
            12.f * vp1 * vp4 + 46.f * vp1 * vp1 + 14.f * vm2 * vp2 - 12.f * vm2 * vp3 -
            2.f * vm2 * vm4 + 2.f * vm2 * vp4 + 11.f * vm2 * vm2 - 12.f * vp2 * vm3 +
            2.f * vp2 * vm4 - 2.f * vp2 * vp4 + 11.f * vp2 * vp2 + 2.f * vm3 * vp3 -
            6.f * vm3 * vm4 + 10.f * vm3 * vm3 - 6.f * vp3 * vp4 + 10.f * vp3 * vp3 +
            1.f * vm4 * vm4 + 1.f * vp4 * vp4,
        kEpsSq);

    const float H_stat = max(
        -18.f * c * hm1 - 18.f * c * hp1 - 36.f * c * hm2 - 36.f * c * hp2 + 18.f * c * hm3 +
            18.f * c * hp3 - 2.f * c * hm4 - 2.f * c * hp4 + 38.f * c * c - 70.f * hm1 * hp1 -
            12.f * hm1 * hm2 + 24.f * hm1 * hp2 - 38.f * hm1 * hm3 + 16.f * hm1 * hp3 +
            12.f * hm1 * hm4 - 6.f * hm1 * hp4 + 46.f * hm1 * hm1 + 24.f * hp1 * hm2 -
            12.f * hp1 * hp2 + 16.f * hp1 * hm3 - 38.f * hp1 * hp3 - 6.f * hp1 * hm4 +
            12.f * hp1 * hp4 + 46.f * hp1 * hp1 + 14.f * hm2 * hp2 - 12.f * hm2 * hp3 -
            2.f * hm2 * hm4 + 2.f * hm2 * hp4 + 11.f * hm2 * hm2 - 12.f * hp2 * hm3 +
            2.f * hp2 * hm4 - 2.f * hp2 * hp4 + 11.f * hp2 * hp2 + 2.f * hm3 * hp3 -
            6.f * hm3 * hm4 + 10.f * hm3 * hm3 - 6.f * hp3 * hp4 + 10.f * hp3 * hp3 +
            1.f * hm4 * hm4 + 1.f * hp4 * hp4,
        kEpsSq);

    vh = V_stat / (V_stat + H_stat);
  }

  vh_dir.write(vh, gid);
}

kernel void rcd_green_at_rb(texture2d<float, access::read>       raw [[texture(0)]],
                            texture2d<float, access::read>       vh_dir [[texture(1)]],
                            texture2d<float, access::read_write> g [[texture(2)]],
                            constant SinglePlaneParams&          params [[buffer(0)]],
                            uint2                                gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height || gid.x < 4u || gid.y < 4u ||
      gid.x + 4u >= params.width || gid.y + 4u >= params.height) {
    return;
  }

  if (FC(params, gid.y, gid.x) == 1u) {
    return;
  }

  const int x = static_cast<int>(gid.x);
  const int y = static_cast<int>(gid.y);

  const float VH_central = LoadTex(vh_dir, y, x);
  const float VH_neigh   = 0.25f * (LoadTex(vh_dir, y - 1, x - 1) + LoadTex(vh_dir, y - 1, x + 1) +
                                  LoadTex(vh_dir, y + 1, x - 1) + LoadTex(vh_dir, y + 1, x + 1));
  const float VH_disc =
      (fabs(0.5f - VH_central) < fabs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const float c   = LoadTex(raw, y, x);
  const float vm1 = LoadTex(raw, y - 1, x);
  const float vp1 = LoadTex(raw, y + 1, x);
  const float vm2 = LoadTex(raw, y - 2, x);
  const float vp2 = LoadTex(raw, y + 2, x);
  const float vm3 = LoadTex(raw, y - 3, x);
  const float vp3 = LoadTex(raw, y + 3, x);
  const float vm4 = LoadTex(raw, y - 4, x);
  const float vp4 = LoadTex(raw, y + 4, x);

  const float hm1 = LoadTex(raw, y, x - 1);
  const float hp1 = LoadTex(raw, y, x + 1);
  const float hm2 = LoadTex(raw, y, x - 2);
  const float hp2 = LoadTex(raw, y, x + 2);
  const float hm3 = LoadTex(raw, y, x - 3);
  const float hp3 = LoadTex(raw, y, x + 3);
  const float hm4 = LoadTex(raw, y, x - 4);
  const float hp4 = LoadTex(raw, y, x + 4);

  const float lpf_c  = LowPassAt(raw, y, x);
  const float lpf_n2 = LowPassAt(raw, y - 2, x);
  const float lpf_s2 = LowPassAt(raw, y + 2, x);
  const float lpf_w2 = LowPassAt(raw, y, x - 2);
  const float lpf_e2 = LowPassAt(raw, y, x + 2);

  const float N_grad = kEps + fabs(vm1 - vp1) + fabs(c - vm2) + fabs(vm1 - vm3) + fabs(vm2 - vm4);
  const float S_grad = kEps + fabs(vp1 - vm1) + fabs(c - vp2) + fabs(vp1 - vp3) + fabs(vp2 - vp4);
  const float W_grad = kEps + fabs(hm1 - hp1) + fabs(c - hm2) + fabs(hm1 - hm3) + fabs(hm2 - hm4);
  const float E_grad = kEps + fabs(hp1 - hm1) + fabs(c - hp2) + fabs(hp1 - hp3) + fabs(hp2 - hp4);

  const float N_est = vm1 * (1.f + (lpf_c - lpf_n2) / (kEps + lpf_c + lpf_n2));
  const float S_est = vp1 * (1.f + (lpf_c - lpf_s2) / (kEps + lpf_c + lpf_s2));
  const float W_est = hm1 * (1.f + (lpf_c - lpf_w2) / (kEps + lpf_c + lpf_w2));
  const float E_est = hp1 * (1.f + (lpf_c - lpf_e2) / (kEps + lpf_c + lpf_e2));

  const float V_est = (S_grad * N_est + N_grad * S_est) / (N_grad + S_grad);
  const float H_est = (W_grad * E_est + E_grad * W_est) / (E_grad + W_grad);

  g.write(max(VH_disc * H_est + (1.f - VH_disc) * V_est, 0.f), gid);
}

kernel void rcd_pq_dir(texture2d<float, access::read>  raw [[texture(0)]],
                       texture2d<float, access::write> pq_dir [[texture(1)]],
                       constant SinglePlaneParams&     params [[buffer(0)]],
                       uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  float pq = 0.0f;
  if (gid.x >= 4u && gid.y >= 4u && gid.x + 4u < params.width && gid.y + 4u < params.height &&
      FC(params, gid.y, gid.x) != 1u) {
    const int   x = static_cast<int>(gid.x);
    const int   y = static_cast<int>(gid.y);
    const float c = LoadTex(raw, y, x);

    const float nw1 = LoadTex(raw, y - 1, x - 1);
    const float se1 = LoadTex(raw, y + 1, x + 1);
    const float nw2 = LoadTex(raw, y - 2, x - 2);
    const float se2 = LoadTex(raw, y + 2, x + 2);
    const float nw3 = LoadTex(raw, y - 3, x - 3);
    const float se3 = LoadTex(raw, y + 3, x + 3);
    const float nw4 = LoadTex(raw, y - 4, x - 4);
    const float se4 = LoadTex(raw, y + 4, x + 4);

    const float sw1 = LoadTex(raw, y + 1, x - 1);
    const float ne1 = LoadTex(raw, y - 1, x + 1);
    const float sw2 = LoadTex(raw, y + 2, x - 2);
    const float ne2 = LoadTex(raw, y - 2, x + 2);
    const float sw3 = LoadTex(raw, y + 3, x - 3);
    const float ne3 = LoadTex(raw, y - 3, x + 3);
    const float sw4 = LoadTex(raw, y + 4, x - 4);
    const float ne4 = LoadTex(raw, y - 4, x + 4);

    const float P_stat = max(
        -18.f * c * nw1 - 18.f * c * se1 - 36.f * c * nw2 - 36.f * c * se2 + 18.f * c * nw3 +
            18.f * c * se3 - 2.f * c * nw4 - 2.f * c * se4 + 38.f * c * c - 70.f * nw1 * se1 -
            12.f * nw1 * nw2 + 24.f * nw1 * se2 - 38.f * nw1 * nw3 + 16.f * nw1 * se3 +
            12.f * nw1 * nw4 - 6.f * nw1 * se4 + 46.f * nw1 * nw1 + 24.f * se1 * nw2 -
            12.f * se1 * se2 + 16.f * se1 * nw3 - 38.f * se1 * se3 - 6.f * se1 * nw4 +
            12.f * se1 * se4 + 46.f * se1 * se1 + 14.f * nw2 * se2 - 12.f * nw2 * se3 -
            2.f * nw2 * nw4 + 2.f * nw2 * se4 + 11.f * nw2 * nw2 - 12.f * se2 * nw3 +
            2.f * se2 * nw4 - 2.f * se2 * se4 + 11.f * se2 * se2 + 2.f * nw3 * se3 -
            6.f * nw3 * nw4 + 10.f * nw3 * nw3 - 6.f * se3 * se4 + 10.f * se3 * se3 +
            1.f * nw4 * nw4 + 1.f * se4 * se4,
        kEpsSq);

    const float Q_stat = max(
        -18.f * c * sw1 - 18.f * c * ne1 - 36.f * c * sw2 - 36.f * c * ne2 + 18.f * c * sw3 +
            18.f * c * ne3 - 2.f * c * sw4 - 2.f * c * ne4 + 38.f * c * c - 70.f * sw1 * ne1 -
            12.f * sw1 * sw2 + 24.f * sw1 * ne2 - 38.f * sw1 * sw3 + 16.f * sw1 * ne3 +
            12.f * sw1 * sw4 - 6.f * sw1 * ne4 + 46.f * sw1 * sw1 + 24.f * ne1 * sw2 -
            12.f * ne1 * ne2 + 16.f * ne1 * sw3 - 38.f * ne1 * ne3 - 6.f * ne1 * sw4 +
            12.f * ne1 * ne4 + 46.f * ne1 * ne1 + 14.f * sw2 * ne2 - 12.f * sw2 * ne3 -
            2.f * sw2 * sw4 + 2.f * sw2 * ne4 + 11.f * sw2 * sw2 - 12.f * ne2 * sw3 +
            2.f * ne2 * sw4 - 2.f * ne2 * ne4 + 11.f * ne2 * ne2 + 2.f * sw3 * ne3 -
            6.f * sw3 * sw4 + 10.f * sw3 * sw3 - 6.f * ne3 * ne4 + 10.f * ne3 * ne3 +
            1.f * sw4 * sw4 + 1.f * ne4 * ne4,
        kEpsSq);

    pq = P_stat / (P_stat + Q_stat);
  }

  pq_dir.write(pq, gid);
}

kernel void rcd_rb_at_rb(texture2d<float, access::read>       pq_dir [[texture(0)]],
                         texture2d<float, access::read>       g [[texture(1)]],
                         texture2d<float, access::read_write> r [[texture(2)]],
                         texture2d<float, access::read_write> b [[texture(3)]],
                         constant SinglePlaneParams&          params [[buffer(0)]],
                         uint2                                gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height || gid.x < 4u || gid.y < 4u ||
      gid.x + 4u >= params.width || gid.y + 4u >= params.height) {
    return;
  }

  const uint color = FC(params, gid.y, gid.x);
  if (color == 1u) {
    return;
  }

  const int  x = static_cast<int>(gid.x);
  const int  y = static_cast<int>(gid.y);
  const uint c = 2u - color;

  const float PQ_c = LoadTex(pq_dir, y, x);
  const float PQ_n = 0.25f * (LoadTex(pq_dir, y - 1, x - 1) + LoadTex(pq_dir, y - 1, x + 1) +
                              LoadTex(pq_dir, y + 1, x - 1) + LoadTex(pq_dir, y + 1, x + 1));
  const float PQ_disc = (fabs(0.5f - PQ_c) < fabs(0.5f - PQ_n)) ? PQ_n : PQ_c;

  const float g_c = LoadTex(g, y, x);

  const float ch_nw1 = (c == 0u) ? LoadTexRW(r, y - 1, x - 1) : LoadTexRW(b, y - 1, x - 1);
  const float ch_ne1 = (c == 0u) ? LoadTexRW(r, y - 1, x + 1) : LoadTexRW(b, y - 1, x + 1);
  const float ch_sw1 = (c == 0u) ? LoadTexRW(r, y + 1, x - 1) : LoadTexRW(b, y + 1, x - 1);
  const float ch_se1 = (c == 0u) ? LoadTexRW(r, y + 1, x + 1) : LoadTexRW(b, y + 1, x + 1);

  const float ch_nw3 = (c == 0u) ? LoadTexRW(r, y - 3, x - 3) : LoadTexRW(b, y - 3, x - 3);
  const float ch_ne3 = (c == 0u) ? LoadTexRW(r, y - 3, x + 3) : LoadTexRW(b, y - 3, x + 3);
  const float ch_sw3 = (c == 0u) ? LoadTexRW(r, y + 3, x - 3) : LoadTexRW(b, y + 3, x - 3);
  const float ch_se3 = (c == 0u) ? LoadTexRW(r, y + 3, x + 3) : LoadTexRW(b, y + 3, x + 3);

  const float g_nw2 = LoadTex(g, y - 2, x - 2);
  const float g_ne2 = LoadTex(g, y - 2, x + 2);
  const float g_sw2 = LoadTex(g, y + 2, x - 2);
  const float g_se2 = LoadTex(g, y + 2, x + 2);

  const float NW_grad = kEps + fabs(ch_nw1 - ch_se1) + fabs(ch_nw1 - ch_nw3) + fabs(g_c - g_nw2);
  const float NE_grad = kEps + fabs(ch_ne1 - ch_sw1) + fabs(ch_ne1 - ch_ne3) + fabs(g_c - g_ne2);
  const float SW_grad = kEps + fabs(ch_sw1 - ch_ne1) + fabs(ch_sw1 - ch_sw3) + fabs(g_c - g_sw2);
  const float SE_grad = kEps + fabs(ch_se1 - ch_nw1) + fabs(ch_se1 - ch_se3) + fabs(g_c - g_se2);

  const float g_nw1 = LoadTex(g, y - 1, x - 1);
  const float g_ne1 = LoadTex(g, y - 1, x + 1);
  const float g_sw1 = LoadTex(g, y + 1, x - 1);
  const float g_se1 = LoadTex(g, y + 1, x + 1);

  const float NW_est = ch_nw1 - g_nw1;
  const float NE_est = ch_ne1 - g_ne1;
  const float SW_est = ch_sw1 - g_sw1;
  const float SE_est = ch_se1 - g_se1;

  const float P_est   = (NW_grad * SE_est + SE_grad * NW_est) / (NW_grad + SE_grad);
  const float Q_est   = (NE_grad * SW_est + SW_grad * NE_est) / (NE_grad + SW_grad);
  const float out_val = max(0.f, g_c + (1.f - PQ_disc) * P_est + PQ_disc * Q_est);

  if (c == 0u) {
    r.write(out_val, gid);
  } else {
    b.write(out_val, gid);
  }
}

kernel void rcd_rb_at_g(texture2d<float, access::read>       vh_dir [[texture(0)]],
                        texture2d<float, access::read>       g [[texture(1)]],
                        texture2d<float, access::read_write> r [[texture(2)]],
                        texture2d<float, access::read_write> b [[texture(3)]],
                        constant SinglePlaneParams&          params [[buffer(0)]],
                        uint2                                gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height || gid.x < 4u || gid.y < 4u ||
      gid.x + 4u >= params.width || gid.y + 4u >= params.height ||
      FC(params, gid.y, gid.x) != 1u) {
    return;
  }

  const int x = static_cast<int>(gid.x);
  const int y = static_cast<int>(gid.y);

  const float VH_central = LoadTex(vh_dir, y, x);
  const float VH_neigh   = 0.25f * (LoadTex(vh_dir, y - 1, x - 1) + LoadTex(vh_dir, y - 1, x + 1) +
                                  LoadTex(vh_dir, y + 1, x - 1) + LoadTex(vh_dir, y + 1, x + 1));
  const float VH_disc =
      (fabs(0.5f - VH_central) < fabs(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const float g_c  = LoadTex(g, y, x);
  const float g_m2 = LoadTex(g, y - 2, x);
  const float g_p2 = LoadTex(g, y + 2, x);
  const float g_l2 = LoadTex(g, y, x - 2);
  const float g_r2 = LoadTex(g, y, x + 2);
  const float g_m1 = LoadTex(g, y - 1, x);
  const float g_p1 = LoadTex(g, y + 1, x);
  const float g_l1 = LoadTex(g, y, x - 1);
  const float g_r1 = LoadTex(g, y, x + 1);

  r.write(ReconstructRbAtGreen(r, y, x, g_c, g_m2, g_p2, g_l2, g_r2, g_m1, g_p1, g_l1, g_r1,
                               VH_disc),
          gid);
  b.write(ReconstructRbAtGreen(b, y, x, g_c, g_m2, g_p2, g_l2, g_r2, g_m1, g_p1, g_l1, g_r1,
                               VH_disc),
          gid);
}

kernel void rcd_merge_rgba(texture2d<float, access::read>   r [[texture(0)]],
                           texture2d<float, access::read>   g [[texture(1)]],
                           texture2d<float, access::read>   b [[texture(2)]],
                           texture2d<float, access::write>  out_rgba [[texture(3)]],
                           constant SinglePlaneParams&      params [[buffer(0)]],
                           uint2                            gid [[thread_position_in_grid]]) {
  const uint out_width  = params.width - 8u;
  const uint out_height = params.height - 8u;
  if (gid.x >= out_width || gid.y >= out_height) {
    return;
  }

  const uint2 in_coord(gid.x + 4u, gid.y + 4u);
  out_rgba.write(float4(r.read(in_coord).r, g.read(in_coord).r, b.read(in_coord).r, 1.0f), gid);
}
