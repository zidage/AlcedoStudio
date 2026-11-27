//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group geometry_camera (plan #1036, phase VK7): geometry_resample_rgba32f and
// camera_color_acescc of opencl_shader/geometry_camera.cl, on RGBA32F storage images. The names,
// the bindings, and the push constant layouts are in
// include/vulkan/vulkan_geometry_camera_kernels.hpp.

#include "color/color_encoding_math.h"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/dng_profile_gpu_math.h"
#include "geometry_sample.slangh"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

// GeometryResamplePushConstants: 60 bytes, the OpenCL GeometryResampleParams.
struct GeometryResamplePushConstants {
  float m00;  // render_to_decoded, row-major 2 x 3
  float m01;
  float m02;
  float m10;
  float m11;
  float m12;
  uint  decoded_width;
  uint  decoded_height;
  uint  render_width;
  uint  render_height;
  float border_r;
  float border_g;
  float border_b;
  float border_a;
  uint  use_bicubic;
};

// CameraColorPushConstants: 24 bytes.
struct CameraColorPushConstants {
  float* camera_to_ap1;  // 9 values, row-major (CameraColorGpuParams)
  float* dng_profile;    // the PackDngProfileGpuData tables
  uint   width;
  uint   height;
};

// Each texel of the render samples the decoded image at render_to_decoded of its center.
[shader("compute")]
[numthreads(16, 16, 1)]
void geometry_resample_rgba32f(uint3 id: SV_DispatchThreadID,
                               uniform GeometryResamplePushConstants push) {
  if (id.x >= push.render_width || id.y >= push.render_height) {
    return;
  }
  const float  cx     = float(id.x) + 0.5f;
  const float  cy     = float(id.y) + 0.5f;
  const float  sx     = push.m00 * cx + push.m01 * cy + push.m02;
  const float  sy     = push.m10 * cx + push.m11 * cy + push.m12;
  const float4 border = float4(push.border_r, push.border_g, push.border_b, push.border_a);
  const int    width  = int(push.decoded_width);
  const int    height = int(push.decoded_height);
  const float4 pixel  = push.use_bicubic != 0u
                            ? BicubicSample(source, width, height, sx, sy, border)
                            : BilinearSample(source, width, height, sx, sy, border);
  destination[id.xy]  = pixel;
}

// Camera RGB to AP1, the DNG profile tables, the ACES reference gamut compression, ACEScc.
[shader("compute")]
[numthreads(16, 16, 1)]
void camera_color_acescc(uint3 id: SV_DispatchThreadID, uniform CameraColorPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const float4 value = source[id.xy];
  float*       m     = push.camera_to_ap1;
  const float  x     = m[0] * value.x + m[1] * value.y + m[2] * value.z;
  const float  y     = m[3] * value.x + m[4] * value.y + m[5] * value.z;
  const float  z     = m[6] * value.x + m[7] * value.y + m[8] * value.z;
  const DngRgb corrected = DngApplyColorProfile(DngMakeRgb(x, y, z), push.dng_profile);
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(corrected.r, corrected.g, corrected.b);
  destination[id.xy] = float4(CeAcesccEncode(compressed.r), CeAcesccEncode(compressed.g),
                              CeAcesccEncode(compressed.b), value.w);
}
