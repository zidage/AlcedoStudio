//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QWidget>
#include <memory>

#include "app/history_mgmt_service.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"

namespace alcedo::ui {

// Opens the editor dialog and runs it modally through the application-facing API.
auto OpenEditorDialog(std::shared_ptr<ImagePoolService>       image_pool,
                      std::shared_ptr<PipelineGuard>          pipeline_guard,
                      std::shared_ptr<EditHistoryMgmtService> history_service,
                      std::shared_ptr<EditHistoryGuard> history_guard, sl_element_id_t element_id,
                      image_id_t image_id, QWidget* parent = nullptr) -> bool;

// Test-only call counters. Production builds leave these as no-ops via weak
// defaults; the test stub implements them so workspace tests can assert the
// legacy modal path was never entered.
void ResetOpenEditorDialogCallCount();
auto OpenEditorDialogCallCount() -> int;

}  // namespace alcedo::ui

#ifdef ALCEDO_EDITOR_DIALOG_INTERNAL
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEasingCurve>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QSurfaceFormat>
#include <QSvgRenderer>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <json.hpp>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/render_service.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/history/version.hpp"
#include "edit/operators/basic/color_temp_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/history_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/render_controller.hpp"
#include "ui/alcedo_main/editor_dialog/dialog.hpp"
#include "ui/alcedo_main/editor_dialog/frame/editor_frame_manager.hpp"
#include "ui/alcedo_main/editor_dialog/history/editor_history_coordinator.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"
#include "ui/alcedo_main/editor_dialog/modules/curve.hpp"
#include "ui/alcedo_main/editor_dialog/modules/geometry.hpp"
#include "ui/alcedo_main/editor_dialog/modules/histogram.hpp"
#include "ui/alcedo_main/editor_dialog/modules/hls.hpp"
#include "ui/alcedo_main/editor_dialog/modules/pipeline_io.hpp"
#include "ui/alcedo_main/editor_dialog/modules/versioning.hpp"
#include "ui/alcedo_main/editor_dialog/render/editor_render_coordinator.hpp"
#include "ui/alcedo_main/editor_dialog/scope/scope_panel.hpp"
#include "ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"
#include "ui/alcedo_main/editor_dialog/state.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/display_transform_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/editor_control_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/editor_viewer_pane.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/history_cards.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/look_control_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/spinner.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/tone_curve_widget.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/versioning_panel_widget.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"
#include "ui/edit_viewer/edit_viewer.hpp"

namespace alcedo::ui {

namespace {

const auto kShortcutUndoHistoryId    = QStringLiteral("editor_dialog.undo_history_transaction");
const auto kShortcutResetGeometryId  = QStringLiteral("editor_dialog.reset_geometry");
const auto kShortcutCommitGeometryId = QStringLiteral("editor_dialog.commit_geometry");
const auto kShortcutCommitGeometryNumpadId = QStringLiteral("editor_dialog.commit_geometry_numpad");
const auto kShortcutSelectPrevLutId        = QStringLiteral("editor_dialog.select_prev_lut");
const auto kShortcutSelectNextLutId        = QStringLiteral("editor_dialog.select_next_lut");
constexpr char kPanelIconPathProperty[]    = "puerhlabPanelIconPath";
constexpr char kLocalizedTextProperty[]    = "puerhlabI18nText";
constexpr char kLocalizedTextUpperProperty[] = "puerhlabI18nTextUpper";
constexpr char kLocalizedToolTipProperty[]   = "puerhlabI18nToolTip";
const QSize    kPanelToggleIconSize(18, 18);
constexpr int  kPanelToggleButtonHeight = 44;
constexpr int  kEditorOuterMargin       = 14;
constexpr int  kControlsPanelMinWidth   = 260;

using namespace std::chrono_literals;

using curve::BuildCurveHermiteCache;
using curve::Clamp01;
using curve::CurveControlPointsEqual;
using curve::CurveControlPointsToParams;
using curve::CurveHermiteCache;
using curve::DefaultCurveControlPoints;
using curve::EvaluateCurveHermite;
using curve::NormalizeCurveControlPoints;
using curve::ParseCurveControlPointsFromParams;
using geometry::ClampCropRect;
using geometry::CropAspectPreset;
void SetLocalizedText(QObject* object, const char* source, bool uppercase = false) {
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
  } else if (auto* checkbox = qobject_cast<QCheckBox*>(object)) {
    checkbox->setText(text);
  }
}

void SetLocalizedToolTip(QWidget* widget, const char* source) {
  if (!widget || source == nullptr) {
    return;
  }
  widget->setProperty(kLocalizedToolTipProperty, source);
  widget->setToolTip(Tr(source));
  widget->setAccessibleName(Tr(source));
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

auto NewLocalizedCheckBox(const char* source, QWidget* parent) -> QCheckBox* {
  auto* checkbox = new QCheckBox(parent);
  SetLocalizedText(checkbox, source);
  return checkbox;
}

void RetranslateMarkedObjects(QObject* root) {
  if (!root) {
    return;
  }

  const QVariant text_source = root->property(kLocalizedTextProperty);
  if (text_source.isValid()) {
    const bool       uppercase = root->property(kLocalizedTextUpperProperty).toBool();
    const QByteArray source    = text_source.toByteArray();
    SetLocalizedText(root, source.constData(), uppercase);
  }

  if (auto* widget = qobject_cast<QWidget*>(root)) {
    const QVariant tooltip_source = widget->property(kLocalizedToolTipProperty);
    if (tooltip_source.isValid()) {
      const QByteArray source = tooltip_source.toByteArray();
      SetLocalizedToolTip(widget, source.constData());
    }
  }

  for (QObject* child : root->children()) {
    RetranslateMarkedObjects(child);
  }
}

[[maybe_unused]] constexpr auto kEditorDialogTranslationSources = std::to_array<const char*>({
    QT_TRANSLATE_NOOP("Alcedo", "Adjustments"),
    QT_TRANSLATE_NOOP("Alcedo", "Tone"),
    QT_TRANSLATE_NOOP("Alcedo", "Primary tonal shaping controls."),
    QT_TRANSLATE_NOOP("Alcedo", "Exposure"),
    QT_TRANSLATE_NOOP("Alcedo", "Contrast"),
    QT_TRANSLATE_NOOP("Alcedo", "Highlights"),
    QT_TRANSLATE_NOOP("Alcedo", "Shadows"),
    QT_TRANSLATE_NOOP("Alcedo", "Whites"),
    QT_TRANSLATE_NOOP("Alcedo", "Blacks"),
    QT_TRANSLATE_NOOP("Alcedo", "Tone Curve"),
    QT_TRANSLATE_NOOP("Alcedo", "Smooth tone curve mapped from input [0, 1] to output [0, 1]."),
    QT_TRANSLATE_NOOP(
        "Alcedo",
        "Left click/drag to shape. Right click a point to remove. Double click to reset."),
    QT_TRANSLATE_NOOP("Alcedo", "Reset Curve"),
    QT_TRANSLATE_NOOP("Alcedo", "Color"),
    QT_TRANSLATE_NOOP("Alcedo", "Color balance and saturation."),
    QT_TRANSLATE_NOOP("Alcedo", "Saturation"),
    QT_TRANSLATE_NOOP("Alcedo", "White Balance"),
    QT_TRANSLATE_NOOP("Alcedo", "As Shot"),
    QT_TRANSLATE_NOOP("Alcedo", "Custom"),
    QT_TRANSLATE_NOOP("Alcedo", "Color Temp"),
    QT_TRANSLATE_NOOP("Alcedo", "Color Tint"),
    QT_TRANSLATE_NOOP("Alcedo", "Color temperature/tint is unavailable for this image."),
    QT_TRANSLATE_NOOP("Alcedo", "Detail"),
    QT_TRANSLATE_NOOP("Alcedo", "Micro-contrast and sharpen controls."),
    QT_TRANSLATE_NOOP("Alcedo", "Sharpen"),
    QT_TRANSLATE_NOOP("Alcedo", "Clarity"),
    QT_TRANSLATE_NOOP("Alcedo", "LUT"),
    QT_TRANSLATE_NOOP("Alcedo", "Browse and apply look-up tables."),
    QT_TRANSLATE_NOOP("Alcedo", "HSL / Color"),
    QT_TRANSLATE_NOOP("Alcedo", "Per-hue lightness and chroma adjustments."),
    QT_TRANSLATE_NOOP("Alcedo", "Hue Shift"),
    QT_TRANSLATE_NOOP("Alcedo", "Lightness"),
    QT_TRANSLATE_NOOP("Alcedo", "Chroma"),
    QT_TRANSLATE_NOOP("Alcedo", "Hue Smoothness"),
    QT_TRANSLATE_NOOP("Alcedo", "Color Wheels"),
    QT_TRANSLATE_NOOP("Alcedo", "CDL: Lift / Gamma / Gain with master offset."),
    QT_TRANSLATE_NOOP("Alcedo", "Gamma"),
    QT_TRANSLATE_NOOP("Alcedo", "Lift"),
    QT_TRANSLATE_NOOP("Alcedo", "Gain"),
    QT_TRANSLATE_NOOP("Alcedo", "Display Rendering Transform"),
    QT_TRANSLATE_NOOP("Alcedo", "Display RT"),
    QT_TRANSLATE_NOOP("Alcedo", "Encoding Space"),
    QT_TRANSLATE_NOOP("Alcedo", "Encoding EOTF"),
    QT_TRANSLATE_NOOP("Alcedo", "Peak Luminance"),
    QT_TRANSLATE_NOOP("Alcedo", "Rendering Method"),
    QT_TRANSLATE_NOOP("Alcedo",
                      "Choose the transform family. Shared encoding settings stay above; "
                      "method-specific settings stay preserved per method."),
    QT_TRANSLATE_NOOP("Alcedo", "ACES 2.0"),
    QT_TRANSLATE_NOOP("Alcedo", "OpenDRT"),
    QT_TRANSLATE_NOOP("Alcedo", "Limiting Space"),
    QT_TRANSLATE_NOOP("Alcedo", "Look Preset"),
    QT_TRANSLATE_NOOP("Alcedo", "Tonescale Preset"),
    QT_TRANSLATE_NOOP("Alcedo", "Creative White"),
    QT_TRANSLATE_NOOP("Alcedo", "Geometry"),
    QT_TRANSLATE_NOOP("Alcedo", "Crop & Aspect Ratio"),
    QT_TRANSLATE_NOOP("Alcedo", "Aspect"),
    QT_TRANSLATE_NOOP("Alcedo", "Crop Rotation"),
    QT_TRANSLATE_NOOP("Alcedo", "Angle"),
    QT_TRANSLATE_NOOP("Alcedo", "Crop Offset"),
    QT_TRANSLATE_NOOP("Alcedo", "X"),
    QT_TRANSLATE_NOOP("Alcedo", "Y"),
    QT_TRANSLATE_NOOP("Alcedo", "Width"),
    QT_TRANSLATE_NOOP("Alcedo", "Height"),
    QT_TRANSLATE_NOOP("Alcedo", "Apply Crop"),
    QT_TRANSLATE_NOOP("Alcedo", "Reset"),
    QT_TRANSLATE_NOOP("Alcedo",
                      "Pixels update on Apply. Double click any slider or the viewer to reset. "
                      "Ctrl+R resets all geometry."),
    QT_TRANSLATE_NOOP("Alcedo", "RAW Decode"),
    QT_TRANSLATE_NOOP(
        "Alcedo",
        "Configure RAW decode options. These settings are shared with thumbnail rendering."),
    QT_TRANSLATE_NOOP("Alcedo", "Enable Highlight Reconstruction"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens Calibration"),
    QT_TRANSLATE_NOOP(
        "Alcedo", "Enable correction and optionally override lens metadata with catalog entries."),
    QT_TRANSLATE_NOOP("Alcedo", "Enable Lens Calibration"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens Brand"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens Model"),
    QT_TRANSLATE_NOOP("Alcedo", "Edit History"),
    QT_TRANSLATE_NOOP("Alcedo", "Position"),
    QT_TRANSLATE_NOOP("Alcedo", "Undo Last"),
    QT_TRANSLATE_NOOP("Alcedo", "Undo last transaction"),
    QT_TRANSLATE_NOOP("Alcedo", "Current"),
    QT_TRANSLATE_NOOP("Alcedo", "Returned to step %1 of %2"),
    QT_TRANSLATE_NOOP("Alcedo", "No edits yet"),
    QT_TRANSLATE_NOOP("Alcedo", "Versions"),
    QT_TRANSLATE_NOOP("Alcedo", "New Version"),
    QT_TRANSLATE_NOOP("Alcedo", "Modified %1"),
    QT_TRANSLATE_NOOP("Alcedo", "CURRENT"),
    QT_TRANSLATE_NOOP("Alcedo", "Rename version"),
    QT_TRANSLATE_NOOP("Alcedo", "Version name"),
    QT_TRANSLATE_NOOP("Alcedo", "Untitled"),
    QT_TRANSLATE_NOOP("Alcedo", "No versions"),
    QT_TRANSLATE_NOOP("Alcedo", "Rec.709"),
    QT_TRANSLATE_NOOP("Alcedo", "P3-D65"),
    QT_TRANSLATE_NOOP("Alcedo", "P3-D60"),
    QT_TRANSLATE_NOOP("Alcedo", "P3-DCI"),
    QT_TRANSLATE_NOOP("Alcedo", "XYZ"),
    QT_TRANSLATE_NOOP("Alcedo", "Rec.2020"),
    QT_TRANSLATE_NOOP("Alcedo", "ProPhoto RGB"),
    QT_TRANSLATE_NOOP("Alcedo", "Adobe RGB"),
    QT_TRANSLATE_NOOP("Alcedo", "Standard"),
    QT_TRANSLATE_NOOP("Alcedo", "Arriba"),
    QT_TRANSLATE_NOOP("Alcedo", "Sylvan"),
    QT_TRANSLATE_NOOP("Alcedo", "Colorful"),
    QT_TRANSLATE_NOOP("Alcedo", "Aery"),
    QT_TRANSLATE_NOOP("Alcedo", "Dystopic"),
    QT_TRANSLATE_NOOP("Alcedo", "Umbra"),
    QT_TRANSLATE_NOOP("Alcedo", "Use Look Preset"),
    QT_TRANSLATE_NOOP("Alcedo", "Low Contrast"),
    QT_TRANSLATE_NOOP("Alcedo", "Medium Contrast"),
    QT_TRANSLATE_NOOP("Alcedo", "High Contrast"),
    QT_TRANSLATE_NOOP("Alcedo", "Arriba Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Sylvan Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Colorful Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Aery Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Dystopic Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Umbra Tonescale"),
    QT_TRANSLATE_NOOP("Alcedo", "Marvelous Tonscape"),
    QT_TRANSLATE_NOOP("Alcedo", "Dagrinchi Tonegroan"),
    QT_TRANSLATE_NOOP("Alcedo", "D93"),
});

// Tonal-slider scale aliases (defined in pipeline_io).
constexpr float  kBlackSliderFromGlobalScale      = pipeline_io::kBlackSliderFromGlobalScale;
constexpr float  kWhiteSliderFromGlobalScale      = pipeline_io::kWhiteSliderFromGlobalScale;
constexpr float  kShadowsSliderFromGlobalScale    = pipeline_io::kShadowsSliderFromGlobalScale;
constexpr float  kHighlightsSliderFromGlobalScale = pipeline_io::kHighlightsSliderFromGlobalScale;

// Aliases for module constants (preserve local names used throughout EditorDialog).
constexpr int    kColorTempCctMin                 = color_temp::kCctMin;
constexpr int    kColorTempCctMax                 = color_temp::kCctMax;
constexpr int    kColorTempTintMin                = color_temp::kTintMin;
constexpr int    kColorTempTintMax                = color_temp::kTintMax;
constexpr int    kColorTempSliderUiMin            = color_temp::kSliderUiMin;
constexpr int    kColorTempSliderUiMax            = color_temp::kSliderUiMax;
constexpr float  kRotationSliderScale             = geometry::kRotationSliderScale;
constexpr float  kCropRectSliderScale             = geometry::kCropRectSliderScale;
constexpr double kCropAspectSpinMin               = 0.01;
constexpr double kCropAspectSpinMax               = 100.0;
auto RenderPanelToggleIcon(const QString& resource_path, const QColor& color, const QSize& size,
                           qreal device_pixel_ratio) -> QIcon {
  QFile svg_file(resource_path);
  if (!svg_file.open(QIODevice::ReadOnly)) {
    return {};
  }

  QByteArray svg_data = svg_file.readAll();
  svg_data.replace("currentColor", color.name(QColor::HexRgb).toUtf8());

  QSvgRenderer renderer(svg_data);
  if (!renderer.isValid()) {
    return {};
  }

  const qreal scale =
      std::max<qreal>(2.0, std::ceil(std::max<qreal>(1.0, device_pixel_ratio) * 2.0));
  const QSize physical_size(std::max(1, qRound(size.width() * scale)),
                            std::max(1, qRound(size.height() * scale)));
  QPixmap     pixmap(physical_size);
  pixmap.fill(Qt::transparent);
  pixmap.setDevicePixelRatio(scale);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  renderer.render(&painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)));
  return QIcon(pixmap);
}

auto RenderDockRailIcon(const QString& resource_path, const QColor& icon_color,
                        const QColor& chevron_color, const QSize& size, qreal device_pixel_ratio,
                        qreal chevron_angle_degrees) -> QIcon {
  QFile svg_file(resource_path);
  if (!svg_file.open(QIODevice::ReadOnly)) {
    return {};
  }

  QByteArray svg_data = svg_file.readAll();
  svg_data.replace("currentColor", icon_color.name(QColor::HexRgb).toUtf8());

  QSvgRenderer renderer(svg_data);
  if (!renderer.isValid()) {
    return {};
  }

  const qreal scale = std::max<qreal>(1.0, device_pixel_ratio);
  const QSize physical_size(std::max(1, qRound(size.width() * scale)),
                            std::max(1, qRound(size.height() * scale)));

  QPixmap     pixmap(physical_size);
  pixmap.fill(Qt::transparent);
  pixmap.setDevicePixelRatio(scale);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const qreal  glyph_span = std::max(4.0, std::min<qreal>(size.height() - 2.0, size.width() - 9.0));
  const QRectF glyph_rect(1.0, (size.height() - glyph_span) * 0.5, glyph_span, glyph_span);
  renderer.render(&painter, glyph_rect);

  const qreal cx = size.width() - 3.8;
  const qreal cy = size.height() * 0.5;

  painter.save();
  painter.translate(cx, cy);
  painter.rotate(chevron_angle_degrees);
  painter.translate(-cx, -cy);
  QPen chevron_pen(chevron_color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(chevron_pen);
  painter.setBrush(Qt::NoBrush);
  QPainterPath chevron_path;
  chevron_path.moveTo(cx - 3.2, cy - 2.6);
  chevron_path.lineTo(cx - 0.3, cy);
  chevron_path.lineTo(cx - 3.2, cy + 2.6);
  painter.drawPath(chevron_path);
  painter.restore();

  return QIcon(pixmap);
}

void ConfigurePanelToggleButton(QPushButton* button, const char* tooltip_source,
                                const QString& icon_resource_path) {
  if (!button) {
    return;
  }

  button->setText(QString());
  button->setCheckable(true);
  button->setAutoDefault(false);
  button->setDefault(false);
  button->setCursor(Qt::PointingHandCursor);
  button->setFixedHeight(kPanelToggleButtonHeight);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  SetLocalizedToolTip(button, tooltip_source);
  button->setIconSize(kPanelToggleIconSize);
  button->setProperty(kPanelIconPathProperty, icon_resource_path);
}

template <typename T>
struct EnumOption {
  T           value_;
  const char* label_;
};

constexpr std::array<EnumOption<ColorUtils::ColorSpace>, 6>     kDisplayEncodingSpaceOptions   = {{
    {ColorUtils::ColorSpace::REC709, "Rec.709"},
    {ColorUtils::ColorSpace::P3_D65, "P3-D65"},
    {ColorUtils::ColorSpace::P3_D60, "P3-D60"},
    {ColorUtils::ColorSpace::P3_DCI, "P3-DCI"},
    {ColorUtils::ColorSpace::XYZ, "XYZ"},
    {ColorUtils::ColorSpace::REC2020, "Rec.2020"},
}};

constexpr std::array<EnumOption<ColorUtils::ColorSpace>, 7>     kAcesLimitingSpaceOptions      = {{
    {ColorUtils::ColorSpace::REC709, "Rec.709"},
    {ColorUtils::ColorSpace::REC2020, "Rec.2020"},
    {ColorUtils::ColorSpace::P3_D65, "P3-D65"},
    {ColorUtils::ColorSpace::P3_D60, "P3-D60"},
    {ColorUtils::ColorSpace::P3_DCI, "P3-DCI"},
    {ColorUtils::ColorSpace::PROPHOTO, "ProPhoto RGB"},
    {ColorUtils::ColorSpace::ADOBE_RGB, "Adobe RGB"},
}};

constexpr std::array<EnumOption<odt_cpu::OpenDRTLookPreset>, 8> kOpenDrtLookPresetOptions      = {{
    {odt_cpu::OpenDRTLookPreset::STANDARD, "Standard"},
    {odt_cpu::OpenDRTLookPreset::ARRIBA, "Arriba"},
    {odt_cpu::OpenDRTLookPreset::SYLVAN, "Sylvan"},
    {odt_cpu::OpenDRTLookPreset::COLORFUL, "Colorful"},
    {odt_cpu::OpenDRTLookPreset::AERY, "Aery"},
    {odt_cpu::OpenDRTLookPreset::DYSTOPIC, "Dystopic"},
    {odt_cpu::OpenDRTLookPreset::UMBRA, "Umbra"},
    {odt_cpu::OpenDRTLookPreset::CUSTOM, "Custom"},
}};

constexpr std::array<EnumOption<odt_cpu::OpenDRTTonescalePreset>, 15> kOpenDrtTonescaleOptions = {{
    {odt_cpu::OpenDRTTonescalePreset::USE_LOOK_PRESET, "Use Look Preset"},
    {odt_cpu::OpenDRTTonescalePreset::LOW_CONTRAST, "Low Contrast"},
    {odt_cpu::OpenDRTTonescalePreset::MEDIUM_CONTRAST, "Medium Contrast"},
    {odt_cpu::OpenDRTTonescalePreset::HIGH_CONTRAST, "High Contrast"},
    {odt_cpu::OpenDRTTonescalePreset::ARRIBA_TONESCALE, "Arriba Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::SYLVAN_TONESCALE, "Sylvan Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::COLORFUL_TONESCALE, "Colorful Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::AERY_TONESCALE, "Aery Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::DYSTOPIC_TONESCALE, "Dystopic Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::UMBRA_TONESCALE, "Umbra Tonescale"},
    {odt_cpu::OpenDRTTonescalePreset::ACES_1_X, "ACES 1.x"},
    {odt_cpu::OpenDRTTonescalePreset::ACES_2_0, "ACES 2.0"},
    {odt_cpu::OpenDRTTonescalePreset::MARVELOUS_TONESCAPE, "Marvelous Tonscape"},
    {odt_cpu::OpenDRTTonescalePreset::DAGRINCHI_TONEGROAN, "Dagrinchi Tonegroan"},
    {odt_cpu::OpenDRTTonescalePreset::CUSTOM, "Custom"},
}};

constexpr std::array<EnumOption<odt_cpu::OpenDRTCreativeWhitePreset>, 7>
     kOpenDrtCreativeWhiteOptions = {{
        {odt_cpu::OpenDRTCreativeWhitePreset::USE_LOOK_PRESET, "Use Look Preset"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D93, "D93"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D75, "D75"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D65, "D65"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D60, "D60"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D55, "D55"},
        {odt_cpu::OpenDRTCreativeWhitePreset::D50, "D50"},
    }};

auto ColorTempSliderPosToCct(int pos) -> float { return color_temp::SliderPosToCct(pos); }
auto ColorTempCctToSliderPos(float cct) -> int { return color_temp::CctToSliderPos(cct); }

auto SupportedDisplayEotfOptions(ColorUtils::ColorSpace encoding_space)
    -> std::vector<EnumOption<ColorUtils::EOTF>> {
  switch (encoding_space) {
    case ColorUtils::ColorSpace::REC709:
      return {{ColorUtils::EOTF::BT1886, "BT.1886"}, {ColorUtils::EOTF::GAMMA_2_2, "Gamma 2.2"}};
    case ColorUtils::ColorSpace::P3_D65:
      return {{ColorUtils::EOTF::GAMMA_2_2, "Gamma 2.2"},
              {ColorUtils::EOTF::ST2084, "ST 2084 (PQ)"}};
    case ColorUtils::ColorSpace::P3_D60:
    case ColorUtils::ColorSpace::P3_DCI:
    case ColorUtils::ColorSpace::XYZ:
      return {{ColorUtils::EOTF::GAMMA_2_6, "Gamma 2.6"}};
    case ColorUtils::ColorSpace::REC2020:
      return {{ColorUtils::EOTF::ST2084, "ST 2084 (PQ)"}, {ColorUtils::EOTF::HLG, "HLG"}};
    default:
      return {{ColorUtils::EOTF::GAMMA_2_2, "Gamma 2.2"}};
  }
}

auto IsSupportedDisplayEncoding(ColorUtils::ColorSpace encoding_space,
                                ColorUtils::EOTF       encoding_eotf) -> bool {
  const auto options = SupportedDisplayEotfOptions(encoding_space);
  return std::any_of(options.begin(), options.end(), [encoding_eotf](const auto& option) {
    return option.value_ == encoding_eotf;
  });
}

auto DefaultDisplayEotfForSpace(ColorUtils::ColorSpace encoding_space) -> ColorUtils::EOTF {
  const auto options = SupportedDisplayEotfOptions(encoding_space);
  return options.empty() ? ColorUtils::EOTF::GAMMA_2_2 : options.front().value_;
}

void SanitizeOdtStateForUi(OdtState& odt_state) {
  const bool encoding_space_supported = std::any_of(
      kDisplayEncodingSpaceOptions.begin(), kDisplayEncodingSpaceOptions.end(),
      [&odt_state](const auto& option) { return option.value_ == odt_state.encoding_space_; });
  if (!encoding_space_supported) {
    odt_state.encoding_space_ = ColorUtils::ColorSpace::REC709;
  }
  if (!IsSupportedDisplayEncoding(odt_state.encoding_space_, odt_state.encoding_eotf_)) {
    odt_state.encoding_eotf_ = DefaultDisplayEotfForSpace(odt_state.encoding_space_);
  }
  const bool limiting_space_supported =
      std::any_of(kAcesLimitingSpaceOptions.begin(), kAcesLimitingSpaceOptions.end(),
                  [&odt_state](const auto& option) {
                    return option.value_ == odt_state.aces_.limiting_space_;
                  });
  if (!limiting_space_supported) {
    odt_state.aces_.limiting_space_ = ColorUtils::ColorSpace::REC709;
  }
  odt_state.peak_luminance_ = std::clamp(odt_state.peak_luminance_, 100.0f, 1000.0f);
}

auto ResolveEditorWindowTitle(const std::shared_ptr<ImagePoolService>& image_pool,
                              image_id_t image_id, sl_element_id_t element_id) -> QString {
  if (image_pool && image_id != 0) {
    try {
      const std::wstring image_name = image_pool->Read<std::wstring>(
          image_id, [](const std::shared_ptr<Image>& image) -> std::wstring {
            if (!image) {
              return {};
            }
            if (!image->image_name_.empty()) {
              return image->image_name_;
            }
            if (!image->image_path_.empty()) {
              return image->image_path_.filename().wstring();
            }
            return {};
          });
      if (!image_name.empty()) {
        return Tr("Editor - %1").arg(QString::fromStdWString(image_name));
      }
    } catch (...) {
    }
  }

  return Tr("Editor - image #%1").arg(static_cast<qulonglong>(element_id));
}

}  // namespace

class EditorDialog final : public QDialog {
 public:
  EditorDialog(std::shared_ptr<ImagePoolService>       image_pool,
               std::shared_ptr<PipelineGuard>          pipeline_guard,
               std::shared_ptr<EditHistoryMgmtService> history_service,
               std::shared_ptr<EditHistoryGuard> history_guard, sl_element_id_t element_id,
               image_id_t image_id, QWidget* parent = nullptr);

 private:
  void BuildViewerAndPanelShell();
  auto BuildControlPanelShell(const QString& panel_style) -> EditorControlPanelWidget*;
  void BuildLookControlPanel(EditorControlPanelWidget* controls_panel, const QString& scroll_style);
  void BuildLookPanel();
  void BuildToneControlPanel();
  void BuildDisplayTransformPanel();
  void BuildGeometryPanel();
  void BuildRawDecodePanel();
  // Types, enums, and state helpers are defined in state.hpp / state.cpp.

  void RegisterShortcuts();

  auto ShouldConsumeUndoShortcutLocally() const -> bool;

  auto ShouldConsumeLutNavigationShortcut() const -> bool;

  static auto DefaultAdjustmentState() -> const AdjustmentState&;

  void        ResetCropAndRotation() {
    if (geometry_panel_) {
      geometry_panel_->ResetCropAndRotation();
    }
  }

  void                                    changeEvent(QEvent* event) override;

  void                                    showEvent(QShowEvent* event) override;

  void                                    resizeEvent(QResizeEvent* event) override;

  void                                    ApplyInitialSplitterSizes();

  void                                    UpdateViewerZoomLabel(float zoom);

  void                                    RetranslateUi();

  void                                    RefreshPanelSwitchUi();

  void                                    SetActiveControlPanel(ControlPanelKind panel);

  void                                    SyncControlsFromState();

  bool                                    LoadStateFromPipelineIfPresent();

  void                                    SetupPipeline();

  void                                    ApplyStateToPipeline(const AdjustmentState& render_state);

  std::shared_ptr<ImagePoolService>       image_pool_;
  std::shared_ptr<PipelineGuard>          pipeline_guard_;
  std::shared_ptr<EditHistoryMgmtService> history_service_;
  std::shared_ptr<EditHistoryGuard>       history_guard_;
  sl_element_id_t                         element_id_ = 0;
  image_id_t                              image_id_   = 0;

  std::shared_ptr<PipelineScheduler>      scheduler_;
  PipelineTask                            base_task_{};

  QtEditViewer*                           viewer_                       = nullptr;
  QWidget*                                viewer_container_             = nullptr;
  QWidget*                                viewer_zoom_overlay_          = nullptr;
  QLabel*                                 viewer_zoom_value_label_      = nullptr;
  QLabel*                                 viewer_zoom_resolution_label_ = nullptr;
  ScopePanel*                             scope_panel_                  = nullptr;
  QScrollArea*                            controls_scroll_              = nullptr;
  QScrollArea*                            tone_controls_scroll_         = nullptr;
  QScrollArea*                            look_controls_scroll_         = nullptr;
  QScrollArea*                            drt_controls_scroll_          = nullptr;
  QScrollArea*                            geometry_controls_scroll_     = nullptr;
  QScrollArea*                            raw_controls_scroll_          = nullptr;
  QSplitter*                              main_splitter_                = nullptr;
  VersioningPanelWidget*                  versioning_panel_             = nullptr;
  QStackedWidget*                         control_panels_stack_         = nullptr;
  SpinnerWidget*                          spinner_                      = nullptr;
  QWidget*                                controls_                     = nullptr;
  QWidget*                                tone_controls_                = nullptr;
  QWidget*                                drt_controls_                 = nullptr;
  QWidget*                                geometry_controls_            = nullptr;
  QWidget*                                raw_controls_                 = nullptr;
  QVBoxLayout*                            controls_layout_              = nullptr;
  QVBoxLayout*                            drt_controls_layout_          = nullptr;
  QVBoxLayout*                            geometry_controls_layout_     = nullptr;
  QVBoxLayout*                            raw_controls_layout_          = nullptr;
  QPushButton*                            tone_panel_btn_               = nullptr;
  QPushButton*                            look_panel_btn_               = nullptr;
  QPushButton*                            drt_panel_btn_                = nullptr;
  QPushButton*                            geometry_panel_btn_           = nullptr;
  QPushButton*                            raw_panel_btn_                = nullptr;
  ToneControlPanelWidget*                 tone_panel_                   = nullptr;
  LookControlPanelWidget*                 look_panel_                   = nullptr;
  DisplayTransformPanelWidget*            drt_panel_                    = nullptr;
  RawDecodePanelWidget*                   raw_panel_                    = nullptr;
  GeometryPanelWidget*                    geometry_panel_               = nullptr;
  std::unique_ptr<ShortcutRegistry>       shortcut_registry_{};

  AdjustmentState                         state_{};
  AdjustmentState                         committed_state_{};
  std::unique_ptr<EditorHistoryCoordinator> history_coordinator_{};
  std::unique_ptr<EditorRenderCoordinator>  render_coordinator_{};
  std::unique_ptr<EditorAdjustmentSession>  adjustment_session_{};
  ControlPanelKind                          active_panel_         = ControlPanelKind::Tone;
  bool                                      pipeline_initialized_ = false;
  bool                                      syncing_controls_     = false;
  bool                                      initial_splitter_sizes_applied_ = false;
  EditorFrameManager                        frame_manager_{};

  ExifDisplayMetaData                       exif_display_;
};

}  // namespace alcedo::ui

#endif  // ALCEDO_EDITOR_DIALOG_INTERNAL
