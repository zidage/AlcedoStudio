//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "opencl/opencl_program_library.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {
namespace {

auto UniqueProgramName(const char* suffix) -> std::string {
  static int sequence = 0;
  return std::string("opencl_program_library_test_") + suffix + "_" + std::to_string(++sequence);
}

auto AnySourcePath() -> std::filesystem::path {
  return std::filesystem::path("unused_test_source.cl");
}

TEST(OpenClProgramLibraryTest, RejectsEmptyProgramName) {
  EXPECT_THROW(OpenClProgramLibrary::Instance().RegisterProgram(
                   OpenClProgramDescriptor{.name = "", .source_paths = {AnySourcePath()}}),
               std::runtime_error);
}

TEST(OpenClProgramLibraryTest, RejectsProgramWithoutSources) {
  EXPECT_THROW(OpenClProgramLibrary::Instance().RegisterProgram(
                   OpenClProgramDescriptor{.name = UniqueProgramName("empty_sources")}),
               std::runtime_error);
}

TEST(OpenClProgramLibraryTest, RejectsDuplicateProgramRegistration) {
  const auto name = UniqueProgramName("duplicate");
  OpenClProgramLibrary::Instance().RegisterProgram(
      OpenClProgramDescriptor{.name = name, .source_paths = {AnySourcePath()}});

  EXPECT_THROW(OpenClProgramLibrary::Instance().RegisterProgram(
                   OpenClProgramDescriptor{.name = name, .source_paths = {AnySourcePath()}}),
               std::runtime_error);
}

TEST(OpenClProgramLibraryTest, RegisteredProgramNamesAreSorted) {
  const auto late_name  = UniqueProgramName("zeta");
  const auto early_name = UniqueProgramName("alpha");
  OpenClProgramLibrary::Instance().RegisterProgram(
      OpenClProgramDescriptor{.name = late_name, .source_paths = {AnySourcePath()}});
  OpenClProgramLibrary::Instance().RegisterProgram(
      OpenClProgramDescriptor{.name = early_name, .source_paths = {AnySourcePath()}});

  const auto names = OpenClProgramLibrary::Instance().RegisteredProgramNames();
  ASSERT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_NE(std::find(names.begin(), names.end(), late_name), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), early_name), names.end());
}

TEST(OpenClProgramLibraryTest, WarmUpRequiredProgramsDoesNothingWhenNoRequiredProgramsExist) {
  const auto optional_name = UniqueProgramName("optional");
  OpenClProgramLibrary::Instance().RegisterProgram(OpenClProgramDescriptor{
      .name                = optional_name,
      .source_paths        = {AnySourcePath()},
      .required_at_startup = false,
  });

  EXPECT_NO_THROW(OpenClProgramLibrary::Instance().WarmUpRequiredPrograms());
  EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(optional_name));
}

TEST(OpenClProgramLibraryTest, IsProgramBuiltIsFalseForUnregisteredName) {
  EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      "opencl_program_library_test_never_registered"));
}

TEST(OpenClProgramLibraryTest, FormatsUnicodePathsAsUtf8ForDiagnostics) {
#if defined(_WIN32)
  const std::filesystem::path unicode_path = std::filesystem::path(L"C:\\测试目录\\着色器.cl");
  const auto                  formatted    = detail::FormatPathForOpenClDiagnostics(unicode_path);
  const auto                  expected_u8  = std::u8string(u8"C:\\测试目录\\着色器.cl");
  const auto                  expected     = std::string(expected_u8.begin(), expected_u8.end());

  EXPECT_EQ(formatted, expected);
#else
  GTEST_SKIP() << "Windows-specific UTF-16 path formatting test.";
#endif
}

TEST(OpenClProgramLibraryTest, DemosaicNetProgramsAreRegisteredLazyAndUnbuiltAfterBackendRegister) {
  RegisterOpenClBackendPrograms();

  const auto manifest_names =
      OpenClBackendProgramRegistry::Instance().RegisteredManifestNames();
  EXPECT_NE(std::find(manifest_names.begin(), manifest_names.end(),
                      OpenCL::DemosaicNet::kManifestName),
            manifest_names.end());

  const char* demosaicnet_programs[] = {
      OpenCL::DemosaicNet::kConvBayerProgramName,
      OpenCL::DemosaicNet::kConvXTransProgramName,
      OpenCL::DemosaicNet::kStructuralProgramName,
  };

  const auto names = OpenClProgramLibrary::Instance().RegisteredProgramNames();
  for (const char* program_name : demosaicnet_programs) {
    EXPECT_NE(std::find(names.begin(), names.end(), program_name), names.end()) << program_name;
    EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(program_name)) << program_name;
  }

  // Application warm-up must not compile optional Neural programs.
  EXPECT_NO_THROW(OpenClProgramLibrary::Instance().WarmUpRequiredPrograms());
  for (const char* program_name : demosaicnet_programs) {
    EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(program_name)) << program_name;
  }
}

}  // namespace
}  // namespace alcedo
