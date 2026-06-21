//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/widgets/look_control_panel_widget.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSizePolicy>
#include <QTextEdit>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/editor_slider_styling.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"
#include "ui/alcedo_main/editor_dialog/modules/hls.hpp"
#include "ui/alcedo_main/editor_dialog/pipeline/look_pipeline_adapter.hpp"
#include "ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/lut_browser_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/trackball.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr char kLocalizedTextProperty[]      = "puerhlabI18nText";
constexpr char kLocalizedTextUpperProperty[] = "puerhlabI18nTextUpper";
constexpr char kLocalizedToolTipProperty[]   = "puerhlabI18nToolTip";

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
  }
}

auto NewLocalizedLabel(const char* source, QWidget* parent, bool uppercase = false) -> QLabel* {
  auto* label = new QLabel(parent);
  SetLocalizedText(label, source, uppercase);
  return label;
}

void RetranslateMarkedLookObjects(QObject* root) {
  if (!root) {
    return;
  }
  const QVariant text_source = root->property(kLocalizedTextProperty);
  if (text_source.isValid()) {
    QString text = Tr(text_source.toString().toUtf8().constData());
    if (root->property(kLocalizedTextUpperProperty).toBool()) {
      text = text.toUpper();
    }
    if (auto* label = qobject_cast<QLabel*>(root)) {
      label->setText(text);
    } else if (auto* button = qobject_cast<QPushButton*>(root)) {
      button->setText(text);
    }
  }
  const QVariant tooltip_source = root->property(kLocalizedToolTipProperty);
  if (tooltip_source.isValid()) {
    if (auto* widget = qobject_cast<QWidget*>(root)) {
      const QString tooltip = Tr(tooltip_source.toString().toUtf8().constData());
      widget->setToolTip(tooltip);
      widget->setAccessibleName(tooltip);
    }
  }
  for (QObject* child : root->children()) {
    RetranslateMarkedLookObjects(child);
  }
}

auto HlsCandidateColor(float hue_degrees) -> QColor { return hls::CandidateColor(hue_degrees); }

auto MakeLookSection(QWidget* parent, QVBoxLayout& layout, const char* title_source,
                     const char* subtitle_source) -> QVBoxLayout* {
  CollapsibleSectionWidget::Options options;
  options.title_source    = title_source;
  options.subtitle_source = subtitle_source;
  options.chrome          = CollapsibleSectionWidget::Chrome::Divider;

  auto* section           = new CollapsibleSectionWidget(options, parent);
  layout.insertWidget(layout.count() - 1, section, 0);
  return section->ContentLayout();
}

}  // namespace

LookControlPanelWidget::LookControlPanelWidget(QWidget* parent) : AdjustmentPanelWidget(parent) {
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(10, 8, 10, 10);
  layout_->setSpacing(8);
  layout_->addStretch();
}

void LookControlPanelWidget::Configure(Dependencies deps, Callbacks callbacks) {
  deps_      = std::move(deps);
  callbacks_ = std::move(callbacks);
  PullLookStateFromDialog();
  PullCommittedLookStateFromDialog();
}

void LookControlPanelWidget::SetSyncing(bool syncing) { local_syncing_ = syncing; }

auto LookControlPanelWidget::IsSyncing() const -> bool {
  return local_syncing_ || (callbacks_.is_global_syncing && callbacks_.is_global_syncing());
}

bool LookControlPanelWidget::eventFilter(QObject* obj, QEvent* event) {
  if (event && event->type() == QEvent::MouseButtonDblClick) {
    if (auto* slider = qobject_cast<QSlider*>(obj)) {
      const auto it = slider_reset_callbacks_.find(slider);
      if (it != slider_reset_callbacks_.end()) {
        if (!IsSyncing() && it->second) {
          it->second();
        }
        return true;
      }
    }
  }
  return AdjustmentPanelWidget::eventFilter(obj, event);
}

void LookControlPanelWidget::RegisterSliderReset(QSlider* slider, std::function<void()> on_reset) {
  if (!slider || !on_reset) {
    return;
  }
  slider->installEventFilter(this);
  slider_reset_callbacks_[slider] = std::move(on_reset);
}

void LookControlPanelWidget::RequestPipelineRender() {
  if (callbacks_.request_render) {
    callbacks_.request_render();
  }
}

void LookControlPanelWidget::CopyLookStateToDialogState(const LookAdjustmentState& look_state,
                                                        AdjustmentState&           state) {
  state.hls_target_hue_              = look_state.hls_target_hue_;
  state.hls_hue_adjust_              = look_state.hls_hue_adjust_;
  state.hls_lightness_adjust_        = look_state.hls_lightness_adjust_;
  state.hls_saturation_adjust_       = look_state.hls_saturation_adjust_;
  state.hls_hue_range_               = look_state.hls_hue_range_;
  state.lift_wheel_                  = look_state.lift_wheel_;
  state.gamma_wheel_                 = look_state.gamma_wheel_;
  state.gain_wheel_                  = look_state.gain_wheel_;
  state.hls_hue_adjust_table_        = look_state.hls_hue_adjust_table_;
  state.hls_lightness_adjust_table_  = look_state.hls_lightness_adjust_table_;
  state.hls_saturation_adjust_table_ = look_state.hls_saturation_adjust_table_;
  state.hls_hue_range_table_         = look_state.hls_hue_range_table_;
  state.lut_path_                    = look_state.lut_path_;
}

void LookControlPanelWidget::ProjectLookStateToDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  CopyLookStateToDialogState(look_state_, *deps_.dialog_state);
}

void LookControlPanelWidget::PullLookStateFromDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  const auto& s                            = *deps_.dialog_state;
  look_state_.hls_target_hue_              = s.hls_target_hue_;
  look_state_.hls_hue_adjust_              = s.hls_hue_adjust_;
  look_state_.hls_lightness_adjust_        = s.hls_lightness_adjust_;
  look_state_.hls_saturation_adjust_       = s.hls_saturation_adjust_;
  look_state_.hls_hue_range_               = s.hls_hue_range_;
  look_state_.lift_wheel_                  = s.lift_wheel_;
  look_state_.gamma_wheel_                 = s.gamma_wheel_;
  look_state_.gain_wheel_                  = s.gain_wheel_;
  look_state_.hls_hue_adjust_table_        = s.hls_hue_adjust_table_;
  look_state_.hls_lightness_adjust_table_  = s.hls_lightness_adjust_table_;
  look_state_.hls_saturation_adjust_table_ = s.hls_saturation_adjust_table_;
  look_state_.hls_hue_range_table_         = s.hls_hue_range_table_;
  look_state_.lut_path_                    = s.lut_path_;
  LoadActiveHlsProfile();
}

void LookControlPanelWidget::PullCommittedLookStateFromDialog() {
  if (!deps_.dialog_committed_state) {
    return;
  }
  const auto& s                                      = *deps_.dialog_committed_state;
  committed_look_state_.hls_target_hue_              = s.hls_target_hue_;
  committed_look_state_.hls_hue_adjust_              = s.hls_hue_adjust_;
  committed_look_state_.hls_lightness_adjust_        = s.hls_lightness_adjust_;
  committed_look_state_.hls_saturation_adjust_       = s.hls_saturation_adjust_;
  committed_look_state_.hls_hue_range_               = s.hls_hue_range_;
  committed_look_state_.lift_wheel_                  = s.lift_wheel_;
  committed_look_state_.gamma_wheel_                 = s.gamma_wheel_;
  committed_look_state_.gain_wheel_                  = s.gain_wheel_;
  committed_look_state_.hls_hue_adjust_table_        = s.hls_hue_adjust_table_;
  committed_look_state_.hls_lightness_adjust_table_  = s.hls_lightness_adjust_table_;
  committed_look_state_.hls_saturation_adjust_table_ = s.hls_saturation_adjust_table_;
  committed_look_state_.hls_hue_range_table_         = s.hls_hue_range_table_;
  committed_look_state_.lut_path_                    = s.lut_path_;
}

void LookControlPanelWidget::PreviewLookField(AdjustmentField field) {
  ProjectLookStateToDialog();
  RequestPipelineRender();
  if (!deps_.session) {
    return;
  }
  deps_.session->Preview(AdjustmentPreview{
      .field  = field,
      .params = LookPipelineAdapter::ParamsFor(field, look_state_),
      .policy = PreviewPolicy::FastViewport,
  });
}

void LookControlPanelWidget::CommitLookField(AdjustmentField field) {
  ProjectLookStateToDialog();
  if (!deps_.session) {
    PullCommittedLookStateFromDialog();
    return;
  }

  if (!LookPipelineAdapter::FieldChanged(field, look_state_, committed_look_state_)) {
    deps_.session->Commit(field);
    PullCommittedLookStateFromDialog();
    return;
  }

  deps_.session->Commit(AdjustmentCommit{
      .field      = field,
      .old_params = LookPipelineAdapter::ParamsFor(field, committed_look_state_),
      .new_params = LookPipelineAdapter::ParamsFor(field, look_state_),
  });
  PullCommittedLookStateFromDialog();
}

auto LookControlPanelWidget::ActiveHlsProfileIndex() const -> int {
  return std::clamp(hls::ClosestCandidateHueIndex(look_state_.hls_target_hue_), 0,
                    static_cast<int>(hls::kCandidateHues.size()) - 1);
}

void LookControlPanelWidget::SaveActiveHlsProfile() {
  const int idx                                 = ActiveHlsProfileIndex();
  look_state_.hls_hue_adjust_table_[idx]        = look_state_.hls_hue_adjust_;
  look_state_.hls_lightness_adjust_table_[idx]  = look_state_.hls_lightness_adjust_;
  look_state_.hls_saturation_adjust_table_[idx] = look_state_.hls_saturation_adjust_;
  look_state_.hls_hue_range_table_[idx]         = look_state_.hls_hue_range_;
}

void LookControlPanelWidget::LoadActiveHlsProfile() {
  const int idx                      = ActiveHlsProfileIndex();
  look_state_.hls_hue_adjust_        = look_state_.hls_hue_adjust_table_[idx];
  look_state_.hls_lightness_adjust_  = look_state_.hls_lightness_adjust_table_[idx];
  look_state_.hls_saturation_adjust_ = look_state_.hls_saturation_adjust_table_[idx];
  look_state_.hls_hue_range_         = look_state_.hls_hue_range_table_[idx];
}

void LookControlPanelWidget::ResetHlsField(
    const std::function<void(LookAdjustmentState&, const AdjustmentState&)>& apply_default) {
  if (!apply_default || !callbacks_.default_adjustment_state) {
    return;
  }
  apply_default(look_state_, callbacks_.default_adjustment_state());
  SaveActiveHlsProfile();
  ProjectLookStateToDialog();
  SyncControlsFromDialogState();
  PreviewLookField(AdjustmentField::Hls);
  CommitLookField(AdjustmentField::Hls);
}

void LookControlPanelWidget::Build() {
  if (built_ || !layout_) {
    return;
  }
  built_       = true;

  auto* header = NewLocalizedLabel("Color", this);
  header->setObjectName("SectionTitle");
  header->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
  AppTheme::MarkFontRole(header, AppTheme::FontRole::UiHeadline);
  layout_->insertWidget(0, header, 0);

  BuildLutSection();
  BuildHlsSection();
  BuildCdlSection();
  SyncControlsFromDialogState();
}

void LookControlPanelWidget::BuildLutSection() {
  if (!layout_) {
    return;
  }
  auto* section_layout = MakeLookSection(this, *layout_, "LUT", "Browse and apply look-up tables.");
  lut_browser_widget_  = new LutBrowserWidget(this);
  section_layout->addWidget(lut_browser_widget_, 0);

  QObject::connect(lut_browser_widget_, &LutBrowserWidget::RefreshRequested, this,
                   [this]() { RefreshLutBrowserUi(true); });
  QObject::connect(lut_browser_widget_, &LutBrowserWidget::OpenFolderRequested, this, [this]() {
    const auto&     directory = lut_controller_.directory();
    std::error_code ec;
    if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) {
      QMessageBox::warning(this, Tr("LUT"), Tr("LUT folder is unavailable."));
      return;
    }
    if (!QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdWString(directory.wstring())))) {
      QMessageBox::warning(this, Tr("LUT"), Tr("Failed to open the LUT folder."));
    }
  });
  QObject::connect(lut_browser_widget_, &LutBrowserWidget::LutPathActivated, this,
                   [this](const QString& entry_path) {
                     const auto resolved_path = lut_controller_.TryResolveSelection(entry_path);
                     if (!resolved_path.has_value() || *resolved_path == look_state_.lut_path_) {
                       return;
                     }
                     look_state_.lut_path_ = *resolved_path;
                     ProjectLookStateToDialog();
                     CommitLookField(AdjustmentField::Lut);
                     RefreshLutBrowserUi(false, true);
                   });
  RefreshLutBrowserUi(false);
}

void LookControlPanelWidget::BuildHlsSection() {
  if (!layout_) {
    return;
  }

  auto* section_layout =
      MakeLookSection(this, *layout_, "HSL / Color", "Per-hue lightness and chroma adjustments.");
  auto* frame = new QWidget(this);
  auto* v     = new QVBoxLayout(frame);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);

  hls_target_label_ = new QLabel(frame);
  hls_target_label_->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
  AppTheme::MarkFontRole(hls_target_label_, AppTheme::FontRole::UiCaption);
  v->addWidget(hls_target_label_, 0);

  auto* swatch_row        = new QWidget(frame);
  auto* swatch_row_layout = new QHBoxLayout(swatch_row);
  swatch_row_layout->setContentsMargins(0, 0, 0, 0);
  swatch_row_layout->setSpacing(6);

  hls_candidate_buttons_.clear();
  hls_candidate_buttons_.reserve(hls::kCandidateHues.size());
  for (int i = 0; i < static_cast<int>(hls::kCandidateHues.size()); ++i) {
    auto* btn = new QPushButton(swatch_row);
    btn->setFixedSize(20, 20);
    btn->setCursor(Qt::PointingHandCursor);
    QObject::connect(btn, &QPushButton::clicked, this, [this, i]() {
      if (IsSyncing()) {
        return;
      }
      SaveActiveHlsProfile();
      look_state_.hls_target_hue_ = hls::kCandidateHues[static_cast<size_t>(i)];
      LoadActiveHlsProfile();
      ProjectLookStateToDialog();
      SyncControlsFromDialogState();
    });
    hls_candidate_buttons_.push_back(btn);
    swatch_row_layout->addWidget(btn);
  }
  swatch_row_layout->addStretch();
  v->addWidget(swatch_row, 0);
  section_layout->addWidget(frame, 0);

  const QString value_chip_style =
      QStringLiteral("QLabel { color: %1; background: transparent; border: none; padding: 0; }")
          .arg(AppTheme::Instance().textMutedColor().name(QColor::HexRgb));

  auto add_slider = [this, section_layout, value_chip_style](
                        const char* name_source, int min, int max, int value, auto&& on_change,
                        auto&& on_release, auto&& on_reset, auto&& formatter) {
    auto* name_label = NewLocalizedLabel(name_source, this);
    name_label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(name_label, AppTheme::FontRole::UiCaption);
    name_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* value_chip = new QLabel(formatter(value), this);
    value_chip->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_chip->setMinimumWidth(40);
    value_chip->setMaximumWidth(72);
    value_chip->setFixedHeight(16);
    value_chip->setStyleSheet(value_chip_style);
    AppTheme::MarkFontRole(value_chip, AppTheme::FontRole::DataCaption);

    auto* slider = new AccentBalanceSlider(kRegularSliderMetrics, this);
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setSingleStep(1);
    slider->setPageStep(std::max(1, (max - min) / 20));
    slider->setMinimumWidth(0);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    slider->setFixedHeight(22);

    QObject::connect(slider, &QSlider::valueChanged, this,
                     [this, value_chip, formatter,
                      on_change = std::forward<decltype(on_change)>(on_change)](int v) {
                       value_chip->setText(formatter(v));
                       if (IsSyncing()) {
                         return;
                       }
                       on_change(v);
                     });
    QObject::connect(slider, &QSlider::sliderReleased, this,
                     [this, on_release = std::forward<decltype(on_release)>(on_release)]() {
                       if (!IsSyncing()) {
                         on_release();
                       }
                     });
    RegisterSliderReset(slider,
                        [this, on_reset = std::forward<decltype(on_reset)>(on_reset)]() mutable {
                          if (!IsSyncing()) {
                            on_reset();
                          }
                        });

    auto* head_row    = new QWidget(this);
    auto* head_layout = new QHBoxLayout(head_row);
    head_layout->setContentsMargins(0, 0, 0, 0);
    head_layout->setSpacing(8);
    head_layout->addWidget(name_label, 1);
    head_layout->addWidget(value_chip, 0, Qt::AlignRight | Qt::AlignVCenter);

    auto* row        = new QWidget(this);
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(2);
    row_layout->addWidget(head_row, 0);
    row_layout->addWidget(slider, 0);

    section_layout->addWidget(row, 0);
    return slider;
  };

  hls_hue_adjust_slider_ = add_slider(
      "Hue Shift", -30, 30, static_cast<int>(std::lround(look_state_.hls_hue_adjust_)),
      [this](int v) {
        look_state_.hls_hue_adjust_ =
            std::clamp(static_cast<float>(v), -hls::kMaxHueShiftDegrees, hls::kMaxHueShiftDegrees);
        SaveActiveHlsProfile();
        PreviewLookField(AdjustmentField::Hls);
      },
      [this]() { CommitLookField(AdjustmentField::Hls); },
      [this]() {
        ResetHlsField([](LookAdjustmentState& state, const AdjustmentState& defaults) {
          state.hls_hue_adjust_ = defaults.hls_hue_adjust_;
        });
      },
      [](int v) { return QString("%1 deg").arg(v); });

  hls_lightness_adjust_slider_ = add_slider(
      "Lightness", -100, 100, static_cast<int>(std::lround(look_state_.hls_lightness_adjust_)),
      [this](int v) {
        look_state_.hls_lightness_adjust_ =
            std::clamp(static_cast<float>(v), hls::kAdjUiMin, hls::kAdjUiMax);
        SaveActiveHlsProfile();
        PreviewLookField(AdjustmentField::Hls);
      },
      [this]() { CommitLookField(AdjustmentField::Hls); },
      [this]() {
        ResetHlsField([](LookAdjustmentState& state, const AdjustmentState& defaults) {
          state.hls_lightness_adjust_ = defaults.hls_lightness_adjust_;
        });
      },
      [](int v) { return QString::number(v, 'f', 0); });

  hls_saturation_adjust_slider_ = add_slider(
      "Chroma", -100, 100, static_cast<int>(std::lround(look_state_.hls_saturation_adjust_)),
      [this](int v) {
        look_state_.hls_saturation_adjust_ =
            std::clamp(static_cast<float>(v), hls::kAdjUiMin, hls::kAdjUiMax);
        SaveActiveHlsProfile();
        PreviewLookField(AdjustmentField::Hls);
      },
      [this]() { CommitLookField(AdjustmentField::Hls); },
      [this]() {
        ResetHlsField([](LookAdjustmentState& state, const AdjustmentState& defaults) {
          state.hls_saturation_adjust_ = defaults.hls_saturation_adjust_;
        });
      },
      [](int v) { return QString::number(v, 'f', 0); });

  hls_hue_range_slider_ = add_slider(
      "Hue Smoothness", 1, 180, static_cast<int>(std::lround(look_state_.hls_hue_range_)),
      [this](int v) {
        look_state_.hls_hue_range_ = static_cast<float>(v);
        SaveActiveHlsProfile();
        PreviewLookField(AdjustmentField::Hls);
      },
      [this]() { CommitLookField(AdjustmentField::Hls); },
      [this]() {
        ResetHlsField([](LookAdjustmentState& state, const AdjustmentState& defaults) {
          state.hls_hue_range_ = defaults.hls_hue_range_;
        });
      },
      [](int v) { return QString("%1 deg").arg(v); });

  RefreshHlsTargetUi();
}

void LookControlPanelWidget::BuildCdlSection() {
  if (!layout_) {
    return;
  }

  auto* section_layout = MakeLookSection(this, *layout_, "Color Wheels",
                                         "CDL: Lift / Gamma / Gain with master offset.");

  auto* wheel_frame    = new QWidget(this);
  wheel_frame->setMinimumWidth(0);
  wheel_frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  auto* wheel_layout = new QVBoxLayout(wheel_frame);
  wheel_layout->setContentsMargins(0, 4, 0, 0);
  wheel_layout->setSpacing(10);

  auto make_wheel_unit = [this, wheel_frame](
                             const char* title_source, CdlWheelState& wheel_state, bool add_unity,
                             bool invert_delta, CdlTrackballDiscWidget*& disc_widget,
                             QLabel*& offset_label, QSlider*& slider_widget) -> QWidget* {
    auto* unit = new QWidget(wheel_frame);
    unit->setMinimumWidth(0);
    unit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* unit_layout = new QVBoxLayout(unit);
    unit_layout->setContentsMargins(0, 0, 0, 0);
    unit_layout->setSpacing(4);

    auto* title_label = NewLocalizedLabel(title_source, unit);
    title_label->setStyleSheet(AppTheme::EditorLabelStyle(QColor(0xCF, 0xCF, 0xCF)));
    AppTheme::MarkFontRole(title_label, AppTheme::FontRole::UiOverline);
    unit_layout->addWidget(title_label, 0, Qt::AlignHCenter);

    CdlWheelState* wheel = &wheel_state;
    disc_widget          = new CdlTrackballDiscWidget(unit);
    disc_widget->setMinimumSize(128, 128);
    disc_widget->setMaximumSize(180, 180);
    disc_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    disc_widget->SetPosition(wheel->disc_position_);
    disc_widget->SetPositionChangedCallback(
        [this, wheel, add_unity, invert_delta](const QPointF& pos) {
          if (IsSyncing()) {
            return;
          }
          wheel->disc_position_ = color_wheel::ClampDiscPoint(pos);
          UpdateCdlWheelDerivedColor(*wheel, add_unity, invert_delta);
          RefreshCdlOffsetLabels();
          PreviewLookField(AdjustmentField::ColorWheel);
        });
    disc_widget->SetPositionReleasedCallback(
        [this, wheel, add_unity, invert_delta](const QPointF& pos) {
          if (IsSyncing()) {
            return;
          }
          wheel->disc_position_ = color_wheel::ClampDiscPoint(pos);
          UpdateCdlWheelDerivedColor(*wheel, add_unity, invert_delta);
          RefreshCdlOffsetLabels();
          PreviewLookField(AdjustmentField::ColorWheel);
          CommitLookField(AdjustmentField::ColorWheel);
        });
    unit_layout->addWidget(disc_widget, 0, Qt::AlignHCenter);

    offset_label = new QLabel(FormatWheelDeltaText(*wheel, add_unity), unit);
    offset_label->setStyleSheet(AppTheme::EditorLabelStyle(QColor(0xA9, 0xA9, 0xA9)));
    AppTheme::MarkFontRole(offset_label, AppTheme::FontRole::DataCaption);
    offset_label->setAlignment(Qt::AlignHCenter);
    unit_layout->addWidget(offset_label, 0);

    slider_widget = new AccentBalanceSlider(kCompactSliderMetrics, unit);
    slider_widget->setRange(color_wheel::kSliderUiMin, color_wheel::kSliderUiMax);
    const float sign = invert_delta ? -1.0f : 1.0f;
    slider_widget->setValue(color_wheel::CdlMasterToSliderUi(wheel->master_offset_ * sign));
    slider_widget->setSingleStep(1);
    slider_widget->setPageStep(100);
    slider_widget->setFixedHeight(14);
    slider_widget->setMinimumWidth(0);
    slider_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QObject::connect(slider_widget, &QSlider::valueChanged, unit, [this, wheel, sign](int value) {
      if (IsSyncing()) {
        return;
      }
      wheel->master_offset_ = color_wheel::CdlSliderUiToMaster(value) * sign;
      RefreshCdlOffsetLabels();
      PreviewLookField(AdjustmentField::ColorWheel);
    });
    QObject::connect(slider_widget, &QSlider::sliderReleased, unit, [this]() {
      if (!IsSyncing()) {
        CommitLookField(AdjustmentField::ColorWheel);
      }
    });
    RegisterSliderReset(slider_widget, [this, wheel]() {
      if (IsSyncing()) {
        return;
      }
      wheel->master_offset_ = 0.0f;
      ProjectLookStateToDialog();
      SyncControlsFromDialogState();
      PreviewLookField(AdjustmentField::ColorWheel);
      CommitLookField(AdjustmentField::ColorWheel);
    });
    unit_layout->addWidget(slider_widget, 0);
    return unit;
  };

  auto* gamma_unit = make_wheel_unit("Gamma", look_state_.gamma_wheel_, true, true,
                                     gamma_disc_widget_, gamma_offset_label_, gamma_master_slider_);
  auto* lift_unit  = make_wheel_unit("Lift", look_state_.lift_wheel_, false, false,
                                     lift_disc_widget_, lift_offset_label_, lift_master_slider_);
  auto* gain_unit = make_wheel_unit("Gain", look_state_.gain_wheel_, true, false, gain_disc_widget_,
                                    gain_offset_label_, gain_master_slider_);

  auto* top_row   = new QWidget(wheel_frame);
  auto* top_row_layout = new QHBoxLayout(top_row);
  top_row_layout->setContentsMargins(0, 0, 0, 0);
  top_row_layout->setSpacing(0);
  top_row_layout->addStretch(1);
  top_row_layout->addWidget(gamma_unit, 2);
  top_row_layout->addStretch(1);
  wheel_layout->addWidget(top_row, 0);

  auto* bottom_row        = new QWidget(wheel_frame);
  auto* bottom_row_layout = new QHBoxLayout(bottom_row);
  bottom_row_layout->setContentsMargins(0, 0, 0, 0);
  bottom_row_layout->setSpacing(10);
  bottom_row_layout->addWidget(lift_unit, 1);
  bottom_row_layout->addWidget(gain_unit, 1);
  wheel_layout->addWidget(bottom_row, 0);

  section_layout->addWidget(wheel_frame, 0);
  RefreshCdlOffsetLabels();
}

void LookControlPanelWidget::RefreshHlsTargetUi() {
  const float hue = hls::WrapHueDegrees(look_state_.hls_target_hue_);
  if (hls_target_label_) {
    hls_target_label_->setText(Tr("Target Hue: %1 deg").arg(hue, 0, 'f', 0));
  }

  const int selected_idx = hls::ClosestCandidateHueIndex(hue);
  for (int i = 0; i < static_cast<int>(hls_candidate_buttons_.size()); ++i) {
    auto* btn = hls_candidate_buttons_[i];
    if (!btn) {
      continue;
    }
    const bool    selected   = (i == selected_idx);
    const QColor  swatch     = HlsCandidateColor(hls::kCandidateHues[static_cast<size_t>(i)]);
    const auto    border_w   = selected ? "3px" : "1px";
    const QString border_col = selected
                                   ? AppTheme::Instance().accentColor().name(QColor::HexRgb)
                                   : AppTheme::Instance().glassStrokeColor().name(QColor::HexArgb);
    btn->setToolTip(Tr("Hue %1 deg").arg(hls::kCandidateHues[static_cast<size_t>(i)], 0, 'f', 0));
    btn->setStyleSheet(QString("QPushButton {"
                               "  background: %1;"
                               "  border: %2 solid %3;"
                               "  border-radius: 11px;"
                               "}"
                               "QPushButton:hover {"
                               "  border-color: %4;"
                               "}")
                           .arg(swatch.name(QColor::HexRgb), border_w, border_col,
                                AppTheme::Instance().accentSecondaryColor().name(QColor::HexRgb)));
  }
}

void LookControlPanelWidget::RefreshCdlOffsetLabels() {
  if (lift_offset_label_) {
    lift_offset_label_->setText(FormatWheelDeltaText(look_state_.lift_wheel_, false));
  }
  if (gamma_offset_label_) {
    gamma_offset_label_->setText(FormatWheelDeltaText(look_state_.gamma_wheel_, true));
  }
  if (gain_offset_label_) {
    gain_offset_label_->setText(FormatWheelDeltaText(look_state_.gain_wheel_, true));
  }
}

void LookControlPanelWidget::SyncCdlControlsFromState() {
  if (lift_disc_widget_) {
    lift_disc_widget_->SetPosition(look_state_.lift_wheel_.disc_position_);
  }
  if (gamma_disc_widget_) {
    gamma_disc_widget_->SetPosition(look_state_.gamma_wheel_.disc_position_);
  }
  if (gain_disc_widget_) {
    gain_disc_widget_->SetPosition(look_state_.gain_wheel_.disc_position_);
  }
  if (lift_master_slider_) {
    lift_master_slider_->setValue(
        color_wheel::CdlMasterToSliderUi(look_state_.lift_wheel_.master_offset_));
  }
  if (gamma_master_slider_) {
    gamma_master_slider_->setValue(
        color_wheel::CdlMasterToSliderUi(-look_state_.gamma_wheel_.master_offset_));
  }
  if (gain_master_slider_) {
    gain_master_slider_->setValue(
        color_wheel::CdlMasterToSliderUi(look_state_.gain_wheel_.master_offset_));
  }
}

void LookControlPanelWidget::SyncControlsFromDialogState() {
  PullLookStateFromDialog();
  PullCommittedLookStateFromDialog();

  const bool prev_sync = local_syncing_;
  local_syncing_       = true;
  SyncCdlControlsFromState();
  if (hls_hue_adjust_slider_) {
    hls_hue_adjust_slider_->setValue(static_cast<int>(std::lround(look_state_.hls_hue_adjust_)));
  }
  if (hls_lightness_adjust_slider_) {
    hls_lightness_adjust_slider_->setValue(
        static_cast<int>(std::lround(look_state_.hls_lightness_adjust_)));
  }
  if (hls_saturation_adjust_slider_) {
    hls_saturation_adjust_slider_->setValue(
        static_cast<int>(std::lround(look_state_.hls_saturation_adjust_)));
  }
  if (hls_hue_range_slider_) {
    hls_hue_range_slider_->setValue(static_cast<int>(std::lround(look_state_.hls_hue_range_)));
  }
  local_syncing_ = prev_sync;

  RefreshHlsTargetUi();
  RefreshCdlOffsetLabels();
  RefreshLutBrowserUi(false);
}

void LookControlPanelWidget::RetranslateUi() {
  RetranslateMarkedLookObjects(this);
  if (lut_browser_widget_) {
    lut_browser_widget_->RetranslateUi();
  }
  RefreshHlsTargetUi();
  RefreshLutBrowserUi(false);
}

void LookControlPanelWidget::RefreshLutBrowserUi(bool force_refresh,
                                                 bool preserve_scroll_position) {
  const auto view_model = lut_controller_.Refresh(look_state_.lut_path_, force_refresh);
  if (!lut_browser_widget_) {
    return;
  }
  lut_browser_widget_->SetDirectoryInfo(view_model.directory_text_, view_model.status_text_,
                                        view_model.can_open_directory_);
  lut_browser_widget_->SetEntries(view_model.entries_, view_model.selected_path_,
                                  preserve_scroll_position);
}

auto LookControlPanelWidget::DefaultLutPath() -> std::string {
  (void)lut_controller_.Refresh(look_state_.lut_path_, false);
  return lut_controller_.DefaultLutPath();
}

void LookControlPanelWidget::ClearAppliedLutPath() { last_applied_lut_path_.clear(); }

auto LookControlPanelWidget::ShouldApplyLutPath(const std::string& lut_path) const -> bool {
  return lut_path != last_applied_lut_path_;
}

void LookControlPanelWidget::MarkAppliedLutPath(const std::string& lut_path) {
  last_applied_lut_path_ = lut_path;
}

auto LookControlPanelWidget::SelectRelativeLut(int step) -> bool {
  return lut_browser_widget_ && lut_browser_widget_->SelectRelativeEntry(step);
}

auto LookControlPanelWidget::CanHandleLutNavigationShortcut(QWidget* focus_widget) const -> bool {
  if (!lut_browser_widget_ || QApplication::activePopupWidget()) {
    return false;
  }
  if (!focus_widget || !isAncestorOf(focus_widget)) {
    return true;
  }
  if (qobject_cast<QComboBox*>(focus_widget) != nullptr ||
      qobject_cast<QAbstractSpinBox*>(focus_widget) != nullptr ||
      qobject_cast<QTextEdit*>(focus_widget) != nullptr ||
      qobject_cast<QPlainTextEdit*>(focus_widget) != nullptr) {
    return false;
  }
  if (auto* line_edit = qobject_cast<QLineEdit*>(focus_widget)) {
    return lut_browser_widget_->isAncestorOf(line_edit);
  }
  return true;
}

void LookControlPanelWidget::LoadFromPipeline() {
  if (callbacks_.load_from_pipeline) {
    const auto loaded_state = callbacks_.load_from_pipeline(look_state_);
    if (loaded_state.has_value()) {
      look_state_           = *loaded_state;
      committed_look_state_ = *loaded_state;
      ProjectLookStateToDialog();
      if (deps_.dialog_committed_state) {
        CopyLookStateToDialogState(committed_look_state_, *deps_.dialog_committed_state);
      }
      SyncControlsFromDialogState();
      return;
    }
  }
  if (deps_.session && deps_.session->LoadFromPipeline()) {
    SyncControlsFromDialogState();
  }
}

void LookControlPanelWidget::ReloadFromCommittedState() { SyncControlsFromDialogState(); }

}  // namespace alcedo::ui
