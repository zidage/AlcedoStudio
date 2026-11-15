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
inline constexpr const char* kDisplayToAp1ProgramName         = "opencl_dag_display_to_ap1";
/// SM8.3: Feather and Expand of AI Mask coverage (ai_mask_coverage.cl).
inline constexpr const char* kAiMaskCoverageProgramName       = "opencl_dag_ai_mask_coverage";
/// F6 of the Film Print DRT Plan: the film print display transform (film_print.cl).
inline constexpr const char* kFilmPrintProgramName            = "opencl_dag_film_print";
/// G4 of the film grain plan: the DRT/Post Film Grain (film_grain_post.cl).
inline constexpr const char* kFilmGrainProgramName            = "opencl_dag_film_grain";

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
/// Histogram of the curve input of a Color Grade (issue #888).
inline constexpr const char* kPrimaryGradeCurveInputHistogramKernelName =
    "primary_grade_curve_input_histogram";
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
inline constexpr const char* kMaskRasterKernelName                = "mask_raster_r8";
inline constexpr const char* kMaskAnalyticRangedKernelName        = "mask_analytic_ranged_r8";
inline constexpr const char* kMaskRasterRangedKernelName          = "mask_raster_ranged_r8";
inline constexpr const char* kMaskFillZeroKernelName              = "mask_fill_zero_r8";
inline constexpr const char* kMaskUnionMaxKernelName              = "mask_union_max_r8";
inline constexpr const char* kMaskValueDisplayKernelName          = "mask_value_display_rgba32f";
inline constexpr const char* kAiMaskMorphologyKernelName          = "ai_mask_morphology_r8";
inline constexpr const char* kAiMaskFeatherRowsKernelName         = "ai_mask_feather_rows_r32f";
inline constexpr const char* kAiMaskFeatherColumnsKernelName      = "ai_mask_feather_columns_r8";
inline constexpr const char* kDrtKernelName                       = "drt_display_rgba32f";
inline constexpr const char* kDisplayToAp1KernelName              = "display_to_ap1_acescc";
inline constexpr const char* kDisplayToAp0LinearKernelName        = "display_to_ap0_linear";
inline constexpr const char* kLinearizeRasterKernelName           = "linearize_raster_rgba";
inline constexpr const char* kDrtSceneKernelName                  = "drt_display_scene_rgba32f";
inline constexpr const char* kDiffusionFilterDecodeSceneKernelName =
    "diffusion_filter_decode_scene_rgba32f";
inline constexpr const char* kDiffusionFilterReduceBoostKernelName = "diffusion_filter_reduce_boost";
inline constexpr const char* kDiffusionFilterDownsampleKernelName  = "diffusion_filter_downsample";
inline constexpr const char* kDiffusionFilterUpsampleAccumulateKernelName =
    "diffusion_filter_upsample_accumulate";
inline constexpr const char* kDiffusionFilterMixSceneKernelName =
    "diffusion_filter_mix_scene_rgba32f";
inline constexpr const char* kFilmPrintExposureKernelName =
    "film_print_exposure";
inline constexpr const char* kFilmPrintFirstAmountsKernelName =
    "film_print_first_amounts";
inline constexpr const char* kFilmPrintHalationFirstAmountsKernelName =
    "film_print_halation_first_amounts";
inline constexpr const char* kFilmPrintReducedFirstAmountsKernelName =
    "film_print_reduced_first_amounts";
inline constexpr const char* kFilmPrintStepHalfSizeKernelName =
    "film_print_step_half_size";
inline constexpr const char* kFilmPrintStepToReducedKernelName =
    "film_print_step_to_reduced";
inline constexpr const char* kFilmPrintStepVerticalKernelName =
    "film_print_step_vertical";
inline constexpr const char* kFilmPrintBlurHorizontalKernelName =
    "film_print_blur_horizontal";
inline constexpr const char* kFilmPrintBlurVerticalKernelName =
    "film_print_blur_vertical";
inline constexpr const char* kFilmPrintBlurVerticalToImageKernelName =
    "film_print_blur_vertical_to_image";
inline constexpr const char* kFilmPrintSpreadVerticalKernelName =
    "film_print_spread_vertical";
inline constexpr const char* kFilmPrintBoxReduceKernelName =
    "film_print_box_reduce";
inline constexpr const char* kFilmPrintCanvasReduceKernelName =
    "film_print_canvas_reduce";
inline constexpr const char* kFilmPrintAmountsReduceKernelName =
    "film_print_amounts_reduce";
inline constexpr const char* kFilmPrintDisplayKernelName =
    "film_print_display";
/// The prescan of rule 'frame_auto_colour' of Negative Scan: the two passes of the area reduction.
inline constexpr const char* kFilmPrintScanPrescanRowsKernelName = "film_print_scan_prescan_rows";
inline constexpr const char* kFilmPrintScanPrescanColumnsKernelName =
    "film_print_scan_prescan_columns";
/// FP6 of print material Negative Scan. It is a kernel of @ref kDrtProgramName, because it applies
/// the display transform of that program.
inline constexpr const char* kFilmPrintScanDisplayKernelName = "film_print_scan_display";
inline constexpr const char* kFilmGrainPostKernelName        = "film_grain_post_rgba32f";
/// Phase G6 of the film grain plan: FP6 with the grain of the negative. The print and light table
/// form is a kernel of @ref kFilmPrintProgramName, the Negative Scan form of @ref kDrtProgramName.
inline constexpr const char* kFilmPrintDisplayGrainKernelName     = "film_print_display_grain";
inline constexpr const char* kFilmPrintScanDisplayGrainKernelName = "film_print_scan_display_grain";
/// The grain of the negative alone on an image of dye amounts, for the backend tests. A kernel of
/// @ref kFilmPrintProgramName.
inline constexpr const char* kFilmGrainNegativeAmountsKernelName  = "film_grain_negative_amounts";

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
