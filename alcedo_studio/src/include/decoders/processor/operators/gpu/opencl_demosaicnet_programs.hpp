//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

namespace alcedo::OpenCL::DemosaicNet {

// Central registry manifest name. Activated only through RegisterOpenClBackendPrograms.
inline constexpr const char* kManifestName = "raw_demosaicnet";

// Stable program names (required_at_startup = false for all).
inline constexpr const char* kConvBayerProgramName   = "raw_demosaicnet_conv_bayer";
inline constexpr const char* kConvXTransProgramName  = "raw_demosaicnet_conv_xtrans";
inline constexpr const char* kStructuralProgramName  = "raw_demosaicnet_structural";

// Convolution kernels (shared source; Bayer/X-Trans differ by build options).
inline constexpr const char* kConv3x3KernelName = "demosaicnet_conv3x3_nhwc4";
inline constexpr const char* kConv1x1KernelName = "demosaicnet_conv1x1_nhwc4";

// Structural / post / boundary kernels.
inline constexpr const char* kPackGammaKernelName            = "demosaicnet_pack_gamma_nhwc4";
inline constexpr const char* kPackReflectNchwKernelName      = "demosaicnet_pack_reflect_nchw";
inline constexpr const char* kPackBayerNchwKernelName        = "demosaicnet_pack_bayer_nchw_to_nhwc4";
inline constexpr const char* kPackXTransNchwKernelName       = "demosaicnet_pack_xtrans_nchw_to_nhwc4";
inline constexpr const char* kResidualAddCropKernelName      = "demosaicnet_residual_add_crop";
inline constexpr const char* kUnpackCropConcatKernelName     = "demosaicnet_unpack_crop_concat_nhwc4";
inline constexpr const char* kFormPostInputC6KernelName      = "demosaicnet_form_post_input_c6";
inline constexpr const char* kOutputRgbHwcKernelName         = "demosaicnet_output_rgb_hwc";
inline constexpr const char* kOutputGammaHwcKernelName       = "demosaicnet_output_gamma_hwc";
inline constexpr const char* kAssembleRgbTileKernelName      = "demosaicnet_assemble_rgb_tile";

// Conservative default build options. Offline tuning (Phase 7) may replace the
// CLBlast-style WGD/MDIMCD/... constants; channel-block counts stay variant-fixed.
inline constexpr const char* kCommonConvBuildOptions =
    "-cl-std=CL1.2 -DWGD=8 -DMDIMCD=8 -DNDIMCD=8 -DMDIMAD=8 -DNDIMBD=8 "
    "-DKWID=1 -DVWMD=1 -DVWND=1 -DPADA=1 -DPADB=1 -DRELAX_WORKGROUP_SIZE=1 "
    "-DUSE_INLINE_KEYWORD=1 -DFUSE_BIAS=1 -DFUSE_RELU=1";

// Bayer trunk C24 → 6 float4 blocks; X-Trans trunk C32 → 8 float4 blocks.
inline constexpr const char* kBayerConvBuildOptions =
    "-cl-std=CL1.2 -DWGD=8 -DMDIMCD=8 -DNDIMCD=8 -DMDIMAD=8 -DNDIMBD=8 "
    "-DKWID=1 -DVWMD=1 -DVWND=1 -DPADA=1 -DPADB=1 -DRELAX_WORKGROUP_SIZE=1 "
    "-DUSE_INLINE_KEYWORD=1 -DFUSE_BIAS=1 -DFUSE_RELU=1 "
    "-DIN_CHANNEL_BLOCKS=6 -DOUT_CHANNEL_BLOCKS=6 "
    "-DIN_LOGICAL_CHANNELS=24 -DOUT_LOGICAL_CHANNELS=24";

inline constexpr const char* kXTransConvBuildOptions =
    "-cl-std=CL1.2 -DWGD=8 -DMDIMCD=8 -DNDIMCD=8 -DMDIMAD=8 -DNDIMBD=8 "
    "-DKWID=1 -DVWMD=1 -DVWND=1 -DPADA=1 -DPADB=1 -DRELAX_WORKGROUP_SIZE=1 "
    "-DUSE_INLINE_KEYWORD=1 -DFUSE_BIAS=1 -DFUSE_RELU=1 "
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
