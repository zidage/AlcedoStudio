//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file viewport_coverage_composite_test.cpp
/// @brief SM8.6: the one viewport composite of the photo and the Mask coverage.
///
/// Each test draws the production ViewportCompositePass (the editor_viewport shaders and the
/// uniforms of BuildViewportCompositeUniforms) on the real QRhi of one backend, with coverage
/// frames from the production coverage channel (PresentMaskCoverage, the viewport coverage sink,
/// and ViewportMaskCoverageLayer) and, where named, photo frames from the production photo path
/// (the photo DAG on the same device, FramePresenter, DirectFrameSink, and the photo queue). The
/// expected pixels and sample positions are computed by the test from the composite equation and
/// the ResolveRenderGeometry matrices, without the shader or the uniform builder.

#include <QColor>
#include <QSize>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../edit/graph/test_camera_profile.hpp"
#include "../edit/input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "mask_coverage_presentation_test_support.hpp"
#include "ui/editor_rhi/viewport_composite_pass.hpp"

namespace alcedo::editor_rhi {
namespace {

using namespace coverage_test;

/// Display-encoded test tint and opacity, distinct from every photo value of the tests.
constexpr MaskCoverageDisplayStyle kTint{true, 0.2f, 0.9f, 0.4f, 0.6f};

auto PhotoPixel(const std::vector<float>& rgba, std::size_t index) -> std::array<float, 4> {
  return {rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2], rgba[index * 4 + 3]};
}

/// The composite equation of section 5.3 for one pixel, in double precision.
auto ExpectedComposite(std::array<float, 4> photo, double coverage,
                       const MaskCoverageDisplayStyle& style) -> std::array<double, 4> {
  const double a = std::clamp(coverage, 0.0, 1.0) * style.opacity;
  if (a <= 0.0) {
    return {photo[0], photo[1], photo[2], photo[3]};
  }
  return {photo[0] * (1.0 - a) + style.red * a, photo[1] * (1.0 - a) + style.green * a,
          photo[2] * (1.0 - a) + style.blue * a, photo[3]};
}

/// Bilinear sample of the red channel of an RGBA32F texture at @p uv with the GPU texel-center
/// convention and clamp-to-edge; zero outside [0, 1] (the coverage frame has no texels there).
auto SampleCoverage(const std::vector<float>& texels, Extent2D extent, double u, double v)
    -> double {
  if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
    return 0.0;
  }
  const double x  = u * extent.width - 0.5;
  const double y  = v * extent.height - 0.5;
  const auto   x0 = static_cast<int>(std::floor(x));
  const auto   y0 = static_cast<int>(std::floor(y));
  const double fx = x - x0;
  const double fy = y - y0;
  const auto   at = [&](int xi, int yi) {
    xi = std::clamp(xi, 0, static_cast<int>(extent.width) - 1);
    yi = std::clamp(yi, 0, static_cast<int>(extent.height) - 1);
    return static_cast<double>(texels[(static_cast<std::size_t>(yi) * extent.width + xi) * 4]);
  };
  return (at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx) * (1.0 - fy) +
         (at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

/// Candidate coverage of a soft disc of radius 12 at (@p cx, @p cy), zero elsewhere.
auto MakeDiscPixels(double cx, double cy) -> AiMaskCoveragePixels {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAnalysis.width) * kAnalysis.height);
  for (std::uint32_t y = 0; y < kAnalysis.height; ++y) {
    for (std::uint32_t x = 0; x < kAnalysis.width; ++x) {
      const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
      pixels[static_cast<std::size_t>(y) * kAnalysis.width + x] =
          static_cast<std::uint8_t>(std::lround(255.0 * std::clamp((12.0 - d) / 3.0, 0.0, 1.0)));
    }
  }
  return std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
}

/// A whole-image AI Mask asset with the pixels of @p candidate.
auto WholeImageAsset(const AiMaskCoveragePixels& candidate) -> MaskAsset {
  MaskAsset asset;
  asset.descriptor = AiMaskCandidateDescriptor(kAnalysis);
  asset.pixels     = *candidate;
  asset.key        = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

/// The document of the photo tests: the default pipeline with the test camera profile and a
/// primary Grade exposure, so that a Mask of the Grade changes the photo.
auto MakePhotoDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  exposure->SetValue(1.5f);
  return document;
}

auto MakePhotoInput() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeF32RgbaPlane(kAnalysis.width, kAnalysis.height),
      gpu_dag_test::FullSensor(kAnalysis.width, kAnalysis.height));
}

auto PhotoSubmission(std::uint64_t request_id) -> FrameCompletionSubmission {
  FrameCompletionSubmission submission;
  submission.metadata.frame_role              = FrameRole::InteractivePrimary;
  submission.metadata.presentation_request_id = request_id;
  submission.metadata.session_epoch           = kEpoch;
  submission.metadata.image_identity          = kImage;
  submission.mode                             = FramePresentationMode::FullFrame;
  return submission;
}

auto PassCounters(const GpuNodePassStats& stats) -> std::vector<std::uint64_t> {
  return {stats.sensor_develop_execute,
          stats.sensor_develop_skip,
          stats.geometry_execute,
          stats.geometry_skip,
          stats.camera_color_execute,
          stats.camera_color_skip,
          stats.mask_execute,
          stats.mask_skip,
          stats.mask_union_execute,
          stats.mask_union_skip,
          stats.primary_grade_execute,
          stats.primary_grade_skip,
          stats.drt_execute,
          stats.drt_skip,
          stats.source_h2d_count};
}

/// A photo frame that the fixture imported from the viewport photo path, as
/// EditorViewportRenderer does.
struct ImportedPhoto {
  QRhiTexture*                texture = nullptr;
  int                         width   = 0;
  int                         height  = 0;
  FramePresentationMode       mode    = FramePresentationMode::FullFrame;
  ResolvedRenderGeometry      geometry{};
  int                         slot = -1;
  std::shared_ptr<const void> owner{};
};

class ViewportCoverageCompositeTest : public CoverageChannelTest {
 protected:
  void ReleaseBeforeTeardown() override {
    pass_.Release();
    for (auto& [size, target] : targets_) {
      delete target.render_target;
      delete target.pass;
      delete target.texture;
    }
    targets_.clear();
    for (auto* texture : uploads_) {
      delete texture;
    }
    uploads_.clear();
    for (auto& photo : photos_) {
      ReleasePhoto(photo);
    }
    photos_.clear();
    auto& queue = *viewport_->present_queue();
    queue.InvalidateTargetGeneration();
    (void)queue.DrainReleasedNatives();
    for (const auto& lease : photo_natives_) {
      adapter_->DestroyTarget(lease);
    }
    photo_natives_.clear();
  }

  /// An RGBA32F texture with @p rgba (top row first).
  auto Upload(int width, int height, const std::vector<float>& rgba) -> QRhiTexture* {
    auto* texture = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(width, height), 1);
    EXPECT_TRUE(texture->create());
    uploads_.push_back(texture);
    QRhiCommandBuffer* command_buffer = nullptr;
    EXPECT_EQ(rhi_->beginOffscreenFrame(&command_buffer), QRhi::FrameOpSuccess);
    QRhiTextureSubresourceUploadDescription description(QByteArray(
        reinterpret_cast<const char*>(rgba.data()), static_cast<int>(rgba.size() * sizeof(float))));
    description.setDataStride(static_cast<quint32>(width * 4 * sizeof(float)));
    description.setSourceSize(QSize(width, height));
    auto* batch = rhi_->nextResourceUpdateBatch();
    batch->uploadTexture(texture,
                         QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));
    command_buffer->resourceUpdate(batch);
    rhi_->endOffscreenFrame();
    return texture;
  }

  /**
   * @brief Draw @p pass (the production pass by default) into an RGBA32F render target of
   *        @p size and read it back, top row first.
   */
  auto Composite(QSize size, const ViewportCompositePass::Textures& textures,
                 const ViewportCompositeUniforms& uniforms,
                 ViewportCompositeOutcome* outcome = nullptr, ViewportCompositePass* pass = nullptr)
      -> std::vector<float> {
    auto&              target         = TargetOfSize(size);
    QRhiCommandBuffer* command_buffer = nullptr;
    if (rhi_->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess) {
      ADD_FAILURE() << "beginOffscreenFrame failed";
      return {};
    }
    const auto result =
        (pass != nullptr ? *pass : pass_)
            .Record(rhi_.get(), command_buffer, target.render_target, textures, uniforms);
    if (outcome != nullptr) {
      *outcome = result;
    }
    QRhiReadbackResult readback;
    auto*              batch = rhi_->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(target.texture), &readback);
    command_buffer->resourceUpdate(batch);
    rhi_->endOffscreenFrame();
    std::vector<float> pixels(static_cast<std::size_t>(readback.data.size()) / sizeof(float));
    std::memcpy(pixels.data(), readback.data.constData(), pixels.size() * sizeof(float));
    if (rhi_->isYUpInFramebuffer()) {
      // OpenGL stores the bottom row of the framebuffer first.
      const std::size_t row = static_cast<std::size_t>(size.width()) * 4;
      for (int y = 0; y < size.height() / 2; ++y) {
        std::swap_ranges(pixels.begin() + y * row, pixels.begin() + (y + 1) * row,
                         pixels.begin() + (size.height() - 1 - y) * row);
      }
    }
    return pixels;
  }

  /// The uniforms of the viewport for @p photo with the displayed coverage and @p style.
  auto UniformsFor(const ImportedPhoto& photo, const MaskCoverageDisplayStyle& style)
      -> ViewportCompositeUniforms {
    ViewportCompositeLayout layout;
    layout.target_size = QSize(photo.width, photo.height);
    layout.primary     = {photo.width, photo.height, photo.mode, photo.geometry};
    layout.coverage_geometry =
        layer_.Texture() != nullptr ? &layer_.Presentation().geometry : nullptr;
    layout.coverage_style = style;
    return BuildViewportCompositeUniforms(layout);
  }

  auto CompositeFor(const ImportedPhoto& photo, const MaskCoverageDisplayStyle& style)
      -> std::vector<float> {
    return Composite(QSize(photo.width, photo.height), {photo.texture, nullptr, layer_.Texture()},
                     UniformsFor(photo, style));
  }

  /// Render the photo DAG on the producer device and present it to @p sink, while this thread
  /// creates the photo targets as the viewport render thread does.
  auto RenderPhoto(const PipelineDocument& document, const PreparedRawInput& input,
                   const RenderRequest& request, IFrameSink& sink,
                   const FrameCompletionSubmission& submission) -> PhotoRender {
    return RunOnRenderThread(
        [&] { return producer_->RenderPhoto(document, input, request, sink, submission); }, nullptr,
        [this] { ServicePhotoTargets(); });
  }

  /// EditorViewportRenderer::fulfillTargetRequests for the photo queue (CUDA, OpenCL).
  void ServicePhotoTargets() {
    if (!UsesQueuedTargets()) {
      return;
    }
    auto&      queue = *viewport_->present_queue();
    const auto epoch = viewport_->sessionEpoch();
    const auto image = viewport_->imageIdentity();
    for (const auto& request : queue.DrainSizeRequests()) {
      if (!request.valid()) {
        continue;
      }
      const auto prepare = queue.PrepareWrite(request.width, request.height, epoch, image);
      if (prepare.ok && !prepare.need_create) {
        continue;
      }
      auto lease = adapter_->CreateTarget(
          rhi_.get(), QSize(request.width, request.height),
          TargetGeneration{queue.CurrentTargetGeneration(), epoch, request.layer_generation, image},
          LeaseFrameLayer::InteractivePrimary);
      if (!lease.has_value()) {
        ADD_FAILURE() << "photo target creation failed: " << adapter_->lastError();
        queue.FailSizeRequest(request);
        continue;
      }
      DirectPresentQueue::SlotNative native;
      native.backend           = lease->backend;
      native.handle_kind       = lease->handle_kind;
      native.writable_kind     = lease->writable_kind;
      native.native_handle     = lease->native_handle;
      native.writable_resource = lease->writable_resource;
      native.sync_object       = lease->sync_object;
      native.sync_value        = lease->sync_value;
      native.adapter_cookie    = lease->native_handle;
      bool published =
          prepare.slot_index >= 0 && queue.PublishCreatedSlot(prepare.slot_index, request.width,
                                                              request.height, native, epoch, image);
      for (int i = 0; !published && i < DirectPresentQueue::kSlotCount; ++i) {
        published =
            queue.PublishCreatedSlot(i, request.width, request.height, native, epoch, image);
      }
      if (!published) {
        adapter_->DestroyTarget(*lease);
        queue.FailSizeRequest(request);
        continue;
      }
      photo_natives_.push_back(*lease);
    }
  }

  /// Import the newest InteractivePrimary photo frame with its geometry, as
  /// EditorViewportRenderer::consumeDirectFrames / consumeImportedGpuFrames do.
  auto ImportPhoto() -> ImportedPhoto& {
    ImportedPhoto photo;
    const auto    epoch = viewport_->sessionEpoch();
    const auto    image = viewport_->imageIdentity();
    if (UsesQueuedTargets()) {
      const auto frame = viewport_->present_queue()->ConsumeNewestReady(
          FrameRole::InteractivePrimary, epoch, image);
      if (frame.has_value()) {
        photo.texture =
            rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame->slot.width, frame->slot.height), 1);
        EXPECT_TRUE(
            photo.texture->createFrom({static_cast<quint64>(frame->slot.native.native_handle), 0}));
        photo.width    = frame->slot.width;
        photo.height   = frame->slot.height;
        photo.mode     = frame->slot.presentation_mode;
        photo.geometry = frame->slot.geometry;
        photo.slot     = frame->slot.index;
      }
    } else {
      auto frames = viewport_->frameSink()->DrainPendingImportedFrames(epoch, image);
      for (auto& frame : frames) {
        if (frame.preview_metadata.frame_role != FrameRole::InteractivePrimary) {
          continue;
        }
        photo.texture = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame.width, frame.height), 1);
        EXPECT_TRUE(photo.texture->createFrom(
            {static_cast<quint64>(frame.texture_handle), frame.native_layout}));
        photo.width    = frame.width;
        photo.height   = frame.height;
        photo.mode     = frame.presentation_mode;
        photo.geometry = frame.geometry;
        photo.owner    = std::move(frame.owner);
      }
    }
    photos_.push_back(std::move(photo));
    return photos_.back();
  }

  void ReleasePhoto(ImportedPhoto& photo) {
    pass_.ForgetTexture(photo.texture);
    delete photo.texture;
    photo.texture = nullptr;
    if (photo.slot >= 0) {
      if (g_backend == EditorBackend::OpenCl) {
        FinishOpenGlBeforeOpenClSharedTextureReuse();
      }
      viewport_->present_queue()->CompleteRendererRead(photo.slot);
      photo.slot = -1;
    }
    photo.owner.reset();
  }

  ViewportCompositePass pass_;

 private:
  struct RenderTarget {
    QRhiTexture*              texture       = nullptr;
    QRhiTextureRenderTarget*  render_target = nullptr;
    QRhiRenderPassDescriptor* pass          = nullptr;
  };

  /// One render target per size for the fixture lifetime: the pass keeps a pipeline for the
  /// render pass descriptor of its target, as the viewport keeps its window target.
  auto TargetOfSize(QSize size) -> RenderTarget& {
    auto& target = targets_[{size.width(), size.height()}];
    if (target.texture == nullptr) {
      target.texture =
          rhi_->newTexture(QRhiTexture::RGBA32F, size, 1,
                           QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
      EXPECT_TRUE(target.texture->create());
      target.render_target = rhi_->newTextureRenderTarget({QRhiColorAttachment(target.texture)});
      target.pass          = target.render_target->newCompatibleRenderPassDescriptor();
      target.render_target->setRenderPassDescriptor(target.pass);
      EXPECT_TRUE(target.render_target->create());
    }
    return target;
  }

  std::map<std::pair<int, int>, RenderTarget> targets_;
  std::vector<QRhiTexture*>                   uploads_;
  std::deque<ImportedPhoto>                   photos_;
  std::vector<WritableTargetLease>            photo_natives_;
};

TEST(ViewportCoverageMapping, PhotoUvMapsToCoverageUvThroughTheReferenceSpace) {
  const auto full = MakePhotoGeometry(1.0f);
  // The same frame: identity.
  const auto same = MapPhotoUvToCoverageUv(full, full);
  ASSERT_TRUE(same.has_value());
  EXPECT_NEAR(same->u_row[0], 1.0f, 1e-5f);
  EXPECT_NEAR(same->u_row[1], 0.0f, 1e-5f);
  EXPECT_NEAR(same->u_row[2], 0.0f, 1e-5f);
  EXPECT_NEAR(same->v_row[0], 0.0f, 1e-5f);
  EXPECT_NEAR(same->v_row[1], 1.0f, 1e-5f);
  EXPECT_NEAR(same->v_row[2], 0.0f, 1e-5f);

  // A ROI photo frame maps each of its corners to the reference point of that corner.
  const ViewRequest roi_view{NormalizedRect{0.3f, 0.25f, 0.4f, 0.5f}, Extent2D{64, 48}};
  const auto        roi = MakePhotoGeometry(1.0f, roi_view);
  const auto        map = MapPhotoUvToCoverageUv(roi, full);
  ASSERT_TRUE(map.has_value());
  for (const auto& [u, v] :
       std::array<std::pair<float, float>, 3>{{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.4f, 0.8f}}}) {
    const auto reference = TransformPoint(
        roi.render_to_reference, Vector2{u * static_cast<float>(roi.render_extent.width),
                                         v * static_cast<float>(roi.render_extent.height)});
    const auto expected = TransformPoint(full.reference_to_render, reference);
    EXPECT_NEAR((map->u_row[0] * u + map->u_row[1] * v + map->u_row[2]) * full.render_extent.width,
                expected.x, 1e-3f);
    EXPECT_NEAR((map->v_row[0] * u + map->v_row[1] * v + map->v_row[2]) * full.render_extent.height,
                expected.y, 1e-3f);
  }

  // Frames of different source images have no common reference space.
  auto other                  = full;
  other.full_reference_extent = Extent2D{128, 96};
  EXPECT_FALSE(MapPhotoUvToCoverageUv(full, other).has_value());
  EXPECT_FALSE(MapPhotoUvToCoverageUv(ResolvedRenderGeometry{}, full).has_value());
}

TEST_F(ViewportCoverageCompositeTest, CandidateAndCommittedCoverageUseSameComposite) {
  const auto                 pixels = MakeCandidatePixels(0);
  const auto                 asset  = WholeImageAsset(pixels);
  const AiMaskEdgeAdjustment edge{1.5f, 1};
  auto                       reader = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  render_thread_.Post([&] { producer_->SetMaskAssets(reader); }).get();
  const auto committed = std::get<AiSegmentationMaskSource>(
      ai_mask_test::MakeAiMask(asset, kAnalysis, "mask.ai", edge).source);

  const MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  const auto                       extent = request.geometry.render_extent;
  // A photo frame of the coverage frame's geometry with values in [0, 1].
  std::vector<float> photo_rgba(static_cast<std::size_t>(extent.width) * extent.height * 4);
  for (std::uint32_t y = 0; y < extent.height; ++y) {
    for (std::uint32_t x = 0; x < extent.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * extent.width + x) * 4;
      photo_rgba[i]       = static_cast<float>(x) / extent.width;
      photo_rgba[i + 1]   = static_cast<float>(y) / extent.height;
      photo_rgba[i + 2]   = 0.5f;
      photo_rgba[i + 3]   = 1.0f;
    }
  }
  ImportedPhoto photo;
  photo.texture =
      Upload(static_cast<int>(extent.width), static_cast<int>(extent.height), photo_rgba);
  photo.width    = static_cast<int>(extent.width);
  photo.height   = static_cast<int>(extent.height);
  photo.geometry = request.geometry;

  ASSERT_EQ(Present(Candidate(pixels, edge), request, Identity(71)),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto candidate_texels = ReadBack(layer_.Texture());
  const auto candidate        = CompositeFor(photo, kTint);

  ASSERT_EQ(Present(MaskCoverageSelection{committed}, request, Identity(72)),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 72u);
  const auto committed_composite = CompositeFor(photo, kTint);

  // Identical effective coverage gives identical viewport pixels.
  ASSERT_EQ(candidate.size(), photo_rgba.size());
  ASSERT_EQ(committed_composite.size(), candidate.size());
  EXPECT_EQ(
      std::memcmp(candidate.data(), committed_composite.data(), candidate.size() * sizeof(float)),
      0)
      << "the candidate and the committed Mask composite differently";

  // Both follow the composite equation at every pixel of the 1:1 draw.
  int tinted   = 0;
  int failures = 0;
  for (std::size_t i = 0; i < candidate.size() / 4 && failures < 8; ++i) {
    const auto expected =
        ExpectedComposite(PhotoPixel(photo_rgba, i), candidate_texels[i * 4], kTint);
    for (int c = 0; c < 4; ++c) {
      if (std::abs(candidate[i * 4 + c] - expected[c]) > 1e-5) {
        ADD_FAILURE() << "pixel " << i << " channel " << c << " = " << candidate[i * 4 + c]
                      << ", expected " << expected[c];
        ++failures;
      }
    }
    tinted += candidate_texels[i * 4] > 0.5f ? 1 : 0;
  }
  EXPECT_GT(tinted, 100) << "the coverage must cover a part of the frame";
}

TEST_F(ViewportCoverageCompositeTest, OverlayToggleDoesNotExecutePhotoDag) {
  const auto document = MakePhotoDocument();
  const auto input    = MakePhotoInput();
  const auto render =
      RenderPhoto(document, input, RenderRequest{}, *viewport_->frameSink(), PhotoSubmission(81));
  auto& photo = ImportPhoto();
  ASSERT_NE(photo.texture, nullptr) << "the photo frame did not reach the viewport";
  // The photo layer carries the geometry of its frame to the render thread.
  EXPECT_EQ(photo.geometry.render_extent, render.geometry.render_extent);
  EXPECT_EQ(photo.geometry.full_reference_extent, render.geometry.full_reference_extent);

  const MaskCoverageDisplayRequest request{photo.geometry, false, 1.0f};
  ASSERT_EQ(Present(Candidate(MakeCandidatePixels(1)), request, Identity(82)),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto coverage_texels = ReadBack(layer_.Texture());
  const auto photo_pixels    = ReadBack(photo.texture);

  viewport_->setMaskCoverageColor(QColor::fromRgbF(0.9f, 0.2f, 0.1f));
  viewport_->setMaskCoverageOpacity(0.5);
  int style_changes = 0;
  QObject::connect(viewport_.get(), &EditorViewportItem::MaskCoverageStyleChanged,
                   [&] { ++style_changes; });

  const auto pass_before =
      PassCounters(render_thread_.Post([this] { return producer_->PassStats(); }).get());
  const auto producer_before    = Counters();
  const auto frames_before      = viewport_->frameSink()->submitted_frame_count();
  const auto adjustments_before = viewport_->adjustmentFrameRequestCount();
  const auto photo_diag_before  = viewport_->present_queue()->DiagnosticsSnapshot();

  const auto shown              = CompositeFor(photo, viewport_->maskCoverageStyle());
  viewport_->setMaskCoverageVisible(false);
  const auto hidden = CompositeFor(photo, viewport_->maskCoverageStyle());
  viewport_->setMaskCoverageVisible(true);
  viewport_->setMaskCoverageVisible(true);  // No change: no notification.
  const auto shown_again = CompositeFor(photo, viewport_->maskCoverageStyle());
  QCoreApplication::processEvents();

  EXPECT_EQ(style_changes, 2);
  // Hidden coverage leaves the photo pixels exactly.
  ASSERT_EQ(hidden.size(), photo_pixels.size());
  EXPECT_EQ(std::memcmp(hidden.data(), photo_pixels.data(), hidden.size() * sizeof(float)), 0);
  EXPECT_EQ(std::memcmp(shown.data(), shown_again.data(), shown.size() * sizeof(float)), 0);
  const MaskCoverageDisplayStyle style = viewport_->maskCoverageStyle();
  EXPECT_FLOAT_EQ(style.opacity, 0.5f);
  int tinted = 0;
  for (std::size_t i = 0; i < shown.size() / 4; ++i) {
    const auto expected =
        ExpectedComposite(PhotoPixel(photo_pixels, i), coverage_texels[i * 4], style);
    for (int c = 0; c < 4; ++c) {
      EXPECT_NEAR(shown[i * 4 + c], expected[c], 1e-5) << "pixel " << i << " channel " << c;
    }
    tinted += coverage_texels[i * 4] > 0.5f ? 1 : 0;
  }
  EXPECT_GT(tinted, 50);

  // No photo DAG pass, render, transfer, or photo frame happened for the style changes.
  EXPECT_EQ(PassCounters(render_thread_.Post([this] { return producer_->PassStats(); }).get()),
            pass_before);
  const auto producer_after = Counters();
  EXPECT_EQ(producer_after.host_to_device, producer_before.host_to_device);
  EXPECT_EQ(producer_after.device_to_host, producer_before.device_to_host);
  EXPECT_EQ(producer_after.display_target_create, producer_before.display_target_create);
  EXPECT_EQ(viewport_->frameSink()->submitted_frame_count(), frames_before);
  EXPECT_EQ(viewport_->adjustmentFrameRequestCount(), adjustments_before);
  const auto photo_diag_after = viewport_->present_queue()->DiagnosticsSnapshot();
  EXPECT_EQ(photo_diag_after.ready_count, 0u);
  EXPECT_EQ(photo_diag_after.live_target_count, photo_diag_before.live_target_count);
  EXPECT_TRUE(viewport_->present_queue()->DrainSizeRequests().empty());
  EXPECT_EQ(viewport_->maskCoverageQueue()->DiagnosticsSnapshot().ready_count, 0u);
}

TEST_F(ViewportCoverageCompositeTest, CoverageFollowsCropRotationZoomAndDetailRoi) {
  const auto                       pixels = MakeDiscPixels(30.0, 22.0);
  // Coverage on the full-scale frame; the photo frames use other grids of the same image.
  const MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(91)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto                     coverage_texels = ReadBack(layer_.Texture());
  const auto                     coverage_extent = layer_.Presentation().geometry.render_extent;
  const auto                     coverage_geom   = layer_.Presentation().geometry;

  constexpr std::array<float, 4> kBase           = {0.3f, 0.3f, 0.3f, 1.0f};
  constexpr std::array<float, 4> kDetail         = {0.7f, 0.1f, 0.5f, 1.0f};
  const QSize                    target(96, 80);

  struct Case {
    std::string                 label;
    ResolvedRenderGeometry      photo;
    FramePresentationMode       mode;
    float                       zoom;
    float                       pan_x;
    float                       pan_y;
    std::optional<FrameRoiRect> detail;
  };
  const std::vector<Case> cases = {
      {"half-scale full frame, zoom 1.6, pan, detail ROI", MakePhotoGeometry(0.5f),
       FramePresentationMode::FullFrame, 1.6f, 0.2f, -0.15f,
       FrameRoiRect{0.35f, 0.3f, 0.3f, 0.35f}},
      {"viewport ROI frame",
       MakePhotoGeometry(1.0f,
                         ViewRequest{NormalizedRect{0.3f, 0.25f, 0.4f, 0.5f}, Extent2D{96, 80}}),
       FramePresentationMode::RoiFrame, 2.5f, 0.4f, 0.3f, std::nullopt},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.label);
    const auto         photo_extent = test_case.photo.render_extent;
    std::vector<float> base_rgba(static_cast<std::size_t>(photo_extent.width) *
                                 photo_extent.height * 4);
    for (std::size_t i = 0; i < base_rgba.size(); i += 4) {
      std::copy(kBase.begin(), kBase.end(), base_rgba.begin() + static_cast<std::ptrdiff_t>(i));
    }
    auto* base = Upload(static_cast<int>(photo_extent.width), static_cast<int>(photo_extent.height),
                        base_rgba);
    std::vector<float> detail_rgba(16 * 16 * 4);
    for (std::size_t i = 0; i < detail_rgba.size(); i += 4) {
      std::copy(kDetail.begin(), kDetail.end(),
                detail_rgba.begin() + static_cast<std::ptrdiff_t>(i));
    }
    auto* detail = test_case.detail.has_value() ? Upload(16, 16, detail_rgba) : nullptr;

    ViewportCompositeLayout layout;
    layout.target_size = target;
    layout.primary = {static_cast<int>(photo_extent.width), static_cast<int>(photo_extent.height),
                      test_case.mode, test_case.photo};
    layout.zoom    = test_case.zoom;
    layout.pan_x   = test_case.pan_x;
    layout.pan_y   = test_case.pan_y;
    layout.detail_roi        = test_case.detail;
    layout.coverage_geometry = &coverage_geom;
    layout.coverage_style    = kTint;
    const auto output =
        Composite(target, {base, detail, layer_.Texture()}, BuildViewportCompositeUniforms(layout));
    ASSERT_EQ(output.size(), static_cast<std::size_t>(target.width()) * target.height() * 4);

    // Independent mapping: target pixel -> quad position -> photo UV -> photo render pixel ->
    // reference -> coverage render pixel -> coverage UV.
    const double aspect_target = static_cast<double>(target.width()) / target.height();
    const double aspect_photo  = static_cast<double>(photo_extent.width) / photo_extent.height;
    const double letter_x      = aspect_photo > aspect_target ? 1.0 : aspect_photo / aspect_target;
    const double letter_y      = aspect_photo > aspect_target ? aspect_target / aspect_photo : 1.0;
    const bool   roi_frame     = test_case.mode == FramePresentationMode::RoiFrame;
    const double zoom          = roi_frame ? 1.0 : test_case.zoom;
    const double pan_x         = roi_frame ? 0.0 : test_case.pan_x;
    const double pan_y         = roi_frame ? 0.0 : test_case.pan_y;
    int          checked = 0, tinted = 0, untinted = 0, failures = 0;
    for (int py = 0; py < target.height(); ++py) {
      for (int px = 0; px < target.width(); ++px) {
        const double ndc_x = (px + 0.5) / target.width() * 2.0 - 1.0;
        const double ndc_y = 1.0 - (py + 0.5) / target.height() * 2.0;
        const double qx    = (ndc_x - pan_x) / (letter_x * zoom);
        const double qy    = (ndc_y - pan_y) / (letter_y * zoom);
        const auto   index = static_cast<std::size_t>(py) * target.width() + px;
        if (std::abs(qx) > 0.999 || std::abs(qy) > 0.999) {
          if (std::abs(qx) > 1.001 || std::abs(qy) > 1.001) {
            // Outside the photo quad: the cleared target.
            EXPECT_EQ(output[index * 4 + 0], 0.0f);
            EXPECT_EQ(output[index * 4 + 3], 1.0f);
          }
          continue;
        }
        const double u           = (qx + 1.0) / 2.0;
        const double v           = (1.0 - qy) / 2.0;
        auto         photo_color = kBase;
        if (test_case.detail.has_value()) {
          const auto&  roi = *test_case.detail;
          const double du = u - roi.x, dv = v - roi.y;
          const bool   near_edge = std::abs(du) < 1e-3 || std::abs(dv) < 1e-3 ||
                                 std::abs(du - roi.width) < 1e-3 ||
                                 std::abs(dv - roi.height) < 1e-3;
          if (near_edge) {
            continue;
          }
          if (du > 0.0 && dv > 0.0 && du < roi.width && dv < roi.height) {
            photo_color = kDetail;
          }
        }
        const auto   reference   = TransformPoint(test_case.photo.render_to_reference,
                                                  Vector2{static_cast<float>(u * photo_extent.width),
                                                      static_cast<float>(v * photo_extent.height)});
        const auto   at_coverage = TransformPoint(coverage_geom.reference_to_render, reference);
        const double cu          = at_coverage.x / coverage_extent.width;
        const double cv          = at_coverage.y / coverage_extent.height;
        if (std::abs(cu) < 1e-3 || std::abs(cv) < 1e-3 || std::abs(cu - 1.0) < 1e-3 ||
            std::abs(cv - 1.0) < 1e-3) {
          continue;  // On the edge of the coverage frame.
        }
        const double coverage = SampleCoverage(coverage_texels, coverage_extent, cu, cv);
        const auto   expected = ExpectedComposite(photo_color, coverage, kTint);
        ++checked;
        tinted += coverage > 0.5 ? 1 : 0;
        untinted += coverage == 0.0 ? 1 : 0;
        for (int c = 0; c < 4 && failures < 10; ++c) {
          if (std::abs(output[index * 4 + c] - expected[c]) > 0.01) {
            ADD_FAILURE() << "target pixel (" << px << ", " << py << ") channel " << c << " = "
                          << output[index * 4 + c] << ", expected " << expected[c]
                          << " (coverage UV " << cu << ", " << cv << ")";
            ++failures;
          }
        }
      }
    }
    EXPECT_GT(checked, 1000);
    EXPECT_GT(tinted, 100) << "covered pixels must be checked";
    EXPECT_GT(untinted, 100) << "uncovered pixels must be checked";
  }
}

TEST_F(ViewportCoverageCompositeTest, CoverageTintNeverAppearsInExportOrScopes) {
  const auto pixels = MakeCandidatePixels(0);
  const auto asset  = WholeImageAsset(pixels);
  auto       reader = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  render_thread_.Post([&] { producer_->SetMaskAssets(reader); }).get();
  const auto               input = MakePhotoInput();

  FinalDisplayFrameTapSink tap(viewport_->frameSink(), nullptr);
  tap.SetFrameIdentity(kImage, kEpoch);

  // Without the Mask: the reference for the Mask effect.
  auto       document = MakePhotoDocument();
  const auto unmasked = RenderPhoto(document, input, RenderRequest{}, tap, PhotoSubmission(101));
  ReleasePhoto(ImportPhoto());

  // The Mask restricts the exposure of the Grade.
  const auto mask = ai_mask_test::MakeAiMask(asset, kAnalysis, "mask.ai");
  grade_mask_test::AddMask(*document.PrimaryGrade(), mask);
  document.MarkTopologyChanged();
  const auto first       = RenderPhoto(document, input, RenderRequest{}, tap, PhotoSubmission(102));
  const auto scope_first = tap.GetCurrentDisplayFrameView();
  ASSERT_TRUE(static_cast<bool>(scope_first));
  auto& photo = ImportPhoto();
  ASSERT_NE(photo.texture, nullptr);
  ASSERT_EQ(first.rgba.size(), unmasked.rgba.size());
  EXPECT_NE(first.rgba, unmasked.rgba) << "export must apply the Mask to the photo";

  // The committed Mask is displayed with a visible tint.
  const auto& source = std::get<AiSegmentationMaskSource>(mask.source);
  ASSERT_EQ(Present(MaskCoverageSelection{source}, {photo.geometry, false, 1.0f}, Identity(103)),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto coverage_texels = ReadBack(layer_.Texture());
  const auto composite       = CompositeFor(photo, kTint);
  int        tinted          = 0;
  for (std::size_t i = 0; i < composite.size() / 4; ++i) {
    if (coverage_texels[i * 4] > 0.5f) {
      float difference = 0.0f;
      for (int c = 0; c < 3; ++c) {
        difference += std::abs(composite[i * 4 + c] - first.rgba[i * 4 + c]);
      }
      tinted += difference > 0.05f ? 1 : 0;
    }
  }
  EXPECT_GT(tinted, 50) << "the viewport must show the tint";

  // The next export of the same document is byte-identical, and the photo input of the viewport
  // and of the scopes is the untinted display output.
  const auto second = RenderPhoto(document, input, RenderRequest{}, tap, PhotoSubmission(104));
  ASSERT_EQ(second.rgba.size(), first.rgba.size());
  EXPECT_EQ(std::memcmp(second.rgba.data(), first.rgba.data(), first.rgba.size() * sizeof(float)),
            0)
      << "the export pixels changed after the coverage display";
  const auto scope_second = tap.GetCurrentDisplayFrameView();
  ASSERT_TRUE(static_cast<bool>(scope_second));
  EXPECT_NE(scope_second.frame_id, scope_first.frame_id);
  EXPECT_EQ(scope_second.width, static_cast<int>(second.extent.width));
  EXPECT_EQ(scope_second.height, static_cast<int>(second.extent.height));
  EXPECT_EQ(scope_second.image_identity, kImage);
  ReleasePhoto(photo);
  auto& second_photo = ImportPhoto();
  ASSERT_NE(second_photo.texture, nullptr);
  const auto viewport_photo = ReadBack(second_photo.texture);
  ASSERT_EQ(viewport_photo.size(), second.rgba.size());
  EXPECT_EQ(
      std::memcmp(viewport_photo.data(), second.rgba.data(), second.rgba.size() * sizeof(float)), 0)
      << "the photo frame of the viewport must be the untinted display output";
}

TEST_F(ViewportCoverageCompositeTest, UncoveredHdrPixelsStayUnchanged) {
  const auto                       pixels = MakeDiscPixels(28.0, 24.0);
  const MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(111)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto                     coverage_texels = ReadBack(layer_.Texture());
  const auto                     extent          = request.geometry.render_extent;

  // Negative, zero, above-one, and large values with a non-one alpha.
  constexpr std::array<float, 6> kValues         = {-0.75f, 0.0f, 0.5f, 1.0f, 2.5f, 16.0f};
  std::vector<float> photo_rgba(static_cast<std::size_t>(extent.width) * extent.height * 4);
  for (std::size_t i = 0; i < photo_rgba.size() / 4; ++i) {
    photo_rgba[i * 4]     = kValues[i % kValues.size()];
    photo_rgba[i * 4 + 1] = kValues[(i + 2) % kValues.size()];
    photo_rgba[i * 4 + 2] = -kValues[(i + 4) % kValues.size()];
    photo_rgba[i * 4 + 3] = 0.5f;
  }
  ImportedPhoto photo;
  photo.texture =
      Upload(static_cast<int>(extent.width), static_cast<int>(extent.height), photo_rgba);
  photo.width       = static_cast<int>(extent.width);
  photo.height      = static_cast<int>(extent.height);
  photo.geometry    = request.geometry;
  // Nearest-equivalent 1:1 draw: every fragment samples its own texel center.
  const auto output = CompositeFor(photo, kTint);
  ASSERT_EQ(output.size(), photo_rgba.size());

  // A pixel is uncovered when its texel and the neighbors that a sample can reach are zero.
  const auto zero_around = [&](std::size_t i) {
    const auto x = static_cast<int>(i % extent.width);
    const auto y = static_cast<int>(i / extent.width);
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = std::clamp(x + dx, 0, static_cast<int>(extent.width) - 1);
        const int ny = std::clamp(y + dy, 0, static_cast<int>(extent.height) - 1);
        if (coverage_texels[(static_cast<std::size_t>(ny) * extent.width + nx) * 4] != 0.0f) {
          return false;
        }
      }
    }
    return true;
  };
  int uncovered = 0, covered = 0, failures = 0;
  for (std::size_t i = 0; i < output.size() / 4 && failures < 8; ++i) {
    const float coverage = coverage_texels[i * 4];
    if (coverage == 0.0f) {
      if (!zero_around(i)) {
        continue;
      }
      ++uncovered;
      if (std::memcmp(&output[i * 4], &photo_rgba[i * 4], 4 * sizeof(float)) != 0) {
        ADD_FAILURE() << "uncovered pixel " << i << " changed: (" << output[i * 4] << ", "
                      << output[i * 4 + 1] << ", " << output[i * 4 + 2] << ", " << output[i * 4 + 3]
                      << ")";
        ++failures;
      }
      continue;
    }
    ++covered;
    const auto expected = ExpectedComposite(PhotoPixel(photo_rgba, i), coverage, kTint);
    for (int c = 0; c < 4; ++c) {
      if (std::abs(output[i * 4 + c] - expected[c]) > 1e-5 * std::max(1.0, std::abs(expected[c]))) {
        ADD_FAILURE() << "covered pixel " << i << " channel " << c << " = " << output[i * 4 + c]
                      << ", expected " << expected[c];
        ++failures;
      }
    }
    EXPECT_EQ(output[i * 4 + 3], 0.5f) << "the composite must keep the photo alpha";
  }
  EXPECT_GT(uncovered, 100);
  EXPECT_GT(covered, 100);
}

TEST_F(ViewportCoverageCompositeTest, ShaderLoadFailureReportsDisplayError) {
  const auto                       pixels = MakeCandidatePixels(0);
  const MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(121)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  const auto    extent = request.geometry.render_extent;
  ImportedPhoto photo;
  photo.texture =
      Upload(static_cast<int>(extent.width), static_cast<int>(extent.height),
             std::vector<float>(static_cast<std::size_t>(extent.width) * extent.height * 4, 0.8f));
  photo.width    = static_cast<int>(extent.width);
  photo.height   = static_cast<int>(extent.height);
  photo.geometry = request.geometry;

  ViewportCompositePass    broken(QStringLiteral(":/shaders/editor_rhi/missing.vert.qsb"),
                                  QStringLiteral(":/shaders/editor_rhi/missing.frag.qsb"));
  ViewportCompositeOutcome outcome = ViewportCompositeOutcome::Drawn;
  const auto               output =
      Composite(QSize(photo.width, photo.height), {photo.texture, nullptr, layer_.Texture()},
                UniformsFor(photo, kTint), &outcome, &broken);
  EXPECT_EQ(outcome, ViewportCompositeOutcome::Failed);
  EXPECT_NE(broken.LastError().find("could not be loaded"), std::string::npos)
      << broken.LastError();
  // Nothing was drawn: the target holds the clear color, not a photo or a coverage preview.
  for (std::size_t i = 0; i < output.size() / 4; ++i) {
    ASSERT_EQ(output[i * 4], 0.0f) << "pixel " << i;
    ASSERT_EQ(output[i * 4 + 3], 1.0f) << "pixel " << i;
  }
  broken.Release();

  // The production pass draws the same inputs.
  ViewportCompositeOutcome production = ViewportCompositeOutcome::Failed;
  (void)Composite(QSize(photo.width, photo.height), {photo.texture, nullptr, layer_.Texture()},
                  UniformsFor(photo, kTint), &production);
  EXPECT_EQ(production, ViewportCompositeOutcome::Drawn) << pass_.LastError();
  EXPECT_TRUE(pass_.LastError().empty());
}

}  // namespace
}  // namespace alcedo::editor_rhi

int main(int argc, char** argv) {
  return alcedo::editor_rhi::coverage_test::RunCoverageChannelTests(argc, argv);
}
