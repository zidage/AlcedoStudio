//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Slider and typed-model input enqueue without live document mutation.
/// GUI callbacks update local controls and admit change descriptions; they do
/// not take the render lock or capture history.

#include "app/editor_pending_input.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
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

class QueuedInputSubmitter final : public QObject, public IEditorAdjustmentSubmitter {
 public:
  explicit QueuedInputSubmitter(alcedo::EditorPendingInputQueue* queue,
                                alcedo::EditorSessionIdentity    identity)
      : queue_(queue), identity_(identity) {}

  auto submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
      -> bool override {
    if (!can_edit_ || queue_ == nullptr) {
      return false;
    }
    alcedo::EditorAdjustmentPatch patch;
    patch.field_key = fieldKey.toStdString();
    patch.write     = std::move(write);
    patch.settled   = settled;
    if (target_.owner_kind != alcedo::EditorParameterOwnerKind::Unspecified) {
      patch.target           = target_;
      patch.target.field_key = patch.field_key;
    }
    const auto admitted = queue_->AdmitFieldChange(identity_, std::move(patch));
    last_error_         = admitted.error;
    return admitted.accepted;
  }

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    nlohmann::json parsed;
    try {
      parsed = paramsJson.isEmpty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(paramsJson.toStdString());
    } catch (const std::exception&) {
      return false;
    }
    std::string error;
    auto        write = alcedo::ParseEditorParameterWrite(fieldKey.toStdString(), parsed, &error);
    if (!write.has_value()) {
      last_error_ = error;
      return false;
    }
    return submitWrite(std::move(fieldKey), std::move(*write), settled);
  }

  auto canEdit() const -> bool override { return can_edit_; }

  alcedo::EditorParameterTarget target_{};
  bool                          can_edit_ = true;
  std::string                   last_error_;

 private:
  alcedo::EditorPendingInputQueue* queue_ = nullptr;
  alcedo::EditorSessionIdentity    identity_{};
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
    identity_.element_id = 42;
    identity_.image_id   = 84;
  }

  void TearDown() override {
    if (handle_.valid) {
      history_.Release(handle_);
    }
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                  journal_path_;
  std::shared_ptr<alcedo::PipelineGuard> guard_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort               history_;
  alcedo::EditorHistoryGuardHandle       handle_{};
  alcedo::EditorSessionIdentity          identity_{};
  alcedo::EditorPendingInputQueue        queue_;
};

TEST_F(SerialInputBoundaryTest, SliderMovesWhileBlockedRenderLeaveLiveParametersUnchanged) {
  QueuedInputSubmitter submitter(&queue_, identity_);
  auto                 model = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setValue(0.0);

  const float live_before = DocumentExposureEv(*guard_->document_);
  EXPECT_FLOAT_EQ(live_before, alcedo::kDefaultPipelineExposureEv);
  const auto live_json_before = guard_->document_->ToJson().dump();

  std::unique_lock render_held(guard_->pipeline_->GetRenderLock());
  std::thread      drag([&] {
    model->beginDrag();
    model->updateDrag(0.10);
    model->updateDrag(0.20);
    model->updateDrag(0.30);
    model->updateDrag(0.50);
  });
  drag.join();

  EXPECT_TRUE(submitter.last_error_.empty()) << submitter.last_error_;
  EXPECT_DOUBLE_EQ(model->value(), 0.50);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), live_before);
  EXPECT_EQ(guard_->document_->ToJson().dump(), live_json_before);
  const auto pending = queue_.Peek();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().seal, alcedo::EditorPendingInputBoundaryKind::None);
  const auto* exposure = alcedo::FindPendingField(pending, "exposure");
  ASSERT_NE(exposure, nullptr);
  EXPECT_EQ(alcedo::PendingScalarValue(*exposure), 0.50f);
  EXPECT_EQ(exposure->identity.element_id, identity_.element_id);
  EXPECT_EQ(exposure->identity.image_id, identity_.image_id);

  render_held.unlock();
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), live_before);
  EXPECT_EQ(guard_->document_->ToJson().dump(), live_json_before);
}

TEST_F(SerialInputBoundaryTest, PendingDifferentFieldsSurviveInputCoalescing) {
  QueuedInputSubmitter exposure_submitter(&queue_, identity_);
  QueuedInputSubmitter contrast_submitter(&queue_, identity_);
  auto                 exposure = std::make_unique<EditorAdjustmentValueModel>();
  exposure->setSubmitter(&exposure_submitter);
  exposure->setFieldKey("exposure");
  exposure->setMinimum(-5.0);
  exposure->setMaximum(5.0);
  exposure->setValue(0.0);
  auto contrast = std::make_unique<EditorAdjustmentValueModel>();
  contrast->setSubmitter(&contrast_submitter);
  contrast->setFieldKey("contrast");
  contrast->setMinimum(-100.0);
  contrast->setMaximum(100.0);
  contrast->setValue(0.0);

  exposure->beginDrag();
  exposure->updateDrag(0.25);
  exposure->updateDrag(0.75);
  contrast->beginDrag();
  contrast->updateDrag(12.0);
  contrast->updateDrag(18.0);

  EXPECT_DOUBLE_EQ(exposure->value(), 0.75);
  EXPECT_DOUBLE_EQ(contrast->value(), 18.0);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), alcedo::kDefaultPipelineExposureEv);

  const auto pending = queue_.Peek();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().fields.size(), 2u);
  const auto* exposure_field = alcedo::FindPendingField(pending, "exposure");
  const auto* contrast_field = alcedo::FindPendingField(pending, "contrast");
  ASSERT_NE(exposure_field, nullptr);
  ASSERT_NE(contrast_field, nullptr);
  EXPECT_EQ(alcedo::PendingScalarValue(*exposure_field), 0.75f);
  EXPECT_EQ(alcedo::PendingScalarValue(*contrast_field), 18.0f);
}

TEST_F(SerialInputBoundaryTest, ReleaseBeforeFirstPreviewKeepsFinalQueuedValuesOnce) {
  QueuedInputSubmitter submitter(&queue_, identity_);
  auto                 model = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setDefaultValue(1.25);
  model->setValue(0.0);
  submitter.last_error_.clear();

  model->reset();

  EXPECT_DOUBLE_EQ(model->value(), 1.25);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), alcedo::kDefaultPipelineExposureEv);
  const auto pending = queue_.Peek();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().seal, alcedo::EditorPendingInputBoundaryKind::Release);
  EXPECT_EQ(pending.sequences.front().fields.size(), 1u);
  const auto* exposure = alcedo::FindPendingField(pending, "exposure");
  ASSERT_NE(exposure, nullptr);
  EXPECT_EQ(alcedo::PendingScalarValue(*exposure), 1.25f);
}

TEST_F(SerialInputBoundaryTest, NodeSwitchKeepsQueuedEditOnOriginalTarget) {
  QueuedInputSubmitter submitter(&queue_, identity_);
  submitter.target_.owner_kind             = alcedo::EditorParameterOwnerKind::ColorGrade;
  submitter.target_.node_id                = alcedo::NodeId{"grade.a"};
  submitter.target_.adjustment_instance_id = alcedo::AdjustmentInstanceId{"tone"};
  auto model = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setValue(0.0);

  model->beginDrag();
  model->updateDrag(0.40);
  const auto sealed =
      queue_.AdmitBoundary(identity_, alcedo::EditorPendingInputBoundaryKind::NodeSwitch);
  ASSERT_TRUE(sealed.accepted) << sealed.error;
  submitter.target_.node_id = alcedo::NodeId{"grade.b"};
  model->updateDrag(0.90);

  const auto pending = queue_.Peek();
  ASSERT_EQ(pending.sequences.size(), 2u);
  EXPECT_EQ(pending.sequences[0].captured_target.node_id, alcedo::NodeId{"grade.a"});
  EXPECT_EQ(pending.sequences[0].seal, alcedo::EditorPendingInputBoundaryKind::NodeSwitch);
  ASSERT_EQ(pending.sequences[0].fields.size(), 1u);
  EXPECT_EQ(pending.sequences[0].fields.front().target.node_id, alcedo::NodeId{"grade.a"});
  EXPECT_EQ(alcedo::PendingScalarValue(pending.sequences[0].fields.front()), 0.4f);
  EXPECT_EQ(pending.sequences[1].captured_target.node_id, alcedo::NodeId{"grade.b"});
  ASSERT_FALSE(pending.sequences[1].fields.empty());
  EXPECT_EQ(pending.sequences[1].fields.front().target.node_id, alcedo::NodeId{"grade.b"});
}

TEST_F(SerialInputBoundaryTest, TypedModelsEnqueueThroughSameOwnerRuleWithoutLiveWrite) {
  QueuedInputSubmitter submitter(&queue_, identity_);
  auto                 value = std::make_unique<EditorAdjustmentValueModel>();
  value->setSubmitter(&submitter);
  value->setFieldKey("exposure");
  value->setMinimum(-5.0);
  value->setMaximum(5.0);
  value->setValue(0.0);
  value->editValue(0.15);

  auto enum_model = std::make_unique<EditorAdjustmentEnumModel>();
  enum_model->setSubmitter(&submitter);
  enum_model->setFieldKey("color_temp_mode");
  QVariantMap as_shot;
  as_shot["value"] = QStringLiteral("as_shot");
  as_shot["label"] = QStringLiteral("As Shot");
  QVariantMap custom;
  custom["value"] = QStringLiteral("custom");
  custom["label"] = QStringLiteral("Custom");
  enum_model->setEntries(QVariantList{as_shot, custom});
  enum_model->setCurrentIndex(0);
  enum_model->selectIndex(1);

  auto toggle = std::make_unique<EditorAdjustmentToggleModel>();
  toggle->setSubmitter(&submitter);
  toggle->setFieldKey("lens_calib_enabled");
  toggle->setValue(false);
  toggle->commitValue(true);

  auto curve = std::make_unique<EditorToneCurveModel>();
  curve->setSubmitter(&submitter);
  curve->setFieldKey("curve");
  curve->beginDrag(0);
  curve->updateDrag(0.0, 0.25);

  auto lut = std::make_unique<EditorLutCatalogModel>();
  lut->setSubmitter(&submitter);
  lut->setFieldKey("lut");
  lut->selectPath(QStringLiteral("C:/luts/look.cube"));

  auto color_temp = std::make_unique<EditorColorTempModel>();
  color_temp->setSubmitter(&submitter);
  color_temp->editCct(6500.0);

  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), alcedo::kDefaultPipelineExposureEv);
  const auto pending = queue_.Peek();
  ASSERT_FALSE(pending.sequences.empty());
  EXPECT_NE(alcedo::FindPendingField(pending, "exposure"), nullptr);
  EXPECT_NE(alcedo::FindPendingField(pending, "color_temp_mode"), nullptr);
  EXPECT_NE(alcedo::FindPendingField(pending, "lens_calib_enabled"), nullptr);
  EXPECT_NE(alcedo::FindPendingField(pending, "curve"), nullptr);
  EXPECT_NE(alcedo::FindPendingField(pending, "lut"), nullptr);
  EXPECT_NE(alcedo::FindPendingField(pending, "color_temp"), nullptr);
  for (const auto& sequence : pending.sequences) {
    EXPECT_EQ(sequence.identity.element_id, identity_.element_id);
    EXPECT_EQ(sequence.identity.image_id, identity_.image_id);
  }
}

TEST_F(SerialInputBoundaryTest, ReleaseBeforeFirstPreviewCommitsFinalValuesOnce) {
  QueuedInputSubmitter submitter(&queue_, identity_);
  auto                 model = std::make_unique<EditorAdjustmentValueModel>();
  model->setSubmitter(&submitter);
  model->setFieldKey("exposure");
  model->setMinimum(-5.0);
  model->setMaximum(5.0);
  model->setDefaultValue(1.25);
  model->setValue(0.0);
  model->reset();

  auto batch = queue_.TakeReadyBatch();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->seal, alcedo::EditorPendingInputBoundaryKind::Release);

  auto history_ptr = std::shared_ptr<alcedo::IEditorHistoryPort>(
      static_cast<alcedo::IEditorHistoryPort*>(&history_), [](alcedo::IEditorHistoryPort*) {});
  alcedo::EditorSessionEditController edit({history_ptr, nullptr});
  const auto outcome = edit.HandlePendingSequence(*batch, handle_, identity_);
  EXPECT_EQ(outcome.kind, alcedo::EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(outcome.reason, alcedo::EditorRenderReason::SettledAdjustment);
  EXPECT_TRUE(outcome.render_command.live_parameters_applied);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), 1.25f);

  std::string error;
  ASSERT_TRUE(history_.Undo(handle_, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), alcedo::kDefaultPipelineExposureEv);
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
