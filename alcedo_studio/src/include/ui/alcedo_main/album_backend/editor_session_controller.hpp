//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>
#include <memory>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_history_types.hpp"
#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_action_availability_model.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_history_operation_publisher.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

namespace alcedo {
class IFrameSink;
class IEditorSessionBackend;
}  // namespace alcedo

namespace alcedo::ui {
class InteractionPolicyController;
}  // namespace alcedo::ui

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
  Q_PROPERTY(bool hasPendingRecovery READ has_pending_recovery NOTIFY StateChanged)
  // Phase 6A: true when an image is open and the session is Interactive, i.e.
  // adjustment controls are enabled and submitPatch will be accepted.
  Q_PROPERTY(bool canEdit READ can_edit NOTIFY StateChanged)
  // True only while the current image has an unmaterialized working head.
  Q_PROPERTY(bool canDiscardCurrentCommit READ can_discard_current_commit NOTIFY StateChanged)
  Q_PROPERTY(uint elementId READ element_id NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ image_id NOTIFY StateChanged)
  // Last image opened with a non-zero id. Survives Close/Finalize so re-entering
  // the editor from the library can restore the same image (Phase 4A-Fix). Cleared
  // by clearLastEditedImage() when the image is deleted or the project switches.
  Q_PROPERTY(uint lastElementId READ last_element_id NOTIFY LastEditedImageChanged)
  Q_PROPERTY(uint lastImageId READ last_image_id NOTIFY LastEditedImageChanged)
  // Composite key for QML viewport session resets (includes load-request generation).
  Q_PROPERTY(QString viewportIdentityKey READ viewport_identity_key NOTIFY StateChanged)
  Q_PROPERTY(QString sessionState READ session_state_name NOTIFY StateChanged)
  Q_PROPERTY(EditorActionAvailabilityModel* actions READ actions CONSTANT)
  Q_PROPERTY(bool filmstripCollapsed READ filmstrip_collapsed WRITE set_filmstrip_collapsed NOTIFY
                 FilmstripUiChanged)
  Q_PROPERTY(double filmstripExpandedHeight READ filmstrip_expanded_height WRITE
                 set_filmstrip_expanded_height NOTIFY FilmstripUiChanged)
  Q_PROPERTY(double filmstripScrollPosition READ filmstrip_scroll_position WRITE
                 set_filmstrip_scroll_position NOTIFY FilmstripUiChanged)
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
  Q_PROPERTY(EditorScopeController* scopeController READ scope_controller CONSTANT)
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
  /// Phase 6C-7: read-only field-value snapshot for panel loading. Keys are
  /// stable field identifiers (e.g. "exposure", "contrast"); values are the
  /// parsed JSON params. Published after open, checkout, undo, redo, recovery,
  /// Paste, and Merge. Panels load from this snapshot via loadFromSnapshot().
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustment_snapshot NOTIFY AdjustmentSnapshotChanged)
  /// Read-only RAW panel capability map resolved by the application layer.
  /// The panel consumes this map; it does not inspect image metadata or build
  /// flags directly.
  Q_PROPERTY(QVariantMap rawDecodeCapabilities READ raw_decode_capabilities NOTIFY
                 RawDecodeCapabilitiesChanged)
  /// Phase 7A P1: typed result of the last history/Version operation, published
  /// at the QML boundary so the owning panel can show pending/success/error
  /// state instead of discarding the backend EditorSessionResult.
  Q_PROPERTY(QString lastHistoryMessage READ last_history_message NOTIFY HistoryOperationFinished)
  Q_PROPERTY(bool lastHistoryFailed READ last_history_failed NOTIFY HistoryOperationFinished)
  Q_PROPERTY(QVariantMap lastHistoryResult READ last_history_result NOTIFY HistoryOperationFinished)

 public:
  explicit EditorSessionController(EditorController* editor = nullptr, QObject* parent = nullptr);
  EditorSessionController(EditorController* editor, alcedo::IEditorSessionBackend* session_backend,
                          QObject* parent = nullptr);
  ~EditorSessionController() override;

  void                     SetSessionBackend(alcedo::IEditorSessionBackend* session_backend);
  void                     SetInteractionPolicy(InteractionPolicyController* interaction_policy);
  void                     SetCopiedPackageAvailable(bool available);

  /// Called when the injected backend reports an async state/identity change
  /// (render presented, save finished, etc.). Mirrors backend into QML properties.
  void                     OnBackendChanged();

  [[nodiscard]] bool       active() const;
  [[nodiscard]] bool       has_image() const;
  [[nodiscard]] bool       has_pending_recovery() const;
  [[nodiscard]] uint       element_id() const;
  [[nodiscard]] uint       image_id() const;
  [[nodiscard]] uint       last_element_id() const { return last_element_id_; }
  [[nodiscard]] uint       last_image_id() const { return last_image_id_; }
  [[nodiscard]] QString    viewport_identity_key() const;
  [[nodiscard]] auto       session_state() const -> alcedo::EditorSessionState;
  [[nodiscard]] QString    session_state_name() const;
  [[nodiscard]] bool       filmstrip_collapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double     filmstrip_expanded_height() const { return filmstrip_expanded_height_; }
  [[nodiscard]] double     filmstrip_scroll_position() const { return filmstrip_scroll_position_; }
  [[nodiscard]] QString    active_adjustment_panel() const { return active_adjustment_panel_; }
  [[nodiscard]] QString    history_panel_page() const { return history_panel_page_; }
  // Phase 6C-7: load panel state from the backend adjustment snapshot.
  [[nodiscard]] auto       adjustment_snapshot() const -> QVariantMap;
  [[nodiscard]] auto       history_snapshot() const -> alcedo::EditorHistorySnapshot;
  [[nodiscard]] auto       actions() -> EditorActionAvailabilityModel* { return &actions_; }
  [[nodiscard]] auto       raw_decode_capabilities() const -> QVariantMap {
    return raw_decode_capabilities_;
  }

  [[nodiscard]] bool presentation_viewport_bound() const {
    return presentation_viewport_ != nullptr;
  }
  // Phase 5D: true when the coordinator has in-flight/pending render work.
  [[nodiscard]] bool        render_busy() const;
  // Phase 5E diagnostics surface for QML and integration tests.
  [[nodiscard]] QString     last_error() const;
  [[nodiscard]] double      first_frame_time_ms() const;
  [[nodiscard]] QVariantMap render_diagnostics() const;
  // Phase 7A R4: typed result of the last history/Version operation, owned by
  // EditorHistoryOperationPublisher (not ad-hoc controller fields).
  [[nodiscard]] QString     last_history_message() const {
    return history_ops_.last_published().message;
  }
  [[nodiscard]] bool last_history_failed() const { return history_ops_.last_published().failed; }
  [[nodiscard]] QVariantMap last_history_result() const {
    return history_ops_.last_published().map;
  }
  // Phase 6A: true when an image is open and the session is Interactive.
  [[nodiscard]] bool        can_edit() const;
  [[nodiscard]] bool        can_discard_current_commit() const;

  // Phase 6A: IEditorAdjustmentSubmitter. The typed adjustment models call
  // submitPatch to route one patch through the session service (interactive
  // preview when settled=false, one committed transaction when settled=true).
  // The same method is the QML-visible entry (Q_INVOKABLE) and the interface
  // override; both forward to the same backend call.
  Q_INVOKABLE bool   submitPatch(QString fieldKey, QString paramsJson, bool settled) override;
  [[nodiscard]] auto canEdit() const -> bool override { return can_edit(); }

  Q_INVOKABLE void   Open(uint elementId = 0, uint imageId = 0);
  /// Check out a named Version by its hex version_id. Completes a save checkpoint
  /// first, then rebuilds the pipeline from root + first-parent commits.
  Q_INVOKABLE void   CheckoutVersion(const QString& versionId);
  Q_INVOKABLE void   CreateRootVersion(const QString& displayName);
  Q_INVOKABLE void   BranchFromCommit(const QString& commitId, const QString& displayName);
  Q_INVOKABLE void   RetrySave();
  Q_INVOKABLE void   DiscardAndContinue();
  Q_INVOKABLE void   Discard();
  Q_INVOKABLE void   CancelPendingNavigation();
  Q_INVOKABLE void   RenameVersion(const QString& versionId, const QString& displayName);
  Q_INVOKABLE void   RemoveVersion(const QString& versionId);
  Q_INVOKABLE void   Undo();
  Q_INVOKABLE void   Redo();
  Q_INVOKABLE void   MoveHeadToCommit(const QString& commitId);
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

  auto               PasteAdjustmentPackage(const alcedo::AdjustmentTransferPackage& package,
                                            const QString& versionDisplayName) -> alcedo::EditorSessionResult;
  auto               BeginMergeAdjustmentPackage(const alcedo::AdjustmentTransferPackage& package,
                                                 alcedo::AdjustmentMergePreview*          preview)
      -> alcedo::EditorSessionResult;
  auto CompleteMergeAdjustments(const std::vector<alcedo::AdjustmentMergeResolution>& resolutions)
      -> alcedo::EditorSessionResult;
  auto               CancelMergeAdjustments() -> alcedo::EditorSessionResult;

  // Bound QQuickRhiItem (EditorViewportItem). QPointer may clear after destroy.
  [[nodiscard]] auto presentation_viewport() const -> QObject*;

  [[nodiscard]] auto scope_controller() const -> EditorScopeController* {
    return scope_controller_.get();
  }

  // Production pipeline entry: resolves the bound viewport through the scope tap.
  // Returns null when unbound or the object is not an EditorViewportItem.
  // Callers must re-resolve after PresentationBindingChanged / StateChanged.
  [[nodiscard]] auto presentation_frame_sink() const -> alcedo::IFrameSink*;

  void               set_filmstrip_collapsed(bool collapsed);
  void               set_filmstrip_expanded_height(double height);
  void               set_filmstrip_scroll_position(double position);
  void               set_active_adjustment_panel(const QString& panel);
  void               set_history_panel_page(const QString& page);

 signals:
  void StateChanged();
  void HistoryChanged();
  // Phase 6C-7: emitted when the backend adjustment snapshot is published.
  void AdjustmentSnapshotChanged();
  void RawDecodeCapabilitiesChanged();
  void ActionAvailabilityChanged();

  void FilmstripUiChanged();
  void DesktopUiChanged();
  void PresentationBindingChanged();
  void LastEditedImageChanged();
  // Phase 7A P1: emitted with the typed result of a history/Version operation.
  void HistoryOperationFinished();

 private:
  void                           LoadFilmstripUiPrefs();
  void                           SaveFilmstripUiPrefs() const;
  void                           LoadDesktopUiPrefs();
  void                           SaveDesktopUiPrefs() const;
  void                           SyncIdentityFromBackend();
  void                           SyncRawDecodeCapabilities();
  void                           ApplyOpenLocal(uint elementId, uint imageId);
  void                           ApplyCloseLocal();
  void                           SyncViewportIdentity();
  void                           InstallBackendNotifier();
  void                           ApplyActionAvailability();
  void                           SyncBackgroundActionRestrictions();
  [[nodiscard]] qulonglong       ImageLoadGeneration() const;
  /// Apply a publisher event to QML properties and emit HistoryOperationFinished.
  void                           ApplyPublishedHistory(
      const EditorHistoryOperationPublisher::Published& published);
  /// Reject at the controller boundary without calling the backend.
  void PublishHistoryRejected(const QString& action, const QString& message,
                              const QString& selected_id = {});
  /// Publish the invokable return value and track async pending when needed.
  void PublishHistoryInvokableReturn(const QString& action, const alcedo::EditorSessionResult& result,
                                     const QString& selected_id = {});
  /// Correlate an async backend result observer delivery to a pending action.
  void OnBackendSessionResult(const alcedo::EditorSessionResult& result);
  [[nodiscard]] static auto      NormalizeAdjustmentPanel(const QString& panel) -> QString;
  [[nodiscard]] static auto      NormalizeHistoryPanelPage(const QString& page) -> QString;

  EditorController*              editor_                    = nullptr;
  alcedo::IEditorSessionBackend* session_backend_           = nullptr;
  InteractionPolicyController*   interaction_policy_        = nullptr;
  EditorActionAvailabilityModel  actions_;
  /// Focused correlator for history/Version operation events (R4). Owns
  /// operation ids, pending-async state, and the last published map.
  EditorHistoryOperationPublisher history_ops_;
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
  double                         filmstrip_scroll_position_ = 0.0;
  // Phase 6C-7: cached adjustment snapshot for QML panel loading.
  mutable QVariantMap            adjustment_snapshot_;
  QVariantMap                    raw_decode_capabilities_;
  /// When true, OnBackendChanged still refreshes the cached snapshot map but
  /// does not emit AdjustmentSnapshotChanged. Used for interactive submitPatch
  /// so pointer moves do not re-enter QML loadFromSnapshot on every tick.
  bool                           suppress_snapshot_publish_ = false;
  // Phase 7A R2: last history revision observed from the backend. OnBackendChanged
  // emits HistoryChanged only when the backend's history_revision advances, so
  // render/preview/task notifications no longer trigger a history projection.
  std::uint64_t last_history_revision_ = 0;
  /// Convert EditorRenderAdjustmentSnapshot patches into a QVariantMap keyed
  /// by field_key with parsed JSON values suitable for QML model loading.
  [[nodiscard]] static auto BuildSnapshotMap(const alcedo::EditorRenderAdjustmentSnapshot& snapshot)
      -> QVariantMap;

  QString                 active_adjustment_panel_ = QStringLiteral("tone");
  QString                 history_panel_page_;
  QPointer<QObject>       presentation_viewport_;
  QPointer<QObject>       interaction_controller_;
  QMetaObject::Connection interaction_view_change_connection_;
  QMetaObject::Connection interaction_policy_connection_;
  mutable std::unique_ptr<EditorScopeController> scope_controller_;
};

}  // namespace alcedo::ui
