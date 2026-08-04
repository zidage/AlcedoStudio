//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {

void RegisterOpenClRawProcessorPrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = "raw_processor",
        .programs =
            {
                OpenClProgramDescriptor{
                    .name            = "raw_processor_core",
                    .source_paths    = {ALCEDO_OPENCL_RAW_UTILS_CL, ALCEDO_OPENCL_RAW_TO_LINEAR_REF_CL},
                    .build_options   = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name            = "raw_processor_debayer_rcd",
                    .source_paths    = {ALCEDO_OPENCL_RAW_UTILS_CL, ALCEDO_OPENCL_DEBAYER_RCD_CL},
                    .build_options   = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name            = "raw_processor_xtrans",
                    .source_paths    = {ALCEDO_OPENCL_XTRANS_INTERPOLATE_CL},
                    .build_options   = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name            = "raw_processor_highlight",
                    .source_paths    = {ALCEDO_OPENCL_HIGHLIGHT_RECONSTRUCT_CL},
                    .build_options   = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
                OpenClProgramDescriptor{
                    .name            = "raw_processor_cvt_ref_space",
                    .source_paths    = {ALCEDO_OPENCL_CVT_REF_SPACE_CL},
                    .build_options   = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif
