//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/scope/opencl_scope_programs.hpp"

#include <mutex>

#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {

void RegisterOpenClScopePrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = OpenCL::Scope::kManifestName,
        .programs =
            {
                OpenClProgramDescriptor{
                    .name                = OpenCL::Scope::kScopeProgramName,
                    .source_paths        = {ALCEDO_OPENCL_SCOPE_ANALYZER_CL},
                    .build_options       = "-cl-std=CL1.2",
                    .required_at_startup = true,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif
