//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/scope/detail/scope_cuda_shared.cuh"
#include "edit/scope/final_display_frame_tap.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

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

enum class PresentEvent {
  Bind,
  Ensure,
  Map,
  Submit,
  Unmap,
  Notify,
};

class RecordingCudaPresentSink final : public IFrameSink {
 public:
  ~RecordingCudaPresentSink() override { ReleaseDeviceBuffer(); }

  void EnsureSize(int width, int height) override {
    events_.push_back(PresentEvent::Ensure);
    width_  = width;
    height_ = height;
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                       sizeof(float) * 4U;
    if (device_bytes_ != bytes) {
      ReleaseDeviceBuffer();
      if (bytes > 0) {
        if (::cudaMalloc(&device_, bytes) != cudaSuccess) {
          throw std::runtime_error("RecordingCudaPresentSink: cudaMalloc failed");
        }
        device_bytes_ = bytes;
      }
    }
  }

  auto MapResourceForWrite(FrameMemoryDomain /*domain*/) -> FrameWriteMapping override {
    events_.push_back(PresentEvent::Map);
    mapped_ = true;
    FrameWriteMapping mapping;
    mapping.data           = device_;
    mapping.row_bytes      = static_cast<std::size_t>(width_) * sizeof(float) * 4U;
    mapping.pixel_format   = FramePixelFormat::RGBA32F;
    mapping.memory_domain  = FrameMemoryDomain::CudaDevice;
    mapping.target_type    = FrameWriteTargetType::LinearBuffer;
    mapping.native_object  = reinterpret_cast<std::uintptr_t>(device_);
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
  }

  auto GetWidth() const -> int override { return width_; }
  auto GetHeight() const -> int override { return height_; }

  std::vector<PresentEvent>   events_;
  FinalDisplayFrameView       last_frame_{};
  FrameCompletionSubmission   last_bound_{};
  FrameCompletionSubmission   last_notified_{};
  int                         submit_count_           = 0;
  bool                        submitted_while_mapped_ = false;

 private:
  void ReleaseDeviceBuffer() {
    if (device_ != nullptr) {
      ::cudaFree(device_);
      device_       = nullptr;
      device_bytes_ = 0;
    }
  }

  void*       device_       = nullptr;
  std::size_t device_bytes_ = 0;
  int         width_        = 0;
  int         height_       = 0;
  bool        mapped_       = false;
};

auto LinearImage(const FinalDisplayFrameView& frame)
    -> const scope::cuda_detail::CudaLinearImageResource* {
  if (frame.image.backend != GpuBackend::Cuda || !frame.image.resource) {
    return nullptr;
  }
  return static_cast<const scope::cuda_detail::CudaLinearImageResource*>(
      frame.image.resource.get());
}

auto StreamSignal(const FinalDisplayFrameView& frame)
    -> const scope::cuda_detail::CudaStreamSignalResource* {
  if (frame.ready_signal.backend != GpuBackend::Cuda || !frame.ready_signal.resource) {
    return nullptr;
  }
  return static_cast<const scope::cuda_detail::CudaStreamSignalResource*>(
      frame.ready_signal.resource.get());
}

class CudaProductScopePresentFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<CudaProductRenderer>(document_, MakeUnpacker());
    image_    = MakeEncodedImage(91);
  }

  auto RenderTo(IFrameSink& sink, const FrameCompletionSubmission& submission = {})
      -> std::shared_ptr<ImageBuffer> {
    return renderer_->Render(image_, DecodeRes::FULL, RenderRequest{}, &sink, submission, false);
  }

  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CudaProductRenderer> renderer_;
  std::shared_ptr<ImageBuffer>         image_;
};

TEST_F(CudaProductScopePresentFixture,
       ProductPresentSubmitsReadableCudaDisplayFrameBeforeUnmapAndNotify) {
  RecordingCudaPresentSink sink;
  FrameCompletionSubmission submission;
  submission.metadata.image_identity          = 18;
  submission.metadata.session_epoch           = 4;
  submission.metadata.presentation_request_id = 249;
  submission.metadata.scope_update_allowed    = true;

  ASSERT_NE(RenderTo(sink, submission), nullptr);

  const std::vector<PresentEvent> expected = {PresentEvent::Bind,   PresentEvent::Ensure,
                                              PresentEvent::Map,    PresentEvent::Submit,
                                              PresentEvent::Unmap,  PresentEvent::Notify};
  EXPECT_EQ(sink.events_, expected);
  EXPECT_EQ(sink.submit_count_, 1);
  EXPECT_TRUE(sink.submitted_while_mapped_);
  EXPECT_EQ(sink.last_bound_.metadata.presentation_request_id, 249U);
  EXPECT_EQ(sink.last_notified_.metadata.presentation_request_id, 249U);

  const auto& frame = sink.last_frame_;
  ASSERT_TRUE(static_cast<bool>(frame));
  EXPECT_EQ(frame.image.backend, GpuBackend::Cuda);
  EXPECT_EQ(frame.format, FramePixelFormat::RGBA32F);
  EXPECT_EQ(frame.domain, AnalysisDomain::DisplayEncoded);
  EXPECT_GT(frame.width, 0);
  EXPECT_GT(frame.height, 0);
  EXPECT_EQ(frame.width, sink.GetWidth());
  EXPECT_EQ(frame.height, sink.GetHeight());
  EXPECT_EQ(frame.display_config.encoding_space, ColorUtils::ColorSpace::REC709);
  EXPECT_EQ(frame.display_config.encoding_eotf, ColorUtils::EOTF::GAMMA_2_2);

  const auto* image = LinearImage(frame);
  ASSERT_NE(image, nullptr);
  EXPECT_NE(image->device_ptr, nullptr);
  EXPECT_FALSE(image->owns_memory);
  EXPECT_EQ(image->width, frame.width);
  EXPECT_EQ(image->height, frame.height);
  EXPECT_EQ(image->row_bytes, static_cast<std::size_t>(frame.width) * sizeof(float) * 4U);

  const auto* signal = StreamSignal(frame);
  ASSERT_NE(signal, nullptr);
  EXPECT_NE(signal->stream, nullptr);

  std::vector<float> host(static_cast<std::size_t>(frame.width) * frame.height * 4U, 0.0f);
  ASSERT_EQ(::cudaMemcpy2D(host.data(), image->row_bytes, image->device_ptr, image->row_bytes,
                           image->row_bytes, static_cast<std::size_t>(frame.height),
                           cudaMemcpyDeviceToHost),
            cudaSuccess);
  bool any_finite_rgb = false;
  for (std::size_t i = 0; i + 3 < host.size(); i += 4) {
    ASSERT_TRUE(std::isfinite(host[i]));
    ASSERT_TRUE(std::isfinite(host[i + 1]));
    ASSERT_TRUE(std::isfinite(host[i + 2]));
    if (host[i] != 0.0f || host[i + 1] != 0.0f || host[i + 2] != 0.0f) {
      any_finite_rgb = true;
    }
  }
  EXPECT_TRUE(any_finite_rgb);
}

TEST_F(CudaProductScopePresentFixture, ProductPresentDisplayConfigMatchesDrtEncoding) {
  auto payload               = document_->Drt()->Params().Params();
  payload.encoding_space     = DrtColorSpace::Rec2020;
  payload.encoding_eotf      = DrtEotf::St2084;
  payload.peak_luminance     = 1000.0f;
  document_->Drt()->Params().ReplaceParams(payload);

  RecordingCudaPresentSink sink;
  ASSERT_NE(RenderTo(sink), nullptr);
  EXPECT_EQ(sink.submit_count_, 1);
  EXPECT_EQ(sink.last_frame_.display_config.encoding_space, ColorUtils::ColorSpace::REC2020);
  EXPECT_EQ(sink.last_frame_.display_config.encoding_eotf, ColorUtils::EOTF::ST2084);
  EXPECT_FLOAT_EQ(sink.last_frame_.display_config.peak_luminance, 1000.0f);
}

auto WaitForHistogramSnapshot(IScopeAnalyzer& analyzer, std::uint64_t image_identity,
                              std::uint64_t session_epoch) -> ScopeRenderSnapshot {
  ScopeRenderSnapshot snapshot;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto output = analyzer.GetLatestOutput();
    if (output.generation != 0 && output.histogram_valid) {
      snapshot = ReadScopeRenderSnapshot(output);
      if (snapshot.histogram.valid && snapshot.image_identity == image_identity &&
          snapshot.session_epoch == session_epoch) {
        return snapshot;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return snapshot;
}

TEST_F(CudaProductScopePresentFixture,
       DeferredTapKeepsCompletedHistogramAfterLaterPresentWouldHaveReclaimedTheSlot) {
  auto                     analyzer = CreateCudaScopeAnalyzer();
  ASSERT_NE(analyzer, nullptr);
  RecordingCudaPresentSink downstream;
  FinalDisplayFrameTapSink tap(&downstream, analyzer);
  tap.SetScopeAnalysisDeferred(true);
  tap.SetScopeActive(true);
  ScopeRequest request;
  request.enabled_mask = static_cast<uint32_t>(ScopeType::Histogram);
  request.target_fps   = 0;
  tap.SetScopeRequest(request);

  FrameCompletionSubmission first;
  first.metadata.image_identity          = 18;
  first.metadata.session_epoch           = 4;
  first.metadata.presentation_request_id = 31;
  first.metadata.scope_update_allowed    = true;
  ASSERT_NE(RenderTo(tap, first), nullptr);
  ASSERT_TRUE(static_cast<bool>(tap.GetCurrentScopeFrameView()));

  analyzer->SubmitFrame(tap.GetCurrentScopeFrameView(), request);
  // Give the analysis stream time to finish without consuming the slot.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  FrameCompletionSubmission second = first;
  second.metadata.presentation_request_id = 32;
  ASSERT_NE(RenderTo(tap, second), nullptr);

  const auto snapshot = WaitForHistogramSnapshot(*analyzer, 18, 4);
  EXPECT_TRUE(snapshot.histogram.valid);
  EXPECT_EQ(snapshot.image_identity, 18U);
  EXPECT_EQ(snapshot.session_epoch, 4U);
  EXPECT_GT(snapshot.generation, 0U);
}

}  // namespace
}  // namespace alcedo
