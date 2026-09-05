//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Slider → history → coordinator boundary characterization. These tests record
/// the current synchronous live mutation: UI control values and live document
/// parameters are distinct storage, but pointer-drag submit acquires the live
/// render lock on the calling thread and writes the document before the blocked
/// renderer completes. Later input-queue work inverts the live-write timing;
/// this file must keep passing on the current path.

#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/latch_blocked_pipeline_scheduler_port.hpp"
#include "support/manual_monotonic_clock.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace alcedo::ui {
namespace {

auto MakePipelineGuard(sl_element_id_t element_id) -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_ = guard->commit_graph_->GetRootId();
  guard->root_document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::ClonePipelineDocument(*guard->document_));
  return guard;
}

auto DocumentExposureEv(const alcedo::PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(alcedo::ReadEditorParameterJson(document, alcedo::test::ColorGradeFieldTarget("exposure"),
                                              &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

class LiveHistorySubmitter final : public QObject, public IEditorAdjustmentSubmitter {
 public:
  explicit LiveHistorySubmitter(EditorSessionHistoryPort* history,
                                alcedo::EditorHistoryGuardHandle handle,
                                alcedo::EditorRenderCoordinator* coordinator)
      : history_(history), handle_(handle), coordinator_(coordinator) {}

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    control_value_at_submit_ = control_value_reader_ ? control_value_reader_() : 0.0;
    if (clock_ != nullptr) {
      clock_->set_ns(submit_ns_);
    }
    {
      std::lock_guard lock(mutex_);
      submit_entered_ = true;
    }
    submit_cv_.notify_all();

    alcedo::EditorAdjustmentPatch patch;
    patch.field_key   = fieldKey.toStdString();
    patch.settled     = settled;
    const auto doc    = QJsonDocument::fromJson(paramsJson.toUtf8());
    const auto object = doc.object();
    double     value  = object.value(QStringLiteral("value")).toDouble();
    if (object.contains(QStringLiteral("exposure"))) {
      value = object.value(QStringLiteral("exposure")).toDouble();
    }
    patch.params_json = nlohmann::json{{"exposure_ev", value}}.dump();

    std::string error;
    const bool  captured = history_->CaptureAdjustmentBeforePreview(handle_, patch, &error);
    if (clock_ != nullptr) {
      clock_->set_ns(apply_ns_);
    }
    live_after_capture_ = live_reader_ ? live_reader_() : 0.0f;
    if (!captured) {
      last_error_ = error;
      return false;
    }
    if (coordinator_ != nullptr) {
      alcedo::EditorRenderIntent intent;
      intent.element_id               = handle_.element_id;
      intent.image_id                 = 84;
      intent.image_load_request_id    = alcedo::ImageLoadRequestId{1};
      intent.quality                  = alcedo::EditorRenderQuality::Interactive;
      intent.priority                 = alcedo::EditorRenderPriority::Normal;
      intent.frame_role               = alcedo::FrameRoleForQuality(intent.quality);
      intent.reason                   = settled ? alcedo::EditorRenderReason::SettledAdjustment
                                                : alcedo::EditorRenderReason::InteractiveAdjustment;
      coordinator_->Submit(intent);
    }
    return true;
  }

  auto canEdit() const -> bool override { return can_edit_; }

  void WaitUntilSubmitEntered() {
    std::unique_lock lock(mutex_);
    submit_cv_.wait(lock, [&] { return submit_entered_; });
  }

  std::function<double()> control_value_reader_;
  std::function<float()>  live_reader_;
  alcedo::test::ManualMonotonicClock* clock_ = nullptr;
  alcedo::test::ManualMonotonicClock::nanoseconds submit_ns_ = 1;
  alcedo::test::ManualMonotonicClock::nanoseconds apply_ns_  = 2;
  double control_value_at_submit_ = 0.0;
  float  live_after_capture_      = 0.0f;
  bool   can_edit_                = true;
  std::string last_error_;

 private:
  EditorSessionHistoryPort*            history_     = nullptr;
  alcedo::EditorHistoryGuardHandle     handle_{};
  alcedo::EditorRenderCoordinator*     coordinator_ = nullptr;
  std::mutex                           mutex_;
  std::condition_variable              submit_cv_;
  bool                                 submit_entered_ = false;
};

class SerialInputBoundaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("serial_input_" + stamp + ".wal");
    guard_        = MakePipelineGuard(42);
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [g = guard_](sl_element_id_t) { return g; }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
    std::string error;
    handle_ = history_.Acquire(42, &error);
    ASSERT_TRUE(handle_.valid) << error;
    scheduler_   = std::make_shared<alcedo::test::LatchBlockedPipelineSchedulerPort>();
    coordinator_ = std::make_unique<alcedo::EditorRenderCoordinator>(scheduler_);
    coordinator_->SetActiveImageLoadRequest(1);
  }

  void TearDown() override {
    if (handle_.valid) {
      history_.Release(handle_);
    }
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                                          journal_path_;
  std::shared_ptr<alcedo::PipelineGuard>                         guard_;
  std::shared_ptr<EditorSessionPipelinePort>                     pipeline_;
  EditorSessionHistoryPort                                       history_;
  alcedo::EditorHistoryGuardHandle                               handle_{};
  std::shared_ptr<alcedo::test::LatchBlockedPipelineSchedulerPort> scheduler_;
  std::unique_ptr<alcedo::EditorRenderCoordinator>               coordinator_;
};

TEST_F(SerialInputBoundaryTest,
       PointerDragUpdatesControlValueBeforeLiveWriteAndMutatesLiveBeforeRenderCompletes) {
  LiveHistorySubmitter submitter(&history_, handle_, coordinator_.get());
  alcedo::test::ManualMonotonicClock clock;
  submitter.clock_ = &clock;
  auto model       = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setValue(0.0);
  submitter.control_value_reader_ = [&] { return model->value(); };
  submitter.live_reader_          = [&] { return DocumentExposureEv(*guard_->document_); };

  const float live_before = DocumentExposureEv(*guard_->document_);
  EXPECT_FLOAT_EQ(live_before, alcedo::kDefaultPipelineExposureEv);

  std::unique_lock render_held(guard_->pipeline_->GetRenderLock());
  std::thread      drag([&] {
    model->beginDrag();
    model->updateDrag(0.5);
  });
  submitter.WaitUntilSubmitEntered();
  EXPECT_DOUBLE_EQ(model->value(), 0.5);
  EXPECT_DOUBLE_EQ(submitter.control_value_at_submit_, 0.5);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), live_before);
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_TRUE(scheduler_->scheduled().empty());

  render_held.unlock();
  drag.join();

  EXPECT_TRUE(submitter.last_error_.empty()) << submitter.last_error_;
  EXPECT_FLOAT_EQ(submitter.live_after_capture_, 0.5f);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), 0.5f);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_TRUE(scheduler_->running());
  EXPECT_EQ(scheduler_->scheduled().size(), 1u);
  EXPECT_LT(submitter.submit_ns_, submitter.apply_ns_);
  EXPECT_GE(clock.now_ns(), submitter.apply_ns_);

  clock.advance_ns(16'000'000);
  EXPECT_TRUE(scheduler_->running())
      << "advancing a test clock must not complete the blocked renderer";
  scheduler_->Complete(true, "frame");
  EXPECT_FALSE(scheduler_->running());
}

TEST_F(SerialInputBoundaryTest,
       InteractivePatchMutatesLiveDocumentWhileCoordinatorStillHasInflightFrame) {
  LiveHistorySubmitter submitter(&history_, handle_, coordinator_.get());
  auto                 model = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setValue(0.0);
  submitter.control_value_reader_ = [&] { return model->value(); };
  submitter.live_reader_          = [&] { return DocumentExposureEv(*guard_->document_); };

  model->beginDrag();
  model->updateDrag(0.25);
  ASSERT_TRUE(coordinator_->has_inflight());
  ASSERT_TRUE(scheduler_->running());
  const auto inflight_id = coordinator_->last_scheduled_request_id();

  model->updateDrag(0.75);
  EXPECT_DOUBLE_EQ(model->value(), 0.75);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), 0.75f);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->last_scheduled_request_id(), inflight_id);
  EXPECT_EQ(scheduler_->scheduled().size(), 1u);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  scheduler_->Complete(true, "frame");
  if (scheduler_->running()) {
    scheduler_->Complete(true, "queued");
  }
}

TEST_F(SerialInputBoundaryTest, ImageExifDisplayFieldsAreOwnedByImageNotPipelineDocument) {
  alcedo::Image image(11);
  alcedo::ExifDisplayMetaData meta;
  meta.shutter_speed_ = {1, 250};
  meta.iso_           = 100;
  meta.aperture_      = 2.8f;
  meta.focal_         = 50.0f;
  meta.focal_35mm_    = 75.0f;
  image.SetExifDisplayMetaData(std::move(meta));

  EXPECT_TRUE(image.has_exif_display_.load());
  EXPECT_EQ(image.exif_display_.shutter_speed_.first, 1);
  EXPECT_EQ(image.exif_display_.shutter_speed_.second, 250);
  EXPECT_EQ(image.exif_display_.iso_, 100u);
  EXPECT_FLOAT_EQ(image.exif_display_.aperture_, 2.8f);
  EXPECT_FLOAT_EQ(image.exif_display_.focal_, 50.0f);
  EXPECT_NE(image.exif_display_.focal_, image.exif_display_.focal_35mm_);
  EXPECT_EQ(guard_->document_->ToJson().dump().find("shutter_speed"), std::string::npos);
  EXPECT_EQ(guard_->document_->ToJson().dump().find("\"iso\""), std::string::npos);
}

}  // namespace
}  // namespace alcedo::ui
