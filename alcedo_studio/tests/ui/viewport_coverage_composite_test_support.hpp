//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file viewport_coverage_composite_test_support.hpp
/// @brief Shared fixture of the viewport composite tests (SM8.6, Mask ranges MR3): the photo path
///        of the coverage harness, photo frame import, and the production ViewportCompositePass on
///        the real QRhi of one backend.

#pragma once

#include <QColor>
#include <QSize>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../edit/graph/test_camera_profile.hpp"
#include "../edit/input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "mask_coverage_presentation_test_support.hpp"
#include "ui/editor_rhi/viewport_composite_pass.hpp"

namespace alcedo::editor_rhi::coverage_test {

/// Display-encoded test tint and opacity, distinct from every photo value of the tests.
inline constexpr MaskCoverageDisplayStyle kTint{true, 0.2f, 0.9f, 0.4f, 0.6f};

inline auto PhotoPixel(const std::vector<float>& rgba, std::size_t index) -> std::array<float, 4> {
  return {rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2], rgba[index * 4 + 3]};
}

/// The composite equation of section 5.3 for one pixel, in double precision.
inline auto ExpectedComposite(std::array<float, 4> photo, double coverage,
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
inline auto SampleCoverage(const std::vector<float>& texels, Extent2D extent, double u, double v)
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
inline auto MakeDiscPixels(double cx, double cy) -> AiMaskCoveragePixels {
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
inline auto WholeImageAsset(const AiMaskCoveragePixels& candidate) -> MaskAsset {
  MaskAsset asset;
  asset.descriptor = AiMaskCandidateDescriptor(kAnalysis);
  asset.pixels     = *candidate;
  asset.key        = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

/// The document of the photo tests: the default pipeline with the test camera profile and a
/// primary Grade exposure, so that a Mask of the Grade changes the photo.
inline auto MakePhotoDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  exposure->SetValue(1.5f);
  return document;
}

inline auto MakePhotoInput() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeF32RgbaPlane(kAnalysis.width, kAnalysis.height),
      gpu_dag_test::FullSensor(kAnalysis.width, kAnalysis.height));
}

inline auto PhotoSubmission(std::uint64_t request_id) -> FrameCompletionSubmission {
  FrameCompletionSubmission submission;
  submission.metadata.frame_role              = FrameRole::InteractivePrimary;
  submission.metadata.presentation_request_id = request_id;
  submission.metadata.session_epoch           = kEpoch;
  submission.metadata.image_identity          = kImage;
  submission.mode                             = FramePresentationMode::FullFrame;
  return submission;
}

inline auto PassCounters(const GpuNodePassStats& stats) -> std::vector<std::uint64_t> {
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

class ViewportCompositeFixture : public CoverageChannelTest {
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

  /// Import the newest photo frame of @p role with its geometry, as
  /// EditorViewportRenderer::consumeDirectFrames / consumeImportedGpuFrames do.
  auto ImportPhoto(FrameRole role = FrameRole::InteractivePrimary) -> ImportedPhoto& {
    ImportedPhoto photo;
    const auto    epoch = viewport_->sessionEpoch();
    const auto    image = viewport_->imageIdentity();
    if (UsesQueuedTargets()) {
      const auto frame = viewport_->present_queue()->ConsumeNewestReady(role, epoch, image);
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
        if (frame.preview_metadata.frame_role != role) {
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

}  // namespace alcedo::editor_rhi::coverage_test
