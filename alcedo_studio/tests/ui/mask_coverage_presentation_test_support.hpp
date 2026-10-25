//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file mask_coverage_presentation_test_support.hpp
/// @brief Shared real-resource harness of the coverage presentation tests (SM8.5, SM8.6).
///
/// The harness runs the production path with the resources of one backend (selected by
/// ALCEDO_TEST_EDITOR_BACKEND: cuda or opencl on Windows, metal on macOS): a render device on its
/// own thread writes AI Mask coverage through PresentMaskCoverage (and, for SM8.6, a photo frame
/// through the production FramePresenter), the viewport sinks hand the frames to the native targets
/// that the real lease adapter creates on a real QRhi, and ViewportMaskCoverageLayer imports the
/// coverage. Pixel values are read back from QRhi textures by the test only.

#pragma once

#include <QtGui/rhi/qrhi.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "../edit/runtime/ai_mask_raster_test_support.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "edit/runtime/mask_coverage_presentation.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/mask_coverage_sink.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"
#include "ui/editor_rhi/viewport_mask_coverage_layer.hpp"
#include "ui/qt_test_plugin_paths.hpp"

#if defined(HAVE_CUDA)
#include "edit/runtime/cuda/cuda_frame_presenter.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#endif
#if defined(HAVE_OPENCL)
#include "edit/runtime/opencl/opencl_frame_presenter.hpp"
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#endif
#if defined(HAVE_METAL)
#include "edit/runtime/metal/metal_frame_presenter.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "metal/metal_context.hpp"
#endif

namespace alcedo::editor_rhi::coverage_test {

using namespace std::chrono_literals;

inline EditorBackend           g_backend = EditorBackend::Cuda;
inline EditorStartupResult     g_startup{};

inline constexpr Extent2D      kAnalysis{64, 48};
inline constexpr std::uint64_t kImage       = 77;
inline constexpr std::uint64_t kEpoch       = 3;
inline constexpr std::uint64_t kNextImage   = 78;
inline constexpr std::uint64_t kNextEpoch   = 4;
inline constexpr auto          kPumpTimeout = 20s;

/// Candidate coverage with a distinct pattern per @p variant: a soft disc plus a ramp.
inline auto                    MakeCandidatePixels(int variant) -> AiMaskCoveragePixels {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAnalysis.width) * kAnalysis.height);
  const double              cx = 20.0 + 12.0 * variant;
  const double              cy = 22.0 + 3.0 * variant;
  for (std::uint32_t y = 0; y < kAnalysis.height; ++y) {
    for (std::uint32_t x = 0; x < kAnalysis.width; ++x) {
      const double d    = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
      const double disc = std::clamp((16.0 - d) / 4.0, 0.0, 1.0);
      const double ramp = 0.25 * x / kAnalysis.width;
      pixels[static_cast<std::size_t>(y) * kAnalysis.width + x] =
          static_cast<std::uint8_t>(std::lround(255.0 * std::max(disc, ramp)));
    }
  }
  return std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
}

/// Photo frame geometry of the analysis image with a crop and a rotation.
inline auto MakePhotoGeometry(float render_scale, ViewRequest view = {}) -> ResolvedRenderGeometry {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.1f, 0.15f, 0.8f, 0.7f};
  image.rotation_degrees = 12.0f;
  ResolutionRequest resolution;
  resolution.render_scale = render_scale;
  return ResolveRenderGeometry(MakeSourceGeometry(kAnalysis, kAnalysis), image, view, resolution,
                               SamplingFootprint{});
}

inline auto ExpectedCoverageR8(const AiMaskCoveragePixels&       pixels,
                               const MaskCoverageDisplayRequest& request)
    -> std::vector<std::uint8_t> {
  AiMaskCoverageTexture texture;
  texture.descriptor = AiMaskCandidateDescriptor(kAnalysis);
  texture.pixels     = *pixels;
  return ai_mask_test::EvaluateAiMaskR8(texture, request.geometry, request.invert, request.opacity);
}

/// The render thread of the producing device: one thread for the device lifetime, as in the
/// editor, so device state never crosses threads.
class RenderThread {
 public:
  RenderThread() : thread_([this] { Run(); }) {}
  ~RenderThread() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }

  template <class F>
  auto Post(F job) -> std::future<std::invoke_result_t<F>> {
    using Result = std::invoke_result_t<F>;
    auto task    = std::make_shared<std::packaged_task<Result()>>(std::move(job));
    auto future  = task->get_future();
    {
      std::lock_guard lock(mutex_);
      jobs_.emplace_back([task] { (*task)(); });
    }
    wake_.notify_all();
    return future;
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) {
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex                        mutex_;
  std::condition_variable           wake_;
  std::deque<std::function<void()>> jobs_;
  bool                              stop_ = false;
  std::thread                       thread_;
};

struct ProducerCounters {
  std::uint64_t host_to_device        = 0;
  std::uint64_t device_to_host        = 0;
  std::size_t   display_targets       = 0;
  std::uint64_t display_target_create = 0;
};

/// Result of one photo render: the host download of the presented display output (the bytes that
/// export writes) and the geometry of the presented frame.
struct PhotoRender {
  Extent2D               extent{};
  std::vector<float>     rgba;
  ResolvedRenderGeometry geometry{};
};

/// The render device of one backend behind the production coverage and photo operations.
class CoverageProducer {
 public:
  virtual ~CoverageProducer() = default;
  virtual auto Present(const MaskCoverageSelection&      selection,
                       const MaskCoverageDisplayRequest& request,
                       const MaskCoverageFrameIdentity& identity, IMaskCoverageSink& sink)
      -> MaskCoveragePublication                                                = 0;
  virtual auto Counters() -> ProducerCounters                                   = 0;
  /// Fail the next texture allocation of the device (FailTextureAllocationAfter(0)).
  virtual void FailNextTextureAllocation()                                      = 0;
  /**
   * @brief Compile and execute the photo DAG of @p document, present the display output to
   *        @p sink with the production FramePresenter (geometry stamped as Renderer does), and
   *        download it with FramePresenter::Download (the export path).
   */
  virtual auto RenderPhoto(const PipelineDocument& document, const PreparedRawInput& input,
                           const RenderRequest& request, IFrameSink& sink,
                           FrameCompletionSubmission submission) -> PhotoRender = 0;
  virtual void SetMaskAssets(std::shared_ptr<const MaskAssetReader> reader)     = 0;
  /// Photo DAG pass counters of the device.
  virtual auto PassStats() -> GpuNodePassStats                                  = 0;
};

template <class Device>
class DeviceCoverageProducer final : public CoverageProducer {
 public:
  using Backend = std::remove_reference_t<decltype(std::declval<Device&>().Workspace().Device())>;

  auto Present(const MaskCoverageSelection& selection, const MaskCoverageDisplayRequest& request,
               const MaskCoverageFrameIdentity& identity, IMaskCoverageSink& sink)
      -> MaskCoveragePublication override {
    return PresentMaskCoverage(device_, selection, request, identity, sink);
  }
  auto Counters() -> ProducerCounters override {
    const auto& workspace = device_.Workspace();
    return {workspace.Device().HostToDeviceCopyCount(), workspace.Device().DeviceToHostCopyCount(),
            workspace.MaskCoverageDisplay().Count(), workspace.MaskCoverageDisplay().CreateCount()};
  }
  void FailNextTextureAllocation() override {
    device_.Workspace().Device().FailTextureAllocationAfter(0);
  }
  auto RenderPhoto(const PipelineDocument& document, const PreparedRawInput& input,
                   const RenderRequest& request, IFrameSink& sink,
                   FrameCompletionSubmission submission) -> PhotoRender override {
    const auto plan     = GraphCompiler::Compile(document, input.CompileSource(), request);
    const auto output   = device_.Execute(plan, input, document);
    submission.geometry = plan.geometry;
    FramePresenter<Backend>::Present(device_, output, sink, submission, ViewerDisplayConfig{});
    const auto  host = FramePresenter<Backend>::Download(device_, output);
    const auto& mat  = host->GetCPUData();
    PhotoRender render;
    render.extent =
        Extent2D{static_cast<std::uint32_t>(mat.cols), static_cast<std::uint32_t>(mat.rows)};
    render.geometry = plan.geometry;
    render.rgba.resize(static_cast<std::size_t>(mat.cols) * mat.rows * 4);
    for (int y = 0; y < mat.rows; ++y) {
      std::memcpy(render.rgba.data() + static_cast<std::size_t>(y) * mat.cols * 4, mat.ptr(y),
                  static_cast<std::size_t>(mat.cols) * 4 * sizeof(float));
    }
    return render;
  }
  void SetMaskAssets(std::shared_ptr<const MaskAssetReader> reader) override {
    device_.Workspace().SetMaskAssets(std::move(reader));
  }
  auto PassStats() -> GpuNodePassStats override { return device_.PassStats(); }

 private:
  Device device_{};
};

inline auto MakeProducer(EditorBackend backend) -> std::unique_ptr<CoverageProducer> {
  switch (backend) {
    case EditorBackend::Cuda:
#if defined(HAVE_CUDA)
      return std::make_unique<DeviceCoverageProducer<CudaRenderDevice>>();
#else
      break;
#endif
    case EditorBackend::OpenCl:
#if defined(HAVE_OPENCL)
      return std::make_unique<DeviceCoverageProducer<OpenClRenderDevice>>();
#else
      break;
#endif
    case EditorBackend::Metal:
#if defined(HAVE_METAL)
      return std::make_unique<DeviceCoverageProducer<MetalRenderDevice>>();
#else
      break;
#endif
  }
  return nullptr;
}

/// The QRhi of the viewport's graphics API on the device that the editor startup selected.
inline auto MakeRhi(EditorBackend backend, std::unique_ptr<QOffscreenSurface>& surface)
    -> std::unique_ptr<QRhi> {
  switch (backend) {
    case EditorBackend::Cuda: {
#if defined(_WIN32) && defined(HAVE_CUDA)
      QRhiD3D11InitParams    params;
      QRhiD3D11NativeHandles adapter;
      adapter.adapterLuidLow  = g_startup.cuda_adapter->luid.LowPart;
      adapter.adapterLuidHigh = g_startup.cuda_adapter->luid.HighPart;
      return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D11, &params, {}, &adapter));
#else
      return nullptr;
#endif
    }
    case EditorBackend::OpenCl: {
      QRhiGles2InitParams params;
      params.format       = QSurfaceFormat::defaultFormat();
      params.shareContext = QOpenGLContext::globalShareContext();
      surface.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
      params.fallbackSurface = surface.get();
      return std::unique_ptr<QRhi>(QRhi::create(QRhi::OpenGLES2, &params));
    }
    case EditorBackend::Metal: {
#if defined(HAVE_METAL)
      QRhiMetalInitParams    params;
      QRhiMetalNativeHandles device;
      device.dev      = reinterpret_cast<MTLDevice*>(MetalContext::Instance().Device());
      device.cmdQueue = reinterpret_cast<MTLCommandQueue*>(MetalContext::Instance().Queue());
      return std::unique_ptr<QRhi>(QRhi::create(QRhi::Metal, &params, {}, &device));
#else
      return nullptr;
#endif
    }
  }
  return nullptr;
}

/**
 * @brief Fixture of one viewport with its coverage channel on a real QRhi, and one producing
 *        render device on its own thread.
 *
 * The fixture thread stands in for the scene-graph render thread of the viewport: it creates the
 * requested native targets and imports frames.
 */
class CoverageChannelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!g_startup.ok) {
      GTEST_SKIP() << "editor backend " << ToString(g_backend)
                   << " is unavailable: " << g_startup.error;
    }
    rhi_ = MakeRhi(g_backend, surface_);
    ASSERT_NE(rhi_, nullptr) << "QRhi creation failed for " << ToString(g_backend);
    if (g_backend == EditorBackend::OpenCl) {
      // The OpenCL lease adapter creates and deletes GL textures in the current context.
      ASSERT_TRUE(rhi_->makeThreadLocalNativeContextCurrent());
    }
    adapter_  = MakeLeaseTargetAdapter(g_backend);
    producer_ = render_thread_.Post([] { return MakeProducer(g_backend); }).get();
    ASSERT_NE(producer_, nullptr);

    viewport_ = std::make_unique<EditorViewportItem>();
    viewport_->setImageIdentity(kImage);
    viewport_->setSessionEpoch(kEpoch);
    // Stands in for EditorViewportRenderer::synchronize() on a shown window.
    viewport_->resumePresentation();
    viewport_->present_queue()->SetConsumerAvailable(true);
    viewport_->maskCoverageQueue()->SetConsumerAvailable(true);
    layer_.Attach(viewport_->maskCoverageQueue(), viewport_->maskCoverageSinkShared());
  }

  void TearDown() override {
    if (!rhi_) {
      return;
    }
    ReleaseBeforeTeardown();
    layer_.Release();
    if (viewport_) {
      viewport_->maskCoverageQueue()->InvalidateTargetGeneration();
      layer_.DestroyRetiredNatives(*adapter_);
    }
    layer_.DestroyNatives(*adapter_);
    if (producer_) {
      render_thread_.Post([this] { producer_.reset(); }).get();
    }
    viewport_.reset();
    adapter_.reset();
    rhi_.reset();
  }

  /// QRhi resources of a derived fixture that must go before the layer and the QRhi.
  virtual void ReleaseBeforeTeardown() {}

  /// Run @p job on the render thread while this thread services the coverage queue as the
  /// viewport render thread does, and @p service each iteration.
  template <class Job>
  auto RunOnRenderThread(Job job, ILeaseTargetAdapter* adapter = nullptr,
                         const std::function<void()>& service = {}) -> std::invoke_result_t<Job> {
    auto       done     = render_thread_.Post(std::move(job));
    const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
    while (done.wait_for(1ms) != std::future_status::ready) {
      layer_.FulfillTargetRequests(rhi_.get(), adapter != nullptr ? *adapter : *adapter_,
                                   viewport_->sessionEpoch(), viewport_->imageIdentity());
      if (service) {
        service();
      }
      QCoreApplication::processEvents();
      if (std::chrono::steady_clock::now() > deadline) {
        // Release the producer to report the failure.
        viewport_->maskCoverageQueue()->Shutdown();
        viewport_->present_queue()->Shutdown();
        ADD_FAILURE() << "the render-thread job did not finish";
        break;
      }
    }
    return done.get();
  }

  /// Run the production coverage operation on the render thread.
  auto Present(const MaskCoverageSelection& selection, const MaskCoverageDisplayRequest& request,
               MaskCoverageFrameIdentity identity, IMaskCoverageSink* sink = nullptr,
               ILeaseTargetAdapter* adapter = nullptr) -> MaskCoveragePublication {
    auto* target_sink = sink != nullptr ? sink : viewport_->maskCoverageSink();
    return RunOnRenderThread(
        [&] { return producer_->Present(selection, request, identity, *target_sink); }, adapter);
  }

  auto Consume() -> bool {
    return layer_.ConsumeNewest(rhi_.get(), viewport_->sessionEpoch(), viewport_->imageIdentity());
  }

  auto Counters() -> ProducerCounters {
    return render_thread_.Post([this] { return producer_->Counters(); }).get();
  }

  /// Test-only readback of a texture as RGBA32F texels, in the row order of the texture data.
  auto ReadBack(QRhiTexture* texture) -> std::vector<float> {
    QRhiCommandBuffer* command_buffer = nullptr;
    if (rhi_->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess) {
      ADD_FAILURE() << "beginOffscreenFrame failed";
      return {};
    }
    QRhiReadbackResult result;
    auto*              batch = rhi_->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(texture), &result);
    command_buffer->resourceUpdate(batch);
    rhi_->endOffscreenFrame();
    std::vector<float> texels(static_cast<std::size_t>(result.data.size()) / sizeof(float));
    std::memcpy(texels.data(), result.data.constData(), texels.size() * sizeof(float));
    return texels;
  }

  /// The displayed coverage equals @p expected_r8 in red (one code), with green and blue 0 and
  /// alpha 1.
  void ExpectDisplayed(const std::vector<std::uint8_t>& expected_r8, const std::string& label) {
    ASSERT_NE(layer_.Texture(), nullptr) << label;
    const auto texels = ReadBack(layer_.Texture());
    ASSERT_EQ(texels.size(), expected_r8.size() * 4) << label;
    int failures = 0;
    for (std::size_t i = 0; i < expected_r8.size() && failures < 8; ++i) {
      const float r  = texels[i * 4];
      const bool  ok = std::abs(r * 255.0f - static_cast<float>(expected_r8[i])) <= 1.0f &&
                      texels[i * 4 + 1] == 0.0f && texels[i * 4 + 2] == 0.0f &&
                      texels[i * 4 + 3] == 1.0f;
      if (!ok) {
        ADD_FAILURE() << label << ": texel " << i << " = (" << r << ", " << texels[i * 4 + 1]
                      << ", " << texels[i * 4 + 2] << ", " << texels[i * 4 + 3]
                      << "), expected red " << int(expected_r8[i]) << " / 255";
        ++failures;
      }
    }
  }

  static auto Identity(std::uint64_t request_id) -> MaskCoverageFrameIdentity {
    return {request_id, kEpoch, kImage};
  }

  static auto Candidate(const AiMaskCoveragePixels& pixels, AiMaskEdgeAdjustment edge = {})
      -> MaskCoverageSelection {
    return MaskCoverageCandidateSelection{pixels, kAnalysis, edge};
  }

  [[nodiscard]] auto UsesQueuedTargets() const -> bool { return g_backend != EditorBackend::Metal; }

  std::unique_ptr<QOffscreenSurface>   surface_;
  std::unique_ptr<QRhi>                rhi_;
  std::unique_ptr<ILeaseTargetAdapter> adapter_;
  RenderThread                         render_thread_;
  std::unique_ptr<CoverageProducer>    producer_;
  std::unique_ptr<EditorViewportItem>  viewport_;
  ViewportMaskCoverageLayer            layer_;
};

/// Body of main() of a coverage presentation test binary.
inline auto RunCoverageChannelTests(int argc, char** argv) -> int {
#ifdef _WIN32
  // Native GPU devices need the Windows QPA; offscreen has no OpenGL context for OpenCL sharing.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArray("windows"));
  }
#endif
  alcedo::ui::test::ConfigureQtPluginPaths(argc > 0 ? argv[0] : nullptr,
                                           /*force_offscreen=*/false);
  const QByteArray requested = qgetenv("ALCEDO_TEST_EDITOR_BACKEND").toLower();
#if defined(__APPLE__)
  (void)requested;
  g_backend = EditorBackend::Metal;
#else
  g_backend = requested == "opencl" ? EditorBackend::OpenCl : EditorBackend::Cuda;
#endif
  if (g_backend == EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }
  QGuiApplication app(argc, argv);
  g_startup = ApplyEditorBackendBeforeWindow(g_backend);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

}  // namespace alcedo::editor_rhi::coverage_test
