//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

#include <QSettings>
#include <QtGlobal>

#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_frame_sink.hpp"

namespace alcedo::ui {
namespace {

constexpr auto kFilmstripCollapsedKey = "editor/filmstripCollapsed";
constexpr auto kFilmstripExpandedHeightKey = "editor/filmstripExpandedHeight";
constexpr auto kActiveAdjustmentPanelKey = "editor/activeAdjustmentPanel";
constexpr double kFilmstripExpandedHeightMin = 72.0;
constexpr double kFilmstripExpandedHeightMax = 320.0;
constexpr double kFilmstripExpandedHeightDefault = 128.0;

}  // namespace

EditorSessionController::EditorSessionController(EditorController* editor, QObject* parent)
    : QObject(parent), editor_(editor) {
  LoadFilmstripUiPrefs();
  LoadDesktopUiPrefs();
}

void EditorSessionController::Open(uint elementId, uint imageId) {
  active_ = true;
  element_id_ = elementId;
  image_id_ = imageId;
  // Remember the last real image so re-entering the editor from the library can
  // restore it (Phase 4A-Fix). Close/Finalize never touch this; only an explicit
  // clearLastEditedImage() (delete / project switch) forgets it.
  if (elementId > 0 && imageId > 0 &&
      (last_element_id_ != elementId || last_image_id_ != imageId)) {
    last_element_id_ = elementId;
    last_image_id_ = imageId;
    emit LastEditedImageChanged();
  }
  // Always advance so A→B→A cannot reuse a generation accepted by the viewport.
  ++session_generation_;
  // Keep the legacy controller identity fields aligned for modules that still
  // read them, but do not open the modal QWidget editor from the QML workspace.
  Q_UNUSED(editor_);
  // Align the still-bound production viewport with this session so pipeline
  // producers that re-resolve presentation_frame_sink() reject prior frames.
  if (auto* item =
          qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
    item->setImageIdentity(static_cast<qulonglong>(imageId));
    item->setImageGeneration(session_generation_);
  }
  emit StateChanged();
}

void EditorSessionController::Close() {
  if (!active_ && element_id_ == 0 && image_id_ == 0) {
    return;
  }
  active_ = false;
  element_id_ = 0;
  image_id_ = 0;
  // Leave session_generation_ intact so a later Open still advances past it.
  // Keep the presentation binding: the viewport outlives image switches and
  // brief no-image states inside the same EditorWorkspace instance.
  emit StateChanged();
}

void EditorSessionController::Finalize(bool persistChanges) {
  Q_UNUSED(persistChanges);
  // Seal only session identity. Do not unbind the presentation viewport here:
  // OpenEditor A→B calls Finalize then Open while the same QML viewport lives.
  // Unbind happens on viewport Component.onDestruction when the workspace unloads.
  Close();
}

void EditorSessionController::clearLastEditedImage() {
  if (last_element_id_ == 0 && last_image_id_ == 0) {
    return;
  }
  last_element_id_ = 0;
  last_image_id_ = 0;
  emit LastEditedImageChanged();
}

void EditorSessionController::bindPresentationViewport(QObject* viewportItem) {
  if (presentation_viewport_ == viewportItem) {
    return;
  }
  presentation_viewport_ = viewportItem;
  if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(viewportItem)) {
    if (has_image()) {
      item->setImageIdentity(static_cast<qulonglong>(image_id_));
      item->setImageGeneration(session_generation_);
    }
  }
  emit PresentationBindingChanged();
}

void EditorSessionController::unbindPresentationViewport() {
  if (!presentation_viewport_) {
    return;
  }
  presentation_viewport_.clear();
  emit PresentationBindingChanged();
}

auto EditorSessionController::presentation_viewport() const -> QObject* {
  return presentation_viewport_.data();
}

auto EditorSessionController::presentation_frame_sink() const -> alcedo::IFrameSink* {
  // Production attach path: resolve the bound QML viewport to its lease sink.
  // Pipeline code must call this (not construct a parallel sink).
  auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (!item) {
    return nullptr;
  }
  return item->frameSink();
}

void EditorSessionController::set_filmstrip_collapsed(bool collapsed) {
  if (filmstrip_collapsed_ == collapsed) {
    return;
  }
  filmstrip_collapsed_ = collapsed;
  SaveFilmstripUiPrefs();
  emit FilmstripUiChanged();
}

void EditorSessionController::set_filmstrip_expanded_height(double height) {
  const double clamped =
      qBound(kFilmstripExpandedHeightMin, height, kFilmstripExpandedHeightMax);
  if (qFuzzyCompare(filmstrip_expanded_height_, clamped)) {
    return;
  }
  filmstrip_expanded_height_ = clamped;
  SaveFilmstripUiPrefs();
  emit FilmstripUiChanged();
}

void EditorSessionController::LoadFilmstripUiPrefs() {
  QSettings settings;
  filmstrip_collapsed_ = settings.value(QLatin1String(kFilmstripCollapsedKey), false).toBool();
  filmstrip_expanded_height_ =
      qBound(kFilmstripExpandedHeightMin,
              settings
                  .value(QLatin1String(kFilmstripExpandedHeightKey),
                         kFilmstripExpandedHeightDefault)
                  .toDouble(),
              kFilmstripExpandedHeightMax);
}

void EditorSessionController::SaveFilmstripUiPrefs() const {
  QSettings settings;
  settings.setValue(QLatin1String(kFilmstripCollapsedKey), filmstrip_collapsed_);
  settings.setValue(QLatin1String(kFilmstripExpandedHeightKey), filmstrip_expanded_height_);
  settings.sync();
}

auto EditorSessionController::NormalizeAdjustmentPanel(const QString& panel) -> QString {
  const QString key = panel.trimmed().toLower();
  if (key == QLatin1String("look") || key == QLatin1String("color")) {
    return QStringLiteral("look");
  }
  if (key == QLatin1String("display") || key == QLatin1String("drt") ||
      key == QLatin1String("displayrenderingtransform")) {
    return QStringLiteral("display");
  }
  if (key == QLatin1String("geometry") || key == QLatin1String("crop")) {
    return QStringLiteral("geometry");
  }
  if (key == QLatin1String("raw") || key == QLatin1String("rawdecode")) {
    return QStringLiteral("raw");
  }
  return QStringLiteral("tone");
}

auto EditorSessionController::NormalizeHistoryPanelPage(const QString& page) -> QString {
  const QString key = page.trimmed().toLower();
  if (key == QLatin1String("history")) {
    return QStringLiteral("history");
  }
  if (key == QLatin1String("versions")) {
    return QStringLiteral("versions");
  }
  return {};
}

void EditorSessionController::set_active_adjustment_panel(const QString& panel) {
  const QString normalized = NormalizeAdjustmentPanel(panel);
  if (active_adjustment_panel_ == normalized) {
    return;
  }
  active_adjustment_panel_ = normalized;
  SaveDesktopUiPrefs();
  emit DesktopUiChanged();
}

void EditorSessionController::set_history_panel_page(const QString& page) {
  const QString normalized = NormalizeHistoryPanelPage(page);
  if (history_panel_page_ == normalized) {
    return;
  }
  history_panel_page_ = normalized;
  emit DesktopUiChanged();
}

void EditorSessionController::LoadDesktopUiPrefs() {
  QSettings settings;
  active_adjustment_panel_ = NormalizeAdjustmentPanel(
      settings.value(QLatin1String(kActiveAdjustmentPanelKey), QStringLiteral("tone"))
          .toString());
  // historyPanelPage is intentionally not restored from disk: collapsed on
  // cold start, but kept in memory across library/editor workspace switches.
}

void EditorSessionController::SaveDesktopUiPrefs() const {
  QSettings settings;
  settings.setValue(QLatin1String(kActiveAdjustmentPanelKey), active_adjustment_panel_);
  settings.sync();
}

}  // namespace alcedo::ui
