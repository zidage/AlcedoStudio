//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Support of VulkanRawOperatorsTest (plan phase VK4): the real RAW inputs of the test run, OpenCL
// buffers and images for the product OpenCL encode functions, and Vulkan buffers for the Vulkan
// encode functions. Each stage gets the same host input on both backends.

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/operators/gpu/opencl_encode.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo::vulkan_raw_test {

// ---- Real RAW inputs ------------------------------------------------------------------------

/// A Bayer RAW (Nikon NEF), an X-Trans RAW (Fujifilm RAF), and a linear DNG (RGB input) of the
/// test images.
inline constexpr const char* kBayerRaw  = "raw/airplane/_DSC1704.NEF";
inline constexpr const char* kXTransRaw = "raw/xtrans_edge/DSCF0019.RAF";
inline constexpr const char* kLinearDng = "raw/linear_dng/mfzoty.dng";

/**
 * @brief The full-size `PreparedRawInput` of @p relative under `TEST_IMG_PATH`, decoded one time
 * for the test run. Null when the file is absent.
 */
inline auto LoadRaw(const std::string& relative) -> std::shared_ptr<const PreparedRawInput> {
  static std::map<std::string, std::shared_ptr<const PreparedRawInput>> cache;
  if (const auto it = cache.find(relative); it != cache.end()) {
    return it->second;
  }
  const auto    path = std::filesystem::path(TEST_IMG_PATH) / relative;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return nullptr;
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  auto input = std::make_shared<const PreparedRawInput>(
      RawInputLoader::LoadEncoded(std::span<const std::byte>(bytes), DecodeRes::FULL));
  cache[relative] = input;
  return input;
}

/// The crop of the pack stage: the rule of `CropOrFull` of opencl_develop_pass.cpp.
inline auto CropOrFull(const PreparedRawInput& input) -> RectI {
  if (input.demosaic_output_crop.width > 0 && input.demosaic_output_crop.height > 0) {
    return input.demosaic_output_crop;
  }
  return {0, 0, static_cast<std::int32_t>(input.host_extent.width),
          static_cast<std::int32_t>(input.host_extent.height)};
}

template <class T>
auto AsBytes(const std::vector<T>& values) -> std::span<const std::byte> {
  return std::as_bytes(std::span<const T>(values));
}

// ---- OpenCL ---------------------------------------------------------------------------------

inline void CheckCl(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL test buffer: ") + operation +
                             " failed with error " + std::to_string(error));
  }
}

/// A buffer of the OpenCL context with the host bytes, or zero-filled.
class OpenClTestBuffer {
 public:
  explicit OpenClTestBuffer(std::span<const std::byte> bytes) : bytes_(bytes.size()) {
    cl_int error = CL_SUCCESS;
    mem_         = clCreateBuffer(OpenClContext::Instance().Context(),
                                  CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, bytes.size(),
                                  const_cast<std::byte*>(bytes.data()), &error);
    CheckCl(error, "clCreateBuffer");
  }
  explicit OpenClTestBuffer(std::size_t bytes) : bytes_(bytes) {
    cl_int error = CL_SUCCESS;
    mem_ = clCreateBuffer(OpenClContext::Instance().Context(), CL_MEM_READ_WRITE, bytes, nullptr,
                          &error);
    CheckCl(error, "clCreateBuffer");
    const cl_uchar zero = 0;
    CheckCl(clEnqueueFillBuffer(OpenClContext::Instance().Queue(), mem_, &zero, sizeof(zero), 0,
                                bytes, 0, nullptr, nullptr),
            "clEnqueueFillBuffer");
  }
  ~OpenClTestBuffer() { clReleaseMemObject(mem_); }
  OpenClTestBuffer(const OpenClTestBuffer&)                                  = delete;
  auto               operator=(const OpenClTestBuffer&) -> OpenClTestBuffer& = delete;

  [[nodiscard]] auto View(std::uint32_t offset_bytes = 0) const -> opencl::OpenClBufferView {
    return {mem_, offset_bytes};
  }
  /// @p count floats at @p offset_bytes; waits for the queue.
  [[nodiscard]] auto ReadFloats(std::size_t count, std::size_t offset_bytes = 0) const
      -> std::vector<float> {
    std::vector<float> values(count);
    CheckCl(clEnqueueReadBuffer(OpenClContext::Instance().Queue(), mem_, CL_TRUE, offset_bytes,
                                count * sizeof(float), values.data(), 0, nullptr, nullptr),
            "clEnqueueReadBuffer");
    return values;
  }

 private:
  cl_mem      mem_   = nullptr;
  std::size_t bytes_ = 0;
};

/// An RGBA32F image of the OpenCL context: the destination of the OpenCL pack kernels.
class OpenClTestImage {
 public:
  OpenClTestImage(std::uint32_t width, std::uint32_t height) : width_(width), height_(height) {
    const cl_image_format format{CL_RGBA, CL_FLOAT};
    cl_image_desc         desc{};
    desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    desc.image_width  = width;
    desc.image_height = height;
    cl_int error      = CL_SUCCESS;
    mem_ = clCreateImage(OpenClContext::Instance().Context(), CL_MEM_READ_WRITE, &format, &desc,
                         nullptr, &error);
    CheckCl(error, "clCreateImage");
  }
  ~OpenClTestImage() { clReleaseMemObject(mem_); }
  OpenClTestImage(const OpenClTestImage&)                                  = delete;
  auto               operator=(const OpenClTestImage&) -> OpenClTestImage& = delete;

  [[nodiscard]] auto Native() const -> cl_mem { return mem_; }
  /// The packed RGBA values; waits for the queue.
  [[nodiscard]] auto ReadFloats() const -> std::vector<float> {
    std::vector<float> values(static_cast<std::size_t>(width_) * height_ * 4);
    const std::size_t  origin[3] = {0, 0, 0};
    const std::size_t  region[3] = {width_, height_, 1};
    CheckCl(clEnqueueReadImage(OpenClContext::Instance().Queue(), mem_, CL_TRUE, origin, region, 0,
                               0, values.data(), 0, nullptr, nullptr),
            "clEnqueueReadImage");
    return values;
  }

 private:
  cl_mem        mem_    = nullptr;
  std::uint32_t width_  = 0;
  std::uint32_t height_ = 0;
};

/// The encode target of the product OpenCL functions: the queue of the context, no event list.
inline auto OpenClQueue() -> opencl::OpenClEncodeQueue {
  return {OpenClContext::Instance().Queue(), nullptr, nullptr};
}

inline void FinishOpenCl() { CheckCl(clFinish(OpenClContext::Instance().Queue()), "clFinish"); }

// ---- Vulkan ---------------------------------------------------------------------------------

/// A device buffer of @p bytes with @p bytes of host data recorded as an upload (no wait).
inline auto UploadVulkanBuffer(VulkanBackend& backend, VulkanCommandContext& command_context,
                               std::span<const std::byte> bytes, std::size_t front_bytes = 0)
    -> VulkanBackend::Buffer {
  auto buffer = backend.CreateBuffer(front_bytes + bytes.size());
  backend.UploadBufferRange(buffer, static_cast<std::uint32_t>(front_bytes), bytes,
                            command_context);
  return buffer;
}

/// @p count floats of @p buffer at @p offset_bytes: submits the recording and waits.
inline auto DownloadVulkanFloats(VulkanBackend& backend, VulkanCommandContext& command_context,
                                 const VulkanBackend::Buffer& buffer, std::size_t count,
                                 std::uint32_t offset_bytes = 0) -> std::vector<float> {
  std::vector<float> values(count);
  backend.DownloadBufferRange(buffer, offset_bytes,
                              std::as_writable_bytes(std::span<float>(values)), command_context);
  return values;
}

inline auto DownloadVulkanTexture(VulkanBackend& backend, VulkanCommandContext& command_context,
                                  const VulkanBackend::Texture2D& texture) -> std::vector<float> {
  std::vector<float> values(static_cast<std::size_t>(texture.Width()) * texture.Height() * 4);
  backend.DownloadTexture2D(texture, std::as_writable_bytes(std::span<float>(values)),
                            command_context);
  return values;
}

inline auto ViewOf(VulkanBackend::Buffer& buffer, std::uint32_t offset_bytes = 0)
    -> VulkanBufferView {
  return {&buffer, offset_bytes};
}

}  // namespace alcedo::vulkan_raw_test
