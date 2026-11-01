//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "color/color_encoding_math.h"
#include "edit/runtime/cuda/cuda_film_print_pass.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kBlock = 256;

/// The point stages have their own kernels, because the blurs of phase F4 run between them.
__global__ void         ExposureKernel(const float4* input, float4* output, std::uint32_t pixels,
                                       const float* values, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float4 source = input[index];
  const auto   q = FilmKernelLayerExposure(&params, values, source.x, source.y, source.z);
  output[index]  = make_float4(q.r, q.g, q.b, source.w);
}

__global__ void FirstAmountsKernel(const float4* exposure, float4* output, std::uint32_t pixels,
                                   const float* values, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float4 q = exposure[index];
  const auto   a = FilmKernelFirstAmounts(&params, values, FilmKernelMakeRgb(q.x, q.y, q.z));
  output[index]  = make_float4(a.r, a.g, a.b, q.w);
}

/// @p amounts is both the inhibitor and the output: a step reads and writes one pixel.
__global__ void InhibitedStepKernel(const float4* exposure, float4* amounts, std::uint32_t pixels,
                                    const float* values, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float4 q = exposure[index];
  const float4 i = amounts[index];
  const auto   a = FilmKernelInhibitedStep(&params, values, FilmKernelMakeRgb(q.x, q.y, q.z),
                                           FilmKernelMakeRgb(i.x, i.y, i.z));
  amounts[index] = make_float4(a.r, a.g, a.b, q.w);
}

__global__ void DisplayKernel(const float4* amounts, float4* output, std::uint32_t pixels,
                              const float* print_rgba, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float4 a      = amounts[index];
  const auto   linear = FilmKernelFinishDisplay(
      &params, FilmKernelPrintDisplay(&params, print_rgba, FilmKernelMakeRgb(a.x, a.y, a.z)));
  const int tf  = static_cast<int>(params.display_encoding);
  output[index] = make_float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                              CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                              CeDisplayEncodeChannel(tf, linear.b, 1.0f), a.w);
}

}  // namespace

void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, const FrameSceneBinding& scene,
                         const FrameSceneBinding& display, std::uint32_t width,
                         std::uint32_t height) {
  const auto resolved =
      DrtOutputResolver::ResolveNode(drt, plan.output_color_override, "ExecuteCudaDrt");
  const auto film    = drt.Params().FilmPrint();
  auto&      tables  = SharedFilmPrintTables();
  const auto data    = tables.Resolve(film.negative, film.print_material, film.scene_light);

  auto&      backend = device.Workspace().Device();
  auto&      context = device.CommandContext();
  const auto print   = backend.AcquireLut(data->print_key, data->print_rgba, data->edge, context);
  const auto table   = backend.AcquireKeyedBuffer(
      data->kernel_key, std::as_bytes(std::span(data->kernel_values)), context);

  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(tables.Bundle(), film.negative, film.print_material,
                                       film.scene_light, data->edge, params);
  params.display_encoding = static_cast<std::uint32_t>(resolved.eotf_);

  const auto* values      = static_cast<const float*>(table.device_pointer);
  const auto* rgba        = static_cast<const float*>(print.device_pointer);
  auto  exposure = device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  auto  amounts  = device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  auto* q        = static_cast<float4*>(exposure.Texture().DevicePointer());
  auto* a        = static_cast<float4*>(amounts.Texture().DevicePointer());

  const std::uint32_t pixels = width * height;
  const std::uint32_t blocks = (pixels + kBlock - 1) / kBlock;
  auto                stream = context.Stream();
  ExposureKernel<<<blocks, kBlock, 0, stream>>>(
      static_cast<const float4*>(CudaSceneTexture(device, scene).DevicePointer()), q, pixels,
      values, params);
  FirstAmountsKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
  if (params.inhibition_stored != 0U) {
    // Rule 'inhibition_two_steps'.
    InhibitedStepKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
    InhibitedStepKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
  }
  DisplayKernel<<<blocks, kBlock, 0, stream>>>(
      a, static_cast<float4*>(CudaSceneTexture(device, display).DevicePointer()), pixels, rgba,
      params);
}

}  // namespace alcedo
