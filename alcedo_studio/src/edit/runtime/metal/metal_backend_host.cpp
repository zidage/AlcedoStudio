//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <stdexcept>
#include <utility>

#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

class MetalCommandContext::Gpu {};

MetalCommandContext::MetalCommandContext()  = default;
MetalCommandContext::~MetalCommandContext() = default;
MetalCommandContext::MetalCommandContext(MetalCommandContext&& other) noexcept
    : gpu_(std::move(other.gpu_)), submission_id_(other.submission_id_) {
  other.submission_id_ = 0;
}
auto MetalCommandContext::operator=(MetalCommandContext&& other) noexcept -> MetalCommandContext& {
  if (this != &other) {
    gpu_                 = std::move(other.gpu_);
    submission_id_       = other.submission_id_;
    other.submission_id_ = 0;
  }
  return *this;
}
auto MetalCommandContext::NativeCommandBuffer() const -> void* { return nullptr; }

class MetalBackend::Gpu {};

MetalBackend::Buffer::Buffer(MetalBackend*, void*, void*, std::size_t, std::uint64_t) {}
MetalBackend::Buffer::~Buffer() { Reset(); }
MetalBackend::Buffer::Buffer(Buffer&& other) noexcept
    : owner_(other.owner_),
      native_(other.native_),
      ptr_(other.ptr_),
      bytes_(other.bytes_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.native_      = nullptr;
  other.ptr_         = nullptr;
  other.bytes_       = 0;
  other.resource_id_ = 0;
}
auto MetalBackend::Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    native_            = other.native_;
    ptr_               = other.ptr_;
    bytes_             = other.bytes_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.native_      = nullptr;
    other.ptr_         = nullptr;
    other.bytes_       = 0;
    other.resource_id_ = 0;
  }
  return *this;
}
void MetalBackend::Buffer::Reset() noexcept {
  native_      = nullptr;
  ptr_         = nullptr;
  owner_       = nullptr;
  bytes_       = 0;
  resource_id_ = 0;
}

MetalBackend::Texture2D::Texture2D(MetalBackend*, void*, std::size_t, std::uint32_t, std::uint32_t,
                                   TextureFormat, std::uint64_t) {}
MetalBackend::Texture2D::~Texture2D() { Reset(); }
MetalBackend::Texture2D::Texture2D(Texture2D&& other) noexcept
    : owner_(other.owner_),
      native_(other.native_),
      bytes_(other.bytes_),
      width_(other.width_),
      height_(other.height_),
      format_(other.format_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.native_      = nullptr;
  other.bytes_       = 0;
  other.width_       = 0;
  other.height_      = 0;
  other.resource_id_ = 0;
}
auto MetalBackend::Texture2D::operator=(Texture2D&& other) noexcept -> Texture2D& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    native_            = other.native_;
    bytes_             = other.bytes_;
    width_             = other.width_;
    height_            = other.height_;
    format_            = other.format_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.native_      = nullptr;
    other.bytes_       = 0;
    other.width_       = 0;
    other.height_      = 0;
    other.resource_id_ = 0;
  }
  return *this;
}
void MetalBackend::Texture2D::Reset() noexcept {
  native_      = nullptr;
  owner_       = nullptr;
  bytes_       = 0;
  width_       = 0;
  height_      = 0;
  resource_id_ = 0;
}

MetalBackend::MetalBackend()  = default;
MetalBackend::~MetalBackend() = default;

auto MetalBackend::CreateBuffer(std::size_t) -> Buffer {
  throw std::runtime_error("MetalBackend: GPU buffer allocation is not implemented");
}
auto MetalBackend::CreateSlab(std::size_t bytes) -> Buffer { return CreateBuffer(bytes); }
auto MetalBackend::CreateTexture2D(std::uint32_t, std::uint32_t, TextureFormat) -> Texture2D {
  throw std::runtime_error("MetalBackend: GPU texture allocation is not implemented");
}
auto MetalBackend::AcquireRecordedWorkScratchBuffer(std::size_t bytes) -> Buffer& {
  recorded_work_scratch_buffers_.push_back(CreateSlab(bytes));
  return recorded_work_scratch_buffers_.back();
}
auto MetalBackend::AcquireRecordedWorkScratchTexture(std::uint32_t width, std::uint32_t height,
                                                     TextureFormat format) -> Texture2D& {
  recorded_work_scratch_textures_.push_back(CreateTexture2D(width, height, format));
  return recorded_work_scratch_textures_.back();
}
void MetalBackend::UploadBufferRange(Buffer&, std::uint32_t, std::span<const std::byte>,
                                     CommandContext&) {
  throw std::runtime_error("MetalBackend: buffer upload is not implemented");
}
void MetalBackend::DownloadBufferRange(const Buffer&, std::uint32_t, std::span<std::byte>,
                                       CommandContext&) {
  throw std::runtime_error("MetalBackend: buffer download is not implemented");
}
void MetalBackend::UploadTexture2D(Texture2D&, std::span<const std::byte>, CommandContext&) {
  throw std::runtime_error("MetalBackend: texture upload is not implemented");
}
void MetalBackend::CopyTexture2D(const Texture2D&, Texture2D&, CommandContext&) {
  throw std::runtime_error("MetalBackend: texture copy is not implemented");
}
void MetalBackend::UploadR8TextureRect(Texture2D&, RectI, std::span<const std::byte>,
                                       CommandContext&) {
  throw std::runtime_error("MetalBackend: R8 upload is not implemented");
}
void MetalBackend::DownloadTexture2D(const Texture2D&, std::span<std::byte>, CommandContext&) {
  throw std::runtime_error("MetalBackend: texture download is not implemented");
}
void MetalBackend::UploadDeviceMemory(void*, std::span<const std::byte>, CommandContext&) {
  throw std::runtime_error("MetalBackend: device memory upload is not implemented");
}
void MetalBackend::FillDeviceMemory(void*, std::size_t, std::uint8_t, CommandContext&) {
  throw std::runtime_error("MetalBackend: device memory fill is not implemented");
}
void MetalBackend::CopyDeviceMemoryToBuffer(void*, Buffer&, std::uint32_t, std::size_t,
                                            CommandContext&) {
  throw std::runtime_error("MetalBackend: device memory copy is not implemented");
}
auto MetalBackend::ResolveDeviceMemory(void*, std::size_t) const
    -> std::pair<void*, std::uint32_t> {
  throw std::runtime_error("MetalBackend: device memory resolve is not implemented");
}
auto MetalBackend::EnsureComputeCommandEncoder(CommandContext&) -> void* {
  throw std::runtime_error("MetalBackend: compute encoder is not implemented");
}
void MetalBackend::EndCommandEncoders(CommandContext&) {}
void MetalBackend::Submit(CommandContext& command_context) {
  in_flight_submission_ = command_context.SubmissionId();
}
void MetalBackend::Wait(CommandContext&) {
  if (in_flight_submission_ == 0) {
    ReleaseRecordedWorkScratchResources();
    return;
  }
  completed_submission_ = in_flight_submission_;
  in_flight_submission_ = 0;
  ReleaseRecordedWorkScratchResources();
}
void MetalBackend::CompleteCurrentCommandBuffer(CommandContext&) {}
void MetalBackend::SynchronizeRecordedWork(CommandContext&) {
  ReleaseRecordedWorkScratchResources();
}
void MetalBackend::WarmUpPipelines(std::span<const MetalPipelineWarmup>) {
  throw std::runtime_error("MetalBackend: pipeline warm-up is not implemented");
}
void MetalBackend::WarmUpPlan(const ExecutionPlan&) {}
auto MetalBackend::AcquireLut(ContentKey, std::span<const std::byte>, std::uint32_t,
                              CommandContext&) -> MetalLutBinding {
  throw std::runtime_error("MetalBackend: LUT cache is not implemented");
}
auto MetalBackend::DummyLut() -> MetalLutBinding {
  throw std::runtime_error("MetalBackend: LUT cache is not implemented");
}
void MetalBackend::SetLutByteBudget(std::size_t) {}
void MetalBackend::ResetCounters() {
  malloc_count_                = 0;
  free_count_                  = 0;
  buffer_create_count_         = 0;
  texture_create_count_        = 0;
  heap_create_count_           = 0;
  command_buffer_create_count_ = 0;
  h2d_copy_count_              = 0;
  h2d_bytes_                   = 0;
  compute_dispatch_count_      = 0;
  lut_upload_bytes_            = 0;
  last_h2d_ranges_.clear();
  last_texture_rectangles_.clear();
}
void MetalBackend::FailNextUpload() { fail_next_upload_ = true; }
auto MetalBackend::NativeDevice() const -> void* { return nullptr; }
auto MetalBackend::NativeQueue() const -> void* { return nullptr; }
auto MetalBackend::WorkingSetBudgetBytes() const -> std::size_t {
  return DefaultTextureBudgetBytes();
}
auto MetalBackend::QueryDeviceMemory() const -> GpuDeviceMemorySnapshot { return {}; }
auto MetalBackend::PipelineCreateCount() const -> std::uint64_t { return 0; }
auto MetalBackend::PipelineHitCount() const -> std::uint64_t { return 0; }

auto BindSystemDefaultMetalPresentationDevice() -> void* { return nullptr; }
auto MetalPresentationDeviceHandle() -> void* { return nullptr; }

}  // namespace alcedo
