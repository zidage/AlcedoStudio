//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

typedef struct LensCalibGpuParams {
  int   version;
  int   src_width;
  int   src_height;
  int   dst_width;
  int   dst_height;
  float norm_scale;
  float norm_unscale;
  float center_x;
  float center_y;
  float camera_crop_factor;
  float nominal_focal_mm;
  float real_focal_mm;
  float lens_center_x;
  float lens_center_y;
  int   source_projection;
  int   target_projection;
  int   distortion_model;
  float distortion_terms[5];
  int   tca_model;
  float tca_terms[12];
  int   vignetting_model;
  float vignetting_terms[3];
  int   crop_mode;
  float crop_bounds[4];
  int   interpolation;
  int   apply_vignetting;
  int   apply_distortion;
  int   apply_tca;
  int   apply_projection;
  int   apply_crop;
  int   apply_crop_circle;
  int   use_user_scale;
  int   use_auto_scale;
  float user_scale;
  float resolved_scale;
  int   perspective_mode;
  float perspective_terms[8];
  int   fast_path_distortion_only;
  int   fast_path_vignetting_only;
  int   low_precision_preview;
} LensCalibGpuParams;

typedef struct CropRectPx {
  float left;
  float right;
  float top;
  float bottom;
} CropRectPx;

inline float2 lens_pixel_to_normalized(float x, float y, LensCalibGpuParams p) {
  return (float2)(x * p.norm_scale - p.center_x, y * p.norm_scale - p.center_y);
}

inline float2 lens_normalized_to_pixel(float2 pt, LensCalibGpuParams p) {
  return (float2)((pt.x + p.center_x) * p.norm_unscale, (pt.y + p.center_y) * p.norm_unscale);
}

inline float lens_safe_atan2(float y, float x) {
  return (fabs(y) <= 1.0e-8f && fabs(x) <= 1.0e-8f) ? 0.0f : atan2(y, x);
}

inline float2 lens_rect_to_erect(float2 in) {
  return (float2)(atan2(in.x, 1.0f), atan2(in.y, sqrt(1.0f + in.x * in.x)));
}

inline float2 lens_fisheye_to_erect(float2 in) {
  const float r  = length(in);
  const float s  = (r <= 1.0e-8f) ? 1.0f : (sin(r) / r);
  const float vx = cos(r);
  const float vy = s * in.x;
  return (float2)(atan2(vy, vx), atan(s * in.y / sqrt(vx * vx + vy * vy)));
}

inline float2 lens_panoramic_to_erect(float2 in) { return (float2)(in.x, atan(in.y)); }

inline float2 lens_orthographic_to_erect(float2 in) {
  const float r     = length(in);
  const float theta = (r < 1.0f) ? asin(r) : 1.5707963267948966f;
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= 1.0e-8f) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return (float2)(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline float2 lens_stereographic_to_erect(float2 in) {
  const float rh    = length(in);
  const float c     = 2.0f * atan(rh / 2.0f);
  const float sinc  = sin(c);
  const float cosc  = cos(c);
  float       out_x = 0.0f;
  float       out_y = 0.0f;
  if (fabs(rh) <= 1.0e-8f) {
    out_y = 1.6e16f;
  } else {
    out_y = asin(in.y * sinc / rh);
    if (fabs(cosc) >= 1.0e-8f || fabs(in.x) >= 1.0e-8f) {
      out_x = atan2(in.x * sinc, cosc * rh);
    } else {
      out_x = 1.6e16f;
    }
  }
  return (float2)(out_x, out_y);
}

inline float2 lens_equisolid_to_erect(float2 in) {
  const float r     = length(in);
  const float theta = (r < 2.0f) ? 2.0f * asin(r / 2.0f) : 1.5707963267948966f;
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= 1.0e-8f) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return (float2)(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline float2 lens_thoby_to_erect(float2 in) {
  const float rho = length(in);
  if (rho > 1.47f) {
    return (float2)(1.6e16f, 1.6e16f);
  }
  const float theta = asin(rho / 1.47f) / 0.713f;
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= 1.0e-8f) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return (float2)(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline float2 lens_erect_to_rect(float2 in) {
  float theta = -in.y + 1.5707963267948966f;
  float x     = in.x;
  if (theta < 0.0f) {
    theta = -theta;
    x += 3.1415926535897932f;
  }
  if (theta > 3.1415926535897932f) {
    theta = 6.2831853071795864f - theta;
    x += 3.1415926535897932f;
  }
  return (float2)(tan(x), 1.0f / (tan(theta) * cos(x)));
}

inline float2 lens_erect_to_fisheye(float2 in) {
  float x     = in.x;
  float y     = in.y;
  float theta = -y + 1.5707963267948966f;
  if (theta < 0.0f) {
    theta = -theta;
    x += 3.1415926535897932f;
  }
  if (theta > 3.1415926535897932f) {
    theta = 6.2831853071795864f - theta;
    x += 3.1415926535897932f;
  }
  const float s     = sin(theta);
  const float vx    = s * sin(x);
  const float vy    = cos(theta);
  const float r     = sqrt(vx * vx + vy * vy);
  theta             = atan2(r, s * cos(x));
  const float inv_r = (r <= 1.0e-8f) ? 0.0f : (1.0f / r);
  return (float2)(theta * vx * inv_r, theta * vy * inv_r);
}

inline float2 lens_erect_to_panoramic(float2 in) { return (float2)(in.x, tan(in.y)); }

inline float2 lens_erect_to_orthographic(float2 in) {
  float x     = in.x;
  float y     = in.y;
  float theta = -y + 1.5707963267948966f;
  if (theta < 0.0f) {
    theta = -theta;
    x += 3.1415926535897932f;
  }
  if (theta > 3.1415926535897932f) {
    theta = 6.2831853071795864f - theta;
    x += 3.1415926535897932f;
  }
  const float s      = sin(theta);
  const float vx     = s * sin(x);
  const float vy     = cos(theta);
  const float theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const float phi2   = atan2(vy, vx);
  const float rho    = sin(theta2);
  return (float2)(rho * cos(phi2), rho * sin(phi2));
}

inline float2 lens_erect_to_stereographic(float2 in) {
  const float cos_phi = cos(in.y);
  const float ksp     = 2.0f / (1.0f + cos_phi * cos(in.x));
  return (float2)(ksp * cos_phi * sin(in.x), ksp * sin(in.y));
}

inline float2 lens_erect_to_equisolid(float2 in) {
  if (fabs(cos(in.y) * cos(in.x) + 1.0f) <= 1.0e-8f) {
    return (float2)(1.6e16f, 1.6e16f);
  }
  const float k1 = sqrt(2.0f / (1.0f + cos(in.y) * cos(in.x)));
  return (float2)(k1 * cos(in.y) * sin(in.x), k1 * sin(in.y));
}

inline float2 lens_erect_to_thoby(float2 in) {
  float x     = in.x;
  float y     = in.y;
  float theta = -y + 1.5707963267948966f;
  if (theta < 0.0f) {
    theta = -theta;
    x += 3.1415926535897932f;
  }
  if (theta > 3.1415926535897932f) {
    theta = 6.2831853071795864f - theta;
    x += 3.1415926535897932f;
  }
  const float s      = sin(theta);
  const float vx     = s * sin(x);
  const float vy     = cos(theta);
  const float theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const float phi2   = atan2(vy, vx);
  const float rho    = 1.47f * sin(theta2 * 0.713f);
  return (float2)(rho * cos(phi2), rho * sin(phi2));
}

inline float2 lens_convert_to_erect(float2 in, int projection) {
  switch (projection) {
    case 1:
      return lens_rect_to_erect(in);
    case 2:
      return lens_fisheye_to_erect(in);
    case 3:
      return lens_panoramic_to_erect(in);
    case 5:
      return lens_orthographic_to_erect(in);
    case 6:
      return lens_stereographic_to_erect(in);
    case 7:
      return lens_equisolid_to_erect(in);
    case 8:
      return lens_thoby_to_erect(in);
    case 4:
    case 0:
    default:
      return in;
  }
}

inline float2 lens_convert_from_erect(float2 in, int projection) {
  switch (projection) {
    case 1:
      return lens_erect_to_rect(in);
    case 2:
      return lens_erect_to_fisheye(in);
    case 3:
      return lens_erect_to_panoramic(in);
    case 5:
      return lens_erect_to_orthographic(in);
    case 6:
      return lens_erect_to_stereographic(in);
    case 7:
      return lens_erect_to_equisolid(in);
    case 8:
      return lens_erect_to_thoby(in);
    case 4:
    case 0:
    default:
      return in;
  }
}

inline float2 lens_apply_projection_transform(float2 in, LensCalibGpuParams p) {
  if (p.apply_projection == 0 || p.target_projection == 0 || p.source_projection == 0 ||
      p.target_projection == p.source_projection) {
    return in;
  }
  const float2 erect = lens_convert_to_erect(in, p.target_projection);
  return lens_convert_from_erect(erect, p.source_projection);
}

inline float2 lens_apply_distortion(float2 in, LensCalibGpuParams p) {
  if (p.apply_distortion == 0) {
    return in;
  }
  const float x   = in.x;
  const float y   = in.y;
  const float ru2 = x * x + y * y;
  if (p.distortion_model == 1) {
    const float poly = 1.0f + p.distortion_terms[0] * ru2;
    return (float2)(x * poly, y * poly);
  }
  if (p.distortion_model == 2) {
    const float poly = 1.0f + p.distortion_terms[0] * ru2 + p.distortion_terms[1] * ru2 * ru2;
    return (float2)(x * poly, y * poly);
  }
  if (p.distortion_model == 3) {
    const float r    = sqrt(ru2);
    const float poly = p.distortion_terms[0] * ru2 * r + p.distortion_terms[1] * ru2 +
                       p.distortion_terms[2] * r + 1.0f;
    return (float2)(x * poly, y * poly);
  }
  return in;
}

inline void lens_apply_tca(float2 in, __private float2* red, __private float2* blue,
                           LensCalibGpuParams p) {
  *red  = in;
  *blue = in;
  if (p.apply_tca == 0) {
    return;
  }
  if (p.tca_model == 1) {
    *red  = (float2)(in.x * p.tca_terms[0], in.y * p.tca_terms[0]);
    *blue = (float2)(in.x * p.tca_terms[1], in.y * p.tca_terms[1]);
    return;
  }
  if (p.tca_model == 2) {
    const float r2 = dot(in, in);
    const float rr = sqrt(r2);
    const float fr = p.tca_terms[4] * r2 + p.tca_terms[2] * rr + p.tca_terms[0];
    const float fb = p.tca_terms[5] * r2 + p.tca_terms[3] * rr + p.tca_terms[1];
    *red           = (float2)(in.x * fr, in.y * fr);
    *blue          = (float2)(in.x * fb, in.y * fb);
  }
}

inline float4 lens_read_with_border(__global const float4* src, int stride, int width, int height,
                                    int x, int y) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return (float4)(0.0f);
  }
  return src[y * stride + x];
}

inline float4 lens_bilinear_sample(__global const float4* src, int stride, int width, int height,
                                   float sx, float sy) {
  const int   x0  = (int)floor(sx);
  const int   y0  = (int)floor(sy);
  const int   x1  = x0 + 1;
  const int   y1  = y0 + 1;
  const float fx  = sx - (float)x0;
  const float fy  = sy - (float)y0;
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  return lens_read_with_border(src, stride, width, height, x0, y0) * w00 +
         lens_read_with_border(src, stride, width, height, x1, y0) * w10 +
         lens_read_with_border(src, stride, width, height, x0, y1) * w01 +
         lens_read_with_border(src, stride, width, height, x1, y1) * w11;
}

inline float lens_bilinear_sample_channel(__global const float4* src, int stride, int width,
                                          int height, float sx, float sy, int channel) {
  const float4 pixel = lens_bilinear_sample(src, stride, width, height, sx, sy);
  return channel == 0 ? pixel.x : (channel == 1 ? pixel.y : (channel == 2 ? pixel.z : pixel.w));
}

inline float2 lens_apply_scale_and_perspective(float2 in, LensCalibGpuParams p) {
  float2 out = in;
  if (fabs(p.resolved_scale) > 1.0e-8f) {
    out *= 1.0f / p.resolved_scale;
  }
  return out;
}

inline CropRectPx lens_resolve_crop_rect_px(LensCalibGpuParams p) {
  CropRectPx  rect   = {0.0f, 0.0f, 0.0f, 0.0f};
  const float width  = (float)p.dst_width;
  const float height = (float)p.dst_height;
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }
  if (p.dst_width >= p.dst_height) {
    rect.left   = p.crop_bounds[0] * width;
    rect.right  = p.crop_bounds[1] * width;
    rect.top    = p.crop_bounds[2] * height;
    rect.bottom = p.crop_bounds[3] * height;
  } else {
    rect.left   = p.crop_bounds[2] * width;
    rect.right  = p.crop_bounds[3] * width;
    rect.top    = p.crop_bounds[0] * height;
    rect.bottom = p.crop_bounds[1] * height;
  }
  if (rect.left > rect.right) {
    const float tmp = rect.left;
    rect.left       = rect.right;
    rect.right      = tmp;
  }
  if (rect.top > rect.bottom) {
    const float tmp = rect.top;
    rect.top        = rect.bottom;
    rect.bottom     = tmp;
  }
  return rect;
}

inline float4 lens_apply_circle_crop_alpha(float4 in, int x, int y, LensCalibGpuParams p) {
  if (p.apply_crop_circle == 0) {
    return in;
  }
  const CropRectPx rect   = lens_resolve_crop_rect_px(p);
  const float      cx     = 0.5f * (rect.left + rect.right);
  const float      cy     = 0.5f * (rect.top + rect.bottom);
  const float      rx     = 0.5f * fabs(rect.right - rect.left);
  const float      ry     = 0.5f * fabs(rect.bottom - rect.top);
  const float      radius = fmin(rx, ry);
  if (radius <= 1.0e-8f) {
    return in;
  }
  const float dx = ((float)x + 0.5f) - cx;
  const float dy = ((float)y + 0.5f) - cy;
  if ((dx * dx + dy * dy) > (radius * radius)) {
    in.w = 0.0f;
  }
  return in;
}

__kernel void edit_geometry_lens_vignetting_rgba32f(__global float4* image, int stride,
                                                    LensCalibGpuParams p) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= p.dst_width || y >= p.dst_height) {
    return;
  }
  const float2 pt = lens_pixel_to_normalized((float)x, (float)y, p);
  const float  r2 = dot(pt, pt);
  const float  r4 = r2 * r2;
  const float  r6 = r4 * r2;
  const float  c =
      1.0f + p.vignetting_terms[0] * r2 + p.vignetting_terms[1] * r4 + p.vignetting_terms[2] * r6;
  const float gain = (fabs(c) > 1.0e-8f) ? (1.0f / c) : 1.0f;
  float4      pix  = image[y * stride + x];
  pix.xyz *= gain;
  image[y * stride + x] = pix;
}

__kernel void edit_geometry_lens_warp_rgba32f(__global const float4* src, int src_stride,
                                              __global float4* dst, int dst_stride,
                                              LensCalibGpuParams p) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= p.dst_width || y >= p.dst_height) {
    return;
  }

  float2 g = lens_pixel_to_normalized((float)x, (float)y, p);
  g        = lens_apply_scale_and_perspective(g, p);
  g        = lens_apply_projection_transform(g, p);
  g        = lens_apply_distortion(g, p);

  float2 r = g;
  float2 b = g;
  lens_apply_tca(g, &r, &b, p);

  const float2 gp = lens_normalized_to_pixel(g, p);
  const float2 rp = lens_normalized_to_pixel(r, p);
  const float2 bp = lens_normalized_to_pixel(b, p);

  float4       out;
  if (p.apply_tca != 0) {
    out.x = lens_bilinear_sample_channel(src, src_stride, p.src_width, p.src_height, rp.x, rp.y, 0);
    out.y = lens_bilinear_sample_channel(src, src_stride, p.src_width, p.src_height, gp.x, gp.y, 1);
    out.z = lens_bilinear_sample_channel(src, src_stride, p.src_width, p.src_height, bp.x, bp.y, 2);
    out.w = lens_bilinear_sample_channel(src, src_stride, p.src_width, p.src_height, gp.x, gp.y, 3);
  } else {
    out = lens_bilinear_sample(src, src_stride, p.src_width, p.src_height, gp.x, gp.y);
  }

  dst[y * dst_stride + x] = lens_apply_circle_crop_alpha(out, x, y, p);
}
