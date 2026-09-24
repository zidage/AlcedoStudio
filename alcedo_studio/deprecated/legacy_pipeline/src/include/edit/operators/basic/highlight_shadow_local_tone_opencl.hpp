//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <CL/cl.h>

#include <array>
#include <cstdint>
#include <vector>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/pipeline/highlight_shadow_local_tone.hpp"
#include "edit/pipeline/opencl_kernel_dispatch.hpp"

namespace alcedo::opencl {
class OpenClImage;
}

namespace alcedo::highlight_shadow_local_tone {

class OpenClStage final : public opencl_detail::OpenClKernelStage<OpenClStage> {
 public:
  static constexpr const char* kStageLabel = "OpenCL H/S local tone";

  OpenClStage();

  [[nodiscard]] auto ShouldRun(const FusedOperatorParams& params) const -> bool;

  void               EnsureKernels();
  void               Execute(const FusedOperatorParams& params, cl_mem fused_params_buffer,
                             const opencl::OpenClImage& src, opencl::OpenClImage& dst);
  void               ReleaseResources();

 private:
  void ReleasePyramidBuffers();
  void EnsurePyramidBuffers(int width, int height, float radius);

  void EnqueueExtractLogIntensity(const opencl::OpenClImage& src);
  void EnqueueExtractLogIntensityResampled(const opencl::OpenClImage& src);
  void EnqueuePyrDown(cl_mem src, int src_width, int src_height, cl_mem dst, int dst_width,
                      int dst_height);
  void BuildSourcePyramid(const opencl::OpenClImage& src);
  void BuildRemapPyramid(const LlfSample& sample, std::array<cl_mem, kMaxLevels>& remap_levels);
  void BuildOutputPyramid(const std::vector<LlfSample>& samples);

  void EnqueueApplyAdjustedL(const opencl::OpenClImage& src, opencl::OpenClImage& dst);
  void EnqueueApplyAdjustedLFromFrame(const opencl::OpenClImage& src, opencl::OpenClImage& dst);
  void EnqueueApplyAdjustedLFromReference(const opencl::OpenClImage& src, opencl::OpenClImage& dst,
                                          cl_mem fused_params_buffer);

  opencl_detail::OpenClKernelHandle extract_kernel_;
  opencl_detail::OpenClKernelHandle extract_resampled_kernel_;
  opencl_detail::OpenClKernelHandle build_remapped_sample_kernel_;
  opencl_detail::OpenClKernelHandle pyr_down_kernel_;
  opencl_detail::OpenClKernelHandle select_interpolated_level_kernel_;
  opencl_detail::OpenClKernelHandle collapse_level_kernel_;
  opencl_detail::OpenClKernelHandle apply_adjusted_l_kernel_;
  opencl_detail::OpenClKernelHandle apply_adjusted_l_from_frame_kernel_;
  opencl_detail::OpenClKernelHandle apply_adjusted_l_from_reference_kernel_;

  std::array<cl_mem, kMaxLevels>    source_levels_         = {};
  std::array<cl_mem, kMaxLevels>    remap_a_levels_        = {};
  std::array<cl_mem, kMaxLevels>    remap_b_levels_        = {};
  std::array<cl_mem, kMaxLevels>    output_levels_         = {};
  std::array<int, kMaxLevels>       level_widths_          = {};
  std::array<int, kMaxLevels>       level_heights_         = {};
  int                               level_count_           = 0;
  int                               cached_width_          = 0;
  int                               cached_height_         = 0;
  int                               cached_frame_width_    = 0;
  int                               cached_frame_height_   = 0;
  int                               cached_pitch_          = 0;
  std::uint64_t                     cached_source_key_     = 0;
  std::uint64_t                     cached_key_            = 0;
  bool                              cached_reference_base_ = false;
};

}  // namespace alcedo::highlight_shadow_local_tone

#endif  // HAVE_OPENCL
