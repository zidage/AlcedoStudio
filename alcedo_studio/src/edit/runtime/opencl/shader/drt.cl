//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

__kernel void drt_display_rgba32f(__read_only image2d_t src, __write_only image2d_t dst,
                                  __global const uchar* params_bytes,
                                  const uint params_offset_bytes) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }

  const __global OpenClToOutputParams* params =
      (__global const OpenClToOutputParams*)(params_bytes + params_offset_bytes);
  const float4 source = read_imagef(src, kNearestClamp, gid);
  const float3 scene  = (float3)(opencl_acescc_decode(source.x), opencl_acescc_decode(source.y),
                                opencl_acescc_decode(source.z));
  float3 display_linear;
  if (params->method_ == 0) {
    display_linear = opencl_aces_output_transform_fwd(scene, &params->aces_params_);
  } else {
    display_linear = opencl_open_drt_transform_fwd(scene, &params->open_drt_params_);
  }
  const float3 encoded = opencl_display_encoding(
      display_linear, params->limit_to_display_matx, params->eotf_, params->display_linear_scale_);
  write_imagef(dst, gid, (float4)(encoded.x, encoded.y, encoded.z, source.w));
}
