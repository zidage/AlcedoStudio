//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX1 experiment (vulkan_compute_backend_plan.md, section 6): three stage kernels of the film
// print, ported from edit/runtime/opencl/shader/film_print.cl with the shared headers
// color_encoding_math.h, aces_reference_gamut_compression.h, and film_print_math.h. Experiment
// code: it does not merge.
//
// - film_print_exposure       FP1: entry gamut compression, locus projection, u'v' table
// - film_print_first_amounts  FP4 and FP5 with no blur: curves and the inhibition steps
// - film_print_display        FP6 with no flare and no soft record: negative grade, print table,
//                             display compression, cinema look, display curve
//
// Binding model of the experiment:
// - set 0, binding 0: scene RGBA32F storage image (read)
// - set 0, binding 1: destination RGBA32F storage image (write)
// - the parameter arena, the tables, and the planes of 3 floats per pixel are device addresses
//   in the push constants.

#include "color/color_encoding_math.h"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/film/film_print_math.h"

struct FilmPush {
  uint64_t data;        // parameter arena (bytes); FilmKernelParams at params_offset
  float*   values;      // table buffer of the selection
  float*   q;           // exposure plane, 3 floats per pixel
  float*   a;           // amounts plane, 3 floats per pixel
  float*   print_rgba;  // print table, 4 values per node
  uint     params_offset;
  uint     q_offset;
  uint     a_offset;
  int      width;
  int      height;
  int      inhibitor_blur;
};

[[vk::push_constant]] FilmPush push;

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> scene_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> dst_image;

static FilmKernelParams* FilmParams(uint64_t data, uint offset) {
  return (FilmKernelParams*)(data + offset);
}

static FilmKernelRgb FilmLoad(float* image, uint offset, uint index) {
  float* texel = image + offset + 3U * index;
  return FilmKernelMakeRgb(texel[0], texel[1], texel[2]);
}

static void FilmStore(float* image, uint offset, uint index, FilmKernelRgb value) {
  float* texel = image + offset + 3U * index;
  texel[0]     = value.r;
  texel[1]     = value.g;
  texel[2]     = value.b;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void film_print_exposure(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const float4      source = scene_image[id.xy];
  FilmKernelParams* params = FilmParams(push.data, push.params_offset);
  const uint        index  = uint(y) * uint(push.width) + uint(x);
  if (params->scan != 0U) {
    FilmStore(push.q, push.q_offset, index,
              FilmKernelLayerExposure(params, push.values, source.x, source.y, source.z));
    return;
  }
  const AcesRgcRgb    compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const FilmKernelRgb exposure =
      FilmKernelLayerExposure(params, push.values, compressed.r, compressed.g, compressed.b);
  FilmStore(push.q, push.q_offset, index, exposure);
}

static void FilmFirstStage(FilmKernelRgb value, float* q, uint q_offset, float* a, uint a_offset,
                           uint index, int inhibitor_blur, float* values,
                           FilmKernelParams* params) {
  if (inhibitor_blur != 0) {
    const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(params, values, value);
    FilmStore(q, q_offset, index, first.log_q);
    FilmStore(a, a_offset, index, first.amounts);
  } else {
    FilmStore(a, a_offset, index, FilmKernelPointAmounts(params, values, value));
  }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void film_print_first_amounts(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint index = uint(y) * uint(push.width) + uint(x);
  FilmFirstStage(FilmLoad(push.q, push.q_offset, index), push.q, push.q_offset, push.a,
                 push.a_offset, index, push.inhibitor_blur, push.values,
                 FilmParams(push.data, push.params_offset));
}

// FilmPrintFinishedLinear of film_print.cl with has_flare = 0 and has_soft = 0.
static FilmKernelRgb FilmPrintFinishedLinear(FilmKernelParams* params, FilmKernelRgb record,
                                             float* print_rgba) {
  if (params->negative_grade != 0U) {
    record = FilmKernelNegativeGrade(params, record, record);
  }
  const FilmKernelRgb finished =
      FilmKernelFinishDisplay(params, FilmKernelPrintDisplay(params, print_rgba, record));
  return params->cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void film_print_display(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  FilmKernelParams*   params = FilmParams(push.data, push.params_offset);
  const uint          index  = uint(y) * uint(push.width) + uint(x);
  const FilmKernelRgb record = FilmLoad(push.a, push.a_offset, index);
  FilmKernelRgb       linear;
  if (params->light_table != 0U) {
    const FilmKernelRgb light = FilmKernelPrintDisplay(params, push.print_rgba, record);
    linear = FilmKernelFinishDisplay(params, FilmKernelDisplaySurround(params, light));
  } else {
    linear = FilmPrintFinishedLinear(params, record, push.print_rgba);
  }
  const float alpha = scene_image[id.xy].w;
  const int   tf    = int(params->display_encoding);
  dst_image[id.xy]  = float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                             CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                             CeDisplayEncodeChannel(tf, linear.b, 1.0f), alpha);
}
