//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <array>
#include <cstdint>
#include <vector>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/pipeline/highlight_shadow_local_tone.hpp"
#include "edit/pipeline/metal_kernel_dispatch.hpp"
#include "edit/pipeline/metal_pipeline_stats.hpp"

namespace alcedo::metal {
class MetalImage;
}

namespace alcedo::highlight_shadow_local_tone {

class MetalStage final : public metal_detail::MetalKernelStage<MetalStage> {
 public:
  static constexpr const char* kStageLabel = "Metal H/S local tone";

  MetalStage();

  [[nodiscard]] auto ShouldRun(const FusedOperatorParams& params) const -> bool;

  void               Execute(const FusedOperatorParams& params, MTL::Buffer* fused_params_buffer,
                             MTL::CommandBuffer* command_buffer, const metal::MetalImage& src,
                             metal::MetalImage& dst, MetalExecutionStats* stats);
  void               ReleaseResources();

 private:
  void InvalidateBaseCache();
  void ReleasePyramidBuffers();
  void EnsurePyramidBuffers(int32_t width, int32_t height, float radius);
  void EnsureSamplePyramidBuffers(int32_t sample_count);

  void EncodeExtractLogIntensity(MTL::CommandBuffer* command_buffer, const metal::MetalImage& src);
  void EncodeExtractLogIntensityResampled(MTL::CommandBuffer*      command_buffer,
                                          const metal::MetalImage& src);
  void EncodePyrDown(MTL::CommandBuffer* command_buffer, MTL::Buffer* src, int32_t src_width,
                     int32_t src_height, int32_t src_offset, MTL::Buffer* dst, int32_t dst_width,
                     int32_t dst_height, int32_t dst_offset);
  void BuildSourcePyramid(MTL::CommandBuffer* command_buffer, const metal::MetalImage& src);
  void BuildRemapPyramid(MTL::CommandBuffer* command_buffer, const LlfSample& sample,
                         std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels>& remap_levels);
  void BuildPackedSamplePyramids(MTL::CommandBuffer*           command_buffer,
                                 const std::vector<LlfSample>& samples);
  void BuildOutputPyramid(MTL::CommandBuffer* command_buffer, const std::vector<LlfSample>& samples,
                          MetalExecutionStats* stats);

  void EncodeApplyAdjustedL(MTL::CommandBuffer* command_buffer, const metal::MetalImage& src,
                            metal::MetalImage& dst);
  void EncodeApplyAdjustedLFromFrame(MTL::CommandBuffer*      command_buffer,
                                     const metal::MetalImage& src, metal::MetalImage& dst);
  void EncodeApplyAdjustedLFromReference(MTL::CommandBuffer*      command_buffer,
                                         const metal::MetalImage& src, metal::MetalImage& dst,
                                         MTL::Buffer* fused_params_buffer);

  metal_detail::MetalKernelHandle                    extract_pipeline_;
  metal_detail::MetalKernelHandle                    extract_resampled_pipeline_;
  metal_detail::MetalKernelHandle                    build_remapped_sample_pipeline_;
  metal_detail::MetalKernelHandle                    build_remapped_samples_packed_pipeline_;
  metal_detail::MetalKernelHandle                    pyr_down_pipeline_;
  metal_detail::MetalKernelHandle                    pyr_down_packed_pipeline_;
  metal_detail::MetalKernelHandle                    select_level_pipeline_;
  metal_detail::MetalKernelHandle                    select_level_packed_pipeline_;
  metal_detail::MetalKernelHandle                    collapse_level_pipeline_;
  metal_detail::MetalKernelHandle                    apply_adjusted_l_pipeline_;
  metal_detail::MetalKernelHandle                    apply_adjusted_l_from_frame_pipeline_;
  metal_detail::MetalKernelHandle                    apply_adjusted_l_from_reference_pipeline_;

  std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels> source_levels_         = {};
  std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels> remap_a_levels_        = {};
  std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels> sample_levels_         = {};
  std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels> output_levels_         = {};
  std::array<int32_t, kMaxLevels>                    level_widths_          = {};
  std::array<int32_t, kMaxLevels>                    level_heights_         = {};
  int32_t                                            level_count_           = 0;
  int32_t                                            sample_count_          = 0;
  int32_t                                            cached_width_          = 0;
  int32_t                                            cached_height_         = 0;
  int32_t                                            cached_frame_width_    = 0;
  int32_t                                            cached_frame_height_   = 0;
  int32_t                                            cached_pitch_          = 0;
  std::uint64_t                                      cached_source_key_     = 0;
  std::uint64_t                                      cached_key_            = 0;
  bool                                               cached_reference_base_ = false;
};

}  // namespace alcedo::highlight_shadow_local_tone

#endif  // HAVE_METAL
