//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file mask_range_coverage_probe_test.cpp
/// @brief Mask ranges MR3: the coverage of one probed Mask, taken from the photo render, in the
///        viewport coverage channel.
///
/// Each test renders a photo document on the device of one backend with a MaskCoverageProbe, in
/// the order of Renderer::RenderImage (execute with the probe record, present the photo frame,
/// download it, publish the probed coverage), and imports the coverage frame through the
/// production ViewportMaskCoverageLayer on the real QRhi of that backend. The expected coverage is
/// the R8 that MaskEvaluate wrote for the probed Mask in the same render, read back from the
/// device.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../edit/runtime/mask_range_test_support.hpp"
#include "edit/runtime/mask_coverage_probe_record.hpp"
#include "viewport_coverage_composite_test_support.hpp"

namespace alcedo::editor_rhi {
namespace {

using namespace coverage_test;

/// Soft Radial with a Luminance Range and a Hue Range: its coverage depends on the photo.
auto MakeRangedMask() -> MaskModel {
  auto mask            = mask_range_test::MakeSoftRadialMask("mask.ranged");
  mask.opacity         = 0.85f;
  mask.luminance_range = LuminanceRangeModel{0.25f, 0.8f, 0.1f};
  mask.color_range     = ColorRangeModel{20.0f, 200.0f, 25.0f};
  return mask;
}

auto MakeRangedDocument(bool enabled = true) -> PipelineDocument {
  auto document = MakePhotoDocument();
  auto mask     = MakeRangedMask();
  mask.enabled  = enabled;
  grade_mask_test::AddMask(*document.PrimaryGrade(), mask);
  document.MarkTopologyChanged();
  return document;
}

const NodeId kPrimaryGrade{"grade.primary"};

class MaskRangeCoverageProbeTest : public ViewportCompositeFixture {
 protected:
  /// Render @p document with @p probe and present it to the viewport photo channel and to
  /// @p coverage_sink (the base coverage channel when null).
  auto RenderProbed(const PipelineDocument& document, const MaskCoverageProbe& probe,
                    std::uint64_t request_id, const RenderRequest& request = {},
                    std::optional<FrameCompletionSubmission> submission = std::nullopt,
                    IMaskCoverageSink* coverage_sink = nullptr) -> ProbedPhotoRender {
    const auto input = MakePhotoInput();
    auto*      sink  = coverage_sink != nullptr ? coverage_sink : viewport_->maskCoverageSink();
    const auto frame = submission.value_or(PhotoSubmission(request_id));
    return RunOnRenderThread(
        [&] {
          return producer_->RenderPhotoWithProbe(document, input, request, *viewport_->frameSink(),
                                                 frame, probe, *sink);
        },
        nullptr, [this] { ServicePhotoTargets(); });
  }

  /**
   * The displayed coverage holds the codes of @p r8: red is the same code / 255 (within 1e-6; the
   * OpenCL division is not correctly rounded), green and blue 0, alpha 1.
   */
  void ExpectDisplayedCodes(const std::vector<std::uint8_t>& r8, const std::string& label,
                            const ViewportMaskCoverageLayer* layer = nullptr) {
    const auto& displayed = layer != nullptr ? *layer : layer_;
    ASSERT_NE(displayed.Texture(), nullptr) << label;
    const auto texels = ReadBack(displayed.Texture());
    ASSERT_EQ(texels.size(), r8.size() * 4) << label;
    int failures = 0;
    for (std::size_t i = 0; i < r8.size() && failures < 8; ++i) {
      const float red = texels[i * 4];
      const bool  ok  = std::lround(red * 255.0f) == r8[i] &&
                      std::abs(red - static_cast<float>(r8[i]) / 255.0f) <= 1e-6f &&
                      texels[i * 4 + 1] == 0.0f && texels[i * 4 + 2] == 0.0f &&
                      texels[i * 4 + 3] == 1.0f;
      if (!ok) {
        ADD_FAILURE() << label << ": texel " << i << " = (" << red << ", " << texels[i * 4 + 1]
                      << ", " << texels[i * 4 + 2] << ", " << texels[i * 4 + 3]
                      << "), expected red " << int(r8[i]) << " / 255";
        ++failures;
      }
    }
  }
};

TEST_F(MaskRangeCoverageProbeTest, PresentedCoverageEqualsTheProbedMaskR8) {
  const auto document = MakeRangedDocument();
  const auto probe    = MaskCoverageProbe{kPrimaryGrade, MaskId{"mask.ranged"}};
  const auto render   = RenderProbed(document, probe, 21);
  ASSERT_EQ(render.outcome, MaskCoverageProbeOutcome::Published);
  ReleasePhoto(ImportPhoto());
  ASSERT_TRUE(Consume());

  ASSERT_EQ(render.probed_r8.size(),
            static_cast<std::size_t>(render.photo.extent.width) * render.photo.extent.height);
  ExpectDisplayedCodes(render.probed_r8, "probed Mask");
  // The coverage frame follows its photo frame.
  EXPECT_EQ(layer_.Presentation().identity.request_id, 21u);
  EXPECT_EQ(layer_.Presentation().identity.session_epoch, kEpoch);
  EXPECT_EQ(layer_.Presentation().identity.image_identity, kImage);
  EXPECT_EQ(layer_.Presentation().geometry.render_extent, render.photo.geometry.render_extent);
  const auto& presented = layer_.Presentation().geometry.render_to_reference.m;
  EXPECT_TRUE(std::equal(std::begin(presented), std::end(presented),
                         std::begin(render.photo.geometry.render_to_reference.m)));
  // The ranges remove part of the Radial: the coverage has unselected, partial, and selected
  // pixels.
  const auto spread = mask_range_test::SpreadOf(render.probed_r8);
  EXPECT_GE(spread.zero, 20u);
  EXPECT_GE(spread.partial, 20u);
}

TEST_F(MaskRangeCoverageProbeTest, ProbeChangesNoPhotoPixelAndNoResultKey) {
  const auto document = MakeRangedDocument();
  const auto probe    = MaskCoverageProbe{kPrimaryGrade, MaskId{"mask.ranged"}};
  const auto input    = MakePhotoInput();

  const auto probed   = RenderProbed(document, probe, 31);
  ASSERT_EQ(probed.outcome, MaskCoverageProbeOutcome::Published);
  ReleasePhoto(ImportPhoto());

  // The same document without the probe: every result of the probed render is valid, and the
  // display output is byte-identical.
  const auto before = render_thread_.Post([this] { return producer_->PassStats(); }).get();
  const auto plain  = RunOnRenderThread(
      [&] {
        return producer_->RenderPhoto(document, input, RenderRequest{}, *viewport_->frameSink(),
                                       PhotoSubmission(32));
      },
      nullptr, [this] { ServicePhotoTargets(); });
  ReleasePhoto(ImportPhoto());
  const auto after = render_thread_.Post([this] { return producer_->PassStats(); }).get();
  EXPECT_EQ(after.mask_execute, before.mask_execute);
  EXPECT_EQ(after.mask_skip, before.mask_skip + 1);
  EXPECT_EQ(after.drt_execute, before.drt_execute);
  EXPECT_EQ(after.drt_skip, before.drt_skip + 1);
  ASSERT_EQ(plain.rgba.size(), probed.photo.rgba.size());
  EXPECT_EQ(
      std::memcmp(plain.rgba.data(), probed.photo.rgba.data(), plain.rgba.size() * sizeof(float)),
      0)
      << "the probe changed the photo output";

  // The probe of a valid Mask result: no pass runs again, and the coverage is still published.
  const auto again = RenderProbed(document, probe, 33);
  ReleasePhoto(ImportPhoto());
  const auto last = render_thread_.Post([this] { return producer_->PassStats(); }).get();
  EXPECT_EQ(last.mask_execute, after.mask_execute);
  EXPECT_EQ(last.drt_execute, after.drt_execute);
  ASSERT_EQ(again.outcome, MaskCoverageProbeOutcome::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 33u);
  ExpectDisplayedCodes(again.probed_r8, "probe of a valid Mask result");
  EXPECT_EQ(again.probed_r8, probed.probed_r8);
}

/// The document of the two-Grade test: Grade 1 sets every pixel to one orange color (a constant
/// 3D LUT), and Grade 2 is a clean Grade.
auto MakeOrangeFirstGradeDocument() -> PipelineDocument {
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  const auto cube     = std::filesystem::absolute("build/tmp/mask_range_probe/" +
                                                  std::string{ToString(g_backend)} + "/orange.cube");
  multi_grade_test::WriteConstantRgbCube(cube, 0.62f, 0.38f, 0.12f);
  multi_grade_test::GradeAdjustment<LmtModel>(document, kPrimaryGrade, type_ids::Lmt())
      .SetCubePath(cube.string());
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  return document;
}

/// A whole-image Mask with a wide Hue Range that contains orange and many photo hues.
auto MakeWideHueMask(std::string id) -> MaskModel {
  auto mask        = mask_range_test::MakeFullCoverageMask(std::move(id));
  mask.color_range = ColorRangeModel{0.0f, 300.0f, 12.0f};
  return mask;
}

/**
 * The same wide Hue Range on a Grade 2 Mask and on a Grade 1 Mask. The Grade 2 coverage is 255 at
 * every pixel, because Grade 2 reads the orange Grade 1 output; the Grade 1 coverage follows the
 * colors of the photo, which enter Grade 1. (A Mask restricts its own Grade, so the Grade 1 Mask is
 * in a second document.)
 */
TEST_F(MaskRangeCoverageProbeTest, SecondGradeCoverageIsComputedFromFirstGradeOutput) {
  auto second_document = MakeOrangeFirstGradeDocument();
  grade_mask_test::AddMask(*multi_grade_test::GradeNode(second_document, "grade.b"),
                           MakeWideHueMask("mask.second"));
  second_document.MarkTopologyChanged();
  const auto second = RenderProbed(second_document, {NodeId{"grade.b"}, MaskId{"mask.second"}}, 41);
  ReleasePhoto(ImportPhoto());
  ASSERT_EQ(second.outcome, MaskCoverageProbeOutcome::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 41u);
  ExpectDisplayedCodes(second.probed_r8, "Grade 2 Mask");
  ASSERT_FALSE(second.probed_r8.empty());
  EXPECT_TRUE(std::all_of(second.probed_r8.begin(), second.probed_r8.end(), [](std::uint8_t code) {
    return code == 255;
  })) << "the Grade 2 range must read the orange Grade 1 output";

  auto first_document = MakeOrangeFirstGradeDocument();
  grade_mask_test::AddMask(*first_document.PrimaryGrade(), MakeWideHueMask("mask.first"));
  first_document.MarkTopologyChanged();
  const auto first = RenderProbed(first_document, {kPrimaryGrade, MaskId{"mask.first"}}, 42);
  ReleasePhoto(ImportPhoto());
  ASSERT_EQ(first.outcome, MaskCoverageProbeOutcome::Published);
  ASSERT_TRUE(Consume());
  ExpectDisplayedCodes(first.probed_r8, "Grade 1 Mask");
  const std::set<std::uint8_t> first_codes(first.probed_r8.begin(), first.probed_r8.end());
  EXPECT_GE(first_codes.size(), 3u) << "the Grade 1 coverage must follow the photo colors";
  EXPECT_LT(std::count(first.probed_r8.begin(), first.probed_r8.end(), std::uint8_t{255}),
            static_cast<std::ptrdiff_t>(first.probed_r8.size()));
}

TEST_F(MaskRangeCoverageProbeTest, DetailRoiRenderPublishesToTheDetailChannel) {
  const auto         document = MakeRangedDocument();
  const auto         probe    = MaskCoverageProbe{kPrimaryGrade, MaskId{"mask.ranged"}};
  const FrameRoiRect roi{0.3f, 0.25f, 0.4f, 0.5f};
  RenderRequest      request;
  request.view = ViewRequest{NormalizedRect{roi.x, roi.y, roi.width, roi.height}, Extent2D{64, 48}};
  auto submission                     = PhotoSubmission(61);
  submission.metadata.frame_role      = FrameRole::DetailPatch;
  submission.metadata.source_roi_norm = roi;
  submission.mode                     = FramePresentationMode::RoiFrame;

  const auto render =
      RenderProbed(document, probe, 61, request, submission, viewport_->detailMaskCoverageSink());
  ReleasePhoto(ImportPhoto(FrameRole::DetailPatch));
  ASSERT_EQ(render.outcome, MaskCoverageProbeOutcome::Published);
  ASSERT_TRUE(ConsumeDetail());
  EXPECT_FALSE(Consume()) << "the base coverage channel must not receive the detail frame";
  EXPECT_EQ(layer_.Texture(), nullptr);

  // The detail frame has the grid of the ROI render, finer than the full frame of the same view.
  const auto& presented = detail_layer_.Presentation();
  EXPECT_EQ(presented.identity.request_id, 61u);
  EXPECT_EQ(presented.geometry.render_extent, render.photo.geometry.render_extent);
  EXPECT_NE(presented.geometry.render_to_reference.m[2], 0.0f) << "the ROI starts inside the image";
  ExpectDisplayedCodes(render.probed_r8, "detail ROI Mask", &detail_layer_);
  EXPECT_GE(mask_range_test::SpreadOf(render.probed_r8).partial, 20u);
}

TEST_F(MaskRangeCoverageProbeTest, ProbeOfAMaskThatTheRenderSkipsClearsTheCoverage) {
  const auto probe = MaskCoverageProbe{kPrimaryGrade, MaskId{"mask.ranged"}};
  const auto shown = RenderProbed(MakeRangedDocument(), probe, 51);
  ReleasePhoto(ImportPhoto());
  ASSERT_EQ(shown.outcome, MaskCoverageProbeOutcome::Published);
  ASSERT_TRUE(Consume());
  ASSERT_NE(layer_.Texture(), nullptr);

  // The disabled Mask is not evaluated: the older coverage must not stay displayed.
  const auto disabled = RenderProbed(MakeRangedDocument(false), probe, 52);
  ReleasePhoto(ImportPhoto());
  EXPECT_EQ(disabled.outcome, MaskCoverageProbeOutcome::NotEvaluated);
  EXPECT_TRUE(disabled.probed_r8.empty());
  EXPECT_TRUE(layer_.ApplyClear());
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_FALSE(Consume());

  // A Mask id that the Grade does not have is not evaluated either.
  const auto missing =
      RenderProbed(MakeRangedDocument(), {kPrimaryGrade, MaskId{"mask.absent"}}, 53);
  ReleasePhoto(ImportPhoto());
  EXPECT_EQ(missing.outcome, MaskCoverageProbeOutcome::NotEvaluated);
}

}  // namespace
}  // namespace alcedo::editor_rhi

int main(int argc, char** argv) {
  return alcedo::editor_rhi::coverage_test::RunCoverageChannelTests(argc, argv);
}
