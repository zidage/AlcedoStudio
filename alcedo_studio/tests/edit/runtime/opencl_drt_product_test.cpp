//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>

#include "edit/runtime/cuda/cuda_render_device.hpp"
#endif

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/opencl/opencl_renderer.hpp"
#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasOpenClImageDevice() -> bool {
  if (!TryInitializeOpenClRuntime()) {
    return false;
  }
  return OpenClContext::Instance().Capabilities().image_support;
}

#ifdef HAVE_CUDA
auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}
#endif

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[0] = tag;
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern,
                                           gpu_dag_test::DefaultLinearization(),
                                           gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

auto AllFinite(const std::vector<Rgba>& pixels) -> bool {
  if (pixels.empty()) {
    return false;
  }
  return std::all_of(pixels.begin(), pixels.end(), [](const Rgba& pixel) {
    return std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b) &&
           std::isfinite(pixel.a);
  });
}

auto MaxAbsError(const std::vector<Rgba>& lhs, const std::vector<Rgba>& rhs) -> float {
  if (lhs.size() != rhs.size()) {
    return INFINITY;
  }
  float error = 0.0f;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    error = std::max(error, std::fabs(lhs[index].r - rhs[index].r));
    error = std::max(error, std::fabs(lhs[index].g - rhs[index].g));
    error = std::max(error, std::fabs(lhs[index].b - rhs[index].b));
    error = std::max(error, std::fabs(lhs[index].a - rhs[index].a));
  }
  return error;
}

auto HostRgbaIsFinite(const std::shared_ptr<ImageBuffer>& image) -> bool {
  if (!image || !image->cpu_data_valid_) {
    return false;
  }
  const auto& mat = image->GetCPUData();
  if (mat.empty() || mat.type() != CV_32FC4) {
    return false;
  }
  for (int row = 0; row < mat.rows; ++row) {
    const auto* pixels = mat.ptr<cv::Vec4f>(row);
    for (int col = 0; col < mat.cols; ++col) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!std::isfinite(pixels[col][channel])) {
          return false;
        }
      }
    }
  }
  return true;
}

auto CompareHostRgba(const std::shared_ptr<ImageBuffer>& left,
                     const std::shared_ptr<ImageBuffer>& right, float max_abs) -> bool {
  if (!left || !right || !left->cpu_data_valid_ || !right->cpu_data_valid_) {
    return false;
  }
  const auto& a = left->GetCPUData();
  const auto& b = right->GetCPUData();
  if (a.empty() || b.empty() || a.size() != b.size() || a.type() != CV_32FC4 ||
      b.type() != CV_32FC4) {
    return false;
  }
  for (int row = 0; row < a.rows; ++row) {
    const auto* pa = a.ptr<cv::Vec4f>(row);
    const auto* pb = b.ptr<cv::Vec4f>(row);
    for (int col = 0; col < a.cols; ++col) {
      for (int channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(pa[col][channel]) || !std::isfinite(pb[col][channel])) {
          return false;
        }
        if (std::abs(pa[col][channel] - pb[col][channel]) > max_abs) {
          return false;
        }
      }
    }
  }
  return true;
}

auto Download(OpenClRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  device.WaitIdle();
  const auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

#ifdef HAVE_CUDA
auto Download(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  device.WaitIdle();
  const auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}
#endif

class OpenClDrtFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    input_  = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                            gpu_dag_test::FullSensor(16, 12));
    plan_   = GraphCompiler::Compile(document_, input_.CompileSource(), RenderRequest{});
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  auto             Render() -> GraphValueId { return device_->Execute(plan_, input_, document_); }

  PipelineDocument document_;
  PreparedRawInput input_;
  ExecutionPlan    plan_;
  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClDrtFixture, OpenClDrtAcesMatchesCudaReferenceWithinTolerance) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA reference is not enabled in this build.";
#else
  if (!HasCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available for the reference render.";
  }

  auto aces   = document_.Drt()->Params().Params();
  aces.method = DrtMethod::Aces20;
  document_.Drt()->Params().ReplaceParams(aces);
  const auto opencl_pixels = Download(*device_, Render());

  auto       cuda_document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(cuda_document);
  cuda_document.Drt()->Params().ReplaceParams(aces);
  CudaRenderDevice cuda_device;
  const auto       cuda_plan = GraphCompiler::Compile(cuda_document, input_.CompileSource(), {});
  const auto       cuda_pixels =
      Download(cuda_device, cuda_device.Execute(cuda_plan, input_, cuda_document));

  ASSERT_EQ(opencl_pixels.size(), cuda_pixels.size());
  ASSERT_TRUE(AllFinite(opencl_pixels));
  ASSERT_TRUE(AllFinite(cuda_pixels));
  EXPECT_LT(MaxAbsError(opencl_pixels, cuda_pixels), 5.0e-3f);
#endif
}

TEST_F(OpenClDrtFixture, OpenClDrtOpenDrtMatchesCudaReferenceWithinTolerance) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA reference is not enabled in this build.";
#else
  if (!HasCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available for the reference render.";
  }

  const auto opencl_pixels = Download(*device_, Render());
  auto       cuda_document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(cuda_document);
  CudaRenderDevice cuda_device;
  const auto       cuda_plan = GraphCompiler::Compile(cuda_document, input_.CompileSource(), {});
  const auto       cuda_pixels =
      Download(cuda_device, cuda_device.Execute(cuda_plan, input_, cuda_document));

  ASSERT_EQ(opencl_pixels.size(), cuda_pixels.size());
  ASSERT_TRUE(AllFinite(opencl_pixels));
  ASSERT_TRUE(AllFinite(cuda_pixels));
  EXPECT_LT(MaxAbsError(opencl_pixels, cuda_pixels), 5.0e-3f);
#endif
}

TEST_F(OpenClDrtFixture, OpenClDrtPackedWriteDoesNotCopyFullDto) {
  OperatorModelFullDtoCopyCount::Reset();
  (void)Render();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST_F(OpenClDrtFixture, OpenClDrtEditRunsOnlyDrtPass) {
  (void)Download(*device_, Render());
  device_->ResetPassStats();
  device_->Workspace().Device().ResetCounters();

  auto params           = document_.Drt()->Params().Params();
  params.peak_luminance = 200.0f;
  document_.Drt()->Params().ReplaceParams(params);
  ASSERT_TRUE(AllFinite(Download(*device_, Render())));
  const auto stats = device_->PassStats();
  EXPECT_EQ(stats.source_h2d_count, 0U);
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 0U);
  EXPECT_EQ(stats.primary_grade_execute, 0U);
  EXPECT_EQ(stats.drt_execute, 1U);
  EXPECT_EQ(stats.drt_skip, 0U);

  device_->ResetPassStats();
  device_->Workspace().Device().ResetCounters();
  ASSERT_TRUE(AllFinite(Download(*device_, Render())));
  EXPECT_EQ(device_->PassStats().drt_execute, 0U);
  EXPECT_EQ(device_->PassStats().drt_skip, 1U);
  EXPECT_EQ(device_->Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
}

enum class PresentEvent {
  Bind,
  Ensure,
  Map,
  Unmap,
  Submit,
  Notify,
};

class RecordingOpenClPresentSink : public IFrameSink {
 public:
  explicit RecordingOpenClPresentSink(bool compatible = true) : compatible_(compatible) {}

  ~RecordingOpenClPresentSink() override { ReleaseImage(); }

  void EnsureSize(int width, int height) override {
    events_.push_back(PresentEvent::Ensure);
    if (width_ != width || height_ != height) {
      ReleaseImage();
      width_  = width;
      height_ = height;
      cl_image_format format{};
      format.image_channel_order     = CL_RGBA;
      format.image_channel_data_type = CL_FLOAT;
      cl_image_desc descriptor{};
      descriptor.image_type   = CL_MEM_OBJECT_IMAGE2D;
      descriptor.image_width  = static_cast<std::size_t>(width);
      descriptor.image_height = static_cast<std::size_t>(height);
      cl_int error            = CL_SUCCESS;
      image_ = clCreateImage(OpenClContext::Instance().Context(), CL_MEM_READ_WRITE, &format,
                             &descriptor, nullptr, &error);
      if (error != CL_SUCCESS || image_ == nullptr) {
        throw std::runtime_error("RecordingOpenClPresentSink: clCreateImage failed");
      }
    }
  }

  auto MapResourceForWrite(FrameMemoryDomain preferred_domain) -> FrameWriteMapping override {
    events_.push_back(PresentEvent::Map);
    preferred_domain_ = preferred_domain;
    mapped_           = true;
    FrameWriteMapping mapping;
    mapping.data         = reinterpret_cast<void*>(image_);
    mapping.row_bytes    = static_cast<std::size_t>(width_) * sizeof(float) * 4U;
    mapping.pixel_format = FramePixelFormat::RGBA32F;
    mapping.memory_domain =
        compatible_ ? FrameMemoryDomain::OpenClDevice : FrameMemoryDomain::HostVisible;
    mapping.target_type =
        compatible_ ? FrameWriteTargetType::OpenClImage : FrameWriteTargetType::LinearBuffer;
    mapping.native_object = reinterpret_cast<std::uintptr_t>(image_);
    return mapping;
  }

  void UnmapResource() override {
    events_.push_back(PresentEvent::Unmap);
    mapped_ = false;
  }

  void BindFrameSubmission(const FrameCompletionSubmission& submission) override {
    events_.push_back(PresentEvent::Bind);
    last_bound_ = submission;
  }

  void SubmitFinalDisplayFrame(const FinalDisplayFrameView& frame) override {
    events_.push_back(PresentEvent::Submit);
    submitted_while_mapped_ = mapped_;
    last_frame_             = frame;
    ++submit_count_;
  }

  void NotifyFrameReady(const FrameCompletionSubmission& submission) override {
    events_.push_back(PresentEvent::Notify);
    last_notified_ = submission;
    ++notify_count_;
  }

  auto                      GetWidth() const -> int override { return width_; }
  auto                      GetHeight() const -> int override { return height_; }

  std::vector<PresentEvent> events_;
  FinalDisplayFrameView     last_frame_{};
  FrameCompletionSubmission last_bound_{};
  FrameCompletionSubmission last_notified_{};
  FrameMemoryDomain         preferred_domain_       = FrameMemoryDomain::HostVisible;
  int                       submit_count_           = 0;
  int                       notify_count_           = 0;
  bool                      submitted_while_mapped_ = false;

 private:
  void ReleaseImage() {
    if (image_ != nullptr) {
      (void)clFinish(OpenClContext::Instance().ProductQueue());
      (void)clReleaseMemObject(image_);
      image_ = nullptr;
    }
  }

  cl_mem image_      = nullptr;
  int    width_      = 0;
  int    height_     = 0;
  bool   mapped_     = false;
  bool   compatible_ = true;
};

class OpenClRendererFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<OpenClRenderer>(document_, MakeUnpacker());
    image_    = MakeEncodedImage(91);
  }

  auto RenderHost(bool session = true) -> std::shared_ptr<ImageBuffer> {
    return renderer_->Render(
        image_, DecodeRes::FULL, RenderRequest{}, nullptr, {}, true,
        session ? RenderCachePolicy::UseSessionCache : RenderCachePolicy::BypassSessionCache);
  }

  auto RenderTo(IFrameSink& sink, const FrameCompletionSubmission& submission = {})
      -> std::shared_ptr<ImageBuffer> {
    return renderer_->Render(image_, DecodeRes::FULL, RenderRequest{}, &sink, submission, false);
  }

  auto RenderRole(FrameRole role, std::uint32_t max_edge) -> std::shared_ptr<ImageBuffer> {
    RenderRequest request;
    request.resolution.max_edge = max_edge;
    FrameCompletionSubmission submission;
    submission.metadata.frame_role = role;
    return renderer_->Render(image_, DecodeRes::FULL, request, nullptr, submission, true);
  }

  auto GeometryId() const -> GraphValueId {
    return {NodeId{"geometry"}, PortId{"scene_source"}};
  }
  auto SensorId() const -> GraphValueId {
    return {NodeId{"develop"}, PortId{"sensor_linear"}};
  }

  auto Exposure() -> ExposureModel* {
    return dynamic_cast<ExposureModel*>(
        document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  }

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<OpenClRenderer>   renderer_;
  std::shared_ptr<ImageBuffer>      image_;
};

TEST_F(OpenClRendererFixture, OpenClRendererPresentsWorkspaceImageWithoutHostDownload) {
  OpenClApiCounterScope counter_scope(true);
  ResetOpenClApiCounters();
  RecordingOpenClPresentSink sink;
  FrameCompletionSubmission  submission;
  submission.metadata.image_identity          = 18;
  submission.metadata.session_epoch           = 4;
  submission.metadata.presentation_request_id = 249;
  submission.metadata.scope_update_allowed    = true;

  const auto before                           = SnapshotOpenClApiCounters();
  ASSERT_NE(RenderTo(sink, submission), nullptr);
  renderer_->Device().WaitIdle();
  const auto delta = DeltaOpenClApiCounters(before, SnapshotOpenClApiCounters());

  const std::vector<PresentEvent> expected = {PresentEvent::Bind,   PresentEvent::Ensure,
                                              PresentEvent::Map,    PresentEvent::Unmap,
                                              PresentEvent::Submit, PresentEvent::Notify};
  EXPECT_EQ(sink.events_, expected);
  EXPECT_EQ(sink.preferred_domain_, FrameMemoryDomain::OpenClDevice);
  EXPECT_EQ(sink.submit_count_, 1);
  EXPECT_EQ(sink.notify_count_, 1);
  EXPECT_FALSE(sink.submitted_while_mapped_);
  EXPECT_EQ(sink.last_bound_.metadata.presentation_request_id, 249U);
  EXPECT_EQ(sink.last_notified_.metadata.presentation_request_id, 249U);
  EXPECT_EQ(delta.d2h_bytes, 0U);

  const auto& frame = sink.last_frame_;
  ASSERT_TRUE(static_cast<bool>(frame));
  EXPECT_EQ(frame.image.backend, GpuBackend::OpenCL);
  EXPECT_EQ(frame.image.resource_type, FrameWriteTargetType::OpenClImage);
  EXPECT_EQ(frame.format, FramePixelFormat::RGBA32F);
  EXPECT_EQ(frame.domain, AnalysisDomain::DisplayEncoded);
  EXPECT_EQ(frame.width, sink.GetWidth());
  EXPECT_EQ(frame.height, sink.GetHeight());
  ASSERT_NE(frame.image.resource, nullptr);
  ASSERT_NE(frame.ready_signal.resource, nullptr);
  EXPECT_EQ(frame.ready_signal.backend, GpuBackend::OpenCL);
  const auto* image =
      static_cast<const scope::opencl_detail::OpenClImageResource*>(frame.image.resource.get());
  const auto* signal = static_cast<const scope::opencl_detail::OpenClEventSignalResource*>(
      frame.ready_signal.resource.get());
  ASSERT_NE(image, nullptr);
  ASSERT_NE(signal, nullptr);
  EXPECT_NE(image->image, nullptr);
  EXPECT_NE(signal->event, nullptr);
  EXPECT_EQ(image->width, frame.width);
  EXPECT_EQ(image->height, frame.height);
}

TEST_F(OpenClRendererFixture, OpenClScopeTapUsesTheFinalDisplayImageAndSubmissionEvent) {
  auto analyzer = CreateOpenClScopeAnalyzer();
  ASSERT_NE(analyzer, nullptr);
  RecordingOpenClPresentSink downstream;
  FinalDisplayFrameTapSink   tap(&downstream, analyzer);
  ScopeRequest               request;
  request.enabled_mask        = static_cast<uint32_t>(ScopeType::Histogram);
  request.histogram_bins      = 64;
  request.analysis_downsample = 1;
  request.target_fps          = 0;
  tap.SetScopeRequest(request);
  tap.SetScopeActive(true);

  FrameCompletionSubmission submission;
  submission.metadata.image_identity          = 7;
  submission.metadata.session_epoch           = 2;
  submission.metadata.presentation_request_id = 88;
  submission.metadata.scope_update_allowed    = true;
  ASSERT_NE(RenderTo(tap, submission), nullptr);

  const auto display = tap.GetCurrentDisplayFrameView();
  const auto scope   = tap.GetCurrentScopeFrameView();
  ASSERT_TRUE(static_cast<bool>(display));
  ASSERT_TRUE(static_cast<bool>(scope));
  EXPECT_EQ(display.image.backend, GpuBackend::OpenCL);
  EXPECT_EQ(scope.image.backend, GpuBackend::OpenCL);
  EXPECT_EQ(display.image.resource_type, FrameWriteTargetType::OpenClImage);
  EXPECT_EQ(scope.image.resource_type, FrameWriteTargetType::OpenClImage);
  EXPECT_EQ(display.image.resource.get(), scope.image.resource.get());
  EXPECT_EQ(display.ready_signal.resource.get(), scope.ready_signal.resource.get());
  EXPECT_EQ(display.image_identity, 7U);
  EXPECT_EQ(display.session_epoch, 2U);
  EXPECT_EQ(display.display_generation, 88U);
  ASSERT_NE(display.ready_signal.resource, nullptr);
  const auto* signal = static_cast<const scope::opencl_detail::OpenClEventSignalResource*>(
      display.ready_signal.resource.get());
  ASSERT_NE(signal, nullptr);
  EXPECT_NE(signal->event, nullptr);

  (void)clFinish(OpenClContext::Instance().ProductQueue());
  const auto output = analyzer->GetLatestOutput();
  EXPECT_GT(output.generation, 0U);
  EXPECT_TRUE(output.histogram_valid);
  EXPECT_EQ(output.image_identity, 7U);
  EXPECT_EQ(output.session_epoch, 2U);
}

TEST_F(OpenClRendererFixture, OpenClOneShotRenderDoesNotPublishIntoSessionCache) {
  ASSERT_NE(RenderHost(true), nullptr);
  const auto session_before = renderer_->SessionResources();
  ASSERT_GT(session_before.published_result_count, 0U);
  renderer_->ResetStats();

  ASSERT_NE(RenderHost(false), nullptr);
  EXPECT_EQ(renderer_->OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer_->SessionResources().published_result_count,
            session_before.published_result_count);
  EXPECT_EQ(renderer_->SessionResources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer_->Stats().plan_cache_hits, 0U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
}

TEST_F(OpenClRendererFixture, OpenClPipelineReturnReleasesSessionResourcesAfterGpuCompletion) {
  ASSERT_NE(RenderHost(true), nullptr);
  ASSERT_GT(renderer_->SessionResources().published_result_count, 0U);
  renderer_->ReleaseSessionCaches();
  const auto resources = renderer_->SessionResources();
  EXPECT_EQ(resources.published_result_count, 0U);
  EXPECT_EQ(resources.texture_pool_entry_count, 0U);
  EXPECT_EQ(resources.prepared_source_entry_count, 0U);
  EXPECT_TRUE(resources.session_value_ids.empty());
  EXPECT_FALSE(renderer_->Device().Workspace().IsRendering());
}

TEST_F(OpenClRendererFixture, OpenClBackendFailureDoesNotEnterCpuOrLegacyOpenClExecution) {
  ASSERT_NE(RenderHost(true), nullptr);
  const auto published_before = renderer_->SessionResources().published_result_count;
  auto       params           = document_->Drt()->Params().Params();
  params.peak_luminance       = 200.0f;
  document_->Drt()->Params().ReplaceParams(params);
  renderer_->Device().Workspace().Device().FailNextUpload();

  EXPECT_THROW(RenderHost(true), std::runtime_error);
  EXPECT_EQ(renderer_->SessionResources().published_result_count, published_before);
  EXPECT_EQ(renderer_->Device().Workspace().Images().UnpublishedCount(), 0U);
  EXPECT_FALSE(renderer_->Device().Workspace().IsRendering());
}

TEST_F(OpenClRendererFixture, OpenClRealRawEditorUsesTheThreeNodeDag) {
  EXPECT_EQ(document_->Graph().Nodes().size(), 3U);
  EXPECT_EQ(document_->Graph().Edges().size(), 2U);
  ASSERT_NE(document_->Develop(), nullptr);
  ASSERT_NE(document_->PrimaryGrade(), nullptr);
  ASSERT_NE(document_->Drt(), nullptr);
  auto develop            = document_->Develop()->Params().Params();
  develop.demosaic_method = "legacy";
  document_->Develop()->Params().ReplaceParams(develop);

  ASSERT_NE(RenderHost(true), nullptr);
  EXPECT_EQ(renderer_->Stats().pass.drt_execute, 1U);
  EXPECT_EQ(document_->Graph().Nodes().size(), 3U);
  EXPECT_EQ(document_->Graph().Edges().size(), 2U);
}

TEST_F(OpenClRendererFixture, OpenClPresentRejectsAnIncompatibleSinkWithoutHostSubmission) {
  RecordingOpenClPresentSink sink(false);
  EXPECT_THROW(RenderTo(sink), std::runtime_error);
  EXPECT_EQ(sink.submit_count_, 0);
  EXPECT_EQ(sink.notify_count_, 0);
  EXPECT_EQ(renderer_->SessionResources().published_result_count, 0U);
  EXPECT_EQ(renderer_->Device().Workspace().Images().UnpublishedCount(), 0U);
  EXPECT_FALSE(renderer_->Device().Workspace().IsRendering());
}

TEST_F(OpenClRendererFixture, QualityBaseBypassesEveryResultCacheAfterSensorDevelop) {
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(40.0f);
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto geometry_handle  = images.Find(GeometryId())->Handle();
  const auto geometry_rev     = images.PublishedRevision(GeometryId());
  const auto publishes_before = images.PersistentPublishCount();
  const auto values_before    = renderer_->Device().Workspace().Values().Size();
  renderer_->ResetStats();
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 0U);
  EXPECT_GE(stats.pass.geometry_execute, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 0U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 0U);
  EXPECT_EQ(stats.pass.drt_skip, 0U);
  EXPECT_GT(stats.pass.result_policy_bypass, 0U);
  EXPECT_EQ(stats.pass.persistent_result_lookups, 1U);
  EXPECT_EQ(images.PersistentPublishCount(), publishes_before);
  EXPECT_EQ(images.UnpublishedCount(), 0U);
  EXPECT_EQ(images.Find(GeometryId())->Handle(), geometry_handle);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  EXPECT_EQ(images.PublishedRepresentation(GeometryId()).extent.width, 16U);
  EXPECT_EQ(renderer_->Device().Workspace().Values().Size(), values_before);
}

TEST_F(OpenClRendererFixture, InteractiveQualityBaseInteractiveReuses2560PixelResults) {
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto geometry_handle = images.Find(GeometryId())->Handle();
  const auto geometry_rev    = images.PublishedRevision(GeometryId());
  const auto geometry_repr   = images.PublishedRepresentation(GeometryId());
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  EXPECT_EQ(images.Find(GeometryId())->Handle(), geometry_handle);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  EXPECT_EQ(images.PublishedRepresentation(GeometryId()).extent, geometry_repr.extent);
  renderer_->ResetStats();
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
}

TEST_F(OpenClRendererFixture, QualityBasePixelsMatchFreshExecutionWithinDeclaredTolerance) {
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(35.0f);
  document_->Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});
  auto* exposure = Exposure();
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.4f);
  ASSERT_TRUE(HostRgbaIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  const auto quality = RenderRole(FrameRole::QualityBase, 32);
  ASSERT_TRUE(HostRgbaIsFinite(quality));
  RenderRequest fresh_request;
  fresh_request.resolution.max_edge = 32;
  const auto fresh = renderer_->Render(image_, DecodeRes::FULL, fresh_request, nullptr, {}, true,
                                       RenderCachePolicy::BypassSessionCache);
  ASSERT_TRUE(HostRgbaIsFinite(fresh));
  EXPECT_EQ(quality->GetCPUData().cols, fresh->GetCPUData().cols);
  EXPECT_EQ(quality->GetCPUData().rows, fresh->GetCPUData().rows);
  EXPECT_TRUE(CompareHostRgba(quality, fresh, 1.0e-4f));
}

}  // namespace
}  // namespace alcedo
