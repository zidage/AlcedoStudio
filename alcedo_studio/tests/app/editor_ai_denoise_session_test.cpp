//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_ai_denoise_session_test.cpp
/// @brief The AI Denoise error and the switch of the open photo through the production session
///        facade over the real history port (integration plan, DN7): the first frame with a
///        model error opens the photo with the error, a ready frame clears it, and Turn Off AI
///        Denoise is one change that undo reverts.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/input/ai_denoise_error.hpp"
#include "edit/runtime/render_progress.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/editor_session_test_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId = 61;
constexpr image_id_t      kImageId   = 610;
const std::string         kVersion(64, 'c');

auto                      AiDenoiseTestPath(std::string_view name) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_ai_denoise_session"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + ".wal");
}

/// Records editor frame requests and the AI Denoise switch of the document that each frame
/// renders; frames complete only when the test reports them.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion /*on_complete*/) -> std::uint64_t override {
    scheduled.push_back(request);
    switch_at_schedule.push_back(read_switch ? read_switch() : false);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t /*job_id*/) override {}

  std::vector<EditorRenderRequest> scheduled;
  std::vector<bool>                switch_at_schedule;
  std::function<bool()>            read_switch;
  std::uint64_t                    next_job = 0;
};

class EditorAiDenoiseSessionTest : public ::testing::Test {
 protected:
  void Start(bool switch_on) {
    journal_path_ = AiDenoiseTestPath("ai_denoise_session");
    auto root     = test::WorkingSpaceBoundDefaultDocument();
    if (switch_on) {
      root.Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersion});
    }
    develop_id_ = root.Develop()->Id();
    lease_      = test::MakeInMemoryEditorLease(kElementId, std::move(root));
    pipeline_   = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t) { return test::CopyEditorLease(lease_); }});
    history_ = std::make_shared<EditorSessionHistoryPort>();
    history_->SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_->SetPipelinePort(pipeline_);
    scheduler_              = std::make_shared<RecordingScheduler>();
    scheduler_->read_switch = [this] {
      const auto preview = pipeline_->CurrentPreview(kElementId);
      return preview && preview->Document().Develop()->Params().AiDenoise().has_value();
    };
    runtime_ = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, std::make_shared<test::FakeEditorTaskPort>(), scheduler_,
        std::make_shared<test::FakeEditorCheckpointStore>(),
        std::make_shared<test::FakeEditorThumbnailPort>());
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

  /// Completes every frame that the coordinator schedules until no new frame follows, each with
  /// @p success and @p message.
  void CompleteFrames(bool success = true, const std::string& message = {}) {
    Drain();
    while (completed_ < scheduler_->scheduled.size()) {
      const auto request_id = scheduler_->scheduled[completed_].request_id;
      ++completed_;
      runtime_->coordinator->NotifySchedulerCompleted(request_id, success, message);
      Drain();
    }
  }

  /// One settled `raw_decode` change with the members @p raw_members of its `raw` object.
  auto CommitRawDecode(const std::string& raw_members) -> EditorSessionResult {
    auto patch = test::PatchFromJson("raw_decode", R"({"raw":{)" + raw_members + "}}", true);
    patch.target.owner_kind = EditorParameterOwnerKind::Develop;
    patch.target.node_id    = develop_id_;
    patch.target.field_key  = "raw_decode";
    return service_->CommitAdjustment(patch);
  }

  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(*history_, kElementId);
  }
  auto Head() -> head_commit_hash_t { return Graph()->GetActiveVersionRef().head_commit_hash; }
  auto WorkingSwitch() -> bool {
    return test::EditorWorkingPreview(*pipeline_, kElementId)
        ->Document()
        .Develop()
        ->Params()
        .AiDenoise()
        .has_value();
  }
  auto LastMessage() const -> std::string {
    const auto results = service_->results();
    return results.empty() ? std::string{} : results.back().message;
  }

  std::filesystem::path                      journal_path_;
  NodeId                                     develop_id_;
  EditorHistoryLease                         lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  std::shared_ptr<EditorSessionHistoryPort>  history_;
  std::shared_ptr<RecordingScheduler>        scheduler_;
  std::unique_ptr<EditorSessionRuntime>      runtime_;
  EditorSessionService*                      service_   = nullptr;
  std::size_t                                completed_ = 0;
};

const std::string kModelMissing =
    AiDenoiseError(AiDenoiseErrorKind::ModelNotInstalled, "AI Denoise: the model is not installed")
        .what();

TEST_F(EditorAiDenoiseSessionTest, FirstFrameWithAModelErrorOpensThePhotoWithTheError) {
  Start(true);
  (void)service_->Open(kElementId, kImageId);
  CompleteFrames(false, "Render failed: " + kModelMissing);
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive) << LastMessage();
  const auto error = service_->ai_denoise_error();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->kind, AiDenoiseErrorKind::ModelNotInstalled);
  EXPECT_EQ(error->text, "AI Denoise: the model is not installed");

  // A failed frame without an AI Denoise error leaves the error as it is.
  const auto frames = scheduler_->scheduled.size();
  ASSERT_EQ(CommitRawDecode(R"("ai_denoise":{"model_version":")" + kVersion + "\"}").kind,
            EditorSessionResultKind::RenderRouted)
      << LastMessage();
  ASSERT_GT(scheduler_->scheduled.size(), frames);
  CompleteFrames(false, "Pipeline returned an empty result");
  EXPECT_TRUE(service_->ai_denoise_error().has_value());
}

TEST_F(EditorAiDenoiseSessionTest, ReadyFrameClearsTheErrorOfThePhoto) {
  Start(true);
  (void)service_->Open(kElementId, kImageId);
  CompleteFrames(false, kModelMissing);
  ASSERT_TRUE(service_->ai_denoise_error().has_value());

  // The model was installed meanwhile: the next frame is ready.
  ASSERT_EQ(CommitRawDecode(R"("method":"legacy")").kind, EditorSessionResultKind::RenderRouted)
      << LastMessage();
  CompleteFrames(true);
  EXPECT_FALSE(service_->ai_denoise_error().has_value());
}

TEST_F(EditorAiDenoiseSessionTest, FirstFrameWithAnotherErrorFailsAsBefore) {
  Start(true);
  (void)service_->Open(kElementId, kImageId);
  CompleteFrames(false, "RawInputLoader: unsupported CFA layout");
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
  EXPECT_FALSE(service_->ai_denoise_error().has_value());
}

TEST_F(EditorAiDenoiseSessionTest, TurnOffAiDenoiseIsOneChangeAndUndoTurnsItOn) {
  Start(true);
  (void)service_->Open(kElementId, kImageId);
  const auto slow = std::string(
      AiDenoiseError(AiDenoiseErrorKind::DeviceTooSlow, "AI Denoise: the GPU is too slow").what());
  CompleteFrames(false, slow);
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive) << LastMessage();
  ASSERT_TRUE(service_->ai_denoise_error().has_value());
  ASSERT_TRUE(WorkingSwitch());
  const auto head_on = Head();

  // Turn Off AI Denoise: one change; the next frame renders a document with the switch off.
  const auto frames  = scheduler_->scheduled.size();
  // The switch as the panel and the card write it (EditorAiDenoiseControl::turnOff).
  ASSERT_EQ(CommitRawDecode(R"("ai_denoise":null)").kind, EditorSessionResultKind::RenderRouted)
      << LastMessage();
  Drain();
  ASSERT_GT(scheduler_->scheduled.size(), frames);
  EXPECT_FALSE(scheduler_->switch_at_schedule[frames]);
  CompleteFrames(true);
  EXPECT_FALSE(service_->ai_denoise_error().has_value());
  EXPECT_FALSE(WorkingSwitch());
  EXPECT_NE(Head(), head_on);

  // One undo returns to the head of the switch on, and the next frame runs the model again.
  const auto frames_before_undo = scheduler_->scheduled.size();
  ASSERT_EQ(service_->Undo().kind, EditorSessionResultKind::Accepted) << LastMessage();
  Drain();
  EXPECT_EQ(Head(), head_on);
  EXPECT_TRUE(WorkingSwitch());
  ASSERT_GT(scheduler_->scheduled.size(), frames_before_undo);
  EXPECT_TRUE(scheduler_->switch_at_schedule[frames_before_undo]);
  CompleteFrames(false, slow);
  EXPECT_TRUE(service_->ai_denoise_error().has_value());
}

/// A value that the renderer sends during a model run (DN14).
auto TilesOf(int completed, int total) -> RenderProgress {
  RenderProgress progress;
  progress.read_raw   = true;
  progress.ai_denoise = true;
  progress.step       = RenderStep::AiDenoise;
  progress.completed  = completed;
  progress.total      = total;
  return progress;
}

TEST_F(EditorAiDenoiseSessionTest, ProgressOfTheOpenPhotoIsPublishedAndClearedByItsFrame) {
  Start(true);
  int notified = 0;
  service_->SetRenderProgressObserver([&] { ++notified; });
  (void)service_->Open(kElementId, kImageId);
  Drain();
  ASSERT_FALSE(scheduler_->scheduled.empty());
  const auto first_frame = scheduler_->scheduled.front().intent.render_progress;
  ASSERT_TRUE(static_cast<bool>(first_frame));
  EXPECT_FALSE(service_->render_progress().has_value());

  // The render worker reports; the owner publishes after its queue.
  first_frame(TilesOf(3, 12));
  EXPECT_FALSE(service_->render_progress().has_value());
  const int before = notified;
  Drain();
  ASSERT_TRUE(service_->render_progress().has_value());
  EXPECT_EQ(*service_->render_progress(), TilesOf(3, 12));
  EXPECT_GT(notified, before);

  // The same value again notifies nothing.
  const int same_before = notified;
  first_frame(TilesOf(3, 12));
  Drain();
  EXPECT_EQ(notified, same_before);

  // The ready frame ends the steps of its render.
  const int ready_before = notified;
  CompleteFrames(true);
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive) << LastMessage();
  EXPECT_FALSE(service_->render_progress().has_value());
  EXPECT_GT(notified, ready_before);

  // A failed frame ends them too: the switch on in the open photo runs the model again.
  const auto frames = scheduler_->scheduled.size();
  ASSERT_EQ(CommitRawDecode(R"("method":"legacy")").kind, EditorSessionResultKind::RenderRouted)
      << LastMessage();
  Drain();
  ASSERT_GT(scheduler_->scheduled.size(), frames);
  const auto switched = scheduler_->scheduled[frames].intent.render_progress;
  ASSERT_TRUE(static_cast<bool>(switched));
  switched(TilesOf(0, 0));
  Drain();
  ASSERT_TRUE(service_->render_progress().has_value());
  CompleteFrames(false, kModelMissing);
  EXPECT_FALSE(service_->render_progress().has_value());
  EXPECT_TRUE(service_->ai_denoise_error().has_value());
}

TEST_F(EditorAiDenoiseSessionTest, ProgressOfAPhotoThatIsNoLongerOpenIsDropped) {
  Start(true);
  (void)service_->Open(kElementId, kImageId);
  Drain();
  ASSERT_FALSE(scheduler_->scheduled.empty());
  const auto first_frame = scheduler_->scheduled.front().intent.render_progress;
  ASSERT_TRUE(static_cast<bool>(first_frame));
  first_frame(TilesOf(1, 12));
  Drain();
  ASSERT_TRUE(service_->render_progress().has_value());

  // The load of the photo ends: its progress goes with it, and a late value is dropped.
  (void)service_->Close(false);
  CompleteFrames(false, "Cancelled during execution");
  Drain();
  EXPECT_FALSE(service_->render_progress().has_value());
  first_frame(TilesOf(2, 12));
  Drain();
  EXPECT_FALSE(service_->render_progress().has_value());
}

}  // namespace
}  // namespace alcedo::ui
