//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>

#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
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

}  // namespace alcedo

#endif  // HAVE_OPENCL
