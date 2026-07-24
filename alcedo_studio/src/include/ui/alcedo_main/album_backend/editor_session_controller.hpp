//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"

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
/// Phase 3-Fix / 5C holds the production presentation viewport and resolves its
/// `DirectFrameSink` so pipeline code can attach without QML calling storage or
/// pipeline infrastructure. The viewport pointer is typed as QObject in the
/// public API; resolution to `IFrameSink` lives in the implementation.
///
/// Phase 5A routes open/close/finalize through an injected `IEditorSessionBackend`
/// (production: EditorSessionService). The controller never owns pipeline guards,
/// the scheduler, or journal storage. UI shell state (filmstrip, panels) remains
/// controller-local.
///
/// Phase 6A implements `IEditorAdjustmentSubmitter` so the typed adjustment
/// models can submit one patch at a time without touching the pipeline scheduler.
/// `canEdit` exposes whether the session is Interactive with an image so panels
/// can gate control enablement.
class EditorSessionController final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY StateChanged)
  Q_PROPERTY(bool hasImage READ has_image NOTIFY StateChanged)
  // Phase 6A: true when an image is open and the session is Interactive, i.e.
  // adjustment controls are enabled and submitPatch will be accepted.
  Q_PROPERTY(bool canEdit READ can_edit NOTIFY StateChanged)
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
  Q_PROPERTY(bool filmstripCollapsed READ filmstrip_collapsed WRITE set_filmstrip_collapsed NOTIFY
                 FilmstripUiChanged)
  Q_PROPERTY(double filmstripExpandedHeight READ filmstrip_expanded_height WRITE
                 set_filmstrip_expanded_height NOTIFY FilmstripUiChanged)
  // Active right-side adjustment panel. One of: tone, look, display, geometry, raw.
  // Survives workspace Loader teardown and application restart (QSettings).
  Q_PROPERTY(QString activeAdjustmentPanel READ active_adjustment_panel WRITE
                 set_active_adjustment_panel NOTIFY DesktopUiChanged)
  // Left History/Versions rail page: empty string = collapsed; "history" or
  // "versions" = expanded. Survives workspace round-trips within the process
  // (not persisted across application restart).
  Q_PROPERTY(QString historyPanelPage READ history_panel_page WRITE set_history_panel_page NOTIFY
                 DesktopUiChanged)
  Q_PROPERTY(bool presentationViewportBound READ presentation_viewport_bound NOTIFY
                 PresentationBindingChanged)
  // Phase 5D: the render coordinator has in-flight or pending work for this
  // session. QML binds a busy indicator to it. Reflects backend render_busy()
  // (coordinator diagnostics); transitions fire StateChanged via the backend
  // notifier so this never exposes pipeline task objects (D6).
  Q_PROPERTY(bool renderBusy READ render_busy NOTIFY StateChanged)
  // Phase 5E: last session/backend error and first-frame latency. QML status
  // chrome and tests may observe these without touching pipeline task objects.
  Q_PROPERTY(QString lastError READ last_error NOTIFY StateChanged)
  Q_PROPERTY(double firstFrameTimeMs READ first_frame_time_ms NOTIFY StateChanged)
  // Aggregate coordinator diagnostics (reason, replace/cancel counts, last
  // rejection, last submitted role). Never includes pipeline task pointers.
  Q_PROPERTY(QVariantMap renderDiagnostics READ render_diagnostics NOTIFY StateChanged)

 public:
  explicit EditorSessionController(EditorController* editor = nullptr, QObject* parent = nullptr);
  EditorSessionController(EditorController* editor, alcedo::IEditorSessionBackend* session_backend,
                          QObject* parent = nullptr);
  ~EditorSessionController() override;

  void                     SetSessionBackend(alcedo::IEditorSessionBackend* session_backend);

  /// Called when the injected backend reports an async state/identity change
  /// (render presented, save finished, etc.). Mirrors backend into QML properties.
  void                     OnBackendChanged();

  [[nodiscard]] bool       active() const;
  [[nodiscard]] bool       has_image() const;
  [[nodiscard]] uint       element_id() const;
  [[nodiscard]] uint       image_id() const;
  [[nodiscard]] uint       last_element_id() const { return last_element_id_; }
  [[nodiscard]] uint       last_image_id() const { return last_image_id_; }
  [[nodiscard]] qulonglong session_generation() const;
  [[nodiscard]] auto       session_state() const -> alcedo::EditorSessionState;
  [[nodiscard]] QString    session_state_name() const;
  [[nodiscard]] bool       filmstrip_collapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double     filmstrip_expanded_height() const { return filmstrip_expanded_height_; }
  [[nodiscard]] QString    active_adjustment_panel() const { return active_adjustment_panel_; }
  [[nodiscard]] QString    history_panel_page() const { return history_panel_page_; }
  [[nodiscard]] bool       presentation_viewport_bound() const {
    return presentation_viewport_ != nullptr;
  }
  // Phase 5D: true when the coordinator has in-flight/pending render work.
  [[nodiscard]] bool       render_busy() const;
  // Phase 5E diagnostics surface for QML and integration tests.
  [[nodiscard]] QString    last_error() const;
  [[nodiscard]] double     first_frame_time_ms() const;
  [[nodiscard]] QVariantMap render_diagnostics() const;
  // Phase 6A: true when an image is open and the session is Interactive.
  [[nodiscard]] bool       can_edit() const;

  // Phase 6A: IEditorAdjustmentSubmitter. The typed adjustment models call
  // submitPatch to route one patch through the session service (interactive
  // preview when settled=false, one committed transaction when settled=true).
  // The same method is the QML-visible entry (Q_INVOKABLE) and the interface
  // override; both forward to the same backend call.
  Q_INVOKABLE bool          submitPatch(QString fieldKey, QString paramsJson, bool settled) override;
  [[nodiscard]] auto       canEdit() const -> bool override { return can_edit(); }

  Q_INVOKABLE void   Open(uint elementId = 0, uint imageId = 0);
  /// Check out a named Version by its hex version_id. Completes a save checkpoint
  /// first, then rebuilds the pipeline from root + first-parent commits.
  Q_INVOKABLE void   CheckoutVersion(const QString& versionId);
  Q_INVOKABLE void   Close();
  Q_INVOKABLE void   Shutdown();
  void               Finalize(bool persistChanges);
  // Forget the last-edited image so re-entering the editor does not resurrect a
  // deleted image or one from a prior project (Phase 4A-Fix).
  Q_INVOKABLE void   clearLastEditedImage();

  // Bind/unbind the production EditorViewportItem for this workspace instance.
  // Bind on mount and after every image Open while the same viewport lives.
  // Unbind only when the workspace tears the viewport down (not on A→B switch).
  Q_INVOKABLE void   bindPresentationViewport(QObject* viewportItem);
  Q_INVOKABLE void   unbindPresentationViewport();
  Q_INVOKABLE void   updatePresentationTargetSize(int width, int height);
  /// Bind the viewport interaction producer directly to the session route.
  /// This keeps DetailRefresh out of a QML Connections relay so a rebuilt
  /// workspace cannot lose the settled zoom notification.
  Q_INVOKABLE void   bindInteractionController(QObject* interactionController);
  // Phase 5D: route a user-driven view change (zoom/pan/resize/crop-rotation/
  // ROI) through the session backend as a typed ViewChange intent. `kind` is an
  // EditorInteractionController::ViewChangeKind (int for QML). The controller
  // only reports the new view and resolves the visible ROI region from the
  // production frame sink; the backend + coordinator decide reuse vs.
  // InteractivePrimary vs. DetailPatch (D2). No-op without a backend/image.
  Q_INVOKABLE void   submitViewChange(int kind);

  // Bound QQuickRhiItem (EditorViewportItem). QPointer may clear after destroy.
  [[nodiscard]] auto presentation_viewport() const -> QObject*;

  // Production pipeline entry: resolves the bound viewport to its DirectFrameSink.
  // Returns null when unbound or the object is not an EditorViewportItem.
  // Callers must re-resolve after PresentationBindingChanged / StateChanged.
  [[nodiscard]] auto presentation_frame_sink() const -> alcedo::IFrameSink*;

  void               set_filmstrip_collapsed(bool collapsed);
  void               set_filmstrip_expanded_height(double height);
  void               set_active_adjustment_panel(const QString& panel);
  void               set_history_panel_page(const QString& page);

 signals:
  void StateChanged();
  void FilmstripUiChanged();
  void DesktopUiChanged();
  void PresentationBindingChanged();
  void LastEditedImageChanged();

 private:
  void                           LoadFilmstripUiPrefs();
  void                           SaveFilmstripUiPrefs() const;
  void                           LoadDesktopUiPrefs();
  void                           SaveDesktopUiPrefs() const;
  void                           SyncIdentityFromBackend();
  void                           ApplyOpenLocal(uint elementId, uint imageId);
  void                           ApplyCloseLocal();
  void                           SyncViewportIdentity();
  void                           InstallBackendNotifier();
  [[nodiscard]] static auto      NormalizeAdjustmentPanel(const QString& panel) -> QString;
  [[nodiscard]] static auto      NormalizeHistoryPanelPage(const QString& page) -> QString;

  EditorController*              editor_                    = nullptr;
  alcedo::IEditorSessionBackend* session_backend_           = nullptr;
  // Local mirror when no backend is injected (legacy shell tests).
  bool                           active_                    = false;
  uint                           element_id_                = 0;
  uint                           image_id_                  = 0;
  uint                           last_element_id_           = 0;
  uint                           last_image_id_             = 0;
  qulonglong                     session_generation_        = 0;
  alcedo::EditorSessionState     session_state_             = alcedo::EditorSessionState::NoImage;
  bool                           filmstrip_collapsed_       = false;
  double                         filmstrip_expanded_height_ = 128.0;
  QString                        active_adjustment_panel_   = QStringLiteral("tone");
  QString                        history_panel_page_;
  QPointer<QObject>              presentation_viewport_;
  QPointer<QObject>              interaction_controller_;
  QMetaObject::Connection        interaction_view_change_connection_;
};

}  // namespace alcedo::ui
