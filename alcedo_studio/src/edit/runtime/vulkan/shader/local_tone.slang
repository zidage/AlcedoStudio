//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group local_tone (plan #1036, phase VK10): the kernels of
// opencl/shader/local_tone.cl that the Local Laplacian stage of a Color Grade (Shadows,
// Highlights) dispatches, and three kernels on device addresses for the zero fill of a plane and
// the copies between a canonical R32F image and a plane. The names, the bindings, and the push
// constant layouts are in include/vulkan/vulkan_local_tone_kernels.hpp.
//
// A plane is an R32F row-major range of the transient arena; a kernel reads it by its device
// address. A scene image is a cached RGBA32F storage image or a scene-work member (an RGBA32F
// row-major buffer): a kernel reads the buffer when its address is not 0, else the image of its
// binding.

#include "color/color_encoding_math.h"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> scene_image;
[[vk::binding(1, 0)]] [vk::image_format("r8")] RWTexture2D<float> mask_image;
[[vk::binding(2, 0)]] [vk::image_format("r32f")] RWTexture2D<float> plane_image;

// ExtractPushConstants: 32 bytes.
struct ExtractPushConstants {
  uint64_t source;  // float4 row-major, or 0: scene_image
  uint64_t output;  // float plane
  int      input_width;
  int      input_height;
  int      output_width;
  int      output_height;
};

// ExtractReferencePushConstants: 64 bytes.
struct ExtractReferencePushConstants {
  uint64_t source;  // float4 row-major, or 0: scene_image
  uint64_t output;  // float plane
  int      input_width;
  int      input_height;
  int      output_width;
  int      output_height;
  float    full_ref_w;
  float    full_ref_h;
  float    reference_to_render[6];  // the first two rows of the 3 x 3 matrix
};

// PyramidDownPushConstants: 32 bytes.
struct PyramidDownPushConstants {
  uint64_t source;
  uint64_t output;
  int      src_width;
  int      src_height;
  int      dst_width;
  int      dst_height;
};

// RemapPushConstants: 48 bytes.
struct RemapPushConstants {
  uint64_t source;
  uint64_t output;
  int      width;
  int      height;
  float    gamma;
  float    target;
  float    beta;
  float    alpha;
  float    sigma;
  int      pad;
};

// SelectPushConstants: 88 bytes.
struct SelectPushConstants {
  uint64_t source;
  uint64_t lo;
  uint64_t lo_coarse;
  uint64_t hi;
  uint64_t hi_coarse;
  uint64_t output;
  int      width;
  int      height;
  int      coarse_width;
  int      coarse_height;
  float    gamma_lo;
  float    gamma_hi;
  int      first;
  int      last;
  int      top;
  int      pad;
};

// CollapsePushConstants: 40 bytes.
struct CollapsePushConstants {
  uint64_t laplacian;
  uint64_t coarse;
  uint64_t output;
  int      width;
  int      height;
  int      coarse_width;
  int      coarse_height;
};

// ApplyPushConstants: 88 bytes.
struct ApplyPushConstants {
  uint64_t working;     // float4 row-major (a scene-work member)
  uint64_t mix_source;  // float4 row-major, or 0: scene_image
  uint64_t reference;   // float plane
  uint64_t adjusted;    // float plane
  int      width;
  int      height;
  int      adjusted_width;
  int      adjusted_height;
  float    render_to_uv[6];  // the first two rows of the 3 x 3 matrix
  int      apply_mix;        // 0: no mix; 1: the mix with the mix source
  int      has_mask;         // 1: the mix is multiplied by mask_image
  float    grade_mix;
  int      pad;
};

// FillPushConstants: 16 bytes.
struct FillPushConstants {
  uint64_t plane;
  uint     count;
  uint     row_texels;  // the texels of one row of the dispatch
};

// PlanePushConstants: 16 bytes (the two copies).
struct PlanePushConstants {
  uint64_t plane;
  int      width;
  int      height;
};

static float*  FloatAt(uint64_t address) { return (float*)address; }
static float4* Float4At(uint64_t address) { return (float4*)address; }

static float   Ap1Intensity(float4 pixel) {
  return 0.272229f * pixel.x + 0.674082f * pixel.y + 0.053689f * pixel.z;
}

static float LogIntensity(float4 acescc) {
  const float4 linear = float4(CeAcesccDecode(acescc.x), CeAcesccDecode(acescc.y),
                               CeAcesccDecode(acescc.z), acescc.w);
  return CeAcesccEncode(max(Ap1Intensity(linear), 1.0e-6f));
}

// The OpenCL mix(a, b, t).
static float4 Mix4(float4 a, float4 b, float t) { return a + (b - a) * t; }

static float4 SceneReadRgbaAt(uint64_t buffer, int2 coord, int width, int height) {
  const int x = clamp(coord.x, 0, width - 1);
  const int y = clamp(coord.y, 0, height - 1);
  if (buffer != 0u) {
    return Float4At(buffer)[uint(y) * uint(width) + uint(x)];
  }
  return scene_image[uint2(uint(x), uint(y))];
}

static float4 SceneReadRgbaBilinear(uint64_t buffer, int width, int height, float x, float y) {
  x              = min(max(x, 0.0f), float(width - 1));
  y              = min(max(y, 0.0f), float(height - 1));
  const int   x0 = int(floor(x));
  const int   y0 = int(floor(y));
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - float(x0);
  const float ty = y - float(y0);
  const float4 a = SceneReadRgbaAt(buffer, int2(x0, y0), width, height);
  const float4 b = SceneReadRgbaAt(buffer, int2(x1, y0), width, height);
  const float4 c = SceneReadRgbaAt(buffer, int2(x0, y1), width, height);
  const float4 d = SceneReadRgbaAt(buffer, int2(x1, y1), width, height);
  return Mix4(Mix4(a, b, tx), Mix4(c, d, tx), ty);
}

static float PlaneRead(uint64_t plane, int x, int y, int width, int height) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return FloatAt(plane)[uint(y) * uint(width) + uint(x)];
}

static float Weight(int tap) {
  return (tap == -2 || tap == 2) ? 1.0f / 16.0f
       : (tap == -1 || tap == 1) ? 4.0f / 16.0f
                                 : 6.0f / 16.0f;
}

// The expand of the coarser level at (x, y). A coordinate is halved only when it is even, so the
// arithmetic shift equals the OpenCL division and stays correct for a negative coordinate.
static float Expand(uint64_t coarse, int coarse_width, int coarse_height, int x, int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) {
      continue;
    }
    const int cy = min(max(sample_y >> 1, 0), coarse_height - 1);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) {
        continue;
      }
      const int cx = min(max(sample_x >> 1, 0), coarse_width - 1);
      sum += 4.0f * Weight(kx) * Weight(ky) *
             FloatAt(coarse)[uint(cy) * uint(coarse_width) + uint(cx)];
    }
  }
  return sum;
}

static float RemapDelta(float delta, float sigma, float alpha, float beta) {
  const float magnitude = abs(delta);
  if (magnitude <= 1.0e-6f) {
    return 0.0f;
  }
  const float sign = delta < 0.0f ? -1.0f : 1.0f;
  if (magnitude <= sigma) {
    return sign * sigma * AlcedoPow(min(magnitude / max(sigma, 1.0e-6f), 1.0f), alpha);
  }
  return sign * (sigma + beta * (magnitude - sigma));
}

static float2 Transform(float matrix[6], float x, float y) {
  return float2(matrix[0] * x + matrix[1] * y + matrix[2],
                matrix[3] * x + matrix[4] * y + matrix[5]);
}

static float Bilinear(uint64_t plane, int width, int height, float x, float y) {
  x                     = min(max(x, 0.0f), float(width - 1));
  y                     = min(max(y, 0.0f), float(height - 1));
  const int   x0        = int(floor(x));
  const int   y0        = int(floor(y));
  const int   x1        = min(x0 + 1, width - 1);
  const int   y1        = min(y0 + 1, height - 1);
  const float tx        = x - float(x0);
  const float ty        = y - float(y0);
  float*      values    = FloatAt(plane);
  const uint  row0      = uint(y0) * uint(width);
  const uint  row1      = uint(y1) * uint(width);
  const float a         = values[row0 + uint(x0)] + (values[row0 + uint(x1)] - values[row0 + uint(x0)]) * tx;
  const float b         = values[row1 + uint(x0)] + (values[row1 + uint(x1)] - values[row1 + uint(x0)]) * tx;
  return a + (b - a) * ty;
}

// The log intensity of the bilinear read of the scene image, resized to the plane.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_extract_scene(uint3 id: SV_DispatchThreadID, uniform ExtractPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.output_width || y >= push.output_height) {
    return;
  }
  const float sx =
      (float(x) + 0.5f) * float(push.input_width) / float(push.output_width) - 0.5f;
  const float sy =
      (float(y) + 0.5f) * float(push.input_height) / float(push.output_height) - 0.5f;
  FloatAt(push.output)[uint(y) * uint(push.output_width) + uint(x)] = LogIntensity(
      SceneReadRgbaBilinear(push.source, push.input_width, push.input_height, sx, sy));
}

// The log intensity of the canonical plane: each plane texel maps through the reference-to-render
// transform to the scene image.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_extract_reference_scene(uint3 id: SV_DispatchThreadID,
                                        uniform ExtractReferencePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.output_width || y >= push.output_height) {
    return;
  }
  const float  u      = (float(x) + 0.5f) / float(push.output_width);
  const float  v      = (float(y) + 0.5f) / float(push.output_height);
  const float2 source = Transform(push.reference_to_render, u * push.full_ref_w, v * push.full_ref_h);
  FloatAt(push.output)[uint(y) * uint(push.output_width) + uint(x)] =
      LogIntensity(SceneReadRgbaBilinear(push.source, push.input_width, push.input_height,
                                         source.x - 0.5f, source.y - 0.5f));
}

// One level down the Gaussian pyramid: the 5 x 5 binomial filter at the even source texels.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_pyr_down(uint3 id: SV_DispatchThreadID, uniform PyramidDownPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.dst_width || y >= push.dst_height) {
    return;
  }
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    for (int kx = -2; kx <= 2; ++kx) {
      sum += Weight(kx) * Weight(ky) *
             PlaneRead(push.source, x * 2 + kx, y * 2 + ky, push.src_width, push.src_height);
    }
  }
  FloatAt(push.output)[uint(y) * uint(push.dst_width) + uint(x)] = sum;
}

// The remap of the source plane around one sample gamma.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_remap(uint3 id: SV_DispatchThreadID, uniform RemapPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const uint index = uint(y) * uint(push.width) + uint(x);
  FloatAt(push.output)[index] =
      push.target +
      RemapDelta(FloatAt(push.source)[index] - push.gamma, push.sigma, push.alpha, push.beta);
}

// The Laplacian of the two samples around the source value, interpolated by the source value;
// written only where the source value lies between the two gammas (or beyond the first or the
// last sample).
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_select(uint3 id: SV_DispatchThreadID, uniform SelectPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const uint  index = uint(y) * uint(push.width) + uint(x);
  const float value = FloatAt(push.source)[index];
  if (!((push.first != 0 && value <= push.gamma_hi) || (push.last != 0 && value >= push.gamma_lo) ||
        (value >= push.gamma_lo && value < push.gamma_hi))) {
    return;
  }
  const float t =
      min(max((value - push.gamma_lo) / max(push.gamma_hi - push.gamma_lo, 1.0e-6f), 0.0f), 1.0f);
  const float lo = FloatAt(push.lo)[index];
  const float hi = FloatAt(push.hi)[index];
  if (push.top != 0) {
    FloatAt(push.output)[index] = lo + (hi - lo) * t;
    return;
  }
  const float lap_lo =
      lo - Expand(push.lo_coarse, push.coarse_width, push.coarse_height, x, y);
  const float lap_hi =
      hi - Expand(push.hi_coarse, push.coarse_width, push.coarse_height, x, y);
  FloatAt(push.output)[index] = lap_lo + (lap_hi - lap_lo) * t;
}

// One level up the Laplacian pyramid: the level plus the expand of the coarser result.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_collapse(uint3 id: SV_DispatchThreadID, uniform CollapsePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const uint index = uint(y) * uint(push.width) + uint(x);
  FloatAt(push.output)[index] =
      FloatAt(push.laplacian)[index] +
      Expand(push.coarse, push.coarse_width, push.coarse_height, x, y);
}

// The tone of the working texel by the ratio of the adjusted and the reference log intensity,
// the gamut scale toward the target intensity, then the optional (masked) mix with the mix source.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_apply_scene(uint3 id: SV_DispatchThreadID, uniform ApplyPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const float2 uv = Transform(push.render_to_uv, float(x) + 0.5f, float(y) + 0.5f);
  const float  ax = uv.x * float(push.adjusted_width) - 0.5f;
  const float  ay = uv.y * float(push.adjusted_height) - 0.5f;
  const float  reference_l =
      Bilinear(push.reference, push.adjusted_width, push.adjusted_height, ax, ay);
  const float adjusted_l =
      Bilinear(push.adjusted, push.adjusted_width, push.adjusted_height, ax, ay);
  const uint   index            = uint(y) * uint(push.width) + uint(x);
  const float4 pixel            = Float4At(push.working)[index];
  const float  source_l         = LogIntensity(pixel);
  const float  source_intensity = max(CeAcesccDecode(source_l), 1.0e-5f);
  const float  target_intensity = CeAcesccDecode(source_l + adjusted_l - reference_l);
  const float  ratio            = min(max(target_intensity / source_intensity, 0.0f), 32.0f);
  float        r                = CeAcesccDecode(pixel.x) * ratio;
  float        g                = CeAcesccDecode(pixel.y) * ratio;
  float        b                = CeAcesccDecode(pixel.z) * ratio;
  const float  kLower           = -1.0e-5f;
  float        gamut_scale      = 1.0f;
  if (r < kLower && target_intensity > r) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - r));
  }
  if (g < kLower && target_intensity > g) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - g));
  }
  if (b < kLower && target_intensity > b) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - b));
  }
  gamut_scale = min(max(gamut_scale, 0.0f), 1.0f);
  r           = target_intensity + (r - target_intensity) * gamut_scale;
  g           = target_intensity + (g - target_intensity) * gamut_scale;
  b           = target_intensity + (b - target_intensity) * gamut_scale;
  float4 tone = float4(CeAcesccEncode(r), CeAcesccEncode(g), CeAcesccEncode(b), pixel.w);
  if (push.apply_mix != 0) {
    const float4 original = SceneReadRgbaAt(push.mix_source, int2(x, y), push.width, push.height);
    float        mix      = push.grade_mix;
    if (push.has_mask != 0) {
      mix *= mask_image[uint2(uint(x), uint(y))];
    }
    mix  = clamp(mix, 0.0f, 1.0f);
    tone = float4(original.xyz + (tone.xyz - original.xyz) * mix, original.w);
  }
  Float4At(push.working)[index] = tone;
}

// Zero of each texel of the plane (OpenClBackend::FillDeviceMemory with 0), in rows of
// row_texels texels.
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_fill_zero(uint3 id: SV_DispatchThreadID, uniform FillPushConstants push) {
  const uint index = id.y * push.row_texels + id.x;
  if (id.x >= push.row_texels || index >= push.count) {
    return;
  }
  FloatAt(push.plane)[index] = 0.0f;
}

// The R32F image at binding 2 into the plane (OpenClBackend::CopyImageToDeviceMemory).
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_copy_image_to_plane(uint3 id: SV_DispatchThreadID, uniform PlanePushConstants push) {
  if (int(id.x) >= push.width || int(id.y) >= push.height) {
    return;
  }
  FloatAt(push.plane)[id.y * uint(push.width) + id.x] = plane_image[id.xy];
}

// The plane into the R32F image at binding 2 (OpenClBackend::CopyDeviceMemoryToImage).
[shader("compute")]
[numthreads(16, 16, 1)]
void local_tone_copy_plane_to_image(uint3 id: SV_DispatchThreadID, uniform PlanePushConstants push) {
  if (int(id.x) >= push.width || int(id.y) >= push.height) {
    return;
  }
  plane_image[id.xy] = FloatAt(push.plane)[id.y * uint(push.width) + id.x];
}
