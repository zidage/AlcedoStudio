//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file mask_coverage_presentation_test.cpp
/// @brief SM8.5: the dedicated coverage presentation channel of the editor viewport.
///
/// Each test runs the production path end to end with real resources of one backend (selected by
/// ALCEDO_TEST_EDITOR_BACKEND: cuda or opencl on Windows, metal on macOS): a render device
/// writes AI Mask coverage through PresentMaskCoverage on its own thread, the viewport's coverage
/// sink hands it to the native targets that the real lease adapter creates on a real QRhi, and
/// ViewportMaskCoverageLayer imports it. Pixel values are read back from the imported QRhiTexture
/// by the test only, and compared with the independent host reference of the raster Mask rule.

#include "edit/runtime/mask_coverage_presentation.hpp"

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
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
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
#endif
#if defined(HAVE_METAL)
#include "edit/runtime/metal/metal_frame_presenter.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "metal/metal_context.hpp"
#endif

namespace alcedo::editor_rhi {
namespace {

using namespace std::chrono_literals;

EditorBackend           g_backend = EditorBackend::Cuda;
EditorStartupResult     g_startup{};

constexpr Extent2D      kAnalysis{64, 48};
constexpr std::uint64_t kImage       = 77;
constexpr std::uint64_t kEpoch       = 3;
constexpr std::uint64_t kNextImage   = 78;
constexpr std::uint64_t kNextEpoch   = 4;
constexpr auto          kPumpTimeout = 20s;

/// Candidate coverage with a distinct pattern per @p variant: a soft disc plus a ramp.
auto                    MakeCandidatePixels(int variant) -> AiMaskCoveragePixels {
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
auto MakePhotoGeometry(float render_scale) -> ResolvedRenderGeometry {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.1f, 0.15f, 0.8f, 0.7f};
  image.rotation_degrees = 12.0f;
  ResolutionRequest resolution;
  resolution.render_scale = render_scale;
  return ResolveRenderGeometry(MakeSourceGeometry(kAnalysis, kAnalysis), image, ViewRequest{},
                               resolution, SamplingFootprint{});
}

auto ExpectedCoverageR8(const AiMaskCoveragePixels&       pixels,
                        const MaskCoverageDisplayRequest& request) -> std::vector<std::uint8_t> {
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

/// The render device of one backend behind the production coverage operation.
class CoverageProducer {
 public:
  virtual ~CoverageProducer() = default;
  virtual auto Present(const MaskCoverageSelection&      selection,
                       const MaskCoverageDisplayRequest& request,
                       const MaskCoverageFrameIdentity& identity, IMaskCoverageSink& sink)
      -> MaskCoveragePublication              = 0;
  virtual auto Counters() -> ProducerCounters = 0;
};

template <class Device>
class DeviceCoverageProducer final : public CoverageProducer {
 public:
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

 private:
  Device device_{};
};

auto MakeProducer(EditorBackend backend) -> std::unique_ptr<CoverageProducer> {
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
auto MakeRhi(EditorBackend backend, std::unique_ptr<QOffscreenSurface>& surface)
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

/// Forwards to the viewport sink and switches the viewport to the next image after the device
/// write and before publication: a coverage frame that is in flight across an image change.
class ImageChangeBeforePublicationSink final : public IMaskCoverageSink {
 public:
  ImageChangeBeforePublicationSink(IMaskCoverageSink& sink, std::function<void()> change_image)
      : sink_(sink), change_image_(std::move(change_image)) {}

  auto MapCoverageTarget(const MaskCoveragePresentation& presentation, int width, int height,
                         FrameMemoryDomain domain) -> FrameWriteMapping override {
    return sink_.MapCoverageTarget(presentation, width, height, domain);
  }
  void UnmapCoverageTarget() override {
    sink_.UnmapCoverageTarget();
    change_image_();
  }
  auto PublishCoverage() -> MaskCoveragePublication override { return sink_.PublishCoverage(); }
  void AbandonCoverageTarget() override { sink_.AbandonCoverageTarget(); }
#if defined(HAVE_METAL)
  auto SubmitMetalCoverage(const MaskCoveragePresentation& presentation,
                           const MaskCoverageMetalFrame&   frame)
      -> MaskCoveragePublication override {
    change_image_();
    return sink_.SubmitMetalCoverage(presentation, frame);
  }
#endif

 private:
  IMaskCoverageSink&    sink_;
  std::function<void()> change_image_;
};

class MaskCoveragePresentationTest : public ::testing::Test {
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

  /// Run the production coverage operation on the render thread while this thread services the
  /// coverage queue as the viewport render thread does.
  auto Present(const MaskCoverageSelection& selection, const MaskCoverageDisplayRequest& request,
               MaskCoverageFrameIdentity identity, IMaskCoverageSink* sink = nullptr,
               ILeaseTargetAdapter* adapter = nullptr) -> MaskCoveragePublication {
    auto* target_sink = sink != nullptr ? sink : viewport_->maskCoverageSink();
    auto  done        = render_thread_.Post(
        [&] { return producer_->Present(selection, request, identity, *target_sink); });
    const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
    while (done.wait_for(1ms) != std::future_status::ready) {
      layer_.FulfillTargetRequests(rhi_.get(), adapter != nullptr ? *adapter : *adapter_,
                                   viewport_->sessionEpoch(), viewport_->imageIdentity());
      QCoreApplication::processEvents();
      if (std::chrono::steady_clock::now() > deadline) {
        viewport_->maskCoverageQueue()->Shutdown();  // Release the producer to report the failure.
        ADD_FAILURE() << "the coverage presentation did not finish";
        break;
      }
    }
    return done.get();
  }

  auto Consume() -> bool {
    return layer_.ConsumeNewest(rhi_.get(), viewport_->sessionEpoch(), viewport_->imageIdentity());
  }

  auto Counters() -> ProducerCounters {
    return render_thread_.Post([this] { return producer_->Counters(); }).get();
  }

  /// Test-only readback of an imported coverage texture as RGBA32F texels.
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

  static auto Candidate(const AiMaskCoveragePixels& pixels) -> MaskCoverageSelection {
    return MaskCoverageCandidateSelection{pixels, kAnalysis, {}};
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

auto SlotStates(const DirectPresentQueue& queue) -> std::vector<std::string> {
  std::vector<std::string> states;
  for (int i = 0; i < DirectPresentQueue::kSlotCount; ++i) {
    const auto slot = queue.SlotAt(i);
    states.push_back(std::to_string(static_cast<int>(slot->state)) + "/" +
                     std::to_string(slot->native.native_handle) + "/" +
                     std::to_string(slot->width) + "x" + std::to_string(slot->height) + "/" +
                     std::to_string(slot->session_epoch) + "/" +
                     std::to_string(slot->preview_metadata.presentation_request_id));
  }
  return states;
}

TEST_F(MaskCoveragePresentationTest, CoverageTransferLeavesPhotoSlotsUnchanged) {
  // A real photo target in the photo queue, as after the first photo frame.
  auto& photo_queue = *viewport_->present_queue();
  if (UsesQueuedTargets()) {
    const auto prepared = photo_queue.PrepareWrite(96, 64, kEpoch, kImage);
    ASSERT_TRUE(prepared.ok);
    auto photo =
        adapter_->CreateTarget(rhi_.get(), QSize(96, 64), TargetGeneration{1, kEpoch, 0, kImage},
                               LeaseFrameLayer::QualityBase);
    ASSERT_TRUE(photo.has_value()) << adapter_->lastError();
    DirectPresentQueue::SlotNative native;
    native.backend           = photo->backend;
    native.handle_kind       = photo->handle_kind;
    native.writable_kind     = photo->writable_kind;
    native.native_handle     = photo->native_handle;
    native.writable_resource = photo->writable_resource;
    ASSERT_TRUE(
        photo_queue.PublishCreatedSlot(prepared.slot_index, 96, 64, native, kEpoch, kImage));
  }
  const auto                 photo_slots_before  = SlotStates(photo_queue);
  const auto                 photo_diag_before   = photo_queue.DiagnosticsSnapshot();
  const auto                 photo_frames_before = viewport_->frameSink()->submitted_frame_count();

  const auto                 pixels              = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(11)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  ExpectDisplayed(ExpectedCoverageR8(pixels, request), "candidate coverage");
  EXPECT_EQ(layer_.Presentation().identity.request_id, 11u);
  EXPECT_EQ(layer_.Presentation().geometry.render_extent, request.geometry.render_extent);
  EXPECT_EQ(layer_.Texture()->pixelSize(),
            QSize(static_cast<int>(request.geometry.render_extent.width),
                  static_cast<int>(request.geometry.render_extent.height)));

  // The photo queue, its native target, and the photo sink are untouched.
  EXPECT_EQ(SlotStates(photo_queue), photo_slots_before);
  const auto photo_diag_after = photo_queue.DiagnosticsSnapshot();
  EXPECT_EQ(photo_diag_after.live_target_count, photo_diag_before.live_target_count);
  EXPECT_EQ(photo_diag_after.ready_count, 0u);
  EXPECT_EQ(photo_diag_after.dropped_stale_frame_count,
            photo_diag_before.dropped_stale_frame_count);
  EXPECT_EQ(viewport_->frameSink()->submitted_frame_count(), photo_frames_before);
  if (UsesQueuedTargets()) {
    EXPECT_EQ(layer_.NativeTargetCount(), 1u);
    EXPECT_GE(layer_.SlotIndex(), 0);
  }
}

TEST_F(MaskCoveragePresentationTest, OldCoverageLivesUntilRendererRelease) {
  const auto                 first  = MakeCandidatePixels(0);
  const auto                 second = MakeCandidatePixels(2);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(first), request, Identity(21)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  auto*      first_texture   = layer_.Texture();
  const int  first_slot      = layer_.SlotIndex();
  const auto imported_before = NativeResourceCounters::Instance().LiveImportedQRhiTextures();

  // The replacement completes, but the renderer has not taken it yet.
  ASSERT_EQ(Present(Candidate(second), request, Identity(22)), MaskCoveragePublication::Published);
  EXPECT_EQ(layer_.Texture(), first_texture);
  EXPECT_EQ(layer_.Presentation().identity.request_id, 21u);
  ExpectDisplayed(ExpectedCoverageR8(first, request), "displayed coverage before the import");
  if (UsesQueuedTargets()) {
    const auto& queue = *viewport_->maskCoverageQueue();
    EXPECT_EQ(queue.SlotAt(first_slot)->state, DirectPresentQueue::SlotState::RendererReading);
    EXPECT_EQ(queue.DiagnosticsSnapshot().ready_count, 1u);
    EXPECT_EQ(queue.DiagnosticsSnapshot().live_target_count, 2u)
        << "the replacement must use a second target";
  } else {
    // Metal: the displayed frame holds its display target, so the replacement used another one.
    EXPECT_EQ(Counters().display_targets, 2u);
  }

  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 22u);
  ExpectDisplayed(ExpectedCoverageR8(second, request), "replacement coverage");
  // The previous import was destroyed after the replacement was imported.
  EXPECT_EQ(NativeResourceCounters::Instance().LiveImportedQRhiTextures(), imported_before);
  if (UsesQueuedTargets()) {
    EXPECT_EQ(viewport_->maskCoverageQueue()->SlotAt(first_slot)->state,
              DirectPresentQueue::SlotState::Available);
    EXPECT_NE(layer_.SlotIndex(), first_slot);
  } else {
    // The renderer released the first frame: the next write reuses its display target.
    const auto creates = Counters().display_target_create;
    ASSERT_EQ(Present(Candidate(first), request, Identity(23)), MaskCoveragePublication::Published);
    EXPECT_EQ(Counters().display_target_create, creates);
    ASSERT_TRUE(Consume());
    ExpectDisplayed(ExpectedCoverageR8(first, request), "coverage in the reused target");
  }

  layer_.Release();
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_EQ(NativeResourceCounters::Instance().LiveImportedQRhiTextures(), imported_before - 1);
}

TEST_F(MaskCoveragePresentationTest, CoverageTransferDoesNotReadBackToHost) {
  const auto                 pixels = MakeCandidatePixels(1);
  MaskCoverageDisplayRequest plain{MakePhotoGeometry(1.0f), false, 1.0f};
  const auto                 start = Counters();
  ASSERT_EQ(Present(Candidate(pixels), plain, Identity(31)), MaskCoveragePublication::Published);
  const auto first = Counters();
  // The only host transfer is the original coverage upload into the device.
  EXPECT_EQ(first.device_to_host, start.device_to_host);
  EXPECT_EQ(first.host_to_device - start.host_to_device, 1u);

  // The same candidate with another invert and opacity: no upload and no readback.
  MaskCoverageDisplayRequest inverted{plain.geometry, true, 0.5f};
  ASSERT_EQ(Present(Candidate(pixels), inverted, Identity(32)), MaskCoveragePublication::Published);
  const auto second = Counters();
  EXPECT_EQ(second.device_to_host, start.device_to_host);
  EXPECT_EQ(second.host_to_device, first.host_to_device);
  ASSERT_TRUE(Consume());
  ExpectDisplayed(ExpectedCoverageR8(pixels, inverted), "inverted half-opacity coverage");
  if (UsesQueuedTargets()) {
    // The target is a device resource shared with the graphics API.
    const auto slot = viewport_->maskCoverageQueue()->SlotAt(layer_.SlotIndex());
    EXPECT_EQ(slot->native.writable_kind, g_backend == EditorBackend::Cuda
                                              ? LeaseWritableResourceKind::CudaArray
                                              : LeaseWritableResourceKind::OpenClImage);
    // CUDA and OpenCL copy into the native target and keep one display target.
    EXPECT_EQ(second.display_targets, 1u);
  }
}

TEST_F(MaskCoveragePresentationTest, CoverageChannelClosesBeforeImageReplacement) {
  const auto                 pixels = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(41)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  // A frame of the current image is in flight when the viewport switches image.
  ImageChangeBeforePublicationSink switching(*viewport_->maskCoverageSink(), [this] {
    QMetaObject::invokeMethod(
        viewport_.get(),
        [this] {
          viewport_->setImageIdentity(kNextImage);
          viewport_->setSessionEpoch(kNextEpoch);
        },
        Qt::BlockingQueuedConnection);
  });
  EXPECT_EQ(Present(Candidate(MakeCandidatePixels(2)), request, Identity(42), &switching),
            MaskCoveragePublication::DiscardedForImageChange);
  // What EditorViewportRenderer::synchronize does on the session change.
  layer_.Release();
  EXPECT_FALSE(Consume()) << "no frame of the previous image may be imported";
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_EQ(viewport_->maskCoverageQueue()->DiagnosticsSnapshot().ready_count, 0u);

  // A late frame for the previous image is refused before any device-to-target copy.
  EXPECT_EQ(Present(Candidate(pixels), request, Identity(43)),
            MaskCoveragePublication::DiscardedForImageChange);
  EXPECT_FALSE(Consume());

  // The new image starts with its own coverage.
  const auto next = MakeCandidatePixels(1);
  ASSERT_EQ(Present(Candidate(next), request, {44, kNextEpoch, kNextImage}),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.image_identity, kNextImage);
  ExpectDisplayed(ExpectedCoverageR8(next, request), "coverage of the next image");
}

TEST_F(MaskCoveragePresentationTest, CoverageDoesNotReplaceScopePhotoInput) {
  // Scope analysis reads the current final photo frame of the tap in front of the photo sink
  // (SubmitCurrentDisplayFrameToScope). The frame stands for the last presented photo; its image
  // is not read here.
  FinalDisplayFrameTapSink tap(viewport_->frameSink(), nullptr);
  tap.SetFrameIdentity(kImage, kEpoch);
  FinalDisplayFrameView photo;
  photo.image.resource = std::make_shared<int>(1);
  photo.image.width    = 96;
  photo.image.height   = 64;
  photo.width          = 96;
  photo.height         = 64;
  tap.SubmitFinalDisplayFrame(photo);
  const auto display_before = tap.GetCurrentDisplayFrameView();
  ASSERT_TRUE(static_cast<bool>(display_before));

  const auto                 pixels = MakeCandidatePixels(1);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(51)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  const auto display_after = tap.GetCurrentDisplayFrameView();
  EXPECT_EQ(display_after.frame_id, display_before.frame_id);
  EXPECT_EQ(display_after.image.resource, display_before.image.resource);
  EXPECT_EQ(display_after.width, 96);
  EXPECT_EQ(viewport_->frameSink()->submitted_frame_count(), 0u);
  EXPECT_EQ(viewport_->present_queue()->DiagnosticsSnapshot().ready_count, 0u);
}

TEST_F(MaskCoveragePresentationTest, FailedTargetCreationKeepsDisplayedCoverage) {
  if (!UsesQueuedTargets()) {
    GTEST_SKIP() << "Metal presents its own texture; the viewport creates no coverage target";
  }
  const auto                 pixels = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(61)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  // A resized photo frame needs a new target, and the render thread cannot create it.
  UnsupportedLeaseTargetAdapter failing(g_backend);
  MaskCoverageDisplayRequest    resized{MakePhotoGeometry(0.5f), false, 1.0f};
  EXPECT_THROW(
      (void)Present(Candidate(MakeCandidatePixels(2)), resized, Identity(62), nullptr, &failing),
      std::runtime_error);
  EXPECT_FALSE(layer_.LastError().empty());
  EXPECT_FALSE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 61u);
  ExpectDisplayed(ExpectedCoverageR8(pixels, request), "coverage kept after the failure");

  // The channel recovers with the next frame.
  ASSERT_EQ(Present(Candidate(pixels), resized, Identity(63)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  ExpectDisplayed(ExpectedCoverageR8(pixels, resized), "coverage after the recovery");
}

}  // namespace
}  // namespace alcedo::editor_rhi

int main(int argc, char** argv) {
#ifdef _WIN32
  // Native GPU devices need the Windows QPA; offscreen has no OpenGL context for OpenCL sharing.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArray("windows"));
  }
#endif
  alcedo::ui::test::ConfigureQtPluginPaths(argc > 0 ? argv[0] : nullptr,
                                           /*force_offscreen=*/false);
  using alcedo::editor_rhi::EditorBackend;
  const QByteArray requested = qgetenv("ALCEDO_TEST_EDITOR_BACKEND").toLower();
#if defined(__APPLE__)
  alcedo::editor_rhi::g_backend = EditorBackend::Metal;
#else
  alcedo::editor_rhi::g_backend =
      requested == "opencl" ? EditorBackend::OpenCl : EditorBackend::Cuda;
#endif
  if (alcedo::editor_rhi::g_backend == EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }
  QGuiApplication app(argc, argv);
  alcedo::editor_rhi::g_startup =
      alcedo::editor_rhi::ApplyEditorBackendBeforeWindow(alcedo::editor_rhi::g_backend);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
