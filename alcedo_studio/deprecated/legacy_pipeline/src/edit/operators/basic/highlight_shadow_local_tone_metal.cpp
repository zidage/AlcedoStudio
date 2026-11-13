//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "edit/operators/basic/highlight_shadow_local_tone_metal.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "image/metal_image.hpp"

namespace alcedo::highlight_shadow_local_tone {
namespace {

constexpr const char* kHsExtractLogIntensityKernelName = "metal_hs_extract_log_intensity_rgba32f";
constexpr const char* kHsExtractLogIntensityResampledKernelName =
    "metal_hs_extract_log_intensity_resampled_rgba32f";
constexpr const char* kHsBuildRemappedSampleKernelName = "metal_hs_build_remapped_sample";
constexpr const char* kHsBuildRemappedSamplesPackedKernelName =
    "metal_hs_build_remapped_samples_packed";
constexpr const char* kHsPyrDownKernelName                 = "metal_hs_pyr_down";
constexpr const char* kHsPyrDownPackedKernelName           = "metal_hs_pyr_down_packed";
constexpr const char* kHsSelectInterpolatedLevelKernelName = "metal_hs_select_interpolated_level";
constexpr const char* kHsSelectInterpolatedLevelPackedKernelName =
    "metal_hs_select_interpolated_level_packed";
constexpr const char* kHsCollapseLevelKernelName = "metal_hs_collapse_level";
constexpr const char* kHsApplyAdjustedLKernelName =
    "metal_hs_apply_adjusted_l_with_reference_rgba32f";
constexpr const char* kHsApplyAdjustedLFromFrameKernelName =
    "metal_hs_apply_adjusted_l_from_frame_rgba32f";
constexpr const char* kHsApplyAdjustedLFromReferenceKernelName =
    "metal_hs_apply_adjusted_l_from_reference_rgba32f";

constexpr const char* kHsExtractLogIntensityDebugLabel = "Metal H/S extract log intensity";
constexpr const char* kHsExtractLogIntensityResampledDebugLabel =
    "Metal H/S extract log intensity resampled";
constexpr const char* kHsBuildRemappedSampleDebugLabel        = "Metal H/S remapped sample";
constexpr const char* kHsBuildRemappedSamplesPackedDebugLabel = "Metal H/S remapped samples packed";
constexpr const char* kHsPyrDownDebugLabel                    = "Metal H/S pyr down";
constexpr const char* kHsPyrDownPackedDebugLabel              = "Metal H/S pyr down packed";
constexpr const char* kHsSelectInterpolatedLevelDebugLabel    = "Metal H/S select level";
constexpr const char* kHsSelectInterpolatedLevelPackedDebugLabel = "Metal H/S select level packed";
constexpr const char* kHsCollapseLevelDebugLabel                 = "Metal H/S collapse level";
constexpr const char* kHsApplyAdjustedLDebugLabel                = "Metal H/S apply adjusted L";
constexpr const char* kHsApplyAdjustedLFromFrameDebugLabel =
    "Metal H/S apply adjusted L from frame";
constexpr const char* kHsApplyAdjustedLFromReferenceDebugLabel =
    "Metal H/S apply adjusted L from reference";

auto FusedPipelineMetallibPath() -> const char* {
#ifndef ALCEDO_METAL_FUSED_PIPELINE_METALLIB_PATH
  return nullptr;
#else
  return ALCEDO_METAL_FUSED_PIPELINE_METALLIB_PATH;
#endif
}

struct alignas(16) MetalHsExtractParams {
  int32_t src_width_  = 0;
  int32_t src_height_ = 0;
  int32_t dst_width_  = 0;
  int32_t dst_height_ = 0;
};

struct alignas(16) MetalHsRemapParams {
  int32_t width_      = 0;
  int32_t height_     = 0;
  float   gamma_      = 0.0f;
  float   target_     = 0.0f;
  float   beta_       = 1.0f;
  float   alpha_      = 1.0f;
  float   sigma_r_    = kBaseSigmaR;
  int32_t dst_offset_ = 0;
};

struct alignas(16) MetalHsRemapPackedParams {
  int32_t                        width_        = 0;
  int32_t                        height_       = 0;
  int32_t                        sample_count_ = 0;
  int32_t                        reserved_     = 0;
  float                          sigma_r_      = kBaseSigmaR;
  std::array<float, kMaxSamples> gammas_       = {};
  std::array<float, kMaxSamples> targets_      = {};
  std::array<float, kMaxSamples> betas_        = {};
  std::array<float, kMaxSamples> alphas_       = {};
};

struct alignas(16) MetalHsPyrDownParams {
  int32_t src_width_   = 0;
  int32_t src_height_  = 0;
  int32_t dst_width_   = 0;
  int32_t dst_height_  = 0;
  int32_t src_offset_  = 0;
  int32_t dst_offset_  = 0;
  int32_t reserved_[2] = {};
};

struct alignas(16) MetalHsPyrDownPackedParams {
  int32_t src_width_    = 0;
  int32_t src_height_   = 0;
  int32_t dst_width_    = 0;
  int32_t dst_height_   = 0;
  int32_t sample_count_ = 0;
  int32_t reserved_[3]  = {};
};

struct alignas(16) MetalHsSelectPackedParams {
  int32_t                        width_         = 0;
  int32_t                        height_        = 0;
  int32_t                        coarse_width_  = 0;
  int32_t                        coarse_height_ = 0;
  int32_t                        sample_count_  = 0;
  int32_t                        top_level_     = 0;
  int32_t                        reserved_[2]   = {};
  std::array<float, kMaxSamples> gammas_        = {};
};

struct alignas(16) MetalHsSelectParams {
  int32_t width_         = 0;
  int32_t height_        = 0;
  int32_t coarse_width_  = 0;
  int32_t coarse_height_ = 0;
  float   gamma_lo_      = 0.0f;
  float   gamma_hi_      = 0.0f;
  int32_t first_pair_    = 0;
  int32_t last_pair_     = 0;
  int32_t top_level_     = 0;
  int32_t reserved_[3]   = {};
};

struct alignas(16) MetalHsPlaneApplyParams {
  int32_t width_           = 0;
  int32_t height_          = 0;
  int32_t adjusted_width_  = 0;
  int32_t adjusted_height_ = 0;
};

auto HsLevelElems(int32_t width, int32_t height) -> size_t {
  return static_cast<size_t>(width) * static_cast<size_t>(height);
}

}  // namespace

MetalStage::MetalStage()
    : extract_pipeline_(FusedPipelineMetallibPath(), kHsExtractLogIntensityKernelName,
                        kHsExtractLogIntensityDebugLabel),
      extract_resampled_pipeline_(FusedPipelineMetallibPath(),
                                  kHsExtractLogIntensityResampledKernelName,
                                  kHsExtractLogIntensityResampledDebugLabel),
      build_remapped_sample_pipeline_(FusedPipelineMetallibPath(), kHsBuildRemappedSampleKernelName,
                                      kHsBuildRemappedSampleDebugLabel),
      build_remapped_samples_packed_pipeline_(FusedPipelineMetallibPath(),
                                              kHsBuildRemappedSamplesPackedKernelName,
                                              kHsBuildRemappedSamplesPackedDebugLabel),
      pyr_down_pipeline_(FusedPipelineMetallibPath(), kHsPyrDownKernelName, kHsPyrDownDebugLabel),
      pyr_down_packed_pipeline_(FusedPipelineMetallibPath(), kHsPyrDownPackedKernelName,
                                kHsPyrDownPackedDebugLabel),
      select_level_pipeline_(FusedPipelineMetallibPath(), kHsSelectInterpolatedLevelKernelName,
                             kHsSelectInterpolatedLevelDebugLabel),
      select_level_packed_pipeline_(FusedPipelineMetallibPath(),
                                    kHsSelectInterpolatedLevelPackedKernelName,
                                    kHsSelectInterpolatedLevelPackedDebugLabel),
      collapse_level_pipeline_(FusedPipelineMetallibPath(), kHsCollapseLevelKernelName,
                               kHsCollapseLevelDebugLabel),
      apply_adjusted_l_pipeline_(FusedPipelineMetallibPath(), kHsApplyAdjustedLKernelName,
                                 kHsApplyAdjustedLDebugLabel),
      apply_adjusted_l_from_frame_pipeline_(FusedPipelineMetallibPath(),
                                            kHsApplyAdjustedLFromFrameKernelName,
                                            kHsApplyAdjustedLFromFrameDebugLabel),
      apply_adjusted_l_from_reference_pipeline_(FusedPipelineMetallibPath(),
                                                kHsApplyAdjustedLFromReferenceKernelName,
                                                kHsApplyAdjustedLFromReferenceDebugLabel) {}

auto MetalStage::ShouldRun(const FusedOperatorParams& params) const -> bool {
  if (!params.hs_local_tone_enabled_) {
    return false;
  }
  const float shadow_amount =
      params.shadows_enabled_
          ? std::clamp(params.shadows_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  const float highlight_amount =
      params.highlights_enabled_
          ? std::clamp(-params.highlights_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  return highlight_shadow_local_tone::ShouldRun(shadow_amount, highlight_amount);
}

void MetalStage::InvalidateBaseCache() {
  cached_width_          = 0;
  cached_height_         = 0;
  cached_frame_width_    = 0;
  cached_frame_height_   = 0;
  cached_pitch_          = 0;
  cached_source_key_     = 0;
  cached_key_            = 0;
  cached_reference_base_ = false;
}

void MetalStage::ReleasePyramidBuffers() {
  for (auto& buffer : source_levels_) {
    buffer = nullptr;
  }
  for (auto& buffer : remap_a_levels_) {
    buffer = nullptr;
  }
  for (auto& buffer : sample_levels_) {
    buffer = nullptr;
  }
  for (auto& buffer : output_levels_) {
    buffer = nullptr;
  }
  level_widths_.fill(0);
  level_heights_.fill(0);
  level_count_  = 0;
  sample_count_ = 0;
  InvalidateBaseCache();
}

void MetalStage::ReleaseResources() {
  extract_pipeline_.Release();
  extract_resampled_pipeline_.Release();
  build_remapped_sample_pipeline_.Release();
  build_remapped_samples_packed_pipeline_.Release();
  pyr_down_pipeline_.Release();
  pyr_down_packed_pipeline_.Release();
  select_level_pipeline_.Release();
  select_level_packed_pipeline_.Release();
  collapse_level_pipeline_.Release();
  apply_adjusted_l_pipeline_.Release();
  apply_adjusted_l_from_frame_pipeline_.Release();
  apply_adjusted_l_from_reference_pipeline_.Release();
  ReleasePyramidBuffers();
}

void MetalStage::EnsurePyramidBuffers(int32_t width, int32_t height, float radius) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("Metal H/S local tone: invalid pyramid dimensions.");
  }

  const int                       new_level_count = ComputeLevelCount(width, height, radius);
  std::array<int32_t, kMaxLevels> new_widths      = {};
  std::array<int32_t, kMaxLevels> new_heights     = {};
  new_widths[0]                                   = width;
  new_heights[0]                                  = height;
  for (int level = 1; level < new_level_count; ++level) {
    new_widths[level]  = std::max<int32_t>(1, (new_widths[level - 1] + 1) / 2);
    new_heights[level] = std::max<int32_t>(1, (new_heights[level - 1] + 1) / 2);
  }

  bool layout_matches = level_count_ == new_level_count;
  for (int level = 0; layout_matches && level < new_level_count; ++level) {
    layout_matches =
        level_widths_[level] == new_widths[level] && level_heights_[level] == new_heights[level] &&
        source_levels_[level].get() != nullptr && remap_a_levels_[level].get() != nullptr &&
        output_levels_[level].get() != nullptr;
  }
  if (layout_matches) {
    return;
  }

  ReleasePyramidBuffers();
  level_count_   = new_level_count;
  level_widths_  = new_widths;
  level_heights_ = new_heights;
  for (int level = 0; level < level_count_; ++level) {
    const size_t elems     = HsLevelElems(level_widths_[level], level_heights_[level]);
    const size_t bytes     = elems * sizeof(float);
    source_levels_[level]  = MakeDeviceBuffer(bytes);
    remap_a_levels_[level] = MakeDeviceBuffer(bytes);
    output_levels_[level]  = MakeDeviceBuffer(bytes);
  }
}

void MetalStage::EnsureSamplePyramidBuffers(int32_t sample_count) {
  if (sample_count < 2 || sample_count > kMaxSamples) {
    throw std::runtime_error("Metal H/S local tone: invalid sample count.");
  }

  bool layout_matches = sample_count_ == sample_count && level_count_ > 0;
  for (int level = 0; layout_matches && level < level_count_; ++level) {
    layout_matches = sample_levels_[level].get() != nullptr;
  }
  if (layout_matches) {
    return;
  }

  for (auto& buffer : sample_levels_) {
    buffer = nullptr;
  }
  sample_count_ = sample_count;
  for (int level = 0; level < level_count_; ++level) {
    const size_t elems = HsLevelElems(level_widths_[level], level_heights_[level]);
    sample_levels_[level] =
        MakeDeviceBuffer(elems * static_cast<size_t>(sample_count_) * sizeof(float));
  }
}

void MetalStage::EncodeExtractLogIntensity(MTL::CommandBuffer*      command_buffer,
                                           const metal::MetalImage& src) {
  auto*                      pipeline = EnsurePipeline(extract_pipeline_);
  const MetalHsExtractParams params{static_cast<int32_t>(src.Width()),
                                    static_cast<int32_t>(src.Height()), level_widths_[0],
                                    level_heights_[0]};
  auto                       encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setTexture(src.Texture(), 0);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBytes(&params, sizeof(params), 1);
  DispatchThreads(encoder.get(), pipeline, static_cast<uint32_t>(level_widths_[0]),
                  static_cast<uint32_t>(level_heights_[0]));
  encoder->endEncoding();
}

void MetalStage::EncodeExtractLogIntensityResampled(MTL::CommandBuffer*      command_buffer,
                                                    const metal::MetalImage& src) {
  auto*                      pipeline = EnsurePipeline(extract_resampled_pipeline_);
  const MetalHsExtractParams params{static_cast<int32_t>(src.Width()),
                                    static_cast<int32_t>(src.Height()), level_widths_[0],
                                    level_heights_[0]};
  auto                       encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setTexture(src.Texture(), 0);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBytes(&params, sizeof(params), 1);
  DispatchThreads(encoder.get(), pipeline, static_cast<uint32_t>(level_widths_[0]),
                  static_cast<uint32_t>(level_heights_[0]));
  encoder->endEncoding();
}

void MetalStage::EncodePyrDown(MTL::CommandBuffer* command_buffer, MTL::Buffer* src,
                               int32_t src_width, int32_t src_height, int32_t src_offset,
                               MTL::Buffer* dst, int32_t dst_width, int32_t dst_height,
                               int32_t dst_offset) {
  auto*                      pipeline = EnsurePipeline(pyr_down_pipeline_);
  const MetalHsPyrDownParams params{src_width,  src_height, dst_width, dst_height,
                                    src_offset, dst_offset, {0, 0}};
  auto                       encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setBuffer(src, 0, 0);
  encoder->setBuffer(dst, 0, 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder.get(), pipeline, static_cast<uint32_t>(dst_width),
                  static_cast<uint32_t>(dst_height));
  encoder->endEncoding();
}

void MetalStage::BuildSourcePyramid(MTL::CommandBuffer*      command_buffer,
                                    const metal::MetalImage& src) {
  if (level_widths_[0] == static_cast<int32_t>(src.Width()) &&
      level_heights_[0] == static_cast<int32_t>(src.Height())) {
    EncodeExtractLogIntensity(command_buffer, src);
  } else {
    EncodeExtractLogIntensityResampled(command_buffer, src);
  }
  for (int level = 1; level < level_count_; ++level) {
    EncodePyrDown(command_buffer, source_levels_[level - 1].get(), level_widths_[level - 1],
                  level_heights_[level - 1], 0, source_levels_[level].get(), level_widths_[level],
                  level_heights_[level], 0);
  }
}

void MetalStage::BuildRemapPyramid(
    MTL::CommandBuffer* command_buffer, const LlfSample& sample,
    std::array<NS::SharedPtr<MTL::Buffer>, kMaxLevels>& remap_levels) {
  auto*                    pipeline = EnsurePipeline(build_remapped_sample_pipeline_);
  const MetalHsRemapParams params{
      level_widths_[0], level_heights_[0], sample.gamma_, sample.target_,
      sample.beta_,     sample.alpha_,     kBaseSigmaR,   0};
  auto encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBuffer(remap_levels[0].get(), 0, 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder.get(), pipeline, static_cast<uint32_t>(level_widths_[0]),
                  static_cast<uint32_t>(level_heights_[0]));
  encoder->endEncoding();

  for (int level = 1; level < level_count_; ++level) {
    EncodePyrDown(command_buffer, remap_levels[level - 1].get(), level_widths_[level - 1],
                  level_heights_[level - 1], 0, remap_levels[level].get(), level_widths_[level],
                  level_heights_[level], 0);
  }
}

void MetalStage::BuildPackedSamplePyramids(MTL::CommandBuffer*           command_buffer,
                                           const std::vector<LlfSample>& samples) {
  const int32_t            sample_count = static_cast<int32_t>(samples.size());
  auto*                    pipeline     = EnsurePipeline(build_remapped_samples_packed_pipeline_);
  MetalHsRemapPackedParams params;
  params.width_        = level_widths_[0];
  params.height_       = level_heights_[0];
  params.sample_count_ = sample_count;
  params.sigma_r_      = kBaseSigmaR;
  for (size_t i = 0; i < samples.size(); ++i) {
    params.gammas_[i]  = samples[i].gamma_;
    params.targets_[i] = samples[i].target_;
    params.betas_[i]   = samples[i].beta_;
    params.alphas_[i]  = samples[i].alpha_;
  }

  auto encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBuffer(sample_levels_[0].get(), 0, 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchSampleThreads(encoder.get(), pipeline, static_cast<uint32_t>(level_widths_[0]),
                        static_cast<uint32_t>(level_heights_[0]),
                        static_cast<uint32_t>(sample_count));
  encoder->endEncoding();

  auto* pyr_down_pipeline = EnsurePipeline(pyr_down_packed_pipeline_);
  for (int level = 1; level < level_count_; ++level) {
    const MetalHsPyrDownPackedParams down_params{level_widths_[level - 1],
                                                 level_heights_[level - 1],
                                                 level_widths_[level],
                                                 level_heights_[level],
                                                 sample_count,
                                                 {0, 0, 0}};
    auto down_encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
    down_encoder->setComputePipelineState(pyr_down_pipeline);
    down_encoder->setBuffer(sample_levels_[level - 1].get(), 0, 0);
    down_encoder->setBuffer(sample_levels_[level].get(), 0, 1);
    down_encoder->setBytes(&down_params, sizeof(down_params), 2);
    DispatchSampleThreads(
        down_encoder.get(), pyr_down_pipeline, static_cast<uint32_t>(level_widths_[level]),
        static_cast<uint32_t>(level_heights_[level]), static_cast<uint32_t>(sample_count));
    down_encoder->endEncoding();
  }
}

void MetalStage::BuildOutputPyramid(MTL::CommandBuffer*           command_buffer,
                                    const std::vector<LlfSample>& samples,
                                    MetalExecutionStats*          stats) {
  EnsureSamplePyramidBuffers(static_cast<int32_t>(samples.size()));

  const auto remap_start = std::chrono::steady_clock::now();
  BuildPackedSamplePyramids(command_buffer, samples);
  const auto remap_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->hs_remap_encode_ms +=
        std::chrono::duration<double, std::milli>(remap_end - remap_start).count();
  }

  const auto select_start = std::chrono::steady_clock::now();
  auto* select_pipeline = EnsurePipeline(select_level_packed_pipeline_);
  for (int level = 0; level < level_count_; ++level) {
    const bool                top_level = level == (level_count_ - 1);
    MetalHsSelectPackedParams params;
    params.width_         = level_widths_[level];
    params.height_        = level_heights_[level];
    params.coarse_width_  = top_level ? 1 : level_widths_[level + 1];
    params.coarse_height_ = top_level ? 1 : level_heights_[level + 1];
    params.sample_count_  = static_cast<int32_t>(samples.size());
    params.top_level_     = top_level ? 1 : 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      params.gammas_[i] = samples[i].gamma_;
    }

    auto encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
    encoder->setComputePipelineState(select_pipeline);
    encoder->setBuffer(source_levels_[level].get(), 0, 0);
    encoder->setBuffer(sample_levels_[level].get(), 0, 1);
    encoder->setBuffer(top_level ? sample_levels_[level].get() : sample_levels_[level + 1].get(),
                       0, 2);
    encoder->setBuffer(output_levels_[level].get(), 0, 3);
    encoder->setBytes(&params, sizeof(params), 4);
    DispatchThreads(encoder.get(), select_pipeline, static_cast<uint32_t>(level_widths_[level]),
                    static_cast<uint32_t>(level_heights_[level]));
    encoder->endEncoding();
  }
  const auto select_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->hs_select_encode_ms +=
        std::chrono::duration<double, std::milli>(select_end - select_start).count();
  }

  const auto collapse_start = std::chrono::steady_clock::now();
  for (int level = level_count_ - 2; level >= 0; --level) {
    auto*                      pipeline = EnsurePipeline(collapse_level_pipeline_);
    const MetalHsPyrDownParams params{level_widths_[level],
                                      level_heights_[level],
                                      level_widths_[level + 1],
                                      level_heights_[level + 1],
                                      0,
                                      0,
                                      {0, 0}};
    auto                       encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(output_levels_[level].get(), 0, 0);
    encoder->setBuffer(output_levels_[level + 1].get(), 0, 1);
    encoder->setBuffer(remap_a_levels_[level].get(), 0, 2);
    encoder->setBytes(&params, sizeof(params), 3);
    DispatchThreads(encoder.get(), pipeline, static_cast<uint32_t>(level_widths_[level]),
                    static_cast<uint32_t>(level_heights_[level]));
    encoder->endEncoding();
    std::swap(output_levels_[level], remap_a_levels_[level]);
  }
  const auto collapse_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->hs_collapse_encode_ms +=
        std::chrono::duration<double, std::milli>(collapse_end - collapse_start).count();
  }
}

void MetalStage::EncodeApplyAdjustedL(MTL::CommandBuffer*      command_buffer,
                                      const metal::MetalImage& src, metal::MetalImage& dst) {
  auto* pipeline = EnsurePipeline(apply_adjusted_l_pipeline_);
  dst.Create(src.Width(), src.Height(), src.Format(), true, true, false);
  const MetalHsPlaneApplyParams params{static_cast<int32_t>(src.Width()),
                                       static_cast<int32_t>(src.Height()),
                                       cached_width_,
                                       cached_height_};
  auto                          encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setTexture(src.Texture(), 0);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBuffer(output_levels_[0].get(), 0, 1);
  encoder->setTexture(dst.Texture(), 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder.get(), pipeline, src.Width(), src.Height());
  encoder->endEncoding();
}

void MetalStage::EncodeApplyAdjustedLFromFrame(MTL::CommandBuffer*      command_buffer,
                                               const metal::MetalImage& src,
                                               metal::MetalImage&       dst) {
  auto* pipeline = EnsurePipeline(apply_adjusted_l_from_frame_pipeline_);
  dst.Create(src.Width(), src.Height(), src.Format(), true, true, false);
  const MetalHsPlaneApplyParams params{static_cast<int32_t>(src.Width()),
                                       static_cast<int32_t>(src.Height()),
                                       cached_width_,
                                       cached_height_};
  auto                          encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setTexture(src.Texture(), 0);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBuffer(output_levels_[0].get(), 0, 1);
  encoder->setTexture(dst.Texture(), 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder.get(), pipeline, src.Width(), src.Height());
  encoder->endEncoding();
}

void MetalStage::EncodeApplyAdjustedLFromReference(MTL::CommandBuffer*      command_buffer,
                                                   const metal::MetalImage& src,
                                                   metal::MetalImage&       dst,
                                                   MTL::Buffer*             fused_params_buffer) {
  auto* pipeline = EnsurePipeline(apply_adjusted_l_from_reference_pipeline_);
  dst.Create(src.Width(), src.Height(), src.Format(), true, true, false);
  const MetalHsPlaneApplyParams params{static_cast<int32_t>(src.Width()),
                                       static_cast<int32_t>(src.Height()),
                                       cached_width_,
                                       cached_height_};
  auto                          encoder = NS::RetainPtr(command_buffer->computeCommandEncoder());
  encoder->setComputePipelineState(pipeline);
  encoder->setTexture(src.Texture(), 0);
  encoder->setBuffer(source_levels_[0].get(), 0, 0);
  encoder->setBuffer(output_levels_[0].get(), 0, 1);
  encoder->setTexture(dst.Texture(), 1);
  encoder->setBuffer(fused_params_buffer, 0, 2);
  encoder->setBytes(&params, sizeof(params), 3);
  DispatchThreads(encoder.get(), pipeline, src.Width(), src.Height());
  encoder->endEncoding();
}

void MetalStage::Execute(const FusedOperatorParams& params, MTL::Buffer* fused_params_buffer,
                         MTL::CommandBuffer* command_buffer, const metal::MetalImage& src,
                         metal::MetalImage& dst, MetalExecutionStats* stats) {
  const float shadow_amount =
      params.shadows_enabled_
          ? std::clamp(params.shadows_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  const float highlight_amount =
      params.highlights_enabled_
          ? std::clamp(-params.highlights_offset_, -kBackendAmountLimit, kBackendAmountLimit)
          : 0.0f;
  const std::uint64_t adjusted_cache_key =
      BuildAdjustedResultCacheKey(params, shadow_amount, highlight_amount);
  const bool roi_frame_with_source_reference = params.render_roi_enabled_ &&
                                               params.render_roi_reference_width_ > 0 &&
                                               params.render_roi_reference_height_ > 0;
  const bool preserve_source_detail  = params.render_hs_preserve_source_detail_;
  const int  reference_max_long_edge =
      std::max(1, std::min(params.render_hs_reference_max_long_edge_, kReferenceMaskMaxLongEdge));
  const MaskDimensions current_reference_dims = ComputeMaskDimensions(
      static_cast<int32_t>(src.Width()), static_cast<int32_t>(src.Height()),
      roi_frame_with_source_reference
          ? std::max(static_cast<int32_t>(src.Width()), static_cast<int32_t>(src.Height()))
          : reference_max_long_edge);
  const std::uint64_t reference_source_cache_key = params.hs_mask_base_cache_key_;
  std::uint64_t       reference_cache_key        = adjusted_cache_key;
  HashCombine(reference_cache_key, static_cast<std::uint64_t>(preserve_source_detail));
  const bool reference_source_cache_valid =
      cached_reference_base_ && source_levels_[0].get() != nullptr &&
      cached_source_key_ == reference_source_cache_key && cached_width_ > 0 && cached_height_ > 0 &&
      cached_frame_width_ > 0 && cached_frame_height_ > 0 && cached_pitch_ > 0;
  const bool reference_result_cache_valid = reference_source_cache_valid &&
                                            output_levels_[0].get() != nullptr &&
                                            cached_key_ == reference_cache_key;
  const int current_reference_long_edge =
      std::max(current_reference_dims.width_, current_reference_dims.height_);
  const int  cached_reference_long_edge = std::max(cached_width_, cached_height_);
  const bool current_can_improve_reference =
      params.render_hs_can_seed_reference_ &&
      current_reference_long_edge > cached_reference_long_edge;
  const auto ensure_reference_output = [&]() {
    if (!reference_result_cache_valid) {
      const auto samples = BuildSamples(shadow_amount, highlight_amount);
      BuildOutputPyramid(command_buffer, samples, stats);
      cached_key_ = reference_cache_key;
    }
  };
  if (CanReuseReferenceForRoi(roi_frame_with_source_reference, reference_source_cache_valid,
                              params.render_roi_reference_width_,
                              params.render_roi_reference_height_)) {
    const auto apply_start = std::chrono::steady_clock::now();
    ensure_reference_output();
    EncodeApplyAdjustedLFromReference(command_buffer, src, dst, fused_params_buffer);
    const auto apply_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->hs_apply_encode_ms +=
          std::chrono::duration<double, std::milli>(apply_end - apply_start).count();
    }
    return;
  }
  if (!roi_frame_with_source_reference && reference_source_cache_valid &&
      !current_can_improve_reference &&
      (cached_frame_width_ != static_cast<int32_t>(src.Width()) ||
       cached_frame_height_ != static_cast<int32_t>(src.Height()))) {
    const auto apply_start = std::chrono::steady_clock::now();
    ensure_reference_output();
    EncodeApplyAdjustedLFromFrame(command_buffer, src, dst);
    const auto apply_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->hs_apply_encode_ms +=
          std::chrono::duration<double, std::milli>(apply_end - apply_start).count();
    }
    return;
  }

  const bool build_roi_local_reference = roi_frame_with_source_reference;
  const bool seed_canonical_reference =
      params.render_hs_can_seed_reference_ && !build_roi_local_reference;
  std::uint64_t        render_cache_key = build_roi_local_reference
                                              ? BuildRoiAdjustedResultCacheKey(params, reference_cache_key)
                                              : reference_cache_key;
  const MaskDimensions mask_dims        = current_reference_dims;
  EnsurePyramidBuffers(mask_dims.width_, mask_dims.height_, params.hs_base_radius_);
  const bool cache_valid = output_levels_[0].get() != nullptr && cached_key_ == render_cache_key &&
                           cached_frame_width_ == static_cast<int32_t>(src.Width()) &&
                           cached_frame_height_ == static_cast<int32_t>(src.Height()) &&
                           cached_width_ == mask_dims.width_ &&
                           cached_height_ == mask_dims.height_ &&
                           cached_pitch_ == level_widths_[0];
  if (!cache_valid) {
    const auto samples      = BuildSamples(shadow_amount, highlight_amount);
    const auto source_start = std::chrono::steady_clock::now();
    BuildSourcePyramid(command_buffer, src);
    const auto source_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->hs_source_encode_ms +=
          std::chrono::duration<double, std::milli>(source_end - source_start).count();
    }
    BuildOutputPyramid(command_buffer, samples, stats);
    cached_key_            = render_cache_key;
    cached_source_key_     = seed_canonical_reference ? reference_source_cache_key : 0;
    cached_width_          = mask_dims.width_;
    cached_height_         = mask_dims.height_;
    cached_frame_width_    = static_cast<int32_t>(src.Width());
    cached_frame_height_   = static_cast<int32_t>(src.Height());
    cached_pitch_          = level_widths_[0];
    cached_reference_base_ = seed_canonical_reference;
  }

  const auto apply_start = std::chrono::steady_clock::now();
  if (cached_width_ == static_cast<int32_t>(src.Width()) &&
      cached_height_ == static_cast<int32_t>(src.Height())) {
    EncodeApplyAdjustedL(command_buffer, src, dst);
  } else {
    EncodeApplyAdjustedLFromFrame(command_buffer, src, dst);
  }
  const auto apply_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->hs_apply_encode_ms +=
        std::chrono::duration<double, std::milli>(apply_end - apply_start).count();
  }
}

}  // namespace alcedo::highlight_shadow_local_tone

#endif  // HAVE_METAL
