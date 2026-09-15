//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <string>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {

inline void EnqueueOpenClNeighborRange(OpenClRenderDevice& device, cl_kernel kernel,
                                       std::uint32_t width, std::uint32_t height,
                                       std::size_t local_edge) {
  const std::size_t local[2]  = {local_edge, local_edge};
  const std::size_t global[2] = {
      ((static_cast<std::size_t>(width) + local_edge - 1) / local_edge) * local_edge,
      ((static_cast<std::size_t>(height) + local_edge - 1) / local_edge) * local_edge};
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "OpenCL neighborhood enqueue");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

inline void EnqueueOpenClNeighborHorizontal(OpenClRenderDevice& device,
                                            const OpenClBackend::Texture2D& src,
                                            OpenClBackend::Texture2D& blur,
                                            const GradeNeighborParams& params, std::uint32_t width,
                                            std::uint32_t height) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kPrimaryGradeProgramName, OpenCL::GpuDag::kPrimaryGradeNeighborBlurKernelName);
  const auto src_mem  = src.Native();
  const auto blur_mem = blur.Native();
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL neighborhood source argument");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL neighborhood blur argument");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params),
              "OpenCL neighborhood parameters");
  EnqueueOpenClNeighborRange(device, kernel, width, height, 8);
}

inline void EnqueueOpenClNeighborVertical(OpenClRenderDevice& device,
                                          const OpenClBackend::Texture2D& src,
                                          const OpenClBackend::Texture2D& blur,
                                          OpenClBackend::Texture2D& dst,
                                          const GradeNeighborParams& params, std::uint32_t width,
                                          std::uint32_t height) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kPrimaryGradeProgramName,
      OpenCL::GpuDag::kPrimaryGradeNeighborApplyKernelName);
  const auto src_mem  = src.Native();
  const auto blur_mem = blur.Native();
  const auto dst_mem  = dst.Native();
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL neighborhood apply source argument");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL neighborhood apply blur argument");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &dst_mem),
              "OpenCL neighborhood apply destination argument");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params),
              "OpenCL neighborhood apply parameters");
  const auto radius      = NeighborhoodVerticalRadius(params);
  constexpr std::size_t kLocalEdge = 8;
  const auto local_bytes = kLocalEdge * (kLocalEdge + 2U * radius) * 4U * sizeof(float);
  CheckOpenCl(clSetKernelArg(kernel, 4, local_bytes, nullptr),
              "OpenCL neighborhood local tile argument");
  EnqueueOpenClNeighborRange(device, kernel, width, height, kLocalEdge);
}

inline void BindOpenClSceneView(cl_kernel kernel, cl_uint start, const OpenClSceneView& view,
                                OpenClBackend& backend, const char* label) {
  cl_mem image  = view.is_buffer ? backend.DummySceneImage() : view.native;
  cl_mem buffer = view.is_buffer ? view.native : backend.DummySceneBuffer();
  int    is_buf = view.is_buffer ? 1 : 0;
  CheckOpenCl(clSetKernelArg(kernel, start, sizeof(cl_mem), &image),
              (std::string(label) + " image").c_str());
  CheckOpenCl(clSetKernelArg(kernel, start + 1, sizeof(cl_mem), &buffer),
              (std::string(label) + " buffer").c_str());
  CheckOpenCl(clSetKernelArg(kernel, start + 2, sizeof(int), &is_buf),
              (std::string(label) + " storage").c_str());
}

inline void EnqueueOpenClNeighborHorizontalScene(OpenClRenderDevice& device,
                                                 const OpenClSceneView& src,
                                                 OpenClBackend::Texture2D& blur,
                                                 const GradeNeighborParams& params,
                                                 std::uint32_t width, std::uint32_t height) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kPrimaryGradeProgramName,
      OpenCL::GpuDag::kPrimaryGradeNeighborBlurSceneKernelName);
  BindOpenClSceneView(kernel, 0, src, device.Workspace().Device(), "OpenCL neighborhood source");
  auto blur_mem = blur.Native();
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_mem), &blur_mem),
              "OpenCL neighborhood blur argument");
  CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params),
              "OpenCL neighborhood parameters");
  CheckOpenCl(clSetKernelArg(kernel, 5, sizeof(width), &width), "OpenCL neighborhood width");
  CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(height), &height), "OpenCL neighborhood height");
  EnqueueOpenClNeighborRange(device, kernel, width, height, 8);
}

inline void EnqueueOpenClNeighborVerticalScene(
    OpenClRenderDevice& device, const OpenClSceneView& src, const OpenClBackend::Texture2D& blur,
    const OpenClSceneView& dst, const OpenClSceneView* mix_source, const GraphValueId* mask_id,
    float mix, const GradeNeighborParams& params, std::uint32_t width, std::uint32_t height) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kPrimaryGradeProgramName,
      OpenCL::GpuDag::kPrimaryGradeNeighborApplySceneKernelName);
  auto& backend = device.Workspace().Device();
  BindOpenClSceneView(kernel, 0, src, backend, "OpenCL neighborhood apply source");
  auto blur_mem = blur.Native();
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_mem), &blur_mem),
              "OpenCL neighborhood apply blur argument");
  BindOpenClSceneView(kernel, 4, dst, backend, "OpenCL neighborhood apply destination");
  const bool apply_mix = mix_source != nullptr && (mix != 1.0f || mask_id != nullptr);
  if (apply_mix) {
    BindOpenClSceneView(kernel, 7, *mix_source, backend, "OpenCL neighborhood mix source");
  } else {
    BindOpenClSceneView(kernel, 7, OpenClSceneView{backend.DummySceneImage(), 1, 1, false}, backend,
                        "OpenCL neighborhood mix dummy");
    int no_mix = -1;
    CheckOpenCl(clSetKernelArg(kernel, 9, sizeof(int), &no_mix), "OpenCL neighborhood mix flag");
  }
  cl_mem mask_mem = backend.DummySceneImage();
  int    has_mask = 0;
  if (mask_id != nullptr) {
    auto* mask = device.Workspace().Images().Find(*mask_id);
    if (mask == nullptr || mask->Empty()) {
      throw std::runtime_error("OpenCL neighborhood mask is missing");
    }
    mask_mem = mask->Texture().Native();
    has_mask = 1;
  }
  CheckOpenCl(clSetKernelArg(kernel, 10, sizeof(cl_mem), &mask_mem),
              "OpenCL neighborhood mask argument");
  CheckOpenCl(clSetKernelArg(kernel, 11, sizeof(int), &has_mask),
              "OpenCL neighborhood mask flag");
  CheckOpenCl(clSetKernelArg(kernel, 12, sizeof(float), &mix), "OpenCL neighborhood mix");
  CheckOpenCl(clSetKernelArg(kernel, 13, sizeof(params), &params),
              "OpenCL neighborhood apply parameters");
  CheckOpenCl(clSetKernelArg(kernel, 14, sizeof(width), &width), "OpenCL neighborhood width");
  CheckOpenCl(clSetKernelArg(kernel, 15, sizeof(height), &height), "OpenCL neighborhood height");
  const auto radius            = NeighborhoodVerticalRadius(params);
  constexpr std::size_t kLocal = 8;
  const auto local_bytes = kLocal * (kLocal + 2U * radius) * 4U * sizeof(float);
  CheckOpenCl(clSetKernelArg(kernel, 16, local_bytes, nullptr),
              "OpenCL neighborhood local tile argument");
  EnqueueOpenClNeighborRange(device, kernel, width, height, kLocal);
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
