//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_diffusion_filter_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

/// Mirrors `DiffusionReduceParams` in drt.cl.
struct ReduceParams {
  std::int32_t src_width  = 0;
  std::int32_t src_height = 0;
  std::int32_t dst_width  = 0;
  std::int32_t dst_height = 0;
  std::int32_t samples    = 1;
  float        gain       = 0.0f;
  float        knee       = 0.0f;
  float        pad0       = 0.0f;
  float        base_to_render[12]{};
};
static_assert(sizeof(ReduceParams) == 80);

/// Mirrors `DiffusionMixParams` in drt.cl.
struct MixParams {
  std::int32_t width            = 0;
  std::int32_t height           = 0;
  float        scatter_fraction = 0.0f;
  float        transmission     = 1.0f;
  float        render_to_base[12]{};
};
static_assert(sizeof(MixParams) == 64);

void CopyMatrix(float* destination, const Matrix3x3& matrix) {
  for (int index = 0; index < 9; ++index) {
    destination[index] = matrix.m[index];
  }
}

auto Kernel(const char* name) -> cl_kernel {
  return OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDrtProgramName, name);
}

void SetMem(cl_kernel kernel, cl_uint index, cl_mem value, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_mem), &value), label);
}

template <typename Value>
void SetValue(cl_kernel kernel, cl_uint index, const Value& value, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(value), &value), label);
}

void Dispatch2D(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                std::uint32_t height, const char* label) {
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  auto&             backend   = device.Workspace().Device();
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, global, local, 0,
                                     nullptr, &event),
              label);
  NoteOpenClEnqueueNdRange();
  backend.TrackKernelEvent(device.CommandContext(), event);
}

auto Native(ResourceLease<OpenClBackend>& lease) -> cl_mem { return lease.Texture().Native(); }

void EnqueueDecode(OpenClRenderDevice& device, const FrameSceneBinding& scene,
                   const FrameSceneBinding& output, std::uint32_t width, std::uint32_t height) {
  auto  kernel  = Kernel(OpenCL::GpuDag::kDiffusionFilterDecodeSceneKernelName);
  auto& backend = device.Workspace().Device();
  BindOpenClSceneView(kernel, 0, OpenClBindScene(device, scene), backend,
                      "OpenCL DiffusionFilter source", OpenClSceneArgAccess::Read);
  BindOpenClSceneView(kernel, 3, OpenClBindScene(device, output), backend,
                      "OpenCL DiffusionFilter destination", OpenClSceneArgAccess::Write);
  SetValue(kernel, 6, width, "ExecuteOpenClDiffusionFilter: width");
  SetValue(kernel, 7, height, "ExecuteOpenClDiffusionFilter: height");
  Dispatch2D(device, kernel, width, height, "ExecuteOpenClDiffusionFilter: decode enqueue");
}

/**
 * @brief Build the scatter image of @p layout from the render @p scene into @p target.
 *
 * @p target has the base-level extent. Pyramid levels are pooled scratch leases, returned at
 * scope exit; later users run on the same in-order queue, so these kernels finish before any
 * reuse.
 */
void BuildScatter(OpenClRenderDevice& device, const FrameSceneBinding& scene, std::uint32_t width,
                  std::uint32_t height, const DiffusionFilterLayout& layout,
                  const DiffusionScatterMapping& mapping, cl_mem target) {
  auto&                                     workspace = device.Workspace();
  const auto                                count     = layout.level_count;
  std::vector<ResourceLease<OpenClBackend>> leases;
  leases.reserve(2U * count);
  auto acquire = [&](ImageExtent extent) {
    leases.push_back(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    return Native(leases.back());
  };
  // With one level the reduction is the scatter image. Otherwise the level-0 accumulation is.
  std::vector<cl_mem> levels(count, nullptr);
  std::vector<cl_mem> accumulated(count, nullptr);
  for (std::uint32_t index = 0; index < count; ++index) {
    levels[index] = count == 1 ? target : acquire(layout.extents[index]);
    if (index + 1 < count) {
      accumulated[index] = index == 0 ? target : acquire(layout.extents[index]);
    }
  }

  const auto base = layout.extents[0];
  {
    auto         kernel = Kernel(OpenCL::GpuDag::kDiffusionFilterReduceBoostKernelName);
    ReduceParams params;
    params.src_width  = static_cast<std::int32_t>(width);
    params.src_height = static_cast<std::int32_t>(height);
    params.dst_width  = static_cast<std::int32_t>(base.width);
    params.dst_height = static_cast<std::int32_t>(base.height);
    params.samples    = static_cast<std::int32_t>(mapping.reduce_samples);
    params.gain       = layout.highlight_gain;
    params.knee       = layout.highlight_knee;
    CopyMatrix(params.base_to_render, mapping.base_to_render);
    BindOpenClSceneView(kernel, 0, OpenClBindScene(device, scene), workspace.Device(),
                        "OpenCL DiffusionFilter reduce source", OpenClSceneArgAccess::Read);
    SetMem(kernel, 3, levels[0], "ExecuteOpenClDiffusionFilter: reduce destination");
    SetValue(kernel, 4, params, "ExecuteOpenClDiffusionFilter: reduce parameters");
    Dispatch2D(device, kernel, base.width, base.height,
               "ExecuteOpenClDiffusionFilter: reduce enqueue");
  }
  for (std::uint32_t index = 1; index < count; ++index) {
    const auto   to         = layout.extents[index];
    auto         kernel     = Kernel(OpenCL::GpuDag::kDiffusionFilterDownsampleKernelName);
    std::int32_t dst_width  = static_cast<std::int32_t>(to.width);
    std::int32_t dst_height = static_cast<std::int32_t>(to.height);
    SetMem(kernel, 0, levels[index - 1], "ExecuteOpenClDiffusionFilter: downsample source");
    SetMem(kernel, 1, levels[index], "ExecuteOpenClDiffusionFilter: downsample destination");
    SetValue(kernel, 2, dst_width, "ExecuteOpenClDiffusionFilter: downsample width");
    SetValue(kernel, 3, dst_height, "ExecuteOpenClDiffusionFilter: downsample height");
    Dispatch2D(device, kernel, to.width, to.height,
               "ExecuteOpenClDiffusionFilter: downsample enqueue");
  }

  // Accumulate from the coarsest level. The coarsest level enters with its own weight.
  cl_mem coarse        = levels[count - 1];
  float  coarse_weight = layout.weights[count - 1];
  for (std::uint32_t index = count - 1; index-- > 0;) {
    const auto   extent      = layout.extents[index];
    auto         kernel      = Kernel(OpenCL::GpuDag::kDiffusionFilterUpsampleAccumulateKernelName);
    std::int32_t level_width = static_cast<std::int32_t>(extent.width);
    std::int32_t level_height = static_cast<std::int32_t>(extent.height);
    const float  level_weight = layout.weights[index];
    SetMem(kernel, 0, coarse, "ExecuteOpenClDiffusionFilter: upsample coarse");
    SetValue(kernel, 1, coarse_weight, "ExecuteOpenClDiffusionFilter: upsample coarse weight");
    SetMem(kernel, 2, levels[index], "ExecuteOpenClDiffusionFilter: upsample level");
    SetMem(kernel, 3, accumulated[index], "ExecuteOpenClDiffusionFilter: upsample destination");
    SetValue(kernel, 4, level_width, "ExecuteOpenClDiffusionFilter: upsample width");
    SetValue(kernel, 5, level_height, "ExecuteOpenClDiffusionFilter: upsample height");
    SetValue(kernel, 6, level_weight, "ExecuteOpenClDiffusionFilter: upsample level weight");
    Dispatch2D(device, kernel, extent.width, extent.height,
               "ExecuteOpenClDiffusionFilter: upsample enqueue");
    coarse        = accumulated[index];
    coarse_weight = 1.0f;
  }
}

void EnqueueMix(OpenClRenderDevice& device, const FrameSceneBinding& scene,
                const FrameSceneBinding& output, std::uint32_t width, std::uint32_t height,
                cl_mem scatter, const DiffusionFilterLayout& layout,
                const DiffusionScatterMapping& mapping) {
  auto      kernel  = Kernel(OpenCL::GpuDag::kDiffusionFilterMixSceneKernelName);
  auto&     backend = device.Workspace().Device();
  MixParams params;
  params.width            = static_cast<std::int32_t>(width);
  params.height           = static_cast<std::int32_t>(height);
  params.scatter_fraction = layout.scatter_fraction;
  params.transmission     = layout.transmission;
  CopyMatrix(params.render_to_base, mapping.render_to_base);
  BindOpenClSceneView(kernel, 0, OpenClBindScene(device, scene), backend,
                      "OpenCL DiffusionFilter mix source", OpenClSceneArgAccess::Read);
  BindOpenClSceneView(kernel, 3, OpenClBindScene(device, output), backend,
                      "OpenCL DiffusionFilter mix destination", OpenClSceneArgAccess::Write);
  SetMem(kernel, 6, scatter, "ExecuteOpenClDiffusionFilter: mix scatter");
  SetValue(kernel, 7, params, "ExecuteOpenClDiffusionFilter: mix parameters");
  Dispatch2D(device, kernel, width, height, "ExecuteOpenClDiffusionFilter: mix enqueue");
}

}  // namespace

auto ExecuteOpenClDiffusionFilter(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                  const PipelineDocument& document, const FrameSceneBinding& scene)
    -> FrameSceneBinding {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClDiffusionFilter: BeginRender has not been called");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteOpenClDiffusionFilter: missing DRT endpoint");
  }
  const auto output = FrameSceneBinding::WorkImage(DestinationWorkMember(scene));
  const auto width  = OpenClSceneWidth(device, scene);
  const auto height = OpenClSceneHeight(device, scene);
  if (OpenClSceneWidth(device, output) != width || OpenClSceneHeight(device, output) != height) {
    throw std::runtime_error(
        "ExecuteOpenClDiffusionFilter: scene-work extent does not match scene");
  }

  const float strength = drt->Params().DiffusionStrength();
  if (!IsDiffusionFilterActive(strength)) {
    EnqueueDecode(device, scene, output, width, height);
    return output;
  }

  const auto& geometry = plan.geometry;
  const auto  layout   = MakeDiffusionFilterLayout(
      DiffusionCanvasExtent(geometry.full_reference_extent), ResolveDiffusionFilterShape(strength));
  const auto                    mapping      = MakeDiffusionScatterMapping(geometry, layout);
  const auto                    base         = layout.extents[0];

  // The scatter image covers the full frame. A full-edit render builds and publishes it; an
  // ROI render samples the published image, so light outside the viewport still scatters in.
  auto&                         invalidation = workspace.ResultInvalidation();
  const auto                    scatter_id   = DiffusionScatterId(plan.drt.node_id);
  const bool                    persist      = workspace.PersistsResult(scatter_id);
  const bool                    full_edit    = CoversFullEditSpace(geometry);
  const auto                    long_edge    = (std::max)(width, height);
  ResourceLease<OpenClBackend>* canonical    = nullptr;
  if (persist) {
    const auto needed =
        invalidation.MakeImageRepresentation(scatter_id, base, TextureFormat::Rgba32f,
                                             DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical =
        workspace.Images().BindValidResult(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           needed, workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);

  cl_mem     scatter  = nullptr;
  std::optional<ResourceLease<OpenClBackend>> transient_scatter;
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    scatter = Native(*canonical);
    ++device.PassStats().diffusion_scatter_sample;
  } else {
    cl_mem target = nullptr;
    if (decision.persist_canonical) {
      target = Native(workspace.AcquireImageForWrite(
          scatter_id, {base.width, base.height, TextureFormat::Rgba32f}));
    } else {
      transient_scatter.emplace(
          workspace.Textures().Acquire({base.width, base.height, TextureFormat::Rgba32f}));
      target = Native(*transient_scatter);
    }
    BuildScatter(device, scene, width, height, layout, mapping, target);
    if (decision.persist_canonical) {
      const auto published = invalidation.MakeImageRepresentation(
          scatter_id, base, TextureFormat::Rgba32f, decision.current_long_edge);
      workspace.Images().RecordUnpublished(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           published, device.CommandContext().SubmissionId(),
                                           decision.current_long_edge);
    }
    scatter = target;
    ++device.PassStats().diffusion_scatter_rebuild;
  }

  EnqueueMix(device, scene, output, width, height, scatter, layout, mapping);
  return output;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
