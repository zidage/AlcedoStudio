//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_backend.hpp"

#include <stdexcept>
#include <utility>

#include "cuda/cuda_check.hpp"

namespace alcedo {

CudaCommandContext::CudaCommandContext() {
  cuda::CheckCuda(::cudaStreamCreate(&stream_), "CudaCommandContext::cudaStreamCreate");
  cuda::CheckCuda(::cudaEventCreateWithFlags(&event_, cudaEventDisableTiming),
                  "CudaCommandContext::cudaEventCreate");
}

CudaCommandContext::~CudaCommandContext() { Destroy(); }

CudaCommandContext::CudaCommandContext(CudaCommandContext&& other) noexcept
    : stream_(other.stream_), event_(other.event_), submission_id_(other.submission_id_) {
  other.stream_        = nullptr;
  other.event_         = nullptr;
  other.submission_id_ = 0;
}

auto CudaCommandContext::operator=(CudaCommandContext&& other) noexcept -> CudaCommandContext& {
  if (this != &other) {
    Destroy();
    stream_            = other.stream_;
    event_             = other.event_;
    submission_id_     = other.submission_id_;
    other.stream_      = nullptr;
    other.event_       = nullptr;
    other.submission_id_ = 0;
  }
  return *this;
}

void CudaCommandContext::Destroy() noexcept {
  if (event_ != nullptr) {
    ::cudaEventDestroy(event_);
    event_ = nullptr;
  }
  if (stream_ != nullptr) {
    ::cudaStreamDestroy(stream_);
    stream_ = nullptr;
  }
}

CudaBackend::Buffer::Buffer(CudaBackend* owner, void* ptr, std::size_t bytes, std::uint64_t id)
    : owner_(owner), ptr_(ptr), bytes_(bytes), resource_id_(id) {}

CudaBackend::Buffer::~Buffer() { Reset(); }

CudaBackend::Buffer::Buffer(Buffer&& other) noexcept
    : owner_(other.owner_),
      ptr_(other.ptr_),
      bytes_(other.bytes_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.ptr_         = nullptr;
  other.bytes_       = 0;
  other.resource_id_ = 0;
}

auto CudaBackend::Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    ptr_               = other.ptr_;
    bytes_             = other.bytes_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.ptr_         = nullptr;
    other.bytes_       = 0;
    other.resource_id_ = 0;
  }
  return *this;
}

void CudaBackend::Buffer::Reset() noexcept {
  if (ptr_ != nullptr) {
    if (owner_ != nullptr) {
      owner_->NoteFree();
    }
    ::cudaFree(ptr_);
    ptr_ = nullptr;
  }
  owner_       = nullptr;
  bytes_       = 0;
  resource_id_ = 0;
}

CudaBackend::Texture2D::Texture2D(CudaBackend* owner, void* ptr, std::size_t bytes,
                                  std::uint32_t width, std::uint32_t height, TextureFormat format,
                                  std::uint64_t id)
    : owner_(owner),
      ptr_(ptr),
      bytes_(bytes),
      width_(width),
      height_(height),
      format_(format),
      resource_id_(id) {}

CudaBackend::Texture2D::~Texture2D() { Reset(); }

CudaBackend::Texture2D::Texture2D(Texture2D&& other) noexcept
    : owner_(other.owner_),
      ptr_(other.ptr_),
      bytes_(other.bytes_),
      width_(other.width_),
      height_(other.height_),
      format_(other.format_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.ptr_         = nullptr;
  other.bytes_       = 0;
  other.width_       = 0;
  other.height_      = 0;
  other.resource_id_ = 0;
}

auto CudaBackend::Texture2D::operator=(Texture2D&& other) noexcept -> Texture2D& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    ptr_               = other.ptr_;
    bytes_             = other.bytes_;
    width_             = other.width_;
    height_            = other.height_;
    format_            = other.format_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.ptr_         = nullptr;
    other.bytes_       = 0;
    other.width_       = 0;
    other.height_      = 0;
    other.resource_id_ = 0;
  }
  return *this;
}

void CudaBackend::Texture2D::Reset() noexcept {
  if (ptr_ != nullptr) {
    if (owner_ != nullptr) {
      owner_->NoteFree();
    }
    ::cudaFree(ptr_);
    ptr_ = nullptr;
  }
  owner_       = nullptr;
  bytes_       = 0;
  width_       = 0;
  height_      = 0;
  resource_id_ = 0;
}

auto CudaBackend::CreateBuffer(std::size_t bytes) -> Buffer {
  if (bytes == 0) {
    return {};
  }
  void* ptr = nullptr;
  cuda::CheckCuda(::cudaMalloc(&ptr, bytes), "CudaBackend::CreateBuffer");
  NoteMalloc();
  return Buffer{this, ptr, bytes, next_resource_id_++};
}

auto CudaBackend::CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
    -> Texture2D {
  const auto bytes =
      static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(format);
  if (bytes == 0) {
    return {};
  }
  void* ptr = nullptr;
  cuda::CheckCuda(::cudaMalloc(&ptr, bytes), "CudaBackend::CreateTexture2D");
  NoteMalloc();
  return Texture2D{this, ptr, bytes, width, height, format, next_resource_id_++};
}

void CudaBackend::UploadBufferRange(Buffer& buffer, std::uint32_t offset,
                                    std::span<const std::byte> bytes,
                                    CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("CudaBackend::UploadBufferRange: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + bytes.size() > buffer.Bytes()) {
    throw std::runtime_error("CudaBackend::UploadBufferRange: range exceeds buffer");
  }
  auto* dst = static_cast<std::byte*>(buffer.DevicePointer()) + offset;
  cuda::CheckCuda(::cudaMemcpyAsync(dst, bytes.data(), bytes.size(), cudaMemcpyHostToDevice,
                                    command_context.Stream()),
                  "CudaBackend::UploadBufferRange");
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_h2d_ranges_.push_back(
      ByteRange{offset, static_cast<std::uint32_t>(bytes.size())});
}

void CudaBackend::DownloadBufferRange(const Buffer& buffer, std::uint32_t offset,
                                      std::span<std::byte> out,
                                      CommandContext& command_context) const {
  if (out.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + out.size() > buffer.Bytes()) {
    throw std::runtime_error("CudaBackend::DownloadBufferRange: range exceeds buffer");
  }
  const auto* src = static_cast<const std::byte*>(buffer.DevicePointer()) + offset;
  cuda::CheckCuda(::cudaMemcpyAsync(out.data(), src, out.size(), cudaMemcpyDeviceToHost,
                                    command_context.Stream()),
                  "CudaBackend::DownloadBufferRange");
  cuda::CheckCuda(::cudaStreamSynchronize(command_context.Stream()),
                  "CudaBackend::DownloadBufferRange sync");
}

void CudaBackend::UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                                  CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("CudaBackend::UploadTexture2D: injected failure");
  }
  if (texture.DevicePointer() == nullptr) {
    throw std::runtime_error("CudaBackend::UploadTexture2D: empty texture");
  }
  if (bytes.size() != texture.Bytes()) {
    throw std::runtime_error("CudaBackend::UploadTexture2D: size does not match texture");
  }
  if (bytes.empty()) {
    return;
  }
  cuda::CheckCuda(::cudaMemcpyAsync(texture.DevicePointer(), bytes.data(), bytes.size(),
                                    cudaMemcpyHostToDevice, command_context.Stream()),
                  "CudaBackend::UploadTexture2D");
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void CudaBackend::UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                                     CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("CudaBackend::UploadDeviceMemory: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (dst == nullptr) {
    throw std::runtime_error("CudaBackend::UploadDeviceMemory: null destination");
  }
  cuda::CheckCuda(::cudaMemcpyAsync(dst, bytes.data(), bytes.size(), cudaMemcpyHostToDevice,
                                    command_context.Stream()),
                  "CudaBackend::UploadDeviceMemory");
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void CudaBackend::DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                                    CommandContext& command_context) const {
  if (texture.DevicePointer() == nullptr) {
    throw std::runtime_error("CudaBackend::DownloadTexture2D: empty texture");
  }
  if (out.size() != texture.Bytes()) {
    throw std::runtime_error("CudaBackend::DownloadTexture2D: size does not match texture");
  }
  if (out.empty()) {
    return;
  }
  cuda::CheckCuda(::cudaMemcpyAsync(out.data(), texture.DevicePointer(), out.size(),
                                    cudaMemcpyDeviceToHost, command_context.Stream()),
                  "CudaBackend::DownloadTexture2D");
  cuda::CheckCuda(::cudaStreamSynchronize(command_context.Stream()),
                  "CudaBackend::DownloadTexture2D sync");
}

void CudaBackend::GenerateMaskMipLevels(Texture2D&) {}

void CudaBackend::Submit(CommandContext& command_context) {
  cuda::CheckCuda(::cudaEventRecord(command_context.Event(), command_context.Stream()),
                  "CudaBackend::Submit");
  in_flight_submission_ = command_context.SubmissionId();
}

void CudaBackend::Wait(CommandContext& command_context) {
  if (in_flight_submission_ == 0) {
    return;
  }
  cuda::CheckCuda(::cudaEventSynchronize(command_context.Event()), "CudaBackend::Wait");
  completed_submission_ = in_flight_submission_;
  in_flight_submission_ = 0;
}

void CudaBackend::ResetCounters() {
  malloc_count_   = 0;
  free_count_     = 0;
  h2d_copy_count_ = 0;
  h2d_bytes_      = 0;
  last_h2d_ranges_.clear();
}

void CudaBackend::FailNextUpload() { fail_next_upload_ = true; }

}  // namespace alcedo
