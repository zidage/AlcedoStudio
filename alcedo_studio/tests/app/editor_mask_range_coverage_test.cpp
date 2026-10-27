//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_mask_range_coverage_test.cpp
/// @brief Mask ranges MR3: the range probe coverage through the production session facade.
///
/// ShowMaskRangeCoverage makes every editor render request carry a MaskCoverageProbe (checked on
/// the requests that the recording scheduler receives) and routes one Quality render; Hide clears
/// the coverage channel without a render. The probe and the AI Mask coverage preview replace each
/// other, and an image change ends the probe.

#include <optional>
#include <string>

#include "editor_mask_coverage_preview_test_support.hpp"

namespace alcedo::ui {
namespace {

using namespace coverage_preview_test;

class EditorMaskRangeCoverageTest : public EditorMaskCoveragePreviewTest {
 protected:
  void Show(const NodeId& grade_id, const MaskId& mask_id) {
    (void)service_->ShowMaskRangeCoverage(grade_id, mask_id);
    Drain();
  }
  void Hide() {
    (void)service_->HideMaskRangeCoverage();
    Drain();
  }
  /// Route one Interactive frame and return the probe of the newest scheduled request.
  auto ProbeOfNextFrame() -> std::optional<MaskCoverageProbe> {
    const auto before = scheduler_->scheduled.size();
    (void)service_->RequestViewChange(EditorRenderReason::ScopeRefresh, std::nullopt);
    CompleteFrames();
    EXPECT_GT(scheduler_->scheduled.size(), before) << "no frame was routed";
    return scheduler_->scheduled.empty() ? std::nullopt
                                         : scheduler_->scheduled.back().intent.mask_coverage_probe;
  }
  void SelectCommitted(const MaskId& mask_id) {
    (void)service_->SelectMaskCoveragePreview(
        GradeId(), mask_id,
        [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
    Drain();
  }
};

TEST_F(EditorMaskRangeCoverageTest, ProbeRidesOnEveryEditorFrameUntilHidden) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const auto probe   = MaskCoverageProbe{GradeId(), mask_id};
  const auto frames  = scheduler_->scheduled.size();

  Show(probe.grade_id, probe.mask_id);
  // One Quality render presents the coverage of the new probe at once; no coverage-only job.
  ASSERT_EQ(scheduler_->scheduled.size(), frames + 1);
  const auto& shown = scheduler_->scheduled.back().intent;
  EXPECT_EQ(shown.reason, EditorRenderReason::MaskCoverageProbeChanged);
  EXPECT_EQ(shown.quality, EditorRenderQuality::Quality);
  EXPECT_EQ(shown.mask_coverage_probe, probe);
  EXPECT_FALSE(ScopeUpdateAllowedForReason(shown.reason));
  EXPECT_TRUE(coverage_->jobs.empty());
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(Preview().range_probe, probe);
  CompleteFrames();

  // The same probe again routes no render; later frames of any reason carry the probe.
  const auto after_show = scheduler_->scheduled.size();
  Show(probe.grade_id, probe.mask_id);
  EXPECT_EQ(scheduler_->scheduled.size(), after_show);
  EXPECT_EQ(ProbeOfNextFrame(), probe);

  // Hide clears the coverage channel without a photo render; later frames carry no probe.
  const auto clears      = coverage_->clear_count;
  const auto before_hide = scheduler_->scheduled.size();
  Hide();
  EXPECT_EQ(coverage_->clear_count, clears + 1);
  EXPECT_EQ(scheduler_->scheduled.size(), before_hide);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Hidden);
  EXPECT_FALSE(Preview().range_probe.has_value());
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);
  Hide();
  EXPECT_EQ(coverage_->clear_count, clears + 1) << "Hide without a probe is no change";
}

TEST_F(EditorMaskRangeCoverageTest, ProbeAndAiCoveragePreviewReplaceEachOther) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const auto probe   = MaskCoverageProbe{GradeId(), mask_id};

  // The probe replaces a committed AI Mask preview: its job ends as Discarded, and the channel is
  // cleared after it.
  SelectCommitted(mask_id);
  ASSERT_EQ(coverage_->jobs.size(), 1u);
  Show(probe.grade_id, probe.mask_id);
  EXPECT_EQ(coverage_->events, (std::vector<std::string>{"job 1", "clear"}));
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_FALSE(outcomes_.empty());
  EXPECT_EQ(outcomes_.back().outcome, EditorMaskCoverageOutcome::Discarded);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  EXPECT_EQ(Preview().range_probe, probe);
  CompleteFrames();

  // A committed AI Mask preview replaces the probe: later frames carry no probe.
  SelectCommitted(mask_id);
  ASSERT_EQ(coverage_->jobs.size(), 2u);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::CommittedMask);
  EXPECT_FALSE(Preview().range_probe.has_value());
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);

  // Opening an AI Mask edit ends the probe, and the probe is refused while the edit is open.
  Show(probe.grade_id, probe.mask_id);
  ASSERT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  CompleteFrames();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  EXPECT_FALSE(Preview().range_probe.has_value());
  const auto frames = scheduler_->scheduled.size();
  Show(probe.grade_id, probe.mask_id);
  EXPECT_FALSE(Preview().range_probe.has_value());
  EXPECT_EQ(scheduler_->scheduled.size(), frames);
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);
}

TEST_F(EditorMaskRangeCoverageTest, ProbeOfAMissingMaskOrGradeIsRefused) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const auto frames  = scheduler_->scheduled.size();
  Show(GradeId(), MaskId{"mask.absent"});
  Show(NodeId{"grade.absent"}, mask_id);
  EXPECT_EQ(scheduler_->scheduled.size(), frames);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_FALSE(Preview().range_probe.has_value());
}

TEST_F(EditorMaskRangeCoverageTest, ImageChangeEndsTheProbe) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  Show(GradeId(), mask_id);
  CompleteFrames();
  ASSERT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);

  const auto frames = scheduler_->scheduled.size();
  (void)service_->Open(kOtherElementId, kOtherImageId);
  CompleteFrames();
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_FALSE(Preview().range_probe.has_value());
  ASSERT_GT(scheduler_->scheduled.size(), frames);
  for (std::size_t i = frames; i < scheduler_->scheduled.size(); ++i) {
    EXPECT_FALSE(scheduler_->scheduled[i].intent.mask_coverage_probe.has_value()) << "frame " << i;
  }
}

}  // namespace
}  // namespace alcedo::ui
