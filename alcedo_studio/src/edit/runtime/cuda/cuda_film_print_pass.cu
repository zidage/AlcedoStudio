//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "color/color_encoding_math.h"
#include "cuda/cuda_check.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_film_print_pass.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kBlock      = 256;
constexpr unsigned      kImageBlock = 16;

auto                    GridFor(std::uint32_t width, std::uint32_t height) -> dim3 {
  return dim3{(width + kImageBlock - 1) / kImageBlock, (height + kImageBlock - 1) / kImageBlock};
}

__device__ __forceinline__ auto Transform(const Matrix3x3& matrix, float x, float y) -> float2 {
  return make_float2(matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
                     matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]);
}

/// The boundary rule 'reflect' of scipy.ndimage: (d c b a | a b c d | d c b a).
__device__ __forceinline__ auto Reflect(int position, int size) -> int {
  if (position >= 0 && position < size) return position;
  const int period = 2 * size;
  position %= period;
  if (position < 0) position += period;
  return position < size ? position : period - 1 - position;
}

/// Bilinear read at a position where texel `i` covers [i, i + 1), as the lab enlarges an image.
__device__ __forceinline__ auto ReadBilinear(const float4* image, int width, int height, float px,
                                             float py) -> float3 {
  const float x  = fminf(fmaxf(px - 0.5f, 0.0f), static_cast<float>(width - 1));
  const float y  = fminf(fmaxf(py - 0.5f, 0.0f), static_cast<float>(height - 1));
  const int   x0 = static_cast<int>(x);
  const int   y0 = static_cast<int>(y);
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const auto& a  = image[y0 * width + x0];
  const auto& b  = image[y0 * width + x1];
  const auto& c  = image[y1 * width + x0];
  const auto& d  = image[y1 * width + x1];
  const float gx = 1.0f - fx;
  const float gy = 1.0f - fy;
  return make_float3(gy * (gx * a.x + fx * b.x) + fy * (gx * c.x + fx * d.x),
                     gy * (gx * a.y + fx * b.y) + fy * (gx * c.y + fx * d.y),
                     gy * (gx * a.z + fx * b.z) + fy * (gx * c.z + fx * d.z));
}

/// The point stages have their own kernels, because the blurs run between them.
__global__ void ExposureKernel(const float4* input, float4* output, std::uint32_t pixels,
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

/// @p amounts holds the inhibitor at the size of the render, and takes the output.
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

/// One step with a half-size inhibitor, enlarged by the bilinear read of the lab.
__global__ void InhibitedStepHalfSizeKernel(const float4* exposure, float4* amounts, int width,
                                            int height, const float4* inhibitor,
                                            int inhibitor_width, int inhibitor_height,
                                            const float* values, FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const int    index = y * width + x;
  const float4 q     = exposure[index];
  const float3 i =
      ReadBilinear(inhibitor, inhibitor_width, inhibitor_height,
                   (static_cast<float>(x) + 0.5f) * static_cast<float>(inhibitor_width) /
                       static_cast<float>(width),
                   (static_cast<float>(y) + 0.5f) * static_cast<float>(inhibitor_height) /
                       static_cast<float>(height));
  const auto a   = FilmKernelInhibitedStep(&params, values, FilmKernelMakeRgb(q.x, q.y, q.z),
                                           FilmKernelMakeRgb(i.x, i.y, i.z));
  amounts[index] = make_float4(a.r, a.g, a.b, q.w);
}

__global__ void BlurHorizontalKernel(const float4* src, float4* dst, int width, int height,
                                     FilmBlurKernel kernel) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const float4* row    = src + static_cast<std::size_t>(y) * width;
  const float4  center = row[x];
  float         r      = kernel.weights[0][0] * center.x;
  float         g      = kernel.weights[1][0] * center.y;
  float         b      = kernel.weights[2][0] * center.z;
  const int     radius = static_cast<int>(kernel.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const float4& left  = row[Reflect(x - tap, width)];
    const float4& right = row[Reflect(x + tap, width)];
    r += kernel.weights[0][tap] * (left.x + right.x);
    g += kernel.weights[1][tap] * (left.y + right.y);
    b += kernel.weights[2][tap] * (left.z + right.z);
  }
  dst[static_cast<std::size_t>(y) * width + x] = make_float4(r, g, b, center.w);
}

/// `dst = weight * blur`, or with @p accumulate, `dst += weight * blur`.
__global__ void BlurVerticalKernel(const float4* src, float4* dst, int width, int height,
                                   FilmBlurKernel kernel, float weight, bool accumulate) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const std::size_t stride = static_cast<std::size_t>(width);
  const float4      center = src[static_cast<std::size_t>(y) * stride + x];
  float             r      = kernel.weights[0][0] * center.x;
  float             g      = kernel.weights[1][0] * center.y;
  float             b      = kernel.weights[2][0] * center.z;
  const int         radius = static_cast<int>(kernel.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const float4& top    = src[static_cast<std::size_t>(Reflect(y - tap, height)) * stride + x];
    const float4& bottom = src[static_cast<std::size_t>(Reflect(y + tap, height)) * stride + x];
    r += kernel.weights[0][tap] * (top.x + bottom.x);
    g += kernel.weights[1][tap] * (top.y + bottom.y);
    b += kernel.weights[2][tap] * (top.z + bottom.z);
  }
  float4& out = dst[static_cast<std::size_t>(y) * stride + x];
  // A new pool image can hold NaN, so this form must not read the output.
  out         = accumulate
                    ? make_float4(out.x + weight * r, out.y + weight * g, out.z + weight * b, center.w)
                    : make_float4(weight * r, weight * g, weight * b, center.w);
}

/// Box mean with cells that cover the image exactly, as the lab reduces an image.
__global__ void BoxReduceKernel(const float4* src, int src_width, int src_height, float4* dst,
                                int columns, int rows) {
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (i >= rows || j >= columns) return;
  const auto cell = [](int index, int size, int count) {
    return static_cast<int>((static_cast<std::int64_t>(index) * size + count - 1) / count);
  };
  const int y0 = cell(i, src_height, rows);
  const int y1 = cell(i + 1, src_height, rows);
  const int x0 = cell(j, src_width, columns);
  const int x1 = cell(j + 1, src_width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const float4& value = src[y * src_width + x];
      r += value.x;
      g += value.y;
      b += value.z;
    }
  }
  const float scale    = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
  dst[i * columns + j] = make_float4(r * scale, g * scale, b * scale, 1.0f);
}

/**
 * Canvas from the render: the mean of @p samples x @p samples render pixels in the footprint of one
 * texel. With @p transmittance, the values are amounts and the mean is of 10^-a.
 */
__global__ void CanvasReduceKernel(const float4* src, int width, int height, float4* dst,
                                   int dst_width, int dst_height, Matrix3x3 canvas_to_render,
                                   int samples, bool transmittance) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= dst_width || y >= dst_height) return;
  const float step = 1.0f / static_cast<float>(samples);
  float       r    = 0.0f;
  float       g    = 0.0f;
  float       b    = 0.0f;
  for (int j = 0; j < samples; ++j) {
    const float cy = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float  cx     = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) * step;
      const float2 render = Transform(canvas_to_render, cx, cy);
      const int    sx     = min(max(static_cast<int>(floorf(render.x)), 0), width - 1);
      const int    sy     = min(max(static_cast<int>(floorf(render.y)), 0), height - 1);
      const float4 value  = src[static_cast<std::size_t>(sy) * width + sx];
      if (transmittance) {
        r += FilmKernelTransmittance(value.x);
        g += FilmKernelTransmittance(value.y);
        b += FilmKernelTransmittance(value.z);
      } else {
        r += value.x;
        g += value.y;
        b += value.z;
      }
    }
  }
  const float scale      = step * step;
  dst[y * dst_width + x] = make_float4(r * scale, g * scale, b * scale, 1.0f);
}

/// FP3: (1 - s) q + s blurred, per layer. @p q takes the output.
__global__ void HalationMixKernel(float4* q, int width, int height, const float4* canvas,
                                  int canvas_width, int canvas_height, Matrix3x3 render_to_canvas,
                                  float3 strength) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const float2 position =
      Transform(render_to_canvas, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
  const float3 blurred = ReadBilinear(canvas, canvas_width, canvas_height, position.x, position.y);
  float4&      value   = q[y * width + x];
  value.x              = (1.0f - strength.x) * value.x + strength.x * blurred.x;
  value.y              = (1.0f - strength.y) * value.y + strength.y * blurred.y;
  value.z              = (1.0f - strength.z) * value.z + strength.z * blurred.z;
}

/// FP6, fused. @p flare is null when the Enlarger flare is off.
__global__ void DisplayKernel(const float4* amounts, float4* output, int width, int height,
                              const float* print_rgba, const float4* flare, int flare_width,
                              int flare_height, Matrix3x3 render_to_flare, float flare_strength,
                              FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const int     index  = y * width + x;
  const float4  a      = amounts[index];
  FilmKernelRgb record = FilmKernelMakeRgb(a.x, a.y, a.z);
  if (flare != nullptr) {
    const float2 position =
        Transform(render_to_flare, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    const float3 blurred = ReadBilinear(flare, flare_width, flare_height, position.x, position.y);
    record = FilmKernelFlareAmounts(record, FilmKernelMakeRgb(blurred.x, blurred.y, blurred.z),
                                    flare_strength);
  }
  const auto linear =
      FilmKernelFinishDisplay(&params, FilmKernelPrintDisplay(&params, print_rgba, record));
  const int tf  = static_cast<int>(params.display_encoding);
  output[index] = make_float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                              CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                              CeDisplayEncodeChannel(tf, linear.b, 1.0f), a.w);
}

using Lease = ResourceLease<CudaBackend>;

auto Pointer(Lease& lease) -> float4* {
  return static_cast<float4*>(lease.Texture().DevicePointer());
}

/// GPU times between the marks of one recording, for the cost record of the plan.
class PassClock {
 public:
  PassClock(std::vector<FilmPrintPassTime>* out, cudaStream_t stream) : out_(out), stream_(stream) {
    if (out_ != nullptr) Record({});
  }
  PassClock(const PassClock&)                    = delete;
  auto operator=(const PassClock&) -> PassClock& = delete;
  ~PassClock() {
    for (auto& mark : marks_) {
      (void)::cudaEventDestroy(mark.second);
    }
  }

  void Mark(const char* name) {
    if (out_ != nullptr) Record(name);
  }

  /// Wait for the marks and add their times to the output.
  void Finish() {
    if (out_ == nullptr || marks_.empty()) return;
    cuda::CheckCuda(::cudaEventSynchronize(marks_.back().second), "RecordCudaFilmPrint: clock");
    for (std::size_t index = 1; index < marks_.size(); ++index) {
      float milliseconds = 0.0f;
      cuda::CheckCuda(
          ::cudaEventElapsedTime(&milliseconds, marks_[index - 1].second, marks_[index].second),
          "RecordCudaFilmPrint: clock");
      out_->push_back({marks_[index].first, milliseconds});
    }
  }

 private:
  void Record(std::string name) {
    cudaEvent_t event = nullptr;
    cuda::CheckCuda(::cudaEventCreate(&event), "RecordCudaFilmPrint: clock");
    marks_.emplace_back(std::move(name), event);
    cuda::CheckCuda(::cudaEventRecord(event, stream_), "RecordCudaFilmPrint: clock");
  }

  std::vector<FilmPrintPassTime>*                  out_;
  cudaStream_t                                     stream_;
  std::vector<std::pair<std::string, cudaEvent_t>> marks_;
};

/**
 * @brief The canonical result @p id: the published image, or an image from @p build.
 *
 * The rule of `DecideDiffusionScatter`: a render that covers the full edit space builds and
 * publishes; an ROI samples a published image, and otherwise builds a temporary image from its own
 * pixels. Nothing is visible to another render before `PublishResults`.
 */
template <class Build>
auto ReadOrBuildCanvasResult(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const GraphValueId& id, ImageExtent extent, std::uint32_t long_edge,
                             std::optional<Lease>& temporary, Build&& build) -> const float4* {
  auto&      workspace    = device.Workspace();
  auto&      invalidation = workspace.ResultInvalidation();
  const bool persist      = workspace.PersistsResult(id);
  const bool full_edit    = CoversFullEditSpace(plan.geometry);
  Lease*     canonical    = nullptr;
  if (persist) {
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::Rgba32f, DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical = workspace.Images().BindValidResult(id, invalidation.RequiredRevision(id), needed,
                                                   workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    ++device.PassStats().film_canvas_sample;
    return Pointer(*canonical);
  }
  float4* target = nullptr;
  if (decision.persist_canonical) {
    target = Pointer(
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::Rgba32f}));
  } else {
    temporary.emplace(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    target = Pointer(*temporary);
  }
  build(target);
  if (decision.persist_canonical) {
    const auto published = invalidation.MakeImageRepresentation(id, extent, TextureFormat::Rgba32f,
                                                                decision.current_long_edge);
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), published,
                                         device.CommandContext().SubmissionId(),
                                         decision.current_long_edge);
  }
  ++device.PassStats().film_canvas_rebuild;
  return target;
}

}  // namespace

void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, const FrameSceneBinding& scene,
                         const FrameSceneBinding& display, std::uint32_t width,
                         std::uint32_t height, std::vector<FilmPrintPassTime>* pass_times) {
  const auto resolved =
      DrtOutputResolver::ResolveNode(drt, plan.output_color_override, "ExecuteCudaDrt");
  const auto  film      = drt.Params().FilmPrint();
  auto&       tables    = SharedFilmPrintTables();
  const auto  data      = tables.Resolve(film.negative, film.print_material, film.scene_light);
  const auto& bundle    = tables.Bundle();

  auto&       workspace = device.Workspace();
  auto&       backend   = workspace.Device();
  auto&       context   = device.CommandContext();
  const auto  print = backend.AcquireLut(data->print_key, data->print_rgba, data->edge, context);
  const auto  table = backend.AcquireKeyedBuffer(
      data->kernel_key, std::as_bytes(std::span(data->kernel_values)), context);

  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(bundle, film.negative, film.print_material, film.scene_light,
                                       data->edge, params);
  params.display_encoding = static_cast<std::uint32_t>(resolved.eotf_);

  const auto negative     = bundle.Negative(film.negative);
  const auto image =
      MakeFilmPrintImagePlan(plan.geometry, bundle.ImagePlaneConstants(), negative.layer_spread_um,
                             params.inhibition_stored != 0U, film);

  const auto* values  = static_cast<const float*>(table.device_pointer);
  const auto* rgba    = static_cast<const float*>(print.device_pointer);
  const auto  acquire = [&workspace](std::uint32_t w, std::uint32_t h) {
    return workspace.Textures().Acquire({w, h, TextureFormat::Rgba32f});
  };
  const auto          w      = static_cast<int>(width);
  const auto          h      = static_cast<int>(height);

  const std::uint32_t pixels = width * height;
  const std::uint32_t blocks = (pixels + kBlock - 1) / kBlock;
  const dim3          image_block{kImageBlock, kImageBlock};
  const dim3          image_grid = GridFor(width, height);
  auto                stream     = context.Stream();
  PassClock           clock(pass_times, stream);

  // FP1.
  auto                first_exposure = acquire(width, height);
  auto*               q              = Pointer(first_exposure);
  ExposureKernel<<<blocks, kBlock, 0, stream>>>(
      static_cast<const float4*>(CudaSceneTexture(device, scene).DevicePointer()), q, pixels,
      values, params);
  clock.Mark("FP1 exposure");

  // FP2, rule 'spread_per_layer_pass'. After the spread, the first image holds the amounts.
  std::optional<Lease> second_exposure;
  std::optional<Lease> scratch;
  float4*              a = nullptr;
  std::optional<Lease> amounts;
  if (image.spread_passes != 0U) {
    second_exposure.emplace(acquire(width, height));
    scratch.emplace(acquire(width, height));
    auto* spread = Pointer(*second_exposure);
    auto* middle = Pointer(*scratch);
    for (std::uint32_t pass = 0; pass < image.spread_passes; ++pass) {
      BlurHorizontalKernel<<<image_grid, image_block, 0, stream>>>(q, middle, w, h,
                                                                   image.spread_x[pass]);
      BlurVerticalKernel<<<image_grid, image_block, 0, stream>>>(
          middle, spread, w, h, image.spread_y[pass], image.spread_weight[pass], pass != 0U);
    }
    a = q;
    q = spread;
    clock.Mark("FP2 spread");
  } else {
    amounts.emplace(acquire(width, height));
    a = Pointer(*amounts);
  }

  const auto           long_edge = (std::max)(width, height);
  const auto           canvas    = image.canvas_extent;

  // FP3: the canonical image, then a read of it per layer.
  std::optional<Lease> temporary_halation;
  if (image.halation_active) {
    const float4* blurred = ReadOrBuildCanvasResult(
        device, plan, FilmHalationResultId(plan.drt.node_id), canvas, long_edge, temporary_halation,
        [&](float4* target) {
          auto       reduced = acquire(canvas.width, canvas.height);
          auto       middle  = acquire(canvas.width, canvas.height);
          const auto grid    = GridFor(canvas.width, canvas.height);
          const auto cw      = static_cast<int>(canvas.width);
          const auto ch      = static_cast<int>(canvas.height);
          CanvasReduceKernel<<<grid, image_block, 0, stream>>>(
              q, w, h, Pointer(reduced), cw, ch, image.canvas_to_render,
              static_cast<int>(image.canvas_samples), false);
          BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(Pointer(reduced), Pointer(middle),
                                                                 cw, ch, image.halation_x);
          BlurVerticalKernel<<<grid, image_block, 0, stream>>>(Pointer(middle), target, cw, ch,
                                                               image.halation_y, 1.0f, false);
        });
    clock.Mark("FP3 canvas");
    HalationMixKernel<<<image_grid, image_block, 0, stream>>>(
        q, w, h, blurred, static_cast<int>(canvas.width), static_cast<int>(canvas.height),
        image.render_to_canvas,
        make_float3(image.halation_strength[0], image.halation_strength[1],
                    image.halation_strength[2]));
    clock.Mark("FP3 mix");
  }

  // FP4.
  FirstAmountsKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
  clock.Mark("FP4 first amounts");

  // FP4 and FP5, rule 'inhibition_two_steps'.
  if (params.inhibition_stored != 0U) {
    if (!image.image_plane) {
      InhibitedStepKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
      InhibitedStepKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
    } else if (image.inhibitor_half_size) {
      const auto extent  = image.inhibitor_extent;
      const auto grid    = GridFor(extent.width, extent.height);
      const auto columns = static_cast<int>(extent.width);
      const auto rows    = static_cast<int>(extent.height);
      auto       reduced = acquire(extent.width, extent.height);
      auto       middle  = acquire(extent.width, extent.height);
      for (int step = 0; step < 2; ++step) {
        BoxReduceKernel<<<grid, image_block, 0, stream>>>(a, w, h, Pointer(reduced), columns, rows);
        BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(Pointer(reduced), Pointer(middle),
                                                               columns, rows, image.inhibitor_x);
        BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
            Pointer(middle), Pointer(reduced), columns, rows, image.inhibitor_y, 1.0f, false);
        clock.Mark("FP4 inhibitor blur");
        InhibitedStepHalfSizeKernel<<<image_grid, image_block, 0, stream>>>(
            q, a, w, h, Pointer(reduced), columns, rows, values, params);
        clock.Mark("FP5 inhibited step");
      }
    } else {
      if (!scratch.has_value()) scratch.emplace(acquire(width, height));
      auto* middle = Pointer(*scratch);
      for (int step = 0; step < 2; ++step) {
        BlurHorizontalKernel<<<image_grid, image_block, 0, stream>>>(a, middle, w, h,
                                                                     image.inhibitor_x);
        BlurVerticalKernel<<<image_grid, image_block, 0, stream>>>(middle, a, w, h,
                                                                   image.inhibitor_y, 1.0f, false);
        clock.Mark("FP4 inhibitor blur");
        InhibitedStepKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, values, params);
        clock.Mark("FP5 inhibited step");
      }
    }
  }

  // FP6: the canonical image of the blurred transmittance, then the fused point stages.
  std::optional<Lease> temporary_flare;
  const float4*        flare = nullptr;
  if (image.flare_active) {
    const auto extent = image.flare_extent;
    flare             = ReadOrBuildCanvasResult(
        device, plan, FilmFlareResultId(plan.drt.node_id), extent, long_edge, temporary_flare,
        [&](float4* target) {
          auto       reduced = acquire(canvas.width, canvas.height);
          const auto cw      = static_cast<int>(canvas.width);
          const auto ch      = static_cast<int>(canvas.height);
          CanvasReduceKernel<<<GridFor(canvas.width, canvas.height), image_block, 0, stream>>>(
              a, w, h, Pointer(reduced), cw, ch, image.canvas_to_render,
              static_cast<int>(image.canvas_samples), true);
          const auto           columns = static_cast<int>(extent.width);
          const auto           rows    = static_cast<int>(extent.height);
          const auto           grid    = GridFor(extent.width, extent.height);
          auto                 middle  = acquire(extent.width, extent.height);
          // A canvas with a small flare sigma has no second reduction.
          std::optional<Lease> coarse;
          float4*              source = Pointer(reduced);
          if (extent.width != canvas.width || extent.height != canvas.height) {
            coarse.emplace(acquire(extent.width, extent.height));
            source = Pointer(*coarse);
            BoxReduceKernel<<<grid, image_block, 0, stream>>>(Pointer(reduced), cw, ch, source,
                                                                          columns, rows);
          }
          BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(source, Pointer(middle), columns,
                                                                             rows, image.flare_x);
          BlurVerticalKernel<<<grid, image_block, 0, stream>>>(Pointer(middle), target, columns,
                                                                           rows, image.flare_y, 1.0f, false);
        });
    clock.Mark("FP6 flare canvas");
  }
  DisplayKernel<<<image_grid, image_block, 0, stream>>>(
      a, static_cast<float4*>(CudaSceneTexture(device, display).DevicePointer()), w, h, rgba, flare,
      static_cast<int>(image.flare_extent.width), static_cast<int>(image.flare_extent.height),
      image.render_to_flare, image.flare, params);
  clock.Mark("FP6 display");
  cuda::CheckCuda(::cudaGetLastError(), "RecordCudaFilmPrint: kernel launch");
  clock.Finish();
}

}  // namespace alcedo
