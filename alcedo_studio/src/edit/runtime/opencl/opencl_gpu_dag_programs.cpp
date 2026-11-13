//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_dag_programs.hpp"

#include <mutex>

#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {

void RegisterOpenClGpuDagPrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = OpenCL::GpuDag::kManifestName,
        .programs =
            {
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kGeometryCameraProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_DNG_PROFILE_MATH_H, ALCEDO_OPENCL_ACES_RGC_H,
                                            ALCEDO_OPENCL_DAG_GEOMETRY_CAMERA_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kPrimaryGradeProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_DAG_OKLAB_CL, ALCEDO_OPENCL_DAG_PRIMARY_GRADE_CL,
                                            ALCEDO_OPENCL_DAG_PRIMARY_GRADE_NEIGHBOR_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kLocalToneProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_DAG_LOCAL_TONE_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kMaskProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_DAG_OKLAB_CL, ALCEDO_OPENCL_DAG_MASK_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kAiMaskCoverageProgramName,
                    .source_paths        = {ALCEDO_OPENCL_DAG_AI_MASK_COVERAGE_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kDisplayToAp1ProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_RASTER_LINEARIZE_MATH_H,
                                            ALCEDO_OPENCL_ACES2_REFERENCE_MATH_H,
                                            ALCEDO_OPENCL_DISPLAY_TO_AP1_MATH_H,
                                            ALCEDO_OPENCL_DAG_DISPLAY_TO_AP1_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name = OpenCL::GpuDag::kDrtProgramName,
                    // FP6 of Negative Scan shows its linear AP1 with the display transform of this
                    // program, so it has its kernels here (film_print_scan.cl), with the grain of
                    // the negative.
                    .source_paths =
                        {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H, ALCEDO_OPENCL_ACES_RGC_H,
                         ALCEDO_OPENCL_PROFILE_TONE_MATH_H, ALCEDO_OPENCL_DAG_DRT_PARAMS_CL,
                         ALCEDO_OPENCL_DAG_COMMON_CL, ALCEDO_OPENCL_DAG_CST_CL,
                         ALCEDO_OPENCL_DAG_DRT_CL, ALCEDO_OPENCL_FILM_PRINT_MATH_H,
                         ALCEDO_OPENCL_FILM_GRAIN_FIELD_H, ALCEDO_OPENCL_FILM_GRAIN_TILE_H,
                         ALCEDO_OPENCL_FILM_GRAIN_NEGATIVE_H, ALCEDO_OPENCL_DAG_FILM_PRINT_SCAN_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name = OpenCL::GpuDag::kFilmPrintProgramName,
                    // film_grain_negative.cl is the grain of the negative alone, for the backend
                    // tests; it has the build options of the FP6 kernels that run the same steps.
                    .source_paths =
                        {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H, ALCEDO_OPENCL_ACES_RGC_H,
                         ALCEDO_OPENCL_FILM_PRINT_MATH_H, ALCEDO_OPENCL_FILM_GRAIN_FIELD_H,
                         ALCEDO_OPENCL_FILM_GRAIN_TILE_H, ALCEDO_OPENCL_FILM_GRAIN_NEGATIVE_H,
                         ALCEDO_OPENCL_DAG_FILM_PRINT_CL, ALCEDO_OPENCL_DAG_FILM_GRAIN_NEGATIVE_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kFilmGrainProgramName,
                    .source_paths        = {ALCEDO_OPENCL_COLOR_ENCODING_MATH_H,
                                            ALCEDO_OPENCL_FILM_GRAIN_FIELD_H,
                                            ALCEDO_OPENCL_FILM_GRAIN_TILE_H,
                                            ALCEDO_OPENCL_FILM_GRAIN_POST_H,
                                            ALCEDO_OPENCL_DAG_FILM_GRAIN_POST_CL},
                    // The power of color_encoding_math.h as exp2(y log2 x): see CE_POW.
                    .build_options       = "-cl-std=CL1.2 -DCE_OPENCL_POW_EXP2_LOG2",
                    .required_at_startup = false,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
