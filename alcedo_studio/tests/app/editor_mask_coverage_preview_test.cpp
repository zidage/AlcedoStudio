//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_mask_coverage_preview_test.cpp
/// @brief The AI Mask coverage preview through the production session facade (SM8.7): a real
///        AiMaskEditSession over a fake segmentation runner, the real history port, and a coverage
///        port that records each presentation job and completes it when the test says so.
///
/// The tests check the temporary history of the session against the presented coverage: the
/// opened Mask and earlier model outputs stay available while a model request runs, a failed
/// model request or a failed presentation keeps the displayed coverage, Undo and Redo present
/// retained outputs without a model call, a failed presentation blocks Commit until Retry, Cancel
/// restores the committed selection, and Commit stores the original coverage in one history
/// operation.

#include "app/editor_mask_coverage_preview.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "app/ai_mask_edit_session.hpp"
#include "app/editor_mask_coverage_port.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_session_test_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId      = 61;
constexpr image_id_t      kImageId        = 610;
constexpr sl_element_id_t kOtherElementId = 62;
constexpr image_id_t      kOtherImageId   = 620;
constexpr Extent2D        kAnalysisExtent{64, 48};
constexpr std::uint32_t   kLogitSize   = 256;
constexpr const char*     kModelSetId  = "sam2.1-tiny+people-sky-head-p1r2e15+s3od-synth";
constexpr const char*     kFingerprint = "0123456789abcdef0123456789abcdef";

auto                      CoveragePreviewTestPath(std::string_view name) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_mask_coverage_preview"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + ".wal");
}

/// Answers each decode with logits of +8 in the bounding rectangle of the Add points (widened by
/// 0.05) and -8 elsewhere. Counts calls; runs @ref on_decode before it answers.
class RectangleRunner final : public IAiMaskSegmentationRunner {
 public:
  auto Decode(const SegmentationPrompt& prompt) -> SegmentationDecodeResult override {
    ++calls;
    if (on_decode) {
      on_decode();
    }
    SegmentationDecodeResult result;
    if (decode_failure.has_value()) {
      result.status.ok    = false;
      result.status.error = *decode_failure;
      return result;
    }
    float x0 = 1.0f, y0 = 1.0f, x1 = 0.0f, y1 = 0.0f;
    for (const auto& point : prompt.points) {
      x0 = std::min(x0, point.x - 0.05f);
      y0 = std::min(y0, point.y - 0.05f);
      x1 = std::max(x1, point.x + 0.05f);
      y1 = std::max(y1, point.y + 0.05f);
    }
    SegmentationMaskLevel level;
    level.logits.assign(static_cast<std::size_t>(kLogitSize) * kLogitSize, -8.0f);
    for (std::uint32_t row = 0; row < kLogitSize; ++row) {
      for (std::uint32_t column = 0; column < kLogitSize; ++column) {
        const float x = (static_cast<float>(column) + 0.5f) / kLogitSize;
        const float y = (static_cast<float>(row) + 0.5f) / kLogitSize;
        if (x >= x0 && x < x1 && y >= y0 && y < y1) {
          level.logits[static_cast<std::size_t>(row) * kLogitSize + column] = 8.0f;
        }
      }
    }
    result.status.ok  = true;
    result.logit_size = kLogitSize;
    result.masks.push_back(std::move(level));
    return result;
  }
  auto RunTask(SegmentationTask /*task*/, std::optional<uint32_t> /*person_index*/)
      -> SegmentationTaskResult override {
    ++calls;
    SegmentationTaskResult result;
    result.status.error = "no task results in this test";
    return result;
  }

  int                        calls = 0;
  std::optional<std::string> decode_failure;
  std::function<void()>      on_decode;
};

auto Stroke(std::vector<std::pair<float, float>> points) -> AiMaskPromptStep {
  AiMaskPromptStep step;
  for (const auto& [x, y] : points) {
    step.points.push_back(AiMaskPromptPoint{x, y, AiMaskComponentOperation::Add});
  }
  return step;
}

auto Applied(const AiMaskStepResult& result) -> bool {
  return result.status == AiMaskStepStatus::Applied;
}

/// Records every coverage job and clear in call order; a job completes only when the test calls
/// @ref Complete, which runs its completion like the render worker does.
class RecordingMaskCoveragePort final : public IEditorMaskCoveragePort {
 public:
  struct Job {
    std::uint64_t                   id = 0;
    EditorMaskCoverageRequest       request;
    EditorMaskCoverageJobCompletion on_complete;
    bool                            completed = false;
  };

  auto ScheduleMaskCoverage(EditorMaskCoverageRequest       request,
                            EditorMaskCoverageJobCompletion on_complete, std::string* error)
      -> std::uint64_t override {
    if (!refusal.empty()) {
      *error = refusal;
      return 0;
    }
    jobs.push_back(Job{++next_id, std::move(request), std::move(on_complete)});
    events.push_back("job " + std::to_string(jobs.back().id));
    return jobs.back().id;
  }
  void ClearMaskCoverage() override {
    ++clear_count;
    events.push_back("clear");
  }

  void Complete(std::size_t index, EditorMaskCoverageJobStatus status, std::string message = {}) {
    ASSERT_LT(index, jobs.size());
    ASSERT_FALSE(jobs[index].completed) << "job " << index << " completed twice";
    jobs[index].completed = true;
    jobs[index].on_complete({status, std::move(message)});
  }
  [[nodiscard]] auto Open() const -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(jobs.begin(), jobs.end(), [](const Job& job) { return !job.completed; }));
  }

  std::vector<Job>         jobs;
  std::vector<std::string> events;
  int                      clear_count = 0;
  std::uint64_t            next_id     = 0;
  std::string              refusal;
};

auto CandidateOf(const EditorMaskCoverageRequest& request)
    -> const MaskCoverageCandidateSelection* {
  return std::get_if<MaskCoverageCandidateSelection>(&request.selection);
}

auto CommittedOf(const EditorMaskCoverageRequest& request) -> const AiSegmentationMaskSource* {
  return std::get_if<AiSegmentationMaskSource>(&request.selection);
}

/// Records editor frame requests; frames complete only when the test reports them.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion /*on_complete*/) -> std::uint64_t override {
    scheduled.push_back(request);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t /*job_id*/) override {}

  std::vector<EditorRenderRequest> scheduled;
  std::uint64_t                    next_job = 0;
};

/// In-memory asset store that records each put.
class RecordingMaskAssetPort final : public IEditorMaskAssetPort {
 public:
  auto PutMaskAsset(const MaskAsset& asset, std::string* /*error*/) -> bool override {
    puts.push_back(asset);
    stored.emplace(std::string{asset.key.Value()}, asset);
    return true;
  }
  auto ReadMaskAsset(const MaskAssetKey& key, MaskAsset* asset, std::string* error)
      -> bool override {
    const auto found = stored.find(std::string{key.Value()});
    if (found == stored.end()) {
      *error = "Mask asset " + std::string{key.Value()} + " is missing from the project";
      return false;
    }
    *asset = found->second;
    return true;
  }

  std::map<std::string, MaskAsset> stored;
  std::vector<MaskAsset>           puts;
};

class EditorMaskCoveragePreviewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = CoveragePreviewTestPath("coverage_preview");
    lease_ = test::MakeInMemoryEditorLease(kElementId, test::WorkingSpaceBoundDefaultDocument());
    other_lease_ =
        test::MakeInMemoryEditorLease(kOtherElementId, test::WorkingSpaceBoundDefaultDocument());
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t element_id) {
          return test::CopyEditorLease(element_id == kElementId ? lease_ : other_lease_);
        }});
    history_ = std::make_shared<EditorSessionHistoryPort>();
    history_->SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_->SetPipelinePort(pipeline_);
    scheduler_   = std::make_shared<RecordingScheduler>();
    mask_assets_ = std::make_shared<RecordingMaskAssetPort>();
    coverage_    = std::make_shared<RecordingMaskCoveragePort>();
    runtime_     = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, std::make_shared<test::FakeEditorTaskPort>(), scheduler_,
        std::make_shared<test::FakeEditorCheckpointStore>(),
        std::make_shared<test::FakeEditorThumbnailPort>(), nullptr, nullptr, nullptr, mask_assets_,
        coverage_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
    Drain();
  }

  void TearDown() override {
    runtime_.reset();
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  void Drain() { service_->DrainCommandQueueForTests(); }

  void CompleteFrames() {
    Drain();
    std::size_t completed = 0;
    while (completed < scheduler_->scheduled.size()) {
      const auto request_id = scheduler_->scheduled[completed].request_id;
      ++completed;
      runtime_->coordinator->NotifySchedulerCompleted(request_id, true);
      Drain();
    }
  }

  void OpenInteractive() {
    (void)service_->Open(kElementId, kImageId);
    CompleteFrames();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  auto OpenAiMaskEdit(std::optional<MaskId> mask_id) -> EditorAiMaskEditOpenResult {
    std::optional<EditorAiMaskEditOpenResult> received;
    (void)service_->OpenAiMaskEdit(
        GradeId(), std::move(mask_id),
        [&received](EditorAiMaskEditOpenResult result) { received = std::move(result); });
    Drain();
    EXPECT_TRUE(received.has_value()) << "completion did not run";
    return received.value_or(EditorAiMaskEditOpenResult{});
  }

  /// A session of a new AI Mask on the analysis image.
  auto NewSession() -> std::unique_ptr<AiMaskEditSession> {
    return std::make_unique<AiMaskEditSession>(
        runner_, AiMaskSessionImage{kAnalysisExtent, kModelSetId, kFingerprint});
  }

  /// A session of the stored AI Mask that @p opened returned; no model call.
  auto OpenedSession(const EditorAiMaskEditOpenResult& opened)
      -> std::unique_ptr<AiMaskEditSession> {
    EXPECT_TRUE(opened.stored_source.has_value() && opened.stored_asset.has_value());
    return std::make_unique<AiMaskEditSession>(
        runner_, AiMaskSessionImage{kAnalysisExtent, kModelSetId, kFingerprint},
        *opened.stored_source, *opened.stored_asset);
  }

  /// Commits a new AI Mask made by one stroke in its own session; returns its id.
  auto CommitNewAiMask(std::vector<std::pair<float, float>> stroke) -> MaskId {
    EXPECT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
    auto session = NewSession();
    EXPECT_TRUE(
        Applied(session->AddPromptStep(Stroke(std::move(stroke)), AiMaskComponentOperation::Add)));
    auto [asset, source] = session->PrepareCommit();
    const auto result    = service_->CommitAiMaskEdit(
        EditorAiMaskCommit{GradeId(), std::nullopt, std::move(asset), std::move(source)});
    CompleteFrames();
    EXPECT_NE(result.kind, EditorSessionResultKind::Rejected) << result.message;
    EXPECT_NE(result.kind, EditorSessionResultKind::Failed) << result.message;
    const auto working = Working();
    const auto masks   = working->Document().PrimaryGrade()->Masks();
    EXPECT_FALSE(masks.empty());
    return masks.empty() ? MaskId{} : masks[masks.size() - 1].id;
  }

  /// Asks the facade to present the current coverage of @p session; records the outcome.
  void Present(const AiMaskEditSession& session) {
    (void)service_->UpdateAiMaskCandidateCoverage(
        MaskCoverageCandidateSelection{session.AcquireCoveragePixels(), session.Image().extent,
                                       session.Edge()},
        [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
    Drain();
  }

  /// Completes the job at @p index like the render worker and delivers it to the owner.
  void CompleteJob(std::size_t index, EditorMaskCoverageJobStatus status,
                   std::string message = {}) {
    coverage_->Complete(index, status, std::move(message));
    Drain();
  }

  auto Commit(const AiMaskEditSession& session, std::optional<MaskId> mask_id)
      -> EditorSessionResult {
    auto [asset, source] = session.PrepareCommit();
    auto result          = service_->CommitAiMaskEdit(
        EditorAiMaskCommit{GradeId(), std::move(mask_id), std::move(asset), std::move(source)});
    CompleteFrames();
    return result;
  }

  auto Preview() const -> EditorMaskCoveragePreviewState {
    return service_->mask_coverage_preview_state();
  }
  auto GradeId() -> NodeId { return Working()->Document().PrimaryGrade()->Id(); }
  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(*history_, kElementId);
  }
  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, kElementId);
  }
  auto WorkingAiSource(const MaskId& mask_id) -> AiSegmentationMaskSource {
    const auto* mask = Working()->Document().PrimaryGrade()->FindMask(mask_id);
    if (mask == nullptr || !std::holds_alternative<AiSegmentationMaskSource>(mask->source)) {
      ADD_FAILURE() << "the working Mask is not an AI Mask";
      return {};
    }
    return std::get<AiSegmentationMaskSource>(mask->source);
  }

  std::filesystem::path                      journal_path_;
  EditorHistoryLease                         lease_;
  EditorHistoryLease                         other_lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  std::shared_ptr<EditorSessionHistoryPort>  history_;
  std::shared_ptr<RecordingScheduler>        scheduler_;
  std::shared_ptr<RecordingMaskAssetPort>    mask_assets_;
  std::shared_ptr<RecordingMaskCoveragePort> coverage_;
  std::unique_ptr<EditorSessionRuntime>      runtime_;
  EditorSessionService*                      service_ = nullptr;
  RectangleRunner                            runner_;
  std::vector<EditorMaskCoverageResult>      outcomes_;
};

TEST_F(EditorMaskCoveragePreviewTest, NewModelRequestKeepsOpenedMaskAndDisplayedCoverage) {
  OpenInteractive();
  const auto mask_id            = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  const int  calls_after_commit = runner_.calls;

  const auto opened             = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted) << opened.result.message;
  auto session = OpenedSession(opened);
  ASSERT_FALSE(HasFatalFailure());
  // The opened Mask shows its stored coverage: no model call.
  EXPECT_EQ(runner_.calls, calls_after_commit);

  const auto opened_pixels = session->AcquireCoveragePixels();
  const auto opened_bytes  = *opened_pixels;
  Present(*session);
  ASSERT_EQ(coverage_->jobs.size(), 1u);
  const auto* first = CandidateOf(coverage_->jobs[0].request);
  ASSERT_NE(first, nullptr);
  // The job holds the allocation of the session, not a copy.
  EXPECT_EQ(first->pixels.get(), opened_pixels.get());
  const auto displayed_pixels = first->pixels;
  EXPECT_EQ(first->analysis_extent, kAnalysisExtent);
  EXPECT_EQ(coverage_->jobs[0].request.element_id, kElementId);
  EXPECT_EQ(coverage_->jobs[0].request.image_id, kImageId);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Pending);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_EQ(outcomes_.size(), 1u);
  EXPECT_EQ(outcomes_[0].outcome, EditorMaskCoverageOutcome::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);

  // While the next model request runs, nothing replaces the displayed coverage and its bytes stay.
  bool checked_during_request = false;
  runner_.on_decode           = [&] {
    checked_during_request = true;
    EXPECT_EQ(coverage_->jobs.size(), 1u);
    EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
    EXPECT_EQ(session->AcquireCoveragePixels().get(), opened_pixels.get());
  };
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.6f, 0.6f}, {0.7f, 0.7f}}), AiMaskComponentOperation::Add)));
  runner_.on_decode = {};
  EXPECT_TRUE(checked_during_request);
  // The stroke joins the open prompt group of the opened Mask, so the session decodes the stored
  // group and the new step; the opened display itself needed no model call (checked above).
  EXPECT_GT(runner_.calls, calls_after_commit);

  Present(*session);
  ASSERT_EQ(coverage_->jobs.size(), 2u);
  const auto* second = CandidateOf(coverage_->jobs[1].request);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second->pixels.get(), opened_pixels.get());
  EXPECT_NE(*second->pixels, opened_bytes);
  // The displayed request still holds the opened coverage, unchanged.
  EXPECT_EQ(displayed_pixels.get(), opened_pixels.get());
  EXPECT_EQ(*displayed_pixels, opened_bytes);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Pending);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  // The opened Mask is still a state of the session.
  EXPECT_TRUE(session->CanUndo());
  EXPECT_EQ(session->StateCount(), 2u);
}

TEST_F(EditorMaskCoveragePreviewTest, FailedModelRequestKeepsStackAndPreview) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.3f, 0.3f}, {0.5f, 0.5f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  const auto displayed   = session->AcquireCoveragePixels();
  const auto states      = session->StateCount();

  runner_.decode_failure = "The segmentation sidecar is not running";
  const auto failed =
      session->AddPromptStep(Stroke({{0.7f, 0.7f}, {0.8f, 0.8f}}), AiMaskComponentOperation::Add);
  EXPECT_EQ(failed.status, AiMaskStepStatus::Failed);
  EXPECT_EQ(failed.message, "The segmentation sidecar is not running");

  // No step, no new presentation, and the displayed coverage is still the current one.
  EXPECT_EQ(session->StateCount(), states);
  EXPECT_EQ(session->AcquireCoveragePixels().get(), displayed.get());
  EXPECT_EQ(coverage_->jobs.size(), 1u);
  EXPECT_EQ(coverage_->clear_count, 0);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);
}

TEST_F(EditorMaskCoveragePreviewTest, UndoAfterNewModelResultRestoresOpenedMaskWithoutInference) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.3f, 0.5f}});
  const auto opened  = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted);
  auto session = OpenedSession(opened);
  ASSERT_FALSE(HasFatalFailure());
  const auto opened_pixels = session->AcquireCoveragePixels();
  Present(*session);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.6f, 0.2f}, {0.8f, 0.4f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);

  const int  calls       = runner_.calls;
  const auto commits     = Graph()->CommitCount();
  const auto puts        = mask_assets_->puts.size();
  const auto history_rev = service_->history_revision();
  ASSERT_TRUE(session->Undo());
  // The opened Mask comes back as the same immutable allocation; no model call.
  EXPECT_EQ(session->AcquireCoveragePixels().get(), opened_pixels.get());
  Present(*session);
  ASSERT_EQ(coverage_->jobs.size(), 3u);
  const auto* undone = CandidateOf(coverage_->jobs[2].request);
  ASSERT_NE(undone, nullptr);
  EXPECT_EQ(undone->pixels.get(), opened_pixels.get());
  EXPECT_EQ(undone->edge, opened.stored_source->edge);
  CompleteJob(2, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);

  // Session Undo is temporary history: no model, asset write, or main history change.
  EXPECT_EQ(runner_.calls, calls);
  EXPECT_EQ(mask_assets_->puts.size(), puts);
  EXPECT_EQ(Graph()->CommitCount(), commits);
  EXPECT_EQ(service_->history_revision(), history_rev);
  EXPECT_EQ(WorkingAiSource(mask_id), *opened.stored_source);
}

TEST_F(EditorMaskCoveragePreviewTest, RedoUsesRetainedModelOutputs) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.2f, 0.2f}, {0.4f, 0.3f}}), AiMaskComponentOperation::Add)));
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.5f, 0.5f}, {0.7f, 0.8f}}), AiMaskComponentOperation::Add)));
  const auto step_bytes = *session->AcquireCoveragePixels();
  Present(*session);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_TRUE(session->Undo());
  Present(*session);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);
  ASSERT_NE(*CandidateOf(coverage_->jobs[1].request)->pixels, step_bytes);

  const int calls = runner_.calls;
  ASSERT_TRUE(session->Redo());
  Present(*session);
  ASSERT_EQ(coverage_->jobs.size(), 3u);
  const auto* redone = CandidateOf(coverage_->jobs[2].request);
  ASSERT_NE(redone, nullptr);
  // Redo composes the step from its retained model outputs: same bytes, no model call.
  EXPECT_EQ(*redone->pixels, step_bytes);
  EXPECT_EQ(redone->pixels.get(), session->AcquireCoveragePixels().get());
  EXPECT_EQ(runner_.calls, calls);
  CompleteJob(2, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(outcomes_.size(), 3u);
}

TEST_F(EditorMaskCoveragePreviewTest, GpuPreviewFailureKeepsDesiredStateAndBlocksCommit) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.3f, 0.3f}, {0.6f, 0.5f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_TRUE(Applied(session->SetEdgeAdjustment(AiMaskEdgeAdjustment{4.0f, 2})));
  Present(*session);
  const auto desired = session->AcquireCoveragePixels();
  const auto states  = session->StateCount();
  CompleteJob(1, EditorMaskCoverageJobStatus::Failed,
              "Mask coverage display: allocating the 640 x 480 RGBA32F target failed");

  // The desired step stays; the viewport keeps the previous result and says so.
  ASSERT_EQ(outcomes_.size(), 2u);
  EXPECT_EQ(outcomes_[1].outcome, EditorMaskCoverageOutcome::Failed);
  EXPECT_EQ(outcomes_[1].message,
            "Mask coverage display: allocating the 640 x 480 RGBA32F target failed");
  EXPECT_EQ(outcomes_[1].state.status, EditorMaskCoveragePreviewStatus::Previous);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Previous);
  EXPECT_EQ(Preview().error,
            "Mask coverage display: allocating the 640 x 480 RGBA32F target failed");
  EXPECT_EQ(session->StateCount(), states);
  EXPECT_EQ(session->Edge(), (AiMaskEdgeAdjustment{4.0f, 2}));

  // Commit is refused while the previous result is displayed.
  const auto commits = Graph()->CommitCount();
  const auto refused = Commit(*session, std::nullopt);
  EXPECT_EQ(refused.kind, EditorSessionResultKind::Rejected);
  EXPECT_NE(refused.message.find("previous AI Mask result"), std::string::npos) << refused.message;
  EXPECT_TRUE(mask_assets_->puts.empty());
  const auto puts_after_refusal = mask_assets_->puts.size();
  EXPECT_EQ(Graph()->CommitCount(), commits);
  EXPECT_TRUE(service_->ai_mask_edit_open());

  // Retry presents the same shared pixels and edge values again, without a model call.
  const int calls = runner_.calls;
  (void)service_->RetryMaskCoveragePreview(
      [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
  Drain();
  ASSERT_EQ(coverage_->jobs.size(), 3u);
  const auto* retried = CandidateOf(coverage_->jobs[2].request);
  ASSERT_NE(retried, nullptr);
  EXPECT_EQ(retried->pixels.get(), desired.get());
  EXPECT_EQ(retried->edge, (AiMaskEdgeAdjustment{4.0f, 2}));
  EXPECT_EQ(runner_.calls, calls);
  // Until the retry is displayed, the previous result stays labelled and Commit stays refused.
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Previous);
  EXPECT_EQ(Commit(*session, std::nullopt).kind, EditorSessionResultKind::Rejected);

  CompleteJob(2, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(outcomes_.back().outcome, EditorMaskCoverageOutcome::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_TRUE(Preview().error.empty());
  const auto committed = Commit(*session, std::nullopt);
  EXPECT_NE(committed.kind, EditorSessionResultKind::Rejected) << committed.message;
  EXPECT_NE(committed.kind, EditorSessionResultKind::Failed) << committed.message;
  EXPECT_EQ(mask_assets_->puts.size(), puts_after_refusal + 1);
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
}

TEST_F(EditorMaskCoveragePreviewTest, CancelRestoresCommittedPreview) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.1f, 0.6f}, {0.3f, 0.9f}});
  const auto source  = WorkingAiSource(mask_id);
  // The commit selected the new Mask; start from an explicit selection.
  for (std::size_t index = 0; index < coverage_->jobs.size(); ++index) {
    if (!coverage_->jobs[index].completed) {
      CompleteJob(index, EditorMaskCoverageJobStatus::Presented);
    }
  }
  const auto jobs_before = coverage_->jobs.size();
  (void)service_->SelectMaskCoveragePreview(
      GradeId(), mask_id,
      [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
  Drain();
  ASSERT_EQ(coverage_->jobs.size(), jobs_before + 1);
  ASSERT_NE(CommittedOf(coverage_->jobs.back().request), nullptr);
  EXPECT_EQ(*CommittedOf(coverage_->jobs.back().request), source);
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::CommittedMask);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);

  const auto opened = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted);
  auto session = OpenedSession(opened);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.6f, 0.1f}, {0.9f, 0.3f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::Candidate);
  const int clears = coverage_->clear_count;

  (void)service_->CancelAiMaskEdit();
  Drain();
  // Cancel presents the committed selection again; the candidate frame stays until it replaces it.
  const auto& restored = coverage_->jobs.back().request;
  ASSERT_NE(CommittedOf(restored), nullptr);
  EXPECT_EQ(*CommittedOf(restored), source);
  EXPECT_EQ(coverage_->clear_count, clears);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::CommittedMask);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Pending);
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
  EXPECT_EQ(WorkingAiSource(mask_id), source);

  // Without a committed selection, Cancel hides the candidate.
  (void)service_->ClearMaskCoveragePreview();
  Drain();
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Hidden);
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto fresh = NewSession();
  ASSERT_TRUE(Applied(
      fresh->AddPromptStep(Stroke({{0.4f, 0.4f}, {0.5f, 0.6f}}), AiMaskComponentOperation::Add)));
  Present(*fresh);
  CompleteJob(coverage_->jobs.size() - 1, EditorMaskCoverageJobStatus::Presented);
  const auto jobs_at_cancel = coverage_->jobs.size();
  const int  clears_before  = coverage_->clear_count;
  (void)service_->CancelAiMaskEdit();
  Drain();
  EXPECT_EQ(coverage_->jobs.size(), jobs_at_cancel);
  EXPECT_EQ(coverage_->clear_count, clears_before + 1);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Hidden);
}

TEST_F(EditorMaskCoveragePreviewTest, CommitStoresOriginalCoverageAndPublishesOneHistoryOperation) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(session->AddPromptStep(Stroke({{0.25f, 0.25f}, {0.5f, 0.6f}}),
                                             AiMaskComponentOperation::Add)));
  ASSERT_TRUE(Applied(session->SetEdgeAdjustment(AiMaskEdgeAdjustment{6.0f, 3})));
  Present(*session);
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  const auto original  = *session->AcquireCoveragePixels();
  const auto commits   = Graph()->CommitCount();
  const auto events    = coverage_->events.size();

  const auto committed = Commit(*session, std::nullopt);
  EXPECT_NE(committed.kind, EditorSessionResultKind::Rejected) << committed.message;
  EXPECT_NE(committed.kind, EditorSessionResultKind::Failed) << committed.message;

  // The asset is the original coverage before Feather and Expand, cropped to its bounds.
  ASSERT_EQ(mask_assets_->puts.size(), 1u);
  const auto& asset = mask_assets_->puts.front();
  const auto  rect  = AiMaskAssetPixelRect(asset.descriptor, kAnalysisExtent);
  ASSERT_TRUE(rect.has_value());
  ASSERT_EQ(asset.pixels.size(), static_cast<std::size_t>(rect->width) * rect->height);
  for (std::int32_t y = 0; y < rect->height; ++y) {
    for (std::int32_t x = 0; x < rect->width; ++x) {
      ASSERT_EQ(
          asset.pixels[static_cast<std::size_t>(y) * rect->width + x],
          original[static_cast<std::size_t>(rect->y + y) * kAnalysisExtent.width + rect->x + x])
          << "at " << x << ", " << y;
    }
  }
  // One history operation that adds the Mask with the edge values in its source.
  ASSERT_EQ(Graph()->CommitCount(), commits + 1);
  const auto head = Graph()->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(head.has_value());
  const auto batch = PipelineEditBatch::FromJSON(Graph()->GetCommit(*head).GetPayloadJSON());
  ASSERT_EQ(batch.changes.size(), 1u);
  const auto* added = std::get_if<AddMaskChange>(&batch.changes.front());
  ASSERT_NE(added, nullptr);
  const auto source = WorkingAiSource(added->mask_id);
  EXPECT_EQ(source.asset_key, asset.key);
  EXPECT_EQ(source.edge, (AiMaskEdgeAdjustment{6.0f, 3}));
  EXPECT_FALSE(service_->ai_mask_edit_open());

  // The committed Mask replaces the candidate as the preview, without a clear in between: the
  // candidate frame stays until the committed frame replaces it.
  ASSERT_EQ(coverage_->events.size(), events + 1);
  EXPECT_EQ(coverage_->events.back(), "job 2");
  const auto* selected = CommittedOf(coverage_->jobs.back().request);
  ASSERT_NE(selected, nullptr);
  EXPECT_EQ(*selected, source);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::CommittedMask);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
}

TEST_F(EditorMaskCoveragePreviewTest, NewestPendingRequestSupersedesOlderPendingRequest) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.3f, 0.3f}, {0.6f, 0.6f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  // Display-only slider values while the first job runs: one job in flight, one pending.
  for (const float feather : {1.0f, 2.0f, 3.0f}) {
    (void)service_->UpdateAiMaskCandidateCoverage(
        MaskCoverageCandidateSelection{session->AcquireCoveragePixels(), kAnalysisExtent,
                                       AiMaskEdgeAdjustment{feather, 0}},
        [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
    Drain();
  }
  EXPECT_EQ(coverage_->jobs.size(), 1u);
  ASSERT_EQ(outcomes_.size(), 2u);
  EXPECT_EQ(outcomes_[0].outcome, EditorMaskCoverageOutcome::Superseded);
  EXPECT_EQ(outcomes_[1].outcome, EditorMaskCoverageOutcome::Superseded);

  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  // The first job is displayed, but a newer request waits: not Current yet.
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Pending);
  ASSERT_EQ(coverage_->jobs.size(), 2u);
  EXPECT_EQ(CandidateOf(coverage_->jobs[1].request)->edge, (AiMaskEdgeAdjustment{3.0f, 0}));
  // Slider values only present; the session records no edge step.
  EXPECT_EQ(session->StateCount(), 2u);
  CompleteJob(1, EditorMaskCoverageJobStatus::Presented);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Current);
}

TEST_F(EditorMaskCoveragePreviewTest, ImageSwitchDiscardsPreviewAndKeepsNoSelection) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  auto session = NewSession();
  ASSERT_TRUE(Applied(
      session->AddPromptStep(Stroke({{0.3f, 0.3f}, {0.6f, 0.6f}}), AiMaskComponentOperation::Add)));
  Present(*session);
  const auto pixels = session->AcquireCoveragePixels();
  ASSERT_EQ(coverage_->Open(), 1u);

  (void)service_->Switch(kOtherElementId, kOtherImageId);
  CompleteFrames();
  ASSERT_EQ(service_->identity().element_id, kOtherElementId);
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Hidden);
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::None);
  EXPECT_EQ(coverage_->events.back(), "clear");

  // The job of the previous image finishes later; it is reported as discarded, not displayed.
  CompleteJob(0, EditorMaskCoverageJobStatus::Presented);
  ASSERT_EQ(outcomes_.size(), 1u);
  EXPECT_EQ(outcomes_[0].outcome, EditorMaskCoverageOutcome::Discarded);
  EXPECT_EQ(Preview().status, EditorMaskCoveragePreviewStatus::Hidden);
  // The job kept the shared read alive until it completed; the session still owns the bytes.
  EXPECT_EQ(CandidateOf(coverage_->jobs[0].request)->pixels.get(), pixels.get());

  // Without an AI Mask edit, a candidate is refused and nothing is scheduled.
  Present(*session);
  ASSERT_EQ(outcomes_.size(), 2u);
  EXPECT_EQ(outcomes_[1].outcome, EditorMaskCoverageOutcome::Rejected);
  EXPECT_EQ(coverage_->jobs.size(), 1u);
}

TEST_F(EditorMaskCoveragePreviewTest, SelectionOfUnionOrAnalyticMaskIsRefusedWithTheReason) {
  OpenInteractive();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  (void)mask_id;
  const auto jobs = coverage_->jobs.size();
  (void)service_->SelectMaskCoveragePreview(
      GradeId(), std::nullopt,
      [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
  (void)service_->SelectMaskCoveragePreview(
      GradeId(), MaskId{"mask.missing"},
      [this](EditorMaskCoverageResult result) { outcomes_.push_back(std::move(result)); });
  Drain();
  ASSERT_EQ(outcomes_.size(), 2u);
  EXPECT_EQ(outcomes_[0].outcome, EditorMaskCoverageOutcome::Rejected);
  EXPECT_NE(outcomes_[0].message.find("Grade Union"), std::string::npos) << outcomes_[0].message;
  EXPECT_EQ(outcomes_[1].outcome, EditorMaskCoverageOutcome::Rejected);
  EXPECT_EQ(outcomes_[1].message, "The Mask is missing");
  EXPECT_EQ(coverage_->jobs.size(), jobs);
}

}  // namespace
}  // namespace alcedo::ui
