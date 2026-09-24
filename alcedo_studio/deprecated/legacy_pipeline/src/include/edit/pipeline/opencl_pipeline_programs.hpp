//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

namespace alcedo::OpenCL::Pipeline {

inline constexpr const char* kManifestName = "edit_pipeline";

inline constexpr const char* kFusedProgramName = "edit_pipeline_fused";

inline constexpr const char* kDetailProgramName = "edit_pipeline_detail";

inline constexpr const char* kFusedKernelName = "edit_pipeline_fused_rgba32f";

inline constexpr const char* kFusedStageKernelName = "edit_pipeline_fused_stage_rgba32f";

inline constexpr const char* kNeighborBlurHorizontalKernelName =
    "edit_pipeline_neighbor_blur_h_rgba32f";
inline constexpr const char* kNeighborApplyVerticalKernelName =
    "edit_pipeline_neighbor_apply_v_rgba32f";
inline constexpr const char* kHsExtractLogIntensityKernelName =
    "edit_pipeline_hs_extract_log_intensity_rgba32f";
inline constexpr const char* kHsExtractLogIntensityResampledKernelName =
    "edit_pipeline_hs_extract_log_intensity_resampled_rgba32f";
inline constexpr const char* kHsBuildRemappedSampleKernelName =
    "edit_pipeline_hs_build_remapped_sample";
inline constexpr const char* kHsPyrDownKernelName = "edit_pipeline_hs_pyr_down";
inline constexpr const char* kHsSelectInterpolatedLevelKernelName =
    "edit_pipeline_hs_select_interpolated_level";
inline constexpr const char* kHsCollapseLevelKernelName = "edit_pipeline_hs_collapse_level";
inline constexpr const char* kHsApplyAdjustedLKernelName =
    "edit_pipeline_hs_apply_adjusted_l_rgba32f";
inline constexpr const char* kHsApplyAdjustedLFromFrameKernelName =
    "edit_pipeline_hs_apply_adjusted_l_from_frame_rgba32f";
inline constexpr const char* kHsApplyAdjustedLFromReferenceKernelName =
    "edit_pipeline_hs_apply_adjusted_l_from_reference_rgba32f";

inline constexpr const char* kValidateFusedParamsKernelName =
    "edit_pipeline_validate_fused_params";

}  // namespace alcedo::OpenCL::Pipeline

#endif
