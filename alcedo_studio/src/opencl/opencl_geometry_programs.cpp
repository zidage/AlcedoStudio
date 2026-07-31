//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_geometry_programs.hpp"

#include <mutex>

#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {

void RegisterOpenClGeometryPrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = OpenCL::Geometry::kManifestName,
        .programs =
            {
                OpenClProgramDescriptor{
                    .name                = OpenCL::Geometry::kGeometryProgramName,
                    .source_paths        = {ALCEDO_OPENCL_GEOMETRY_UTILS_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name                = OpenCL::Geometry::kLensCalibProgramName,
                    .source_paths        = {ALCEDO_OPENCL_EDIT_GEOMETRY_LENS_CALIB_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif
