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
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../edit/graph/test_camera_profile.hpp"
#include "../edit/input/prepared_raw_test_support.hpp"
#include "app/editor_mask_coverage_port.hpp"
#include "app/editor_mask_coverage_preview.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "viewport_coverage_composite_test_support.hpp"

namespace alcedo::editor_rhi {
namespace {

using namespace coverage_test;

class ViewportCoverageCompositeTest : public ViewportCompositeFixture {};

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

/// Coverage port of the preview owner that queues its jobs and clears in call order. The test runs
/// each one through the coverage channel of the fixture, as EditorSessionRenderSchedulerPort runs
/// them on the editor worker.
class QueuedCoveragePort final : public IEditorMaskCoveragePort {
 public:
  struct Job {
    EditorMaskCoverageRequest       request;
    EditorMaskCoverageJobCompletion on_complete;
  };
  /// A job, or std::monostate for a clear.
  using Work = std::variant<std::monostate, Job>;

  auto ScheduleMaskCoverage(EditorMaskCoverageRequest       request,
                            EditorMaskCoverageJobCompletion on_complete, std::string* /*error*/)
      -> std::uint64_t override {
    work.emplace_back(Job{std::move(request), std::move(on_complete)});
    return ++next_id;
  }
  void             ClearMaskCoverage() override { work.emplace_back(std::monostate{}); }

  std::deque<Work> work;
  std::uint64_t    next_id = 0;
};

// #516: the Commit of an AI Mask edit without a committed selection shows the photo without the
// coverage tint. The production preview owner ends the candidate; its clear reaches the viewport
// through the coverage channel, and the composite draws the photo pixels unchanged.
TEST_F(ViewportCoverageCompositeTest, CommitWithoutSelectionShowsPhotoWithoutTint) {
  const MaskCoverageDisplayRequest display{MakePhotoGeometry(1.0f), false, 1.0f};
  const auto                       extent = display.geometry.render_extent;
  // A blue photo, like sky or water.
  std::vector<float> photo_rgba(static_cast<std::size_t>(extent.width) * extent.height * 4);
  for (std::uint32_t y = 0; y < extent.height; ++y) {
    for (std::uint32_t x = 0; x < extent.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * extent.width + x) * 4;
      photo_rgba[i]       = 0.2f + 0.3f * static_cast<float>(x) / extent.width;
      photo_rgba[i + 1]   = 0.6f;
      photo_rgba[i + 2]   = 0.9f - 0.3f * static_cast<float>(y) / extent.height;
      photo_rgba[i + 3]   = 1.0f;
    }
  }
  ImportedPhoto photo;
  photo.texture =
      Upload(static_cast<int>(extent.width), static_cast<int>(extent.height), photo_rgba);
  photo.width                            = static_cast<int>(extent.width);
  photo.height                           = static_cast<int>(extent.height);
  photo.geometry                         = display.geometry;

  auto                              port = std::make_shared<QueuedCoveragePort>();
  std::deque<std::function<void()>> owner_tasks;
  EditorMaskCoveragePreview         preview(
      port, [&owner_tasks](std::function<void()> task) { owner_tasks.push_back(std::move(task)); });
  // Run the port work on the render thread, deliver the completions on this (owner) thread, and
  // display the result as the viewport render thread does, until no work is left.
  const auto run = [&] {
    while (!port->work.empty() || !owner_tasks.empty()) {
      while (!port->work.empty()) {
        auto work = std::move(port->work.front());
        port->work.pop_front();
        if (auto* job = std::get_if<QueuedCoveragePort::Job>(&work)) {
          const auto publication =
              Present(job->request.selection,
                      {display.geometry, job->request.mask.invert, job->request.mask.opacity},
                      Identity(job->request.request_id));
          job->on_complete({publication == MaskCoveragePublication::Published
                                ? EditorMaskCoverageJobStatus::Presented
                                : EditorMaskCoverageJobStatus::Failed,
                            {}});
        } else {
          render_thread_.Post([this] { viewport_->maskCoverageSink()->ClearCoverage(); }).get();
        }
      }
      while (!owner_tasks.empty()) {
        auto task = std::move(owner_tasks.front());
        owner_tasks.pop_front();
        task();
      }
    }
    (void)layer_.ApplyClear();
    (void)Consume();
  };
  const EditorMaskCoveragePreview::ImageTarget target{kImage, kImage, {}};
  std::vector<EditorMaskCoverageOutcome>       outcomes;

  // The candidate of the edit is displayed with the tint of the AppTheme roles (the default style
  // of the viewport).
  const auto                                   pixels = MakeCandidatePixels(0);
  preview.UpdateCandidate(
      target, MaskCoverageCandidateSelection{pixels, kAnalysis, {}}, {}, 1,
      [&outcomes](EditorMaskCoverageResult result) { outcomes.push_back(result.outcome); });
  run();
  ASSERT_EQ(outcomes, std::vector{EditorMaskCoverageOutcome::Presented});
  ASSERT_NE(layer_.Texture(), nullptr);
  const auto coverage_texels = ReadBack(layer_.Texture());
  const auto candidate       = CompositeFor(photo, MaskCoverageDisplayStyle{});
  ASSERT_EQ(candidate.size(), photo_rgba.size());
  int tinted = 0;
  for (std::size_t i = 0; i < candidate.size() / 4; ++i) {
    if (coverage_texels[i * 4] > 0.9f) {
      // Full coverage moves the photo halfway to the light red: a large change on blue pixels.
      float difference = 0.0f;
      for (int c = 0; c < 3; ++c) {
        difference = std::max(difference, std::abs(candidate[i * 4 + c] - photo_rgba[i * 4 + c]));
      }
      tinted += difference > 0.2f ? 1 : 0;
    }
  }
  EXPECT_GT(tinted, 50) << "the candidate tint must be visible on the blue photo";

  // Commit without a committed selection: the preview hides the coverage.
  const auto asset = WholeImageAsset(pixels);
  const auto mask  = ai_mask_test::MakeAiMask(asset, kAnalysis, "mask.ai");
  preview.EndCandidateAfterCommit(
      target,
      EditorMaskCoveragePreview::CommittedMask{NodeId("grade.primary"), mask.id,
                                               std::get<AiSegmentationMaskSource>(mask.source)},
      2);
  run();
  EXPECT_EQ(preview.State().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_EQ(preview.State().status, EditorMaskCoveragePreviewStatus::Hidden);
  EXPECT_EQ(layer_.Texture(), nullptr);
  const auto committed = CompositeFor(photo, MaskCoverageDisplayStyle{});
  ASSERT_EQ(committed.size(), photo_rgba.size());
  EXPECT_EQ(std::memcmp(committed.data(), photo_rgba.data(), committed.size() * sizeof(float)), 0)
      << "after the Commit the viewport must show the photo pixels without the tint";
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
      // Equal values: the Metal draw returns +0 for a -0 texel, so the sign of zero is not
      // compared.
      const bool unchanged =
          output[i * 4] == photo_rgba[i * 4] && output[i * 4 + 1] == photo_rgba[i * 4 + 1] &&
          output[i * 4 + 2] == photo_rgba[i * 4 + 2] && output[i * 4 + 3] == photo_rgba[i * 4 + 3];
      if (!unchanged) {
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
