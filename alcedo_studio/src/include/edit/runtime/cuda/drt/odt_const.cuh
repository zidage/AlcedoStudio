//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

// JMh Constants
namespace alcedo {
namespace CUDA {

// Table generation
__constant__ int   tableSize              = 360;
__constant__ int   additionalTableEntries = 2;  // allots for extra entries to wrap the hues
__constant__ int   totalTableSize         = 362;
__constant__ int   baseIndex          = 1;  // array index for smallest filled entry of padded table

__constant__ float hue_limit          = 360.f;

__constant__ int   cuspCornerCount    = 6;
__constant__ int   totalCornerCount   = 8;
__constant__ int   max_sorted_corners = 12;
__constant__ float reach_cusp_tolerance   = 1e-3f;
__constant__ float display_cusp_tolerance = 1e-7f;

__constant__ float gamma_minimum          = 0.0f;
__constant__ float gamma_maximum          = 5.0f;
__constant__ float gamma_search_step      = 0.4f;
__constant__ float gamma_accuracy         = 1e-5f;

// CAM Parameters
__constant__ float ref_luminance          = 100.f;
__constant__ float L_A                    = 100.f;
__constant__ float Y_b                    = 20.f;
__constant__ float surround[3]            = {0.9f, 0.59f, 0.9f};  // Dim surround
__constant__ float J_scale                = 100.0f;
__constant__ float cam_nl_Y_reference     = 100.0f;
__constant__ float cam_nl_offset          = 27.13f;
__constant__ float cam_nl_scale           = 400.f;

__constant__ float model_gamma            = 1.13705599f;

// Chroma compression
__constant__ float chroma_compress        = 2.4f;
__constant__ float chroma_compress_fact   = 3.3f;
__constant__ float chroma_expand          = 1.3f;
__constant__ float chroma_expand_fact     = 0.69f;
__constant__ float chroma_expand_thr      = 0.5f;

// Gamut compression
__constant__ float smooth_cusps           = 0.12f;
__constant__ float smooth_m               = 0.27f;
__constant__ float cusp_mid_blend         = 1.3f;

__constant__ float focus_gain_blend       = 0.3f;
__constant__ float focus_adjust_gain      = 0.55f;
__constant__ float focus_distance         = 1.35f;
__constant__ float focus_distance_scaling = 1.75f;

__constant__ float compression_threshold  = 0.75f;

// Near-black CAM16/Hellwig guards (deviate from Academy CTL).
// hunt_nJ: Hunt colorfulness fade; 50% at J = hunt_nJ * limit_J_max (~1.2 J).
// chroma_j_floor: minimum |J| used when rescaling M through the tonescale.
// rgb_mapping_failure_ratio: chroma-preserving peak clamp is only valid near the
// tonescale peak; larger RGB is a JMh mapping failure and must not become white.
__constant__ float hunt_nJ                     = 0.012f;
__constant__ float chroma_j_floor              = 0.25f;
__constant__ float rgb_mapping_failure_ratio   = 8.0f;

__constant__ float AP0_to_XYZ[9]     = {0.986519f,  0.023971f,  -0.010490f, 0.359689f, 0.714586f,
                                        -0.074275f, -0.000386f, 0.000029f,  1.000356f};

__constant__ float AP0_XYZ_TO_RGB[9] = {
    1.04981101f, -0.495903015f,    0.f,           0.f,         1.37331307f,
    0.f,         -9.74845389e-05f, 0.0982400328f, 0.991252005f};

__constant__ float AP1_to_XYZ[9]       = {0.687209f, 0.159347f,  0.153444f, 0.282567f, 0.664611f,
                                          0.052823f, -0.005795f, 0.003998f, 1.001797f};

__constant__ float CAM16_XYZ_TO_RGB[9] = {0.364074498f,  -0.222245097f, -0.002067619f,
                                          0.594700813f,  1.07385552f,   0.0488260463f,
                                          0.0411012731f, 0.14794533f,   0.950387537f};

__constant__ float AP0_TO_AP1[9]    = {1.45143926f,   -0.0765537769f, 0.00831614807f,
                                          -0.236510754f, 1.17622972f,    -0.00603244966f,
                                          -0.214928567f, -0.0996759236f, 0.997716308f};
__constant__ float AP1_TO_AP0[9]    = {0.695452213f, 0.0447945632f, -0.00552588236f,
                                          0.140678704f, 0.859671116f,  0.00402521016f,
                                          0.163869068f, 0.0955343172f, 1.00150073f};

__constant__ float MATRIX_IDENTITY[9]  = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
}  // namespace CUDA
}  // namespace alcedo