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
                    .source_paths        = {ALCEDO_OPENCL_DAG_GEOMETRY_CAMERA_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kPrimaryGradeProgramName,
                    .source_paths        = {ALCEDO_OPENCL_DAG_PRIMARY_GRADE_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kLocalToneProgramName,
                    .source_paths        = {ALCEDO_OPENCL_DAG_LOCAL_TONE_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kMaskProgramName,
                    .source_paths        = {ALCEDO_OPENCL_DAG_MASK_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::GpuDag::kDrtProgramName,
                    .source_paths        = {ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL,
                                            ALCEDO_OPENCL_EDIT_PIPELINE_COMMON_CL,
                                            ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL,
                                            ALCEDO_OPENCL_DAG_DRT_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
