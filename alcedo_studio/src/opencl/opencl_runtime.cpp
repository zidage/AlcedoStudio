//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_runtime.hpp"

#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {

void PrepareOpenClRuntime(const OpenClInitializationOptions& options) {
  InitializeOpenClRuntime(options);
  WarmUpOpenClRuntime();
}

void InitializeOpenClRuntime(const OpenClInitializationOptions& options) {
  RegisterOpenClBackendPrograms();
  OpenClContext::Instance().Initialize(options);
}

auto TryInitializeOpenClRuntime(const OpenClInitializationOptions& options) -> bool {
  try {
    InitializeOpenClRuntime(options);
    return true;
  } catch (...) {
    return false;
  }
}

void WarmUpOpenClRuntime() {
  // Only programs marked required_at_startup are compiled during warm-up.
  // Optional Neural (DemosaicNet) programs stay lazy until first explicit use.
  OpenClProgramLibrary::Instance().WarmUpRequiredPrograms();
}

auto TryPrepareOpenClRuntime(const OpenClInitializationOptions& options) -> bool {
  try {
    if (!TryInitializeOpenClRuntime(options)) {
      return false;
    }
    WarmUpOpenClRuntime();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace alcedo

#endif
