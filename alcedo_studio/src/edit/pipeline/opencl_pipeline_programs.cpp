//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/pipeline/opencl_pipeline_programs.hpp"
#include "opencl/opencl_backend_program_registry.hpp"

#include <mutex>

namespace alcedo {

void RegisterOpenClEditPipelinePrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = OpenCL::Pipeline::kManifestName,
        .programs =
            {
                OpenClProgramDescriptor{
                    .name = OpenCL::Pipeline::kFusedProgramName,
                    .source_paths =
                        {
                            ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_COMMON_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_BASIC_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_VALIDATION_CL,
                        },
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name = OpenCL::Pipeline::kDetailProgramName,
                    .source_paths =
                        {
                            ALCEDO_OPENCL_PRNG_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_COMMON_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_FILM_GRAIN_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_HALATION_CL,
                            ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_CL,
                        },
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif
