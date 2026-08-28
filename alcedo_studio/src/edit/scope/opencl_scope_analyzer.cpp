//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "edit/scope/opencl_scope_programs.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace {

constexpr int    kScopeSlotCount    = 3;
constexpr size_t kRowAlignmentBytes = 256;
constexpr size_t kRgba32fPixelBytes = sizeof(float) * 4U;
constexpr size_t kRgba32uPixelBytes = sizeof(uint32_t) * 4U;

inline auto      HasScopeEnabled(const ScopeRequest& request, ScopeType type) -> bool {
  return (request.enabled_mask & static_cast<uint32_t>(type)) != 0U;
}

inline auto ClampPositive(int value, int fallback) -> int { return value > 0 ? value : fallback; }

auto        AlignBytes(size_t bytes) -> size_t {
  return ((bytes + kRowAlignmentBytes - 1U) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

void CheckOpenCl(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL scope analyzer: ") + operation +
                             " failed with error " + std::to_string(error) + ".");
  }
}

auto CreateOpenClBuffer(size_t byte_size) -> cl_mem {
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }

  cl_int error  = CL_SUCCESS;
  cl_mem buffer = clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, byte_size, nullptr, &error);
  CheckOpenCl(error, "clCreateBuffer");
  if (buffer == nullptr) {
    throw std::runtime_error("OpenCL scope analyzer: clCreateBuffer returned null.");
  }
  return buffer;
}

struct ScopeSlot {
  std::shared_ptr<scope::opencl_detail::OpenClLinearImageResource> input_image     = {};
  std::shared_ptr<scope::opencl_detail::OpenClImageResource>       input_image_2d  = {};
  std::shared_ptr<scope::opencl_detail::OpenClEventSignalResource> input_ready     = {};
  std::shared_ptr<scope::opencl_detail::OpenClEventSignalResource> completion      = {};
  std::shared_ptr<scope::opencl_detail::OpenClBufferResource>      histogram       = {};
  std::shared_ptr<scope::opencl_detail::OpenClLinearImageResource> waveform        = {};
  int                                                              input_width     = 0;
  int                                                              input_height    = 0;
  int                                                              histogram_bins  = 0;
  int                                                              waveform_width  = 0;
  int                                                              waveform_height = 0;
  uint64_t                                                         generation      = 0;
  uint64_t                                                         image_identity  = 0;
  uint64_t                                                         session_epoch = 0;
  uint64_t                                                         display_generation = 0;

  void                                                             ResetResources() {
    input_image.reset();
    input_image_2d.reset();
    input_ready.reset();
    completion.reset();
    histogram.reset();
    waveform.reset();
    input_width     = 0;
    input_height    = 0;
    histogram_bins  = 0;
    waveform_width  = 0;
    waveform_height = 0;
    generation      = 0;
    image_identity  = 0;
    session_epoch = 0;
    display_generation = 0;
  }
};

class OpenClScopeAnalyzerImpl final : public IScopeAnalyzer {
 public:
  ~OpenClScopeAnalyzerImpl() override { ReleaseResources(); }

  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) override {
    const bool histogram_enabled = HasScopeEnabled(request, ScopeType::Histogram);
    const bool waveform_enabled  = HasScopeEnabled(request, ScopeType::Waveform);
    if (!frame || frame.image.backend != GpuBackend::OpenCL || request.enabled_mask == 0U ||
        (!histogram_enabled && !waveform_enabled)) {
      return;
    }

    const auto now        = std::chrono::steady_clock::now();
    const int  target_fps = std::max(0, request.target_fps);
    if (target_fps > 0 && last_submit_time_.time_since_epoch().count() != 0) {
      const auto min_interval = std::chrono::milliseconds(1000 / target_fps);
      if ((now - last_submit_time_) < min_interval) {
        return;
      }
    }

    const bool use_image_input = frame.image.resource_type == FrameWriteTargetType::OpenClImage;
    auto input_image = std::shared_ptr<scope::opencl_detail::OpenClLinearImageResource>(
        frame.image.resource,
        static_cast<scope::opencl_detail::OpenClLinearImageResource*>(frame.image.resource.get()));
    auto input_image_2d = std::shared_ptr<scope::opencl_detail::OpenClImageResource>(
        frame.image.resource,
        static_cast<scope::opencl_detail::OpenClImageResource*>(frame.image.resource.get()));
    if (frame.format != FramePixelFormat::RGBA32F || frame.image.format != FramePixelFormat::RGBA32F) {
      return;
    }
    if (use_image_input) {
      if (!input_image_2d || input_image_2d->image == nullptr) {
        return;
      }
    } else if (!input_image || input_image->buffer == nullptr) {
      return;
    }

    auto input_ready = std::shared_ptr<scope::opencl_detail::OpenClEventSignalResource>(
        frame.ready_signal.resource,
        static_cast<scope::opencl_detail::OpenClEventSignalResource*>(
            frame.ready_signal.resource.get()));
    if (use_image_input &&
        (frame.ready_signal.backend != GpuBackend::OpenCL || !input_ready ||
         input_ready->event == nullptr)) {
      return;
    }

    const int source_width  = use_image_input ? input_image_2d->width : input_image->width;
    const int source_height = use_image_input ? input_image_2d->height : input_image->height;
    const int frame_width   = ClampPositive(frame.width, source_width);
    const int frame_height  = ClampPositive(frame.height, source_height);
    if (frame_width <= 0 || frame_height <= 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScopeSlot*                  slot = AcquireAvailableSlot();
    if (slot == nullptr) {
      return;
    }

    EnsureKernels();
    EnsureSlotStorage(*slot, frame_width, frame_height, request, use_image_input);
    slot->input_image_2d = use_image_input ? std::move(input_image_2d) : nullptr;
    slot->input_ready    = std::move(input_ready);
    slot->image_identity     = frame.image_identity;
    slot->session_epoch   = frame.session_epoch;
    slot->display_generation = frame.display_generation;

    auto&        context   = OpenClContext::Instance();
    const cl_uint wait_count =
        slot->input_ready && slot->input_ready->event != nullptr ? 1U : 0U;
    const cl_event* wait_list = wait_count == 0U ? nullptr : &slot->input_ready->event;
    if (!use_image_input) {
      const size_t src_bytes = static_cast<size_t>(frame_height) * input_image->row_bytes;
      const size_t dst_bytes = static_cast<size_t>(slot->input_height) * slot->input_image->row_bytes;
      CheckOpenCl(clEnqueueCopyBuffer(context.ProductQueue(), input_image->buffer,
                                      slot->input_image->buffer, 0, 0,
                                      std::min(src_bytes, dst_bytes), wait_count, wait_list, nullptr),
                  "clEnqueueCopyBuffer(input)");
    }

    const uint32_t zero = 0U;
    if (slot->histogram && histogram_enabled) {
      CheckOpenCl(clEnqueueFillBuffer(context.ProductQueue(), slot->histogram->buffer, &zero,
                                      sizeof(zero), 0, slot->histogram->size_bytes, wait_count,
                                      wait_list, nullptr),
                  "clEnqueueFillBuffer(histogram)");
    }
    if (slot->waveform && waveform_enabled) {
      CheckOpenCl(clEnqueueFillBuffer(
                      context.ProductQueue(), slot->waveform->buffer, &zero, sizeof(zero), 0,
                      static_cast<size_t>(slot->waveform_height) * slot->waveform->row_bytes,
                      wait_count, wait_list, nullptr),
                  "clEnqueueFillBuffer(waveform)");
    }

    const int    sample_step    = std::max(1, request.analysis_downsample);
    const size_t global_size[2] = {
        static_cast<size_t>((frame_width + sample_step - 1) / sample_step),
        static_cast<size_t>((frame_height + sample_step - 1) / sample_step)};

    if (slot->histogram && histogram_enabled) {
      EnqueueHistogramKernel(*slot, frame_width, frame_height, sample_step, global_size);
    }

    if (slot->waveform && waveform_enabled) {
      EnqueueWaveformKernel(*slot, frame_width, frame_height, sample_step, global_size);
    }

    cl_event completion_event = nullptr;
    CheckOpenCl(clEnqueueMarkerWithWaitList(context.ProductQueue(), 0, nullptr, &completion_event),
                "clEnqueueMarkerWithWaitList(scope completion)");
    slot->completion = std::make_shared<scope::opencl_detail::OpenClEventSignalResource>();
    slot->completion->event = completion_event;
    slot->generation  = next_generation_++;
    last_submit_time_ = now;
  }

  auto GetLatestOutput() -> ScopeOutputSet override {
    std::lock_guard<std::mutex> lock(mutex_);

    ScopeSlot*                  latest_slot = nullptr;
    for (auto& slot : slots_) {
      if (slot.generation == 0) {
        continue;
      }
      if (!latest_slot || slot.generation > latest_slot->generation) {
        latest_slot = &slot;
      }
    }

    if (latest_slot == nullptr) {
      return {};
    }

    ScopeOutputSet output;
    output.generation      = latest_slot->generation;
    output.histogram_bins  = latest_slot->histogram_bins;
    output.waveform_width  = latest_slot->waveform_width;
    output.waveform_height = latest_slot->waveform_height;
    output.image_identity     = latest_slot->image_identity;
    output.session_epoch   = latest_slot->session_epoch;
    output.display_generation = latest_slot->display_generation;

    if (latest_slot->histogram) {
      output.histogram_buffer.backend = GpuBackend::OpenCL;
      output.histogram_buffer.resource =
          std::shared_ptr<void>(latest_slot->histogram, latest_slot->histogram.get());
      output.histogram_buffer.size_bytes = latest_slot->histogram->size_bytes;
      output.histogram_valid             = true;
    }

    if (latest_slot->waveform) {
      output.waveform_image.backend = GpuBackend::OpenCL;
      output.waveform_image.resource =
          std::shared_ptr<void>(latest_slot->waveform, latest_slot->waveform.get());
      output.waveform_image.width     = latest_slot->waveform_width;
      output.waveform_image.height    = latest_slot->waveform_height;
      output.waveform_image.row_bytes = latest_slot->waveform->row_bytes;
      output.waveform_image.format    = FramePixelFormat::RGBA32F;
      output.waveform_valid           = true;
    }

    return output;
  }

  void ResizeResources(const ScopeRequest&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (OpenClContext::Instance().IsInitialized()) {
      clFinish(OpenClContext::Instance().ProductQueue());
    }
    for (auto& slot : slots_) {
      slot.ResetResources();
    }
  }

  void ReleaseResources() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (OpenClContext::Instance().IsInitialized()) {
      clFinish(OpenClContext::Instance().ProductQueue());
    }
    for (auto& slot : slots_) {
      slot.ResetResources();
    }
    if (histogram_kernel_ != nullptr) {
      clReleaseKernel(histogram_kernel_);
      histogram_kernel_ = nullptr;
    }
    if (waveform_kernel_ != nullptr) {
      clReleaseKernel(waveform_kernel_);
      waveform_kernel_ = nullptr;
    }
    if (histogram_image_kernel_ != nullptr) {
      clReleaseKernel(histogram_image_kernel_);
      histogram_image_kernel_ = nullptr;
    }
    if (waveform_image_kernel_ != nullptr) {
      clReleaseKernel(waveform_image_kernel_);
      waveform_image_kernel_ = nullptr;
    }
  }

 private:
  auto CompletionFinished(ScopeSlot& slot) -> bool {
    if (!slot.completion || slot.completion->event == nullptr) {
      return true;
    }
    cl_int status = CL_QUEUED;
    if (clGetEventInfo(slot.completion->event, CL_EVENT_COMMAND_EXECUTION_STATUS,
                       sizeof(status), &status, nullptr) != CL_SUCCESS) {
      slot.completion.reset();
      return true;
    }
    if (status > CL_COMPLETE) {
      return false;
    }
    slot.completion.reset();
    return true;
  }

  auto AcquireAvailableSlot() -> ScopeSlot* {
    for (auto& slot : slots_) {
      if (slot.generation == 0 || CompletionFinished(slot)) {
        return &slot;
      }
    }
    return nullptr;
  }

  void EnsureKernels() {
    if (histogram_kernel_ != nullptr && waveform_kernel_ != nullptr &&
        histogram_image_kernel_ != nullptr && waveform_image_kernel_ != nullptr) {
      return;
    }

    cl_program program =
        OpenClProgramLibrary::Instance().GetProgram(OpenCL::Scope::kScopeProgramName);
    if (program == nullptr) {
      throw std::runtime_error("OpenCL scope analyzer: scope program is unavailable.");
    }

    if (histogram_kernel_ == nullptr) {
      cl_int error      = CL_SUCCESS;
      histogram_kernel_ = clCreateKernel(program, OpenCL::Scope::kHistogramKernelName, &error);
      CheckOpenCl(error, "clCreateKernel(histogram)");
    }
    if (waveform_kernel_ == nullptr) {
      cl_int error     = CL_SUCCESS;
      waveform_kernel_ = clCreateKernel(program, OpenCL::Scope::kWaveformKernelName, &error);
      CheckOpenCl(error, "clCreateKernel(waveform)");
    }
    if (histogram_image_kernel_ == nullptr) {
      cl_int error            = CL_SUCCESS;
      histogram_image_kernel_ =
          clCreateKernel(program, OpenCL::Scope::kHistogramImageKernelName, &error);
      CheckOpenCl(error, "clCreateKernel(histogram image)");
    }
    if (waveform_image_kernel_ == nullptr) {
      cl_int error           = CL_SUCCESS;
      waveform_image_kernel_ =
          clCreateKernel(program, OpenCL::Scope::kWaveformImageKernelName, &error);
      CheckOpenCl(error, "clCreateKernel(waveform image)");
    }
  }

  void EnsureSlotStorage(ScopeSlot& slot, int frame_width, int frame_height,
                         const ScopeRequest& request, bool image_input) {
    const int    histogram_bins  = ClampPositive(request.histogram_bins, 256);
    const int    waveform_width  = ClampPositive(request.waveform_width, 384);
    const int    waveform_height = ClampPositive(request.waveform_height, 192);

    const size_t input_row_bytes = static_cast<size_t>(frame_width) * kRgba32fPixelBytes;
    if (image_input) {
      slot.input_image.reset();
    } else if (!slot.input_image || slot.input_width != frame_width ||
               slot.input_height != frame_height || slot.input_image->row_bytes != input_row_bytes) {
      slot.input_image = std::make_shared<scope::opencl_detail::OpenClLinearImageResource>();
      slot.input_image->buffer =
          CreateOpenClBuffer(static_cast<size_t>(frame_height) * input_row_bytes);
      slot.input_image->row_bytes   = input_row_bytes;
      slot.input_image->width       = frame_width;
      slot.input_image->height      = frame_height;
      slot.input_image->format      = FramePixelFormat::RGBA32F;
      slot.input_image->owns_memory = true;
    }
    slot.input_width  = frame_width;
    slot.input_height = frame_height;

    if (HasScopeEnabled(request, ScopeType::Histogram)) {
      const size_t histogram_bytes = static_cast<size_t>(histogram_bins) * 3U * sizeof(uint32_t);
      if (!slot.histogram || slot.histogram_bins != histogram_bins ||
          slot.histogram->size_bytes != histogram_bytes) {
        slot.histogram             = std::make_shared<scope::opencl_detail::OpenClBufferResource>();
        slot.histogram->buffer     = CreateOpenClBuffer(histogram_bytes);
        slot.histogram->size_bytes = histogram_bytes;
        slot.histogram->owns_memory = true;
        slot.histogram_bins         = histogram_bins;
      }
    } else {
      slot.histogram.reset();
      slot.histogram_bins = 0;
    }

    if (HasScopeEnabled(request, ScopeType::Waveform)) {
      const size_t waveform_row_bytes =
          AlignBytes(static_cast<size_t>(waveform_width) * kRgba32uPixelBytes);
      if (!slot.waveform || slot.waveform_width != waveform_width ||
          slot.waveform_height != waveform_height ||
          slot.waveform->row_bytes != waveform_row_bytes) {
        slot.waveform = std::make_shared<scope::opencl_detail::OpenClLinearImageResource>();
        slot.waveform->buffer =
            CreateOpenClBuffer(static_cast<size_t>(waveform_height) * waveform_row_bytes);
        slot.waveform->row_bytes   = waveform_row_bytes;
        slot.waveform->width       = waveform_width;
        slot.waveform->height      = waveform_height;
        slot.waveform->format      = FramePixelFormat::RGBA32F;
        slot.waveform->owns_memory = true;
        slot.waveform_width        = waveform_width;
        slot.waveform_height       = waveform_height;
      }
    } else {
      slot.waveform.reset();
      slot.waveform_width  = 0;
      slot.waveform_height = 0;
    }
  }

  void EnqueueHistogramKernel(const ScopeSlot& slot, int frame_width, int frame_height,
                              int sample_step, const size_t global_size[2]) {
    if (slot.input_image_2d) {
      EnqueueHistogramImageKernel(slot, frame_width, frame_height, sample_step, global_size);
      return;
    }
    cl_int  error        = CL_SUCCESS;
    cl_uint arg_index    = 0;
    cl_mem  input_buffer = slot.input_image->buffer;
    cl_uint input_pitch_pixels =
        static_cast<cl_uint>(slot.input_image->row_bytes / kRgba32fPixelBytes);
    cl_mem histogram_buffer = slot.histogram->buffer;

    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_mem), &input_buffer);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_uint), &input_pitch_pixels);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_int), &frame_width);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_int), &frame_height);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_int), &sample_step);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_int), &slot.histogram_bins);
    error |= clSetKernelArg(histogram_kernel_, arg_index++, sizeof(cl_mem), &histogram_buffer);
    CheckOpenCl(error, "clSetKernelArg(histogram)");

    const cl_uint wait_count =
        slot.input_ready && slot.input_ready->event != nullptr ? 1U : 0U;
    const cl_event* wait_list = wait_count == 0U ? nullptr : &slot.input_ready->event;
    CheckOpenCl(clEnqueueNDRangeKernel(OpenClContext::Instance().ProductQueue(), histogram_kernel_,
                                       2, nullptr, global_size, nullptr, wait_count, wait_list,
                                       nullptr),
                "clEnqueueNDRangeKernel(histogram)");
  }

  void EnqueueHistogramImageKernel(const ScopeSlot& slot, int frame_width, int frame_height,
                                   int sample_step, const size_t global_size[2]) {
    cl_int error          = CL_SUCCESS;
    cl_uint arg_index     = 0;
    cl_mem  input_image   = slot.input_image_2d->image;
    cl_mem  histogram     = slot.histogram->buffer;
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_mem), &input_image);
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_int), &frame_width);
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_int), &frame_height);
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_int), &sample_step);
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_int),
                            &slot.histogram_bins);
    error |= clSetKernelArg(histogram_image_kernel_, arg_index++, sizeof(cl_mem), &histogram);
    CheckOpenCl(error, "clSetKernelArg(histogram image)");

    const cl_uint wait_count =
        slot.input_ready && slot.input_ready->event != nullptr ? 1U : 0U;
    const cl_event* wait_list = wait_count == 0U ? nullptr : &slot.input_ready->event;
    CheckOpenCl(clEnqueueNDRangeKernel(OpenClContext::Instance().ProductQueue(),
                                       histogram_image_kernel_, 2, nullptr, global_size, nullptr,
                                       wait_count, wait_list, nullptr),
                "clEnqueueNDRangeKernel(histogram image)");
  }

  void EnqueueWaveformKernel(const ScopeSlot& slot, int frame_width, int frame_height,
                             int sample_step, const size_t global_size[2]) {
    if (slot.input_image_2d) {
      EnqueueWaveformImageKernel(slot, frame_width, frame_height, sample_step, global_size);
      return;
    }
    cl_int  error        = CL_SUCCESS;
    cl_uint arg_index    = 0;
    cl_mem  input_buffer = slot.input_image->buffer;
    cl_uint input_pitch_pixels =
        static_cast<cl_uint>(slot.input_image->row_bytes / kRgba32fPixelBytes);
    cl_mem  waveform_buffer = slot.waveform->buffer;
    cl_uint waveform_pitch_pixels =
        static_cast<cl_uint>(slot.waveform->row_bytes / kRgba32uPixelBytes);

    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_mem), &input_buffer);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_uint), &input_pitch_pixels);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_int), &frame_width);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_int), &frame_height);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_int), &sample_step);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_mem), &waveform_buffer);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_uint), &waveform_pitch_pixels);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_int), &slot.waveform_width);
    error |= clSetKernelArg(waveform_kernel_, arg_index++, sizeof(cl_int), &slot.waveform_height);
    CheckOpenCl(error, "clSetKernelArg(waveform)");

    const cl_uint wait_count =
        slot.input_ready && slot.input_ready->event != nullptr ? 1U : 0U;
    const cl_event* wait_list = wait_count == 0U ? nullptr : &slot.input_ready->event;
    CheckOpenCl(clEnqueueNDRangeKernel(OpenClContext::Instance().ProductQueue(), waveform_kernel_,
                                       2, nullptr, global_size, nullptr, wait_count, wait_list,
                                       nullptr),
                "clEnqueueNDRangeKernel(waveform)");
  }

  void EnqueueWaveformImageKernel(const ScopeSlot& slot, int frame_width, int frame_height,
                                  int sample_step, const size_t global_size[2]) {
    cl_int  error                  = CL_SUCCESS;
    cl_uint arg_index              = 0;
    cl_mem  input_image            = slot.input_image_2d->image;
    cl_mem  waveform               = slot.waveform->buffer;
    const cl_uint waveform_pitch_pixels =
        static_cast<cl_uint>(slot.waveform->row_bytes / kRgba32uPixelBytes);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_mem), &input_image);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_int), &frame_width);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_int), &frame_height);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_int), &sample_step);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_mem), &waveform);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_uint),
                            &waveform_pitch_pixels);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_int),
                            &slot.waveform_width);
    error |= clSetKernelArg(waveform_image_kernel_, arg_index++, sizeof(cl_int),
                            &slot.waveform_height);
    CheckOpenCl(error, "clSetKernelArg(waveform image)");

    const cl_uint wait_count =
        slot.input_ready && slot.input_ready->event != nullptr ? 1U : 0U;
    const cl_event* wait_list = wait_count == 0U ? nullptr : &slot.input_ready->event;
    CheckOpenCl(clEnqueueNDRangeKernel(OpenClContext::Instance().ProductQueue(),
                                       waveform_image_kernel_, 2, nullptr, global_size, nullptr,
                                       wait_count, wait_list, nullptr),
                "clEnqueueNDRangeKernel(waveform image)");
  }

  std::array<ScopeSlot, kScopeSlotCount> slots_{};
  cl_kernel                              histogram_kernel_ = nullptr;
  cl_kernel                              waveform_kernel_  = nullptr;
  cl_kernel                              histogram_image_kernel_ = nullptr;
  cl_kernel                              waveform_image_kernel_  = nullptr;
  std::chrono::steady_clock::time_point  last_submit_time_{};
  uint64_t                               next_generation_ = 1;
  std::mutex                             mutex_{};
};

}  // namespace

auto CreateOpenClScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  return std::make_shared<OpenClScopeAnalyzerImpl>();
}

}  // namespace alcedo

#endif
