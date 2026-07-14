//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"

#include <mutex>

#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {

void RegisterOpenClDemosaicNetPrograms() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Neural programs are optional and large. Keep required_at_startup = false so
    // application warm-up never compiles them; first explicit GetProgram builds once.
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = OpenCL::DemosaicNet::kManifestName,
        .programs =
            {
                OpenClProgramDescriptor{
                    .name = OpenCL::DemosaicNet::kConvBayerProgramName,
                    .source_paths = {ALCEDO_OPENCL_DEMOSAICNET_CONV_CL},
                    .build_options = OpenCL::DemosaicNet::kBayerConvBuildOptions,
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name = OpenCL::DemosaicNet::kConvXTransProgramName,
                    .source_paths = {ALCEDO_OPENCL_DEMOSAICNET_CONV_CL},
                    .build_options = OpenCL::DemosaicNet::kXTransConvBuildOptions,
                    .required_at_startup = false,
                },
                OpenClProgramDescriptor{
                    .name = OpenCL::DemosaicNet::kStructuralProgramName,
                    .source_paths = {ALCEDO_OPENCL_DEMOSAICNET_STRUCTURAL_CL},
                    .build_options = OpenCL::DemosaicNet::kStructuralBuildOptions,
                    .required_at_startup = false,
                },
            },
    });
  });
}

}  // namespace alcedo

#endif
