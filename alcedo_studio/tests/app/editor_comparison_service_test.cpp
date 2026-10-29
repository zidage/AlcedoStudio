//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_service_test.cpp
/// @brief The editor comparison through the production session facade and history port: entry,
///        action restriction, captured working values, Version sources, close refresh, image
///        switch ordering, and HDR admission.

#include "app/editor_comparison_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/ai_mask_analysis_image.hpp"
#include "app/editor_action_policy.hpp"
#include "app/editor_image_render_port.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/lut_preview_image.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/editor_session_test_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId      = 42;
constexpr image_id_t      kImageId        = 420;
constexpr sl_element_id_t kOtherElementId = 43;
constexpr image_id_t      kOtherImageId   = 430;

auto ComparisonTestPath(std::string_view name) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_comparison"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + ".wal");
}

/// Records editor frame requests; frames complete only when the test reports them.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request, EditorPipelineScheduleCompletion /*on_complete*/)
      -> std::uint64_t override {
    scheduled.push_back(request);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t /*job_id*/) override {}

  std::vector<EditorRenderRequest> scheduled;
  std::uint64_t                    next_job = 0;
};

/**
 * @brief Image port whose jobs complete only when the test runs their completion, as the editor
 *        render worker would, from outside the session owner.
 */
class ControllableImageRenderPort final : public IEditorImageRenderPort {
 public:
  struct Job {
    std::uint64_t               id = 0;
    EditorImageRenderRequest    request;
    EditorImageRenderCompletion completion;
  };

  auto ScheduleImages(EditorImageRenderRequest request, EditorImageRenderCompletion on_complete,
                      std::string* error) -> std::uint64_t override {
    if (!reject_reason.empty()) {
      if (error != nullptr) *error = reject_reason;
      return 0;
    }
    jobs.push_back(Job{++next_id, std::move(request), std::move(on_complete)});
    return next_id;
  }
  void CancelImages(std::uint64_t job_id) override { cancelled.push_back(job_id); }
  [[nodiscard]] auto HasImageJob() const -> bool override {
    return std::any_of(jobs.begin(), jobs.end(),
                       [](const Job& job) { return static_cast<bool>(job.completion); });
  }

  /// Runs the completion of the last accepted job with @p status.
  void CompleteLast(EditorImageRenderStatus status, std::string message = {}) {
    ASSERT_FALSE(jobs.empty());
    EditorImageRenderResult result;
    result.status  = status;
    result.message = std::move(message);
    if (status == EditorImageRenderStatus::Completed) {
      result.images.resize(jobs.back().request.snapshots.size());
    }
    auto completion = std::move(jobs.back().completion);
    completion(std::move(result));
  }

  /// Runs the completion of the last accepted job as Completed with @p images.
  void CompleteLastWithImages(std::vector<RenderedPipelineImage> images) {
    ASSERT_FALSE(jobs.empty());
    EditorImageRenderResult result;
    result.status   = EditorImageRenderStatus::Completed;
    result.images   = std::move(images);
    auto completion = std::move(jobs.back().completion);
    completion(std::move(result));
  }

  std::vector<Job>           jobs;
  std::vector<std::uint64_t> cancelled;
  std::uint64_t              next_id = 0;
  std::string                reject_reason;
};

/// The LMT of the primary Color Grade of @p document.
auto PrimaryLmt(const PipelineDocument& document) -> const LmtModel* {
  const auto* grade = document.PrimaryGrade();
  return grade == nullptr
             ? nullptr
             : dynamic_cast<const LmtModel*>(grade->FindAdjustmentByType(type_ids::Lmt()));
}

/// The LUT write that applying a library entry submits, with a strength that a preview ignores.
auto LibraryLutWrite() -> EditorLutWrite {
  EditorLutWrite write;
  write.reference       = LutReference{LibraryLutReference{"user/look.cube"}};
  write.display_name    = "Look";
  write.input_encoding  = "acescc";
  write.output_encoding = "acescc";
  write.strength        = 0.25f;
  return write;
}

auto DevelopPayloadOf(const PipelineGraphSnapshot& snapshot) -> DevelopPayload {
  return snapshot.Document().Develop()->Params().Params();
}

auto ExposureEv(const PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(
      ReadEditorParameterJson(document, test::ColorGradeFieldTarget("exposure"), &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

class EditorComparisonServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = ComparisonTestPath("comparison_service");
    lease_        = test::MakeInMemoryEditorLease(kElementId, test::WorkingSpaceBoundDefaultDocument());
    other_lease_ =
        test::MakeInMemoryEditorLease(kOtherElementId, test::WorkingSpaceBoundDefaultDocument());
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [this](sl_element_id_t element_id) {
                                       return test::CopyEditorLease(
                                           element_id == kElementId ? lease_ : other_lease_);
                                     }});
    history_ = std::make_shared<EditorSessionHistoryPort>();
    history_->SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_->SetPipelinePort(pipeline_);
    scheduler_   = std::make_shared<RecordingScheduler>();
    images_      = std::make_shared<ControllableImageRenderPort>();
    checkpoints_ = std::make_shared<test::FakeEditorCheckpointStore>();
    runtime_     = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, std::make_shared<test::FakeEditorTaskPort>(), scheduler_, checkpoints_,
        std::make_shared<test::FakeEditorThumbnailPort>(), nullptr, nullptr, images_);
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

  /// Evaluate action availability again after the test wrote history directly through the port
  /// (in production every write is a session command, which publishes availability itself).
  void RepublishAvailability() {
    service_->SetCopiedPackageAvailable(false);
    Drain();
  }

  /// Completes every frame the coordinator schedules until no new frame follows.
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

  void OpenInteractive(sl_element_id_t element_id = kElementId, image_id_t image_id = kImageId) {
    (void)service_->Open(element_id, image_id);
    CompleteFrames();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
    ASSERT_EQ(service_->identity().element_id, element_id);
  }

  void OpenComparison(EditorComparisonKind kind = EditorComparisonKind::BeforeAfter) {
    (void)service_->OpenComparison(kind);
    Drain();
    ASSERT_TRUE(service_->comparison_state().active()) << LastMessage();
  }

  auto Handle() const -> EditorHistoryGuardHandle { return {kElementId, true}; }
  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(*history_, kElementId);
  }
  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, kElementId);
  }
  auto Decision(EditorAction action) const -> EditorActionDecision {
    return service_->action_availability().For(action);
  }
  auto LastMessage() const -> std::string {
    const auto results = service_->results();
    return results.empty() ? std::string{} : results.back().message;
  }

  auto CommitPanelField(const std::string& field, const std::string& json) -> bool {
    std::string error;
    const bool  ok =
        history_->CaptureAdjustmentBeforePreview(Handle(), test::PatchFromJson(field, json, false),
                                                 &error) &&
        history_->CommitAdjustment(Handle(), test::PatchFromJson(field, json, true), &error);
    EXPECT_TRUE(ok) << error;
    return ok;
  }

  auto CommitOdtEotf(const std::string& eotf) -> bool {
    const auto  json = R"({"odt":{"encoding_eotf":")" + eotf + R"("}})";
    std::string error;
    const bool  ok =
        history_->CaptureAdjustmentBeforePreview(
            Handle(), test::WithDrtPostTarget(test::PatchFromJson("odt", json, false)), &error) &&
        history_->CommitAdjustment(
            Handle(), test::WithDrtPostTarget(test::PatchFromJson("odt", json, true)), &error);
    EXPECT_TRUE(ok) << error;
    return ok;
  }

  std::filesystem::path                            journal_path_;
  EditorHistoryLease                               lease_;
  EditorHistoryLease                               other_lease_;
  std::shared_ptr<EditorSessionPipelinePort>       pipeline_;
  std::shared_ptr<EditorSessionHistoryPort>        history_;
  std::shared_ptr<RecordingScheduler>              scheduler_;
  std::shared_ptr<ControllableImageRenderPort>     images_;
  std::shared_ptr<test::FakeEditorCheckpointStore> checkpoints_;
  std::unique_ptr<EditorSessionRuntime>            runtime_;
  EditorSessionService*                            service_ = nullptr;
};

TEST_F(EditorComparisonServiceTest,
       ComparisonAdmissionBlocksEditsAndHistoryButAllowsCloseAndImageSelection) {
  OpenInteractive();
  ASSERT_TRUE(CommitPanelField("raw_decode", R"({"raw":{"highlights_reconstruct":false}})"));
  ASSERT_TRUE(Decision(EditorAction::OpenComparison).allowed);
  OpenComparison();
  const auto commits_before = Graph()->CommitCount();
  const auto head_before    = Graph()->GetActiveVersionRef().head_commit_hash;

  // Projected decisions: every write, history move, Version write, Paste, and view change is off.
  for (const auto action :
       {EditorAction::PreviewAdjustment, EditorAction::CommitAdjustment, EditorAction::Undo,
        EditorAction::Redo, EditorAction::MoveHead, EditorAction::DiscardChanges,
        EditorAction::CheckoutVersion, EditorAction::CreateRootVersion,
        EditorAction::BranchVersion, EditorAction::RenameVersion, EditorAction::RemoveVersion,
        EditorAction::ApplyPaste, EditorAction::RequestViewChange, EditorAction::OpenComparison}) {
    EXPECT_FALSE(Decision(action).allowed) << EditorActionName(action);
  }
  EXPECT_TRUE(Decision(EditorAction::SelectImage).allowed);
  EXPECT_TRUE(Decision(EditorAction::CloseEditor).allowed);
  EXPECT_TRUE(Decision(EditorAction::Shutdown).allowed);

  // Command admission applies the same decisions.
  const auto settled = test::WithColorGradeTarget(
      test::PatchFromJson("exposure", R"({"exposure":1.0})", true));
  EXPECT_EQ(service_->CommitAdjustment(settled).kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->Undo().kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->CreateRootVersion("Blocked").kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt).kind,
            EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->OpenComparison(EditorComparisonKind::Versions).kind,
            EditorSessionResultKind::Rejected);
  // Queued slider input and Mask input are refused at their own admission.
  EXPECT_EQ(service_->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                                                 test::PatchFromJson("exposure",
                                                                     R"({"exposure":3.0})", false)))
                .kind,
            EditorSessionResultKind::Rejected);
  EditorMaskCreationCommand mask_command;
  mask_command.kind = EditorMaskCreationCommandKind::BeginCreation;
  EXPECT_EQ(service_->EnqueueMaskCreation(mask_command).kind, EditorSessionResultKind::Rejected);
  Drain();
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
  EXPECT_EQ(Graph()->CommitCount(), commits_before);
  EXPECT_EQ(Graph()->GetActiveVersionRef().head_commit_hash, head_before);

  // Close removes exactly the restriction.
  (void)service_->CloseComparison(false, std::nullopt);
  Drain();
  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_TRUE(Decision(EditorAction::PreviewAdjustment).allowed);
  EXPECT_TRUE(Decision(EditorAction::Undo).allowed);
  EXPECT_TRUE(Decision(EditorAction::RequestViewChange).allowed);
  EXPECT_TRUE(Decision(EditorAction::OpenComparison).allowed);
  EXPECT_EQ(service_->CommitAdjustment(settled).kind, EditorSessionResultKind::RenderRouted);

  // Image selection stays admissible while comparing, and it closes the comparison first.
  OpenComparison();
  (void)service_->Switch(kOtherElementId, kOtherImageId);
  CompleteFrames();
  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_EQ(service_->identity().element_id, kOtherElementId);
  EXPECT_TRUE(Decision(EditorAction::PreviewAdjustment).allowed);
}

TEST_F(EditorComparisonServiceTest,
       CurrentVersionComparisonIncludesCapturedWorkingValuesWithoutSaving) {
  OpenInteractive();
  const auto active_version = Graph()->GetActiveVersionId();
  const auto commits_before = Graph()->CommitCount();
  // An exposure drag that has not been released when Compare is pressed.
  ASSERT_EQ(service_->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                                                 test::PatchFromJson("exposure",
                                                                     R"({"exposure":2.0})", false)))
                .kind,
            EditorSessionResultKind::Accepted);

  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);
  const auto& first = images_->jobs.front().request;
  ASSERT_EQ(first.snapshots.size(), 2u);
  const auto captured = first.snapshots[1];
  EXPECT_FLOAT_EQ(ExposureEv(captured->Document()), 2.0f) << "B includes the unreleased value";
  EXPECT_FLOAT_EQ(ExposureEv(first.snapshots[0]->Document()),
                  ExposureEv(lease_.root_->document))
      << "A is the imported root";
  EXPECT_EQ(captured, Working()) << "the capture is the published working document";
  EXPECT_EQ(first.element_id, kElementId);
  EXPECT_EQ(first.image_id, kImageId);
  // The pending value settled through the normal seal: one commit of the user's edit, no save.
  EXPECT_EQ(Graph()->CommitCount(), commits_before + 1);
  EXPECT_EQ(Graph()->GetActiveVersionId(), active_version);
  EXPECT_EQ(checkpoints_->materialize_count, 0);

  // Later selections reuse the same capture for Current.
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  Drain();
  (void)service_->SelectComparisonSources(EditorComparisonKind::Versions,
                                          EditorComparisonSource::Current(),
                                          EditorComparisonSource::Root());
  Drain();
  ASSERT_EQ(images_->jobs.size(), 2u);
  EXPECT_EQ(images_->jobs.back().request.snapshots[0], captured);
  EXPECT_EQ(service_->comparison_state().kind, EditorComparisonKind::Versions);
  (void)service_->CloseComparison(false, std::nullopt);
  Drain();
  EXPECT_EQ(Working(), captured) << "comparison never replaced the working document";
  EXPECT_EQ(checkpoints_->materialize_count, 0);
}

TEST_F(EditorComparisonServiceTest,
       ComparingVersionDoesNotCheckoutAndCheckoutStillAppliesItsSensorSettings) {
  OpenInteractive();
  const auto look_a = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitPanelField("raw_decode", R"({"raw":{"highlights_reconstruct":false}})"));
  ASSERT_TRUE(CommitPanelField("lens_calib", R"({"lens_calib":{"enabled":true}})"));
  ASSERT_TRUE(CommitPanelField("color_temp",
                               R"({"wb_mode":"custom","custom_cct":4100.0,"custom_tint":12.0})"));
  version_ref_id_t look_b{};
  std::string      error;
  ASSERT_TRUE(history_->CreateRootVersionAndCheckout(Handle(), "Look B", &look_b, &error)) << error;
  ASSERT_TRUE(CommitPanelField("color_temp",
                               R"({"wb_mode":"custom","custom_cct":6900.0,"custom_tint":-4.0})"));
  const auto current = DevelopPayloadOf(*Working());
  ASSERT_TRUE(current.highlights_reconstruct);
  ASSERT_FALSE(current.lens_enabled);

  OpenComparison(EditorComparisonKind::Versions);
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  Drain();
  (void)service_->SelectComparisonSources(EditorComparisonKind::Versions,
                                          EditorComparisonSource::Version(look_a),
                                          EditorComparisonSource::Current());
  Drain();
  ASSERT_EQ(service_->comparison_state().status, EditorComparisonStatus::Rendering)
      << service_->comparison_state().error;
  const auto& a = *images_->jobs.back().request.snapshots[0];
  // Comparing reads Look A with the current sensor settings and its own white balance.
  EXPECT_TRUE(DevelopPayloadOf(a).highlights_reconstruct);
  EXPECT_FALSE(DevelopPayloadOf(a).lens_enabled);
  EXPECT_FLOAT_EQ(DevelopPayloadOf(a).custom_cct, 4100.0f);
  EXPECT_EQ(Graph()->GetActiveVersionId(), look_b) << "comparing did not check out Look A";
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  (void)service_->CloseComparison(false, std::nullopt);
  Drain();

  // A normal checkout of Look A still applies every stored sensor field.
  (void)service_->CheckoutVersion(look_a);
  CompleteFrames();
  ASSERT_EQ(Graph()->GetActiveVersionId(), look_a);
  const auto checked_out = DevelopPayloadOf(*Working());
  EXPECT_FALSE(checked_out.highlights_reconstruct);
  EXPECT_TRUE(checked_out.lens_enabled);
  EXPECT_FLOAT_EQ(checked_out.custom_cct, 4100.0f);
}

TEST_F(EditorComparisonServiceTest, ClosePreservesViewTransformAndRefreshesCurrentDocument) {
  OpenInteractive();
  const auto active_version = Graph()->GetActiveVersionId();
  OpenComparison();
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  Drain();
  ASSERT_EQ(service_->comparison_state().status, EditorComparisonStatus::Ready);
  const auto frames_before = scheduler_->scheduled.size();

  // The viewport region of the current zoom and pan travels with the close unchanged.
  ViewportRenderRegion region;
  region.x_                = 120;
  region.y_                = 64;
  region.scale_x_          = 2.0f;
  region.scale_y_          = 2.0f;
  region.reference_width_  = 4096;
  region.reference_height_ = 2731;
  region.target_width_     = 640;
  region.target_height_    = 480;
  (void)service_->CloseComparison(true, region);
  Drain();

  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_EQ(images_->cancelled.size(), 0u) << "a finished job is not cancelled";
  EXPECT_TRUE(service_->TakeComparisonImages(images_->next_id).empty())
      << "close released the pair the GUI had not taken";
  ASSERT_EQ(scheduler_->scheduled.size(), frames_before + 1);
  const auto& refresh = scheduler_->scheduled.back().intent;
  EXPECT_EQ(refresh.reason, EditorRenderReason::ComparisonClosed);
  EXPECT_EQ(refresh.quality, EditorRenderQuality::Quality);
  EXPECT_EQ(refresh.element_id, kElementId);
  ASSERT_TRUE(refresh.view_region.has_value());
  EXPECT_EQ(refresh.view_region->x_, region.x_);
  EXPECT_EQ(refresh.view_region->y_, region.y_);
  EXPECT_FLOAT_EQ(refresh.view_region->scale_x_, region.scale_x_);
  EXPECT_EQ(Graph()->GetActiveVersionId(), active_version);
  CompleteFrames();
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  // Closing with nothing open changes nothing and renders nothing.
  const auto frames_after = scheduler_->scheduled.size();
  EXPECT_EQ(service_->CloseComparison(true, region).kind, EditorSessionResultKind::Accepted);
  Drain();
  EXPECT_EQ(scheduler_->scheduled.size(), frames_after);
}

TEST_F(EditorComparisonServiceTest, QueuedPairCompletionAfterImageSwitchCannotReopenComparison) {
  OpenInteractive();
  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);
  const auto job_id = images_->jobs.front().id;

  // The user selects another image while the pair renders. The switch reaches the owner first.
  (void)service_->Switch(kOtherElementId, kOtherImageId);
  Drain();
  EXPECT_FALSE(service_->comparison_state().active());
  ASSERT_EQ(images_->cancelled.size(), 1u);
  EXPECT_EQ(images_->cancelled.front(), job_id);

  // The worker finishes B anyway and queues its completion behind the switch.
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  CompleteFrames();
  EXPECT_EQ(service_->identity().element_id, kOtherElementId);
  const auto state = service_->comparison_state();
  EXPECT_FALSE(state.active());
  EXPECT_EQ(state.pair_id, 0u);
  EXPECT_TRUE(service_->TakeComparisonImages(job_id).empty());
  EXPECT_TRUE(Decision(EditorAction::PreviewAdjustment).allowed);

  // A completion that is queued before Close is reduced, then Close drops the pair.
  OpenComparison();
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  (void)service_->CloseComparison(false, std::nullopt);
  Drain();
  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_TRUE(service_->TakeComparisonImages(images_->jobs.back().id).empty());
}

TEST_F(EditorComparisonServiceTest, ShutdownDuringPairCancelsTheJobAndIgnoresItsCompletion) {
  OpenInteractive();
  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);
  const auto job_id = images_->jobs.front().id;

  (void)service_->Shutdown();
  Drain();
  EXPECT_EQ(service_->state(), EditorSessionState::ShuttingDown);
  EXPECT_FALSE(service_->comparison_state().active());
  ASSERT_EQ(images_->cancelled.size(), 1u);
  EXPECT_EQ(images_->cancelled.front(), job_id);

  // The render port reports the cancelled job after the session began shutting down.
  images_->CompleteLast(EditorImageRenderStatus::Cancelled);
  Drain();
  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_TRUE(service_->TakeComparisonImages(job_id).empty());
}

TEST_F(EditorComparisonServiceTest, HdrDocumentDisablesEntryAndSelectedHdrVersionReportsReason) {
  OpenInteractive();
  // HDR current document: entry is refused at admission with the reason.
  ASSERT_TRUE(CommitOdtEotf("st2084"));
  RepublishAvailability();
  const auto denied = Decision(EditorAction::OpenComparison);
  EXPECT_FALSE(denied.allowed);
  EXPECT_EQ(denied.reason, "Comparison is unavailable for HDR output.");
  EXPECT_EQ(service_->OpenComparison(EditorComparisonKind::BeforeAfter).kind,
            EditorSessionResultKind::Rejected);
  Drain();
  EXPECT_FALSE(service_->comparison_state().active());
  EXPECT_TRUE(images_->jobs.empty());

  // An SDR working Version: entry is admitted; selecting the HLG Version reports why.
  const auto hdr_version = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitOdtEotf("hlg"));
  version_ref_id_t sdr_version{};
  std::string      error;
  ASSERT_TRUE(history_->CreateRootVersionAndCheckout(Handle(), "SDR", &sdr_version, &error))
      << error;
  RepublishAvailability();
  ASSERT_TRUE(Decision(EditorAction::OpenComparison).allowed);
  OpenComparison(EditorComparisonKind::Versions);
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  Drain();
  const auto jobs_before = images_->jobs.size();
  (void)service_->SelectComparisonSources(EditorComparisonKind::Versions,
                                          EditorComparisonSource::Version(hdr_version),
                                          EditorComparisonSource::Current());
  Drain();
  const auto state = service_->comparison_state();
  EXPECT_TRUE(state.active()) << "the comparison stays open for another selection";
  EXPECT_EQ(state.status, EditorComparisonStatus::Failed);
  EXPECT_NE(state.error.find("HDR"), std::string::npos) << state.error;
  EXPECT_EQ(images_->jobs.size(), jobs_before) << "no image job for an HDR selection";
}

TEST_F(EditorComparisonServiceTest, PairPublishesOnceAndRenderFailureKeepsComparisonOpen) {
  OpenInteractive();
  OpenComparison();
  EXPECT_EQ(service_->comparison_state().status, EditorComparisonStatus::Rendering);
  // Selections that need new images wait for the rendering pair.
  EXPECT_EQ(service_
                ->SelectComparisonSources(EditorComparisonKind::Versions,
                                          EditorComparisonSource::Current(),
                                          EditorComparisonSource::Root())
                .kind,
            EditorSessionResultKind::Rejected);
  Drain();

  images_->CompleteLast(EditorImageRenderStatus::Completed);
  Drain();
  auto state = service_->comparison_state();
  ASSERT_EQ(state.status, EditorComparisonStatus::Ready);
  EXPECT_EQ(state.pair_id, images_->jobs.back().id);
  EXPECT_EQ(service_->TakeComparisonImages(state.pair_id).size(), 2u);
  EXPECT_TRUE(service_->TakeComparisonImages(state.pair_id).empty()) << "taken once";

  // A render failure keeps the comparison open with the real error; Retry renders again.
  ASSERT_EQ(service_->RetryComparison().kind, EditorSessionResultKind::Accepted);
  images_->CompleteLast(EditorImageRenderStatus::Failed, "CUDA download failed");
  Drain();
  state = service_->comparison_state();
  EXPECT_EQ(state.status, EditorComparisonStatus::Failed);
  EXPECT_EQ(state.error, "CUDA download failed");
  EXPECT_FALSE(Decision(EditorAction::PreviewAdjustment).allowed);

  images_->reject_reason = "Another image job is accepted";
  ASSERT_EQ(service_->RetryComparison().kind, EditorSessionResultKind::Accepted);
  Drain();
  state = service_->comparison_state();
  EXPECT_EQ(state.status, EditorComparisonStatus::Failed);
  EXPECT_EQ(state.error, "Another image job is accepted");
}

TEST_F(EditorComparisonServiceTest, PreviewImagesRenderRootAndWorkingValuesWithRequestedGeometry) {
  OpenInteractive();
  const auto commits_before = Graph()->CommitCount();
  ASSERT_EQ(service_
                ->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                    test::PatchFromJson("exposure", R"({"exposure":2.0})", false)))
                .kind,
            EditorSessionResultKind::Accepted);
  Drain();

  RenderRequest geometry                   = EditorImageRenderRequest{}.geometry;
  geometry.resolution.max_edge             = 512;
  geometry.view.visible_rect_in_edit_space = NormalizedRect{0.25f, 0.25f, 0.5f, 0.25f};
  auto       outcome  = std::make_shared<std::optional<EditorPreviewImagesResult>>();
  const auto accepted = service_->RenderPreviewImages(
      geometry, EditorPreviewImageSet::CurrentAndRoot,
      [outcome](EditorPreviewImagesResult result) { *outcome = std::move(result); });
  ASSERT_NE(accepted.kind, EditorSessionResultKind::Rejected) << accepted.message;
  Drain();

  ASSERT_EQ(images_->jobs.size(), 1u) << LastMessage();
  const auto& request = images_->jobs.front().request;
  ASSERT_EQ(request.snapshots.size(), 2u);
  EXPECT_FLOAT_EQ(ExposureEv(request.snapshots[0]->Document()), ExposureEv(lease_.root_->document))
      << "the first image is the imported root";
  EXPECT_FLOAT_EQ(ExposureEv(request.snapshots[1]->Document()), 2.0f)
      << "the second image has the working values, including an unreleased drag";
  EXPECT_EQ(request.geometry.resolution.max_edge, 512u);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.w, 0.5f);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.h, 0.25f);
  EXPECT_FALSE(outcome->has_value()) << "nothing is reported before the job ends";

  images_->CompleteLast(EditorImageRenderStatus::Completed);
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorPreviewImagesStatus::Completed) << (*outcome)->message;
  EXPECT_EQ(Graph()->CommitCount(), commits_before) << "a preview commits nothing";
  EXPECT_EQ(checkpoints_->materialize_count, 0);
}

// Issue #355: render.preview with compare 'none' renders only the working values.
TEST_F(EditorComparisonServiceTest, PreviewImagesCurrentOnlyRenderOnlyTheWorkingValues) {
  OpenInteractive();
  ASSERT_EQ(service_
                ->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                    test::PatchFromJson("exposure", R"({"exposure":1.5})", false)))
                .kind,
            EditorSessionResultKind::Accepted);
  Drain();

  auto       outcome  = std::make_shared<std::optional<EditorPreviewImagesResult>>();
  const auto accepted = service_->RenderPreviewImages(
      EditorImageRenderRequest{}.geometry, EditorPreviewImageSet::CurrentOnly,
      [outcome](EditorPreviewImagesResult result) { *outcome = std::move(result); });
  ASSERT_NE(accepted.kind, EditorSessionResultKind::Rejected) << accepted.message;
  Drain();

  ASSERT_EQ(images_->jobs.size(), 1u) << LastMessage();
  const auto& request = images_->jobs.front().request;
  ASSERT_EQ(request.snapshots.size(), 1u) << "the root is not rendered";
  EXPECT_FLOAT_EQ(ExposureEv(request.snapshots[0]->Document()), 1.5f)
      << "the only image has the working values";

  images_->CompleteLast(EditorImageRenderStatus::Completed);
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorPreviewImagesStatus::Completed) << (*outcome)->message;
}

// Issue #598: the port runs accepted image jobs in order, so preview images wait for the
// comparison job instead of failing, and the comparison job keeps running.
TEST_F(EditorComparisonServiceTest, PreviewImagesQueueBehindTheComparisonJobAndLeaveIt) {
  OpenInteractive();
  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);

  auto       outcome = std::make_shared<std::optional<EditorPreviewImagesResult>>();
  const auto record = [outcome](EditorPreviewImagesResult result) { *outcome = std::move(result); };
  (void)service_->RenderPreviewImages(EditorImageRenderRequest{}.geometry,
                                      EditorPreviewImageSet::CurrentAndRoot, record);
  Drain();
  EXPECT_FALSE(outcome->has_value()) << "the preview job waits; it is not refused";
  ASSERT_EQ(images_->jobs.size(), 2u) << "a second job was accepted";
  EXPECT_TRUE(images_->cancelled.empty()) << "the comparison job was not cancelled";
  EXPECT_EQ(service_->comparison_state().status, EditorComparisonStatus::Rendering);

  images_->CompleteLast(EditorImageRenderStatus::Failed, "device lost");
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorPreviewImagesStatus::Failed);
  EXPECT_EQ((*outcome)->message, "device lost");
}

// SM4: the AI Mask analysis image renders the import root look with the current Develop settings,
// without crop and rotation, and converts it to RGB8.
TEST_F(EditorComparisonServiceTest,
       AiMaskAnalysisImageRendersTheAnalysisDocumentWithoutCropOrRotation) {
  OpenInteractive();
  ASSERT_EQ(service_
                ->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                    test::PatchFromJson("exposure", R"({"exposure":1.5})", true)))
                .kind,
            EditorSessionResultKind::Accepted);
  Drain();
  ASSERT_TRUE(CommitPanelField("crop_rotate", R"({"rotation_degrees":7.0})"));
  const auto commits_before = Graph()->CommitCount();

  auto       key_only       = std::make_shared<std::optional<EditorAiMaskAnalysisResult>>();
  (void)service_->RenderAiMaskAnalysisImage(
      EditorAiMaskAnalysisContent::KeyOnly,
      [key_only](EditorAiMaskAnalysisResult result) { *key_only = std::move(result); });
  Drain();
  ASSERT_TRUE(key_only->has_value());
  ASSERT_EQ((*key_only)->status, EditorAiMaskAnalysisStatus::Completed) << (*key_only)->message;

  auto       outcome  = std::make_shared<std::optional<EditorAiMaskAnalysisResult>>();
  const auto accepted = service_->RenderAiMaskAnalysisImage(
      EditorAiMaskAnalysisContent::KeyAndPixels,
      [outcome](EditorAiMaskAnalysisResult result) { *outcome = std::move(result); });
  ASSERT_NE(accepted.kind, EditorSessionResultKind::Rejected) << accepted.message;
  Drain();

  ASSERT_EQ(images_->jobs.size(), 1u) << LastMessage();
  const auto& request = images_->jobs.front().request;
  ASSERT_EQ(request.snapshots.size(), 1u);
  EXPECT_FLOAT_EQ(ExposureEv(request.snapshots[0]->Document()), ExposureEv(lease_.root_->document))
      << "the analysis document has the root look, not the working grade";
  EXPECT_FLOAT_EQ(ExposureEv(Working()->Document()), 1.5f);
  EXPECT_EQ(DevelopPayloadOf(*request.snapshots[0]), DevelopPayloadOf(*Working()));
  EXPECT_EQ(request.geometry.document_geometry, DocumentGeometryUse::ReferenceSource);
  EXPECT_EQ(request.geometry.resolution.max_edge, kAiMaskAnalysisMaxLongEdge);
  EXPECT_TRUE(request.geometry.view.visible_rect_in_edit_space.IsFullFrame());
  EXPECT_EQ(request.element_id, kElementId);
  EXPECT_EQ(request.image_id, kImageId);
  EXPECT_FALSE(outcome->has_value()) << "nothing is reported before the job ends";

  RenderedPipelineImage rendered;
  rendered.pixels =
      std::make_shared<ImageBuffer>(cv::Mat(4, 8, CV_32FC4, cv::Scalar(0.5f, 0.25f, 1.0f, 1.0f)));
  rendered.geometry.render_extent         = {8, 4};
  rendered.geometry.full_reference_extent = {16, 8};
  images_->CompleteLastWithImages({std::move(rendered)});
  ASSERT_TRUE(outcome->has_value());
  ASSERT_EQ((*outcome)->status, EditorAiMaskAnalysisStatus::Completed) << (*outcome)->message;
  EXPECT_EQ((*outcome)->image_key, (*key_only)->image_key);
  EXPECT_EQ((*outcome)->image.width, 8u);
  EXPECT_EQ((*outcome)->image.height, 4u);
  ASSERT_EQ((*outcome)->image.pixels.size(), 8u * 4u * 3u);
  EXPECT_EQ((*outcome)->image.pixels[0], 128);
  EXPECT_EQ((*outcome)->image.pixels[1], 64);
  EXPECT_EQ((*outcome)->image.pixels[2], 255);
  EXPECT_EQ((*outcome)->full_reference_extent, (Extent2D{16, 8}));
  EXPECT_EQ(Graph()->CommitCount(), commits_before) << "the analysis image commits nothing";
}

TEST_F(EditorComparisonServiceTest, AiMaskAnalysisKeyOnlyStartsNoJob) {
  auto       outcome = std::make_shared<std::optional<EditorAiMaskAnalysisResult>>();
  const auto record  = [outcome](EditorAiMaskAnalysisResult result) {
    *outcome = std::move(result);
  };
  (void)service_->RenderAiMaskAnalysisImage(EditorAiMaskAnalysisContent::KeyOnly, record);
  Drain();
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorAiMaskAnalysisStatus::Rejected) << "no image is open yet";

  OpenInteractive();
  outcome->reset();
  (void)service_->RenderAiMaskAnalysisImage(EditorAiMaskAnalysisContent::KeyOnly, record);
  Drain();
  ASSERT_TRUE(outcome->has_value());
  ASSERT_EQ((*outcome)->status, EditorAiMaskAnalysisStatus::Completed) << (*outcome)->message;
  EXPECT_EQ((*outcome)->image_key.rfind(kAiMaskAnalysisImageKeyPrefix, 0), 0u);
  EXPECT_TRUE((*outcome)->image.pixels.empty());
  EXPECT_EQ((*outcome)->image.width, 0u);
  EXPECT_TRUE(images_->jobs.empty()) << "a key needs no render";
}

// Issue #598: the analysis render waits for the comparison job instead of failing.
TEST_F(EditorComparisonServiceTest, AiMaskAnalysisQueuesBehindTheComparisonJob) {
  OpenInteractive();
  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);

  auto       outcome = std::make_shared<std::optional<EditorAiMaskAnalysisResult>>();
  const auto record  = [outcome](EditorAiMaskAnalysisResult result) {
    *outcome = std::move(result);
  };
  (void)service_->RenderAiMaskAnalysisImage(EditorAiMaskAnalysisContent::KeyAndPixels, record);
  Drain();
  EXPECT_FALSE(outcome->has_value()) << "the analysis job waits; it is not refused";
  ASSERT_EQ(images_->jobs.size(), 2u) << "a second job was accepted";
  EXPECT_TRUE(images_->cancelled.empty()) << "the comparison job was not cancelled";
  images_->CompleteLast(EditorImageRenderStatus::Failed, "device lost");
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorAiMaskAnalysisStatus::Failed);
  EXPECT_EQ((*outcome)->message, "device lost");
  EXPECT_TRUE((*outcome)->image_key.empty());

  // A completed render without host pixels is a conversion failure, not an empty image.
  outcome->reset();
  (void)service_->RenderAiMaskAnalysisImage(EditorAiMaskAnalysisContent::KeyAndPixels, record);
  Drain();
  ASSERT_EQ(images_->jobs.size(), 3u);
  images_->CompleteLast(EditorImageRenderStatus::Completed);
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorAiMaskAnalysisStatus::Failed);
  EXPECT_NE((*outcome)->message.find("no host pixels"), std::string::npos) << (*outcome)->message;
  EXPECT_TRUE((*outcome)->image.pixels.empty());
}

// Issue #598: a LUT preview is the import root of the photo with the LUT at full strength, at a
// long edge of 256, so the edits of the photo do not change it.
TEST_F(EditorComparisonServiceTest, LutPreviewRendersTheImportRootWithTheLutAtFullStrength) {
  OpenInteractive();
  ASSERT_EQ(service_
                ->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                    test::PatchFromJson("exposure", R"({"exposure":1.5})", true)))
                .kind,
            EditorSessionResultKind::Accepted);
  Drain();
  ASSERT_TRUE(CommitPanelField("crop_rotate", R"({"rotation_degrees":7.0})"));
  const auto              commits_before = Graph()->CommitCount();

  auto                    outcome = std::make_shared<std::optional<EditorLutPreviewResult>>();
  EditorLutPreviewRequest request;
  request.image_id    = kImageId;
  request.write       = LibraryLutWrite();
  const auto accepted = service_->RenderLutPreview(
      request, [outcome](EditorLutPreviewResult result) { *outcome = std::move(result); });
  ASSERT_NE(accepted.kind, EditorSessionResultKind::Rejected) << accepted.message;
  Drain();

  ASSERT_EQ(images_->jobs.size(), 1u) << LastMessage();
  const auto& job = images_->jobs.front().request;
  ASSERT_EQ(job.snapshots.size(), 1u);
  const auto& document = job.snapshots[0]->Document();
  EXPECT_FLOAT_EQ(ExposureEv(document), ExposureEv(lease_.root_->document))
      << "the preview has the root look, not the working grade";
  EXPECT_FLOAT_EQ(ExposureEv(Working()->Document()), 1.5f);
  EXPECT_FLOAT_EQ(document.Geometry().RotationDegrees(), 0.0f)
      << "the preview has the root geometry, not the working crop";
  const auto* lmt = PrimaryLmt(document);
  ASSERT_NE(lmt, nullptr);
  EXPECT_EQ(lmt->Reference(), (LutReference{LibraryLutReference{"user/look.cube"}}));
  EXPECT_EQ(lmt->DisplayName(), "Look");
  EXPECT_FLOAT_EQ(lmt->Strength(), kDefaultLutStrength) << "a preview shows the complete LUT";
  EXPECT_EQ(job.snapshots[0]->Lineage(), Working()->Lineage())
      << "the executor keeps the binding of the image";
  EXPECT_EQ(job.geometry.resolution.max_edge, kLutPreviewLongEdge);
  EXPECT_EQ(job.geometry.document_geometry, DocumentGeometryUse::ApplyCropAndRotation);
  EXPECT_FALSE(job.output_color.has_value()) << "an SDR root renders its own encoding";
  EXPECT_EQ(job.element_id, kElementId);
  EXPECT_EQ(job.image_id, kImageId);
  EXPECT_FALSE(outcome->has_value()) << "nothing is reported before the job ends";

  RenderedPipelineImage rendered;
  rendered.pixels =
      std::make_shared<ImageBuffer>(cv::Mat(4, 6, CV_32FC4, cv::Scalar(0.5f, 0.25f, 1.0f, 1.0f)));
  rendered.geometry.render_extent = {6, 4};
  images_->CompleteLastWithImages({std::move(rendered)});
  ASSERT_TRUE(outcome->has_value());
  ASSERT_EQ((*outcome)->status, EditorLutPreviewStatus::Completed) << (*outcome)->message;
  ASSERT_NE((*outcome)->image.pixels, nullptr);
  EXPECT_EQ((*outcome)->image.geometry.render_extent, (Extent2D{6, 4}));
  EXPECT_EQ(Graph()->CommitCount(), commits_before) << "a preview commits nothing";
}

TEST_F(EditorComparisonServiceTest, LutPreviewIsRejectedWithoutTheRequestedOpenImage) {
  auto       outcome = std::make_shared<std::optional<EditorLutPreviewResult>>();
  const auto record  = [outcome](EditorLutPreviewResult result) { *outcome = std::move(result); };
  EditorLutPreviewRequest request;
  request.image_id = kImageId;
  request.write    = LibraryLutWrite();
  (void)service_->RenderLutPreview(request, record);
  Drain();
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorLutPreviewStatus::Rejected) << "no image is open yet";

  OpenInteractive();
  outcome->reset();
  request.image_id = kOtherImageId;
  (void)service_->RenderLutPreview(request, record);
  Drain();
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorLutPreviewStatus::Rejected);
  EXPECT_NE((*outcome)->message.find("another image"), std::string::npos) << (*outcome)->message;
  EXPECT_TRUE(images_->jobs.empty()) << "a preview of another image never renders";

  // A write that the LMT rejects fails with the real reason and starts no job.
  outcome->reset();
  request.image_id             = kImageId;
  request.write.input_encoding = "not-an-encoding";
  (void)service_->RenderLutPreview(request, record);
  Drain();
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorLutPreviewStatus::Failed);
  EXPECT_FALSE((*outcome)->message.empty());
  EXPECT_TRUE(images_->jobs.empty());
}

TEST_F(EditorComparisonServiceTest, LutPreviewQueuesBehindTheComparisonJobAndReportsItsFailure) {
  OpenInteractive();
  OpenComparison();
  ASSERT_EQ(images_->jobs.size(), 1u);

  auto                    outcome = std::make_shared<std::optional<EditorLutPreviewResult>>();
  EditorLutPreviewRequest request;
  request.image_id = kImageId;
  request.write    = LibraryLutWrite();
  (void)service_->RenderLutPreview(
      request, [outcome](EditorLutPreviewResult result) { *outcome = std::move(result); });
  Drain();
  ASSERT_EQ(images_->jobs.size(), 2u);
  EXPECT_TRUE(images_->cancelled.empty()) << "the comparison job was not cancelled";
  EXPECT_FALSE(outcome->has_value());

  images_->CompleteLast(EditorImageRenderStatus::Failed, "device lost");
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ((*outcome)->status, EditorLutPreviewStatus::Failed);
  EXPECT_EQ((*outcome)->message, "device lost");
  EXPECT_EQ((*outcome)->image.pixels, nullptr);
}

TEST(LutPreviewImageTest, HdrDocumentRendersTheSdrOutputAndSdrDocumentItsOwnEncoding) {
  auto document = CreateDefaultPipelineDocument();
  EXPECT_FALSE(LutPreviewOutputColor(document).has_value());

  DrtParameterUpdate update;
  update.encoding_eotf = DrtEotf::St2084;
  document.Drt()->Params().ApplyUpdate(update);
  const auto output = LutPreviewOutputColor(document);
  ASSERT_TRUE(output.has_value()) << "a tile is an 8-bit SDR image";
  EXPECT_EQ(output->encoding_space, ColorUtils::ColorSpace::REC709);
  EXPECT_EQ(output->encoding_eotf, ColorUtils::EOTF::GAMMA_2_2);
  EXPECT_FLOAT_EQ(output->peak_luminance, 100.0f);
}

}  // namespace
}  // namespace alcedo::ui
