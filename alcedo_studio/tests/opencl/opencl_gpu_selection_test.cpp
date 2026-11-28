// Copyright 2026 Alcedo Studio contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "opencl/opencl_context.hpp"

namespace {
// This executable supplies a CPU-only ICD through the public OpenCL entry points.
// Selection must fail before the production context owner creates a context.
bool           platform_present       = true;
cl_device_type device_type            = CL_DEVICE_TYPE_CPU;
int            context_creation_count = 0;
int            platform_handle        = 0;
int            device_handle          = 0;
}  // namespace

extern "C" {
CL_API_ENTRY cl_int CL_API_CALL clGetPlatformIDs(cl_uint, cl_platform_id* platforms,
                                                 cl_uint* count) {
  if (count) *count = platform_present ? 1 : 0;
  if (platforms && platform_present) {
    platforms[0] = reinterpret_cast<cl_platform_id>(&platform_handle);
  }
  return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL clGetDeviceIDs(cl_platform_id, cl_device_type, cl_uint,
                                               cl_device_id* devices, cl_uint* count) {
  if (count) *count = 1;
  if (devices) devices[0] = reinterpret_cast<cl_device_id>(&device_handle);
  return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL clGetDeviceInfo(cl_device_id, cl_device_info field, size_t size,
                                                void* value, size_t* actual_size) {
  if (field == CL_DEVICE_NAME || field == CL_DEVICE_VENDOR || field == CL_DRIVER_VERSION ||
      field == CL_DEVICE_VERSION || field == CL_DEVICE_OPENCL_C_VERSION ||
      field == CL_DEVICE_EXTENSIONS) {
    const char text[] = "test-cpu";
    if (actual_size) *actual_size = sizeof(text);
    if (value) std::memcpy(value, text, std::min(size, sizeof(text)));
    return CL_SUCCESS;
  }
  if (value) {
    std::memset(value, 0, size);
    if (field == CL_DEVICE_TYPE) {
      std::memcpy(value, &device_type, sizeof(device_type));
    } else if (field == CL_DEVICE_AVAILABLE || field == CL_DEVICE_COMPILER_AVAILABLE ||
               field == CL_DEVICE_IMAGE_SUPPORT) {
      const cl_bool available = CL_TRUE;
      std::memcpy(value, &available, sizeof(available));
    }
  }
  return CL_SUCCESS;
}

CL_API_ENTRY cl_context CL_API_CALL
clCreateContext(const cl_context_properties*, cl_uint, const cl_device_id*,
                void(CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int* error) {
  ++context_creation_count;
  if (error) *error = CL_DEVICE_NOT_AVAILABLE;
  return nullptr;
}
}

namespace alcedo {
namespace {
TEST(OpenClGpuSelectionTest, CpuOnlyIcdReportsErrorWithoutCreatingAContext) {
  platform_present = true;
  device_type      = CL_DEVICE_TYPE_CPU;
  auto& context    = OpenClContext::Instance();
  EXPECT_FALSE(context.TryInitialize());
  EXPECT_FALSE(context.IsInitialized());
  EXPECT_NE(context.LastInitializationError().find("no usable GPU"), std::string::npos);
  EXPECT_EQ(context_creation_count, 0);
}

TEST(OpenClGpuSelectionTest, PreferredCpuDeviceCannotEnableCpuProcessing) {
  platform_present = true;
  device_type      = CL_DEVICE_TYPE_CPU;
  OpenClInitializationOptions options;
  options.preferred_device = "test-cpu";
  auto& context            = OpenClContext::Instance();
  EXPECT_FALSE(context.TryInitialize(options));
  EXPECT_FALSE(context.IsInitialized());
  EXPECT_NE(context.LastInitializationError().find("no usable GPU"), std::string::npos);
  EXPECT_EQ(context_creation_count, 0);
}

TEST(OpenClGpuSelectionTest, MissingIcdReportsErrorWithoutCreatingAContext) {
  platform_present = false;
  auto& context    = OpenClContext::Instance();
  EXPECT_FALSE(context.TryInitialize());
  EXPECT_FALSE(context.IsInitialized());
  EXPECT_NE(context.LastInitializationError().find("no OpenCL platform"), std::string::npos);
  EXPECT_EQ(context_creation_count, 0);
}
}  // namespace
}  // namespace alcedo
