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

// MR4: during the AI Mask edit of a stored Mask, the range coverage of that Mask covers the
// candidate. Hide presents the newest candidate again without a model call, a candidate update
// replaces the probe, and the end of the edit ends it. Another Mask stays refused.
TEST_F(EditorMaskRangeCoverageTest, ProbeOfTheStoredMaskCoversItsCandidateUntilHidden) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const auto other   = CommitNewAiMask({{0.6f, 0.6f}, {0.8f, 0.8f}});
  const auto opened  = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted);
  auto session = OpenedSession(opened);
  Present(*session);
  ASSERT_FALSE(coverage_->jobs.empty());
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  ASSERT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);
  CompleteFrames();

  const auto frames = scheduler_->scheduled.size();
  Show(GradeId(), other);
  EXPECT_FALSE(Preview().range_probe.has_value()) << "only the Mask of the edit has a probe";
  EXPECT_EQ(scheduler_->scheduled.size(), frames);

  const auto probe = MaskCoverageProbe{GradeId(), mask_id};
  Show(probe.grade_id, probe.mask_id);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(Preview().range_probe, probe);
  ASSERT_EQ(scheduler_->scheduled.size(), frames + 1);
  EXPECT_EQ(scheduler_->scheduled.back().intent.mask_coverage_probe, probe);
  CompleteFrames();
  EXPECT_EQ(ProbeOfNextFrame(), probe);

  // Hide presents the newest candidate again: same shared pixels, no model call.
  const auto calls = runner_.calls;
  const auto jobs  = coverage_->jobs.size();
  Hide();
  ASSERT_EQ(coverage_->jobs.size(), jobs + 1);
  const auto* candidate = CandidateOf(coverage_->jobs.back().request);
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->pixels, CandidateOf(coverage_->jobs[jobs - 1].request)->pixels);
  EXPECT_EQ(runner_.calls, calls);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);
  EXPECT_FALSE(Preview().range_probe.has_value());
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);

  // A candidate update replaces the probe.
  Show(probe.grade_id, probe.mask_id);
  CompleteFrames();
  ASSERT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  Present(*session);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);
  EXPECT_FALSE(Preview().range_probe.has_value());
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);

  // The end of the edit ends the probe.
  Show(probe.grade_id, probe.mask_id);
  CompleteFrames();
  ASSERT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  (void)service_->CancelAiMaskEdit();
  CompleteFrames();
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_FALSE(Preview().range_probe.has_value());
  EXPECT_EQ(ProbeOfNextFrame(), std::nullopt);
}

// MR4: a range drag of the stored Mask of an open AI Mask edit writes live values without a commit
// and settles as one SetMaskField commit. Other Masks and other fields stay refused, and the AI
// Mask commit keeps the range.
TEST_F(EditorMaskRangeCoverageTest, RangeDragOfTheStoredAiMaskCommitsOnceDuringItsAiMaskEdit) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const auto other   = CommitNewAiMask({{0.6f, 0.6f}, {0.8f, 0.8f}});
  const auto opened  = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted);
  auto       session       = OpenedSession(opened);

  const auto range_command = [this](EditorMaskCreationCommandKind kind, const MaskId& target,
                                    nlohmann::json value) {
    EditorMaskCreationCommand command;
    command.kind        = kind;
    command.node_id     = GradeId();
    command.mask_id     = target;
    command.field_key   = std::string{kMaskFieldLuminanceRange};
    command.field_value = std::move(value);
    return command;
  };
  const auto enqueue = [this](const EditorMaskCreationCommand& command) {
    return service_->EnqueueMaskCreation(command).kind;
  };
  const auto live_range = [this](const MaskId& target) {
    return Working()->Document().PrimaryGrade()->FindMask(target)->luminance_range;
  };
  const LuminanceRangeModel first{0.2f, 0.6f, 0.05f};
  const LuminanceRangeModel second{0.3f, 0.7f, 0.05f};
  const auto                commits = Graph()->CommitCount();

  ASSERT_EQ(enqueue(range_command(EditorMaskCreationCommandKind::BeginMaskField, mask_id, {})),
            EditorSessionResultKind::Accepted);
  ASSERT_EQ(enqueue(range_command(EditorMaskCreationCommandKind::SetMaskField, mask_id,
                                  LuminanceRangeToJson(first))),
            EditorSessionResultKind::Accepted);
  ASSERT_EQ(enqueue(range_command(EditorMaskCreationCommandKind::SetMaskField, mask_id,
                                  LuminanceRangeToJson(second))),
            EditorSessionResultKind::Accepted);
  CompleteFrames();
  EXPECT_EQ(live_range(mask_id), second);
  EXPECT_EQ(Graph()->CommitCount(), commits) << "a drag step writes no commit";

  EditorMaskCreationCommand finish;
  finish.kind    = EditorMaskCreationCommandKind::Finish;
  finish.node_id = GradeId();
  finish.mask_id = mask_id;
  ASSERT_EQ(enqueue(finish), EditorSessionResultKind::Accepted);
  CompleteFrames();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  EXPECT_EQ(live_range(mask_id), second);
  EXPECT_TRUE(service_->ai_mask_edit_open());

  // Another Mask is refused by the owner; a field other than a range is refused at once.
  (void)enqueue(range_command(EditorMaskCreationCommandKind::SetMaskField, other,
                              LuminanceRangeToJson(first)));
  CompleteFrames();
  EXPECT_FALSE(live_range(other).has_value());
  EditorMaskCreationCommand invert;
  invert.kind    = EditorMaskCreationCommandKind::ToggleMaskInvert;
  invert.node_id = GradeId();
  invert.mask_id = mask_id;
  EXPECT_EQ(enqueue(invert), EditorSessionResultKind::Rejected);
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);

  // The AI Mask commit replaces the source and keeps the range.
  ASSERT_TRUE(Applied(session->AddPromptStep(Stroke({{0.25f, 0.25f}, {0.45f, 0.45f}}),
                                             AiMaskComponentOperation::Add)));
  const auto committed = Commit(*session, mask_id);
  EXPECT_NE(committed.kind, EditorSessionResultKind::Rejected) << committed.message;
  EXPECT_NE(committed.kind, EditorSessionResultKind::Failed) << committed.message;
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_EQ(live_range(mask_id), second);
}

}  // namespace
}  // namespace alcedo::ui
