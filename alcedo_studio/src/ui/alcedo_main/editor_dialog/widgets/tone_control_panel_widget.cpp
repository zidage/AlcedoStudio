//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <algorithm>
#include <cmath>
#include <utility>

#include "edit/operators/basic/color_temp_op.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/editor_slider_styling.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_dialog/modules/curve.hpp"
#include "ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr char kLocalizedTextProperty[]      = "puerhlabI18nText";
constexpr char kLocalizedTextUpperProperty[] = "puerhlabI18nTextUpper";
constexpr int  kWheelSliderSettledMs         = 180;

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

auto NewLocalizedButton(const char* source, QWidget* parent) -> QPushButton* {
  auto* button = new QPushButton(parent);
  SetLocalizedText(button, source);
  return button;
}

auto ColorTempSliderPosToCct(int pos) -> float { return color_temp::SliderPosToCct(pos); }
auto ColorTempCctToSliderPos(float cct) -> int { return color_temp::CctToSliderPos(cct); }

}  // namespace

ToneControlPanelWidget::ToneControlPanelWidget(QWidget* parent) : AdjustmentPanelWidget(parent) {}

void ToneControlPanelWidget::Configure(Dependencies deps, Callbacks callbacks) {
  deps_      = std::move(deps);
  callbacks_ = std::move(callbacks);
  PullToneStateFromDialog();
  PullCommittedToneStateFromDialog();
  PullColorTempStateFromDialog();
  PullCommittedColorTempStateFromDialog();
}

void ToneControlPanelWidget::SetSyncing(bool syncing) { local_syncing_ = syncing; }

auto ToneControlPanelWidget::IsSyncing() const -> bool {
  if (local_syncing_) {
    return true;
  }
  if (callbacks_.is_global_syncing && callbacks_.is_global_syncing()) {
    return true;
  }
  return false;
}

void ToneControlPanelWidget::RequestPipelineRender() {
  if (callbacks_.request_render) {
    callbacks_.request_render();
  }
}

void ToneControlPanelWidget::ProjectToneStateToDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  auto& s         = *deps_.dialog_state;
  s.exposure_     = tone_state_.exposure_;
  s.contrast_     = tone_state_.contrast_;
  s.blacks_       = tone_state_.blacks_;
  s.whites_       = tone_state_.whites_;
  s.shadows_      = tone_state_.shadows_;
  s.highlights_   = tone_state_.highlights_;
  s.curve_points_ = tone_state_.curve_points_;
  s.saturation_   = tone_state_.saturation_;
  s.sharpen_      = tone_state_.sharpen_;
  s.clarity_      = tone_state_.clarity_;
  s.film_grain_   = tone_state_.film_grain_;
  s.halation_     = tone_state_.halation_;
}

void ToneControlPanelWidget::PullToneStateFromDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  const auto& s             = *deps_.dialog_state;
  tone_state_.exposure_     = s.exposure_;
  tone_state_.contrast_     = s.contrast_;
  tone_state_.blacks_       = s.blacks_;
  tone_state_.whites_       = s.whites_;
  tone_state_.shadows_      = s.shadows_;
  tone_state_.highlights_   = s.highlights_;
  tone_state_.curve_points_ = s.curve_points_;
  tone_state_.saturation_   = s.saturation_;
  tone_state_.sharpen_      = s.sharpen_;
  tone_state_.clarity_      = s.clarity_;
  tone_state_.film_grain_   = s.film_grain_;
  tone_state_.halation_     = s.halation_;
}

void ToneControlPanelWidget::PullCommittedToneStateFromDialog() {
  if (!deps_.dialog_committed_state) {
    return;
  }
  const auto& s                       = *deps_.dialog_committed_state;
  committed_tone_state_.exposure_     = s.exposure_;
  committed_tone_state_.contrast_     = s.contrast_;
  committed_tone_state_.blacks_       = s.blacks_;
  committed_tone_state_.whites_       = s.whites_;
  committed_tone_state_.shadows_      = s.shadows_;
  committed_tone_state_.highlights_   = s.highlights_;
  committed_tone_state_.curve_points_ = s.curve_points_;
  committed_tone_state_.saturation_   = s.saturation_;
  committed_tone_state_.sharpen_      = s.sharpen_;
  committed_tone_state_.clarity_      = s.clarity_;
  committed_tone_state_.film_grain_   = s.film_grain_;
  committed_tone_state_.halation_     = s.halation_;
}

void ToneControlPanelWidget::ProjectColorTempStateToDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  auto& s                     = *deps_.dialog_state;
  s.color_temp_mode_          = color_temp_state_.mode_;
  s.color_temp_custom_cct_    = color_temp_state_.custom_cct_;
  s.color_temp_custom_tint_   = color_temp_state_.custom_tint_;
  s.color_temp_resolved_cct_  = color_temp_state_.resolved_cct_;
  s.color_temp_resolved_tint_ = color_temp_state_.resolved_tint_;
  s.color_temp_supported_     = color_temp_state_.supported_;
}

void ToneControlPanelWidget::PullColorTempStateFromDialog() {
  if (!deps_.dialog_state) {
    return;
  }
  const auto& s                    = *deps_.dialog_state;
  color_temp_state_.mode_          = s.color_temp_mode_;
  color_temp_state_.custom_cct_    = s.color_temp_custom_cct_;
  color_temp_state_.custom_tint_   = s.color_temp_custom_tint_;
  color_temp_state_.resolved_cct_  = s.color_temp_resolved_cct_;
  color_temp_state_.resolved_tint_ = s.color_temp_resolved_tint_;
  color_temp_state_.supported_     = s.color_temp_supported_;
}

void ToneControlPanelWidget::PullCommittedColorTempStateFromDialog() {
  if (!deps_.dialog_committed_state) {
    return;
  }
  const auto& s                              = *deps_.dialog_committed_state;
  committed_color_temp_state_.mode_          = s.color_temp_mode_;
  committed_color_temp_state_.custom_cct_    = s.color_temp_custom_cct_;
  committed_color_temp_state_.custom_tint_   = s.color_temp_custom_tint_;
  committed_color_temp_state_.resolved_cct_  = s.color_temp_resolved_cct_;
  committed_color_temp_state_.resolved_tint_ = s.color_temp_resolved_tint_;
  committed_color_temp_state_.supported_     = s.color_temp_supported_;
}

bool ToneControlPanelWidget::eventFilter(QObject* obj, QEvent* event) {
  if (!event) {
    return AdjustmentPanelWidget::eventFilter(obj, event);
  }

  if (event->type() == QEvent::Wheel) {
    if (auto* slider = qobject_cast<QSlider*>(obj);
        slider != nullptr && slider_settled_callbacks_.contains(slider)) {
      ScheduleWheelSliderSettled(slider);
    }
  }

  if (event->type() == QEvent::MouseButtonDblClick) {
    if (auto* slider = qobject_cast<QSlider*>(obj)) {
      const auto it = slider_reset_callbacks_.find(slider);
      if (it != slider_reset_callbacks_.end()) {
        if (!IsSyncing() && it->second) {
          it->second();
        }
        return true;
      }
    }
    if (dynamic_cast<ToneCurveWidget*>(obj) != nullptr && curve_reset_callback_) {
      if (!IsSyncing()) {
        curve_reset_callback_();
      }
      return true;
    }
  }
  return AdjustmentPanelWidget::eventFilter(obj, event);
}

void ToneControlPanelWidget::RegisterSliderReset(QSlider* slider, std::function<void()> on_reset) {
  if (!slider || !on_reset) {
    return;
  }
  slider->installEventFilter(this);
  slider_reset_callbacks_[slider] = std::move(on_reset);
}

void ToneControlPanelWidget::RegisterSliderSettled(QSlider*              slider,
                                                   std::function<void()> on_settled) {
  if (!slider || !on_settled) {
    return;
  }
  slider->installEventFilter(this);
  slider_settled_callbacks_[slider] = std::move(on_settled);
}

void ToneControlPanelWidget::ScheduleWheelSliderSettled(QSlider* slider) {
  if (!slider || IsSyncing()) {
    return;
  }
  pending_wheel_slider_ = slider;
  EnsureWheelSliderSettledTimer();
  wheel_slider_settled_timer_->start(kWheelSliderSettledMs);
}

void ToneControlPanelWidget::EnsureWheelSliderSettledTimer() {
  if (wheel_slider_settled_timer_) {
    return;
  }
  wheel_slider_settled_timer_ = new QTimer(this);
  wheel_slider_settled_timer_->setSingleShot(true);
  QObject::connect(wheel_slider_settled_timer_, &QTimer::timeout, this, [this]() {
    auto* slider          = pending_wheel_slider_;
    pending_wheel_slider_ = nullptr;
    if (IsSyncing() || !slider) {
      return;
    }
    const auto it = slider_settled_callbacks_.find(slider);
    if (it != slider_settled_callbacks_.end() && it->second) {
      it->second();
    }
  });
}

void ToneControlPanelWidget::RegisterCurveReset(ToneCurveWidget*      widget,
                                                std::function<void()> on_reset) {
  if (!widget || !on_reset) {
    return;
  }
  widget->installEventFilter(this);
  curve_reset_callback_ = std::move(on_reset);
}

void ToneControlPanelWidget::PreviewToneField(AdjustmentField field) {
  ProjectToneStateToDialog();
  RequestPipelineRender();
  if (deps_.session) {
    AdjustmentPreview preview{
        .field  = field,
        .params = nlohmann::json{},
        .policy = PreviewPolicy::FastViewport,
    };
    if (deps_.dialog_state) {
      preview.params = deps_.session->ParamsForField(field, *deps_.dialog_state);
    }
    deps_.session->Preview(preview);
  }
}

void ToneControlPanelWidget::CommitToneField(AdjustmentField field) {
  ProjectToneStateToDialog();
  if (deps_.session) {
    deps_.session->Commit(field);
  }
  PullCommittedToneStateFromDialog();
}

void ToneControlPanelWidget::ResetToneFieldToDefault(
    AdjustmentField                                                                field,
    const std::function<void(const ToneAdjustmentState&, const AdjustmentState&)>& apply_default) {
  if (!apply_default || !callbacks_.default_adjustment_state) {
    return;
  }
  const AdjustmentState& dialog_defaults = callbacks_.default_adjustment_state();
  ToneAdjustmentState    tone_defaults{};
  tone_defaults.exposure_     = dialog_defaults.exposure_;
  tone_defaults.contrast_     = dialog_defaults.contrast_;
  tone_defaults.blacks_       = dialog_defaults.blacks_;
  tone_defaults.whites_       = dialog_defaults.whites_;
  tone_defaults.shadows_      = dialog_defaults.shadows_;
  tone_defaults.highlights_   = dialog_defaults.highlights_;
  tone_defaults.curve_points_ = dialog_defaults.curve_points_;
  tone_defaults.saturation_   = dialog_defaults.saturation_;
  tone_defaults.sharpen_      = dialog_defaults.sharpen_;
  tone_defaults.clarity_      = dialog_defaults.clarity_;
  tone_defaults.film_grain_   = dialog_defaults.film_grain_;
  tone_defaults.halation_     = dialog_defaults.halation_;

  apply_default(tone_defaults, dialog_defaults);
  ProjectToneStateToDialog();
  if (callbacks_.sync_controls_from_state) {
    callbacks_.sync_controls_from_state();
  }
  RequestPipelineRender();
  CommitToneField(field);
}

void ToneControlPanelWidget::ResetCurveToDefaultLocal() {
  tone_state_.curve_points_ = curve::DefaultCurveControlPoints();
  ProjectToneStateToDialog();
  if (callbacks_.sync_controls_from_state) {
    callbacks_.sync_controls_from_state();
  }
  RequestPipelineRender();
  CommitToneField(AdjustmentField::Curve);
}

void ToneControlPanelWidget::PromoteColorTempToCustomForEditing() {
  PullColorTempStateFromDialog();
  if (color_temp_state_.mode_ == ColorTempMode::CUSTOM) {
    return;
  }
  if (!deps_.dialog_state) {
    return;
  }
  color_temp_state_.custom_cct_  = DisplayedColorTempCct(*deps_.dialog_state);
  color_temp_state_.custom_tint_ = DisplayedColorTempTint(*deps_.dialog_state);
  color_temp_state_.mode_        = ColorTempMode::CUSTOM;
  ProjectColorTempStateToDialog();

  if (color_temp_mode_combo_) {
    const bool prev = local_syncing_;
    local_syncing_  = true;
    color_temp_mode_combo_->setCurrentIndex(ColorTempModeToComboIndex(color_temp_state_.mode_));
    local_syncing_ = prev;
  }
}

void ToneControlPanelWidget::CacheAsShotColorTemp(float cct, float tint) {
  last_known_as_shot_cct_            = std::clamp(cct, static_cast<float>(color_temp::kCctMin),
                                                  static_cast<float>(color_temp::kCctMax));
  last_known_as_shot_tint_           = std::clamp(tint, static_cast<float>(color_temp::kTintMin),
                                                  static_cast<float>(color_temp::kTintMax));
  has_last_known_as_shot_color_temp_ = true;
}

void ToneControlPanelWidget::PrimeColorTempDisplayForAsShot() {
  if (!has_last_known_as_shot_color_temp_) {
    return;
  }
  PullColorTempStateFromDialog();
  color_temp_state_.resolved_cct_  = last_known_as_shot_cct_;
  color_temp_state_.resolved_tint_ = last_known_as_shot_tint_;
  ProjectColorTempStateToDialog();
  if (deps_.dialog_committed_state) {
    deps_.dialog_committed_state->color_temp_resolved_cct_  = last_known_as_shot_cct_;
    deps_.dialog_committed_state->color_temp_resolved_tint_ = last_known_as_shot_tint_;
  }
  PullCommittedColorTempStateFromDialog();
}

void ToneControlPanelWidget::WarmAsShotColorTempCacheFromPipeline(CPUPipelineExecutor* pipeline) {
  if (has_last_known_as_shot_color_temp_ || !pipeline) {
    return;
  }

  auto params = pipeline->GetGlobalParams();
  try {
    static const nlohmann::json kAsShotColorTempParams = {
        {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0f}, {"tint", 0.0f}}}};
    ColorTempOp as_shot_probe(kAsShotColorTempParams);
    as_shot_probe.SetGlobalParams(params);
  } catch (...) {
    return;
  }

  if (!params.color_temp_matrices_valid_) {
    return;
  }

  CacheAsShotColorTemp(params.color_temp_resolved_cct_, params.color_temp_resolved_tint_);
  PullColorTempStateFromDialog();
  if (color_temp_state_.mode_ == ColorTempMode::AS_SHOT) {
    PrimeColorTempDisplayForAsShot();
  }
}

auto ToneControlPanelWidget::RefreshColorTempRuntimeStateFromGlobalParams(
    CPUPipelineExecutor* pipeline) -> bool {
  if (!pipeline) {
    return false;
  }

  PullColorTempStateFromDialog();
  const auto& global = pipeline->GetGlobalParams();
  const float new_cct =
      std::clamp(global.color_temp_resolved_cct_, static_cast<float>(color_temp::kCctMin),
                 static_cast<float>(color_temp::kCctMax));
  const float new_tint =
      std::clamp(global.color_temp_resolved_tint_, static_cast<float>(color_temp::kTintMin),
                 static_cast<float>(color_temp::kTintMax));
  const bool new_sup = global.color_temp_matrices_valid_;

  const bool changed = !NearlyEqual(color_temp_state_.resolved_cct_, new_cct) ||
                       !NearlyEqual(color_temp_state_.resolved_tint_, new_tint) ||
                       color_temp_state_.supported_ != new_sup;

  color_temp_state_.resolved_cct_  = new_cct;
  color_temp_state_.resolved_tint_ = new_tint;
  color_temp_state_.supported_     = new_sup;
  ProjectColorTempStateToDialog();

  if (deps_.dialog_committed_state) {
    deps_.dialog_committed_state->color_temp_resolved_cct_  = new_cct;
    deps_.dialog_committed_state->color_temp_resolved_tint_ = new_tint;
    deps_.dialog_committed_state->color_temp_supported_     = new_sup;
  }
  PullCommittedColorTempStateFromDialog();
  if (color_temp_state_.mode_ == ColorTempMode::AS_SHOT && new_sup) {
    CacheAsShotColorTemp(new_cct, new_tint);
  }

  return changed;
}

void ToneControlPanelWidget::ResetColorTempToAsShot() {
  PullColorTempStateFromDialog();
  if (color_temp_state_.mode_ == ColorTempMode::AS_SHOT) {
    return;
  }
  color_temp_state_.mode_ = ColorTempMode::AS_SHOT;
  ProjectColorTempStateToDialog();
  PrimeColorTempDisplayForAsShot();
  SyncColorTempControlsFromDialogState();
  RequestPipelineRender();
  if (deps_.session) {
    deps_.session->Commit(AdjustmentField::ColorTemp);
  }
  PullCommittedColorTempStateFromDialog();
}

void ToneControlPanelWidget::ClearSubmittedColorTempRequest() {
  last_submitted_color_temp_request_.reset();
}

void ToneControlPanelWidget::MarkSubmittedColorTempRequest(const AdjustmentState& state) {
  last_submitted_color_temp_request_ = BuildColorTempRequest(state);
}

auto ToneControlPanelWidget::ShouldSubmitColorTempRequest(bool                   operator_missing,
                                                          const AdjustmentState& render_state) const
    -> bool {
  const auto color_temp_request = BuildColorTempRequest(render_state);
  return operator_missing || !last_submitted_color_temp_request_.has_value() ||
         !ColorTempRequestEqual(*last_submitted_color_temp_request_, color_temp_request);
}

void ToneControlPanelWidget::Build() {
  if (!deps_.panel_layout) {
    return;
  }

  auto* parent_widget   = this;
  auto& layout          = *deps_.panel_layout;

  auto* controls_header = NewLocalizedLabel("Adjustments", parent_widget);
  controls_header->setObjectName("SectionTitle");
  controls_header->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
  AppTheme::MarkFontRole(controls_header, AppTheme::FontRole::UiHeadline);
  layout.addWidget(controls_header, 0);
  layout.addStretch();

  BuildToneSection();
  BuildToneCurveSection();
  BuildColorSection();
  BuildDetailSection();
}

namespace {

auto AddSection(QWidget* parent, QVBoxLayout& layout, const char* title_source,
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

void ToneControlPanelWidget::BuildToneSection() {
  auto* parent_widget = this;
  auto& layout        = *deps_.panel_layout;
  auto* section_layout =
      AddSection(parent_widget, layout, "Tone", "Primary tonal shaping controls.");

  const QString value_chip_style =
      QStringLiteral(
          "QLabel {"
          "  color: %1;"
          "  background: transparent;"
          "  border: none;"
          "  padding: 0;"
          "}")
          .arg(AppTheme::Instance().textMutedColor().name(QColor::HexRgb));

  auto add_slider = [&, value_chip_style](
                        const char* name_source, int min, int max, int value,
                        std::function<void(int)> on_change, std::function<void()> on_release,
                        std::function<void()> on_reset, std::function<QString(int)> formatter,
                        SliderVisualStyle visual_style = SliderVisualStyle::Accent) -> QSlider* {
    auto* name_label = NewLocalizedLabel(name_source, parent_widget);
    name_label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(name_label, AppTheme::FontRole::UiCaption);
    name_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* value_chip = new QLabel(formatter(value), parent_widget);
    value_chip->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_chip->setMinimumWidth(40);
    value_chip->setMaximumWidth(72);
    value_chip->setFixedHeight(16);
    value_chip->setStyleSheet(value_chip_style);
    AppTheme::MarkFontRole(value_chip, AppTheme::FontRole::DataCaption);

    QSlider* slider = nullptr;
    if (visual_style == SliderVisualStyle::Accent) {
      slider = new AccentBalanceSlider(kRegularSliderMetrics, parent_widget);
    } else {
      slider = new QSlider(Qt::Horizontal, parent_widget);
    }
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setSingleStep(1);
    slider->setPageStep(std::max(1, (max - min) / 20));
    slider->setMinimumWidth(0);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    slider->setFixedHeight(22);

    QObject::connect(slider, &QSlider::valueChanged, parent_widget,
                     [this, value_chip, formatter, on_change](int v) {
                       value_chip->setText(formatter(v));
                       if (IsSyncing()) {
                         return;
                       }
                       on_change(v);
                     });

    QObject::connect(slider, &QSlider::sliderReleased, parent_widget, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderSettled(slider, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderReset(slider, [this, on_reset]() {
      if (IsSyncing()) {
        return;
      }
      on_reset();
    });

    auto* head_row    = new QWidget(parent_widget);
    auto* head_layout = new QHBoxLayout(head_row);
    head_layout->setContentsMargins(0, 0, 0, 0);
    head_layout->setSpacing(8);
    head_layout->addWidget(name_label, 1);
    head_layout->addWidget(value_chip, 0, Qt::AlignRight | Qt::AlignVCenter);

    auto* row        = new QWidget(parent_widget);
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(2);
    row_layout->addWidget(head_row, 0);
    row_layout->addWidget(slider, 0);

    section_layout->addWidget(row, 0);
    return slider;
  };

  exposure_slider_ = add_slider(
      "Exposure", -1000, 1000, static_cast<int>(std::lround(tone_state_.exposure_ * 100.0f)),
      [this](int v) {
        tone_state_.exposure_ = static_cast<float>(v) / 100.0f;
        PreviewToneField(AdjustmentField::Exposure);
      },
      [this]() { CommitToneField(AdjustmentField::Exposure); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Exposure,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.exposure_ = defaults.exposure_;
            });
      },
      [](int v) { return QString::number(v / 100.0, 'f', 2); });

  contrast_slider_ = add_slider(
      "Contrast", -100, 100, static_cast<int>(std::lround(tone_state_.contrast_)),
      [this](int v) {
        tone_state_.contrast_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Contrast);
      },
      [this]() { CommitToneField(AdjustmentField::Contrast); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Contrast,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.contrast_ = defaults.contrast_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  highlights_slider_ = add_slider(
      "Highlights", -100, 100, static_cast<int>(std::lround(tone_state_.highlights_)),
      [this](int v) {
        tone_state_.highlights_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Highlights);
      },
      [this]() { CommitToneField(AdjustmentField::Highlights); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Highlights,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.highlights_ = defaults.highlights_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  shadows_slider_ = add_slider(
      "Shadows", -100, 100, static_cast<int>(std::lround(tone_state_.shadows_)),
      [this](int v) {
        tone_state_.shadows_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Shadows);
      },
      [this]() { CommitToneField(AdjustmentField::Shadows); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Shadows,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.shadows_ = defaults.shadows_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  whites_slider_ = add_slider(
      "Whites", -100, 100, static_cast<int>(std::lround(tone_state_.whites_)),
      [this](int v) {
        tone_state_.whites_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Whites);
      },
      [this]() { CommitToneField(AdjustmentField::Whites); },
      [this]() {
        ResetToneFieldToDefault(AdjustmentField::Whites, [this](const ToneAdjustmentState& defaults,
                                                                const AdjustmentState&) {
          tone_state_.whites_ = defaults.whites_;
        });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  blacks_slider_ = add_slider(
      "Blacks", -100, 100, static_cast<int>(std::lround(tone_state_.blacks_)),
      [this](int v) {
        tone_state_.blacks_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Blacks);
      },
      [this]() { CommitToneField(AdjustmentField::Blacks); },
      [this]() {
        ResetToneFieldToDefault(AdjustmentField::Blacks, [this](const ToneAdjustmentState& defaults,
                                                                const AdjustmentState&) {
          tone_state_.blacks_ = defaults.blacks_;
        });
      },
      [](int v) { return QString::number(v, 'f', 2); });
}

void ToneControlPanelWidget::BuildToneCurveSection() {
  auto* parent_widget  = this;
  auto& layout         = *deps_.panel_layout;
  auto* section_layout = AddSection(parent_widget, layout, "Tone Curve",
                                    "Smooth tone curve mapped from input [0, 1] to output [0, 1].");

  auto* frame          = new QFrame(parent_widget);
  auto* v_layout       = new QVBoxLayout(frame);
  v_layout->setContentsMargins(0, 0, 0, 0);
  v_layout->setSpacing(4);

  curve_widget_ = new ToneCurveWidget(frame);
  curve_widget_->SetControlPoints(tone_state_.curve_points_);
  curve_widget_->SetCurveChangedCallback([this](const std::vector<QPointF>& points) {
    if (IsSyncing()) {
      return;
    }
    tone_state_.curve_points_ = curve::NormalizeCurveControlPoints(points);
    PreviewToneField(AdjustmentField::Curve);
  });
  curve_widget_->SetCurveReleasedCallback([this](const std::vector<QPointF>& points) {
    if (IsSyncing()) {
      return;
    }
    tone_state_.curve_points_ = curve::NormalizeCurveControlPoints(points);
    CommitToneField(AdjustmentField::Curve);
  });
  RegisterCurveReset(curve_widget_, [this]() { ResetCurveToDefaultLocal(); });

  auto* actions_row        = new QWidget(frame);
  auto* actions_row_layout = new QHBoxLayout(actions_row);
  actions_row_layout->setContentsMargins(0, 0, 0, 0);
  actions_row_layout->setSpacing(8);

  auto* curve_hint = NewLocalizedLabel(
      "Left click/drag to shape. Right click a point to remove. Double click to reset.",
      actions_row);
  curve_hint->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(curve_hint, AppTheme::FontRole::UiHint);
  curve_hint->setWordWrap(true);

  auto* reset_curve_btn = NewLocalizedButton("Reset Curve", actions_row);
  reset_curve_btn->setFixedHeight(28);
  reset_curve_btn->setStyleSheet(AppTheme::EditorPrimaryButtonStyle());
  QObject::connect(reset_curve_btn, &QPushButton::clicked, this,
                   [this]() { ResetCurveToDefaultLocal(); });

  actions_row_layout->addWidget(curve_hint, 1);
  actions_row_layout->addWidget(reset_curve_btn, 0);

  v_layout->addWidget(curve_widget_, 1);
  v_layout->addWidget(actions_row, 0);

  section_layout->addWidget(frame, 0);
}

void ToneControlPanelWidget::BuildColorSection() {
  auto* parent_widget = this;
  auto& layout        = *deps_.panel_layout;
  auto* section_layout =
      AddSection(parent_widget, layout, "Color", "Color balance and saturation.");

  const QString value_chip_style =
      QStringLiteral(
          "QLabel {"
          "  color: %1;"
          "  background: transparent;"
          "  border: none;"
          "  padding: 0;"
          "}")
          .arg(AppTheme::Instance().textMutedColor().name(QColor::HexRgb));

  auto add_combo_box = [&](const char* name_source, const QStringList& items, int initial_index,
                           std::function<void(int)> on_change) -> QComboBox* {
    auto* label = NewLocalizedLabel(name_source, parent_widget);
    label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(label, AppTheme::FontRole::UiBody);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* combo = new QComboBox(parent_widget);
    combo->addItems(items);
    combo->setCurrentIndex(initial_index);
    combo->setMinimumWidth(96);
    combo->setMaximumWidth(160);
    combo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    combo->setFixedHeight(26);
    combo->setStyleSheet(AppTheme::EditorComboBoxStyle());

    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), parent_widget,
                     [this, on_change](int idx) {
                       if (IsSyncing()) {
                         return;
                       }
                       on_change(idx);
                     });

    auto* row        = new QWidget(parent_widget);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(8);
    row_layout->addWidget(label, 1);
    row_layout->addWidget(combo, 0, Qt::AlignRight | Qt::AlignVCenter);

    section_layout->addWidget(row, 0);
    return combo;
  };

  auto add_slider = [&, value_chip_style](
                        const char* name_source, int min, int max, int value,
                        std::function<void(int)> on_change, std::function<void()> on_release,
                        std::function<void()> on_reset, std::function<QString(int)> formatter,
                        SliderVisualStyle visual_style = SliderVisualStyle::Accent) -> QSlider* {
    auto* name_label = NewLocalizedLabel(name_source, parent_widget);
    name_label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(name_label, AppTheme::FontRole::UiCaption);
    name_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* value_chip = new QLabel(formatter(value), parent_widget);
    value_chip->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_chip->setMinimumWidth(40);
    value_chip->setMaximumWidth(72);
    value_chip->setFixedHeight(16);
    value_chip->setStyleSheet(value_chip_style);
    AppTheme::MarkFontRole(value_chip, AppTheme::FontRole::DataCaption);

    QSlider* slider = nullptr;
    if (visual_style == SliderVisualStyle::Accent) {
      slider = new AccentBalanceSlider(kRegularSliderMetrics, parent_widget);
    } else {
      slider = new QSlider(Qt::Horizontal, parent_widget);
    }
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setSingleStep(1);
    slider->setPageStep(std::max(1, (max - min) / 20));
    slider->setMinimumWidth(0);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    slider->setFixedHeight(22);

    QObject::connect(slider, &QSlider::valueChanged, parent_widget,
                     [this, value_chip, formatter, on_change](int v) {
                       value_chip->setText(formatter(v));
                       if (IsSyncing()) {
                         return;
                       }
                       on_change(v);
                     });

    QObject::connect(slider, &QSlider::sliderReleased, parent_widget, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderSettled(slider, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderReset(slider, [this, on_reset]() {
      if (IsSyncing()) {
        return;
      }
      on_reset();
    });

    auto* head_row    = new QWidget(parent_widget);
    auto* head_layout = new QHBoxLayout(head_row);
    head_layout->setContentsMargins(0, 0, 0, 0);
    head_layout->setSpacing(8);
    head_layout->addWidget(name_label, 1);
    head_layout->addWidget(value_chip, 0, Qt::AlignRight | Qt::AlignVCenter);

    auto* row        = new QWidget(parent_widget);
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(2);
    row_layout->addWidget(head_row, 0);
    row_layout->addWidget(slider, 0);

    section_layout->addWidget(row, 0);
    return slider;
  };

  saturation_slider_ = add_slider(
      "Saturation", -100, 100, static_cast<int>(std::lround(tone_state_.saturation_)),
      [this](int v) {
        tone_state_.saturation_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Saturation);
      },
      [this]() { CommitToneField(AdjustmentField::Saturation); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Saturation,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.saturation_ = defaults.saturation_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  PullColorTempStateFromDialog();
  auto* dialog_state_ptr = deps_.dialog_state;
  color_temp_mode_combo_ = add_combo_box(
      "White Balance", {Tr("As Shot"), Tr("Custom")},
      ColorTempModeToComboIndex(dialog_state_ptr ? dialog_state_ptr->color_temp_mode_
                                                 : ColorTempMode::AS_SHOT),
      [this](int idx) {
        if (!deps_.dialog_state) {
          return;
        }
        auto&      s        = *deps_.dialog_state;
        const auto new_mode = ComboIndexToColorTempMode(idx);
        if (new_mode == s.color_temp_mode_) {
          return;
        }
        PullColorTempStateFromDialog();
        if (s.color_temp_mode_ == ColorTempMode::AS_SHOT && new_mode == ColorTempMode::CUSTOM) {
          color_temp_state_.custom_cct_  = DisplayedColorTempCct(s);
          color_temp_state_.custom_tint_ = DisplayedColorTempTint(s);
        }
        color_temp_state_.mode_ = new_mode;
        ProjectColorTempStateToDialog();
        if (new_mode == ColorTempMode::AS_SHOT) {
          PrimeColorTempDisplayForAsShot();
        }
        if (callbacks_.sync_controls_from_state) {
          callbacks_.sync_controls_from_state();
        }
        RequestPipelineRender();
        if (deps_.session) {
          deps_.session->Commit(AdjustmentField::ColorTemp);
        }
        PullCommittedColorTempStateFromDialog();
      });

  color_temp_cct_slider_ = add_slider(
      "Color Temp", color_temp::kSliderUiMin, color_temp::kSliderUiMax,
      ColorTempCctToSliderPos(dialog_state_ptr ? DisplayedColorTempCct(*dialog_state_ptr)
                                               : 6500.0f),
      [this](int v) {
        if (!deps_.dialog_state) {
          return;
        }
        PromoteColorTempToCustomForEditing();
        PullColorTempStateFromDialog();
        color_temp_state_.custom_cct_ = ColorTempSliderPosToCct(v);
        ProjectColorTempStateToDialog();
        RequestPipelineRender();
        if (deps_.session) {
          AdjustmentPreview preview{
              .field = AdjustmentField::ColorTemp,
              .params =
                  deps_.session->ParamsForField(AdjustmentField::ColorTemp, *deps_.dialog_state),
              .policy = PreviewPolicy::FastViewport,
          };
          deps_.session->Preview(preview);
        }
      },
      [this]() {
        if (deps_.session) {
          deps_.session->Commit(AdjustmentField::ColorTemp);
        }
        PullCommittedColorTempStateFromDialog();
      },
      [this]() { ResetColorTempToAsShot(); },
      [](int v) {
        return QString("%1 K").arg(static_cast<int>(std::lround(ColorTempSliderPosToCct(v))));
      },
      SliderVisualStyle::Native);
  color_temp_cct_slider_->setStyleSheet(
      "QSlider::groove:horizontal {"
      "  border: 1px solid #2A2A2A;"
      "  height: 8px;"
      "  border-radius: 4px;"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
      "stop:0 #9BD8FF, stop:0.5 #FFE8B0, stop:1 #FF8A3D);"
      "}"
      "QSlider::handle:horizontal {"
      "  background: #F2F2F2;"
      "  border: 1px solid #2A2A2A;"
      "  width: 14px;"
      "  margin: -4px 0;"
      "  border-radius: 7px;"
      "}");

  color_temp_tint_slider_ = add_slider(
      "Color Tint", color_temp::kTintMin, color_temp::kTintMax,
      static_cast<int>(
          std::lround(dialog_state_ptr ? DisplayedColorTempTint(*dialog_state_ptr) : 0.0f)),
      [this](int v) {
        if (!deps_.dialog_state) {
          return;
        }
        PromoteColorTempToCustomForEditing();
        PullColorTempStateFromDialog();
        color_temp_state_.custom_tint_ = static_cast<float>(v);
        ProjectColorTempStateToDialog();
        RequestPipelineRender();
        if (deps_.session) {
          AdjustmentPreview preview{
              .field = AdjustmentField::ColorTemp,
              .params =
                  deps_.session->ParamsForField(AdjustmentField::ColorTemp, *deps_.dialog_state),
              .policy = PreviewPolicy::FastViewport,
          };
          deps_.session->Preview(preview);
        }
      },
      [this]() {
        if (deps_.session) {
          deps_.session->Commit(AdjustmentField::ColorTemp);
        }
        PullCommittedColorTempStateFromDialog();
      },
      [this]() { ResetColorTempToAsShot(); }, [](int v) { return QString::number(v, 'f', 0); },
      SliderVisualStyle::Native);
  color_temp_tint_slider_->setStyleSheet(
      "QSlider::groove:horizontal {"
      "  border: 1px solid #2A2A2A;"
      "  height: 8px;"
      "  border-radius: 4px;"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
      "stop:0 #49C26D, stop:0.5 #E6E6E6, stop:1 #A85AE6);"
      "}"
      "QSlider::handle:horizontal {"
      "  background: #F2F2F2;"
      "  border: 1px solid #2A2A2A;"
      "  width: 14px;"
      "  margin: -4px 0;"
      "  border-radius: 7px;"
      "}");

  color_temp_unsupported_label_ =
      NewLocalizedLabel("Color temperature/tint is unavailable for this image.", parent_widget);
  color_temp_unsupported_label_->setWordWrap(true);
  AppTheme::MarkFontRole(color_temp_unsupported_label_, AppTheme::FontRole::UiHint);
  color_temp_unsupported_label_->setStyleSheet(
      "QLabel {"
      "  color: #FFB454;"
      "  background: rgba(255, 180, 84, 0.12);"
      "  border: 1px solid rgba(255, 180, 84, 0.35);"
      "  border-radius: 8px;"
      "  padding: 6px 8px;"
      "}");
  section_layout->addWidget(color_temp_unsupported_label_, 0);
  color_temp_unsupported_label_->setVisible(
      dialog_state_ptr ? !dialog_state_ptr->color_temp_supported_ : false);
}

void ToneControlPanelWidget::BuildDetailSection() {
  auto* parent_widget = this;
  auto& layout        = *deps_.panel_layout;
  auto* section_layout =
      AddSection(parent_widget, layout, "Detail", "Micro-contrast and sharpen controls.");

  const QString value_chip_style =
      QStringLiteral(
          "QLabel {"
          "  color: %1;"
          "  background: transparent;"
          "  border: none;"
          "  padding: 0;"
          "}")
          .arg(AppTheme::Instance().textMutedColor().name(QColor::HexRgb));

  auto add_slider = [&, value_chip_style](const char* name_source, int min, int max, int value,
                                          std::function<void(int)>    on_change,
                                          std::function<void()>       on_release,
                                          std::function<void()>       on_reset,
                                          std::function<QString(int)> formatter) -> QSlider* {
    auto* name_label = NewLocalizedLabel(name_source, parent_widget);
    name_label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(name_label, AppTheme::FontRole::UiCaption);
    name_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* value_chip = new QLabel(formatter(value), parent_widget);
    value_chip->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_chip->setMinimumWidth(40);
    value_chip->setMaximumWidth(72);
    value_chip->setFixedHeight(16);
    value_chip->setStyleSheet(value_chip_style);
    AppTheme::MarkFontRole(value_chip, AppTheme::FontRole::DataCaption);

    auto* slider = new AccentBalanceSlider(kRegularSliderMetrics, parent_widget);
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setSingleStep(1);
    slider->setPageStep(std::max(1, (max - min) / 20));
    slider->setMinimumWidth(0);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    slider->setFixedHeight(22);

    QObject::connect(slider, &QSlider::valueChanged, parent_widget,
                     [this, value_chip, formatter, on_change](int v) {
                       value_chip->setText(formatter(v));
                       if (IsSyncing()) {
                         return;
                       }
                       on_change(v);
                     });

    QObject::connect(slider, &QSlider::sliderReleased, parent_widget, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderSettled(slider, [this, on_release]() {
      if (IsSyncing()) {
        return;
      }
      on_release();
    });

    RegisterSliderReset(slider, [this, on_reset]() {
      if (IsSyncing()) {
        return;
      }
      on_reset();
    });

    auto* head_row    = new QWidget(parent_widget);
    auto* head_layout = new QHBoxLayout(head_row);
    head_layout->setContentsMargins(0, 0, 0, 0);
    head_layout->setSpacing(8);
    head_layout->addWidget(name_label, 1);
    head_layout->addWidget(value_chip, 0, Qt::AlignRight | Qt::AlignVCenter);

    auto* row        = new QWidget(parent_widget);
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(2);
    row_layout->addWidget(head_row, 0);
    row_layout->addWidget(slider, 0);

    section_layout->addWidget(row, 0);
    return slider;
  };

  sharpen_slider_ = add_slider(
      "Sharpen", -100, 100, static_cast<int>(std::lround(tone_state_.sharpen_)),
      [this](int v) {
        tone_state_.sharpen_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Sharpen);
      },
      [this]() { CommitToneField(AdjustmentField::Sharpen); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Sharpen,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.sharpen_ = defaults.sharpen_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  clarity_slider_ = add_slider(
      "Clarity", -100, 100, static_cast<int>(std::lround(tone_state_.clarity_)),
      [this](int v) {
        tone_state_.clarity_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Clarity);
      },
      [this]() { CommitToneField(AdjustmentField::Clarity); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Clarity,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.clarity_ = defaults.clarity_;
            });
      },
      [](int v) { return QString::number(v, 'f', 2); });

  film_grain_slider_ = add_slider(
      "Film Grain", 0, 100, static_cast<int>(std::lround(tone_state_.film_grain_)),
      [this](int v) {
        tone_state_.film_grain_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::FilmGrain);
      },
      [this]() { CommitToneField(AdjustmentField::FilmGrain); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::FilmGrain,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.film_grain_ = defaults.film_grain_;
            });
      },
      [](int v) { return QString::number(v, 'f', 0); });

  halation_slider_ = add_slider(
      "Halation", 0, 100, static_cast<int>(std::lround(tone_state_.halation_)),
      [this](int v) {
        tone_state_.halation_ = static_cast<float>(v);
        PreviewToneField(AdjustmentField::Halation);
      },
      [this]() { CommitToneField(AdjustmentField::Halation); },
      [this]() {
        ResetToneFieldToDefault(
            AdjustmentField::Halation,
            [this](const ToneAdjustmentState& defaults, const AdjustmentState&) {
              tone_state_.halation_ = defaults.halation_;
            });
      },
      [](int v) { return QString::number(v, 'f', 0); });
}

void ToneControlPanelWidget::SyncControlsFromDialogState() {
  if (!deps_.dialog_state) {
    return;
  }
  PullToneStateFromDialog();
  PullCommittedToneStateFromDialog();
  PullColorTempStateFromDialog();
  PullCommittedColorTempStateFromDialog();

  const bool prev = local_syncing_;
  local_syncing_  = true;
  if (exposure_slider_) {
    exposure_slider_->setValue(static_cast<int>(std::lround(tone_state_.exposure_ * 100.0f)));
  }
  if (contrast_slider_) {
    contrast_slider_->setValue(static_cast<int>(std::lround(tone_state_.contrast_)));
  }
  if (saturation_slider_) {
    saturation_slider_->setValue(static_cast<int>(std::lround(tone_state_.saturation_)));
  }
  if (blacks_slider_) {
    blacks_slider_->setValue(static_cast<int>(std::lround(tone_state_.blacks_)));
  }
  if (whites_slider_) {
    whites_slider_->setValue(static_cast<int>(std::lround(tone_state_.whites_)));
  }
  if (shadows_slider_) {
    shadows_slider_->setValue(static_cast<int>(std::lround(tone_state_.shadows_)));
  }
  if (highlights_slider_) {
    highlights_slider_->setValue(static_cast<int>(std::lround(tone_state_.highlights_)));
  }
  if (sharpen_slider_) {
    sharpen_slider_->setValue(static_cast<int>(std::lround(tone_state_.sharpen_)));
  }
  if (clarity_slider_) {
    clarity_slider_->setValue(static_cast<int>(std::lround(tone_state_.clarity_)));
  }
  if (film_grain_slider_) {
    film_grain_slider_->setValue(static_cast<int>(std::lround(tone_state_.film_grain_)));
  }
  if (halation_slider_) {
    halation_slider_->setValue(static_cast<int>(std::lround(tone_state_.halation_)));
  }
  if (curve_widget_) {
    curve_widget_->SetControlPoints(tone_state_.curve_points_);
  }
  SyncColorTempControlsFromDialogState();
  local_syncing_ = prev;
}

void ToneControlPanelWidget::SyncColorTempControlsFromDialogState() {
  if (!deps_.dialog_state) {
    return;
  }
  PullColorTempStateFromDialog();
  const auto& s    = *deps_.dialog_state;
  const bool  prev = local_syncing_;
  local_syncing_   = true;
  if (color_temp_mode_combo_) {
    color_temp_mode_combo_->setCurrentIndex(ColorTempModeToComboIndex(s.color_temp_mode_));
  }
  if (color_temp_cct_slider_) {
    color_temp_cct_slider_->setValue(ColorTempCctToSliderPos(DisplayedColorTempCct(s)));
    color_temp_cct_slider_->setEnabled(s.color_temp_supported_);
  }
  if (color_temp_tint_slider_) {
    color_temp_tint_slider_->setValue(static_cast<int>(std::lround(DisplayedColorTempTint(s))));
    color_temp_tint_slider_->setEnabled(s.color_temp_supported_);
  }
  if (color_temp_unsupported_label_) {
    color_temp_unsupported_label_->setVisible(!s.color_temp_supported_);
  }
  local_syncing_ = prev;
}

void ToneControlPanelWidget::RetranslateColorTempModeCombo() {
  if (!color_temp_mode_combo_) {
    return;
  }
  const int  current_value = color_temp_mode_combo_->currentData().toInt();
  const bool prev          = local_syncing_;
  local_syncing_           = true;
  color_temp_mode_combo_->clear();
  color_temp_mode_combo_->addItem(Tr("As Shot"), static_cast<int>(ColorTempMode::AS_SHOT));
  color_temp_mode_combo_->addItem(Tr("Custom"), static_cast<int>(ColorTempMode::CUSTOM));
  const int index = color_temp_mode_combo_->findData(current_value);
  color_temp_mode_combo_->setCurrentIndex(std::max(0, index));
  local_syncing_ = prev;
}

void ToneControlPanelWidget::LoadFromPipeline() {
  if (!deps_.session) {
    return;
  }
  if (deps_.session->LoadFromPipeline()) {
    PullToneStateFromDialog();
    PullCommittedToneStateFromDialog();
  }
}

void ToneControlPanelWidget::ReloadFromCommittedState() { SyncControlsFromDialogState(); }

}  // namespace alcedo::ui
