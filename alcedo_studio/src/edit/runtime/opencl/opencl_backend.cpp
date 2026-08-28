//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_backend.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_kernel_cache.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kTextureBudgetFloorBytes = 256ull << 20;

auto ImageFormatFor(TextureFormat format) -> cl_image_format {
  switch (format) {
    case TextureFormat::R8:
      return cl_image_format{CL_R, CL_UNORM_INT8};
    case TextureFormat::Rgba8:
      return cl_image_format{CL_RGBA, CL_UNORM_INT8};
    case TextureFormat::R32f:
      return cl_image_format{CL_R, CL_FLOAT};
    case TextureFormat::Rgba32f:
      return cl_image_format{CL_RGBA, CL_FLOAT};
    case TextureFormat::R16u:
      return cl_image_format{CL_R, CL_UNSIGNED_INT16};
  }
  throw std::runtime_error("OpenClBackend: unsupported texture format");
}

auto SupportsImageFormat(cl_context context, const cl_image_format& wanted) -> bool {
  cl_uint count = 0;
  cl_int  error =
      clGetSupportedImageFormats(context, CL_MEM_READ_WRITE, CL_MEM_OBJECT_IMAGE2D, 0, nullptr,
                                 &count);
  if (error != CL_SUCCESS || count == 0) {
    return false;
  }
  std::vector<cl_image_format> formats(count);
  error = clGetSupportedImageFormats(context, CL_MEM_READ_WRITE, CL_MEM_OBJECT_IMAGE2D, count,
                                     formats.data(), nullptr);
  if (error != CL_SUCCESS) {
    return false;
  }
  return std::any_of(formats.begin(), formats.end(), [&](const cl_image_format& format) {
    return format.image_channel_order == wanted.image_channel_order &&
           format.image_channel_data_type == wanted.image_channel_data_type;
  });
}

void ValidateOpenClDagCapabilities(const OpenClContext& context) {
  const auto& cap    = context.Capabilities();
  const auto  prefix = [&]() {
    std::ostringstream message;
    message << "OpenClBackend: device '" << cap.name << "'";
    return message.str();
  };

  if ((cap.device_type & CL_DEVICE_TYPE_GPU) == 0) {
    throw std::runtime_error(prefix() + " is not a GPU.");
  }
  if (!cap.available) {
    throw std::runtime_error(prefix() + " is not available.");
  }
  if (!cap.compiler_available) {
    throw std::runtime_error(prefix() + " has no OpenCL compiler.");
  }
  if (!cap.image_support) {
    throw std::runtime_error(prefix() + " lacks CL_DEVICE_IMAGE_SUPPORT.");
  }

  const TextureFormat required[] = {TextureFormat::Rgba32f, TextureFormat::R32f, TextureFormat::R8};
  for (const auto format : required) {
    if (!SupportsImageFormat(context.Context(), ImageFormatFor(format))) {
      throw std::runtime_error(prefix() + " does not support " + TextureFormatName(format) +
                               " image2d.");
    }
  }

  std::size_t max_image_width  = 0;
  std::size_t max_image_height = 0;
  CheckOpenCl(clGetDeviceInfo(context.Device(), CL_DEVICE_IMAGE2D_MAX_WIDTH, sizeof(max_image_width),
                              &max_image_width, nullptr),
              "OpenClBackend: CL_DEVICE_IMAGE2D_MAX_WIDTH");
  CheckOpenCl(clGetDeviceInfo(context.Device(), CL_DEVICE_IMAGE2D_MAX_HEIGHT,
                              sizeof(max_image_height), &max_image_height, nullptr),
              "OpenClBackend: CL_DEVICE_IMAGE2D_MAX_HEIGHT");
  if (max_image_width == 0 || max_image_height == 0) {
    throw std::runtime_error(prefix() + " reports a zero IMAGE2D max size.");
  }
  if (cap.global_memory_bytes == 0 || cap.max_single_allocation_bytes == 0) {
    throw std::runtime_error(prefix() + " reports zero global or max-allocation memory.");
  }
}

void RequirePackedTextureBytes(const OpenClBackend::Texture2D& texture,
                               std::size_t                     byte_count) {
  if (texture.Native() == nullptr) {
    throw std::runtime_error("OpenClBackend: empty texture");
  }
  if (byte_count != texture.Bytes()) {
    throw std::runtime_error("OpenClBackend: packed size does not match texture");
  }
}

auto MakeImageDesc(std::uint32_t width, std::uint32_t height) -> cl_image_desc {
  cl_image_desc desc{};
  desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width  = width;
  desc.image_height = height;
  return desc;
}

}  // namespace

OpenClCommandContext::~OpenClCommandContext() { ReleaseTrackedEvents(); }

OpenClCommandContext::OpenClCommandContext(OpenClCommandContext&& other) noexcept
    : live_events_(std::move(other.live_events_)),
      final_event_(other.final_event_),
      submission_id_(other.submission_id_) {
  other.final_event_   = nullptr;
  other.submission_id_ = 0;
}

auto OpenClCommandContext::operator=(OpenClCommandContext&& other) noexcept
    -> OpenClCommandContext& {
  if (this != &other) {
    ReleaseTrackedEvents();
    live_events_         = std::move(other.live_events_);
    final_event_         = other.final_event_;
    submission_id_       = other.submission_id_;
    other.final_event_   = nullptr;
    other.submission_id_ = 0;
  }
  return *this;
}

void OpenClCommandContext::TrackEvent(cl_event event) {
  if (event == nullptr) {
    return;
  }
  live_events_.push_back(event);
}

auto OpenClCommandContext::ReleaseTrackedEvents() noexcept -> std::size_t {
  const auto count = live_events_.size();
  for (cl_event event : live_events_) {
    if (event != nullptr) {
      clReleaseEvent(event);
    }
  }
  live_events_.clear();
  final_event_ = nullptr;
  return count;
}

OpenClBackend::Buffer::Buffer(OpenClBackend* owner, cl_mem native, void* device_pointer,
                              std::size_t bytes, std::uint64_t id)
    : owner_(owner), native_(native), ptr_(device_pointer), bytes_(bytes), resource_id_(id) {}

OpenClBackend::Buffer::~Buffer() { Reset(); }

OpenClBackend::Buffer::Buffer(Buffer&& other) noexcept
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

auto OpenClBackend::Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
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

void OpenClBackend::Buffer::Reset() noexcept {
  if (native_ != nullptr) {
    if (owner_ != nullptr) {
      owner_->UnregisterBuffer(native_);
      owner_->NoteFree();
    }
    clReleaseMemObject(native_);
    NoteOpenClReleaseMemObject();
  }
  owner_       = nullptr;
  native_      = nullptr;
  ptr_         = nullptr;
  bytes_       = 0;
  resource_id_ = 0;
}

OpenClBackend::Texture2D::Texture2D(OpenClBackend* owner, cl_mem native, std::size_t bytes,
                                    std::uint32_t width, std::uint32_t height, TextureFormat format,
                                    std::uint64_t id)
    : owner_(owner),
      native_(native),
      bytes_(bytes),
      width_(width),
      height_(height),
      format_(format),
      resource_id_(id) {}

OpenClBackend::Texture2D::~Texture2D() { Reset(); }

OpenClBackend::Texture2D::Texture2D(Texture2D&& other) noexcept
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

auto OpenClBackend::Texture2D::operator=(Texture2D&& other) noexcept -> Texture2D& {
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

void OpenClBackend::Texture2D::Reset() noexcept {
  if (native_ != nullptr) {
    if (owner_ != nullptr) {
      owner_->NoteFree();
    }
    clReleaseMemObject(native_);
    NoteOpenClReleaseMemObject();
    NoteOpenClReleaseImage();
  }
  owner_       = nullptr;
  native_      = nullptr;
  bytes_       = 0;
  width_       = 0;
  height_      = 0;
  resource_id_ = 0;
}

OpenClBackend::OpenClBackend() {
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    throw std::runtime_error("OpenClBackend: OpenCL context is not initialized.");
  }
  ValidateOpenClDagCapabilities(context);
  device_  = context.Device();
  context_ = context.Context();
  queue_   = context.ProductQueue();
  RegisterOpenClBackendPrograms();
  kernel_create_baseline_ = OpenClKernelCache::Instance().CreateCount();
  kernel_hit_baseline_    = OpenClKernelCache::Instance().HitCount();
  program_build_baseline_ = SnapshotOpenClApiCounters().program_builds;
}

OpenClBackend::~OpenClBackend() {
  dummy_lut_.Reset();
  lut_cache_.clear();
}

void OpenClBackend::UnregisterBuffer(cl_mem native) noexcept {
  live_buffers_.erase(std::remove_if(live_buffers_.begin(), live_buffers_.end(),
                                     [native](const LiveBuffer& entry) {
                                       return entry.native == native;
                                     }),
                      live_buffers_.end());
}

void OpenClBackend::TrackEnqueueEvent(CommandContext& command_context, cl_event event) {
  if (event == nullptr) {
    return;
  }
  command_context.TrackEvent(event);
  NoteEventCreate();
  NoteOpenClCreateEvent();
}

auto OpenClBackend::EnqueueMarker(CommandContext& command_context) -> cl_event {
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueMarkerWithWaitList(queue_, 0, nullptr, &event),
              "OpenClBackend::clEnqueueMarkerWithWaitList");
  TrackEnqueueEvent(command_context, event);
  command_context.final_event_ = event;
  return event;
}

auto OpenClBackend::CreateBuffer(std::size_t bytes) -> Buffer {
  if (bytes == 0) {
    return {};
  }
  cl_int error  = CL_SUCCESS;
  cl_mem native = clCreateBuffer(context_, CL_MEM_READ_WRITE, bytes, nullptr, &error);
  CheckOpenCl(error, "OpenClBackend::CreateBuffer");
  if (native == nullptr) {
    throw std::runtime_error("OpenClBackend::CreateBuffer: clCreateBuffer returned null");
  }
  NoteOpenClCreateBuffer();
  NoteBufferCreate();
  const auto address = next_virtual_address_;
  next_virtual_address_ += (bytes + 255u) & ~std::uint64_t{255};
  live_buffers_.push_back(LiveBuffer{native, address, bytes});
  return Buffer{this, native, reinterpret_cast<void*>(address), bytes, next_resource_id_++};
}

auto OpenClBackend::CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
    -> Texture2D {
  const auto bytes = static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(format);
  if (bytes == 0) {
    return {};
  }
  const auto    image_format = ImageFormatFor(format);
  cl_image_desc desc         = MakeImageDesc(width, height);
  cl_int        error        = CL_SUCCESS;
  cl_mem        native =
      clCreateImage(context_, CL_MEM_READ_WRITE, &image_format, &desc, nullptr, &error);
  CheckOpenCl(error, "OpenClBackend::CreateTexture2D");
  if (native == nullptr) {
    throw std::runtime_error("OpenClBackend::CreateTexture2D: clCreateImage returned null");
  }
  NoteOpenClCreateImage();
  NoteTextureCreate();
  return Texture2D{this, native, bytes, width, height, format, next_resource_id_++};
}

void OpenClBackend::UploadBufferRange(Buffer& buffer, std::uint32_t offset,
                                      std::span<const std::byte> bytes,
                                      CommandContext&            command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("OpenClBackend::UploadBufferRange: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + bytes.size() > buffer.Bytes()) {
    throw std::runtime_error("OpenClBackend::UploadBufferRange: range exceeds buffer");
  }
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueWriteBuffer(queue_, buffer.Native(), CL_FALSE, offset, bytes.size(),
                                   bytes.data(), 0, nullptr, &event),
              "OpenClBackend::UploadBufferRange");
  TrackEnqueueEvent(command_context, event);
  NoteOpenClH2DBytes(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_h2d_ranges_.push_back(ByteRange{offset, static_cast<std::uint32_t>(bytes.size())});
}

void OpenClBackend::DownloadBufferRange(const Buffer& buffer, std::uint32_t offset,
                                        std::span<std::byte> out, CommandContext& command_context) {
  if (out.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + out.size() > buffer.Bytes()) {
    throw std::runtime_error("OpenClBackend::DownloadBufferRange: range exceeds buffer");
  }
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueReadBuffer(queue_, buffer.Native(), CL_FALSE, offset, out.size(), out.data(),
                                  0, nullptr, &event),
              "OpenClBackend::DownloadBufferRange");
  TrackEnqueueEvent(command_context, event);
  CheckOpenCl(clWaitForEvents(1, &event), "OpenClBackend::DownloadBufferRange wait");
  NoteOpenClD2HBytes(out.size());
  ++wait_count_;
}

void OpenClBackend::UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                                    CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("OpenClBackend::UploadTexture2D: injected failure");
  }
  RequirePackedTextureBytes(texture, bytes.size());
  if (bytes.empty()) {
    return;
  }
  const std::size_t origin[3] = {0, 0, 0};
  const std::size_t region[3] = {texture.Width(), texture.Height(), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueWriteImage(queue_, texture.Native(), CL_FALSE, origin, region, 0, 0,
                                  bytes.data(), 0, nullptr, &event),
              "OpenClBackend::UploadTexture2D");
  TrackEnqueueEvent(command_context, event);
  NoteOpenClH2DBytes(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void OpenClBackend::CopyTexture2D(const Texture2D& src, Texture2D& dst,
                                  CommandContext& command_context) {
  if (src.Native() == nullptr || dst.Native() == nullptr) {
    throw std::runtime_error("OpenClBackend::CopyTexture2D: empty texture");
  }
  if (src.Width() != dst.Width() || src.Height() != dst.Height() || src.Format() != dst.Format()) {
    throw std::runtime_error("OpenClBackend::CopyTexture2D: size or format mismatch");
  }
  if (src.Bytes() == 0) {
    return;
  }
  const std::size_t origin[3] = {0, 0, 0};
  const std::size_t region[3] = {src.Width(), src.Height(), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueCopyImage(queue_, src.Native(), dst.Native(), origin, origin, region, 0,
                                 nullptr, &event),
              "OpenClBackend::CopyTexture2D");
  TrackEnqueueEvent(command_context, event);
}

void OpenClBackend::UploadR8TextureRect(Texture2D& texture, RectI rectangle,
                                        std::span<const std::byte> bytes,
                                        CommandContext&            command_context) {
  if (texture.Format() != TextureFormat::R8 || rectangle.x < 0 || rectangle.y < 0 ||
      rectangle.width <= 0 || rectangle.height <= 0 ||
      rectangle.X1() > static_cast<std::int32_t>(texture.Width()) ||
      rectangle.Y1() > static_cast<std::int32_t>(texture.Height()) ||
      bytes.size() != static_cast<std::size_t>(rectangle.width) * rectangle.height) {
    throw std::runtime_error("OpenClBackend::UploadR8TextureRect: invalid rectangle");
  }
  const std::size_t origin[3] = {static_cast<std::size_t>(rectangle.x),
                                 static_cast<std::size_t>(rectangle.y), 0};
  const std::size_t region[3] = {static_cast<std::size_t>(rectangle.width),
                                 static_cast<std::size_t>(rectangle.height), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueWriteImage(queue_, texture.Native(), CL_FALSE, origin, region,
                                  static_cast<std::size_t>(rectangle.width), 0, bytes.data(), 0,
                                  nullptr, &event),
              "OpenClBackend::UploadR8TextureRect");
  TrackEnqueueEvent(command_context, event);
  NoteOpenClH2DBytes(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_texture_rectangles_.push_back(rectangle);
}

void OpenClBackend::DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                                      CommandContext& command_context) {
  RequirePackedTextureBytes(texture, out.size());
  if (out.empty()) {
    return;
  }
  const std::size_t origin[3] = {0, 0, 0};
  const std::size_t region[3] = {texture.Width(), texture.Height(), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueReadImage(queue_, texture.Native(), CL_FALSE, origin, region, 0, 0,
                                 out.data(), 0, nullptr, &event),
              "OpenClBackend::DownloadTexture2D");
  TrackEnqueueEvent(command_context, event);
  CheckOpenCl(clWaitForEvents(1, &event), "OpenClBackend::DownloadTexture2D wait");
  NoteOpenClD2HBytes(out.size());
  ++wait_count_;
}

void OpenClBackend::CopyBufferToImage(const Buffer& src, std::uint32_t src_offset, Texture2D& dst,
                                      CommandContext& command_context) {
  if (src.Empty() || dst.Native() == nullptr) {
    throw std::runtime_error("OpenClBackend::CopyBufferToImage: empty resource");
  }
  if (static_cast<std::size_t>(src_offset) + dst.Bytes() > src.Bytes()) {
    throw std::runtime_error("OpenClBackend::CopyBufferToImage: range exceeds buffer");
  }
  const std::size_t origin[3] = {0, 0, 0};
  const std::size_t region[3] = {dst.Width(), dst.Height(), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueCopyBufferToImage(queue_, src.Native(), dst.Native(), src_offset, origin,
                                         region, 0, nullptr, &event),
              "OpenClBackend::CopyBufferToImage");
  TrackEnqueueEvent(command_context, event);
}

void OpenClBackend::CopyImageToBuffer(const Texture2D& src, Buffer& dst, std::uint32_t dst_offset,
                                      CommandContext& command_context) {
  if (src.Native() == nullptr || dst.Empty()) {
    throw std::runtime_error("OpenClBackend::CopyImageToBuffer: empty resource");
  }
  if (static_cast<std::size_t>(dst_offset) + src.Bytes() > dst.Bytes()) {
    throw std::runtime_error("OpenClBackend::CopyImageToBuffer: range exceeds buffer");
  }
  const std::size_t origin[3] = {0, 0, 0};
  const std::size_t region[3] = {src.Width(), src.Height(), 1};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueCopyImageToBuffer(queue_, src.Native(), dst.Native(), origin, region,
                                         dst_offset, 0, nullptr, &event),
              "OpenClBackend::CopyImageToBuffer");
  TrackEnqueueEvent(command_context, event);
}

auto OpenClBackend::ResolveDeviceMemory(void* device_pointer, std::size_t bytes) const
    -> std::pair<cl_mem, std::uint32_t> {
  if (device_pointer == nullptr || bytes == 0) {
    throw std::runtime_error("OpenClBackend::ResolveDeviceMemory: invalid destination");
  }
  const auto address = reinterpret_cast<std::uint64_t>(device_pointer);
  for (const auto& entry : live_buffers_) {
    if (address >= entry.gpu_address && address + bytes <= entry.gpu_address + entry.bytes) {
      return {entry.native, static_cast<std::uint32_t>(address - entry.gpu_address)};
    }
  }
  throw std::runtime_error("OpenClBackend::ResolveDeviceMemory: destination is not a live buffer");
}

void OpenClBackend::UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                                       CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("OpenClBackend::UploadDeviceMemory: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  const auto resolved = ResolveDeviceMemory(dst, bytes.size());
  cl_event   event    = nullptr;
  CheckOpenCl(clEnqueueWriteBuffer(queue_, resolved.first, CL_FALSE, resolved.second, bytes.size(),
                                   bytes.data(), 0, nullptr, &event),
              "OpenClBackend::UploadDeviceMemory");
  TrackEnqueueEvent(command_context, event);
  NoteOpenClH2DBytes(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void OpenClBackend::FillDeviceMemory(void* dst, std::size_t bytes, std::uint8_t value,
                                     CommandContext& command_context) {
  if (bytes == 0) {
    return;
  }
  const auto        resolved = ResolveDeviceMemory(dst, bytes);
  const std::uint8_t pattern  = value;
  cl_event          event    = nullptr;
  CheckOpenCl(clEnqueueFillBuffer(queue_, resolved.first, &pattern, sizeof(pattern),
                                  resolved.second, bytes, 0, nullptr, &event),
              "OpenClBackend::FillDeviceMemory");
  TrackEnqueueEvent(command_context, event);
}

void OpenClBackend::CopyDeviceMemoryToBuffer(void* src, Buffer& dst, std::uint32_t dst_offset,
                                             std::size_t bytes, CommandContext& command_context) {
  if (bytes == 0) {
    return;
  }
  if (dst.Empty() || static_cast<std::size_t>(dst_offset) + bytes > dst.Bytes()) {
    throw std::runtime_error("OpenClBackend::CopyDeviceMemoryToBuffer: range exceeds destination");
  }
  const auto resolved = ResolveDeviceMemory(src, bytes);
  cl_event   event    = nullptr;
  CheckOpenCl(clEnqueueCopyBuffer(queue_, resolved.first, dst.Native(), resolved.second, dst_offset,
                                  bytes, 0, nullptr, &event),
              "OpenClBackend::CopyDeviceMemoryToBuffer");
  TrackEnqueueEvent(command_context, event);
}

void OpenClBackend::Submit(CommandContext& command_context) {
  EnqueueMarker(command_context);
  CheckOpenCl(clFlush(queue_), "OpenClBackend::Submit clFlush");
  NoteOpenClFlush();
  ++flush_count_;
  in_flight_submission_ = command_context.SubmissionId();
}

void OpenClBackend::Wait(CommandContext& command_context) {
  const bool completing_submission = in_flight_submission_ != 0;
  if (!command_context.live_events_.empty()) {
    cl_event last = completing_submission && command_context.FinalEvent() != nullptr
                        ? command_context.FinalEvent()
                        : command_context.live_events_.back();
    CheckOpenCl(clWaitForEvents(1, &last), "OpenClBackend::Wait");
    NoteOpenClFinalWait();
    ++wait_count_;
  }
  if (completing_submission) {
    completed_submission_ = in_flight_submission_;
    in_flight_submission_ = 0;
  }
  const auto released = command_context.ReleaseTrackedEvents();
  NoteEventRelease(released);
  for (std::size_t i = 0; i < released; ++i) {
    NoteOpenClReleaseEvent();
  }
}

void OpenClBackend::SynchronizeRecordedWork(CommandContext& command_context) {
  cl_event event = EnqueueMarker(command_context);
  CheckOpenCl(clWaitForEvents(1, &event), "OpenClBackend::SynchronizeRecordedWork");
  NoteOpenClFinalWait();
  ++wait_count_;
}

void OpenClBackend::WarmUpPlan(const ExecutionPlan& plan) {
  RegisterOpenClBackendPrograms();
  auto& cache = OpenClKernelCache::Instance();
  auto  add   = [&](const char* program, const char* kernel) {
    (void)cache.GetKernel(program, kernel);
  };

  if (plan.Contains(GpuPassKind::GeometryResample)) {
    add(OpenCL::GpuDag::kGeometryCameraProgramName, OpenCL::GpuDag::kGeometryResampleKernelName);
  }
  if (plan.Contains(GpuPassKind::CameraToAp1)) {
    add(OpenCL::GpuDag::kGeometryCameraProgramName, OpenCL::GpuDag::kCameraColorKernelName);
  }
  if (plan.Contains(GpuPassKind::PrimaryColorGrade)) {
    add(OpenCL::GpuDag::kPrimaryGradeProgramName, OpenCL::GpuDag::kPrimaryGradePointwiseKernelName);
    for (const auto& stage : plan.primary_grade_stages) {
      if (stage.kind == CompiledGradeStageKind::LocalLaplacian) {
        add(OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneKernelName);
        break;
      }
    }
  }
  if (plan.Contains(GpuPassKind::MaskEvaluate) || plan.Contains(GpuPassKind::MaskFeather)) {
    add(OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskEvaluateKernelName);
  }
  if (plan.Contains(GpuPassKind::Drt)) {
    add(OpenCL::GpuDag::kDrtProgramName, OpenCL::GpuDag::kDrtKernelName);
  }
}

auto OpenClBackend::AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                               std::uint32_t edge, CommandContext& command_context)
    -> OpenClLutBinding {
  if (key.Empty() || edge <= 1 || packed_rgba.empty()) {
    return DummyLut();
  }
  for (auto& entry : lut_cache_) {
    if (entry.key == key && entry.edge_size == edge) {
      last_lut_resource_id_ = entry.buffer.ResourceId();
      return OpenClLutBinding{entry.buffer.Native(), entry.buffer.ResourceId(), entry.edge_size};
    }
  }
  if (lut_byte_budget_ > 0 && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
    while (!lut_cache_.empty() && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
      lut_cache_bytes_ -= lut_cache_.front().bytes;
      lut_cache_.erase(lut_cache_.begin());
    }
  }
  auto buffer = CreateBuffer(packed_rgba.size());
  UploadBufferRange(buffer, 0, packed_rgba, command_context);
  lut_upload_bytes_ += packed_rgba.size();
  last_lut_resource_id_ = buffer.ResourceId();
  LutCacheEntry entry;
  entry.key       = key;
  entry.edge_size = edge;
  entry.bytes     = packed_rgba.size();
  entry.buffer    = std::move(buffer);
  lut_cache_bytes_ += entry.bytes;
  lut_cache_.push_back(std::move(entry));
  return OpenClLutBinding{lut_cache_.back().buffer.Native(), last_lut_resource_id_, edge};
}

auto OpenClBackend::DummyLut() -> OpenClLutBinding {
  if (dummy_lut_.Empty()) {
    dummy_lut_ = CreateBuffer(16);
  }
  return OpenClLutBinding{dummy_lut_.Native(), dummy_lut_.ResourceId(), 0};
}

void OpenClBackend::SetLutByteBudget(std::size_t bytes) { lut_byte_budget_ = bytes; }

void OpenClBackend::ResetCounters() {
  malloc_count_           = 0;
  free_count_             = 0;
  buffer_create_count_    = 0;
  texture_create_count_   = 0;
  event_create_count_     = 0;
  event_release_count_    = 0;
  flush_count_            = 0;
  wait_count_             = 0;
  h2d_copy_count_         = 0;
  h2d_bytes_              = 0;
  lut_upload_bytes_       = 0;
  last_h2d_ranges_.clear();
  last_texture_rectangles_.clear();
  kernel_create_baseline_ = OpenClKernelCache::Instance().CreateCount();
  kernel_hit_baseline_    = OpenClKernelCache::Instance().HitCount();
  program_build_baseline_ = SnapshotOpenClApiCounters().program_builds;
}

void OpenClBackend::FailNextUpload() { fail_next_upload_ = true; }

auto OpenClBackend::DefaultTextureBudgetBytes() -> std::size_t {
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    return kTextureBudgetFloorBytes;
  }
  const auto& cap     = context.Capabilities();
  const auto  quarter = static_cast<std::size_t>(cap.global_memory_bytes / 4);
  auto        budget  = quarter > kTextureBudgetFloorBytes ? quarter : kTextureBudgetFloorBytes;
  const auto  max_alloc = static_cast<std::size_t>(cap.max_single_allocation_bytes);
  if (max_alloc > 0 && max_alloc < budget) {
    budget = max_alloc;
  }
  return budget;
}

auto OpenClBackend::WorkingSetBudgetBytes() const -> std::size_t {
  return DefaultTextureBudgetBytes();
}

auto OpenClBackend::QueryDeviceMemory() const -> GpuDeviceMemorySnapshot {
  GpuDeviceMemorySnapshot snapshot;
  auto&                   context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    return snapshot;
  }
  snapshot.total_bytes = static_cast<std::size_t>(context.Capabilities().global_memory_bytes);
  snapshot.free_bytes  = snapshot.total_bytes;
  snapshot.valid       = snapshot.total_bytes > 0;
  return snapshot;
}

auto OpenClBackend::ProgramBuildCount() const -> std::uint64_t {
  const auto builds = SnapshotOpenClApiCounters().program_builds;
  return builds >= program_build_baseline_ ? builds - program_build_baseline_ : 0;
}

auto OpenClBackend::KernelCreateCount() const -> std::uint64_t {
  const auto creates = OpenClKernelCache::Instance().CreateCount();
  return creates >= kernel_create_baseline_ ? creates - kernel_create_baseline_ : 0;
}

auto OpenClBackend::KernelHitCount() const -> std::uint64_t {
  const auto hits = OpenClKernelCache::Instance().HitCount();
  return hits >= kernel_hit_baseline_ ? hits - kernel_hit_baseline_ : 0;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
