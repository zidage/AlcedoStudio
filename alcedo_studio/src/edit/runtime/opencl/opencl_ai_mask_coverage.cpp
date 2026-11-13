//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM8.3: OpenCL dispatch of the AI Mask Feather and Expand (SM8 section 5.1). The kernels are in
// shader/ai_mask_coverage.cl, registered once as opencl_dag_ai_mask_coverage.

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

/// Layout of `AiMaskEdgeGrid` in ai_mask_coverage.cl.
struct AiMaskEdgeGrid {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t source_x      = 0;
  std::int32_t source_y      = 0;
  std::int32_t source_width  = 0;
  std::int32_t source_height = 0;
};

static_assert(sizeof(AiMaskEdgeGrid) == 24);

/// Layout of `AiMaskFeatherWeights` in ai_mask_coverage.cl. Passed by value: with the grid and
/// two images the arguments stay below the 1024-byte minimum CL_DEVICE_MAX_PARAMETER_SIZE.
struct FeatherWeightsArgument {
  std::int32_t radius = 0;
  float        values[kAiMaskFeatherMaximumRadius + 1]{};
};

static_assert(sizeof(FeatherWeightsArgument) == 808);

constexpr std::size_t kLocalSize = 16;

void RequireTexture(const OpenClBackend::Texture2D* texture, Extent2D extent, TextureFormat format,
                    const char* name) {
  if (texture == nullptr || texture->Native() == nullptr) {
    throw std::runtime_error(std::string{"OpenClBackend::RecordAiMaskEdge: the "} + name +
                             " texture is missing");
  }
  if (texture->Width() != extent.width || texture->Height() != extent.height ||
      texture->Format() != format) {
    throw std::runtime_error(std::string{"OpenClBackend::RecordAiMaskEdge: the "} + name +
                             " texture is not " + std::to_string(extent.width) + " x " +
                             std::to_string(extent.height) + " " + TextureFormatName(format));
  }
}

template <class Value>
void SetArgument(cl_kernel kernel, cl_uint index, const Value& value, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(Value), &value), label);
}

/// Grid of a pass whose input is @p input placed at (@p x, @p y) in the output.
auto MakeGrid(Extent2D output, const OpenClBackend::Texture2D& input, std::int32_t x,
              std::int32_t y) -> AiMaskEdgeGrid {
  return AiMaskEdgeGrid{
      static_cast<std::int32_t>(output.width),  static_cast<std::int32_t>(output.height), x, y,
      static_cast<std::int32_t>(input.Width()), static_cast<std::int32_t>(input.Height())};
}

}  // namespace

void OpenClBackend::RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                                     const AiMaskEdgeTextures<Texture2D>& textures,
                                     CommandContext&                      command_context) {
  const auto extent = dispatch.output_extent;
  if (extent.Empty() || (dispatch.expand_steps == 0 && !dispatch.HasFeather())) {
    throw std::runtime_error("OpenClBackend::RecordAiMaskEdge: the dispatch has no work");
  }
  if (static_cast<int>(dispatch.feather_weights.size()) > kAiMaskFeatherMaximumRadius + 1) {
    throw std::runtime_error("OpenClBackend::RecordAiMaskEdge: the Feather radius is too large");
  }
  RequireTexture(textures.source, dispatch.source_extent, TextureFormat::R8, "source");
  RequireTexture(textures.output, extent, TextureFormat::R8, "output");
  for (std::uint32_t i = 0; i < dispatch.MorphologyWorkTextureCount(); ++i) {
    RequireTexture(textures.morphology[i], extent, TextureFormat::R8, "morphology work");
  }
  if (dispatch.HasFeather()) {
    RequireTexture(textures.feather_rows, extent, TextureFormat::R32f, "Feather work");
  }

  const std::size_t global[2] = {((extent.width + kLocalSize - 1) / kLocalSize) * kLocalSize,
                                 ((extent.height + kLocalSize - 1) / kLocalSize) * kLocalSize};
  const std::size_t local[2]  = {kLocalSize, kLocalSize};
  const auto        enqueue   = [&](cl_kernel kernel, const char* label) {
    cl_event event = nullptr;
    CheckOpenCl(
        clEnqueueNDRangeKernel(queue_, kernel, 2, nullptr, global, local, 0, nullptr, &event),
        label);
    NoteOpenClEnqueueNdRange();
    TrackKernelEvent(command_context, event);
  };

  auto&            kernels = OpenClKernelCache::Instance();
  const Texture2D* current = textures.source;
  AiMaskEdgeGrid   grid    = MakeGrid(extent, *current, dispatch.source_x, dispatch.source_y);
  if (dispatch.expand_steps > 0) {
    const auto         kernel = kernels.GetKernel(OpenCL::GpuDag::kAiMaskCoverageProgramName,
                                                  OpenCL::GpuDag::kAiMaskMorphologyKernelName);
    const std::int32_t dilate = dispatch.dilate ? 1 : 0;
    for (std::int32_t step = 0; step < dispatch.expand_steps; ++step) {
      const bool last_writes_output = !dispatch.HasFeather() && step + 1 == dispatch.expand_steps;
      Texture2D& target = last_writes_output ? *textures.output : *textures.morphology[step % 2];
      const std::int32_t square = step % 2;
      SetArgument(kernel, 0, current->Native(), "AI Mask morphology input");
      SetArgument(kernel, 1, target.Native(), "AI Mask morphology output");
      SetArgument(kernel, 2, grid, "AI Mask morphology grid");
      SetArgument(kernel, 3, dilate, "AI Mask morphology operation");
      SetArgument(kernel, 4, square, "AI Mask morphology kernel shape");
      enqueue(kernel, "AI Mask morphology");
      current = &target;
      grid    = MakeGrid(extent, target, 0, 0);
    }
  }

  if (dispatch.HasFeather()) {
    FeatherWeightsArgument weights;
    weights.radius = static_cast<std::int32_t>(dispatch.feather_weights.size()) - 1;
    for (std::size_t k = 0; k < dispatch.feather_weights.size(); ++k) {
      weights.values[k] = dispatch.feather_weights[k];
    }
    const auto rows_kernel = kernels.GetKernel(OpenCL::GpuDag::kAiMaskCoverageProgramName,
                                               OpenCL::GpuDag::kAiMaskFeatherRowsKernelName);
    SetArgument(rows_kernel, 0, current->Native(), "AI Mask Feather rows input");
    SetArgument(rows_kernel, 1, textures.feather_rows->Native(), "AI Mask Feather rows output");
    SetArgument(rows_kernel, 2, grid, "AI Mask Feather rows grid");
    SetArgument(rows_kernel, 3, weights, "AI Mask Feather rows weights");
    enqueue(rows_kernel, "AI Mask Feather rows");

    const auto columns_kernel = kernels.GetKernel(OpenCL::GpuDag::kAiMaskCoverageProgramName,
                                                  OpenCL::GpuDag::kAiMaskFeatherColumnsKernelName);
    const auto columns_grid   = MakeGrid(extent, *textures.feather_rows, 0, 0);
    SetArgument(columns_kernel, 0, textures.feather_rows->Native(),
                "AI Mask Feather columns input");
    SetArgument(columns_kernel, 1, textures.output->Native(), "AI Mask Feather columns output");
    SetArgument(columns_kernel, 2, columns_grid, "AI Mask Feather columns grid");
    SetArgument(columns_kernel, 3, weights, "AI Mask Feather columns weights");
    enqueue(columns_kernel, "AI Mask Feather columns");
  }
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
