//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/operators/basic/highlight_shadow_local_tone_opencl.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/pipeline/opencl_pipeline_programs.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo::highlight_shadow_local_tone {

OpenClStage::OpenClStage()
    : extract_kernel_(OpenCL::Pipeline::kDetailProgramName,
                      OpenCL::Pipeline::kHsExtractLogIntensityKernelName),
      extract_resampled_kernel_(OpenCL::Pipeline::kDetailProgramName,
                                OpenCL::Pipeline::kHsExtractLogIntensityResampledKernelName),
      build_remapped_sample_kernel_(OpenCL::Pipeline::kDetailProgramName,
                                    OpenCL::Pipeline::kHsBuildRemappedSampleKernelName),
      pyr_down_kernel_(OpenCL::Pipeline::kDetailProgramName,
                       OpenCL::Pipeline::kHsPyrDownKernelName),
      select_interpolated_level_kernel_(OpenCL::Pipeline::kDetailProgramName,
                                        OpenCL::Pipeline::kHsSelectInterpolatedLevelKernelName),
      collapse_level_kernel_(OpenCL::Pipeline::kDetailProgramName,
                             OpenCL::Pipeline::kHsCollapseLevelKernelName),
      apply_adjusted_l_kernel_(OpenCL::Pipeline::kDetailProgramName,
                               OpenCL::Pipeline::kHsApplyAdjustedLKernelName),
      apply_adjusted_l_from_frame_kernel_(OpenCL::Pipeline::kDetailProgramName,
                                          OpenCL::Pipeline::kHsApplyAdjustedLFromFrameKernelName),
      apply_adjusted_l_from_reference_kernel_(
          OpenCL::Pipeline::kDetailProgramName,
          OpenCL::Pipeline::kHsApplyAdjustedLFromReferenceKernelName) {}

auto OpenClStage::ShouldRun(const FusedOperatorParams& params) const -> bool {
  if (!params.hs_local_tone_enabled_) {
    return false;
  }
  const float shadow_amount = params.shadows_enabled_ ? params.shadows_offset_ : 0.0f;
  const float highlight_amount =
      params.highlights_enabled_
          ? std::clamp(-params.highlights_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  return highlight_shadow_local_tone::ShouldRun(shadow_amount, highlight_amount);
}

void OpenClStage::EnsureKernels() {
  extract_kernel_.Ensure(kStageLabel);
  extract_resampled_kernel_.Ensure(kStageLabel);
  build_remapped_sample_kernel_.Ensure(kStageLabel);
  pyr_down_kernel_.Ensure(kStageLabel);
  select_interpolated_level_kernel_.Ensure(kStageLabel);
  collapse_level_kernel_.Ensure(kStageLabel);
  apply_adjusted_l_kernel_.Ensure(kStageLabel);
  apply_adjusted_l_from_frame_kernel_.Ensure(kStageLabel);
  apply_adjusted_l_from_reference_kernel_.Ensure(kStageLabel);
}

void OpenClStage::ReleasePyramidBuffers() {
  for (cl_mem& buffer : source_levels_) {
    if (buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
  for (cl_mem& buffer : remap_a_levels_) {
    if (buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
  for (cl_mem& buffer : remap_b_levels_) {
    if (buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
  for (cl_mem& buffer : output_levels_) {
    if (buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
  level_widths_.fill(0);
  level_heights_.fill(0);
  level_count_           = 0;
  cached_width_          = 0;
  cached_height_         = 0;
  cached_frame_width_    = 0;
  cached_frame_height_   = 0;
  cached_pitch_          = 0;
  cached_source_key_     = 0;
  cached_key_            = 0;
  cached_reference_base_ = false;
}

void OpenClStage::ReleaseResources() {
  extract_kernel_.Release();
  extract_resampled_kernel_.Release();
  build_remapped_sample_kernel_.Release();
  pyr_down_kernel_.Release();
  select_interpolated_level_kernel_.Release();
  collapse_level_kernel_.Release();
  apply_adjusted_l_kernel_.Release();
  apply_adjusted_l_from_frame_kernel_.Release();
  apply_adjusted_l_from_reference_kernel_.Release();
  ReleasePyramidBuffers();
}

void OpenClStage::EnsurePyramidBuffers(int width, int height, float radius) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("OpenCL H/S local tone: invalid pyramid dimensions.");
  }

  const int                   new_level_count = ComputeLevelCount(width, height, radius);
  std::array<int, kMaxLevels> new_widths      = {};
  std::array<int, kMaxLevels> new_heights     = {};
  new_widths[0]                               = width;
  new_heights[0]                              = height;
  for (int level = 1; level < new_level_count; ++level) {
    new_widths[level]  = std::max(1, (new_widths[level - 1] + 1) / 2);
    new_heights[level] = std::max(1, (new_heights[level - 1] + 1) / 2);
  }

  bool layout_matches = level_count_ == new_level_count;
  for (int level = 0; layout_matches && level < new_level_count; ++level) {
    layout_matches = level_widths_[level] == new_widths[level] &&
                     level_heights_[level] == new_heights[level] &&
                     source_levels_[level] != nullptr && remap_a_levels_[level] != nullptr &&
                     remap_b_levels_[level] != nullptr && output_levels_[level] != nullptr;
  }
  if (layout_matches) {
    return;
  }

  ReleasePyramidBuffers();
  level_count_   = new_level_count;
  level_widths_  = new_widths;
  level_heights_ = new_heights;

  auto& context  = OpenClContext::Instance();
  for (int level = 0; level < level_count_; ++level) {
    const size_t elems =
        static_cast<size_t>(level_widths_[level]) * static_cast<size_t>(level_heights_[level]);
    cl_int err = CL_SUCCESS;
    source_levels_[level] =
        clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, elems * sizeof(float), nullptr, &err);
    if (err != CL_SUCCESS || source_levels_[level] == nullptr) {
      ReleasePyramidBuffers();
      throw std::runtime_error("OpenCL H/S local tone: failed to allocate source level.");
    }
    remap_a_levels_[level] =
        clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, elems * sizeof(float), nullptr, &err);
    if (err != CL_SUCCESS || remap_a_levels_[level] == nullptr) {
      ReleasePyramidBuffers();
      throw std::runtime_error("OpenCL H/S local tone: failed to allocate remap A level.");
    }
    remap_b_levels_[level] =
        clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, elems * sizeof(float), nullptr, &err);
    if (err != CL_SUCCESS || remap_b_levels_[level] == nullptr) {
      ReleasePyramidBuffers();
      throw std::runtime_error("OpenCL H/S local tone: failed to allocate remap B level.");
    }
    output_levels_[level] =
        clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, elems * sizeof(float), nullptr, &err);
    if (err != CL_SUCCESS || output_levels_[level] == nullptr) {
      ReleasePyramidBuffers();
      throw std::runtime_error("OpenCL H/S local tone: failed to allocate output level.");
    }
  }
}

void OpenClStage::EnqueueExtractLogIntensity(const opencl::OpenClImage& src) {
  cl_mem src_buf = src.Buffer();
  cl_mem dst_buf = source_levels_[0];
  cl_int width   = src.Width();
  cl_int height  = src.Height();

  SetKernelArgs(extract_kernel_.Get(), "extract log-intensity", src_buf, dst_buf, width, height);
  EnqueueKernel2D(extract_kernel_.Get(), width, height, "extract log-intensity kernel");
}

void OpenClStage::EnqueueExtractLogIntensityResampled(const opencl::OpenClImage& src) {
  cl_mem src_buf    = src.Buffer();
  cl_mem dst_buf    = source_levels_[0];
  cl_int src_width  = src.Width();
  cl_int src_height = src.Height();
  cl_int dst_width  = level_widths_[0];
  cl_int dst_height = level_heights_[0];

  SetKernelArgs(extract_resampled_kernel_.Get(), "resampled extract", src_buf, dst_buf, src_width,
                src_height, dst_width, dst_height);
  EnqueueKernel2D(extract_resampled_kernel_.Get(), dst_width, dst_height,
                  "resampled extract kernel");
}

void OpenClStage::EnqueuePyrDown(cl_mem src, int src_width, int src_height, cl_mem dst,
                                 int dst_width, int dst_height) {
  cl_int cl_src_width  = src_width;
  cl_int cl_src_height = src_height;
  cl_int cl_dst_width  = dst_width;
  cl_int cl_dst_height = dst_height;

  SetKernelArgs(pyr_down_kernel_.Get(), "pyr-down", src, dst, cl_src_width, cl_src_height,
                cl_dst_width, cl_dst_height);
  EnqueueKernel2D(pyr_down_kernel_.Get(), dst_width, dst_height, "pyr-down kernel");
}

void OpenClStage::BuildSourcePyramid(const opencl::OpenClImage& src) {
  if (level_widths_[0] == src.Width() && level_heights_[0] == src.Height()) {
    EnqueueExtractLogIntensity(src);
  } else {
    EnqueueExtractLogIntensityResampled(src);
  }
  for (int level = 1; level < level_count_; ++level) {
    EnqueuePyrDown(source_levels_[level - 1], level_widths_[level - 1], level_heights_[level - 1],
                   source_levels_[level], level_widths_[level], level_heights_[level]);
  }
}

void OpenClStage::BuildRemapPyramid(const LlfSample&                sample,
                                    std::array<cl_mem, kMaxLevels>& remap_levels) {
  cl_mem   src_buf = source_levels_[0];
  cl_mem   dst_buf = remap_levels[0];
  cl_int   width   = level_widths_[0];
  cl_int   height  = level_heights_[0];
  cl_float gamma   = sample.gamma;
  cl_float target  = sample.target;
  cl_float beta    = sample.beta;
  cl_float alpha   = sample.alpha;
  cl_float sigma_r = kBaseSigmaR;

  SetKernelArgs(build_remapped_sample_kernel_.Get(), "remapped-sample", src_buf, dst_buf, width,
                height, gamma, target, beta, alpha, sigma_r);
  EnqueueKernel2D(build_remapped_sample_kernel_.Get(), width, height, "remapped-sample kernel");

  for (int level = 1; level < level_count_; ++level) {
    EnqueuePyrDown(remap_levels[level - 1], level_widths_[level - 1], level_heights_[level - 1],
                   remap_levels[level], level_widths_[level], level_heights_[level]);
  }
}

void OpenClStage::BuildOutputPyramid(const std::vector<LlfSample>& samples) {
  auto&       context = OpenClContext::Instance();
  const float zero    = 0.0f;
  for (int level = 0; level < level_count_; ++level) {
    const size_t elems =
        static_cast<size_t>(level_widths_[level]) * static_cast<size_t>(level_heights_[level]);
    const cl_int err =
        clEnqueueFillBuffer(context.Queue(), output_levels_[level], &zero, sizeof(zero), 0,
                            elems * sizeof(float), 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
      throw std::runtime_error("OpenCL H/S local tone: failed to clear output level.");
    }
  }

  BuildRemapPyramid(samples.front(), remap_a_levels_);
  BuildRemapPyramid(samples[1], remap_b_levels_);

  for (size_t pair_index = 0; pair_index + 1 < samples.size(); ++pair_index) {
    for (int level = 0; level < level_count_; ++level) {
      const bool top_level        = level == (level_count_ - 1);
      cl_mem     source_level     = source_levels_[level];
      cl_mem     sample_lo_level  = remap_a_levels_[level];
      cl_mem     sample_lo_coarse = top_level ? remap_a_levels_[level] : remap_a_levels_[level + 1];
      cl_mem     sample_hi_level  = remap_b_levels_[level];
      cl_mem     sample_hi_coarse = top_level ? remap_b_levels_[level] : remap_b_levels_[level + 1];
      cl_mem     output_level     = output_levels_[level];
      cl_int     width            = level_widths_[level];
      cl_int     height           = level_heights_[level];
      cl_int     coarse_width     = top_level ? 1 : level_widths_[level + 1];
      cl_int     coarse_height    = top_level ? 1 : level_heights_[level + 1];
      cl_float   gamma_lo         = samples[pair_index].gamma;
      cl_float   gamma_hi         = samples[pair_index + 1].gamma;
      cl_int     first_pair       = pair_index == 0 ? 1 : 0;
      cl_int     last_pair        = pair_index + 2 == samples.size() ? 1 : 0;
      cl_int     top_level_arg    = top_level ? 1 : 0;

      SetKernelArgs(select_interpolated_level_kernel_.Get(), "select-level", source_level,
                    sample_lo_level, sample_lo_coarse, sample_hi_level, sample_hi_coarse,
                    output_level, width, height, coarse_width, coarse_height, gamma_lo, gamma_hi,
                    first_pair, last_pair, top_level_arg);
      EnqueueKernel2D(select_interpolated_level_kernel_.Get(), width, height,
                      "select-level kernel");
    }

    if (pair_index + 2 < samples.size()) {
      std::swap(remap_a_levels_, remap_b_levels_);
      BuildRemapPyramid(samples[pair_index + 2], remap_b_levels_);
    }
  }

  for (int level = level_count_ - 2; level >= 0; --level) {
    cl_mem lap_level     = output_levels_[level];
    cl_mem coarse_level  = output_levels_[level + 1];
    cl_mem dst_level     = remap_a_levels_[level];
    cl_int width         = level_widths_[level];
    cl_int height        = level_heights_[level];
    cl_int coarse_width  = level_widths_[level + 1];
    cl_int coarse_height = level_heights_[level + 1];

    SetKernelArgs(collapse_level_kernel_.Get(), "collapse-level", lap_level, coarse_level,
                  dst_level, width, height, coarse_width, coarse_height);
    EnqueueKernel2D(collapse_level_kernel_.Get(), width, height, "collapse-level kernel");
    std::swap(output_levels_[level], remap_a_levels_[level]);
  }
}

void OpenClStage::EnqueueApplyAdjustedL(const opencl::OpenClImage& src, opencl::OpenClImage& dst) {
  dst.Create(src.Width(), src.Height(), src.Type());

  cl_mem src_buf      = src.Buffer();
  cl_mem adjusted_buf = output_levels_[0];
  cl_mem dst_buf      = dst.Buffer();
  cl_int width        = src.Width();
  cl_int height       = src.Height();

  SetKernelArgs(apply_adjusted_l_kernel_.Get(), "apply adjusted-L", src_buf, adjusted_buf, dst_buf,
                width, height);
  EnqueueKernel2D(apply_adjusted_l_kernel_.Get(), width, height, "apply adjusted-L kernel");
}

void OpenClStage::EnqueueApplyAdjustedLFromFrame(const opencl::OpenClImage& src,
                                                 opencl::OpenClImage&       dst) {
  dst.Create(src.Width(), src.Height(), src.Type());

  cl_mem src_buf         = src.Buffer();
  cl_mem reference_buf   = source_levels_[0];
  cl_mem adjusted_buf    = output_levels_[0];
  cl_mem dst_buf         = dst.Buffer();
  cl_int width           = src.Width();
  cl_int height          = src.Height();
  cl_int adjusted_width  = cached_width_;
  cl_int adjusted_height = cached_height_;

  SetKernelArgs(apply_adjusted_l_from_frame_kernel_.Get(), "apply adjusted-L-from-frame", src_buf,
                reference_buf, adjusted_buf, dst_buf, width, height, adjusted_width,
                adjusted_height);
  EnqueueKernel2D(apply_adjusted_l_from_frame_kernel_.Get(), width, height,
                  "apply adjusted-L-from-frame kernel");
}

void OpenClStage::EnqueueApplyAdjustedLFromReference(const opencl::OpenClImage& src,
                                                     opencl::OpenClImage&       dst,
                                                     cl_mem fused_params_buffer) {
  dst.Create(src.Width(), src.Height(), src.Type());

  cl_mem src_buf         = src.Buffer();
  cl_mem reference_buf   = source_levels_[0];
  cl_mem adjusted_buf    = output_levels_[0];
  cl_mem dst_buf         = dst.Buffer();
  cl_int width           = src.Width();
  cl_int height          = src.Height();
  cl_int adjusted_width  = cached_width_;
  cl_int adjusted_height = cached_height_;

  SetKernelArgs(apply_adjusted_l_from_reference_kernel_.Get(), "apply adjusted-L-from-reference",
                src_buf, reference_buf, adjusted_buf, dst_buf, fused_params_buffer, width, height,
                adjusted_width, adjusted_height);
  EnqueueKernel2D(apply_adjusted_l_from_reference_kernel_.Get(), width, height,
                  "apply adjusted-L-from-reference kernel");
}

void OpenClStage::Execute(const FusedOperatorParams& params, cl_mem fused_params_buffer,
                          const opencl::OpenClImage& src, opencl::OpenClImage& dst) {
  const float shadow_amount = params.shadows_enabled_ ? params.shadows_offset_ : 0.0f;
  const float highlight_amount =
      params.highlights_enabled_
          ? std::clamp(-params.highlights_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  const std::uint64_t adjusted_cache_key =
      BuildAdjustedResultCacheKey(params, shadow_amount, highlight_amount);

  const bool roi_frame_with_source_reference = params.render_roi_enabled_ &&
                                               params.render_roi_reference_width_ > 0 &&
                                               params.render_roi_reference_height_ > 0;
  const bool preserve_source_detail = params.render_hs_preserve_source_detail_;
  const int  reference_max_long_edge =
      std::max(1, std::min(params.render_hs_reference_max_long_edge_, kReferenceMaskMaxLongEdge));
  const MaskDimensions current_reference_dims =
      ComputeMaskDimensions(src.Width(), src.Height(), reference_max_long_edge);
  const std::uint64_t reference_source_cache_key = params.hs_mask_base_cache_key_;
  std::uint64_t       reference_cache_key        = adjusted_cache_key;
  HashCombine(reference_cache_key, static_cast<std::uint64_t>(preserve_source_detail));
  const bool reference_source_cache_valid =
      cached_reference_base_ && source_levels_[0] != nullptr &&
      cached_source_key_ == reference_source_cache_key && cached_width_ > 0 && cached_height_ > 0 &&
      cached_frame_width_ > 0 && cached_frame_height_ > 0 && cached_pitch_ > 0;
  const bool reference_result_cache_valid = reference_source_cache_valid &&
                                            output_levels_[0] != nullptr &&
                                            cached_key_ == reference_cache_key;
  const int current_reference_long_edge =
      std::max(current_reference_dims.width, current_reference_dims.height);
  const int  cached_reference_long_edge = std::max(cached_width_, cached_height_);
  const bool current_can_improve_reference =
      params.render_hs_can_seed_reference_ &&
      current_reference_long_edge > cached_reference_long_edge;
  const auto ensure_reference_output = [&]() {
    if (!reference_result_cache_valid) {
      const auto samples = BuildSamples(shadow_amount, highlight_amount);
      BuildOutputPyramid(samples);
      cached_key_ = reference_cache_key;
    }
  };
  if (CanReuseReferenceForRoi(roi_frame_with_source_reference, reference_source_cache_valid,
                              params.render_roi_reference_width_,
                              params.render_roi_reference_height_)) {
    ensure_reference_output();
    EnqueueApplyAdjustedLFromReference(src, dst, fused_params_buffer);
    return;
  }
  if (!roi_frame_with_source_reference && reference_source_cache_valid &&
      !current_can_improve_reference &&
      (cached_frame_width_ != src.Width() || cached_frame_height_ != src.Height())) {
    ensure_reference_output();
    EnqueueApplyAdjustedLFromFrame(src, dst);
    return;
  }

  const bool build_roi_local_reference = roi_frame_with_source_reference;
  const bool seed_canonical_reference =
      params.render_hs_can_seed_reference_ && !build_roi_local_reference;
  std::uint64_t        render_cache_key = build_roi_local_reference
                                              ? BuildRoiAdjustedResultCacheKey(params, reference_cache_key)
                                              : reference_cache_key;
  const MaskDimensions mask_dims        = current_reference_dims;
  EnsurePyramidBuffers(mask_dims.width, mask_dims.height, params.hs_base_radius_);
  const bool cache_valid = output_levels_[0] != nullptr && cached_key_ == render_cache_key &&
                           cached_frame_width_ == src.Width() &&
                           cached_frame_height_ == src.Height() &&
                           cached_width_ == mask_dims.width && cached_height_ == mask_dims.height &&
                           cached_pitch_ == level_widths_[0];
  if (!cache_valid) {
    const auto samples = BuildSamples(shadow_amount, highlight_amount);
    BuildSourcePyramid(src);
    BuildOutputPyramid(samples);
    cached_key_            = render_cache_key;
    cached_source_key_     = seed_canonical_reference ? reference_source_cache_key : 0;
    cached_width_          = mask_dims.width;
    cached_height_         = mask_dims.height;
    cached_frame_width_    = src.Width();
    cached_frame_height_   = src.Height();
    cached_pitch_          = level_widths_[0];
    cached_reference_base_ = seed_canonical_reference;
  }

  if (cached_width_ == src.Width() && cached_height_ == src.Height()) {
    EnqueueApplyAdjustedL(src, dst);
  } else {
    EnqueueApplyAdjustedLFromFrame(src, dst);
  }
}

}  // namespace alcedo::highlight_shadow_local_tone

#endif  // HAVE_OPENCL
