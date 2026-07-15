//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include "opencl/opencl_context.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <CL/cl_d3d11.h>
#include <CL/cl_gl.h>
#endif

namespace alcedo {
namespace {

struct OpenClDeviceCandidate {
  cl_platform_id           platform = nullptr;
  cl_device_id             device   = nullptr;
  OpenClDeviceCapabilities capabilities;
};

auto ToLower(std::string value) -> std::string {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

auto DeviceTypeRank(cl_device_type type) -> int {
  if ((type & CL_DEVICE_TYPE_GPU) != 0) {
    return 3;
  }
  if ((type & CL_DEVICE_TYPE_ACCELERATOR) != 0) {
    return 2;
  }
  if ((type & CL_DEVICE_TYPE_CPU) != 0) {
    return 1;
  }
  return 0;
}

template <typename T>
auto GetDeviceInfoValue(cl_device_id device, cl_device_info field) -> T {
  T      value = {};
  cl_int error = clGetDeviceInfo(device, field, sizeof(T), &value, nullptr);
  if (error != CL_SUCCESS) {
    throw std::runtime_error("[FATAL] OpenClContext: clGetDeviceInfo failed.");
  }
  return value;
}

auto GetDeviceInfoString(cl_device_id device, cl_device_info field) -> std::string {
  size_t size  = 0;
  cl_int error = clGetDeviceInfo(device, field, 0, nullptr, &size);
  if (error != CL_SUCCESS) {
    throw std::runtime_error("[FATAL] OpenClContext: failed to query string size.");
  }

  std::string value(size, '\0');
  error = clGetDeviceInfo(device, field, size, value.data(), nullptr);
  if (error != CL_SUCCESS) {
    throw std::runtime_error("[FATAL] OpenClContext: failed to query string value.");
  }
  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

auto GetMaxWorkItemSizes(cl_device_id device, cl_uint dimensions) -> std::array<size_t, 3> {
  std::array<size_t, 3> result = {};
  const auto            clamped_dimensions =
      static_cast<size_t>(std::min<cl_uint>(dimensions, static_cast<cl_uint>(result.size())));
  if (clamped_dimensions == 0) {
    return result;
  }

  std::vector<size_t> queried_sizes(static_cast<size_t>(dimensions), 0);
  cl_int              error =
      clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_ITEM_SIZES, queried_sizes.size() * sizeof(size_t),
                      queried_sizes.data(), nullptr);
  if (error != CL_SUCCESS) {
    throw std::runtime_error("[FATAL] OpenClContext: failed to query work-item sizes.");
  }

  std::copy_n(queried_sizes.begin(), clamped_dimensions, result.begin());
  return result;
}

auto QueryCapabilities(cl_device_id device) -> OpenClDeviceCapabilities {
  OpenClDeviceCapabilities capabilities;
  capabilities.name             = GetDeviceInfoString(device, CL_DEVICE_NAME);
  capabilities.vendor           = GetDeviceInfoString(device, CL_DEVICE_VENDOR);
  capabilities.driver_version   = GetDeviceInfoString(device, CL_DRIVER_VERSION);
  capabilities.device_version   = GetDeviceInfoString(device, CL_DEVICE_VERSION);
  capabilities.opencl_c_version = GetDeviceInfoString(device, CL_DEVICE_OPENCL_C_VERSION);
  capabilities.extensions       = GetDeviceInfoString(device, CL_DEVICE_EXTENSIONS);
  capabilities.device_type      = GetDeviceInfoValue<cl_device_type>(device, CL_DEVICE_TYPE);
  capabilities.compute_units    = GetDeviceInfoValue<cl_uint>(device, CL_DEVICE_MAX_COMPUTE_UNITS);
  capabilities.max_clock_frequency_mhz =
      GetDeviceInfoValue<cl_uint>(device, CL_DEVICE_MAX_CLOCK_FREQUENCY);
  capabilities.global_memory_bytes =
      GetDeviceInfoValue<cl_ulong>(device, CL_DEVICE_GLOBAL_MEM_SIZE);
  capabilities.max_single_allocation_bytes =
      GetDeviceInfoValue<cl_ulong>(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE);
  capabilities.local_memory_bytes = GetDeviceInfoValue<cl_ulong>(device, CL_DEVICE_LOCAL_MEM_SIZE);
  capabilities.max_work_group_size =
      GetDeviceInfoValue<size_t>(device, CL_DEVICE_MAX_WORK_GROUP_SIZE);
  capabilities.max_work_item_dimensions =
      GetDeviceInfoValue<cl_uint>(device, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS);
  capabilities.max_work_item_sizes =
      GetMaxWorkItemSizes(device, capabilities.max_work_item_dimensions);
  capabilities.image_support =
      GetDeviceInfoValue<cl_bool>(device, CL_DEVICE_IMAGE_SUPPORT) == CL_TRUE;
  capabilities.available = GetDeviceInfoValue<cl_bool>(device, CL_DEVICE_AVAILABLE) == CL_TRUE;
  capabilities.compiler_available =
      GetDeviceInfoValue<cl_bool>(device, CL_DEVICE_COMPILER_AVAILABLE) == CL_TRUE;
  return capabilities;
}

auto IsUsable(const OpenClDeviceCandidate& candidate) -> bool {
  return candidate.capabilities.available && candidate.capabilities.compiler_available;
}

auto IsGpu(const OpenClDeviceCandidate& candidate) -> bool {
  return (candidate.capabilities.device_type & CL_DEVICE_TYPE_GPU) != 0;
}

auto IsPreferredDevice(const OpenClDeviceCandidate& candidate, std::string_view preferred_device)
    -> bool {
  const auto haystack = ToLower(candidate.capabilities.vendor + " " + candidate.capabilities.name);
  const auto needle   = ToLower(std::string(preferred_device));
  return !needle.empty() && haystack.find(needle) != std::string::npos;
}

auto PreferCandidate(const OpenClDeviceCandidate& lhs, const OpenClDeviceCandidate& rhs) -> bool {
  const auto lhs_rank = DeviceTypeRank(lhs.capabilities.device_type);
  const auto rhs_rank = DeviceTypeRank(rhs.capabilities.device_type);
  if (lhs_rank != rhs_rank) {
    return lhs_rank > rhs_rank;
  }
  if (lhs.capabilities.global_memory_bytes != rhs.capabilities.global_memory_bytes) {
    return lhs.capabilities.global_memory_bytes > rhs.capabilities.global_memory_bytes;
  }
  return lhs.capabilities.compute_units > rhs.capabilities.compute_units;
}

auto EnumerateCandidates() -> std::vector<OpenClDeviceCandidate> {
  cl_uint platform_count = 0;
  cl_int  error          = clGetPlatformIDs(0, nullptr, &platform_count);
  if (error != CL_SUCCESS || platform_count == 0) {
    throw std::runtime_error("[FATAL] OpenClContext: no OpenCL platform is available.");
  }

  std::vector<cl_platform_id> platforms(platform_count, nullptr);
  error = clGetPlatformIDs(platform_count, platforms.data(), nullptr);
  if (error != CL_SUCCESS) {
    throw std::runtime_error("[FATAL] OpenClContext: failed to enumerate OpenCL platforms.");
  }

  std::vector<OpenClDeviceCandidate> candidates;
  for (const auto platform : platforms) {
    cl_uint device_count = 0;
    error                = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
    if (error == CL_DEVICE_NOT_FOUND || device_count == 0) {
      continue;
    }
    if (error != CL_SUCCESS) {
      throw std::runtime_error("[FATAL] OpenClContext: failed to enumerate platform devices.");
    }

    std::vector<cl_device_id> devices(device_count, nullptr);
    error = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr);
    if (error != CL_SUCCESS) {
      throw std::runtime_error("[FATAL] OpenClContext: failed to read platform devices.");
    }

    for (const auto device : devices) {
      OpenClDeviceCandidate candidate;
      candidate.platform     = platform;
      candidate.device       = device;
      candidate.capabilities = QueryCapabilities(device);
      candidates.push_back(std::move(candidate));
    }
  }

  if (candidates.empty()) {
    throw std::runtime_error("[FATAL] OpenClContext: no OpenCL device was found.");
  }
  return candidates;
}

auto DescribeCandidates(const std::vector<OpenClDeviceCandidate>& candidates) -> std::string {
  std::ostringstream stream;
  for (const auto& candidate : candidates) {
    stream << "\n  - " << candidate.capabilities.vendor << " " << candidate.capabilities.name;
    if (!IsUsable(candidate)) {
      stream << " (unusable)";
    }
  }
  return stream.str();
}

auto ResolvePreferredDevice(const OpenClInitializationOptions& options)
    -> std::optional<std::string> {
  if (options.preferred_device.has_value() && !options.preferred_device->empty()) {
    return options.preferred_device;
  }

#if defined(_WIN32)
  char*  env_override = nullptr;
  size_t env_size     = 0;
  if (_dupenv_s(&env_override, &env_size, "ALCEDO_OPENCL_DEVICE") == 0 && env_override != nullptr) {
    std::string value(env_override);
    std::free(env_override);
    if (!value.empty()) {
      return value;
    }
  }
#else
  const char* env_override = std::getenv("ALCEDO_OPENCL_DEVICE");
  if (env_override != nullptr && env_override[0] != '\0') {
    return std::string(env_override);
  }
#endif
  return std::nullopt;
}

#if defined(_WIN32)
auto GetD3D11DeviceIds(cl_platform_id platform, ID3D11Device* d3d11_device)
    -> std::vector<cl_device_id> {
  if (platform == nullptr || d3d11_device == nullptr) {
    return {};
  }

  auto get_device_ids_from_d3d11 =
      reinterpret_cast<clGetDeviceIDsFromD3D11KHR_fn>(
          clGetExtensionFunctionAddressForPlatform(platform, "clGetDeviceIDsFromD3D11KHR"));
  if (get_device_ids_from_d3d11 == nullptr) {
    return {};
  }

  cl_uint count = 0;
  cl_int  error = get_device_ids_from_d3d11(platform, CL_D3D11_DEVICE_KHR, d3d11_device,
                                            CL_PREFERRED_DEVICES_FOR_D3D11_KHR, 0, nullptr, &count);
  if (error != CL_SUCCESS || count == 0) {
    error = get_device_ids_from_d3d11(platform, CL_D3D11_DEVICE_KHR, d3d11_device,
                                      CL_ALL_DEVICES_FOR_D3D11_KHR, 0, nullptr, &count);
  }
  if (error != CL_SUCCESS || count == 0) {
    return {};
  }

  std::vector<cl_device_id> devices(count);
  error = get_device_ids_from_d3d11(platform, CL_D3D11_DEVICE_KHR, d3d11_device,
                                    CL_ALL_DEVICES_FOR_D3D11_KHR, count, devices.data(), nullptr);
  if (error != CL_SUCCESS) {
    return {};
  }
  return devices;
}

auto SupportsD3D11Sharing(const OpenClDeviceCandidate& candidate, void* d3d11_device) -> bool {
  if (d3d11_device == nullptr) {
    return true;
  }
  const auto devices =
      GetD3D11DeviceIds(candidate.platform, static_cast<ID3D11Device*>(d3d11_device));
  return std::find(devices.begin(), devices.end(), candidate.device) != devices.end();
}

auto MakeGLContextProperties(cl_platform_id platform, void* gl_context, void* gl_device_context)
    -> std::vector<cl_context_properties> {
  if (platform == nullptr || gl_context == nullptr || gl_device_context == nullptr) {
    return {};
  }
  return {
      CL_CONTEXT_PLATFORM,
      reinterpret_cast<cl_context_properties>(platform),
      CL_GL_CONTEXT_KHR,
      reinterpret_cast<cl_context_properties>(gl_context),
      CL_WGL_HDC_KHR,
      reinterpret_cast<cl_context_properties>(gl_device_context),
      0,
  };
}

auto SupportsGLSharing(const OpenClDeviceCandidate& candidate, void* gl_context,
                       void* gl_device_context) -> bool {
  if (gl_context == nullptr && gl_device_context == nullptr) {
    return true;
  }
  if (gl_context == nullptr || gl_device_context == nullptr) {
    return false;
  }

  auto get_gl_context_info =
      reinterpret_cast<clGetGLContextInfoKHR_fn>(
          clGetExtensionFunctionAddressForPlatform(candidate.platform, "clGetGLContextInfoKHR"));
  if (get_gl_context_info == nullptr) {
    return false;
  }

  const auto properties =
      MakeGLContextProperties(candidate.platform, gl_context, gl_device_context);
  if (properties.empty()) {
    return false;
  }

  size_t device_bytes = 0;
  cl_int error =
      get_gl_context_info(properties.data(), CL_DEVICES_FOR_GL_CONTEXT_KHR, 0, nullptr,
                          &device_bytes);
  if (error != CL_SUCCESS || device_bytes == 0) {
    return false;
  }

  std::vector<cl_device_id> devices(device_bytes / sizeof(cl_device_id), nullptr);
  error = get_gl_context_info(properties.data(), CL_DEVICES_FOR_GL_CONTEXT_KHR, device_bytes,
                              devices.data(), nullptr);
  if (error != CL_SUCCESS) {
    return false;
  }
  return std::find(devices.begin(), devices.end(), candidate.device) != devices.end();
}
#else
auto SupportsD3D11Sharing(const OpenClDeviceCandidate&, void* d3d11_device) -> bool {
  return d3d11_device == nullptr;
}

auto MakeGLContextProperties(cl_platform_id, void*, void*) -> std::vector<cl_context_properties> {
  return {};
}

auto SupportsGLSharing(const OpenClDeviceCandidate&, void* gl_context, void* gl_device_context)
    -> bool {
  return gl_context == nullptr && gl_device_context == nullptr;
}
#endif

auto SelectCandidate(const std::vector<OpenClDeviceCandidate>& candidates,
                      const OpenClInitializationOptions& options) -> const OpenClDeviceCandidate& {
  std::vector<const OpenClDeviceCandidate*> usable_candidates;
  std::vector<const OpenClDeviceCandidate*> usable_gpu_candidates;
  usable_candidates.reserve(candidates.size());
  usable_gpu_candidates.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    if (IsUsable(candidate) && SupportsD3D11Sharing(candidate, options.d3d11_device) &&
        SupportsGLSharing(candidate, options.gl_context, options.gl_device_context)) {
      usable_candidates.push_back(&candidate);
      if (IsGpu(candidate)) {
        usable_gpu_candidates.push_back(&candidate);
      }
    }
  }

  if (usable_candidates.empty()) {
    throw std::runtime_error(
        options.gl_context != nullptr || options.gl_device_context != nullptr
            ? "[FATAL] OpenClContext: no usable OpenCL device supports OpenGL sharing."
        : options.d3d11_device != nullptr
            ? "[FATAL] OpenClContext: no usable OpenCL device supports D3D11 sharing."
            : "[FATAL] OpenClContext: OpenCL devices exist, but none are usable.");
  }

  const auto preferred_device = ResolvePreferredDevice(options);
  if (preferred_device.has_value()) {
    std::vector<const OpenClDeviceCandidate*> preferred_candidates;
    for (const auto* candidate : usable_candidates) {
      if (IsPreferredDevice(*candidate, *preferred_device)) {
        preferred_candidates.push_back(candidate);
      }
    }
    if (preferred_candidates.empty()) {
      throw std::runtime_error("[FATAL] OpenClContext: preferred OpenCL device '" +
                               *preferred_device +
                               "' was not found among usable devices. Available devices:" +
                               DescribeCandidates(candidates));
    }
    return **std::max_element(
        preferred_candidates.begin(), preferred_candidates.end(),
        [](const auto* lhs, const auto* rhs) { return PreferCandidate(*rhs, *lhs); });
  }

  if (usable_gpu_candidates.empty()) {
    throw std::runtime_error(
        "[FATAL] OpenClContext: no usable OpenCL GPU device was found. Available devices:" +
        DescribeCandidates(candidates));
  }

  return **std::max_element(
      usable_gpu_candidates.begin(), usable_gpu_candidates.end(),
      [](const auto* lhs, const auto* rhs) { return PreferCandidate(*rhs, *lhs); });
}

}  // namespace

OpenClContext::~OpenClContext() {
  queue_override_ = nullptr;
  if (profiling_queue_ != nullptr) {
    clReleaseCommandQueue(profiling_queue_);
    profiling_queue_ = nullptr;
  }
  if (queue_ != nullptr) {
    clReleaseCommandQueue(queue_);
    queue_ = nullptr;
  }
  if (context_ != nullptr) {
    clReleaseContext(context_);
    context_ = nullptr;
  }
}

auto OpenClContext::Instance() -> OpenClContext& {
  static OpenClContext instance;
  return instance;
}

auto OpenClDeviceCapabilities::SupportsExtension(const std::string_view extension) const -> bool {
  if (extension.empty()) {
    return false;
  }
  std::size_t offset = 0;
  while ((offset = extensions.find(extension, offset)) != std::string::npos) {
    const bool begins_token = offset == 0 || extensions[offset - 1] == ' ';
    const auto  end         = offset + extension.size();
    const bool ends_token   = end == extensions.size() || extensions[end] == ' ';
    if (begins_token && ends_token) {
      return true;
    }
    offset = end;
  }
  return false;
}

void OpenClContext::Initialize(const OpenClInitializationOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    if (options.gl_context != nullptr && !gl_sharing_enabled_) {
      throw std::runtime_error(
          "[FATAL] OpenClContext: OpenCL was already initialized without OpenGL sharing.");
    }
    if (options.d3d11_device != nullptr && !d3d11_sharing_enabled_) {
      throw std::runtime_error(
          "[FATAL] OpenClContext: OpenCL was already initialized without D3D11 sharing.");
    }
    return;
  }
  initialization_attempted_                        = true;

  if (options.d3d11_device != nullptr &&
      (options.gl_context != nullptr || options.gl_device_context != nullptr)) {
    throw std::runtime_error(
        "[FATAL] OpenClContext: D3D11 sharing and OpenGL sharing are mutually exclusive.");
  }

  const auto                  candidates           = EnumerateCandidates();
  const auto&                 selected             = SelectCandidate(candidates, options);

#if defined(_WIN32)
  const cl_context_properties d3d11_context_properties[] = {
      CL_CONTEXT_PLATFORM,
      reinterpret_cast<cl_context_properties>(selected.platform),
      CL_CONTEXT_D3D11_DEVICE_KHR,
      reinterpret_cast<cl_context_properties>(options.d3d11_device),
      0};
#endif
  const auto gl_context_properties =
      MakeGLContextProperties(selected.platform, options.gl_context, options.gl_device_context);
  const cl_context_properties default_context_properties[] = {
      CL_CONTEXT_PLATFORM, reinterpret_cast<cl_context_properties>(selected.platform), 0};
  const cl_context_properties* context_properties =
#if defined(_WIN32)
      !gl_context_properties.empty()
          ? gl_context_properties.data()
          : options.d3d11_device != nullptr ? d3d11_context_properties : default_context_properties;
#else
      default_context_properties;
#endif
  cl_int error = CL_SUCCESS;
  context_     = clCreateContext(context_properties, 1, &selected.device, nullptr, nullptr, &error);
  if (error != CL_SUCCESS || context_ == nullptr) {
    throw std::runtime_error("[FATAL] OpenClContext: failed to create OpenCL context.");
  }

  queue_ = clCreateCommandQueue(context_, selected.device, 0, &error);
  if (error != CL_SUCCESS || queue_ == nullptr) {
    clReleaseContext(context_);
    context_ = nullptr;
    throw std::runtime_error("[FATAL] OpenClContext: failed to create OpenCL command queue.");
  }

  platform_     = selected.platform;
  device_       = selected.device;
  capabilities_ = selected.capabilities;
  d3d11_sharing_enabled_ =
#if defined(_WIN32)
      options.d3d11_device != nullptr;
#else
      false;
#endif
  gl_sharing_enabled_ =
#if defined(_WIN32)
      options.gl_context != nullptr && options.gl_device_context != nullptr;
#else
      false;
#endif
  initialized_  = true;
  last_initialization_error_.clear();
}

auto OpenClContext::TryInitialize(const OpenClInitializationOptions& options) -> bool {
  try {
    Initialize(options);
    return true;
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    initialization_attempted_  = true;
    last_initialization_error_ = error.what();
    return false;
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    initialization_attempted_  = true;
    last_initialization_error_ = "[FATAL] OpenClContext: unknown initialization error.";
    return false;
  }
}

auto OpenClContext::IsInitialized() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

auto OpenClContext::InitializationAttempted() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialization_attempted_;
}

auto OpenClContext::LastInitializationError() const -> std::string {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_initialization_error_;
}

auto OpenClContext::Platform() const -> cl_platform_id {
  std::lock_guard<std::mutex> lock(mutex_);
  return platform_;
}

auto OpenClContext::Device() const -> cl_device_id {
  std::lock_guard<std::mutex> lock(mutex_);
  return device_;
}

auto OpenClContext::Context() const -> cl_context {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_;
}

auto OpenClContext::Queue() const -> cl_command_queue {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_override_ != nullptr ? queue_override_ : queue_;
}

auto OpenClContext::ProductQueue() const -> cl_command_queue {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_;
}

auto OpenClContext::D3D11SharingEnabled() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return d3d11_sharing_enabled_;
}

auto OpenClContext::GLSharingEnabled() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return gl_sharing_enabled_;
}

auto OpenClContext::Capabilities() const -> const OpenClDeviceCapabilities& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    throw std::runtime_error(
        "[FATAL] OpenClContext: capabilities requested before initialization.");
  }
  return capabilities_;
}

void OpenClContext::InstallProfilingQueueOverride() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || context_ == nullptr || device_ == nullptr) {
    throw std::runtime_error(
        "[FATAL] OpenClContext: InstallProfilingQueueOverride requires an initialized context.");
  }
  if (profiling_queue_ == nullptr) {
    cl_int error = CL_SUCCESS;
    profiling_queue_ =
        clCreateCommandQueue(context_, device_, CL_QUEUE_PROFILING_ENABLE, &error);
    if (error != CL_SUCCESS || profiling_queue_ == nullptr) {
      throw std::runtime_error(
          "[FATAL] OpenClContext: failed to create profiling-enabled command queue.");
    }
  }
  queue_override_ = profiling_queue_;
}

void OpenClContext::ClearQueueOverride() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_override_ = nullptr;
}

auto OpenClContext::ProfilingQueueInstalled() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_override_ != nullptr && queue_override_ == profiling_queue_;
}

auto OpenClContext::HasProfilingQueue() const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return profiling_queue_ != nullptr;
}

}  // namespace alcedo

#endif
