//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

namespace alcedo::OpenCL::DemosaicNet {

// Central registry manifest name. Activated only through RegisterOpenClBackendPrograms.
inline constexpr const char* kManifestName              = "raw_demosaicnet";

// Stable program names (required_at_startup = false for all).
inline constexpr const char* kConvBayerProgramName      = "raw_demosaicnet_conv_bayer";
inline constexpr const char* kConvXTransProgramName     = "raw_demosaicnet_conv_xtrans";
inline constexpr const char* kStructuralProgramName     = "raw_demosaicnet_structural";

// Convolution kernels (shared source; Bayer/X-Trans differ by build options).
inline constexpr const char* kConv3x3KernelName         = "demosaicnet_conv3x3_nhwc4";
inline constexpr const char* kConv1x1KernelName         = "demosaicnet_conv1x1_nhwc4";
inline constexpr const char* kConv3x3C6KernelName       = "demosaicnet_conv3x3_c6_nhwc4";
inline constexpr const char* kConv1x1Output3KernelName  = "demosaicnet_conv1x1_out3_nhwc4";

// Structural / post / boundary kernels.
inline constexpr const char* kPackGammaKernelName       = "demosaicnet_pack_gamma_nhwc4";
inline constexpr const char* kPackReflectNchwKernelName = "demosaicnet_pack_reflect_nchw";
inline constexpr const char* kPackReflectBayerNhwc4KernelName =
    "demosaicnet_pack_reflect_bayer_nhwc4";
inline constexpr const char* kPackReflectXTransNhwc4KernelName =
    "demosaicnet_pack_reflect_xtrans_nhwc4";
inline constexpr const char* kUnpackReflectConcatKernelName =
    "demosaicnet_unpack_reflect_concat_nhwc4";
inline constexpr const char* kPackBayerNchwKernelName     = "demosaicnet_pack_bayer_nchw_to_nhwc4";
inline constexpr const char* kPackXTransNchwKernelName    = "demosaicnet_pack_xtrans_nchw_to_nhwc4";
inline constexpr const char* kResidualAddCropKernelName   = "demosaicnet_residual_add_crop";
inline constexpr const char* kUnpackCropConcatKernelName  = "demosaicnet_unpack_crop_concat_nhwc4";
inline constexpr const char* kFormPostInputC6KernelName   = "demosaicnet_form_post_input_c6";
inline constexpr const char* kOutputRgbHwcKernelName      = "demosaicnet_output_rgb_hwc";
inline constexpr const char* kOutputGammaHwcKernelName    = "demosaicnet_output_gamma_hwc";
inline constexpr const char* kAssembleRgbTileKernelName   = "demosaicnet_assemble_rgb_tile";
// Product RAW boundary helpers (Phase 6 routing).
inline constexpr const char* kClamp01KernelName           = "demosaicnet_clamp01";
inline constexpr const char* kPackCfaMonoToHwc3KernelName = "demosaicnet_pack_cfa_mono_to_hwc3";
inline constexpr const char* kRgb3ToRgba4KernelName       = "demosaicnet_rgb3_to_rgba4";

// The project-owned direct kernel uses a fixed 16x8 local tile. The output
// channel-block bound is compile-time for register/local-memory allocation;
// runtime dimensions still cover the first/post-layer shape families.
inline constexpr const char* kBayerConvBuildOptions =
    "-cl-std=CL1.2 "
    "-DIN_CHANNEL_BLOCKS=6 -DOUT_CHANNEL_BLOCKS=6 "
    "-DIN_LOGICAL_CHANNELS=24 -DOUT_LOGICAL_CHANNELS=24";

inline constexpr const char* kXTransConvBuildOptions =
    "-cl-std=CL1.2 "
    "-DIN_CHANNEL_BLOCKS=8 -DOUT_CHANNEL_BLOCKS=8 "
    "-DIN_LOGICAL_CHANNELS=32 -DOUT_LOGICAL_CHANNELS=32";

inline constexpr const char* kStructuralBuildOptions = "-cl-std=CL1.2";

}  // namespace alcedo::OpenCL::DemosaicNet

namespace alcedo {

// Registers the DemosaicNet program manifest with OpenClBackendProgramRegistry.
// Invoked only from the central RegisterOpenClBackendPrograms aggregation point.
void RegisterOpenClDemosaicNetPrograms();

}  // namespace alcedo

#endif  // HAVE_OPENCL
