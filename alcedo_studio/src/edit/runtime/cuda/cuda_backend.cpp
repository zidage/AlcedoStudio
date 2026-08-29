//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_backend.hpp"

#include <limits>
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
    stream_              = other.stream_;
    event_               = other.event_;
    submission_id_       = other.submission_id_;
    other.stream_        = nullptr;
    other.event_         = nullptr;
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
  const auto bytes = static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(format);
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
                                    CommandContext&            command_context) {
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
  last_h2d_ranges_.push_back(ByteRange{offset, static_cast<std::uint32_t>(bytes.size())});
}

void CudaBackend::DownloadBufferRange(const Buffer& buffer, std::uint32_t offset,
                                      std::span<std::byte> out,
                                      CommandContext&      command_context) const {
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

void CudaBackend::CopyTexture2D(const Texture2D& src, Texture2D& dst,
                                CommandContext& command_context) {
  if (src.DevicePointer() == nullptr || dst.DevicePointer() == nullptr) {
    throw std::runtime_error("CudaBackend::CopyTexture2D: empty texture");
  }
  if (src.Width() != dst.Width() || src.Height() != dst.Height() || src.Format() != dst.Format()) {
    throw std::runtime_error("CudaBackend::CopyTexture2D: size or format mismatch");
  }
  if (src.Bytes() == 0) {
    return;
  }
  cuda::CheckCuda(::cudaMemcpyAsync(dst.DevicePointer(), src.DevicePointer(), src.Bytes(),
                                    cudaMemcpyDeviceToDevice, command_context.Stream()),
                  "CudaBackend::CopyTexture2D");
}

void CudaBackend::UploadR8TextureRect(Texture2D& texture, RectI rectangle,
                                      std::span<const std::byte> bytes,
                                      CommandContext&            command_context) {
  if (texture.Format() != TextureFormat::R8 || rectangle.x < 0 || rectangle.y < 0 ||
      rectangle.width <= 0 || rectangle.height <= 0 ||
      rectangle.X1() > static_cast<std::int32_t>(texture.Width()) ||
      rectangle.Y1() > static_cast<std::int32_t>(texture.Height()) ||
      bytes.size() != static_cast<std::size_t>(rectangle.width) * rectangle.height) {
    throw std::runtime_error("CudaBackend::UploadR8TextureRect: invalid rectangle");
  }
  auto* destination = static_cast<std::byte*>(texture.DevicePointer()) +
                      static_cast<std::size_t>(rectangle.y) * texture.Width() + rectangle.x;
  cuda::CheckCuda(::cudaMemcpy2DAsync(destination, texture.Width(), bytes.data(), rectangle.width,
                                      rectangle.width, rectangle.height, cudaMemcpyHostToDevice,
                                      command_context.Stream()),
                  "CudaBackend::UploadR8TextureRect");
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_texture_rectangles_.push_back(rectangle);
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

void CudaBackend::SynchronizeRecordedWork(CommandContext& command_context) {
  cuda::CheckCuda(::cudaStreamSynchronize(command_context.Stream()),
                  "CudaBackend::SynchronizeRecordedWork");
}

auto CudaBackend::AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                             std::uint32_t edge, CommandContext& command_context)
    -> CudaLutBinding {
  if (key.Empty() || edge <= 1 || packed_rgba.empty()) {
    return DummyLut();
  }
  for (auto& entry : lut_cache_) {
    if (entry.key == key && entry.edge_size == edge) {
      entry.lru_tick             = ++lut_lru_clock_;
      entry.last_used_submission = command_context.SubmissionId();
      last_lut_resource_id_      = entry.buffer.ResourceId();
      return CudaLutBinding{entry.buffer.DevicePointer(), entry.buffer.ResourceId(),
                            entry.edge_size};
    }
  }
  if (lut_byte_budget_ > 0 && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
    while (!lut_cache_.empty() && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
      std::size_t victim_index = lut_cache_.size();
      auto        oldest       = (std::numeric_limits<std::uint64_t>::max)();
      for (std::size_t index = 0; index < lut_cache_.size(); ++index) {
        const auto& entry = lut_cache_[index];
        if (IsResourceBusy(entry.last_used_submission) || entry.lru_tick >= oldest) {
          continue;
        }
        oldest       = entry.lru_tick;
        victim_index = index;
      }
      if (victim_index == lut_cache_.size()) {
        break;
      }
      lut_cache_bytes_ -= lut_cache_[victim_index].bytes;
      lut_cache_.erase(lut_cache_.begin() + static_cast<std::ptrdiff_t>(victim_index));
    }
  }
  auto buffer = CreateBuffer(packed_rgba.size());
  UploadBufferRange(buffer, 0, packed_rgba, command_context);
  lut_upload_bytes_ += packed_rgba.size();
  last_lut_resource_id_ = buffer.ResourceId();
  LutCacheEntry entry;
  entry.key                  = key;
  entry.edge_size            = edge;
  entry.bytes                = packed_rgba.size();
  entry.lru_tick             = ++lut_lru_clock_;
  entry.last_used_submission = command_context.SubmissionId();
  entry.buffer               = std::move(buffer);
  lut_cache_bytes_ += entry.bytes;
  lut_cache_.push_back(std::move(entry));
  return CudaLutBinding{lut_cache_.back().buffer.DevicePointer(), last_lut_resource_id_, edge};
}

auto CudaBackend::DummyLut() -> CudaLutBinding {
  if (dummy_lut_.Empty()) {
    dummy_lut_ = CreateBuffer(16);
  }
  return CudaLutBinding{dummy_lut_.DevicePointer(), dummy_lut_.ResourceId(), 0};
}

void CudaBackend::SetLutByteBudget(std::size_t bytes) {
  lut_byte_budget_ = bytes;
  if (lut_byte_budget_ == 0) {
    return;
  }
  while (lut_cache_bytes_ > lut_byte_budget_ && !lut_cache_.empty()) {
    std::size_t victim_index = lut_cache_.size();
    auto        oldest       = (std::numeric_limits<std::uint64_t>::max)();
    for (std::size_t index = 0; index < lut_cache_.size(); ++index) {
      const auto& entry = lut_cache_[index];
      if (IsResourceBusy(entry.last_used_submission) || entry.lru_tick >= oldest) {
        continue;
      }
      oldest       = entry.lru_tick;
      victim_index = index;
    }
    if (victim_index == lut_cache_.size()) {
      break;
    }
    lut_cache_bytes_ -= lut_cache_[victim_index].bytes;
    lut_cache_.erase(lut_cache_.begin() + static_cast<std::ptrdiff_t>(victim_index));
  }
}

void CudaBackend::ResetCounters() {
  malloc_count_     = 0;
  free_count_       = 0;
  h2d_copy_count_   = 0;
  h2d_bytes_        = 0;
  lut_upload_bytes_ = 0;
  last_h2d_ranges_.clear();
  last_texture_rectangles_.clear();
}

void CudaBackend::FailNextUpload() { fail_next_upload_ = true; }

auto CudaBackend::QueryDeviceMemory() const -> GpuDeviceMemorySnapshot {
  GpuDeviceMemorySnapshot snapshot;
  if (::cudaMemGetInfo(&snapshot.free_bytes, &snapshot.total_bytes) == cudaSuccess) {
    snapshot.valid = true;
  }
  return snapshot;
}

auto CudaBackend::DefaultTextureBudgetBytes() -> std::size_t {
  return DefaultProductTextureBudgetBytes();
}

auto CudaBackend::MaxTransientBytes() const -> std::size_t {
  constexpr std::size_t kFloorBytes = 256ull << 20;
  const auto            memory      = QueryDeviceMemory();
  if (!memory.valid || memory.total_bytes == 0) {
    return kFloorBytes;
  }
  const auto three_quarters = memory.total_bytes - memory.total_bytes / 4;
  return three_quarters > kFloorBytes ? three_quarters : kFloorBytes;
}

auto DefaultProductTextureBudgetBytes() -> std::size_t {
  constexpr std::size_t kFloorBytes = 256ull << 20;
  std::size_t           free_bytes  = 0;
  std::size_t           total_bytes = 0;
  if (::cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess || total_bytes == 0) {
    return kFloorBytes;
  }
  const auto quarter = total_bytes / 4;
  return quarter > kFloorBytes ? quarter : kFloorBytes;
}

}  // namespace alcedo
