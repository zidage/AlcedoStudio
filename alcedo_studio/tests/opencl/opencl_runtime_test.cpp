//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "edit/pipeline/opencl_pipeline_programs.hpp"
#include "edit/scope/opencl_scope_programs.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_geometry_programs.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

auto UniqueProgramName(const char* suffix) -> std::string {
  static int sequence = 0;
  return std::string("opencl_runtime_smoke_test_") + suffix + "_" + std::to_string(++sequence);
}

auto MakeUniqueTempDirectory() -> std::filesystem::path {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
  const auto path = std::filesystem::temp_directory_path() /
                    (std::wstring(L"alcedo_opencl_测试_") + std::to_wstring(tick));
#else
  const auto path =
      std::filesystem::temp_directory_path() / ("alcedo_opencl_test_" + std::to_string(tick));
#endif
  std::filesystem::create_directories(path);
  return path;
}

class TempDirectory {
 private:
  std::filesystem::path path_;

 public:
  TempDirectory() : path_(MakeUniqueTempDirectory()) {}

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  auto Path() const -> const std::filesystem::path& { return path_; }
};

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file << text;
  ASSERT_TRUE(file.good());
}

auto TryEnsureOpenClContext() -> bool {
  auto& context = OpenClContext::Instance();
  return context.TryInitialize();
}

class ClMemGuard {
 private:
  cl_mem value_ = nullptr;

 public:
  explicit ClMemGuard(cl_mem value) : value_(value) {}
  ~ClMemGuard() {
    if (value_ != nullptr) {
      clReleaseMemObject(value_);
    }
  }

  auto Get() const -> cl_mem { return value_; }
};

class ClKernelGuard {
 private:
  cl_kernel value_ = nullptr;

 public:
  explicit ClKernelGuard(cl_kernel value) : value_(value) {}
  ~ClKernelGuard() {
    if (value_ != nullptr) {
      clReleaseKernel(value_);
    }
  }

  auto Get() const -> cl_kernel { return value_; }
};

struct RegisteredProgram {
  std::string name;
  cl_program  program = nullptr;
};

auto BuildProgramFromSource(const std::string& suffix, const std::filesystem::path& source_path,
                            bool required_at_startup = false) -> RegisteredProgram {
  const auto program_name  = UniqueProgramName(suffix.c_str());
  const auto manifest_name = program_name + "_mock_manifest";
  OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
      .name = manifest_name,
      .programs =
          {
              OpenClProgramDescriptor{
                  .name                = program_name,
                  .source_paths        = {source_path},
                  .required_at_startup = required_at_startup,
              },
          },
  });
  RegisterOpenClBackendPrograms();
  if (required_at_startup) {
    OpenClProgramLibrary::Instance().WarmUpRequiredPrograms();
  }
  return RegisteredProgram{
      .name    = program_name,
      .program = OpenClProgramLibrary::Instance().GetProgram(program_name),
  };
}

auto ComputeCpuMidpointSquareIntegral(cl_uint sample_count) -> double {
  const double dx  = 1.0 / static_cast<double>(sample_count);
  double       sum = 0.0;
  for (cl_uint index = 0; index < sample_count; ++index) {
    const double x = (static_cast<double>(index) + 0.5) * dx;
    sum += x * x * dx;
  }
  return sum;
}

auto ComputeCpuIteratedIntegral(cl_uint sample_count, cl_uint iteration_count) -> double {
  const float dx  = 1.0F / static_cast<float>(sample_count);
  double      sum = 0.0;
  for (cl_uint index = 0; index < sample_count; ++index) {
    const float x     = (static_cast<float>(index) + 0.5F) * dx;
    float       value = x;
    for (cl_uint iteration = 0; iteration < iteration_count; ++iteration) {
      value = value * 0.99991F + 0.00009F * (x + static_cast<float>(iteration & 7U));
    }
    sum += static_cast<double>(value * value * dx);
  }
  return sum;
}

auto RunMidpointSquareIntegration(OpenClContext& context, cl_program program, cl_uint sample_count)
    -> double {
  std::vector<float> partials(sample_count, 0.0F);

  cl_int             error = CL_SUCCESS;
  ClMemGuard         partials_buffer(clCreateBuffer(context.Context(), CL_MEM_WRITE_ONLY,
                                                    partials.size() * sizeof(float), nullptr, &error));
  EXPECT_EQ(error, CL_SUCCESS);
  EXPECT_NE(partials_buffer.Get(), nullptr);
  if (error != CL_SUCCESS || partials_buffer.Get() == nullptr) {
    return 0.0;
  }

  ClKernelGuard kernel(clCreateKernel(program, "integrate_square", &error));
  EXPECT_EQ(error, CL_SUCCESS);
  EXPECT_NE(kernel.Get(), nullptr);
  if (error != CL_SUCCESS || kernel.Get() == nullptr) {
    return 0.0;
  }

  const auto partials_mem = partials_buffer.Get();
  EXPECT_EQ(clSetKernelArg(kernel.Get(), 0, sizeof(cl_mem), &partials_mem), CL_SUCCESS);
  EXPECT_EQ(clSetKernelArg(kernel.Get(), 1, sizeof(cl_uint), &sample_count), CL_SUCCESS);

  const size_t global_work_size = static_cast<size_t>(sample_count);
  EXPECT_EQ(clEnqueueNDRangeKernel(context.Queue(), kernel.Get(), 1, nullptr, &global_work_size,
                                   nullptr, 0, nullptr, nullptr),
            CL_SUCCESS);
  EXPECT_EQ(
      clEnqueueReadBuffer(context.Queue(), partials_buffer.Get(), CL_TRUE, 0,
                          partials.size() * sizeof(float), partials.data(), 0, nullptr, nullptr),
      CL_SUCCESS);

  return std::accumulate(partials.begin(), partials.end(), 0.0);
}

auto RunIteratedIntegration(OpenClContext& context, cl_program program, cl_uint sample_count,
                            cl_uint iteration_count) -> double {
  std::vector<float> partials(sample_count, 0.0F);

  cl_int             error = CL_SUCCESS;
  ClMemGuard         partials_buffer(clCreateBuffer(context.Context(), CL_MEM_WRITE_ONLY,
                                                    partials.size() * sizeof(float), nullptr, &error));
  EXPECT_EQ(error, CL_SUCCESS);
  EXPECT_NE(partials_buffer.Get(), nullptr);
  if (error != CL_SUCCESS || partials_buffer.Get() == nullptr) {
    return 0.0;
  }

  ClKernelGuard kernel(clCreateKernel(program, "integrate_iterated", &error));
  EXPECT_EQ(error, CL_SUCCESS);
  EXPECT_NE(kernel.Get(), nullptr);
  if (error != CL_SUCCESS || kernel.Get() == nullptr) {
    return 0.0;
  }

  const auto partials_mem = partials_buffer.Get();
  EXPECT_EQ(clSetKernelArg(kernel.Get(), 0, sizeof(cl_mem), &partials_mem), CL_SUCCESS);
  EXPECT_EQ(clSetKernelArg(kernel.Get(), 1, sizeof(cl_uint), &sample_count), CL_SUCCESS);
  EXPECT_EQ(clSetKernelArg(kernel.Get(), 2, sizeof(cl_uint), &iteration_count), CL_SUCCESS);

  const size_t global_work_size = static_cast<size_t>(sample_count);
  EXPECT_EQ(clEnqueueNDRangeKernel(context.Queue(), kernel.Get(), 1, nullptr, &global_work_size,
                                   nullptr, 0, nullptr, nullptr),
            CL_SUCCESS);
  EXPECT_EQ(
      clEnqueueReadBuffer(context.Queue(), partials_buffer.Get(), CL_TRUE, 0,
                          partials.size() * sizeof(float), partials.data(), 0, nullptr, nullptr),
      CL_SUCCESS);

  return std::accumulate(partials.begin(), partials.end(), 0.0);
}

auto BuildIntegrationProgram(const std::filesystem::path& source_path, bool required_at_startup)
    -> RegisteredProgram {
  WriteTextFile(source_path, R"(
__kernel void integrate_square(__global float* partials, const uint sample_count) {
  const size_t gid = get_global_id(0);
  const float dx = 1.0f / (float)sample_count;
  const float x = ((float)gid + 0.5f) * dx;
  partials[gid] = x * x * dx;
}
)");
  return BuildProgramFromSource("integrate_square", source_path, required_at_startup);
}

auto BuildIteratedIntegrationProgram(const std::filesystem::path& source_path,
                                     bool required_at_startup) -> RegisteredProgram {
  WriteTextFile(source_path, R"(
__kernel void integrate_iterated(__global float* partials, const uint sample_count,
                                 const uint iteration_count) {
  const size_t gid = get_global_id(0);
  const float dx = 1.0f / (float)sample_count;
  const float x = ((float)gid + 0.5f) * dx;
  float value = x;
  for (uint iteration = 0; iteration < iteration_count; ++iteration) {
    value = value * 0.99991f + 0.00009f * (x + (float)(iteration & 7u));
  }
  partials[gid] = value * value * dx;
}
)");
  return BuildProgramFromSource("integrate_iterated", source_path, required_at_startup);
}

TEST(OpenClRuntimeTest, ContextInitializesAndReportsUsableCapabilities) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }
  const auto capabilities = context.Capabilities();

  EXPECT_TRUE(context.IsInitialized());
  EXPECT_TRUE(capabilities.available);
  EXPECT_TRUE(capabilities.compiler_available);
  EXPECT_GT(capabilities.compute_units, 0U);
  EXPECT_GT(capabilities.global_memory_bytes, 0U);
  EXPECT_GT(capabilities.max_work_group_size, 0U);
}

TEST(OpenClRuntimeTest, WarmUpCompilesProgramFromUnicodePath) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  TempDirectory temp_directory;
#if defined(_WIN32)
  const auto source_path = temp_directory.Path() / std::filesystem::path(L"并行测试.cl");
#else
  const auto source_path = temp_directory.Path() / "parallel_smoke.cl";
#endif
  WriteTextFile(source_path, R"(
__kernel void write_one(__global int* output) {
  output[get_global_id(0)] = 1;
}
)");

  const auto program = BuildProgramFromSource("unicode_path", source_path, true);
  EXPECT_NE(program.program, nullptr);
}

TEST(OpenClRuntimeTest, BuiltinEditPipelineFusedParamsProgramCompiles) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();
  cl_program program =
      OpenClProgramLibrary::Instance().GetProgram(OpenCL::Pipeline::kFusedProgramName);
  ASSERT_NE(program, nullptr);

  cl_int    error = CL_SUCCESS;
  cl_kernel kernel =
      clCreateKernel(program, OpenCL::Pipeline::kValidateFusedParamsKernelName, &error);
  EXPECT_EQ(error, CL_SUCCESS);
  EXPECT_NE(kernel, nullptr);
  if (kernel != nullptr) {
    clReleaseKernel(kernel);
  }
}

TEST(OpenClRuntimeTest, BuiltinEditPipelineDetailProgramCompiles) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();
  cl_program program =
      OpenClProgramLibrary::Instance().GetProgram(OpenCL::Pipeline::kDetailProgramName);
  ASSERT_NE(program, nullptr);

  const char* detail_kernels[] = {
      OpenCL::Pipeline::kNeighborBlurHorizontalKernelName,
      OpenCL::Pipeline::kNeighborApplyVerticalKernelName,
      OpenCL::Pipeline::kHsExtractLogIntensityKernelName,
      OpenCL::Pipeline::kHsApplyAdjustedLKernelName,
  };
  for (const char* kernel_name : detail_kernels) {
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(program, kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << kernel_name;
    EXPECT_NE(kernel, nullptr) << kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }
}

TEST(OpenClRuntimeTest, BuiltinRawProcessorProgramsCompile) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();
  struct ProgramKernel {
    const char* program_name;
    const char* kernel_name;
  };
  const ProgramKernel raw_kernels[] = {
      {"raw_processor_core", "to_linear_ref_u16_to_f32"},
      {"raw_processor_debayer_rcd", "rcd_init_and_vh"},
      {"raw_processor_xtrans", "xtrans_green"},
      {"raw_processor_highlight", "hlr_build_mask"},
      {"raw_processor_cvt_ref_space", "apply_inverse_cam_mul_rgba32f"},
  };

  for (const auto& item : raw_kernels) {
    cl_program program = OpenClProgramLibrary::Instance().GetProgram(item.program_name);
    ASSERT_NE(program, nullptr) << item.program_name;
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(program, item.kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << item.program_name << "." << item.kernel_name;
    EXPECT_NE(kernel, nullptr) << item.program_name << "." << item.kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }
}

TEST(OpenClRuntimeTest, BuiltinGeometryProgramsCompile) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();
  cl_program geometry_program =
      OpenClProgramLibrary::Instance().GetProgram(OpenCL::Geometry::kGeometryProgramName);
  ASSERT_NE(geometry_program, nullptr);
  cl_program lens_program =
      OpenClProgramLibrary::Instance().GetProgram(OpenCL::Geometry::kLensCalibProgramName);
  ASSERT_NE(lens_program, nullptr);

  const char* geometry_kernels[] = {
      OpenCL::Geometry::kCropResizeLinearKernelName,
      OpenCL::Geometry::kCropResizeAreaKernelName,
      OpenCL::Geometry::kWarpAffineLinearKernelName,
      OpenCL::Geometry::kRotateKernelName,
  };
  for (const char* kernel_name : geometry_kernels) {
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(geometry_program, kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << kernel_name;
    EXPECT_NE(kernel, nullptr) << kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }

  const char* lens_kernels[] = {
      OpenCL::Geometry::kLensVignettingKernelName,
      OpenCL::Geometry::kLensWarpKernelName,
  };
  for (const char* kernel_name : lens_kernels) {
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(lens_program, kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << kernel_name;
    EXPECT_NE(kernel, nullptr) << kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }
}

TEST(OpenClRuntimeTest, BuiltinScopeProgramCompiles) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();
  cl_program program =
      OpenClProgramLibrary::Instance().GetProgram(OpenCL::Scope::kScopeProgramName);
  ASSERT_NE(program, nullptr);

  const char* scope_kernels[] = {
      OpenCL::Scope::kHistogramKernelName,
      OpenCL::Scope::kWaveformKernelName,
  };
  for (const char* kernel_name : scope_kernels) {
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(program, kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << kernel_name;
    EXPECT_NE(kernel, nullptr) << kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }
}

TEST(OpenClRuntimeTest, VectorAddKernelExecutesAcrossManyWorkItems) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  TempDirectory temp_directory;
  const auto    source_path = temp_directory.Path() / "vector_add.cl";
  WriteTextFile(source_path, R"(
__kernel void vector_add(__global const float* lhs, __global const float* rhs,
                         __global float* output) {
  const size_t gid = get_global_id(0);
  output[gid] = lhs[gid] + rhs[gid];
}
)");
  const auto         program       = BuildProgramFromSource("vector_add", source_path);

  constexpr size_t   element_count = 4096;
  std::vector<float> lhs(element_count);
  std::vector<float> rhs(element_count);
  std::vector<float> output(element_count, 0.0F);
  for (size_t index = 0; index < element_count; ++index) {
    lhs[index] = static_cast<float>(index) * 0.25F;
    rhs[index] = static_cast<float>(element_count - index) * 0.5F;
  }

  cl_int     error = CL_SUCCESS;
  ClMemGuard lhs_buffer(clCreateBuffer(context.Context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       lhs.size() * sizeof(float), lhs.data(), &error));
  ASSERT_EQ(error, CL_SUCCESS);
  ASSERT_NE(lhs_buffer.Get(), nullptr);

  ClMemGuard rhs_buffer(clCreateBuffer(context.Context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       rhs.size() * sizeof(float), rhs.data(), &error));
  ASSERT_EQ(error, CL_SUCCESS);
  ASSERT_NE(rhs_buffer.Get(), nullptr);

  ClMemGuard output_buffer(clCreateBuffer(context.Context(), CL_MEM_WRITE_ONLY,
                                          output.size() * sizeof(float), nullptr, &error));
  ASSERT_EQ(error, CL_SUCCESS);
  ASSERT_NE(output_buffer.Get(), nullptr);

  ClKernelGuard kernel(clCreateKernel(program.program, "vector_add", &error));
  ASSERT_EQ(error, CL_SUCCESS);
  ASSERT_NE(kernel.Get(), nullptr);

  const auto lhs_mem    = lhs_buffer.Get();
  const auto rhs_mem    = rhs_buffer.Get();
  const auto output_mem = output_buffer.Get();
  ASSERT_EQ(clSetKernelArg(kernel.Get(), 0, sizeof(cl_mem), &lhs_mem), CL_SUCCESS);
  ASSERT_EQ(clSetKernelArg(kernel.Get(), 1, sizeof(cl_mem), &rhs_mem), CL_SUCCESS);
  ASSERT_EQ(clSetKernelArg(kernel.Get(), 2, sizeof(cl_mem), &output_mem), CL_SUCCESS);

  const size_t global_work_size = element_count;
  ASSERT_EQ(clEnqueueNDRangeKernel(context.Queue(), kernel.Get(), 1, nullptr, &global_work_size,
                                   nullptr, 0, nullptr, nullptr),
            CL_SUCCESS);
  ASSERT_EQ(clEnqueueReadBuffer(context.Queue(), output_buffer.Get(), CL_TRUE, 0,
                                output.size() * sizeof(float), output.data(), 0, nullptr, nullptr),
            CL_SUCCESS);

  for (size_t index = 0; index < element_count; ++index) {
    EXPECT_FLOAT_EQ(output[index], lhs[index] + rhs[index]);
  }
}

TEST(OpenClRuntimeTest, MidpointIntegrationMatchesCpuBaseline) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  TempDirectory     temp_directory;
  const auto        source_path  = temp_directory.Path() / "integrate_square.cl";
  const auto        program      = BuildIntegrationProgram(source_path, false);

  constexpr cl_uint sample_count = 1U << 16U;
  const double      cpu_integral = ComputeCpuMidpointSquareIntegral(sample_count);
  const double      opencl_integral =
      RunMidpointSquareIntegration(context, program.program, sample_count);

  EXPECT_NEAR(cpu_integral, 1.0 / 3.0, 1.0e-10);
  EXPECT_NEAR(opencl_integral, cpu_integral, 1.0e-4);
}

TEST(OpenClRuntimeTest, PrecompiledProgramAcceleratesComputeHeavyIntegration) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  TempDirectory temp_directory;
  const auto    source_path = temp_directory.Path() / "integrate_iterated_precompiled.cl";
  const auto    program     = BuildIteratedIntegrationProgram(source_path, true);

  // A second lookup should reuse the already-built program instead of compiling again.
  EXPECT_EQ(OpenClProgramLibrary::Instance().GetProgram(program.name), program.program);

  constexpr cl_uint sample_count    = 1U << 20U;
  constexpr cl_uint iteration_count = 256U;

  const auto        cpu_begin       = std::chrono::steady_clock::now();
  const auto        cpu_integral    = ComputeCpuIteratedIntegral(sample_count, iteration_count);
  const auto        cpu_end         = std::chrono::steady_clock::now();

  const auto        opencl_begin    = std::chrono::steady_clock::now();
  const auto        opencl_integral =
      RunIteratedIntegration(context, program.program, sample_count, iteration_count);
  const auto opencl_end = std::chrono::steady_clock::now();

  const auto cpu_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(cpu_end - cpu_begin)
          .count();
  const auto opencl_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                             opencl_end - opencl_begin)
                             .count();

  RecordProperty("cpu_baseline_ms", cpu_ms);
  RecordProperty("opencl_precompiled_ms", opencl_ms);
  RecordProperty("observed_speedup", cpu_ms / opencl_ms);
  std::cout << "CPU compute-heavy baseline: " << cpu_ms
            << " ms; precompiled OpenCL path: " << opencl_ms
            << " ms; observed speedup: " << cpu_ms / opencl_ms << "x\n";

  EXPECT_NEAR(opencl_integral, cpu_integral, 1.0e-3);
}

TEST(OpenClRuntimeTest, WarmUpOpenClRuntimeDoesNotCompileColdDemosaicNetPrograms) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();

  const char* demosaicnet_programs[] = {
      OpenCL::DemosaicNet::kConvBayerProgramName,
      OpenCL::DemosaicNet::kConvXTransProgramName,
      OpenCL::DemosaicNet::kStructuralProgramName,
  };

  // Snapshot built-state before warm-up. The process-wide library may already
  // hold programs compiled by an earlier test in this binary; the contract is
  // that warm-up never builds a still-cold Neural program.
  bool built_before[3] = {};
  for (size_t i = 0; i < 3; ++i) {
    built_before[i] = OpenClProgramLibrary::Instance().IsProgramBuilt(demosaicnet_programs[i]);
  }

  WarmUpOpenClRuntime();

  for (size_t i = 0; i < 3; ++i) {
    if (!built_before[i]) {
      EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(demosaicnet_programs[i]))
          << demosaicnet_programs[i];
    }
  }
}

TEST(OpenClRuntimeTest, DemosaicNetProgramsCompileOnceOnFirstUseAndReuse) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClContext()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  RegisterOpenClBackendPrograms();

  const char* demosaicnet_programs[] = {
      OpenCL::DemosaicNet::kConvBayerProgramName,
      OpenCL::DemosaicNet::kConvXTransProgramName,
      OpenCL::DemosaicNet::kStructuralProgramName,
  };

  for (const char* program_name : demosaicnet_programs) {
    const bool built_before = OpenClProgramLibrary::Instance().IsProgramBuilt(program_name);
    cl_program first = OpenClProgramLibrary::Instance().GetProgram(program_name);
    ASSERT_NE(first, nullptr) << program_name;
    EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(program_name)) << program_name;

    cl_program second = OpenClProgramLibrary::Instance().GetProgram(program_name);
    EXPECT_EQ(first, second) << program_name;
    if (!built_before) {
      // First GetProgram is the compile path; second must reuse the same object.
      EXPECT_NE(first, nullptr);
    }
  }

  struct ProgramKernel {
    const char* program_name;
    const char* kernel_name;
  };
  const ProgramKernel demosaicnet_kernels[] = {
      {OpenCL::DemosaicNet::kConvBayerProgramName, OpenCL::DemosaicNet::kConv3x3KernelName},
      {OpenCL::DemosaicNet::kConvBayerProgramName, OpenCL::DemosaicNet::kConv1x1KernelName},
      {OpenCL::DemosaicNet::kConvXTransProgramName, OpenCL::DemosaicNet::kConv3x3KernelName},
      {OpenCL::DemosaicNet::kConvXTransProgramName, OpenCL::DemosaicNet::kConv1x1KernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kPackGammaKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kPackBayerNchwKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kPackXTransNchwKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName,
       OpenCL::DemosaicNet::kResidualAddCropKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName,
       OpenCL::DemosaicNet::kUnpackCropConcatKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName,
       OpenCL::DemosaicNet::kFormPostInputC6KernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kOutputRgbHwcKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kOutputGammaHwcKernelName},
      {OpenCL::DemosaicNet::kStructuralProgramName,
       OpenCL::DemosaicNet::kAssembleRgbTileKernelName},
  };

  for (const auto& item : demosaicnet_kernels) {
    cl_program program = OpenClProgramLibrary::Instance().GetProgram(item.program_name);
    ASSERT_NE(program, nullptr) << item.program_name;
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(program, item.kernel_name, &error);
    EXPECT_EQ(error, CL_SUCCESS) << item.program_name << "." << item.kernel_name;
    EXPECT_NE(kernel, nullptr) << item.program_name << "." << item.kernel_name;
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
    }
  }
}

}  // namespace
}  // namespace alcedo
