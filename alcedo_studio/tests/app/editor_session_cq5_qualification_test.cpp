//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_session_cq5_qualification_test.cpp
/// @brief CQ5 transitional-path removal and production-sequence qualification.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "support/editor_session_command_queue_test_support.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo {
namespace {
using namespace alcedo::test;

auto MakeExposureTransferPackage(double exposure) -> AdjustmentTransferPackage {
  AdjustmentTransferPackage package;
  package.operators_.push_back(AdjustmentTransferEntry{
      PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE, true, false,
      nlohmann::json{{"exposure", exposure}}});
  return package;
}

class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request) -> std::uint64_t override {
    scheduled_.push_back(request);
    return ++next_job_;
  }
  void Cancel(std::uint64_t job_id) override { cancelled_.push_back(job_id); }
  void WaitForSessionIdle(std::uint64_t session_generation) override {
    waited_sessions_.push_back(session_generation);
  }

  std::vector<EditorRenderRequest> scheduled_;
  std::vector<std::uint64_t>       cancelled_;
  std::vector<std::uint64_t>       waited_sessions_;
  std::uint64_t                    next_job_ = 0;
};

class EditorSessionCq5QualificationTest : public ::testing::Test {
 protected:
  void SetUp() override { RebuildSession(); }

  void RebuildSession() {
    history_          = std::make_shared<ControllableEditorHistoryPort>();
    pipeline_         = std::make_shared<FakeEditorPipelinePort>();
    tasks_            = std::make_shared<FakeEditorTaskPort>();
    journal_          = std::make_shared<OrderRecordingJournalPort>();
    scheduler_        = std::make_shared<RecordingScheduler>();
    checkpoint_store_ = std::make_shared<FakeEditorCheckpointStore>();
    thumbnails_       = std::make_shared<FakeEditorThumbnailPort>();

    runtime_ = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, journal_,
                                                     scheduler_, checkpoint_store_, thumbnails_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);

    recorder_ = std::make_unique<SessionResultRecorder>();
    service_->SetResultObserver(recorder_->result_observer());
    service_->SetChangeNotifier(recorder_->change_notifier());
  }

  void drainQueue() { service_->DrainCommandQueueForTests(); }

  void presentFirstFrame() {
    drainQueue();
    const auto rid = service_->first_frame_request_id();
    if (rid == 0) {
      return;
    }
    runtime_->coordinator->NotifySchedulerCompleted(rid, true);
    drainQueue();
    runtime_->coordinator->NotifyFrameSubmitted(rid);
    drainQueue();
    runtime_->coordinator->NotifyFramePresented(rid);
    drainQueue();
  }

  void openInteractive(sl_element_id_t eid = 10, image_id_t iid = 20) {
    (void)service_->Open(eid, iid);
    presentFirstFrame();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  std::shared_ptr<ControllableEditorHistoryPort> history_;
  std::shared_ptr<FakeEditorPipelinePort>        pipeline_;
  std::shared_ptr<FakeEditorTaskPort>            tasks_;
  std::shared_ptr<OrderRecordingJournalPort>     journal_;
  std::shared_ptr<RecordingScheduler>            scheduler_;
  std::shared_ptr<FakeEditorCheckpointStore>     checkpoint_store_;
  std::shared_ptr<FakeEditorThumbnailPort>       thumbnails_;
  std::unique_ptr<EditorSessionRuntime>          runtime_;
  EditorSessionService*                          service_ = nullptr;
  std::unique_ptr<SessionResultRecorder>         recorder_;
};

TEST_F(EditorSessionCq5QualificationTest,
       PresentationCommandsReduceThroughQueueWithoutDirectBypass) {
  openInteractive();
  EXPECT_EQ(service_->presentation_sink_id(), 1u);

  service_->SetPresentationSinkId(42);
  service_->SetPresentationSize(1280, 720);
  service_->SetGeometryOverlayActive(true);
  EXPECT_EQ(service_->presentation_sink_id(), 42u);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionCq5QualificationTest,
       DirtyPastePerformsOnePublicationAndOneFinalRender) {
  openInteractive();
  service_->SetCopiedPackageAvailable(true);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;

  const auto render_count_before      = scheduler_->scheduled_.size();
  const auto materialize_count_before = checkpoint_store_->materialize_count;
  const auto result =
      service_->PasteAdjustments(MakeExposureTransferPackage(0.5), "Pasted Version");
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted);
  drainQueue();

  EXPECT_EQ(checkpoint_store_->materialize_count, materialize_count_before + 1);
  EXPECT_EQ(history_->transfer_publication_count, 1);
  EXPECT_EQ(scheduler_->scheduled_.size(), render_count_before + 1);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], "save_started");
  EXPECT_EQ(events[1], "version_created");
}

TEST_F(EditorSessionCq5QualificationTest,
       SaveCompletionWithoutExecutorIsDropped) {
  auto journal          = std::make_shared<FakeEditorJournalPort>();
  auto tasks            = std::make_shared<FakeEditorTaskPort>();
  auto checkpoint_store = std::make_shared<FakeEditorCheckpointStore>();
  auto thumbnails       = std::make_shared<FakeEditorThumbnailPort>();
  auto coordinator      = std::make_shared<EditorSaveCheckpointCoordinator>();
  tasks->fail_begin     = true;

  EditorSaveCheckpointService::Dependencies deps;
  deps.journal          = journal;
  deps.tasks            = tasks;
  deps.checkpoint_store = checkpoint_store;
  deps.thumbnails       = thumbnails;
  deps.save_coordinator = coordinator;
  // Intentionally omit command_executor — CQ5 drops the inline completion path.
  EditorSaveCheckpointService service(std::move(deps));

  bool called = false;
  auto lock   = service.TryAcquireSaveLock(1);
  SaveCheckpointRequest request;
  request.element_id            = 1;
  request.image_load_request_id = ImageLoadRequestId{1};
  request.capture               = MakeOpaqueSaveCapture();
  request.save_lock             = std::move(lock);
  const auto ticket = service.Start(std::move(request), [&](const SaveCheckpointResult&) {
    called = true;
  });
  EXPECT_FALSE(ticket.valid());
  EXPECT_FALSE(called);
}

TEST(EditorSessionCq5StaticApiBan,
     QmlAndPublicApiOmitBannedGenerationAndSnapshotRevisionTokens) {
  namespace fs = std::filesystem;
  const fs::path roots[] = {
      fs::path("alcedo_studio/src/ui/alcedo_main/qml"),
      fs::path("alcedo_studio/src/include/ui/alcedo_main/album_backend"),
      fs::path("alcedo_studio/src/include/app"),
  };
  const std::set<std::string> banned = {"sessionGeneration", "snapshotRevision",
                                        "renderGeneration", "viewGeneration"};
  std::vector<std::string>    hits;
  for (const auto& root : roots) {
    if (!fs::exists(root)) {
      continue;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto ext = entry.path().extension().string();
      if (ext != ".qml" && ext != ".hpp" && ext != ".cpp") {
        continue;
      }
      std::ifstream in(entry.path());
      std::string   line;
      while (std::getline(in, line)) {
        if (ext == ".hpp" || ext == ".cpp") {
          if (line.find("Q_PROPERTY") == std::string::npos &&
              line.find("QStringLiteral") == std::string::npos) {
            continue;
          }
        }
        for (const auto& token : banned) {
          if (line.find(token) != std::string::npos) {
            hits.push_back(entry.path().string() + ": " + line);
          }
        }
      }
    }
  }
  EXPECT_TRUE(hits.empty()) << "Banned public API tokens found:\n"
                            << [&] {
                                 std::string out;
                                 for (const auto& hit : hits) {
                                   out += hit;
                                   out += '\n';
                                 }
                                 return out;
                               }();
}

TEST(EditorSessionCq5StaticApiBan, HistoryTransferOmitsOneShotPasteMergeWrappers) {
  namespace fs = std::filesystem;
  const fs::path transfer_hpp =
      fs::path("alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_transfer.hpp");
  ASSERT_TRUE(fs::exists(transfer_hpp));
  std::ifstream in(transfer_hpp);
  std::string   contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents.find("PasteAdjustments("), std::string::npos);
  EXPECT_EQ(contents.find("auto BeginMerge("), std::string::npos);
  EXPECT_EQ(contents.find("auto CompleteMerge("), std::string::npos);
  EXPECT_NE(contents.find("PreparePaste("), std::string::npos);
  EXPECT_NE(contents.find("PublishTransferCandidate("), std::string::npos);
}

}  // namespace
}  // namespace alcedo
