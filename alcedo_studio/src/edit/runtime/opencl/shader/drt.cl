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
  const AcesRgcRgb compressed =
      AcesReferenceGamutCompress(opencl_acescc_decode(source.x), opencl_acescc_decode(source.y),
                                 opencl_acescc_decode(source.z));
  const float3 scene = (float3)(compressed.r, compressed.g, compressed.b);
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

__kernel void drt_display_scene_rgba32f(__read_only image2d_t src_image,
                                        __global const float4* src_buffer, int src_is_buffer,
                                        __write_only image2d_t dst_image, __global float4* dst_buffer,
                                        int dst_is_buffer, __global const uchar* params_bytes,
                                        const uint params_offset_bytes, uint width, uint height) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) {
    return;
  }
  const __global OpenClToOutputParams* params =
      (__global const OpenClToOutputParams*)(params_bytes + params_offset_bytes);
  const float4 source = src_is_buffer != 0
                            ? src_buffer[(uint)gid.y * width + (uint)gid.x]
                            : read_imagef(src_image, kNearestClamp, gid);
  const AcesRgcRgb compressed =
      AcesReferenceGamutCompress(opencl_acescc_decode(source.x), opencl_acescc_decode(source.y),
                                 opencl_acescc_decode(source.z));
  const float3 scene = (float3)(compressed.r, compressed.g, compressed.b);
  float3 display_linear;
  if (params->method_ == 0) {
    display_linear = opencl_aces_output_transform_fwd(scene, &params->aces_params_);
  } else {
    display_linear = opencl_open_drt_transform_fwd(scene, &params->open_drt_params_);
  }
  const float3 encoded = opencl_display_encoding(
      display_linear, params->limit_to_display_matx, params->eotf_, params->display_linear_scale_);
  const float4 outp = (float4)(encoded.x, encoded.y, encoded.z, source.w);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)gid.y * width + (uint)gid.x] = outp;
  } else {
    write_imagef(dst_image, gid, outp);
  }
}
