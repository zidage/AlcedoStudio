//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

namespace alcedo::OpenCL::RawProcessor {

inline constexpr const char* kManifestName            = "raw_processor";
inline constexpr const char* kCoreProgramName         = "raw_processor_core";
inline constexpr const char* kDebayerRcdProgramName   = "raw_processor_debayer_rcd";
inline constexpr const char* kXTransProgramName       = "raw_processor_xtrans";
inline constexpr const char* kHighlightProgramName    = "raw_processor_highlight";
inline constexpr const char* kCvtRefSpaceProgramName  = "raw_processor_cvt_ref_space";

inline constexpr const char* kToLinearRefKernelName   = "to_linear_ref_u16_to_f32";
inline constexpr const char* kLinearizeRgbKernelName            = "linearize_rgb";
inline constexpr const char* kCfaClamp01KernelName    = "cfa_clamp01_f32";

inline constexpr const char* kRcdInitAndVhKernelName  = "rcd_init_and_vh";
inline constexpr const char* kRcdGreenAtRbKernelName  = "rcd_green_at_rb";
inline constexpr const char* kRcdPqDirKernelName      = "rcd_pq_dir";
inline constexpr const char* kRcdRbAtRbKernelName     = "rcd_rb_at_rb";
inline constexpr const char* kRcdRbAtGKernelName      = "rcd_rb_at_g";
inline constexpr const char* kRcdMergeRgbaKernelName  = "rcd_merge_rgba";

inline constexpr const char* kXTransGreenKernelName   = "xtrans_green";
inline constexpr const char* kXTransRgbaKernelName    = "xtrans_rgba";

inline constexpr const char* kHlrBuildMaskKernelName           = "hlr_build_mask";
inline constexpr const char* kHlrDilateMaskKernelName          = "hlr_dilate_mask";
inline constexpr const char* kHlrChrominanceContribKernelName  = "hlr_chrominance_contrib";
inline constexpr const char* kHlrReconstructKernelName         = "hlr_reconstruct";
inline constexpr const char* kHlrReconstructFromStatsKernelName = "hlr_reconstruct_from_stats";
inline constexpr const char* kHlrBuildMaskPlanarKernelName              = "hlr_build_mask_planar";
inline constexpr const char* kHlrChrominanceContribPlanarKernelName     = "hlr_chrominance_contrib_planar";
inline constexpr const char* kHlrReconstructFromStatsPlanarPackKernelName =
    "hlr_reconstruct_from_stats_planar_pack";

inline constexpr const char* kApplyInverseCamMulKernelName           = "apply_inverse_cam_mul_rgba32f";
inline constexpr const char* kPackPlanesCropInverseOrientKernelName  = "pack_planes_crop_inverse_orient";
inline constexpr const char* kCopyRgbaCropInverseOrientKernelName    = "copy_rgba_crop_inverse_orient";
inline constexpr const char* kCopyRgbCropInverseOrientKernelName     = "copy_rgb_crop_inverse_orient";

}  // namespace alcedo::OpenCL::RawProcessor

#endif
