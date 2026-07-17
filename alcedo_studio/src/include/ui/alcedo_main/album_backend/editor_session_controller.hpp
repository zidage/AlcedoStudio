//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QtGlobal>

namespace alcedo {
class IFrameSink;
}

namespace alcedo::ui {

class EditorController;

/// QML-facing editor session facade used by workspace routing.
///
/// Phase 1B owns route/session identity and filmstrip shell preferences for the
/// unified QML editor workspace. It does not open the legacy modal dialog; that
/// path remains on EditorController until the cutover phase removes it.
///
/// Phase 3-Fix holds the production presentation viewport and resolves its
/// `LeaseFrameSink` so pipeline code can attach without QML calling storage or
/// pipeline infrastructure. The viewport pointer is typed as QObject in the
/// public API; resolution to `IFrameSink` lives in the implementation.
class EditorSessionController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY StateChanged)
  Q_PROPERTY(bool hasImage READ has_image NOTIFY StateChanged)
  Q_PROPERTY(uint elementId READ element_id NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ image_id NOTIFY StateChanged)
  // Monotonic counter advanced on every Open, including A→B→A reopens. Distinct
  // from imageId so the viewport can reject stale frames from a prior session.
  Q_PROPERTY(qulonglong sessionGeneration READ session_generation NOTIFY StateChanged)
  Q_PROPERTY(bool filmstripCollapsed READ filmstrip_collapsed WRITE set_filmstrip_collapsed
                 NOTIFY FilmstripUiChanged)
  Q_PROPERTY(double filmstripExpandedHeight READ filmstrip_expanded_height WRITE
                 set_filmstrip_expanded_height NOTIFY FilmstripUiChanged)
  Q_PROPERTY(bool presentationViewportBound READ presentation_viewport_bound NOTIFY
                 PresentationBindingChanged)

 public:
  explicit EditorSessionController(EditorController* editor, QObject* parent = nullptr);

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] bool has_image() const { return active_ && element_id_ > 0 && image_id_ > 0; }
  [[nodiscard]] uint element_id() const { return element_id_; }
  [[nodiscard]] uint image_id() const { return image_id_; }
  [[nodiscard]] qulonglong session_generation() const { return session_generation_; }
  [[nodiscard]] bool filmstrip_collapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double filmstrip_expanded_height() const { return filmstrip_expanded_height_; }
  [[nodiscard]] bool presentation_viewport_bound() const {
    return presentation_viewport_ != nullptr;
  }

  Q_INVOKABLE void Open(uint elementId = 0, uint imageId = 0);
  Q_INVOKABLE void Close();
  void Finalize(bool persistChanges);

  // Bind/unbind the production EditorViewportItem for this workspace instance.
  // Bind on mount and after every image Open while the same viewport lives.
  // Unbind only when the workspace tears the viewport down (not on A→B switch).
  Q_INVOKABLE void bindPresentationViewport(QObject* viewportItem);
  Q_INVOKABLE void unbindPresentationViewport();

  // Bound QQuickRhiItem (EditorViewportItem). QPointer may clear after destroy.
  [[nodiscard]] auto presentation_viewport() const -> QObject*;

  // Production pipeline entry: resolves the bound viewport to its LeaseFrameSink.
  // Returns null when unbound or the object is not an EditorViewportItem.
  // Callers must re-resolve after PresentationBindingChanged / StateChanged.
  [[nodiscard]] auto presentation_frame_sink() const -> alcedo::IFrameSink*;

  void set_filmstrip_collapsed(bool collapsed);
  void set_filmstrip_expanded_height(double height);

 signals:
  void StateChanged();
  void FilmstripUiChanged();
  void PresentationBindingChanged();

 private:
  void LoadFilmstripUiPrefs();
  void SaveFilmstripUiPrefs() const;

  EditorController* editor_ = nullptr;
  bool              active_ = false;
  uint              element_id_ = 0;
  uint              image_id_ = 0;
  qulonglong        session_generation_ = 0;
  bool              filmstrip_collapsed_ = false;
  double            filmstrip_expanded_height_ = 128.0;
  QPointer<QObject> presentation_viewport_;
};

}  // namespace alcedo::ui
