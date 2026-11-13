//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_mask_coverage_preview_test_support.hpp
/// @brief Shared fixture of the coverage preview session tests (SM8.7, Mask ranges MR3): the
///        production session facade over the real history port, a recording coverage port, and a
///        recording frame scheduler.

#pragma once

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
#include "app/editor_mask_coverage_preview.hpp"
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

namespace alcedo::ui::coverage_preview_test {

inline constexpr sl_element_id_t kElementId      = 61;
inline constexpr image_id_t      kImageId        = 610;
inline constexpr sl_element_id_t kOtherElementId = 62;
inline constexpr image_id_t      kOtherImageId   = 620;
inline constexpr Extent2D        kAnalysisExtent{64, 48};
inline constexpr std::uint32_t   kLogitSize   = 256;
inline constexpr const char*     kModelSetId  = "sam2.1-tiny+people-sky-head-p1r2e15+s3od-synth";
inline constexpr const char*     kFingerprint = "0123456789abcdef0123456789abcdef";

inline auto CoveragePreviewTestPath(std::string_view name) -> std::filesystem::path {
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

inline auto Stroke(std::vector<std::pair<float, float>> points) -> AiMaskPromptStep {
  AiMaskPromptStep step;
  for (const auto& [x, y] : points) {
    step.points.push_back(AiMaskPromptPoint{x, y, AiMaskComponentOperation::Add});
  }
  return step;
}

inline auto Applied(const AiMaskStepResult& result) -> bool {
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

inline auto CandidateOf(const EditorMaskCoverageRequest& request)
    -> const MaskCoverageCandidateSelection* {
  return std::get_if<MaskCoverageCandidateSelection>(&request.selection);
}

inline auto CommittedOf(const EditorMaskCoverageRequest& request)
    -> const AiSegmentationMaskSource* {
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

}  // namespace alcedo::ui::coverage_preview_test
