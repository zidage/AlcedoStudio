//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "opencl/opencl_context.hpp"

namespace alcedo {

// Use for an explicit user selection of OpenCL. Failure should surface.
void PrepareOpenClRuntime(const OpenClInitializationOptions& options = {});

// Initializes the OpenCL context and registers builtin programs without compiling
// startup kernels. Use this when the UI must become visible before warm-up.
void InitializeOpenClRuntime(const OpenClInitializationOptions& options = {});
auto TryInitializeOpenClRuntime(const OpenClInitializationOptions& options = {}) -> bool;

// Compiles only programs marked required_at_startup after the context is
// initialized. Optional Neural (DemosaicNet) programs remain uncompiled until
// first explicit OpenClProgramLibrary::GetProgram use.
void WarmUpOpenClRuntime();

// Use for automatic fallback probing. Failure should not interrupt the caller's
// attempt to continue with another backend.
auto TryPrepareOpenClRuntime(const OpenClInitializationOptions& options = {}) -> bool;

}  // namespace alcedo

#endif
