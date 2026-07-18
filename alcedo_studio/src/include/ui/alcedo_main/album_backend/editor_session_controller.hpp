//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>

#include "app/editor_session_types.hpp"

namespace alcedo {
class IFrameSink;
class IEditorSessionBackend;
}  // namespace alcedo

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
///
/// Phase 5A routes open/close/finalize through an injected `IEditorSessionBackend`
/// (production: EditorSessionService). The controller never owns pipeline guards,
/// the scheduler, or journal storage. UI shell state (filmstrip, panels) remains
/// controller-local.
class EditorSessionController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY StateChanged)
  Q_PROPERTY(bool hasImage READ has_image NOTIFY StateChanged)
  Q_PROPERTY(uint elementId READ element_id NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ image_id NOTIFY StateChanged)
  // Last image opened with a non-zero id. Survives Close/Finalize so re-entering
  // the editor from the library can restore the same image (Phase 4A-Fix). Cleared
  // by clearLastEditedImage() when the image is deleted or the project switches.
  Q_PROPERTY(uint lastElementId READ last_element_id NOTIFY LastEditedImageChanged)
  Q_PROPERTY(uint lastImageId READ last_image_id NOTIFY LastEditedImageChanged)
  // Monotonic counter advanced on every Open, including A→B→A reopens. Distinct
  // from imageId so the viewport can reject stale frames from a prior session.
  Q_PROPERTY(qulonglong sessionGeneration READ session_generation NOTIFY StateChanged)
  Q_PROPERTY(QString sessionState READ session_state_name NOTIFY StateChanged)
  Q_PROPERTY(bool filmstripCollapsed READ filmstrip_collapsed WRITE set_filmstrip_collapsed
                 NOTIFY FilmstripUiChanged)
  Q_PROPERTY(double filmstripExpandedHeight READ filmstrip_expanded_height WRITE
                 set_filmstrip_expanded_height NOTIFY FilmstripUiChanged)
  // Active right-side adjustment panel. One of: tone, look, display, geometry, raw.
  // Survives workspace Loader teardown and application restart (QSettings).
  Q_PROPERTY(QString activeAdjustmentPanel READ active_adjustment_panel WRITE
                 set_active_adjustment_panel NOTIFY DesktopUiChanged)
  // Left History/Versions rail page: empty string = collapsed; "history" or
  // "versions" = expanded. Survives workspace round-trips within the process
  // (not persisted across application restart).
  Q_PROPERTY(QString historyPanelPage READ history_panel_page WRITE set_history_panel_page
                 NOTIFY DesktopUiChanged)
  Q_PROPERTY(bool presentationViewportBound READ presentation_viewport_bound NOTIFY
                 PresentationBindingChanged)

 public:
  explicit EditorSessionController(EditorController* editor = nullptr, QObject* parent = nullptr);
  EditorSessionController(EditorController* editor, alcedo::IEditorSessionBackend* session_backend,
                          QObject* parent = nullptr);

  void SetSessionBackend(alcedo::IEditorSessionBackend* session_backend);

  [[nodiscard]] bool active() const;
  [[nodiscard]] bool has_image() const;
  [[nodiscard]] uint element_id() const;
  [[nodiscard]] uint image_id() const;
  [[nodiscard]] uint last_element_id() const { return last_element_id_; }
  [[nodiscard]] uint last_image_id() const { return last_image_id_; }
  [[nodiscard]] qulonglong session_generation() const;
  [[nodiscard]] auto session_state() const -> alcedo::EditorSessionState;
  [[nodiscard]] QString session_state_name() const;
  [[nodiscard]] bool filmstrip_collapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double filmstrip_expanded_height() const { return filmstrip_expanded_height_; }
  [[nodiscard]] QString active_adjustment_panel() const { return active_adjustment_panel_; }
  [[nodiscard]] QString history_panel_page() const { return history_panel_page_; }
  [[nodiscard]] bool presentation_viewport_bound() const {
    return presentation_viewport_ != nullptr;
  }

  Q_INVOKABLE void Open(uint elementId = 0, uint imageId = 0);
  Q_INVOKABLE void Close();
  void Finalize(bool persistChanges);
  // Forget the last-edited image so re-entering the editor does not resurrect a
  // deleted image or one from a prior project (Phase 4A-Fix).
  Q_INVOKABLE void clearLastEditedImage();

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
  void set_active_adjustment_panel(const QString& panel);
  void set_history_panel_page(const QString& page);

 signals:
  void StateChanged();
  void FilmstripUiChanged();
  void DesktopUiChanged();
  void PresentationBindingChanged();
  void LastEditedImageChanged();

 private:
  void LoadFilmstripUiPrefs();
  void SaveFilmstripUiPrefs() const;
  void LoadDesktopUiPrefs();
  void SaveDesktopUiPrefs() const;
  void SyncIdentityFromBackend();
  void ApplyOpenLocal(uint elementId, uint imageId);
  void ApplyCloseLocal();
  void SyncViewportIdentity();
  [[nodiscard]] static auto NormalizeAdjustmentPanel(const QString& panel) -> QString;
  [[nodiscard]] static auto NormalizeHistoryPanelPage(const QString& page) -> QString;

  EditorController*              editor_ = nullptr;
  alcedo::IEditorSessionBackend* session_backend_ = nullptr;
  // Local mirror when no backend is injected (legacy shell tests).
  bool                           active_ = false;
  uint                           element_id_ = 0;
  uint                           image_id_ = 0;
  uint                           last_element_id_ = 0;
  uint                           last_image_id_ = 0;
  qulonglong                     session_generation_ = 0;
  alcedo::EditorSessionState     session_state_ = alcedo::EditorSessionState::NoImage;
  bool                           filmstrip_collapsed_ = false;
  double                         filmstrip_expanded_height_ = 128.0;
  QString                        active_adjustment_panel_ = QStringLiteral("tone");
  QString                        history_panel_page_;
  QPointer<QObject>              presentation_viewport_;
};

}  // namespace alcedo::ui
