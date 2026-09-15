//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

namespace alcedo::OpenCL::GpuDag {

inline constexpr const char* kManifestName                    = "gpu_dag";

inline constexpr const char* kGeometryCameraProgramName       = "opencl_dag_geometry_camera";
inline constexpr const char* kPrimaryGradeProgramName         = "opencl_dag_primary_grade";
inline constexpr const char* kLocalToneProgramName            = "opencl_dag_local_tone";
inline constexpr const char* kMaskProgramName                 = "opencl_dag_mask";
inline constexpr const char* kDrtProgramName                  = "opencl_dag_drt";

inline constexpr const char* kGeometryResampleKernelName      = "geometry_resample_rgba32f";
inline constexpr const char* kCameraColorKernelName           = "camera_color_acescc";
inline constexpr const char* kWarpRectilinearKernelName       = "warp_rectilinear_rgba32f";
inline constexpr const char* kPrimaryGradePointwiseKernelName = "primary_grade_pointwise_rgba32f";
inline constexpr const char* kPrimaryGradePointwiseSceneKernelName =
    "primary_grade_pointwise_scene_rgba32f";
inline constexpr const char* kPrimaryGradeNeighborBlurKernelName =
    "primary_grade_neighbor_blur_h_rgba32f";
inline constexpr const char* kPrimaryGradeNeighborApplyKernelName =
    "primary_grade_neighbor_apply_v_rgba32f";
inline constexpr const char* kPrimaryGradeNeighborBlurSceneKernelName =
    "primary_grade_neighbor_blur_h_scene_rgba32f";
inline constexpr const char* kPrimaryGradeNeighborApplySceneKernelName =
    "primary_grade_neighbor_apply_v_scene_rgba32f";
inline constexpr const char* kPrimaryGradeMixKernelName       = "primary_grade_mix_rgba32f";
inline constexpr const char* kPrimaryGradeMixMaskedKernelName = "primary_grade_mix_masked_rgba32f";
inline constexpr const char* kLocalToneExtractKernelName      = "local_tone_extract";
inline constexpr const char* kLocalToneExtractSceneKernelName = "local_tone_extract_scene";
inline constexpr const char* kLocalToneExtractReferenceKernelName = "local_tone_extract_reference";
inline constexpr const char* kLocalToneExtractReferenceSceneKernelName =
    "local_tone_extract_reference_scene";
inline constexpr const char* kLocalTonePyramidDownKernelName      = "local_tone_pyr_down";
inline constexpr const char* kLocalToneRemapKernelName            = "local_tone_remap";
inline constexpr const char* kLocalToneSelectKernelName           = "local_tone_select";
inline constexpr const char* kLocalToneCollapseKernelName         = "local_tone_collapse";
inline constexpr const char* kLocalToneApplyKernelName            = "local_tone_apply";
inline constexpr const char* kLocalToneApplySceneKernelName       = "local_tone_apply_scene";
inline constexpr const char* kMaskAnalyticKernelName              = "mask_analytic_r8";
inline constexpr const char* kMaskFillZeroKernelName              = "mask_fill_zero_r8";
inline constexpr const char* kMaskUnionMaxKernelName              = "mask_union_max_r8";
inline constexpr const char* kDrtKernelName                       = "drt_display_rgba32f";
inline constexpr const char* kDrtSceneKernelName                  = "drt_display_scene_rgba32f";

}  // namespace alcedo::OpenCL::GpuDag

namespace alcedo {

/**
 * @brief Register the GPU DAG OpenCL program manifest with the backend registry.
 *
 * Describes compilation units only. Does not create a renderer, workspace, buffer,
 * image, or kernel. Invoked from RegisterBuiltinOpenClProgramManifests.
 */
void RegisterOpenClGpuDagPrograms();

}  // namespace alcedo

#endif  // HAVE_OPENCL
