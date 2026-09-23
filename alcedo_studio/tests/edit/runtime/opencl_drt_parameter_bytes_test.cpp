//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "drt_expected_data_support.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_drt_gpu_params.hpp"
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

// The stored files were packed through ODT_Op and ResolveOpenClDrtParams at commit 0cf45f45.
// OpenClToOutputParams has no padding, so the whole struct is compared.
TEST(OpenClDrtParameterBytes, OpenClDrtParameterBytesMatchStoredExpectedBytes) {
  const std::filesystem::path directory(ALCEDO_DRT_EXPECTED_PARAMETER_DIR);
  const auto                  rows = drt_expected_data::ConfigurationMatrix();
  ASSERT_EQ(rows.size(), 12U);
  for (const auto& row : rows) {
    SCOPED_TRACE(row.file_stem_);
    ColorUtils::TO_OUTPUT_Params resolved;
    std::string                  error;
    ASSERT_TRUE(DrtOutputResolver::Resolve(row.payload_, nullptr, &resolved, &error)) << error;
    std::vector<std::byte> actual;
    drt_expected_data::AppendBytes(actual, PackOpenClDrtParams(resolved));
    const auto stored = drt_expected_data::ReadBytes(
        drt_expected_data::ExpectedParameterPath(directory, "opencl", row));
    ASSERT_EQ(stored.size(), sizeof(OpenCL::Pipeline::OpenClToOutputParams));
    EXPECT_EQ(drt_expected_data::FirstDifference(actual, stored), std::string::npos);
  }
}

// Every OpenCL source of the DRT program lives in the runtime tree: the shaders under
// edit/runtime/opencl/shader and the shared ACES gamut compression header under
// include/edit/runtime. The program must also build from those files.
TEST(OpenClDrtParameterBytes, OpenClDrtProgramBuildsFromRuntimeShaderDirectory) {
  RegisterOpenClGpuDagPrograms();
  OpenClBackendProgramRegistry::Instance().RegisterProgramsForManifest(
      OpenCL::GpuDag::kManifestName);
  const auto paths =
      OpenClProgramLibrary::Instance().RegisteredSourcePaths(OpenCL::GpuDag::kDrtProgramName);
  ASSERT_EQ(paths.size(), 5U);
  for (const auto& path : paths) {
    const auto text = path.generic_string();
    EXPECT_EQ(text.find("edit/pipeline"), std::string::npos) << text;
    if (path.extension() == ".cl") {
      EXPECT_NE(text.find("src/edit/runtime/opencl/shader/"), std::string::npos) << text;
    } else {
      EXPECT_NE(text.find("src/include/edit/runtime/"), std::string::npos) << text;
    }
  }
  EXPECT_EQ(paths[1].filename(), "drt_params.cl");
  EXPECT_EQ(paths[2].filename(), "common.cl");
  EXPECT_EQ(paths[3].filename(), "cst.cl");
  EXPECT_EQ(paths[4].filename(), "drt.cl");

  if (!TryInitializeOpenClRuntime() || !OpenClContext::Instance().Capabilities().image_support) {
    GTEST_SKIP() << "No OpenCL image device available to build the program.";
  }
  EXPECT_NE(OpenClProgramLibrary::Instance().GetProgram(OpenCL::GpuDag::kDrtProgramName), nullptr);
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kDrtProgramName));
}

}  // namespace
}  // namespace alcedo
