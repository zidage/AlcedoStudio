//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

__kernel void scope_accumulate_histogram(__global const float4* input,
                                         const uint input_pitch_pixels, const int width,
                                         const int height, const int sample_step, const int bins,
                                         volatile __global uint* histogram_counts) {
  const int sample_x = (int)get_global_id(0);
  const int sample_y = (int)get_global_id(1);
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height || bins <= 0) {
    return;
  }

  const float4 pixel  = input[(size_t)y * input_pitch_pixels + (uint)x];
  const float  rgb[3] = {
      clamp(pixel.x, 0.0f, 1.0f),
      clamp(pixel.y, 0.0f, 1.0f),
      clamp(pixel.z, 0.0f, 1.0f),
  };

  for (int channel = 0; channel < 3; ++channel) {
    const int bin = min((int)(rgb[channel] * (float)(bins - 1) + 0.5f), bins - 1);
    atomic_add(&histogram_counts[channel * bins + bin], 1u);
  }
}

__kernel void scope_accumulate_waveform(__global const float4* input, const uint input_pitch_pixels,
                                        const int width, const int height, const int sample_step,
                                        volatile __global uint* waveform_counts,
                                        const uint waveform_pitch_pixels, const int waveform_width,
                                        const int waveform_height) {
  const int sample_x = (int)get_global_id(0);
  const int sample_y = (int)get_global_id(1);
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height || waveform_width <= 0 || waveform_height <= 0 ||
      waveform_pitch_pixels == 0) {
    return;
  }

  const float4 pixel  = input[(size_t)y * input_pitch_pixels + (uint)x];
  const float  rgb[3] = {
      clamp(pixel.x, 0.0f, 1.0f),
      clamp(pixel.y, 0.0f, 1.0f),
      clamp(pixel.z, 0.0f, 1.0f),
  };
  const float width_denom = (float)max(width - 1, 1);
  const int   x_bin =
      min((int)(((float)x / width_denom) * (float)(waveform_width - 1) + 0.5f), waveform_width - 1);

  for (int channel = 0; channel < 3; ++channel) {
    const int y_bin =
        waveform_height - 1 -
        min((int)(rgb[channel] * (float)(waveform_height - 1) + 0.5f), waveform_height - 1);
    const size_t base = ((size_t)y_bin * waveform_pitch_pixels + (uint)x_bin) * 4U;
    atomic_add(&waveform_counts[base + (uint)channel], 1u);
    atomic_add(&waveform_counts[base + 3U], 1u);
  }
}

__constant sampler_t scope_nearest_clamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

__kernel void scope_accumulate_histogram_image(read_only image2d_t input, const int width,
                                               const int height, const int sample_step,
                                               const int bins,
                                               volatile __global uint* histogram_counts) {
  const int sample_x = (int)get_global_id(0);
  const int sample_y = (int)get_global_id(1);
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height || bins <= 0) {
    return;
  }

  const float4 pixel  = read_imagef(input, scope_nearest_clamp, (int2)(x, y));
  const float  rgb[3] = {
      clamp(pixel.x, 0.0f, 1.0f),
      clamp(pixel.y, 0.0f, 1.0f),
      clamp(pixel.z, 0.0f, 1.0f),
  };
  for (int channel = 0; channel < 3; ++channel) {
    const int bin = min((int)(rgb[channel] * (float)(bins - 1) + 0.5f), bins - 1);
    atomic_add(&histogram_counts[channel * bins + bin], 1u);
  }
}

__kernel void scope_accumulate_waveform_image(read_only image2d_t input, const int width,
                                              const int height, const int sample_step,
                                              volatile __global uint* waveform_counts,
                                              const uint waveform_pitch_pixels,
                                              const int waveform_width,
                                              const int waveform_height) {
  const int sample_x = (int)get_global_id(0);
  const int sample_y = (int)get_global_id(1);
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height || waveform_width <= 0 || waveform_height <= 0 ||
      waveform_pitch_pixels == 0) {
    return;
  }

  const float4 pixel  = read_imagef(input, scope_nearest_clamp, (int2)(x, y));
  const float  rgb[3] = {
      clamp(pixel.x, 0.0f, 1.0f),
      clamp(pixel.y, 0.0f, 1.0f),
      clamp(pixel.z, 0.0f, 1.0f),
  };
  const float width_denom = (float)max(width - 1, 1);
  const int   x_bin =
      min((int)(((float)x / width_denom) * (float)(waveform_width - 1) + 0.5f),
          waveform_width - 1);
  for (int channel = 0; channel < 3; ++channel) {
    const int y_bin = waveform_height - 1 -
                      min((int)(rgb[channel] * (float)(waveform_height - 1) + 0.5f),
                          waveform_height - 1);
    const size_t base = ((size_t)y_bin * waveform_pitch_pixels + (uint)x_bin) * 4U;
    atomic_add(&waveform_counts[base + (uint)channel], 1u);
    atomic_add(&waveform_counts[base + 3U], 1u);
  }
}
