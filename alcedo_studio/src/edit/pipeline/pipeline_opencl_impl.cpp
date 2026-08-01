//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/operators/GPU_kernels/opencl_param.hpp"
#include "edit/operators/basic/highlight_shadow_local_tone_opencl.hpp"
#include "edit/pipeline/opencl_kernel_dispatch.hpp"
#include "edit/pipeline/opencl_pipeline_programs.hpp"
#include "edit/pipeline/pipeline_gpu_wrapper.hpp"
#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

constexpr uint32_t kOpenClNeighborMaxTapCount = 64;

enum class OpenClNeighborOpKind : uint32_t {
  Sharpen = 1,
  Clarity = 2,
  Halation = 3,
  FilmGrain = 4,
};

struct OpenClNeighborStageParams {
  uint32_t                                      kind_      = 0;
  uint32_t                                      radius_    = 0;
  uint32_t                                      tap_count_ = 0;
  float                                         amount_    = 0.0f;
  float                                         threshold_ = 0.0f;
  std::array<float, kOpenClNeighborMaxTapCount> weights_   = {};
  uint32_t                                      enabled_   = 0;
  int32_t                                       eotf_      = 0;
  uint32_t                                      seed_lo_   = 0;
  uint32_t                                      seed_hi_   = 0;
  float                                         sigma_x_   = 0.0f;
  float                                         sigma_y_   = 0.0f;
  float                                         redshift_[3] = {};
  float                                         reserved_    = 0.0f;
  uint32_t                                      roi_enabled_ = 0;
  int32_t                                       roi_x_       = 0;
  int32_t                                       roi_y_       = 0;
  float                                         roi_scale_x_ = 1.0f;
  float                                         roi_scale_y_ = 1.0f;
  int32_t                                       roi_reference_width_  = 0;
  int32_t                                       roi_reference_height_ = 0;
  uint32_t                                      reserved_tail_         = 0;
};

struct OpenClNeighborStage {
  OpenClNeighborStageParams params_ = {};
};

static_assert(sizeof(OpenClNeighborStageParams) == 348);
static_assert(alignof(OpenClNeighborStageParams) == alignof(float));

auto ResolveViewerDisplayConfig(const OperatorParams& params) -> ViewerDisplayConfig {
  return ViewerDisplayConfig{params.to_output_params_.encoding_space_,
                             params.to_output_params_.eotf_,
                             params.to_output_params_.peak_luminance_};
}

auto BuildGaussianWeights(float sigma, uint32_t radius)
    -> std::array<float, kOpenClNeighborMaxTapCount> {
  std::array<float, kOpenClNeighborMaxTapCount> weights{};
  const double safe_sigma  = std::max(static_cast<double>(sigma), 1.0e-4);
  const double inv2sigma2  = 0.5 / (safe_sigma * safe_sigma);
  double       full_weight = 1.0;

  weights[0]               = 1.0f;
  for (uint32_t tap = 1; tap <= radius; ++tap) {
    const double w = std::exp(-(static_cast<double>(tap) * static_cast<double>(tap)) * inv2sigma2);
    weights[tap]   = static_cast<float>(w);
    full_weight += 2.0 * w;
  }

  if (full_weight > 0.0) {
    for (uint32_t tap = 0; tap <= radius; ++tap) {
      weights[tap] = static_cast<float>(static_cast<double>(weights[tap]) / full_weight);
    }
  }

  return weights;
}

auto BuildNeighborStageParams(OpenClNeighborOpKind kind, float sigma, float amount, float threshold,
                              int gaussian_tap_count, const float* gaussian_weights)
    -> OpenClNeighborStageParams {
  OpenClNeighborStageParams params;

  params.kind_      = static_cast<uint32_t>(kind);
  params.amount_    = amount;
  params.threshold_ = threshold;
  params.enabled_   = 1U;

  const int clamped_tap_count =
      std::clamp(gaussian_tap_count, 0, static_cast<int>(kOpenClNeighborMaxTapCount));
  if (clamped_tap_count > 0 && gaussian_weights != nullptr) {
    params.tap_count_ = static_cast<uint32_t>(clamped_tap_count);
    params.radius_    = params.tap_count_ - 1U;
    std::copy_n(gaussian_weights, clamped_tap_count, params.weights_.begin());
    return params;
  }

  if (sigma <= 0.0f) {
    return params;
  }

  const float    safe_sigma = std::max(sigma, 1.0e-4f);
  const uint32_t max_radius =
      (kOpenClNeighborMaxTapCount > 0U) ? (kOpenClNeighborMaxTapCount - 1U) : 0U;
  params.radius_ =
      std::clamp<uint32_t>(static_cast<uint32_t>(std::ceil(3.0f * safe_sigma)), 1U, max_radius);
  params.tap_count_ = params.radius_ + 1U;
  params.weights_   = BuildGaussianWeights(safe_sigma, params.radius_);
  return params;
}

auto BuildHalationStageParams(const OperatorParams::HalationParams& halation, float scale_x,
                              float scale_y, GPU_EOTF eotf) -> OpenClNeighborStageParams {
  OpenClNeighborStageParams params;
  params.kind_      = static_cast<uint32_t>(OpenClNeighborOpKind::Halation);
  params.enabled_   = halation.enabled_ ? 1U : 0U;
  params.eotf_      = static_cast<int32_t>(eotf);
  params.amount_    = std::clamp(halation.strength_, 0.0f, 2.0f);
  params.sigma_x_   = halation.sigma_ * std::clamp(scale_x, 1.0e-4f, 1.0f);
  params.sigma_y_   = halation.sigma_ * std::clamp(scale_y, 1.0e-4f, 1.0f);
  std::copy_n(halation.redshift_, 3, params.redshift_);
  return params;
}

auto BuildFilmGrainStageParams(const FusedOperatorParams& fused_params)
    -> OpenClNeighborStageParams {
  const auto& grain = fused_params.film_grain_;
  OpenClNeighborStageParams params;
  params.kind_                  = static_cast<uint32_t>(OpenClNeighborOpKind::FilmGrain);
  params.enabled_               = grain.enabled_ ? 1U : 0U;
  params.amount_                = std::clamp(grain.strength_, 0.0f, 1.0f);
  params.seed_lo_               = static_cast<uint32_t>(grain.seed_ & 0xffffffffULL);
  params.seed_hi_               = static_cast<uint32_t>((grain.seed_ >> 32U) & 0xffffffffULL);
  params.roi_enabled_           = fused_params.render_roi_enabled_ ? 1U : 0U;
  params.roi_x_                 = fused_params.render_roi_x_;
  params.roi_y_                 = fused_params.render_roi_y_;
  params.roi_scale_x_           = fused_params.render_roi_scale_x_;
  params.roi_scale_y_           = fused_params.render_roi_scale_y_;
  params.roi_reference_width_   = fused_params.render_roi_reference_width_;
  params.roi_reference_height_  = fused_params.render_roi_reference_height_;
  return params;
}

auto UploadStageParams(const OpenClNeighborStageParams& params) -> OpenCL::Pipeline::OpenClBuffer {
  return OpenCL::Pipeline::OpenClBuffer::CreateReadOnlyCopy(&params,
                                                            sizeof(OpenClNeighborStageParams));
}

void CheckOpenClFrameCopy(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL fused pipeline: ") + operation +
                             " failed with error " + std::to_string(error) + ".");
  }
}

auto TrySubmitOpenClFrameToSink(opencl::OpenClImage& image, IFrameSink& frame_sink,
                                const FrameCompletionSubmission& submission) -> bool {
  frame_sink.EnsureSize(image.Width(), image.Height());
  const FrameWriteMapping mapping = frame_sink.MapResourceForWrite(FrameMemoryDomain::OpenClDevice);
  if (!mapping) {
    // DirectFrameSink already logs the concrete handshake / domain reason.
    // host_upload after this is a no-op on the production QML path.
    std::cerr << "[OpenCL Pipeline] direct present mapping failed " << image.Width() << "x"
              << image.Height()
              << " (present will report host_upload; production sink has no host path)\n";
    return false;
  }

  const auto unmap = [&frame_sink]() { frame_sink.UnmapResource(); };
  if (mapping.pixel_format != FramePixelFormat::RGBA32F ||
      mapping.memory_domain != FrameMemoryDomain::OpenClDevice ||
      mapping.target_type != FrameWriteTargetType::OpenClImage || mapping.data == nullptr) {
    std::cerr << "[OpenCL Pipeline] direct present mapping has wrong type "
              << "domain=" << static_cast<int>(mapping.memory_domain)
              << " target=" << static_cast<int>(mapping.target_type) << "\n";
    unmap();
    return false;
  }

  auto&        context   = OpenClContext::Instance();
  const size_t origin[3] = {0, 0, 0};
  const size_t region[3] = {static_cast<size_t>(image.Width()), static_cast<size_t>(image.Height()),
                            1};
  cl_mem       target_image = static_cast<cl_mem>(mapping.data);
  const cl_int copy_error   = clEnqueueCopyBufferToImage(
      context.Queue(), image.Buffer(), target_image, 0, origin, region, 0, nullptr, nullptr);
  if (copy_error != CL_SUCCESS) {
    unmap();
    CheckOpenClFrameCopy(copy_error, "clEnqueueCopyBufferToImage");
  }

  frame_sink.UnmapResource();
  CheckOpenClFrameCopy(clFinish(context.Queue()), "clFinish after OpenGL frame copy");
  frame_sink.NotifyFrameReady(submission);
  return true;
}

auto MakeOpenClScopeImageResource(const opencl::OpenClImage& image)
    -> std::shared_ptr<scope::opencl_detail::OpenClLinearImageResource> {
  if (image.Empty() || image.Type() != CV_32FC4 || image.Buffer() == nullptr) {
    return {};
  }

  CheckOpenClFrameCopy(clRetainMemObject(image.Buffer()), "clRetainMemObject(scope frame)");
  auto resource           = std::make_shared<scope::opencl_detail::OpenClLinearImageResource>();
  resource->buffer        = image.Buffer();
  resource->row_bytes     = image.RowBytes();
  resource->width         = image.Width();
  resource->height        = image.Height();
  resource->format        = FramePixelFormat::RGBA32F;
  resource->owns_memory   = true;
  resource->native_object = reinterpret_cast<std::uintptr_t>(image.Buffer());
  return resource;
}

void SubmitOpenClFrameForScope(const opencl::OpenClImage& image, IFrameSink& frame_sink,
                               const ViewerDisplayConfig& display_config) {
  auto final_image = MakeOpenClScopeImageResource(image);
  if (!final_image) {
    return;
  }

  frame_sink.SubmitFinalDisplayFrame(FinalDisplayFrameView{
      SharedGpuImageHandle{GpuBackend::OpenCL,
                           std::shared_ptr<void>(final_image, final_image.get()), image.Width(),
                           image.Height(), image.RowBytes(), FramePixelFormat::RGBA32F},
      image.Width(),
      image.Height(),
      FramePixelFormat::RGBA32F,
      display_config,
      AnalysisDomain::DisplayEncoded,
      {},
      0});
}

class OpenCLGPUPipeline final : public GPUPipelineImpl,
                                private opencl_detail::OpenClKernelStage<OpenCLGPUPipeline> {
 public:
  static constexpr const char* kStageLabel = "OpenCL fused pipeline";

 private:
  std::shared_ptr<ImageBuffer>           input_img_;
  OperatorParams*                        cpu_params_   = nullptr;
  IFrameSink*                            frame_sink_   = nullptr;
  FrameCompletionSubmission              bound_frame_submission_{};
  FusedOperatorParams                    fused_params_ = {};
  OpenCL::Pipeline::OpenClFusedResources resources_    = {};

  opencl_detail::OpenClKernelHandle      fused_kernel_{OpenCL::Pipeline::kFusedProgramName,
                                                  OpenCL::Pipeline::kFusedKernelName};
  opencl_detail::OpenClKernelHandle      fused_stage_kernel_{OpenCL::Pipeline::kFusedProgramName,
                                                        OpenCL::Pipeline::kFusedStageKernelName};
  opencl_detail::OpenClKernelHandle      validate_kernel_{
      OpenCL::Pipeline::kFusedProgramName, OpenCL::Pipeline::kValidateFusedParamsKernelName};
  opencl_detail::OpenClKernelHandle blur_h_kernel_{
      OpenCL::Pipeline::kDetailProgramName, OpenCL::Pipeline::kNeighborBlurHorizontalKernelName};
  opencl_detail::OpenClKernelHandle apply_v_kernel_{
      OpenCL::Pipeline::kDetailProgramName, OpenCL::Pipeline::kNeighborApplyVerticalKernelName};
  highlight_shadow_local_tone::OpenClStage hs_stage_;

  opencl::OpenClImage                      working_;
  opencl::OpenClImage                      pre_hs_working_;
  opencl::OpenClImage                      hs_working_;
  opencl::OpenClImage                      blur_horizontal_;
  opencl::OpenClImage                      detail_scratch_;

  void                                     EnsureOpenClInput() {
    if (!input_img_) {
      throw std::runtime_error("OpenCL fused pipeline: input image is null.");
    }

    const bool has_valid_gpu = input_img_->gpu_data_valid_;
    const bool has_valid_cpu = input_img_->cpu_data_valid_;

    if (!has_valid_gpu && !has_valid_cpu) {
      throw std::runtime_error("OpenCL fused pipeline: input image has no valid CPU or GPU data.");
    }

    const bool needs_sync = !has_valid_gpu || input_img_->GetGPUBackend() != GpuBackendKind::OpenCL;

    if (needs_sync) {
      if (has_valid_gpu && input_img_->GetGPUBackend() != GpuBackendKind::OpenCL) {
        input_img_->SyncToCPU();
      }
      input_img_->SyncToGPU(GpuBackendKind::OpenCL);
    }

    if (input_img_->GetGPUType() != CV_32FC4) {
      input_img_->ConvertGPUDataTo(CV_32FC4);
    }
  }

  void EnsureFusedKernels() {
    fused_kernel_.Ensure(kStageLabel);
    fused_stage_kernel_.Ensure(kStageLabel);
    validate_kernel_.Ensure(kStageLabel);
  }

  void EnsureDetailKernels() {
    blur_h_kernel_.Ensure(kStageLabel);
    apply_v_kernel_.Ensure(kStageLabel);
    hs_stage_.EnsureKernels();
  }

  void ValidateParamsABI() {
    auto& context = OpenClContext::Instance();
    if (!context.IsInitialized()) {
      throw std::runtime_error("OpenCL fused pipeline: OpenCL context is not initialized.");
    }

    cl_kernel kernel = validate_kernel_.Get();
    if (kernel == nullptr) {
      throw std::runtime_error("OpenCL fused pipeline: validation kernel is null.");
    }

    cl_int err = CL_SUCCESS;
    cl_mem output_buffer =
        clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, 12 * sizeof(float), nullptr, &err);
    if (err != CL_SUCCESS || output_buffer == nullptr) {
      throw std::runtime_error("OpenCL fused pipeline: failed to create validation output buffer.");
    }

    cl_mem params_buf = resources_.params_buffer_.Get();
    try {
      SetKernelArgs(kernel, "validation", params_buf, output_buffer);
    } catch (...) {
      clReleaseMemObject(output_buffer);
      throw;
    }

    size_t global_size = 1;
    err = clEnqueueNDRangeKernel(context.Queue(), kernel, 1, nullptr, &global_size, nullptr, 0,
                                 nullptr, nullptr);
    if (err != CL_SUCCESS) {
      clReleaseMemObject(output_buffer);
      throw std::runtime_error("OpenCL fused pipeline: failed to enqueue validation kernel.");
    }

    std::array<float, 12> result{};
    err = clEnqueueReadBuffer(context.Queue(), output_buffer, CL_TRUE, 0,
                              result.size() * sizeof(float), result.data(), 0, nullptr, nullptr);
    clReleaseMemObject(output_buffer);

    if (err != CL_SUCCESS) {
      throw std::runtime_error("OpenCL fused pipeline: failed to read validation output.");
    }

    clFinish(context.Queue());

    const auto nearly_equal = [](float lhs, float rhs) {
      return std::abs(lhs - rhs) <= 1.0e-5f * std::max(1.0f, std::abs(rhs));
    };
    const auto& params                 = resources_.opencl_params_;
    const float expected_output_header = static_cast<float>(params.to_output_params_.method_) +
                                         params.to_output_params_.display_linear_scale_;
    const float expected_open_drt_header = params.to_output_params_.limit_to_display_matx[0] +
                                           params.to_output_params_.open_drt_params_.tn_con_;
    if (!nearly_equal(result[4], expected_output_header) ||
        !nearly_equal(result[5], expected_open_drt_header) ||
        !nearly_equal(result[6], params.to_output_params_.aces_params_.ts_.forward_limit_) ||
        !nearly_equal(result[7], params.to_output_params_.aces_params_.limit_J_max) ||
        !nearly_equal(result[8], params.to_output_params_.open_drt_params_.ts_s_) ||
        !nearly_equal(result[9], params.to_output_params_.open_drt_params_.ts_m2_) ||
        !nearly_equal(result[10], params.to_output_params_.aces_params_.ts_.m_2_) ||
        !nearly_equal(result[11], params.to_output_params_.aces_params_.ts_.g_)) {
      throw std::runtime_error("OpenCL fused pipeline: fused params ABI validation failed.");
    }
  }

  void EnqueueFusedKernel(const opencl::OpenClImage& src) {
    auto& context = OpenClContext::Instance();
    if (!context.IsInitialized()) {
      throw std::runtime_error("OpenCL fused pipeline: context is not initialized.");
    }

    working_.Create(src.Width(), src.Height(), src.Type());

    cl_int             err               = CL_SUCCESS;

    cl_mem             src_buffer        = src.Buffer();
    cl_mem             dst_buffer        = working_.Buffer();
    cl_mem             params_buffer     = resources_.params_buffer_.Get();
    cl_mem             lmt_lut_buffer    = resources_.lmt_lut_buffer_.Get();
    cl_int             width             = src.Width();
    cl_int             height            = src.Height();

    static const float kDummyLutEntry[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cl_mem             fallback_lut      = nullptr;
    if (lmt_lut_buffer == nullptr) {
      fallback_lut =
          clCreateBuffer(context.Context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                         sizeof(kDummyLutEntry), const_cast<float*>(kDummyLutEntry), &err);
      if (err != CL_SUCCESS || fallback_lut == nullptr) {
        throw std::runtime_error("OpenCL fused pipeline: failed to create fallback LUT buffer.");
      }
      lmt_lut_buffer = fallback_lut;
    }

    try {
      SetKernelArgs(fused_kernel_.Get(), "fused kernel", src_buffer, dst_buffer, params_buffer,
                    lmt_lut_buffer, width, height);
    } catch (...) {
      if (fallback_lut != nullptr) clReleaseMemObject(fallback_lut);
      throw;
    }

    EnqueueKernel2D(fused_kernel_.Get(), width, height, "fused kernel");
    if (fallback_lut != nullptr) {
      clReleaseMemObject(fallback_lut);
    }
  }

  void EnqueueFusedStageKernel(const opencl::OpenClImage& src, opencl::OpenClImage& dst,
                               int stage) {
    auto& context = OpenClContext::Instance();
    if (!context.IsInitialized()) {
      throw std::runtime_error("OpenCL fused pipeline: context is not initialized.");
    }

    dst.Create(src.Width(), src.Height(), src.Type());

    cl_int             err               = CL_SUCCESS;
    cl_mem             src_buffer        = src.Buffer();
    cl_mem             dst_buffer        = dst.Buffer();
    cl_mem             params_buffer     = resources_.params_buffer_.Get();
    cl_mem             lmt_lut_buffer    = resources_.lmt_lut_buffer_.Get();
    cl_int             width             = src.Width();
    cl_int             height            = src.Height();
    cl_int             stage_arg         = stage;

    static const float kDummyLutEntry[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cl_mem             fallback_lut      = nullptr;
    if (lmt_lut_buffer == nullptr) {
      fallback_lut =
          clCreateBuffer(context.Context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                         sizeof(kDummyLutEntry), const_cast<float*>(kDummyLutEntry), &err);
      if (err != CL_SUCCESS || fallback_lut == nullptr) {
        throw std::runtime_error("OpenCL fused pipeline: failed to create fallback LUT buffer.");
      }
      lmt_lut_buffer = fallback_lut;
    }

    try {
      SetKernelArgs(fused_stage_kernel_.Get(), "fused stage", src_buffer, dst_buffer, params_buffer,
                    lmt_lut_buffer, width, height, stage_arg);
    } catch (...) {
      if (fallback_lut != nullptr) clReleaseMemObject(fallback_lut);
      throw;
    }

    EnqueueKernel2D(fused_stage_kernel_.Get(), width, height, "fused stage");
    if (fallback_lut != nullptr) {
      clReleaseMemObject(fallback_lut);
    }
  }

  void EnqueueNeighborBlurHorizontal(const opencl::OpenClImage& src, opencl::OpenClImage& dst,
                                     cl_mem stage_buffer) {
    dst.Create(src.Width(), src.Height(), src.Type());

    cl_mem src_buf = src.Buffer();
    cl_mem dst_buf = dst.Buffer();
    cl_int width   = src.Width();
    cl_int height  = src.Height();

    SetKernelArgs(blur_h_kernel_.Get(), "neighbor blur horizontal", src_buf, dst_buf, stage_buffer,
                  width, height);

    EnqueueKernel2D(blur_h_kernel_.Get(), width, height, "neighbor blur horizontal kernel");
  }

  void EnqueueNeighborApplyVertical(const opencl::OpenClImage& src,
                                    const opencl::OpenClImage& blur_horizontal,
                                    opencl::OpenClImage& dst, cl_mem stage_buffer) {
    dst.Create(src.Width(), src.Height(), src.Type());

    cl_mem src_buf  = src.Buffer();
    cl_mem blur_buf = blur_horizontal.Buffer();
    cl_mem dst_buf  = dst.Buffer();
    cl_int width    = src.Width();
    cl_int height   = src.Height();

    SetKernelArgs(apply_v_kernel_.Get(), "neighbor apply vertical", src_buf, blur_buf, dst_buf,
                  stage_buffer, width, height);

    EnqueueKernel2D(apply_v_kernel_.Get(), width, height, "neighbor apply vertical kernel");
  }

  auto ShouldRunSharpen() const -> bool {
    return fused_params_.sharpen_enabled_ && fused_params_.sharpen_offset_ != 0.0f &&
           fused_params_.sharpen_radius_ > 0.0f;
  }

  auto ShouldRunClarity() const -> bool {
    return fused_params_.clarity_enabled_ && fused_params_.clarity_offset_ != 0.0f &&
           fused_params_.clarity_radius_ > 0.0f;
  }

  auto ShouldRunHalation() const -> bool {
    const auto& halation = fused_params_.halation_;
    return halation.enabled_ && std::clamp(halation.strength_, 0.0f, 2.0f) > 0.0f;
  }

  auto ShouldRunFilmGrain() const -> bool {
    const auto& grain = fused_params_.film_grain_;
    return grain.enabled_ && std::clamp(grain.strength_, 0.0f, 1.0f) > 0.0f;
  }

  auto BuildNeighborStages() const -> std::vector<OpenClNeighborStage> {
    std::vector<OpenClNeighborStage> stages;
    stages.reserve(4);

    if (ShouldRunSharpen()) {
      stages.push_back(OpenClNeighborStage{BuildNeighborStageParams(
          OpenClNeighborOpKind::Sharpen, fused_params_.sharpen_radius_,
          fused_params_.sharpen_offset_, fused_params_.sharpen_threshold_,
          fused_params_.sharpen_gaussian_tap_count_, fused_params_.sharpen_gaussian_weights_)});
    }
    if (ShouldRunClarity()) {
      stages.push_back(OpenClNeighborStage{BuildNeighborStageParams(
          OpenClNeighborOpKind::Clarity, fused_params_.clarity_radius_,
          fused_params_.clarity_offset_, 0.0f, fused_params_.clarity_gaussian_tap_count_,
          fused_params_.clarity_gaussian_weights_)});
    }
    if (ShouldRunHalation()) {
      const auto effective_eotf =
          static_cast<GPU_EOTF>(resources_.opencl_params_.to_output_params_.eotf_);
      stages.push_back(OpenClNeighborStage{BuildHalationStageParams(
          fused_params_.halation_, fused_params_.render_output_scale_x_,
          fused_params_.render_output_scale_y_, effective_eotf)});
    }
    if (ShouldRunFilmGrain()) {
      stages.push_back(OpenClNeighborStage{BuildFilmGrainStageParams(fused_params_)});
    }

    return stages;
  }

 public:
  void SetInputImage(std::shared_ptr<ImageBuffer> input_image) override {
    input_img_ = std::move(input_image);
  }

  void SetParams(OperatorParams& params) override {
    cpu_params_   = &params;
    fused_params_ = FusedParamsConverter::ConvertFromCPU(params, fused_params_);
    resources_ =
        OpenCL::Pipeline::OpenClFusedParamUploader::Upload(fused_params_, params, resources_);
  }

  void SetFrameSink(IFrameSink* frame_sink) override { frame_sink_ = frame_sink; }

  void SetBoundFrameSubmission(const FrameCompletionSubmission& submission) override {
    bound_frame_submission_ = submission;
  }

  void Execute(std::shared_ptr<ImageBuffer> output_img) override {
    using ProfileClock    = std::chrono::steady_clock;
    const auto exec_start = ProfileClock::now();

    if (!cpu_params_) {
      throw std::runtime_error("OpenCL fused pipeline: parameters were not set.");
    }

    double ensure_input_ms     = 0.0;
    double ensure_kernels_ms   = 0.0;
    double validate_abi_ms     = 0.0;
    double fused_kernel_ms     = 0.0;
    double detail_ms           = 0.0;
    double sync_ms             = 0.0;
    double download_ms         = 0.0;
    double submit_ms           = 0.0;
    bool   submitted_gpu_frame = false;

    {
      const auto t0 = ProfileClock::now();
      EnsureOpenClInput();
      ensure_input_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    {
      const auto t0 = ProfileClock::now();
      EnsureFusedKernels();
      ValidateParamsABI();
      validate_abi_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    const auto neighbor_stages   = BuildNeighborStages();
    const bool run_hs_local_tone = hs_stage_.ShouldRun(fused_params_);

    {
      const auto t0 = ProfileClock::now();
      if (run_hs_local_tone || !neighbor_stages.empty()) {
        EnsureDetailKernels();
      }
      ensure_kernels_ms =
          std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    const auto& input = input_img_->GetOpenClImage();

    {
      const auto t0 = ProfileClock::now();
      if (run_hs_local_tone) {
        EnqueueFusedStageKernel(input, pre_hs_working_, 1);
        hs_stage_.Execute(fused_params_, resources_.params_buffer_.Get(), pre_hs_working_,
                          hs_working_);
        EnqueueFusedStageKernel(hs_working_, working_, 2);
      } else {
        EnqueueFusedKernel(input);
      }
      fused_kernel_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    opencl::OpenClImage*                        detail_src = &working_;
    opencl::OpenClImage*                        detail_dst = &detail_scratch_;

    std::vector<OpenCL::Pipeline::OpenClBuffer> stage_buffers;
    stage_buffers.reserve(neighbor_stages.size());

    {
      const auto t0 = ProfileClock::now();
      for (const auto& stage : neighbor_stages) {
        stage_buffers.push_back(UploadStageParams(stage.params_));
        cl_mem stage_buffer = stage_buffers.back().Get();

        EnqueueNeighborBlurHorizontal(*detail_src, blur_horizontal_, stage_buffer);
        EnqueueNeighborApplyVertical(*detail_src, blur_horizontal_, *detail_dst, stage_buffer);

        std::swap(detail_src, detail_dst);
      }
      detail_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    {
      const auto t0      = ProfileClock::now();
      auto&      context = OpenClContext::Instance();
      clFinish(context.Queue());
      sync_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
    }

    if (frame_sink_) {
      const ViewerDisplayConfig display_config = ResolveViewerDisplayConfig(*cpu_params_);
      submitted_gpu_frame = TrySubmitOpenClFrameToSink(*detail_src, *frame_sink_, bound_frame_submission_);
      if (!submitted_gpu_frame) {
        cv::Mat host_image;
        {
          const auto t0 = ProfileClock::now();
          detail_src->Download(host_image);
          download_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
        }

        if (host_image.type() != CV_32FC4) {
          throw std::runtime_error(
              "OpenCL fused pipeline: expected RGBA32F host frame for viewer.");
        }

        const size_t row_bytes =
            static_cast<size_t>(host_image.cols) * static_cast<size_t>(sizeof(cv::Vec4f));
        auto host_pixels = std::make_shared<std::vector<float>>(
            static_cast<size_t>(host_image.cols) * static_cast<size_t>(host_image.rows) * 4U);
        cv::Mat contiguous_host(host_image.rows, host_image.cols, CV_32FC4, host_pixels->data(),
                                row_bytes);
        host_image.copyTo(contiguous_host);

        const auto t0 = ProfileClock::now();
        frame_sink_->SubmitHostFrame(
            ViewerFrame{host_image.cols, host_image.rows, row_bytes,
                        std::shared_ptr<const void>(host_pixels, host_pixels->data()),
                        display_config, FramePresentationMode::FullFrame});
        submit_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - t0).count();
      } else {
        submit_ms = 0.0;
      }
      SubmitOpenClFrameForScope(*detail_src, *frame_sink_, display_config);
    }

    if (output_img) {
      *output_img = ImageBuffer(std::move(*detail_src));
    }

    const double total_ms =
        std::chrono::duration<double, std::milli>(ProfileClock::now() - exec_start).count();

    static int           frame_count  = 0;
    static constexpr int kLogInterval = 30;
    if (++frame_count % kLogInterval == 1) {
      std::cout << "[OpenCL Pipeline] frame=" << frame_count << " total=" << std::fixed
                << std::setprecision(2) << total_ms << " ms"
                << " | input=" << ensure_input_ms << " abi=" << validate_abi_ms
                << " kernels=" << ensure_kernels_ms << " fused=" << fused_kernel_ms
                << " detail=" << detail_ms << " sync=" << sync_ms << " download=" << download_ms
                << " submit=" << submit_ms
                << " present=" << (submitted_gpu_frame ? "direct_opengl" : "host_upload")
                << " | size=" << input.Width() << "x" << input.Height() << std::endl;
    }
  }

  void ReleaseScratchBuffers() override {
    working_.Release();
    pre_hs_working_.Release();
    hs_working_.Release();
    blur_horizontal_.Release();
    detail_scratch_.Release();
  }

  void ReleaseResources() override {
    fused_kernel_.Release();
    fused_stage_kernel_.Release();
    validate_kernel_.Release();
    blur_h_kernel_.Release();
    apply_v_kernel_.Release();
    hs_stage_.ReleaseResources();
    ReleaseScratchBuffers();
    resources_.Reset();
  }
};

}  // namespace

auto CreateOpenCLGPUPipeline() -> std::unique_ptr<GPUPipelineImpl> {
  return std::make_unique<OpenCLGPUPipeline>();
}

}  // namespace alcedo

#endif
