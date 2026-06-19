//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <algorithm>
#include <utility>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/pipeline/raw_pipeline_adapter.hpp"
#include "ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr char kLocalizedTextProperty[]      = "puerhlabI18nText";
constexpr char kLocalizedTextUpperProperty[] = "puerhlabI18nTextUpper";

void           SetLocalizedText(QObject* object, const char* source, bool uppercase = false) {
  if (!object || source == nullptr) {
    return;
  }
  object->setProperty(kLocalizedTextProperty, source);
  object->setProperty(kLocalizedTextUpperProperty, uppercase);
  QString text = Tr(source);
  if (uppercase) {
    text = text.toUpper();
  }
  if (auto* label = qobject_cast<QLabel*>(object)) {
    label->setText(text);
  } else if (auto* button = qobject_cast<QPushButton*>(object)) {
    button->setText(text);
  } else if (auto* check_box = qobject_cast<QCheckBox*>(object)) {
    check_box->setText(text);
  }
}

auto NewLocalizedLabel(const char* source, QWidget* parent, bool uppercase = false) -> QLabel* {
  auto* label = new QLabel(parent);
  SetLocalizedText(label, source, uppercase);
  return label;
}

auto NewLocalizedCheckBox(const char* source, QWidget* parent) -> QCheckBox* {
  auto* check_box = new QCheckBox(parent);
  SetLocalizedText(check_box, source);
  return check_box;
}

auto AddRawSection(QWidget* parent, QVBoxLayout& layout, const char* title_source,
                   const char* subtitle_source) -> QVBoxLayout* {
  auto* frame = new QFrame(parent);
  frame->setObjectName("EditorSection");
  auto* v = new QVBoxLayout(frame);
  v->setContentsMargins(12, 10, 12, 10);
  v->setSpacing(8);

  auto* title = NewLocalizedLabel(title_source, frame);
  title->setObjectName("EditorSectionTitle");
  auto* subtitle = NewLocalizedLabel(subtitle_source, frame);
  subtitle->setObjectName("EditorSectionSub");
  subtitle->setWordWrap(true);
  v->addWidget(title, 0);
  v->addWidget(subtitle, 0);
  layout.insertWidget(layout.count() - 1, frame);
  return v;
}

}  // namespace

RawDecodePanelWidget::RawDecodePanelWidget(QWidget* parent) : AdjustmentPanelWidget(parent) {}

void RawDecodePanelWidget::Configure(Dependencies deps, Callbacks callbacks) {
  deps_      = std::move(deps);
  callbacks_ = std::move(callbacks);
  PullRawStateFromDialog();
  PullCommittedRawStateFromDialog();
}

void RawDecodePanelWidget::SetSyncing(bool syncing) { local_syncing_ = syncing; }

auto RawDecodePanelWidget::IsSyncing() const -> bool {
  return local_syncing_ || (callbacks_.is_global_syncing && callbacks_.is_global_syncing());
}

void RawDecodePanelWidget::RequestPipelineRender() {
  if (callbacks_.request_render) {
    callbacks_.request_render();
  }
}

void RawDecodePanelWidget::ProjectRawStateToDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  auto& s                       = *deps_.dialog_state;
  s.raw_highlights_reconstruct_ = raw_state_.raw_highlights_reconstruct_;
  s.lens_calib_enabled_         = raw_state_.lens_calib_enabled_;
  s.lens_override_make_         = raw_state_.lens_override_make_;
  s.lens_override_model_        = raw_state_.lens_override_model_;
}

void RawDecodePanelWidget::PullRawStateFromDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  const auto& s                          = *deps_.dialog_state;
  raw_state_.raw_highlights_reconstruct_ = s.raw_highlights_reconstruct_;
  raw_state_.lens_calib_enabled_         = s.lens_calib_enabled_;
  raw_state_.lens_override_make_         = s.lens_override_make_;
  raw_state_.lens_override_model_        = s.lens_override_model_;
}

void RawDecodePanelWidget::PullCommittedRawStateFromDialog() {
  if (!deps_.dialog_committed_state) {
    return;
  }
  const auto& s                                    = *deps_.dialog_committed_state;
  committed_raw_state_.raw_highlights_reconstruct_ = s.raw_highlights_reconstruct_;
  committed_raw_state_.lens_calib_enabled_         = s.lens_calib_enabled_;
  committed_raw_state_.lens_override_make_         = s.lens_override_make_;
  committed_raw_state_.lens_override_model_        = s.lens_override_model_;
}

void RawDecodePanelWidget::PreviewRawField(AdjustmentField field) {
  ProjectRawStateToDialog();
  if (!deps_.session) {
    return;
  }
  deps_.session->Preview(AdjustmentPreview{
      .field  = field,
      .params = RawPipelineAdapter::ParamsFor(field, raw_state_, deps_.session->Pipeline()),
      .policy = PreviewPolicy::FastViewport,
  });
}

void RawDecodePanelWidget::CommitRawField(AdjustmentField field) {
  ProjectRawStateToDialog();
  if (!deps_.session) {
    PullCommittedRawStateFromDialog();
    RequestPipelineRender();
    return;
  }

  if (!RawPipelineAdapter::FieldChanged(field, raw_state_, committed_raw_state_)) {
    deps_.session->Commit(field);
    PullCommittedRawStateFromDialog();
    RequestPipelineRender();
    return;
  }

  deps_.session->Commit(AdjustmentCommit{
      .field      = field,
      .old_params = RawPipelineAdapter::ParamsFor(field, committed_raw_state_,
                                                  deps_.session->Pipeline()),
      .new_params = RawPipelineAdapter::ParamsFor(field, raw_state_, deps_.session->Pipeline()),
  });
  PullCommittedRawStateFromDialog();
  RequestPipelineRender();
}

void RawDecodePanelWidget::Build() {
  if (!deps_.panel_layout) {
    return;
  }

  auto* controls_header = NewLocalizedLabel("RAW Decode", this);
  controls_header->setObjectName("SectionTitle");
  controls_header->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
  AppTheme::MarkFontRole(controls_header, AppTheme::FontRole::UiHeadline);
  deps_.panel_layout->insertWidget(0, controls_header, 0);

  BuildDecodeSection();
  BuildLensSection();
  RefreshLensComboFromState();
}

void RawDecodePanelWidget::BuildDecodeSection() {
  if (!deps_.panel_layout) {
    return;
  }

  auto* decode_layout = AddRawSection(
      this, *deps_.panel_layout, "RAW Decode",
      "Configure RAW decode options. These settings are shared with thumbnail rendering.");

  raw_highlights_reconstruct_checkbox_ =
      NewLocalizedCheckBox("Enable Highlight Reconstruction", this);
  raw_highlights_reconstruct_checkbox_->setChecked(raw_state_.raw_highlights_reconstruct_);
  raw_highlights_reconstruct_checkbox_->setStyleSheet(AppTheme::EditorCheckBoxStyle());
  QObject::connect(raw_highlights_reconstruct_checkbox_, &QCheckBox::toggled, this,
                   [this](bool checked) {
                     if (IsSyncing()) {
                       return;
                     }
                     raw_state_.raw_highlights_reconstruct_ = checked;
                     PreviewRawField(AdjustmentField::RawDecode);
                     CommitRawField(AdjustmentField::RawDecode);
                   });
  decode_layout->addWidget(raw_highlights_reconstruct_checkbox_, 0);
}

void RawDecodePanelWidget::BuildLensSection() {
  if (!deps_.panel_layout) {
    return;
  }

  auto* lens_layout = AddRawSection(
      this, *deps_.panel_layout, "Lens Calibration",
      "Enable correction and optionally override lens metadata with catalog entries.");

  lens_calib_enabled_checkbox_ = NewLocalizedCheckBox("Enable Lens Calibration", this);
  lens_calib_enabled_checkbox_->setChecked(raw_state_.lens_calib_enabled_);
  lens_calib_enabled_checkbox_->setStyleSheet(AppTheme::EditorCheckBoxStyle());
  QObject::connect(lens_calib_enabled_checkbox_, &QCheckBox::toggled, this, [this](bool checked) {
    if (IsSyncing()) {
      return;
    }
    raw_state_.lens_calib_enabled_ = checked;
    PreviewRawField(AdjustmentField::LensCalib);
    CommitRawField(AdjustmentField::LensCalib);
  });
  lens_layout->addWidget(lens_calib_enabled_checkbox_, 0);

  auto* brand_row        = new QWidget(this);
  auto* brand_row_layout = new QHBoxLayout(brand_row);
  brand_row_layout->setContentsMargins(0, 0, 0, 0);
  brand_row_layout->setSpacing(8);

  auto* brand_label = NewLocalizedLabel("Lens Brand", brand_row);
  brand_label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
  AppTheme::MarkFontRole(brand_label, AppTheme::FontRole::UiCaption);

  lens_brand_combo_ = new QComboBox(brand_row);
  lens_brand_combo_->setMinimumWidth(0);
  lens_brand_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  lens_brand_combo_->setStyleSheet(AppTheme::EditorComboBoxStyle());
  QObject::connect(
      lens_brand_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (IsSyncing() || !lens_brand_combo_) {
          return;
        }
        raw_state_.lens_override_make_ = lens_brand_combo_->currentData().toString().toStdString();
        if (raw_state_.lens_override_make_.empty()) {
          raw_state_.lens_override_model_.clear();
        }
        RefreshLensModelComboFromState();
        PreviewRawField(AdjustmentField::LensCalib);
        CommitRawField(AdjustmentField::LensCalib);
      });
  brand_row_layout->addWidget(brand_label, 0);
  brand_row_layout->addWidget(lens_brand_combo_, 1);
  lens_layout->addWidget(brand_row, 0);

  auto* model_row        = new QWidget(this);
  auto* model_row_layout = new QHBoxLayout(model_row);
  model_row_layout->setContentsMargins(0, 0, 0, 0);
  model_row_layout->setSpacing(8);

  auto* model_label = NewLocalizedLabel("Lens Model", model_row);
  model_label->setStyleSheet(brand_label->styleSheet());
  AppTheme::MarkFontRole(model_label, AppTheme::FontRole::UiCaption);

  lens_model_combo_ = new QComboBox(model_row);
  lens_model_combo_->setMinimumWidth(0);
  lens_model_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  lens_model_combo_->setStyleSheet(lens_brand_combo_->styleSheet());
  QObject::connect(
      lens_model_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (IsSyncing() || !lens_model_combo_ || raw_state_.lens_override_make_.empty()) {
          return;
        }
        raw_state_.lens_override_model_ = lens_model_combo_->currentData().toString().toStdString();
        PreviewRawField(AdjustmentField::LensCalib);
        CommitRawField(AdjustmentField::LensCalib);
      });
  model_row_layout->addWidget(model_label, 0);
  model_row_layout->addWidget(lens_model_combo_, 1);
  lens_layout->addWidget(model_row, 0);

  lens_catalog_status_label_ = new QLabel(this);
  lens_catalog_status_label_->setWordWrap(true);
  lens_catalog_status_label_->setStyleSheet(
      AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(lens_catalog_status_label_, AppTheme::FontRole::UiHint);
  lens_layout->addWidget(lens_catalog_status_label_, 0);
}

void RawDecodePanelWidget::EnsureLensCatalogLoaded() {
  if (!lens_catalog_.brands_.empty() || !lens_catalog_.models_by_brand_.empty()) {
    return;
  }
  lens_catalog_ = lens_calib::LoadLensCatalog();
}

void RawDecodePanelWidget::RefreshLensBrandComboFromState() {
  if (!lens_brand_combo_) {
    return;
  }
  EnsureLensCatalogLoaded();

  const bool prev_sync = local_syncing_;
  local_syncing_       = true;

  lens_brand_combo_->clear();
  lens_brand_combo_->addItem(Tr("Auto (metadata)"), QString());
  for (const auto& brand : lens_catalog_.brands_) {
    lens_brand_combo_->addItem(QString::fromStdString(brand), QString::fromStdString(brand));
  }

  int selected_index = 0;
  if (!raw_state_.lens_override_make_.empty()) {
    selected_index =
        lens_brand_combo_->findData(QString::fromStdString(raw_state_.lens_override_make_));
    if (selected_index < 0) {
      lens_brand_combo_->addItem(QString::fromStdString(raw_state_.lens_override_make_),
                                 QString::fromStdString(raw_state_.lens_override_make_));
      selected_index = lens_brand_combo_->count() - 1;
    }
  }
  lens_brand_combo_->setCurrentIndex(std::max(0, selected_index));

  local_syncing_ = prev_sync;
}

void RawDecodePanelWidget::RefreshLensModelComboFromState() {
  if (!lens_model_combo_) {
    return;
  }
  EnsureLensCatalogLoaded();

  const bool prev_sync = local_syncing_;
  local_syncing_       = true;

  lens_model_combo_->clear();

  if (raw_state_.lens_override_make_.empty()) {
    lens_model_combo_->addItem(Tr("Auto (metadata)"), QString());
    lens_model_combo_->setCurrentIndex(0);
    lens_model_combo_->setEnabled(false);
    raw_state_.lens_override_model_.clear();
  } else {
    std::vector<std::string> models;
    if (const auto it = lens_catalog_.models_by_brand_.find(raw_state_.lens_override_make_);
        it != lens_catalog_.models_by_brand_.end()) {
      models = it->second;
    }
    if (!raw_state_.lens_override_model_.empty() &&
        std::find(models.begin(), models.end(), raw_state_.lens_override_model_) == models.end()) {
      models.push_back(raw_state_.lens_override_model_);
    }
    lens_calib::SortAndUniqueStrings(&models);
    for (const auto& model : models) {
      lens_model_combo_->addItem(QString::fromStdString(model), QString::fromStdString(model));
    }

    int selected_index = 0;
    if (!raw_state_.lens_override_model_.empty()) {
      selected_index =
          lens_model_combo_->findData(QString::fromStdString(raw_state_.lens_override_model_));
    }
    if (selected_index < 0 && lens_model_combo_->count() > 0) {
      selected_index = 0;
    }
    lens_model_combo_->setCurrentIndex(selected_index);
    lens_model_combo_->setEnabled(lens_model_combo_->count() > 0);

    if (lens_model_combo_->count() > 0) {
      raw_state_.lens_override_model_ = lens_model_combo_->currentData().toString().toStdString();
    } else {
      raw_state_.lens_override_model_.clear();
    }
  }

  if (lens_catalog_status_label_) {
    if (lens_catalog_.brands_.empty()) {
      lens_catalog_status_label_->setText(
          Tr("Lens catalog not found. You can still use Auto (metadata) mode."));
    } else {
      lens_catalog_status_label_->setText(
          Tr("Lens catalog: %1 brands").arg(static_cast<int>(lens_catalog_.brands_.size())));
    }
  }

  local_syncing_ = prev_sync;
}

void RawDecodePanelWidget::RefreshLensComboFromState() {
  RefreshLensBrandComboFromState();
  RefreshLensModelComboFromState();
  ProjectRawStateToDialog();
}

void RawDecodePanelWidget::SyncControlsFromDialogState() {
  PullRawStateFromDialog();
  PullCommittedRawStateFromDialog();

  const bool prev_sync = local_syncing_;
  local_syncing_       = true;
  if (raw_highlights_reconstruct_checkbox_) {
    raw_highlights_reconstruct_checkbox_->setChecked(raw_state_.raw_highlights_reconstruct_);
  }
  if (lens_calib_enabled_checkbox_) {
    lens_calib_enabled_checkbox_->setChecked(raw_state_.lens_calib_enabled_);
  }
  local_syncing_ = prev_sync;
  RefreshLensComboFromState();
}

void RawDecodePanelWidget::RetranslateUi() { RefreshLensComboFromState(); }

void RawDecodePanelWidget::LoadFromPipeline() {
  if (callbacks_.load_from_pipeline) {
    const auto loaded_state = callbacks_.load_from_pipeline(raw_state_);
    if (loaded_state.has_value()) {
      raw_state_           = *loaded_state;
      committed_raw_state_ = *loaded_state;
      ProjectRawStateToDialog();
      if (deps_.dialog_committed_state) {
        deps_.dialog_committed_state->raw_highlights_reconstruct_ =
            committed_raw_state_.raw_highlights_reconstruct_;
        deps_.dialog_committed_state->lens_calib_enabled_ =
            committed_raw_state_.lens_calib_enabled_;
        deps_.dialog_committed_state->lens_override_make_ =
            committed_raw_state_.lens_override_make_;
        deps_.dialog_committed_state->lens_override_model_ =
            committed_raw_state_.lens_override_model_;
      }
      SyncControlsFromDialogState();
      return;
    }
  }
  if (deps_.session && deps_.session->LoadFromPipeline()) {
    SyncControlsFromDialogState();
  }
}

void RawDecodePanelWidget::ReloadFromCommittedState() { SyncControlsFromDialogState(); }

}  // namespace alcedo::ui
